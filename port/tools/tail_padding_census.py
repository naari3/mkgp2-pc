#!/usr/bin/env python3
"""Lists C++ classes whose first own member the host compiler places inside
the tail padding of a base class.

MWCC never does that: a derived class starts after sizeof(base). The Itanium
C++ ABI (clang, gcc) reuses the padding at the end of a base that is not a
plain C struct, so `class B { virtual ~B(); int a; char b; }` followed by
`class D : B { short c; }` has c at 6 on the host and at 8 in the game. Where
one translation unit builds such an object and another, or loaded data,
reads it with a flat struct, the member is read from the wrong place (seen:
PanelIcon::resId, the "round 1" banner drew picture 0).

Reads the DWARF of the linked bring-up executable:

    python3 port/tools/tail_padding_census.py [build/pc-gfx/cmake/mkgp2-gfx]

The cure is on the decomp side and keeps the MWCC build byte-identical: name
the padding at the end of the base (`char pad86[2];`).
"""
import collections
import re
import subprocess
import sys


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "build/pc-gfx/cmake/mkgp2-gfx"
    text = subprocess.run(["llvm-dwarfdump", "--debug-info", exe], capture_output=True, text=True, check=True).stdout
    die = re.compile(r"^(0x[0-9a-f]+):(\s+)(DW_TAG_\w+|NULL)")
    attr = re.compile(r"^\s+(DW_AT_\w+)\s+\((.*)\)\s*$")
    records = {}  # offset -> dict
    stack = []  # (depth, record offset or None)
    cur = None
    unit = ""
    for line in text.splitlines():
        m = die.match(line)
        if m:
            off, depth, tag = int(m.group(1), 16), len(m.group(2)), m.group(3)
            while stack and stack[-1][0] >= depth:
                stack.pop()
            cur = None
            if tag == "DW_TAG_compile_unit":
                cur = {"tag": tag}
                unit_holder = cur
                units.append(cur)
            elif tag in ("DW_TAG_class_type", "DW_TAG_structure_type"):
                cur = {"tag": tag, "off": off, "bases": [], "members": [], "unit": units[-1] if units else None}
                records[off] = cur
                stack.append((depth, cur))
            elif tag in ("DW_TAG_inheritance", "DW_TAG_member") and stack and stack[-1][0] == depth - 2:
                cur = {"tag": tag}
                stack[-1][1]["bases" if tag == "DW_TAG_inheritance" else "members"].append(cur)
            continue
        m = attr.match(line)
        if m and cur is not None:
            cur[m.group(1)] = m.group(2)
    found = collections.OrderedDict()
    for r in records.values():
        if "DW_AT_declaration" in r:
            continue
        for b in r["bases"]:
            t = re.match(r"0x([0-9a-f]+)", b.get("DW_AT_type", ""))
            if not t:
                continue
            base = records.get(int(t.group(1), 16))
            if base is None or "DW_AT_byte_size" not in base:
                continue
            start = int(b.get("DW_AT_data_member_location", "0"), 0)
            end = start + int(base["DW_AT_byte_size"], 0)
            inside = [m for m in r["members"] if "DW_AT_data_member_location" in m and "DW_AT_external" not in m
                      and start < int(m["DW_AT_data_member_location"], 0) < end]
            later = [x for x in r["bases"] if x is not b and start < int(x.get("DW_AT_data_member_location", "0"), 0) < end]
            if inside or later:
                m0 = min(int(m["DW_AT_data_member_location"], 0) for m in inside) if inside else int(later[0]["DW_AT_data_member_location"], 0)
                src = (r["unit"] or {}).get("DW_AT_name", "?").strip('"')
                key = (src, r.get("DW_AT_name", "?").strip('"'), base.get("DW_AT_name", "?").strip('"'))
                found[key] = (end - start, m0 - start, (inside[0].get("DW_AT_name", "?").strip('"') if inside else "(second base)"))
    by_file = collections.defaultdict(list)
    for (src, cls, base), (size, at, member) in found.items():
        by_file[src].append((cls, base, size, at, member))
    print("# classes whose member sits in a base's tail padding on the host: %d in %d files" % (len(found), len(by_file)))
    print("# file: class : base (sizeof base) <- first member inside, at offset")
    for src in sorted(by_file):
        for cls, base, size, at, member in sorted(by_file[src]):
            print("%s: %s : %s (0x%X) <- %s at 0x%X" % (src, cls, base, size, member, at))


units = []
if __name__ == "__main__":
    main()
