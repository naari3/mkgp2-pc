#!/usr/bin/env python3
"""Functions declared with their float and integer parameters in another
order than they are defined with.

On the console floats travel in the FP registers and everything else in the
general ones, so `f(obj, pos, rot, 1.0f, 2.0f, 3.0f)` reaches
`f(obj, float, float, float, pos, rot)` intact. On a host with one argument
list it does not. Reads the LLVM IR the link census leaves in
build/pc-census (`python3 port/tools/link_census.py ... --link` first) and
compares every `declare` with the `define` of the same name.

The second list is calls that pass fewer arguments than the definition
takes. The console leaves an argument register alone until somebody writes
it, so `Derived_Init(self)` can call `Base_Init(self)` and the base still
finds the caller's other four arguments; a host does not. Definitions under
C++ names are followed through the rename the link census applied
(`*.cxxsyms`). The third list is the reverse, calls with more arguments
than the definition takes: harmless unless the function forwards them. A `()` declaration in C tells nothing and is skipped.

The fourth list is calls whose result is a float where the definition
returns something else, or the reverse. A float comes back on the x87
register stack on 32-bit x86: a caller that takes the function for `void`
never pops it, and after eight such calls every float load in the thread
is NaN; a caller that expects a float from a function that returns none
pops an empty stack, NaN as well.

    python3 port/tools/arg_order_census.py            # report
"""
import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "pc-census")


def split_params(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def kinds(params):
    ks = []
    for p in params:
        if p == "...":
            ks.append("...")
        elif re.match(r"(float|double)\b", p):
            ks.append("F")
        else:
            ks.append("I")
    return "".join(ks)


def main():
    defs = {}
    decls = collections.defaultdict(dict)
    renamed = collections.defaultdict(dict)   # unit -> {mangled: C name}
    for f in glob.glob(os.path.join(OUT, "*.cxxsyms")):
        unit = os.path.basename(f)[:-len(".o.cxxsyms")].replace("__", "/")
        for line in open(f):
            a, b = line.split()
            renamed[unit][a] = re.sub(r"^mkgp2pc_(ct2?|dt[012])__", "", b)
    cdefs = {}
    calls = collections.defaultdict(dict)     # name -> {unit: fewest arguments}
    call_re = re.compile(r"\bcall [^@\n]*?@([\w.$]+)\(")
    FP = ("float", "double", "x86_fp80")
    ret_re = re.compile(r"\b(?:call|define)\b((?: [\w()]+)*?) (void|float|double|x86_fp80|i\d+|ptr|%[\w.\":]+|\{[^}]*\}|<[^>]*>|\[[^\]]*\])(?: \([^)]*\))? @")
    rdefs = {}
    rcalls = collections.defaultdict(dict)    # name -> {unit: returns a float}
    for ll in sorted(glob.glob(os.path.join(OUT, "*.ll"))):
        unit = os.path.basename(ll)[:-3].replace("__", "/")
        for line in open(ll, encoding="utf-8", errors="replace"):
            if not line.startswith(("define ", "declare ")):
                m = call_re.search(line) if " call " in line else None
                if m and not m.group(1).startswith("llvm."):
                    if ")(" in line[:m.start(1)]:
                        continue  # called through an explicit function type: `()` declaration
                    n = len(split_params(line[m.end():line.rindex(")")]))
                    name = renamed[unit].get(m.group(1), m.group(1))
                    r = ret_re.search(line)
                    if r:
                        rcalls[name].setdefault(unit, r.group(2) in FP)
                    calls[name][unit] = min(n, calls[name].get(unit, 99))
                continue
            if line.startswith("define "):
                m = re.match(r"define .*?@([\w.$]+)\((.*)\)[^)]*$", line.rstrip())
                if m:
                    name = renamed[unit].get(m.group(1), m.group(1))
                    r = ret_re.search(line)
                    if r:
                        rdefs.setdefault(name, (r.group(2) in FP, unit))
                    if not name.startswith("_Z"):
                        cdefs.setdefault(name, (len(split_params(m.group(2))), unit, "..." in m.group(2)))
            m = re.match(r"(define|declare) .*?@([\w.$]+)\((.*)\)[^)]*$", line.rstrip())
            if not m:
                continue
            # a definition under a C++ name counts under the C name the link
            # census gave it (`Tween::Set(float, float, short)` is
            # `fn_8020A734`, which 30 units declare `(void *, int, float, float)`)
            oname = renamed[unit].get(m.group(2), m.group(2)) if m.group(1) == "define" else m.group(2)
            if oname.startswith(("llvm.", "_Z", "__mw")):
                continue
            k = kinds(split_params(m.group(3)))
            if m.group(1) == "define":
                defs.setdefault(oname, (k, unit))
            else:
                decls[oname][unit] = k
    n = 0
    for name in sorted(decls):
        if name not in defs:
            continue
        dk, dunit = defs[name]
        bad = {u: k for u, k in decls[name].items()
               if k != dk and "..." not in k and "F" in (k + dk) and len(k) == len(dk)}
        if bad:
            n += 1
            print(f"{name}: defined {dk} in {dunit}")
            for u, k in sorted(bad.items()):
                print(f"    declared {k} in {u}")
    print(f"{n} functions declared with another parameter order")
    n = 0
    for name in sorted(calls):
        if name not in cdefs or cdefs[name][2]:
            continue
        want, dunit, _ = cdefs[name]
        bad = {u: k for u, k in calls[name].items() if k < want}
        if bad:
            n += 1
            print(f"{name}: takes {want} in {dunit}")
            for u, k in sorted(bad.items()):
                print(f"    called with {k} in {u}")
    print(f"{n} functions called with fewer arguments than they take")
    # The other direction is harmless by itself, but not when the function
    # hands its caller's extra arguments on in the registers
    # (clItemBoxResponder_CmdGetWorldPos(self) calls self->GetWorldPos(),
    # which stores through r4..r6): every entry needs a look at its body.
    n = 0
    for name in sorted(calls):
        if name not in cdefs or cdefs[name][2]:
            continue
        want, dunit, _ = cdefs[name]
        bad = {u: k for u, k in calls[name].items() if k > want and k != 99}
        if bad:
            n += 1
            print(f"{name}: takes {want} in {dunit}")
            for u, k in sorted(bad.items()):
                print(f"    called with {k} in {u}")
    print(f"{n} functions called with more arguments than they take")
    n = 0
    for name in sorted(rcalls):
        if name not in rdefs:
            continue
        fp, dunit = rdefs[name]
        bad = sorted(u for u, k in rcalls[name].items() if k != fp)
        if bad:
            n += 1
            print(f"{name}: {'returns a float' if fp else 'returns no float'} in {dunit}")
            for u in bad:
                print(f"    called {'for a float' if not fp else 'without taking a float'} in {u}")
    print(f"{n} functions called with another kind of result (float or not) than they return")
    return 0


if __name__ == "__main__":
    sys.exit(main())
