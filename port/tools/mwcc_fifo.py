#!/usr/bin/env python3
"""Turn stores to the write-gather pipe into calls, in clang's LLVM IR.

The SDK's vertex functions are inline stores to 0xCC008000, and the game
also writes there directly (`GXWGFifo.f32 = x;`, about 120 places). A PC
renderer needs each value as a call. The header override
(port/compat/include/dolphin/gx/GXVert.h) takes care of the SDK functions;
this pass takes the direct stores, by the type that is stored:

    store volatile float %x, ptr inttoptr (i32 -872382464 to ptr)
 -> call void @GXParam1f32(float %x)

Five units declare the pipe as an object of their own
(`volatile PPCWGPipe GXWGFifo AT_ADDRESS(0xCC008000);`), which is an
ordinary global on the host; stores to a global of that name count too.

Only used for the rendering build (link_census.py --gfx).
"""
import re

_STORE = re.compile(
    r"store volatile (i8|i16|i32|float) ([^,\n]+), ptr (?:inttoptr \(i32 -872382464 to ptr\)|@GXWGFifo(?:_p\d+)?\b)(?:, align \d+)?")
_FN = {"i8": ("GXParam1u8", "i8 zeroext"), "i16": ("GXParam1u16", "i16 zeroext"),
       "i32": ("GXParam1u32", "i32"), "float": ("GXParam1f32", "float")}


def rewrite(text):
    used = set()

    def one(m):
        name, arg = _FN[m.group(1)]
        used.add(m.group(1))
        return "call void @%s(%s %s)" % (name, arg, m.group(2))
    text, n = _STORE.subn(one, text)
    for ty in sorted(used):
        name, arg = _FN[ty]
        if not re.search(r"^(?:declare|define) [^\n]*@%s\(" % name, text, re.M):
            text += "\ndeclare void @%s(%s)\n" % (name, arg)
    return text, n
