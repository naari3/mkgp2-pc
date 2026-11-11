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
   slots move down by one. The deleting destructor of an abstract class,
   which clang emits as a trap, gets its real body: the sources model some
   instantiated classes with a pure virtual slot, and MWCC's one destructor
   does not tell abstract classes apart.

Input must be produced with
    -S -emit-llvm -g -fstandalone-debug -fno-discard-value-names
    -flto -fwhole-program-vtables
(the type tests tell the class of each virtual call, the debug metadata
tells which slot of that class is the destructor). The output is plain IR
for `clang -c`.

For 32-bit Windows targets the `this`-in-register convention is taken off
member functions (see the comment in rewrite()).

3. Two polymorphic bases. MWCC writes one table: the first base's part,
   the second base's part (with its own two header words), then the virtual
   functions added by the class and below it. Itanium keeps the added
   functions in the first group. A table emitted here is reordered, and
   calls of the added functions skip the second base's part (step 1b).

4. `dynamic_cast` and type descriptions (step 1c and the cast rewrite at
   the top of rewrite()).

Not handled: where the vtable pointer sits inside an object, more than two
polymorphic bases, pointers to virtual member functions of classes with a
virtual destructor.
"""
import re
import sys


def _class_dtor_slots(text):
    """type test name (_ZTS...) -> Itanium slot of the complete destructor."""
    comp = {}    # id -> name
    scope = {}   # id -> scope id
    ns = {}      # id -> namespace name
    dtor = {}    # class id -> (unusable) index of its virtual destructor
    used = {}    # class id -> slots of its other virtual functions
    bases = {}   # class id -> [base id]
    alias = {}   # typedef id -> base type id
    base_off = {}  # (class id, base id) -> byte offset of the base
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
            if name and sc and v:
                if name.group(1).startswith("~"):
                    dtor[sc.group(1)] = int(v.group(1))
                else:
                    used.setdefault(sc.group(1), set()).add(int(v.group(1)))
        else:
            bt = re.search(r"\bbaseType: (!\d+)", body)
            if "DW_TAG_inheritance" in body and sc and bt:
                bases.setdefault(sc.group(1), []).append(bt.group(1))
                off = re.search(r"\boffset: (\d+)", body)
                base_off[(sc.group(1), bt.group(1))] = int(off.group(1)) // 8 if off else 0
            elif "DW_TAG_typedef" in body and bt:
                alias[ident] = bt.group(1)

    # The index the debug description gives a virtual destructor is 0
    # wherever it is declared, so it cannot be used. The other virtual
    # functions carry their real slots, and the destructor's pair is the gap
    # they leave: the first slot no other virtual function of the class or of
    # its bases has.
    def slot(c):
        seen, has_dtor = set(), False
        for _ in range(32):
            while c in alias:
                c = alias[c]
            has_dtor = has_dtor or c in dtor
            seen |= used.get(c, set())
            b = bases.get(c, [])[:1]   # the primary base chain
            if not b:
                break
            c = b[0]
        if not has_dtor:
            return None
        k = 0
        while k in seen:
            k += 1
        return k

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

    # A class with two polymorphic bases. MWCC writes one table: the first
    # base's part, then the second base's part with its own two header
    # words, then the virtual functions the class and its descendants add
    # (Itanium keeps those in the first group and puts the second base's
    # group last). For each class below such a class: (Itanium slots of the
    # first base's part, words of the second base's part in MWCC's table).
    def chain(c):
        res = []
        for _ in range(32):
            while c in alias:
                c = alias[c]
            res.append(c)
            b = bases.get(c, [])[:1]
            if not b:
                break
            c = b[0]
        return res

    def itanium_slots(c):
        """(number of Itanium slots, has a virtual destructor) of c's primary chain"""
        seen, has_dtor = set(), False
        for x in chain(c):
            has_dtor = has_dtor or x in dtor
            seen |= used.get(x, set())
        if has_dtor:
            k = 0
            while k in seen:
                k += 1
            seen |= {k, k + 1}
        return (max(seen) + 1 if seen else 0), has_dtor

    def split(c):
        for x in chain(c):
            poly = []
            for b in bases.get(x, []):
                while b in alias:
                    b = alias[b]
                if itanium_slots(b)[0]:
                    poly.append(b)
            if len(poly) > 2 or (len(poly) == 2 and poly[0] != chain(x)[1:2][0]):
                return "unsupported"
            if len(poly) == 2:
                if any(len([b for b in bases.get(y, []) if itanium_slots(b)[0]]) > 1 for y in chain(poly[0]) + chain(poly[1])):
                    return "unsupported"
                n2, d2 = itanium_slots(poly[1])
                return itanium_slots(poly[0])[0], 2 + n2 - (1 if d2 else 0)
        return None

    # Every base class of c with its offset, for the type descriptions of
    # step 1c: [(name, offset)], or None when a name is not a plain one.
    def all_bases(c, at=0, depth=0):
        res = []
        for b in bases.get(c, []):
            o = at + base_off.get((c, b), 0)
            while b in alias:
                b = alias[b]
            n = comp.get(b)
            if n is None or not re.fullmatch(r"\w+", n) or scope.get(b) in comp or scope.get(b) in ns or depth > 16:
                return None
            sub = all_bases(b, o, depth + 1)
            if sub is None:
                return None
            res += [(n, o)] + sub
        return res

    out = {}
    clash = set()
    splits = {}
    types = {}
    for c in comp:
        key = mangled(c)
        if key is None:
            continue
        s = slot(c)
        if key in out and out[key] != s:
            clash.add(key)
        out[key] = s
        sp = split(c)
        if sp is not None:
            splits[key] = sp
        if re.fullmatch(r"_ZTS\d+\w+", key):
            ab = all_bases(c)
            if ab is not None and (key not in types or len(ab) > len(types[key][1])):
                types[key] = (comp[c], ab)
    return out, clash, splits, types


