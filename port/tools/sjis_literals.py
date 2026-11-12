#!/usr/bin/env python3
"""Shift-JIS text in the game's string literals.

The sources are UTF-8. The matching build hands each file to MWCC through
sjiswrap, which converts the whole file to Shift-JIS first, so the bytes of
a literal such as "ゲーム" in the arcade program are Shift-JIS, and the game
reads them that way (it looks glyphs up by Shift-JIS code). clang has no
-fexec-charset other than UTF-8, so the port converts the literals itself:
every character outside ASCII inside a string or character literal becomes
the octal escapes of its Shift-JIS bytes ("\\203Q\\201[\\203\\200"; escapes,
because a second byte may be 0x5C, a backslash). Comments and everything
else stay as they are, and no line moves.

    python3 port/tools/sjis_literals.py                 # list the literals that change
    python3 port/tools/sjis_literals.py --check-dol <main.dol or the disc image>
    python3 port/tools/sjis_literals.py -c src/game/x.cpp   # print the converted file

link_census.py calls convert() on each unit and on the headers it includes
(docs/port/text_encoding.md).
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# what sjiswrap uses (encoding_rs SHIFT_JIS = Windows-31J)
CODEC = "cp932"
EXTS = (".c", ".cpp", ".h", ".hpp", ".inc", ".cp", ".pch", ".pch++")


class Unencodable(Exception):
    pass


def scan(text):
    """Yield (start, end, kind, prefix_L) for every string ('"') and
    character ("'") literal in C or C++ source text; start and end are the
    positions of the opening and closing quote."""
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and text.startswith("//", i):
            # a line comment runs on over backslash-newline
            while i < n and text[i] != "\n":
                i += 2 if text[i] == "\\" and i + 1 < n else 1
            continue
        if c == "/" and text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" and j + 1 < n else 1
            if j < n and text[j] == c:
                yield i, j, c, i > 0 and text[i - 1] == "L"
                i = j + 1
            else:
                # not a literal (an apostrophe in an #error line, in
                # assembly): go on after the quote
                i += 1
            continue
        i += 1


def encode_body(body, where=""):
    out = []
    for ch in body:
        if ord(ch) < 0x80:
            out.append(ch)
            continue
        try:
            raw = ch.encode(CODEC)
        except UnicodeEncodeError:
            raise Unencodable(f"{where}: U+{ord(ch):04X} {ch!r} has no Shift-JIS code")
        out.append("".join("\\%03o" % b for b in raw))
    return "".join(out)


def convert(text, name="source", found=None):
    """Return (converted text, number of literals changed). `found`, a list,
    receives (line, kind, wide, literal body) for each."""
    out = []
    pos = 0
    count = 0
    for s, e, kind, wide in scan(text):
        body = text[s + 1:e]
        if body.isascii():
            continue
        line = text.count("\n", 0, s) + 1
        if found is not None:
            found.append((line, kind, wide, body))
        if wide:
            # a wide literal keeps its characters; none in the game today
            continue
        out.append(text[pos:s + 1])
        out.append(encode_body(body, f"{name}:{line}"))
        pos = e
        count += 1
    out.append(text[pos:])
    return "".join(out), count


def converted_source(path, src, out):
    """Write the copy of `src` (the unit `path`, or its patched copy) with
    converted literals to `out` and return True; False when it has none."""
    if os.environ.get("MKGP2PC_UTF8_LITERALS") == "1":
        return False  # the build as it was before, for comparison
    text = open(src, encoding="utf-8", errors="surrogateescape").read()
    if text.isascii():
        return False
    text, count = convert(text, path)
    if not count:
        return False
    if not text.startswith("#line 1 "):
        # diagnostics and debug information keep naming the original file
        text = f'#line 1 "{os.path.join(ROOT, path)}"\n' + text
    with open(out, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(text)
    return True


def included_literals():
    """Literals with text outside ASCII in files that are not units
    (headers, .inc files): link_census.py does not convert those."""
    out = []
    for path in source_files(("src", "include", "libs/melee/src", "port/compat")):
        if path.endswith((".c", ".cpp")):
            continue
        text = open(os.path.join(ROOT, path), "rb").read()
        if text.isascii():
            continue
        found = []
        convert(text.decode("utf-8", "replace"), path, found)
        out += [f"{path}:{line}" for line, _, wide, _ in found]
    return out


def source_files(tops=("src", "include", "libs", "port/patches")):
    for top in tops:
        for d, _, fs in os.walk(os.path.join(ROOT, top)):
            for f in sorted(fs):
                if f.endswith(EXTS) or f.endswith(".patch"):
                    yield os.path.relpath(os.path.join(d, f), ROOT)


def unescape(body):
    """The bytes of a converted literal body, for --check-dol."""
    simple = {"n": 10, "t": 9, "r": 13, "0": 0, "\\": 92, '"': 34, "'": 39, "a": 7, "b": 8, "f": 12, "v": 11}
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.append(ord(c))
            i += 1
            continue
        c = body[i + 1]
        if c in "01234567":
            j = i + 1
            while j < len(body) and j < i + 4 and body[j] in "01234567":
                j += 1
            out.append(int(body[i + 1:j], 8) & 0xFF)
            i = j
        elif c == "x":
            j = i + 2
            while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(body[i + 2:j], 16) & 0xFF)
            i = j
        elif c == "\n":
            i += 2
        else:
            out.append(simple.get(c, ord(c)))
            i += 2
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-c", "--convert", metavar="FILE", help="print the converted file")
    ap.add_argument("--check-dol", metavar="FILE", help="look each converted string up in the original program (main.dol, or the whole disc image)")
    ap.add_argument("-q", "--quiet", action="store_true", help="totals only")
    args = ap.parse_args()
    if args.convert:
        text, _ = convert(open(args.convert, encoding="utf-8").read(), args.convert)
        sys.stdout.write(text)
        return 0
    dol = None
    if args.check_dol:
        import mmap
        dol_file = open(args.check_dol, "rb")
        dol = mmap.mmap(dol_file.fileno(), 0, access=mmap.ACCESS_READ)
    files = total = missing = 0
    for path in source_files():
        try:
            text = open(os.path.join(ROOT, path), encoding="utf-8").read()
        except UnicodeDecodeError as e:
            print(f"{path}: not UTF-8 ({e})")
            continue
        if text.isascii():
            continue
        found = []
        try:
            convert(text, path, found)
        except Unencodable as e:
            print(e)
        if not found:
            continue
        files += 1
        for line, kind, wide, body in found:
            total += 1
            note = ""
            if dol is not None and kind == '"' and not wide:
                raw = unescape(encode_body(body))
                # printf formats and the like are in the program whole; look
                # for the string with its terminator
                if dol.find(raw + b"\0") < 0:
                    note = "   [not in the program]"
                    missing += 1
            if not args.quiet or note:
                print(f"{path}:{line}: {'L' if wide else ''}{kind}{body}{kind}{note}")
    print(f"{total} literals with text outside ASCII in {files} files" + (f", {missing} not found in the program" if dol is not None else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
