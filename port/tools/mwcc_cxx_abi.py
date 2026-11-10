#!/usr/bin/env python3
"""Rewrite clang's LLVM IR for a C++ unit to MWCC's vtable conventions.

The game mixes objects whose vtables are written out as data (in MWCC's
layout) with classes the host compiler lays out itself, so both have to
agree. See docs/port/census.md section 13. What this pass changes:

1. Address point. Itanium objects point at the first function of the
   vtable (two words in); MWCC objects point at the start of the table and
   functions start at +8. Vtable pointer stores use the table start, and
   every virtual call adds two slots.
2. Destructor slots. Itanium has two slots per virtual destructor
   (complete, deleting); MWCC has one function with a flag (>0: destroy and
   delete, 0: destroy as a base, <0: destroy). Emitted vtables get one slot
   with a small wrapper, calls through the slot pass the flag, and later
   slots move down by one.

Input must be produced with
    -S -emit-llvm -g -fstandalone-debug -fno-discard-value-names
    -flto -fwhole-program-vtables
(the type tests tell the class of each virtual call, the debug metadata
tells which slot of that class is the destructor). The output is plain IR
for `clang -c`.

For 32-bit Windows targets the `this`-in-register convention is taken off
member functions (see the comment in rewrite()).

Not handled: where the vtable pointer sits inside an object, multiple
inheritance (secondary vtables), pointers to virtual member functions of
classes with a virtual destructor.
"""
import re
import sys


def _class_dtor_slots(text):
    """type test name (_ZTS...) -> Itanium slot of the complete destructor."""
    comp = {}    # id -> name
    scope = {}   # id -> scope id
    ns = {}      # id -> namespace name
    dtor = {}    # class id -> index
    bases = {}   # class id -> [base id]
    alias = {}   # typedef id -> base type id
    for m in re.finditer(r"^(!\d+) = (?:distinct )?!(DICompositeType|DISubprogram|DIDerivedType|DINamespace)\((.*)\)$", text, re.M):
        ident, kind, body = m.groups()
        name = re.search(r'\bname: "([^"]*)"', body)
        sc = re.search(r"\bscope: (!\d+)", body)
        if kind == "DICompositeType":
            if name:
                comp[ident] = name.group(1)
            if sc:
                scope[ident] = sc.group(1)
        elif kind == "DINamespace":
            ns[ident] = name.group(1) if name else ""
            if sc:
                scope[ident] = sc.group(1)
        elif kind == "DISubprogram":
            v = re.search(r"\bvirtualIndex: (\d+)", body)
            if name and sc and v and name.group(1).startswith("~"):
                dtor[sc.group(1)] = int(v.group(1))
        else:
            bt = re.search(r"\bbaseType: (!\d+)", body)
            if "DW_TAG_inheritance" in body and sc and bt:
                bases.setdefault(sc.group(1), []).append(bt.group(1))
            elif "DW_TAG_typedef" in body and bt:
                alias[ident] = bt.group(1)

    def slot(c, depth=0):
        while c in alias:
            c = alias[c]
        if c in dtor:
            return dtor[c]
        if depth < 32:
            for b in bases.get(c, [])[:1]:   # the primary base chain
                s = slot(b, depth + 1)
                if s is not None:
                    return s
        return None

    def mangled(c):
        parts = []
        cur = c
        while cur is not None:
            n = comp.get(cur) if cur in comp else ns.get(cur)
            if n is None:
                break
            if not re.fullmatch(r"\w+", n):
                return None   # templates and the like: not needed so far
            parts.append("%d%s" % (len(n), n))
            cur = scope.get(cur)
        parts.reverse()
        if not parts:
            return None
        return "_ZTS" + (parts[0] if len(parts) == 1 else "N" + "".join(parts) + "E")

    out = {}
    clash = set()
    for c in comp:
        key = mangled(c)
        if key is None:
            continue
        s = slot(c)
        if key in out and out[key] != s:
            clash.add(key)
        out[key] = s
    return out, clash


