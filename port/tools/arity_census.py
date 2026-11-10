#!/usr/bin/env python3
"""Calls that pass a different number of arguments than the callee defines.

The matching build only compares machine code, so a call written through a
local prototype with fewer parameters still matches when the missing argument
happens to be in the right register already (`buf = Alloc(n);
DisplayContext_Init();`). On PC the callee then reads garbage. This compiles
every unit to LLVM IR with the census flags and compares each direct call's
argument count with the definition's parameter count.

    python3 port/tools/arity_census.py [dirs...]          # fewer arguments only
    python3 port/tools/arity_census.py --all [dirs...]    # also calls with extra arguments
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
import link_census as lc  # noqa: E402

ROOT = cc.ROOT if hasattr(cc, "ROOT") else lc.ROOT


def count_args(text, i):
    """text[i] is '(' ; returns (number of top-level items, has `...`)."""
    depth, n, seen, j, start = 0, 0, False, i, i + 1
    while j < len(text):
        c = text[j]
        if c in "([{<":
            depth += 1
        elif c in ")]}>":
            depth -= 1
            if depth == 0:
                break
        elif c == "," and depth == 1:
            n += 1
        elif not c.isspace() and depth >= 1 and j > i:
            seen = True
        j += 1
    body = text[start:j]
    return (n + 1 if seen else 0), "..." in body.split(",")[-1]


def scan(path):
    cmd = ["clang", "-target", cc.TARGET, "-S", "-emit-llvm", "-O0", "-fno-exceptions", "-fno-rtti", "-fcommon"] + cc.base_flags(path)[1:]
    cmd += ["-include", cc.COMPAT]
    cmd += ["-x", "c++", "-std=gnu++98"] if cc.is_cxx(path, cc.TARGET) else [
        "-x", "c", "-std=gnu99", "-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int",
        "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types"]
    r = subprocess.run(cmd + [path, "-o", "-"], cwd=ROOT, capture_output=True, text=True, errors="replace")
    defs, calls = {}, []
    if r.returncode != 0:
        return path, defs, calls
    for line in r.stdout.splitlines():
        if line.startswith("define "):
            m = re.search(r"@([A-Za-z_][\w.$]*)\(", line)
            if m:
                n, va = count_args(line, m.end() - 1)
                defs[lc.unsuffix(m.group(1))] = None if va else n
        elif " call " in line or line.lstrip().startswith("call "):
            m = re.search(r"\bcall\b[^@]*?@([A-Za-z_][\w.$]*)\(", line)
            if m and "(...)" not in line[:m.start(1)].replace("(...) ", "X"):
                n, _ = count_args(line, m.end() - 1)
                calls.append((lc.unsuffix(m.group(1)), n))
            elif m:
                n, _ = count_args(line, m.end() - 1)
                calls.append((lc.unsuffix(m.group(1)), n))
    return path, defs, calls


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*", default=["game", "sound", "ui", "util", "resource", "triforce"])
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()
    files = []
    for d in args.dirs:
        for base, _, names in os.walk(os.path.join(ROOT, "src", d)):
            files += [os.path.relpath(os.path.join(base, n), ROOT) for n in names if n.endswith((".c", ".cpp"))]
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        res = list(ex.map(scan, sorted(files)))
    defs = {}
    for path, d, _ in res:
        for n, k in d.items():
            if not n.startswith("_Z"):
                defs[n] = (k, path)
    rows = collections.OrderedDict()
    for path, _, calls in res:
        for n, k in calls:
            if n in defs and defs[n][0] is not None and k != defs[n][0] and (args.all or k < defs[n][0]):
                rows.setdefault((path, n, k, defs[n][0], defs[n][1]), 0)
                rows[(path, n, k, defs[n][0], defs[n][1])] += 1
    for (path, n, k, want, where), times in rows.items():
        print(f"{path}: {n} called with {k}, defined with {want} in {where}" + (f" ({times} calls)" if times > 1 else ""))
    fewer = sum(1 for (_, _, k, want, _) in rows if k < want)
    print(f"# {len(rows)} caller/callee pairs ({fewer} with fewer arguments than the definition), "
          f"{len({r[1] for r in rows})} callees, {len({r[0] for r in rows})} calling files", file=sys.stderr)
    return 0


if __name__ == "__main__":
    import signal
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    sys.exit(main())
