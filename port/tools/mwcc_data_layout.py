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
    r'^(@([\w.$]+) = (?!private |internal |external |extern_weak |linkonce|weak|available_externally )[^\n]*?)\b(?:common )?(global|constant) ([^\n]*?), align (\d+)', re.M)

SECTIONS = ("rodata", "data", "sdata", "sdata2")


def original_objects(root):
    """name -> (section, address) for the initialised data of symbols.txt."""
    import os
    out = {}
    for line in open(os.path.join(root, "config", "GNLJ82", "symbols.txt"), encoding="utf-8"):
        m = re.match(r"(\w+) = \.(\w+):0x([0-9A-Fa-f]+);.*type:object", line)
        if m and m.group(2) in SECTIONS:
            out[m.group(1)] = (m.group(2), int(m.group(3), 16))
    return out


BSS = ("bss", "sbss", "sbss2")


def original_bss(root):
    """name -> (section, address, size) for the zero-initialised objects."""
    import os
    out = {}
    for line in open(os.path.join(root, "config", "GNLJ82", "symbols.txt"), encoding="utf-8"):
        m = re.match(r"(\w+) = \.(\w+):0x([0-9A-Fa-f]+);.*type:object size:0x([0-9A-Fa-f]+)", line)
        if m and m.group(2) in BSS:
            out[m.group(1)] = (m.group(2), int(m.group(3), 16), int(m.group(4), 16))
    return out


def bss_script(bss, host_sizes, notes, noload=True):
    """Zero-initialised objects: one block per original section, and every
    object's name assigned to its original offset in it (an assignment in
    the script replaces the definitions in the object files). The code
    clears and indexes across object boundaries there (ItemObjectManager_Init
    clears 0x1EC00 bytes of an object symbols.txt gives 0x1E4CB), so the
    neighbours have to be the original ones. An object whose host definition
    is larger than the room up to the next one keeps its own definition.
    host_sizes: name -> largest size any object file gives it."""
    out = ["SECTIONS", "{"]
    assign = []
    for sec in BSS:
        objs = sorted((a, n, z) for n, (s, a, z) in bss.items() if s == sec)
        if not objs:
            continue
        base = objs[0][0] & ~0x1F
        end = max(a + z for a, n, z in objs)
        # (GNU ld gives every NOLOAD section of a PE image the same address;
        # there the blocks are ordinary zero-filled data)
        out.append("  .mw_%s%s : ALIGN(0x1000) { __mw_%s_start = .; . = . + 0x%X; }"
                   % (sec, " (NOLOAD)" if noload else "", sec, end - base + 32))
        for i, (a, n, z) in enumerate(objs):
            room = (objs[i + 1][0] if i + 1 < len(objs) else end) - a
            if host_sizes.get(n, 0) > max(room, z):
                notes.append("%s: 0x%X bytes here, 0x%X in the original" % (n, host_sizes[n], room))
                continue
            assign.append("%s = __mw_%s_start + 0x%X;" % (n, sec, a - base))
    out += ["}", "INSERT AFTER .bss;"] + assign + [""]
    return "\n".join(out)


def rewrite(text, objects=None):
    """objects (from original_objects): every object of that table the unit
    defines goes into an input section of its own, `mw.<section>.<name>`, so
    that the link can put it at its original distance from the others (see
    linker_script). Read-only objects become writable on the way, which lets
    one output section hold them all."""
    text, n = _GLOBAL.subn(lambda m: m.group(1) + "4", text)
    if objects:
        def place(m):
            o = objects.get(m.group(2))
            if o is None or "section " in m.group(0) or "comdat" in m.group(0) or "thread_local" in m.group(0):
                return m.group(0)
            return '%sglobal %s, section "mw.%s.%s", align %s' % (m.group(1), m.group(4), o[0], m.group(2), m.group(5))
        text = _DEF.sub(place, text)
    return text, n


def linker_script(objects, inputs, notes):
    """GNU ld script (INSERT form) that lays the objects out as the original
    does, per original section. inputs: name -> [(size, align), ...] over all
    object files (one entry per defining file). An object is left to the
    catch-all at the end when it is defined more than once, would overlap its
    neighbour (its host type is larger than the original object) or needs an
    alignment its original address does not have."""
    out = ["SECTIONS", "{"]
    for sec in SECTIONS:
        names = sorted((a, n) for n, (s, a) in objects.items() if s == sec and n in inputs)
        if not names:
            continue
        base = names[0][0] & ~0x1F
        out.append("  .mw_%s : ALIGN(0x1000)" % sec)
        out.append("  {")
        out.append("    __mw_%s_start = .;" % sec)
        cursor = 0
        for a, n in names:
            defs = inputs[n]
            size, align = defs[0]
            off = a - base
            if len(defs) != 1:
                notes.append("%s: defined in %d units" % (n, len(defs)))
            elif off < cursor:
                notes.append("%s: overlaps the object before it by 0x%X" % (n, cursor - off))
            elif align > 1 and a % align:
                notes.append("%s: host alignment %d does not fit address 0x%X" % (n, align, a))
            else:
                out.append("    . = __mw_%s_start + 0x%X; *(mw.%s.%s)" % (sec, off, sec, n))
                cursor = off + size
        out.append("    *(mw.%s.*)" % sec)
        out.append("  }")
    out += ["}", "INSERT AFTER .data;", ""]
    return "\n".join(out)


if __name__ == "__main__":
    import sys
    out, n = rewrite(open(sys.argv[1]).read())
    open(sys.argv[2], "w").write(out)
    sys.stderr.write("%d objects realigned\n" % n)
