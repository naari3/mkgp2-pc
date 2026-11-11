#!/usr/bin/env python3
"""List data whose definition and an `extern` declaration elsewhere disagree
on the element width (for example defined `int x[2]`, declared
`unsigned char x[8]`).  On the console both views see the same big-endian
bytes; on a little-endian host the bytes of each element are reversed, so
the narrower view reads the wrong values.  Scalar element types only;
struct types are skipped.  Usage: python3 port/tools/data_width_census.py
"""
import collections, glob, re, sys

W = {"char": 1, "u8": 1, "s8": 1, "bool": 1, "BOOL8": 1, "short": 2, "u16": 2, "s16": 2,
     "wchar_t": 2, "int": 4, "long": 4, "u32": 4, "s32": 4, "float": 4, "f32": 4,
     "double": 8, "f64": 8, "u64": 8, "s64": 8}
TYPE = r"((?:(?:unsigned|signed)\s+)?(?:char|short|int|long|float|double|u8|s8|u16|s16|u32|s32|f32|f64|wchar_t))"
QUAL = r"(?:(?:const|volatile|static)\s+)*"

def width(t):
    return W[t.split()[-1]]

def main():
    defs = {}
    decls = collections.defaultdict(set)
    files = glob.glob("src/**/*.c", recursive=True) + glob.glob("src/**/*.cpp", recursive=True) \
        + glob.glob("include/**/*.h", recursive=True)
    for f in files:
        text = open(f, errors="replace").read()
        for m in re.finditer(r"^" + QUAL + TYPE + r"\s+(\w+)\s*((?:\[[^\]]*\])*)\s*=", text, re.M):
            if "static" in m.group(0):
                continue
            defs[m.group(2)] = (f, m.group(1), m.group(3))
        for m in re.finditer(r"extern\s+(?:\"C\"\s+)?" + QUAL + TYPE + r"\s+(\w+)\s*((?:\[[^\]]*\])*)\s*;", text):
            decls[m.group(2)].add((f, m.group(1), m.group(3)))
    n = 0
    for name in sorted(defs):
        f, t, dims = defs[name]
        bad = sorted(d for d in decls.get(name, ()) if width(d[1]) != width(t))
        if bad:
            n += 1
            print("%s  defined %s%s in %s" % (name, t, dims, f))
            for bf, bt, bd in bad:
                print("    declared %s%s in %s" % (bt, bd, bf))
    print("%d symbols" % n, file=sys.stderr)

if __name__ == "__main__":
    main()
