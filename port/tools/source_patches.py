"""Port-side corrections to game sources, kept as patches.

The game sources under src/ and include/ come from the decompilation and
stay as they are there: they reproduce the arcade program, including the
places where it reads or writes outside an object and only works because of
how the console's stack happens to be laid out. Where such a place breaks
on the PC, the correction lives here in the port as
port/patches/<source path>.patch, a unified diff against the file, and is
applied to a copy in the build folder just before the file is compiled.

A patch whose context no longer matches the source stops the build: the
source changed upstream and the patch has to be looked at again.
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIR = os.path.join(ROOT, "port", "patches")


def patch_for(path):
    p = os.path.join(DIR, path + ".patch")
    return p if os.path.exists(p) else None


def apply(text, patch, name="patch"):
    src = text.split("\n")
    out = []
    pos = 0
    lines = patch.split("\n")
    i = 0
    while i < len(lines) and not lines[i].startswith("@@"):
        i += 1
    while i < len(lines):
        m = re.match(r"@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@", lines[i])
        if not m:
            raise SystemExit(f"{name}: bad hunk header: {lines[i]}")
        start = int(m.group(1)) - 1
        i += 1
        old, new = [], []
        while i < len(lines) and not lines[i].startswith("@@"):
            l = lines[i]
            i += 1
            if l.startswith("\\") or (l == "" and i == len(lines)):
                continue
            if l[:1] in (" ", ""):
                old.append(l[1:])
                new.append(l[1:])
            elif l[0] == "-":
                old.append(l[1:])
            elif l[0] == "+":
                new.append(l[1:])
            else:
                raise SystemExit(f"{name}: bad patch line: {l}")
        # the line number is a hint only; the context has to match exactly
        at = None
        for d in sorted(range(-200, 201), key=abs):
            s = start + d
            if s >= pos and src[s:s + len(old)] == old:
                at = s
                break
        if at is None:
            # already in the source (a correction on its way out of upstream)
            for d in sorted(range(-200, 201), key=abs):
                s = start + d
                if s >= pos and src[s:s + len(new)] == new:
                    at = s
                    old = new
                    break
        if at is None:
            raise SystemExit(f"{name}: hunk at line {start + 1} does not match the source; "
                             "the source changed, review the patch")
        out += src[pos:at] + new
        pos = at + len(old)
    return "\n".join(out + src[pos:])


def patched_source(path, out):
    """Write the patched copy of `path` (relative to the repository root) to
    `out` and return True, or return False when the file has no patch."""
    p = patch_for(path)
    if not p:
        return False
    full = os.path.join(ROOT, path)
    text = apply(open(full, encoding="utf-8", errors="surrogateescape").read(),
                 open(p, encoding="utf-8").read(), os.path.relpath(p, ROOT))
    with open(out, "w", encoding="utf-8", errors="surrogateescape") as f:
        # diagnostics and debug information keep naming the original file
        f.write(f'#line 1 "{full}"\n' + text)
    return True
