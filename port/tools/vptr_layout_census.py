#!/usr/bin/env python3
"""List the classes whose vtable pointer MWCC does not put first.

MWCC puts the vtable pointer where the first `virtual` is declared among
the data members, and after a base that has no virtual functions; the host
compiler always puts it at +0 (docs/port/cxx_classes.md). C++ code that uses
such a class through its definition agrees with itself on the host. A
function that reads the same objects through a private struct written for
MWCC's offsets does not: every member in front of MWCC's vtable pointer is
one pointer further in on the host (docs/port/census.md section 33).

    python3 port/tools/vptr_layout_census.py build/pc-census/mkgp2-nogfx

reads the executable's debug information (llvm-dwarfdump) and prints, per
class and defining file: size, the members declared before the first
virtual function, and any base without virtual functions. For each class
listed, look for C functions that take its objects: its vtable's entries,
list helpers called from its users.
"""
import collections
import re
import subprocess
import sys

HDR = re.compile(r"^0x[0-9a-f]+:(\s+)(DW_TAG_\w+|NULL)")
ATTR = re.compile(r"^\s+(DW_AT_\w+)\s+\((.*)\)\s*$")


def scan(lines, root):
    res = collections.OrderedDict()

    def close(n):
        if n["tag"] not in ("DW_TAG_class_type", "DW_TAG_structure_type") or "DW_AT_declaration" in n["a"]:
            return
        kids = n["k"]
        if not any(k["tag"] == "DW_TAG_member" and k["a"].get("DW_AT_name", "").startswith('"_vptr$') for k in kids):
            return
        virt = [int(k["a"]["DW_AT_decl_line"]) for k in kids
                if k["tag"] == "DW_TAG_subprogram" and "DW_AT_virtuality" in k["a"] and "DW_AT_decl_line" in k["a"]]
        first = min(virt) if virt else 10 ** 9
        before = [k["a"]["DW_AT_name"].strip('"') for k in kids
                  if k["tag"] == "DW_TAG_member" and "DW_AT_artificial" not in k["a"]
                  and "DW_AT_external" not in k["a"] and int(k["a"].get("DW_AT_decl_line", "0")) < first]
        bases = [re.sub(r'^0x[0-9a-f]+ ', "", k["a"].get("DW_AT_type", "")).strip('"')
                 for k in kids if k["tag"] == "DW_TAG_inheritance"]
        if before or bases:
            name = n["a"].get("DW_AT_name", "?").strip('"')
            path = n["a"].get("DW_AT_decl_file", "?").strip('"').replace(root, "")
            res.setdefault((name, path), (n["a"].get("DW_AT_byte_size", "?"), before, bases))

    stack = []
    cur = None
    for line in lines:
        m = HDR.match(line)
        if m:
            ind = len(m.group(1))
            while stack and stack[-1][0] >= ind:
                close(stack.pop()[1])
            if m.group(2) == "NULL":
                cur = None
                continue
            cur = {"tag": m.group(2), "a": {}, "k": []}
            if stack:
                stack[-1][1]["k"].append(cur)
            stack.append((ind, cur))
            continue
        m = ATTR.match(line)
        if m and cur is not None:
            cur["a"][m.group(1)] = m.group(2)
    while stack:
        close(stack.pop()[1])
    return res


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    import os
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))) + "/"
    p = subprocess.Popen(["llvm-dwarfdump", "--debug-info", sys.argv[1]], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, errors="replace")
    res = scan(p.stdout, root)
    for (name, path), (size, before, bases) in sorted(res.items()):
        what = []
        if before:
            what.append("members first: " + ", ".join(before))
        if bases:
            what.append("base without virtuals: " + ", ".join(bases))
        print("%-20s %-52s size %-5s %s" % (name, path, size, "; ".join(what)))


if __name__ == "__main__":
    main()