def rewrite(text, warn=None):
    warn = warn or (lambda s: None)
    slots, clash = _class_dtor_slots(text)

    # 32-bit Windows (GNU ABI) passes `this` in a register for member
    # functions. The vtables written as data hold C functions and C code calls
    # member functions by their MWCC names, so the game needs one convention
    # for both: member functions become plain C-convention functions.
    text, n_cc = re.subn(r"\bx86_thiscallcc ", "", text)

    # drop the module summary (stale after the edits)
    text = re.sub(r"^\^\d+ = .*\n", "", text, flags=re.M)

    # 1. vtables defined here: one destructor slot
    wrappers = {}

    def fix_vtable(m):
        head, count, body, tail = m.groups()
        items = [s.strip() for s in body.split(",")]
        new = []
        i = 0
        while i < len(items):
            a = items[i]
            b = items[i + 1] if i + 1 < len(items) else ""
            # (the complete-object destructor, or the base-object one where the
            # compiler found the two identical and kept only that)
            ma = re.fullmatch(r"ptr @(\w*D[12]Ev)", a)
            mb = re.fullmatch(r"ptr @(\w*D0Ev)", b)
            if ma and mb and ma.group(1)[:-4] == mb.group(1)[:-4]:
                w = "__mwdt." + mb.group(1)
                wrappers[w] = (ma.group(1), mb.group(1))
                new.append("ptr @%s" % w)
                i += 2
            else:
                new.append(a)
                i += 1
        return "%s{ [%d x ptr] } { [%d x ptr] [%s] }%s" % (head, len(new), len(new), ", ".join(new), tail)

    text = re.sub(r"^(@_ZTV\S+ = .*?)\{ \[(\d+) x ptr\] \} \{ \[\d+ x ptr\] \[(.*?)\] \}(.*)$", fix_vtable, text, flags=re.M)

    # 2. address point = start of the table
    text, n_addr = re.subn(r"getelementptr inbounds \(\{ \[\d+ x ptr\] \}, ptr (@_ZTV[\w.$]+), i32 0, inrange i32 0, i32 2\)", r"\1", text)
    if re.search(r"getelementptr inbounds \(\{[^)]*\}, ptr @_ZTV", text):
        warn("vtable reference left as it was (secondary vtable?)")

    # 3. virtual calls
    lines = text.split("\n")
    # A class that is only ever deleted through a pointer here has no debug
    # description in this unit. The `delete` itself tells where its
    # destructor is: it calls the deleting one, the second of the pair.
    label, seen = "", {}
    for line in lines:
        m = re.match(r"([\w.]+):", line)
        if m:
            label = m.group(1)
            continue
        if line.startswith("define "):
            label = ""
            seen.clear()
            continue
        m = re.match(r"\s*%[\w.]+ = call i1 @llvm\.(?:public\.)?type\.test\(ptr (%[\w.]+), metadata !\"([^\"]+)\"\)", line)
        if m:
            seen[m.group(1)] = m.group(2)
            continue
        m = re.match(r"\s*%vfn[\w.]* = getelementptr inbounds ptr, ptr (%vtable[\w.]*), i64 (\d+)", line)
        if m and label.startswith("delete.notnull"):
            cls = seen.get(m.group(1))
            if cls and cls not in slots and int(m.group(2)) > 0:
                slots[cls] = int(m.group(2)) - 1
    out = []
    vt_class = {}     # %vtable name -> _ZTS name (per function)
    dtor_fn = {}      # value loaded from a destructor slot -> flag
    vfn_info = {}     # %vfn name -> flag or None
    assume_drop = set()
    n_calls = 0
    for line in lines:
        if line.startswith("define "):
            vt_class.clear()
            dtor_fn.clear()
            vfn_info.clear()
            assume_drop.clear()
        m = re.match(r"\s*(%[\w.]+) = call i1 @llvm\.(?:public\.)?type\.test\(ptr (%[\w.]+), metadata !\"([^\"]+)\"\)", line)
        if m:
            vt_class[m.group(2)] = m.group(3)
            assume_drop.add(m.group(1))
            continue
        m = re.match(r"\s*call void @llvm\.assume\(i1 (%[\w.]+)\)", line)
        if m and m.group(1) in assume_drop:
            continue
        m = re.match(r"(\s*)(%vfn[\w.]*) = getelementptr inbounds ptr, ptr (%vtable[\w.]*), i64 (\d+)(.*)$", line)
        if m:
            ind, vfn, vt, n, rest = m.groups()
            n = int(n)
            cls = vt_class.get(vt)
            flag = None
            if cls is None:
                warn("virtual call without a class (slot %d)" % n)
            else:
                if cls in clash:
                    warn("class %s has two definitions with different destructor slots" % cls)
                k = slots.get(cls)
                if cls not in slots:
                    warn("no debug info for %s" % cls)
                if k is not None:
                    if n == k:
                        flag = -1
                    elif n == k + 1:
                        flag = 1
                        n = k
                    elif n > k + 1:
                        n -= 1
            vfn_info[vfn] = flag
            out.append("%s%s = getelementptr inbounds ptr, ptr %s, i64 %d%s" % (ind, vfn, vt, n + 2, rest))
            n_calls += 1
            continue
        m = re.match(r"\s*(%[\w.]+) = load ptr, ptr (%vfn[\w.]*),", line)
        if m and vfn_info.get(m.group(2)) is not None:
            dtor_fn[m.group(1)] = vfn_info[m.group(2)]
        m = re.match(r"(\s*)(call|invoke) void (%[\w.]+)\((ptr [^,]*)\)((?:, !\w+ !\d+)*)$", line)
        if m and m.group(3) in dtor_fn:
            ind, op, fn, arg, rest = m.groups()
            out.append("%s%%mw.dt.%d = %s ptr %s(%s, i32 noundef %d)%s" % (ind, len(out), op, fn, arg, dtor_fn[fn], rest))
            continue
        # pointer to member function, virtual case: offsets count from +8
        m = re.match(r"(\s*%[\w.]+) = sub i32 (%memptr\.ptr[\w.]*), 1((?:, !\w+ !\d+)*)$", line)
        if m:
            out.append("%s = add i32 %s, 7%s" % m.groups())
            continue
        out.append(line)
    text = "\n".join(out)

    if wrappers:
        extra = []
        for w, (d1, d0) in sorted(wrappers.items()):
            extra.append("""
define linkonce_odr ptr @%s(ptr %%this, i32 %%flag) {
entry:
  %%del = icmp sgt i32 %%flag, 0
  br i1 %%del, label %%delete, label %%destroy
delete:
  call void @%s(ptr %%this)
  ret ptr %%this
destroy:
  call void @%s(ptr %%this)
  ret ptr %%this
}
""" % (w, d0, d1))
        # before the metadata block
        cut = re.search(r"^(attributes #|!llvm\.|!\d+ = )", text, re.M)
        pos = cut.start() if cut else len(text)
        text = text[:pos] + "".join(extra) + "\n" + text[pos:]
    return text, {"address_points": n_addr, "virtual_calls": n_calls, "dtor_slots": len(wrappers), "thiscall_removed": n_cc}


if __name__ == "__main__":
    src = open(sys.argv[1]).read()
    res, stats = rewrite(src, lambda s: sys.stderr.write("warning: %s\n" % s))
    open(sys.argv[2], "w").write(res)
    sys.stderr.write("%s\n" % stats)
