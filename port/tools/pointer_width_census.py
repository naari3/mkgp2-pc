#!/usr/bin/env python3
"""Measure what in the decompiled source depends on 4-byte pointers.

Runs `clang -fsyntax-only` over the same files as compile_census.py for a
32-bit and a 64-bit target and reports
  - casts between pointers and narrower integers (64-bit target only),
  - records (struct/class/union) whose size or field offsets differ between
    the two targets, and how many files use one,
  - record names that already have more than one layout on the 32-bit target,
  - accesses by byte offset (`*(T *)(p + 0x10)` and the per-file macros for
    it), which no compiler can adjust.
Results as of 2026-11-12 are in docs/port/64bit.md.

    python3 port/tools/pointer_width_census.py            # about 10 minutes
    python3 port/tools/pointer_width_census.py --target64 x86_64-pc-linux-gnu

The default 64-bit target is Windows (long stays 4 bytes). On 64-bit Linux
`u32` (`unsigned long` in include/dolphin/types.h) widens too and every
record with a u32 member changes; that is a typedef to override port-side,
not a property of the game code.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compile_census as cc

ROOT = cc.ROOT
WARN = ["-Wno-everything", "-Wpointer-to-int-cast", "-Wint-to-pointer-cast", "-Wvoid-pointer-to-int-cast"]
LAYOUT_RE = re.compile(r"Type: (.*)\n\nLayout: <ASTRecordLayout\n  Size:(\d+)\n(?:.*?\n)*?  FieldOffsets: \[([^\]]*)\]>")
CAST_RE = re.compile(r"(.+?):(\d+):(\d+): (?:warning|error): (cast (?:to|from) .*)")
OFFSET_MACRO_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(\w+)\(([\w, ]*)\)[ \t]+(\(?\*?[ \t]*\([^()]*\*[ \t]*\)[ \t]*\(.*\+.*)$", re.M)
OFFSET_CAST_RE = re.compile(r"\*\s*\(\s*[\w ]+\*+\s*\)\s*\(\s*\(\s*(?:unsigned |signed )?(?:char|u8|s8)\s*\*\s*\)")


def run(path, target):
    cxx = cc.is_cxx(path, "i686-pc-linux-gnu")
    cmd = ["clang", "-target", target] + [f for f in cc.base_flags(path) if f != "-w"] + WARN + ["-include", cc.COMPAT]
    cmd += ["-x", "c++", "-std=gnu++98"] if cxx else ["-x", "c", "-std=gnu99", "-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int", "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types"]
    r = subprocess.run(cmd + ["-Xclang", "-fdump-record-layouts-simple", path], cwd=ROOT, capture_output=True, text=True, errors="replace")
    casts = {(m.group(1), m.group(2), m.group(3)) for m in map(CAST_RE.match, r.stderr.splitlines()) if m}
    recs = {m.group(1): (m.group(2), m.group(3).replace(" ", "")) for m in LAYOUT_RE.finditer(r.stdout)}
    return casts, recs


def byte_offset_accesses():
    macro = direct = files = 0
    for d in ("src/game", "include/game"):
        for base, _, names in os.walk(os.path.join(ROOT, d)):
            for n in names:
                if not n.endswith((".c", ".cpp", ".h", ".inc")):
                    continue
                t = open(os.path.join(base, n), encoding="utf-8", errors="replace").read()
                m = sum(max(0, len(re.findall(r"\b" + re.escape(x.group(1)) + r"\s*\(", t)) - 1) for x in OFFSET_MACRO_RE.finditer(t))
                c = len(OFFSET_CAST_RE.findall(t))
                macro += m
                direct += c
                files += bool(m or c)
    return macro, direct, files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*", default=cc.DEFAULT_DIRS)
    ap.add_argument("--target64", default="x86_64-w64-windows-gnu")
    args = ap.parse_args()
    files = sorted(os.path.relpath(os.path.join(b, n), ROOT) for d in args.dirs
                   for b, _, ns in os.walk(os.path.join(ROOT, "src", d)) for n in ns if n.endswith((".c", ".cpp")))
    casts, l32, l64, users = set(), collections.defaultdict(set), collections.defaultdict(set), 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        for (c64, r64), (_, r32) in ex.map(lambda p: (run(p, args.target64), run(p, "i686-pc-linux-gnu")), files):
            casts |= c64
            for n, v in r32.items():
                l32[n].add(v)
            for n, v in r64.items():
                l64[n].add(v)
            users += any(n in r32 and r32[n] != v for n, v in r64.items())
    names = set(l32) & set(l64)
    print(f"files: {len(files)}")
    print(f"pointer/integer casts that lose bits: {len(casts)} sites in {len({c[0] for c in casts})} files")
    print(f"records laid out: {len(names)}, layout differs on {args.target64}: {sum(1 for n in names if l32[n] != l64[n])}")
    print(f"files using at least one such record: {users}")
    print(f"record names with more than one 32-bit layout: {sum(1 for n in names if len(l32[n]) > 1)}")
    print("accesses by byte offset in src/game + include/game: %d through macros, %d written out, %d files" % byte_offset_accesses())
    return 0


if __name__ == "__main__":
    sys.exit(main())
