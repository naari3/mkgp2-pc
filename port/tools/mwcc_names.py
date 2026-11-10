#!/usr/bin/env python3
"""MWCC C++ symbol names, enough of them to pair with the host compiler's.

The matching build renames C++ symbols per unit (tools/extab_user_renames.json:
`__ct__11InputObjectFi` -> `InputObj_Ctor_Internal`), because a constructor,
destructor or method written as C++ in one unit is an `extern "C"` function in
another. The PC link has to apply the same renames to objects made by a
compiler with Itanium names. `canon_mwcc` and `canon_itanium` turn both
spellings into one comparable string; anything this does not understand
returns None and is reported by the caller.
"""
import re

BASIC = {"v": "void", "b": "bool", "c": "char", "s": "short", "i": "int", "l": "long",
         "x": "long long", "f": "float", "d": "double", "w": "wchar_t", "e": "..."}
SPECIAL = {"__nw": "operator new", "__nwa": "operator new[]", "__dl": "operator delete",
           "__dla": "operator delete[]", "__as": "operator=", "__vc": "operator[]",
           "__eq": "operator==", "__ne": "operator!=", "__cl": "operator()",
           "__pl": "operator+", "__mi": "operator-", "__ml": "operator*"}


class Bad(Exception):
    pass


def _name(s, i):
    """<len><chars> or Q<n><name>...; returns (text, last component, next index)."""
    if s[i] == "Q":
        n = int(s[i + 1])
        i += 2
        parts = []
        for _ in range(n):
            t, _, i = _name(s, i)
            parts.append(t)
        return "::".join(parts), parts[-1], i
    m = re.match(r"\d+", s[i:])
    if not m:
        raise Bad(s)
    n = int(m.group())
    i += len(m.group())
    t = s[i:i + n]
    if len(t) != n:
        raise Bad(s)
    if "<" in t:
        t = _template(t)
    return t, t.split("<")[0], i + n


def _template(t):
    """`ListImpl<Pv>` -> `ListImpl<void*>`: the arguments are mangled types
    (or integers) separated by commas."""
    head, rest = t.split("<", 1)
    if not rest.endswith(">"):
        raise Bad(t)
    rest, args, depth, start = rest[:-1], [], 0, 0
    for j, ch in enumerate(rest + ","):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == "," and depth == 0:
            a = rest[start:j]
            start = j + 1
            if re.fullmatch(r"-?\d+", a):
                args.append(a)
                continue
            text, k = _type(a, 0)
            if k != len(a):
                raise Bad(t)
            args.append(text)
    out = f"{head}<{', '.join(args)}>"
    return out.replace(">>", "> >")


def _type(s, i):
    """Returns (text in c++filt style, next index)."""
    c = s[i]
    if c == "C":
        t, i = _type(s, i + 1)
        return _const(t), i
    if c == "V":
        return _type(s, i + 1)
    if c in "US":
        t, i = _type(s, i + 1)
        return ("unsigned " if c == "U" else "signed ") + t, i
    if c == "P" or c == "R":
        mark = "*" if c == "P" else "&"
        if s[i + 1] == "F":  # pointer to function
            args, i = _args(s, i + 2, stop="_")
            ret, i = _type(s, i + 1)
            return f"{ret} ({mark})({args})", i
        t, i = _type(s, i + 1)
        if t.endswith("]"):  # pointer to array
            base, dims = t.split(" [", 1)
            return f"{base} ({mark}) [{dims}", i
        return t + mark, i
    if c == "A":
        m = re.match(r"A(\d+)_", s[i:])
        t, j = _type(s, i + len(m.group()))
        if t.endswith("]"):
            base, dims = t.split(" [", 1)
            return f"{base} [{m.group(1)}][{dims}", j
        return f"{t} [{m.group(1)}]", j
    if c in BASIC:
        return BASIC[c], i + 1
    if c.isdigit() or c == "Q":
        t, _, i = _name(s, i)
        return t, i
    raise Bad(s)


def _const(t):
    if t.endswith("*") or t.endswith("&"):
        return t + " const"
    return t + " const"


def _args(s, i, stop=None):
    out = []
    while i < len(s) and s[i] != stop:
        t, i = _type(s, i)
        out.append(t)
    if out == ["void"]:
        out = []
    return ", ".join(out), i


def demangle_mwcc(sym):
    """`Class::name(args)` in c++filt's spelling, or None."""
    try:
        m = re.match(r"(__vt)__(.+)$", sym)
        if m:
            t, _, i = _name(m.group(2), 0)
            return "vtable for " + t if i == len(m.group(2)) else None
        m = re.match(r"(.+?)__([0-9Q].*|F.*|CF.*)$", sym)
        if not m or "$" in sym:
            return None
        base, rest = m.group(1), m.group(2)
        targs = ""
        if "<" in base and not base.startswith("__"):  # function template
            targs = _template(base)[len(base.split("<")[0]):]
            base = base.split("<")[0]
        cls = last = None
        i = 0
        if rest[0] != "F" and not rest.startswith("CF"):
            cls, last, i = _name(rest, 0)
        const = ""
        if rest[i:i + 1] == "C":
            const = " const"
            i += 1
        if rest[i:i + 1] != "F":
            return None
        args, i = _args(rest, i + 1)
        if i != len(rest):
            return None
        if base == "__ct":
            name = last
        elif base == "__dt":
            name = "~" + last
        else:
            name = SPECIAL.get(base, base)
            if name.startswith("__"):
                return None
        full = (f"{cls}::{name}" if cls else name) + targs
        return f"{full}({args}){const}"
    except (Bad, IndexError, AttributeError, ValueError):
        return None


def canon(text):
    """One spelling for both compilers: no spaces, `long` and `int` alike
    (size_t is unsigned long for MWCC and unsigned int on a 32-bit host)."""
    t = text.replace("long long", "\x00")
    t = re.sub(r"\blong\b", "int", t).replace("\x00", "long long")
    return t.replace(" ", "")


def canon_mwcc(sym):
    d = demangle_mwcc(sym)
    return canon(d) if d else None


def canon_itanium(demangled):
    # c++filt prints the return type in front of a function template
    head = demangled.split("(")[0]
    if head.endswith(">") and not head.startswith("vtable"):
        depth = 0
        for j, ch in enumerate(head):
            depth += (ch == "<") - (ch == ">")
            if ch == " " and depth == 0:
                demangled = demangled[j + 1:]
                break
    return canon(demangled)


if __name__ == "__main__":
    import sys
    for a in sys.argv[1:]:
        print(a, "->", demangle_mwcc(a))
