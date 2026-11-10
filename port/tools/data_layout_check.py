#!/usr/bin/env python3
"""Check that the data objects of each generated data unit keep their
original distances in the host executable.

The game still reads some data relative to another object of the same unit
(docs/port/census.md section 16), so inside a unit the host layout has to
match the original one. Usage: data_layout_check.py <host executable>
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    exe = sys.argv[1]
    host = {}
    for line in subprocess.run(["nm", exe], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            host[p[2]] = int(p[0], 16)
    orig = {}
    for line in open(os.path.join(ROOT, "config", "GNLJ82", "symbols.txt"), encoding="utf-8"):
        m = re.match(r"(\S+) = \.\w+:0x([0-9A-Fa-f]+);", line)
        if m:
            orig[m.group(1)] = int(m.group(2), 16)
    units = bad_units = pairs = 0
    for f in sorted(glob.glob(os.path.join(ROOT, "src", "game", "autodata", "*.c"))):
        names = re.findall(r"^[A-Za-z_][^\n=;(]*?\b(\w+)(?:\[[^\]]*\])*(?: __attribute__\(\(aligned\(\d+\)\)\))? = ", open(f, encoding="utf-8").read(), re.M)
        objs = sorted((orig[n], n) for n in names if n in orig and n in host)
        if len(objs) < 2:
            continue
        units += 1
        first = None
        for (a0, n0), (a1, n1) in zip(objs, objs[1:]):
            pairs += 1
            if host[n1] - host[n0] != a1 - a0 and first is None:
                first = "%s: %s -> %s is 0x%X in the original, 0x%X here" % (
                    os.path.relpath(f, ROOT), n0, n1, a1 - a0, host[n1] - host[n0])
        if first:
            bad_units += 1
            print(first)
    print("%d units, %d object pairs, %d units with a different layout" % (units, pairs, bad_units))


if __name__ == "__main__":
    main()
