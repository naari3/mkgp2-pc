#!/usr/bin/env python3
"""Give data objects MWCC's alignment in clang's LLVM IR.

MWCC aligns every array and struct object to at least 4 bytes; clang gives a
`char` array alignment 1. The decompiled code still reads some data through
an anchor ("the string 0xBC bytes after lbl_802E96B8"), which only works
when the objects of a unit sit at the same distances as in the original.
First seen in Scene3D_Init: the name of the archive's public symbol is read
relative to an unrelated object and came out six bytes off.

Scalars keep their natural alignment, as under MWCC. Objects the compiler
made itself (string literals, private constants) are left alone.
"""
import re

_GLOBAL = re.compile(
    r'^(@(?!\.)[\w.$"]+ = (?!private |internal unnamed_addr )[^\n]*?\b(?:global|constant) '
    r'(?:\[|%struct\.|%class\.|%union\.|\{|<\{)[^\n]*?, align )([12])\b', re.M)


_DEF = re.compile(
    r'^(@(?!\.)[\w.$"]+ = (?!private |internal |external |extern_weak )[^\n]*?)\b(?:common )?(global|constant) ([^\n]*?), align (\d+)', re.M)


def rewrite(text, one_section=None):
    """one_section: put every object the unit defines into this section, in
    definition order. For the generated data units, whose objects all come
    from one section of the original and may be read relative to each other;
    the host would otherwise split them into read-only and writable data."""
    text, n = _GLOBAL.subn(lambda m: m.group(1) + "4", text)
    if one_section:
        text = _DEF.sub(lambda m: '%sglobal %s, section "%s", align %s' % (
            m.group(1), m.group(3), one_section, m.group(4)), text)
    return text, n


if __name__ == "__main__":
    import sys
    out, n = rewrite(open(sys.argv[1]).read(), sys.argv[3] if len(sys.argv) > 3 else None)
    open(sys.argv[2], "w").write(out)
    sys.stderr.write("%d objects realigned\n" % n)
