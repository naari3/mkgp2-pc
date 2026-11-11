#!/usr/bin/env python3
"""Check what the Windows loader insists on and Wine and GNU ld do not: every
section of a PE image starts at a multiple of SectionAlignment and the
sections follow each other without a gap. An image that fails is refused
with "This app can't run on your PC".

    python3 port/tools/pe_check.py file.exe [...]
"""
import struct
import sys


def check(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec, optsize = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    align, size_of_image = struct.unpack_from("<I", d, opt + 32)[0], struct.unpack_from("<I", d, opt + 56)[0]
    bad, expect = [], None
    for i in range(nsec):
        s = opt + optsize + 40 * i
        name = d[s:s + 8].rstrip(b"\0").decode("latin-1")
        vsize, va = struct.unpack_from("<II", d, s + 8)
        if va % align:
            bad.append(f"{name}: address 0x{va:X} is not a multiple of 0x{align:X}")
        elif expect is not None and va != expect:
            bad.append(f"{name}: address 0x{va:X}, expected 0x{expect:X} after the section before")
        expect = (va + max(vsize, 1) + align - 1) // align * align
    if expect != size_of_image:
        bad.append(f"SizeOfImage 0x{size_of_image:X}, sections end at 0x{expect:X}")
    print(f"{path}: {'OK' if not bad else 'NOT LOADABLE ON WINDOWS'} ({nsec} sections)")
    for b in bad:
        print("   ", b)
    return not bad


if __name__ == "__main__":
    sys.exit(0 if all([check(p) for p in sys.argv[1:]]) else 1)
