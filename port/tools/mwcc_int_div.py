#!/usr/bin/env python3
"""Integer division the way the console does it, on LLVM IR text.

A PowerPC divide instruction never traps: with a zero divisor (or the one
signed overflow, INT_MIN / -1) it just leaves an unspecified value in the
result register, and the game relies on that in places
(`rand() % counts[i]` with an empty table entry, then a range check that
makes the garbage harmless). An x86 divide raises SIGFPE instead.

Every division or remainder whose divisor is not a constant becomes a call
to a small helper (port/platform/runtime/mwcc_runtime.c) that returns what
the console's CPU returns. A remainder is computed as MWCC computes it,
`a - (a / b) * b`, so a zero divisor gives back the dividend.
"""
import re

_OP = re.compile(r"^(\s*%[\w.]+ = )(sdiv|srem|udiv|urem)(?: exact)? (i32|i64) (.+), (%[\w.]+)((?:, !\w+ !\d+)*)$", re.M)


def rewrite(text):
    used = set()

    def one(m):
        head, op, ty, a, b, tail = m.groups()
        name = "__mw_%s%s" % (op, ty[1:])
        used.add((name, ty))
        return "%scall %s @%s(%s %s, %s %s)%s" % (head, ty, name, ty, a, ty, b, tail)

    text, n = _OP.subn(one, text)
    for name, ty in sorted(used):
        if not re.search(r"^declare .*@%s\(" % name, text, re.M):
            text += "\ndeclare %s @%s(%s, %s)\n" % (ty, name, ty, ty)
    return text, n
