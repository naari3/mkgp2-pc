#!/usr/bin/env python3
"""Functions declared with their float and integer parameters in another
order than they are defined with.

On the console floats travel in the FP registers and everything else in the
general ones, so `f(obj, pos, rot, 1.0f, 2.0f, 3.0f)` reaches
`f(obj, float, float, float, pos, rot)` intact. On a host with one argument
list it does not. Reads the LLVM IR the link census leaves in
build/pc-census (`python3 port/tools/link_census.py ... --link` first) and
compares every `declare` with the `define` of the same name.

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
    for ll in sorted(glob.glob(os.path.join(OUT, "*.ll"))):
        unit = os.path.basename(ll)[:-3].replace("__", "/")
        for line in open(ll, encoding="utf-8", errors="replace"):
            if not line.startswith(("define ", "declare ")):
                continue
            m = re.match(r"(define|declare) .*?@([\w.$]+)\((.*)\)[^)]*$", line.rstrip())
            if not m or m.group(2).startswith(("llvm.", "_Z", "__mw")):
                continue
            k = kinds(split_params(m.group(3)))
            if m.group(1) == "define":
                defs.setdefault(m.group(2), (k, unit))
            else:
                decls[m.group(2)][unit] = k
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
    print(f"{n} functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
