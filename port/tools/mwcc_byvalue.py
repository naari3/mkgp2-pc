#!/usr/bin/env python3
"""Struct arguments the way MWCC passes them, on LLVM IR text.

MWCC passes a struct or class argument as the address of a copy the caller
made, so the sources are free to declare the same function both ways:
`f(Vec3 v)` in one unit and `f(Vec3 *v)` in another, or call it through a
function-pointer cast of the other kind (tools/port_abi/byvalue_struct.py
upstream lists a few hundred such places). On 32-bit x86 a by-value struct is
copied onto the stack instead, and the two spellings no longer meet.

Dropping `byval` makes every such argument a plain pointer, which is MWCC's
convention. A function defined with a by-value parameter copies it on entry,
so that it still owns what it modifies whoever called it. Functions of the
host's own libraries keep their convention (HOST: Aurora's GX takes GXColor
by value).
"""
import re

HOST = re.compile(r"GX[A-Z][A-Za-z0-9]*$")
_T = r'(?:%"[^"]*"|%[\w.$:]+|\{[^{}]*\}|\[[^\[\]]*\])'
_BYVAL = re.compile(r" byval\((" + _T + r")\)")
_PARAM = re.compile(r"ptr((?: \w+(?:\(\d+\))?)*?) byval\((" + _T + r")\)((?: \w+(?:\(\d+\))?)*?)(?: align (\d+))? (%[\w.$]+|%\"[^\"]*\")")
_MEMCPY = "declare void @llvm.memcpy.p0.p0.i32(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i32, i1 immarg)"


def rewrite(text):
    n = 0
    used_memcpy = False
    out = []
    pending = None
    for line in text.split("\n"):
        if pending is not None and (line.endswith(":") or line.startswith("  ")):
            # the first line of the body (after the entry label, if printed)
            if line.endswith(":") and not line.startswith("  "):
                out.append(line)
                continue
            out.extend(pending)
            pending = None
        if " byval(" in line:
            m = re.search(r"@([\w.$]+|\"[^\"]*\")\(", line)
            direct = bool(re.match(r"\s*(?:%[\w.$]+ = )?(?:tail |musttail |notail )?(?:call|invoke) [^@]*@", line)) or line.startswith(("define", "declare"))
            if direct and m and HOST.match(m.group(1).strip('"')):
                out.append(line)
                continue
            if line.startswith("define"):
                copies = []

                def param(m):
                    name = m.group(5)
                    if re.match(r"%\d+$", name):  # unnamed (numbered) parameter: no copy
                        return f"ptr{m.group(1)}{m.group(3)} {name}"
                    ref = (name[:-1] + '.byref"') if name.endswith('"') else name + ".byref"
                    align = m.group(4) or "4"
                    copies.append(f"  {name} = alloca {m.group(2)}, align {align}")
                    copies.append(f"  call void @llvm.memcpy.p0.p0.i32(ptr align {align} {name}, ptr align {align} {ref}, "
                                  f"i32 ptrtoint (ptr getelementptr ({m.group(2)}, ptr null, i32 1) to i32), i1 false)")
                    return f"ptr{m.group(1)}{m.group(3)} {ref}"
                line, k = _PARAM.subn(param, line)
                n += k
                if copies:
                    pending = copies
                    used_memcpy = True
            line, k = _BYVAL.subn("", line)
            n += k
        out.append(line)
    text = "\n".join(out)
    if used_memcpy and "declare void @llvm.memcpy.p0.p0.i32(" not in text:
        text += "\n" + _MEMCPY + "\n"
    return text, n
