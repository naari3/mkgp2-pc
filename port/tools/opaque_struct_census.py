#!/usr/bin/env python3
"""List initialised data whose type is a struct with nothing but a byte
array in it (`struct X { char data[12]; }`, as tools/struct_merge.py leaves a
type nobody named the members of).  Such an object holds the console's
big-endian bytes whatever the host, so code that reads it through another
view of the type (`int`, `float`, pointers) gets byte-swapped values on a
little-endian host: docs/port/census.md section 35.  All-zero objects are not
listed.  tools/port data_width_census.py covers scalar types only.
Usage: python3 port/tools/opaque_struct_census.py
"""
import glob, re

def main():
    opaque = {}
    files = glob.glob("src/**/*.c", recursive=True) + glob.glob("src/**/*.cpp", recursive=True) \
        + glob.glob("src/**/*.inc", recursive=True) + glob.glob("include/**/*.h", recursive=True)
    texts = {f: open(f, errors="replace").read() for f in files}
    member = r"(?:/\*[^*]*\*/\s*)?(?:unsigned\s+|signed\s+)?(?:char|u8|s8)\s+\w+(?:\[(\w+)\])?;\s*"
    pat = re.compile(r"struct\s+(\w+)\s*\{\s*((?:" + member + r")+)\}")
    for f, text in texts.items():
        for m in pat.finditer(text):
            size = sum(int(d or "1", 0) for d in re.findall(member, m.group(2)))
            if size > 1:
                opaque[m.group(1)] = (f, str(size))
    n = 0
    for f in sorted(texts):
        for m in re.finditer(r"^(?:const\s+)?(?:struct\s+)?(\w+)\s+(\w+)\s*((?:\[[^\]]*\])*)\s*=\s*\{(.*?)\n\};", texts[f], re.M | re.S):
            t, name, dims, body = m.groups()
            if t not in opaque or not re.search(r"[1-9A-Fa-f]", re.sub(r"0x", "", body)):
                continue
            n += 1
            print("%s  %s%s in %s  (struct of %s bytes, %s)" % (name, t, dims, f, opaque[t][1], opaque[t][0]))
    print("%d objects" % n)

if __name__ == "__main__":
    main()