def rewrite(text, warn=None):
    warn = warn or (lambda s: None)
    slots, clash, splits, types = _class_dtor_slots(text)

    # 32-bit Windows (GNU ABI) passes `this` in a register for member
    # functions. The vtables written as data hold C functions and C code calls
    # member functions by their MWCC names, so the game needs one convention
    # for both: member functions become plain C-convention functions.
    text, n_cc = re.subn(r"\bx86_thiscallcc ", "", text)

    # deleting destructors of abstract classes: destroy and delete
    def untrap(m):
        name = m.group(1)
        d1 = name[:-4] + "D1Ev"
        if not re.search(r"^(define|declare) [^@\n]*@%s\(|^@%s = " % (re.escape(d1), re.escape(d1)), text, re.M):
            d1 = name[:-4] + "D2Ev"
        return (m.group(0)[:m.start(2) - m.start(0)]
                + "call void @%s(ptr noundef %%this1)\n  call void @_ZdlPv(ptr noundef %%this1)\n  ret void" % d1)
    text, n_trap = re.subn(r"^define [^@\n]*@(\w+D0Ev)\([^\n]*\{\n(?:(?!^\}).*\n)*?  (call void @llvm\.trap\(\)[^\n]*\n  unreachable[^\n]*)",
                           untrap, text, flags=re.M)
    if n_trap and not re.search(r"^declare [^@\n]*@_ZdlPv\(", text, re.M):
        text += "\ndeclare void @_ZdlPv(ptr noundef)\n"

    # `dynamic_cast<T *>` (units compiled with RTTI on). The host runtime
    # would look for its type_info below the address point, which step 2
    # moves, and half of the game's objects have MWCC's type_info instead.
    # The call goes to port/platform/runtime/mwcc_runtime.c with T's name;
    # that answers from MWCC's type_info (tables written as data) or from
    # the description step 1c puts into the tables emitted here.
    cast_names = {}

    def cast(m):
        obj, dst, hint = m.groups()
        plain = re.fullmatch(r"\d+(\w+)", dst)
        if not plain:
            warn("dynamic_cast to %s: not checked" % dst)
            return "@mkgp2pc_dynamic_cast(%s, ptr null, ptr null, %s)" % (obj, hint)
        g = cast_names.setdefault(plain.group(1), "@__mwcast.%d" % len(cast_names))
        return "@mkgp2pc_dynamic_cast(%s, ptr null, ptr %s, %s)" % (obj, g, hint)

    text = re.sub(r"@__dynamic_cast\((ptr [^,]+), ptr @_ZTI[\w.$]+, ptr @_ZTI([\w.$]+), (i32 -?\d+)\)", cast, text)
    text = re.sub(r"@__dynamic_cast\b", "@mkgp2pc_dynamic_cast", text)

    # drop the module summary (stale after the edits)
    text = re.sub(r"^\^\d+ = .*\n", "", text, flags=re.M)

    # 1. vtables defined here: one destructor slot
    wrappers = {}

    # 1c. Type description. The second header word of a table emitted here
    # (the host's type_info pointer, null without RTTI) points at a
    # description in MWCC's form, { name, { base, offset }... }; the first
    # word stays the offset to the complete object, so MWCC's own
    # __dynamic_cast (fn_80271340) goes on treating these classes as before
    # and only mkgp2pc_dynamic_cast reads it.
    type_defs = {}

    def type_info(table):
        t = types.get("_ZTS" + table[5:])
        if t is None:
            return "ptr null"
        if table not in type_defs:
            names = [t[0]] + [n for n, _ in t[1]]
            d = []
            for n in dict.fromkeys(names):
                d.append('@"__mwtn.%s.%s" = private constant [%d x i8] c"%s\\00"' % (table[5:], n, len(n) + 1, n))
                d.append('@"__mwtq.%s.%s" = private constant { ptr, ptr } { ptr @"__mwtn.%s.%s", ptr null }' % (table[5:], n, table[5:], n))
            ents = ['{ ptr, i32 } { ptr @"__mwtq.%s.%s", i32 %d }' % (table[5:], n, o) for n, o in t[1]] + ["{ ptr, i32 } zeroinitializer"]
            d.append('@"__mwtb.%s" = private constant [%d x { ptr, i32 }] [%s]' % (table[5:], len(ents), ", ".join(ents)))
            d.append('@"__mwti.%s" = private constant { ptr, ptr } { ptr @"__mwtn.%s.%s", ptr @"__mwtb.%s" }' % (table[5:], table[5:], t[0], table[5:]))
            type_defs[table] = "\n".join(d)
        return 'ptr @"__mwti.%s"' % table[5:]

    def fix_vtable(m):
        head, count, body, tail = m.groups()
        items = [s.strip() for s in body.split(",")]
        mt = re.match(r"@_ZTV[\w.$]+", head)
        if len(items) > 1 and re.fullmatch(r"ptr (null|@_ZTI[\w.$]+)", items[1]):
            items[1] = type_info(mt.group(0)) if mt else "ptr null"
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

    # 1b. a vtable defined here for a class with two polymorphic bases
    # (clResultType00): one destructor slot per group, and MWCC's order of
    # the parts (see `split` above). The second group's destructors are the
    # this-adjusting thunks. Classes whose table is written out as data and
    # that get a table here only through a key function are rewritten the
    # same way; nothing uses those.
    local_groups = {}   # @_ZTV name -> byte offset of each group's start

    def fix_vtable_groups(m):
        head, body, tail = m.groups()
        table = re.match(r"@_ZTV[\w.$]+", head).group(0)
        groups = []
        for g in re.finditer(r"\[\d+ x ptr\] \[(.*?)\]", body):
            one = fix_vtable(re.match(r"(.*)\n()(.*)()$", table + "\n" + g.group(1), re.S))
            one = one[len(table):]
            groups.append(re.fullmatch(r"\{ \[\d+ x ptr\] \} \{ \[\d+ x ptr\] \[(.*)\] \}", one).group(1).split(", "))
        sp = splits.get("_ZTS" + table[5:])
        if len(groups) != 2 or sp is None or sp == "unsupported":
            warn("vtable %s: groups kept in the host's order" % table)
            first, rest = groups[0], []
        else:
            k = slots.get("_ZTS" + table[5:])
            cut = 2 + sp[0] - (1 if k is not None and k < sp[0] else 0)
            first, rest = groups[0][:cut], groups[0][cut:]
            if len(groups[1]) != sp[1]:
                warn("vtable %s: second group has %d words, expected %d" % (table, len(groups[1]), sp[1]))
        offs, flat = [0], list(first)
        for g in groups[1:]:
            offs.append(4 * len(flat))
            flat += g
        flat += rest
        local_groups[table] = offs
        return "%s{ [%d x ptr] } { [%d x ptr] [%s] }%s" % (head, len(flat), len(flat), ", ".join(flat), tail)

    text = re.sub(r"^(@_ZTV\S+ = .*?)\{ \[\d+ x ptr\](?:, \[\d+ x ptr\])+ \} \{ (.*) \}(, align .*)$",
                  fix_vtable_groups, text, flags=re.M)

    # 2. address point = start of the table
    text, n_addr = re.subn(r"getelementptr inbounds \(\{ \[\d+ x ptr\] \}, ptr (@_ZTV[\w.$]+), i32 0, inrange i32 0, i32 2\)", r"\1", text)
    # 2b. a class with two bases whose vtable is data written out elsewhere
    # (MWCC layout): the table has one group per base, each with its own two
    # header words. Group 0 is the table start; group 1 starts after group
    # 0, which is one slot shorter than Itanium's when the class has a
    # virtual destructor (one destructor slot instead of two).
    external = set(re.findall(r"^(@_ZTV[\w.$]+) = external ", text, re.M))

    def group_address(m):
        sizes, table, group = [int(n) for n in re.findall(r"\[(\d+) x ptr\]", m.group(1))], m.group(2), int(m.group(3))
        if table in local_groups:   # step 1b
            off = local_groups[table][group]
            return table if off == 0 else "getelementptr inbounds (i8, ptr %s, i32 %d)" % (table, off)
        if table not in external or group > 1:
            return m.group(0)
        if group == 0:
            return table
        dtor = 1 if slots.get("_ZTS" + table[5:]) is not None else 0
        return "getelementptr inbounds (i8, ptr %s, i32 %d)" % (table, 4 * (sizes[0] - dtor))

    text, n_group = re.subn(r"getelementptr inbounds \((\{ \[\d+ x ptr\](?:, \[\d+ x ptr\])+ \}), ptr (@_ZTV[\w.$]+), i32 0, inrange i32 (\d+), i32 2\)",
                            group_address, text)
    n_addr += n_group
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
                # past the second base's part (step 1b)
                sp = splits.get(cls)
                if sp == "unsupported":
                    warn("class %s: more than two polymorphic bases, slot %d not moved" % (cls, n))
                elif sp is not None and int(m.group(4)) >= sp[0]:
                    n += sp[1]
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

    if type_defs or cast_names:
        defs = list(type_defs.values()) + ['%s = private constant [%d x i8] c"%s\\00"' % (g, len(n) + 1, n) for n, g in cast_names.items()]
        cut = re.search(r"^(define |declare )", text, re.M)
        pos = cut.start() if cut else len(text)
        text = text[:pos] + "\n".join(defs) + "\n\n" + text[pos:]
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
