#!/usr/bin/env python3
"""Where does the sign of plain `char` change what the game does?

MWCC's default for plain `char` is signed, and the matching build passes
`-char unsigned` only for the Triforce library and most of the Dolphin SDK
(docs/port/char_sign.md). The port compiled every file with -funsigned-char
until 2026-11-12. This tool keeps that from coming back and shows what it
cost:

  char_sign_census.py                 functions whose code differs between
                                      -fsigned-char and -funsigned-char
  char_sign_census.py --lines         source lines where the WRONG sign makes
                                      a comparison constant (clang warnings)
  char_sign_census.py --check-table [BUILD_NINJA]
                                      compare compile_census.char_flag() with
                                      the -char options of the matching
                                      build (a generated build.ninja of
                                      mkgp2-decomp; default ./build.ninja)

The first mode compiles each file twice to optimised LLVM IR and compares
the functions one by one, with the `signext` / `zeroext` marks on arguments
and results left out: a function that only hands a char on is not listed,
the one that widens or compares it is. Initialised data whose bytes differ
is listed as `data NAME`. A listed function is one where the
two readings of `char` compute something different; whether the game ever
has a negative value there is for a person to judge.

Files are read as they are in src/, without port/patches.
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

ROOT = cc.ROOT
SIGN_FLAGS = ("-fsigned-char", "-funsigned-char")
C_RELAX = ["-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int",
           "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types", "-Wno-error=return-type"]
# Comparisons that are constant for one reading of a narrow type.
WARN = ["-Wtautological-constant-out-of-range-compare", "-Wtautological-unsigned-char-zero-compare",
        "-Wtautological-unsigned-zero-compare", "-Wtautological-value-range-compare", "-Wswitch"]
WARN_RE = re.compile(r"(.+?):(\d+):\d+: warning: (.*) \[-W(tautological[\w-]*|switch)\]")


def lang(path):
    return ["-x", "c++", "-std=gnu++98"] if cc.is_cxx(path, cc.TARGET) else ["-x", "c", "-std=gnu99", "-Dinline="] + C_RELAX


def flags(path, sign):
    return [f for f in cc.base_flags(path) if f not in SIGN_FLAGS and f != "-fsyntax-only"] + [sign]


_DEFINE = re.compile(r"^define [^@]*@(\"[^\"]+\"|[\w.$?@]+)\(", re.M)
_EXT = re.compile(r"\b(signext|zeroext) ")


def functions(ir):
    """name -> body text, extension marks removed."""
    out = {}
    pos = [(m.start(), m.group(1).strip('"')) for m in _DEFINE.finditer(ir)]
    for i, (start, name) in enumerate(pos):
        end = ir.find("\n}\n", start)
        body = ir[start:end if end >= 0 else None]
        body = re.sub(r"#\d+|, ![\w.]+ !\d+", "", _EXT.sub("", body))
        out[name] = body
    # initialised data: `int x = (char)0x80;` differs in the object, not in code
    for m in re.finditer(r"^@(\"[^\"]+\"|[\w.$?@]+) = (.*)$", ir, re.M):
        out["data " + m.group(1).strip('"')] = m.group(2)
    return out


def ir_of(path, sign):
    cmd = ["clang", "-target", cc.TARGET, "-S", "-emit-llvm", "-O1", "-g0", "-fcommon", "-fshort-wchar", "-fno-discard-value-names", "-o", "-"]
    cmd += flags(path, sign) + ["-include", cc.COMPAT] + lang(path) + [path]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
    return r.stdout if r.returncode == 0 else None


def diff_one(path):
    a, b = ir_of(path, SIGN_FLAGS[0]), ir_of(path, SIGN_FLAGS[1])
    if a is None or b is None:
        return path, None
    fa, fb = functions(a), functions(b)
    return path, sorted(n for n in fa if fa[n] != fb.get(n))


def lines_one(path):
    """Warnings under the sign the file is NOT built with."""
    right = cc.char_flag(path)
    wrong = SIGN_FLAGS[1 - SIGN_FLAGS.index(right)]
    hits = {}
    for sign in (right, wrong):
        cmd = ["clang", "-target", cc.TARGET, "-fsyntax-only", "-fno-caret-diagnostics"]
        cmd += [f for f in flags(path, sign) if f != "-w"] + ["-Wno-everything"] + WARN
        cmd += ["-include", cc.COMPAT] + lang(path) + [path]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
        hits[sign] = {(m.group(1), int(m.group(2)), m.group(3)) for m in map(WARN_RE.match, r.stderr.splitlines()) if m}
    # normalise the type spelling so a line constant under both readings drops out
    return path, sorted(hits[wrong] - hits[right]), sorted(hits[right] - hits[wrong])


def source_files(dirs):
    files = []
    for d in dirs:
        for base, _, names in os.walk(os.path.join(ROOT, "src", d)):
            files += [os.path.relpath(os.path.join(base, n), ROOT) for n in sorted(names) if n.endswith((".c", ".cpp"))]
    return sorted(files)


def check_table(ninja):
    text = open(ninja, encoding="utf-8").read().replace("$\n", "")
    bad = n = 0
    for block in re.split(r"\n(?=build )", text):
        m = re.match(r"build \S+\.o: mwcc\S* +(\S+)", block)
        cf = re.search(r"\n  cflags = (.*)", block)
        if not m or not cf:
            continue
        opts = re.findall(r"-char\s+(\w+)", cf.group(1))
        want = "-funsigned-char" if opts and opts[-1] == "unsigned" else "-fsigned-char"  # MWCC default: signed
        n += 1
        if cc.char_flag(m.group(1)) != want:
            bad += 1
            print(f"MISMATCH {m.group(1)}: matching build {want}, port {cc.char_flag(m.group(1))}")
    print(f"{n} objects of the matching build checked, {bad} mismatch(es)")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*", default=cc.DEFAULT_DIRS, help="subdirectories of src/")
    ap.add_argument("--lines", action="store_true", help="list source lines instead of functions")
    ap.add_argument("--check-table", nargs="?", const="build.ninja", metavar="BUILD_NINJA")
    args = ap.parse_args()
    if args.check_table:
        return check_table(args.check_table)

    files = source_files(args.dirs)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        if args.lines:
            total = 0
            other = 0
            for path, wrong, right in ex.map(lines_one, files):
                for f, line, msg in wrong:
                    total += 1
                    print(f"{f}:{line}: {msg}")
                other += len(right)
            print(f"\n{total} comparison(s) in {len(files)} files are constant only under the wrong char sign")
            print(f"({other} are constant only under the right one: the original program has them too)")
            return 0
        per_dir = collections.Counter()
        nfun = nfile = failed = 0
        for path, names in ex.map(diff_one, files):
            if names is None:
                failed += 1
                continue
            if names:
                nfile += 1
                nfun += len(names)
                per_dir[path.split(os.sep)[1]] += len(names)
                print(f"{path}: {' '.join(names)}")
    print(f"\n{nfun} function(s) in {nfile} of {len(files)} files depend on the sign of plain char"
          f" ({failed} file(s) did not compile)")
    for d, c in sorted(per_dir.items()):
        print(f"  src/{d:<12} {c}")
    return 0


if __name__ == "__main__":
    import signal
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    sys.exit(main())
