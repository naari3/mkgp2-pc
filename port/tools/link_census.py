#!/usr/bin/env python3
"""Compile the decompiled sources to host objects and count what a link needs.

Second measurement after compile_census.py. Every file the host compiler
accepts is compiled to an object (32-bit little-endian, -O0, nothing is
linked or run). From the objects' symbol tables it reports

  * symbols defined more than once (per-TU copies that the matching build
    keeps apart with local scope or section tricks),
  * symbols the chosen directories use but do not define, grouped by the
    part of the original program that owns them (config/GNLJ82/symbols.txt
    and splits.txt), which is the list of SDK entry points the PC build has
    to get from Aurora, port/sdk or port/platform.

    python3 port/tools/link_census.py                 # game, sound, ui, util, resource
    python3 port/tools/link_census.py --list dolphin/gx   # undefined symbols owned by src/dolphin/gx
"""
import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compile_census as cc  # noqa: E402

ROOT = cc.ROOT
OUT = os.path.join(ROOT, "build", "pc-census")
DEFAULT_DIRS = ["game", "sound", "ui", "util", "resource"]


def compile_one(path, target):
    obj = os.path.join(OUT, path.replace(os.sep, "__") + ".o")
    cmd = ["clang", "-target", target, "-c", "-O0", "-fno-exceptions", "-fno-rtti", "-fcommon"] + cc.BASE[1:]
    cmd += ["-include", cc.COMPAT]
    cmd += ["-x", "c++", "-std=gnu++98"] if cc.is_cxx(path, target) else ["-x", "c", "-std=gnu99"]
    r = subprocess.run(cmd + [path, "-o", obj], cwd=ROOT, capture_output=True, text=True, errors="replace")
    return path, (obj if r.returncode == 0 else None)


def owners():
    """symbol name -> unit path (as in splits.txt), by address."""
    ranges = []  # (start, end, unit)
    unit = None
    for line in open(os.path.join(ROOT, "config", "GNLJ82", "splits.txt"), encoding="utf-8"):
        if line and not line[0].isspace() and line.rstrip().endswith(":") and line.strip() != "Sections:":
            unit = line.strip()[:-1]
            continue
        m = re.search(r"start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)", line)
        if m and unit:
            ranges.append((int(m.group(1), 16), int(m.group(2), 16), unit))
    ranges.sort()
    starts = [r[0] for r in ranges]
    import bisect
    table = {}
    for line in open(os.path.join(ROOT, "config", "GNLJ82", "symbols.txt"), encoding="utf-8"):
        m = re.match(r"(\S+) = \S+:0x([0-9A-Fa-f]+);", line)
        if not m:
            continue
        addr = int(m.group(2), 16)
        i = bisect.bisect_right(starts, addr) - 1
        table[m.group(1)] = ranges[i][2] if i >= 0 and addr < ranges[i][1] else "(no split: auto unit)"
    return table


def group_of(unit):
    parts = unit.split("/")
    if parts[0] in ("dolphin", "sysdolphin", "musyx") and len(parts) > 2:
        return "/".join(parts[:2])
    return parts[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*", default=DEFAULT_DIRS)
    ap.add_argument("--list", metavar="GROUP", help="print the undefined symbols owned by this group")
    ap.add_argument("--dups", action="store_true", help="print symbols defined more than once")
    args = ap.parse_args()
    target = cc.TARGET_SIZE_T_LONG
    os.makedirs(OUT, exist_ok=True)

    files = []
    for d in args.dirs:
        for base, _, names in os.walk(os.path.join(ROOT, "src", d)):
            files += [os.path.relpath(os.path.join(base, n), ROOT) for n in names if n.endswith((".c", ".cpp"))]
    files.sort()
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        built = dict(ex.map(lambda p: compile_one(p, target), files))
    objs = [o for o in built.values() if o]

    defined = collections.defaultdict(list)
    undefined = collections.defaultdict(set)
    for i in range(0, len(objs), 200):
        out = subprocess.run(["llvm-nm", "-g", "--format=posix"] + objs[i:i + 200],
                             capture_output=True, text=True).stdout
        cur = None
        for line in out.splitlines():
            if line.endswith(":") and " " not in line:
                cur = line[:-1]
                continue
            f = line.split()
            if len(f) < 2:
                continue
            name, kind = f[0], f[1]
            if kind == "U":
                undefined[name].add(cur)
            elif kind in "TDBRSGC":   # strong definitions and commons; weak (W, V) inline copies are fine
                defined[name].append((cur, kind))

    weak = set()
    dups = {n: v for n, v in defined.items() if len(v) > 1 and not all(k == "C" for _, k in v)}
    missing = {n: u for n, u in undefined.items() if n not in defined}
    # weak definitions satisfy references too
    for i in range(0, len(objs), 200):
        out = subprocess.run(["llvm-nm", "-g", "--format=posix"] + objs[i:i + 200], capture_output=True, text=True).stdout
        for line in out.splitlines():
            f = line.split()
            if len(f) >= 2 and f[1] in "WVwv":
                weak.add(f[0])
    missing = {n: u for n, u in missing.items() if n not in weak}

    own = owners()
    groups = collections.defaultdict(list)
    for n in missing:
        if n.startswith("_Z"):
            groups["(C++ mangled, host ABI)"].append(n)
        elif n in own:
            groups[group_of(own[n])].append(n)
        else:
            groups["(not in symbols.txt)"].append(n)

    # A name used with C++ linkage in one file and defined with C linkage in
    # another (or the reverse) can never link: MWCC `#pragma cplusplus on`
    # regions leave the same header declared both ways.
    plain = {}
    for n in list(defined) + list(weak):
        m = re.match(r"_Z(\d+)", n)
        if m:
            k = int(m.group(1))
            plain.setdefault(n[m.end():m.end() + k], n)
    linkage = [n for n in missing if not n.startswith("_Z") and n in plain]
    for n in missing:
        m = re.match(r"_Z(\d+)", n)
        if m and n[m.end():m.end() + int(m.group(1))] in defined:
            linkage.append(n)
    print(f"objects built: {len(objs)} / {len(files)}")
    print(f"external symbols defined: {len(defined)}; defined more than once: {len(dups)}")
    print(f"used but not defined by these directories: {len(missing)}")
    for g in sorted(groups, key=lambda g: -len(groups[g])):
        print(f"  {len(groups[g]):>5}  {g}")
    print(f"of those, same function under the other language linkage (C vs C++): {len(linkage)}")
    if args.dups:
        for n in sorted(dups):
            print(f"dup {n}: {len(dups[n])} definitions")
    if args.list:
        for n in sorted(groups.get(args.list, [])):
            print(f"{n}  ({len(missing[n])} users)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
