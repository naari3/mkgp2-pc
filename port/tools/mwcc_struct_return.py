#!/usr/bin/env python3
"""Struct results the way MWCC passes them, on LLVM IR text.

MWCC returns a struct through a hidden first argument and nothing else about
the call changes, so the sources are free to declare the same function both
ways: `Iter insert(List *, ...)` in one unit and
`void insert(Iter *result, List *, ...)` in another (the shared list
functions, DoublyLinkedList_PushBefore and friends, are called both ways).
On 32-bit x86 the two are different conventions: a function that returns a
struct removes the hidden pointer from the stack itself, and a caller that
passed the pointer as an ordinary argument then finds its stack 4 bytes off.

Dropping the `sret` attribute makes the hidden pointer an ordinary first
argument everywhere in the game, which is MWCC's convention. Functions of the
host's own libraries keep theirs (HOST lists the ones that return structs).
"""
import re

HOST = {"div", "ldiv", "lldiv", "imaxdiv"}
_SRET = re.compile(r' sret\((?:%"[^"]*"|[^()"]*)\)')


def rewrite(text):
    n = 0
    out = []
    for line in text.split("\n"):
        if " sret(" in line:
            m = re.search(r"@([\w.$]+)\(", line)
            if not (m and m.group(1) in HOST):
                line, k = _SRET.subn("", line)
                n += k
        out.append(line)
    return "\n".join(out), n
