#!/usr/bin/env python3
"""Count how much of the decompiled source a host compiler accepts.

Runs `clang -fsyntax-only` (32-bit x86 target, no host headers) over every C
and C++ file under the given src/ subdirectories and tallies the files that
pass and the kinds of errors in the ones that fail. Nothing is built; this
measures portability the way the matching report measures matching.

    python3 port/tools/compile_census.py                # default directories
    python3 port/tools/compile_census.py game sound -v  # list failing files
    python3 port/tools/compile_census.py --json out.json
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DIRS = ["game", "sound", "ui", "util", "resource", "init", "sysdolphin", "musyx", "triforce", "thp"]
COMPAT = os.path.join(ROOT, "port", "compat", "mwcc_compat.h")

BASE = [
    "-fsyntax-only", "-nostdinc", "-w",
    "-ferror-limit=0", "-fno-caret-diagnostics", "-fno-diagnostics-fixit-info",
    "-Iport/compat/include",  # ahead of include/libc: PC versions of stdarg.h, math.h
    "-Iinclude", "-Iinclude/libc", "-Isrc", "-Isrc/game", "-Isrc/zlib", "-I.",
    "-DVERSION_GNLJ82", "-DNDEBUG=1", "-DBUILD_VERSION=0",
    "-DNON_MATCHING",    # take the C side of functions that are asm in the matching build
    "-funsigned-char",   # PowerPC EABI char is unsigned
]
# 32-bit little-endian, the first PC target (docs/port/readiness.md).
TARGET = os.environ.get("MKGP2PC_CENSUS_TARGET", "i686-pc-linux-gnu")
# Same, but with size_t = unsigned long as on CodeWarrior. Used to look past
# the `operator new(unsigned long)` declarations until upstream spells them
# with size_t.
TARGET_SIZE_T_LONG = "i386-unknown-openbsd"

CPLUSPLUS_RE = re.compile(r"^\s*#\s*pragma\s+cplusplus\s+on", re.M)
_lang_flags = None


def configure_lang_cxx():
    """Objects that configure.py compiles with -lang=c++ despite a .c name."""
    global _lang_flags
    if _lang_flags is None:
        _lang_flags = set()
        text = open(os.path.join(ROOT, "configure.py"), encoding="utf-8").read()
        for m in re.finditer(r'Object\([^\n]*?"([^"]+\.c)"[^\n]*', text):
            if re.search(r"-lang[= ]c\+\+", m.group(0)):
                _lang_flags.add("src/" + m.group(1))
    return _lang_flags


def is_cxx(path, target):
    """MWCC switches a .c file to C++ with -lang=c++ or `#pragma cplusplus on`
    (also when the pragma sits in an included header)."""
    if path.endswith(".cpp") or path.replace(os.sep, "/") in configure_lang_cxx():
        return True
    r = subprocess.run(["clang", "-E", "-target", target] + base_flags(path)[1:] + ["-std=gnu99", path],
                       cwd=ROOT, capture_output=True, text=True, errors="replace")
    return bool(CPLUSPLUS_RE.search(r.stdout))


def normalise(msg):
    msg = re.sub(r"'[^']*'", "'_'", msg)
    msg = re.sub(r"\b0x[0-9A-Fa-f]+\b|\b\d+\b", "N", msg)
    return msg


# src/sysdolphin (HSD, shared with the Melee decompilation) is built against
# its own header set in the matching build (cflags_hsd_melee in configure.py).
HSD_INCLUDES = ["-Ilibs/melee/src", "-Ilibs/melee/libs/doldecomp_include", "-Ilibs/melee/src/MSL",
                "-Ilibs/melee/libs/dolphin_include", "-Ilibs/melee/src/sysdolphin/baselib",
                "-Isrc/sysdolphin/baselib", "-DMUST_MATCH"]
HSD_OWN_HEADERS = ("util.c", "id.c")  # these two use the game's include set


def base_flags(path):
    p = path.replace(os.sep, "/")
    if p.startswith("src/sysdolphin/") and not p.endswith(HSD_OWN_HEADERS):
        return [f for f in BASE if not f.startswith("-I") or f == "-Iport/compat/include"] + HSD_INCLUDES
    return BASE


def check(path, use_compat, target):
    cmd = ["clang", "-target", target] + base_flags(path)
    if use_compat:
        cmd += ["-include", COMPAT]
    cmd += ["-x", "c++", "-std=gnu++98"] if is_cxx(path, target) else ["-x", "c", "-std=gnu99", "-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int", "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types"]
    r = subprocess.run(cmd + [path], cwd=ROOT, capture_output=True, text=True, errors="replace")
    errs = []
    for line in r.stderr.splitlines():
        m = re.match(r"(.+?):(\d+):(\d+): (fatal error|error): (.*)", line)
        if m:
            errs.append((m.group(1), normalise(m.group(5))))
    if r.returncode != 0 and not errs:
        errs.append((path, "compiler failed without a diagnostic"))
    return path, errs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*", default=DEFAULT_DIRS, help="subdirectories of src/")
    ap.add_argument("--no-compat", action="store_true", help="do not force-include port/compat/mwcc_compat.h")
    ap.add_argument("--size-t-long", action="store_true",
                    help="use a 32-bit target whose size_t is unsigned long, as on CodeWarrior")
    ap.add_argument("-v", "--verbose", action="store_true", help="list failing files with their first error")
    ap.add_argument("--top", type=int, default=25, help="error kinds to show")
    ap.add_argument("--json", help="write per-file results here")
    args = ap.parse_args()

    files = []
    for d in args.dirs:
        for base, _, names in os.walk(os.path.join(ROOT, "src", d)):
            for n in sorted(names):
                if n.endswith((".c", ".cpp")):
                    files.append(os.path.relpath(os.path.join(base, n), ROOT))
    files.sort()

    target = TARGET_SIZE_T_LONG if args.size_t_long else TARGET
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        for path, errs in ex.map(lambda p: check(p, not args.no_compat, target), files):
            results[path] = errs

    per_dir = collections.defaultdict(lambda: [0, 0])
    kinds = collections.Counter()      # error kind -> number of files showing it
    first = collections.Counter()      # first error of each failing file
    in_header = collections.Counter()  # header that the first error sits in
    for path, errs in results.items():
        d = path.split(os.sep)[1]
        per_dir[d][1] += 1
        if not errs:
            per_dir[d][0] += 1
            continue
        for k in {e[1] for e in errs}:
            kinds[k] += 1
        first[errs[0][1]] += 1
        if errs[0][0] != path:
            in_header[errs[0][0]] += 1

    ok = sum(v[0] for v in per_dir.values())
    print(f"files accepted: {ok} / {len(files)} ({100.0 * ok / max(1, len(files)):.1f} %)")
    for d in sorted(per_dir):
        print(f"  src/{d:<12} {per_dir[d][0]:>5} / {per_dir[d][1]}")
    print(f"\nerror kinds by number of files (top {args.top}):")
    for k, n in kinds.most_common(args.top):
        print(f"  {n:>5}  {k}")
    print(f"\nfirst error of failing files (top {args.top}):")
    for k, n in first.most_common(args.top):
        print(f"  {n:>5}  {k}")
    if in_header:
        print("\nheaders holding the first error (top 15):")
        for k, n in in_header.most_common(15):
            print(f"  {n:>5}  {k}")
    if args.verbose:
        print("\nfailing files:")
        for path in files:
            if results[path]:
                print(f"  {path}: {results[path][0][1]}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump({p: e for p, e in results.items()}, f, indent=1)
    return 0


if __name__ == "__main__":
    import signal
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    sys.exit(main())
