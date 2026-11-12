#!/usr/bin/env python3
"""Lists the draw commands of a frame from a dump (docs/port/dumps.md).

    gx_fifo.py <dump folder or gx_fifo.bin.gz>            every command, one line each
    gx_fifo.py <...> --summary                            counts only
    gx_fifo.py <...> --verts                              also the vertices of every draw
    gx_fifo.py <...> --draw 37 --verts                    one draw (numbered from 1) with what led to it
    gx_fifo.py <folder> --prev                            the frame before (crash folders have it)

The file is what the game's GX calls wrote for the rendering library in one
frame: the console's command stream (BP, CP and XF register loads, vertex
data) plus Aurora's own commands where the console passed an address
(vertex arrays, textures). port/gfx/gx_shim.cpp copies it; the notes in the
listing ("GXCopyTex ...", "emptied unseen") come from there. With the
memory files of the same dump beside it, addresses are named.
"""
import argparse
import gzip
import os
import struct
import sys

ATTRS = ["PNMTXIDX", "TEX0MTXIDX", "TEX1MTXIDX", "TEX2MTXIDX", "TEX3MTXIDX", "TEX4MTXIDX", "TEX5MTXIDX", "TEX6MTXIDX",
         "TEX7MTXIDX", "POS", "NRM", "CLR0", "CLR1", "TEX0", "TEX1", "TEX2", "TEX3", "TEX4", "TEX5", "TEX6", "TEX7"]
POS, NRM, CLR0, CLR1, TEX0 = 9, 10, 11, 12, 13
TYPES = ["none", "direct", "index8", "index16"]
COMP = ["u8", "s8", "u16", "s16", "f32"]
CLR = ["rgb565", "rgb8", "rgbx8", "rgba4", "rgba6", "rgba8"]
PRIMS = {0x80: "quads", 0x88: "quads2", 0x90: "triangles", 0x98: "tristrip", 0xA0: "trifan", 0xA8: "lines",
         0xB0: "linestrip", 0xB8: "points"}

BP = {0x00: "genMode", 0x0F: "indMask", 0x20: "scissorTL", 0x21: "scissorBR", 0x22: "lineptWidth", 0x25: "ras1_ss0", 0x26: "ras1_ss1",
      0x27: "iref", 0x40: "zMode", 0x41: "blendMode", 0x42: "dstAlpha", 0x43: "peControl", 0x44: "fieldMask",
      0x45: "drawDone", 0x47: "peToken", 0x48: "peTokenInt", 0x49: "efbSrcTL", 0x4A: "efbSrcSize", 0x4B: "xfbAddr",
      0x4D: "dispStride", 0x4E: "dispCopyYScale", 0x4F: "clearAR", 0x50: "clearGB", 0x51: "clearZ", 0x52: "copyTrigger",
      0x53: "copyFilter0", 0x54: "copyFilter1", 0x59: "scissorOffset", 0x64: "loadTlut0", 0x65: "loadTlut1",
      0x66: "texInvalidate", 0x67: "perfMode", 0x68: "fieldMode", 0xE8: "fogRange", 0xEE: "fogParam0",
      0xEF: "fogParam1", 0xF0: "fogParam2", 0xF1: "fogParam3", 0xF2: "fogColor", 0xF3: "alphaCompare",
      0xF4: "zTexBias", 0xF5: "zTexMode", 0xFE: "bpMask"}
for _i in range(3):
    BP[0x06 + _i * 3], BP[0x07 + _i * 3], BP[0x08 + _i * 3] = "indMtx%dA" % _i, "indMtx%dB" % _i, "indMtx%dC" % _i
for _i in range(16):
    BP[0x10 + _i] = "indCmd%d" % _i
    BP[0xC0 + _i * 2] = "tevColor%d" % _i
    BP[0xC1 + _i * 2] = "tevAlpha%d" % _i
for _i in range(8):
    BP[0x28 + _i] = "tref%d" % _i
    BP[0x30 + _i * 2] = "suSsize%d" % _i
    BP[0x31 + _i * 2] = "suTsize%d" % _i
    BP[0xF6 + _i] = "tevKSel%d" % _i
for _i in range(4):
    BP[0xE0 + _i * 2] = "tevRegRA%d" % _i
    BP[0xE1 + _i * 2] = "tevRegBG%d" % _i
for _base, _first in ((0x80, 0), (0xA0, 4)):
    for _i in range(4):
        for _k, _n in enumerate(("texMode0", "texMode1", "texImage0", "texImage1", "texImage2", "texImage3", "texTlut")):
            BP[_base + _k * 4 + _i] = "%s_%d" % (_n, _first + _i)

XF = {0x1000: "error", 0x1001: "diagnostics", 0x1002: "state0", 0x1003: "state1", 0x1004: "clock", 0x1005: "clipDisable",
      0x1006: "perf0", 0x1007: "perf1", 0x1008: "inVtxSpec", 0x1009: "numColors", 0x100A: "ambient0",
      0x100B: "ambient1", 0x100C: "material0", 0x100D: "material1", 0x100E: "color0Ctrl", 0x100F: "color1Ctrl",
      0x1010: "alpha0Ctrl", 0x1011: "alpha1Ctrl", 0x1012: "dualTexTransform", 0x1018: "matrixIndexA",
      0x1019: "matrixIndexB", 0x101A: "viewportScaleX", 0x101B: "viewportScaleY", 0x101C: "viewportScaleZ",
      0x101D: "viewportOffsetX", 0x101E: "viewportOffsetY", 0x101F: "viewportOffsetZ", 0x1020: "projectionA",
      0x1021: "projectionB", 0x1022: "projectionC", 0x1023: "projectionD", 0x1024: "projectionE",
      0x1025: "projectionF", 0x1026: "projectionType", 0x103F: "numTexGens"}
for _i in range(8):
    XF[0x1040 + _i] = "texGen%d" % _i
    XF[0x1050 + _i] = "dualTex%d" % _i
XF_FLOAT = set(range(0x101A, 0x1026))

AURORA = {0x0001: "viewportRender", 0x0002: "scissorRender", 0x0003: "projectionFull", 0x0020: "debugGroupPush",
          0x0021: "debugGroupPop", 0x0022: "debugMarker", 0x0030: "loadTexObj", 0x0031: "loadTlut",
          0x0032: "destroyTexObj", 0x0033: "destroyTlut", 0x0034: "destroyCopyTex", 0x0035: "copySrc",
          0x0036: "copyDst", 0x0037: "copyDest", 0x0038: "setTexImgPtr", 0x0039: "beginOffscreen",
          0x003A: "endOffscreen", 0x003B: "dispCopySrc", 0x0040: "drawSized", 0x0041: "drawIndexed",
          0x0042: "drawsBlocking", 0x0050: "readbackPeeks", 0x0051: "readbackCopies", 0x0052: "clear",
          0x1000: "polygonOffset"}
TEXFMT = {0: "I4", 1: "I8", 2: "IA4", 3: "IA8", 4: "RGB565", 5: "RGB5A3", 6: "RGBA8", 8: "C4", 9: "C8", 10: "C14X2",
          14: "CMPR"}


def xf_place(addr):
    if addr < 0x78:
        return "posMtx%d%s" % (addr // 12, "+%d" % (addr % 12) if addr % 12 else "")
    if addr < 0xF0:
        return "texMtx%d%s" % ((addr - 0x78) // 12, "+%d" % ((addr - 0x78) % 12) if (addr - 0x78) % 12 else "")
    if addr < 0x100:
        return "mtx+0x%x" % addr
    if 0x400 <= addr < 0x460:
        return "nrmMtx%d%s" % ((addr - 0x400) // 9, "+%d" % ((addr - 0x400) % 9) if (addr - 0x400) % 9 else "")
    if 0x500 <= addr < 0x600:
        return "postMtx%d%s" % ((addr - 0x500) // 12, "+%d" % ((addr - 0x500) % 12) if (addr - 0x500) % 12 else "")
    if 0x600 <= addr < 0x680:
        return "light%d+%d" % ((addr - 0x600) // 16, (addr - 0x600) % 16)
    return XF.get(addr, "xf%04x" % addr)


class Stop(Exception):
    pass


class State:
    def __init__(self):
        self.vcd = [0] * 21
        self.known = False                       # vertex description known (from the frame's start record)
        self.vat = [[(0, 0, 0)] * 21 for _ in range(8)]
        self.arrays = {}                         # attribute -> (address, size, little-endian)
        self.strides = {}

    def comp(self, attr, fmt):
        """(bytes per component, component count) of a direct attribute."""
        cnt, typ, _ = self.vat[fmt][attr]
        if attr < POS:
            return 1, 1
        if attr in (CLR0, CLR1):
            return {0: 2, 3: 2, 1: 3, 4: 3, 2: 4, 5: 4}.get(typ, 4), 1
        size = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4}.get(typ)
        if size is None:
            raise Stop("vertex format %d: attribute %s has component type %d" % (fmt, ATTRS[attr], typ))
        if attr == POS:
            return size, 3 if cnt else 2
        if attr == NRM:
            return size, 9 if cnt else 3
        return size, 2 if cnt else 1

    def layout(self, fmt):
        """[(attribute, kind, bytes)] of one vertex."""
        out = []
        for a in range(21):
            t = self.vcd[a]
            if t == 0:
                continue
            if t == 1:
                size, n = self.comp(a, fmt)
                out.append((a, "direct", size * n))
            else:
                k = 3 if a == NRM and self.vat[fmt][a][0] == 2 else 1
                out.append((a, TYPES[t], k * (1 if t == 2 else 2)))
        return out

    def describe(self, fmt):
        parts = []
        for a, kind, size in self.layout(fmt):
            cnt, typ, shift = self.vat[fmt][a]
            if kind == "direct" and a >= POS:
                what = CLR[typ] if a in (CLR0, CLR1) and typ < 6 else "%dx%s" % (self.comp(a, fmt)[1], COMP[typ])
                if a not in (CLR0, CLR1) and typ < 4 and shift:
                    what += ">>%d" % shift
            else:
                what = kind
            parts.append("%s:%s" % (ATTRS[a], what))
        return " ".join(parts)


class Listing:
    def __init__(self, args, namer):
        self.a = args
        self.namer = namer
        self.st = State()
        self.draws = 0
        self.counts = {}
        self.verts = 0
        self.lines = []       # since the last draw, for --draw
        self.problems = []

    def ptr(self, v):
        n = self.namer(v) if self.namer else ""
        return "%08x%s" % (v, " (%s)" % n if n else "")

    def emit(self, kind, text, draw=False):
        self.counts[kind] = self.counts.get(kind, 0) + 1
        if self.a.summary:
            return
        if self.a.draw:
            self.lines.append(text)
            if draw:
                if self.draws == self.a.draw:
                    print("\n".join(self.lines))
                self.lines = []
            return
        print(text)

    def problem(self, text):
        self.problems.append(text)
        self.emit("problem", "  !! " + text)

    def note(self, text):
        self.emit("note", "-- " + text)

    def state(self, p):
        self.st.vcd = list(p[:21])
        self.st.known = True
        o = 21
        for f in range(8):
            for a in range(POS, 21):
                self.st.vat[f][a] = tuple(p[o:o + 3])
                o += 3
        self.emit("state", "-- at the frame's start: vertex description " +
                  (" ".join("%s:%s" % (ATTRS[a], TYPES[t]) for a, t in enumerate(self.st.vcd) if t) or "empty"))

    def cp(self, addr, v):
        st = self.st
        name = "cp%02x" % addr
        if addr == 0x30:
            name = "matrixIndexA"
        elif addr == 0x40:
            name = "matrixIndexB"
        elif addr == 0x50:
            name = "vcdLo"
            for i in range(9):
                st.vcd[i] = (v >> i) & 1
            for i in range(4):
                st.vcd[POS + i] = (v >> (9 + i * 2)) & 3
        elif addr == 0x60:
            name = "vcdHi"
            for i in range(8):
                st.vcd[TEX0 + i] = (v >> (i * 2)) & 3
        elif 0x70 <= addr < 0x78:
            f = st.vat[addr - 0x70]
            name = "vatA fmt%d" % (addr - 0x70)
            f[POS] = (v & 1, (v >> 1) & 7, (v >> 4) & 31)
            f[NRM] = (2 if v >> 31 else (v >> 9) & 1, (v >> 10) & 7, 0)
            f[CLR0] = ((v >> 13) & 1, (v >> 14) & 7, 0)
            f[CLR1] = ((v >> 17) & 1, (v >> 18) & 7, 0)
            f[TEX0] = ((v >> 21) & 1, (v >> 22) & 7, (v >> 25) & 31)
        elif 0x80 <= addr < 0x88:
            f = st.vat[addr - 0x80]
            name = "vatB fmt%d" % (addr - 0x80)
            f[TEX0 + 1] = (v & 1, (v >> 1) & 7, (v >> 4) & 31)
            f[TEX0 + 2] = ((v >> 9) & 1, (v >> 10) & 7, (v >> 13) & 31)
            f[TEX0 + 3] = ((v >> 18) & 1, (v >> 19) & 7, (v >> 22) & 31)
            f[TEX0 + 4] = ((v >> 27) & 1, (v >> 28) & 7, f[TEX0 + 4][2])
        elif 0x90 <= addr < 0x98:
            f = st.vat[addr - 0x90]
            name = "vatC fmt%d" % (addr - 0x90)
            f[TEX0 + 4] = (f[TEX0 + 4][0], f[TEX0 + 4][1], v & 31)
            f[TEX0 + 5] = ((v >> 5) & 1, (v >> 6) & 7, (v >> 9) & 31)
            f[TEX0 + 6] = ((v >> 14) & 1, (v >> 15) & 7, (v >> 18) & 31)
            f[TEX0 + 7] = ((v >> 23) & 1, (v >> 24) & 7, (v >> 27) & 31)
        elif 0xA0 <= addr < 0xB0:
            name = "arrayBase %s" % ATTRS[POS + addr - 0xA0] if addr - 0xA0 < 12 else "arrayBase %d" % (addr - 0xA0)
        elif 0xB0 <= addr < 0xC0:
            i = POS + addr - 0xB0
            st.strides[i] = v & 0xFF
            name = "arrayStride %s" % (ATTRS[i] if i < 21 else "array%d" % i)
        self.emit("cp", "CP  %-24s %08x" % (name, v))

    def draw(self, off, cmd, count, data, how=""):
        st = self.st
        prim, fmt = cmd & 0xF8, cmd & 7
        self.draws += 1
        self.verts += count
        self.emit("draw", "DRAW #%d %s fmt%d %d vertices%s  [%s]" %
                  (self.draws, PRIMS.get(prim, "prim%02x" % prim), fmt, count, how, st.describe(fmt)), draw=not self.a.verts)
        for a, kind, _ in st.layout(fmt):
            if kind != "direct" and a >= POS and a not in st.arrays:
                pass   # set in an earlier frame: nothing to say
            elif kind != "direct" and a >= POS and st.arrays[a][0] == 0:
                self.problem("draw #%d reads %s by index but the array's address is 0" % (self.draws, ATTRS[a]))
        if self.a.verts and not self.a.summary:
            lay = st.layout(fmt)
            at = 0
            rows = []
            for _ in range(count):
                cols = []
                for a, kind, size in lay:
                    b = data[at:at + size]
                    at += size
                    if kind != "direct" or a < POS:
                        cols.append("%s[%s]" % (ATTRS[a], ",".join(str(int.from_bytes(b[i:i + len(b) // max(1, (3 if len(b) in (3, 6) and a == NRM else 1))], "big"))
                                                                    for i in range(0, len(b), len(b) // max(1, (3 if len(b) in (3, 6) and a == NRM else 1))))))
                    elif a in (CLR0, CLR1):
                        cols.append("%s #%s" % (ATTRS[a], b.hex()))
                    else:
                        cnt, typ, shift = st.vat[fmt][a]
                        cs = st.comp(a, fmt)[0]
                        vals = []
                        for i in range(0, len(b), cs):
                            if typ == 4:
                                vals.append("%g" % struct.unpack(">f", b[i:i + 4])[0])
                            else:
                                x = int.from_bytes(b[i:i + cs], "big", signed=typ in (1, 3))
                                vals.append("%g" % (x / (1 << shift)) if shift else str(x))
                        cols.append("%s(%s)" % (ATTRS[a], ", ".join(vals)))
                rows.append("    " + "  ".join(cols))
            self.emit("verts", "\n".join(rows), draw=True)

    def stream(self, d):
        st = self.st
        at = 0
        n = len(d)

        def take(k):
            nonlocal at
            if at + k > n:
                raise Stop("the stream ends inside a command (at byte %d, %d more wanted)" % (at, at + k - n))
            b = d[at:at + k]
            at += k
            return b

        def u(k):
            return int.from_bytes(take(k), "big")

        def f32():
            return struct.unpack(">f", take(4))[0]

        while at < n:
            start = at
            cmd = u(1)
            op = cmd & 0xF8
            if cmd == 0:
                continue
            if cmd == 0x61:
                v = u(4)
                reg = v >> 24
                self.emit("bp", "BP  %-24s %06x" % (BP.get(reg) or "bp%02x" % reg, v & 0xFFFFFF))
            elif op == 0x08:
                addr = u(1)
                self.cp(addr, u(4))
            elif op == 0x10:
                head = u(4)
                count, addr = (head >> 16) + 1, head & 0xFFFF
                vals = [u(4) for _ in range(count)]
                if addr < 0x1000 or addr in XF_FLOAT:
                    text = " ".join("%g" % struct.unpack(">f", struct.pack(">I", v))[0] for v in vals)
                else:
                    text = " ".join("%08x" % v for v in vals)
                names = xf_place(addr) if count == 1 or addr < 0x1000 else "%s.. (%d)" % (xf_place(addr), count)
                self.emit("xf", "XF  %-24s %s" % (names, text))
            elif op in (0x20, 0x28, 0x30, 0x38):
                idx, al = u(2), u(2)
                which = (op - 0x20) // 8
                attr = 21 + which       # GX_POS_MTX_ARRAY and the three after it
                name = ("posMtxArray", "nrmMtxArray", "texMtxArray", "lightArray")[which]
                self.emit("indx", "XF  %-24s from %s[%d], %d words" % (xf_place(al & 0xFFF), name, idx, (al >> 12) + 1))
                if attr in st.arrays and st.arrays[attr][0] == 0:
                    self.problem("indexed load from %s, whose address is 0 (\"unmapped array %d\")" % (name, attr))
                elif attr not in st.arrays:
                    self.emit("hint", "    (%s was not set in this frame)" % name)
            elif op == 0x40:
                take(8)
                self.emit("other", "CALL_DL (nested: ignored by the library)")
            elif op == 0x48:
                self.emit("other", "INVALIDATE vertex cache")
            elif cmd == 0x50:
                sub = u(2)
                name = AURORA.get(sub)
                if 0x10 <= sub <= 0x1F:
                    attr = POS + sub - 0x10
                    addr, size, le = u(8), u(4), u(1)
                    st.arrays[attr] = (addr, size, le)
                    label = ATTRS[attr] if attr < 21 else ("posMtxArray", "nrmMtxArray", "texMtxArray", "lightArray")[attr - 21] if attr < 25 else "array%d" % attr
                    self.emit("aurora", "AUR array %-18s %s size %d %s" % (label, self.ptr(addr), size, "little-endian" if le else "big-endian"))
                    if addr == 0:
                        self.emit("hint", "    (address 0)")
                elif sub == 0x0001:
                    self.emit("aurora", "AUR viewportRender         " + " ".join("%g" % f32() for _ in range(6)))
                elif sub in (0x0002, 0x0035, 0x003B):
                    self.emit("aurora", "AUR %-22s %s" % (name, " ".join(str(struct.unpack(">i", take(4))[0]) for _ in range(4))))
                elif sub == 0x0003:
                    self.emit("aurora", "AUR projectionFull         " + " ".join("%g" % f32() for _ in range(16)))
                elif sub in (0x0020, 0x0022):
                    self.emit("aurora", "AUR %-22s %s" % (name, take(u(2)).decode("utf-8", "replace")))
                elif sub in (0x0021, 0x003A):
                    self.emit("aurora", "AUR " + name)
                elif sub == 0x0030:
                    tid, data, w, h, fmt, tlut, flag, oid, ver = u(1), u(8), u(4), u(4), u(4), u(4), u(1), u(4), u(4)
                    self.emit("aurora", "AUR loadTexObj map%d %s %dx%d %s tlut %d object %d version %d" %
                              (tid, self.ptr(data), w, h, TEXFMT.get(fmt, "fmt%d" % fmt), tlut, oid, ver))
                elif sub == 0x0038:
                    tid, data = u(1), u(8)
                    self.emit("aurora", "AUR setTexImgPtr map%d %s" % (tid, self.ptr(data)))
                elif sub == 0x0031:
                    idx, data, fmt, entries, oid, ver = u(1), u(8), u(4), u(2), u(4), u(4)
                    self.emit("aurora", "AUR loadTlut slot%d %s format %d entries %d object %d version %d" % (idx, self.ptr(data), fmt, entries, oid, ver))
                elif sub in (0x0032, 0x0033):
                    self.emit("aurora", "AUR %-22s object %d" % (name, u(4)))
                elif sub in (0x0034, 0x0037):
                    self.emit("aurora", "AUR %-22s %s" % (name, self.ptr(u(8))))
                elif sub == 0x0036:
                    w, h, fmt, mip = u(4), u(4), u(4), u(1)
                    self.emit("aurora", "AUR copyDst                %dx%d %s mipmap %d" % (w, h, TEXFMT.get(fmt, "fmt%d" % fmt), mip))
                elif sub == 0x0039:
                    self.emit("aurora", "AUR beginOffscreen         %dx%d" % (u(4), u(4)))
                elif sub == 0x0042:
                    self.emit("aurora", "AUR drawsBlocking          %d" % u(1))
                elif sub == 0x1000:
                    self.emit("aurora", "AUR polygonOffset          " + " ".join("%g" % f32() for _ in range(5)))
                elif sub == 0x0052:
                    flags, x, y, w, h, color, z = u(1), u(4), u(4), u(4), u(4), take(4), u(4)
                    self.emit("aurora", "AUR clear flags %02x rect %d,%d %dx%d colour #%s z %06x" % (flags, x, y, w, h, color.hex(), z))
                elif sub == 0x0050:
                    u(8), u(8)
                    count = u(4)
                    u(1)
                    take(count * 13)
                    self.emit("aurora", "AUR readbackPeeks          %d points" % count)
                elif sub == 0x0051:
                    u(8), u(8)
                    count = u(4)
                    take(count * 16)
                    self.emit("aurora", "AUR readbackCopies         %d" % count)
                elif sub == 0x0040:
                    c, nbytes = u(1), u(4)
                    size = sum(x[2] for x in st.layout(c & 7))
                    if nbytes and (size == 0 or nbytes % size):
                        raise Stop("drawSized: %d bytes is not a whole number of %d-byte vertices" % (nbytes, size))
                    self.draw(start, c, nbytes // size if size else 0, take(nbytes), " (sized)")
                elif sub == 0x0041:
                    c, vcount, icount = u(1), u(2), u(4)
                    take(icount * 2)
                    size = sum(x[2] for x in st.layout(c & 7))
                    self.draw(start, c, vcount, take(vcount * size), " (%d indices, from a display list)" % icount)
                else:
                    raise Stop("unknown Aurora command %04x at byte %d" % (sub, start))
            elif cmd >= 0x80:
                count = u(2)
                size = sum(x[2] for x in st.layout(cmd & 7))
                self.draw(start, cmd, count, take(count * size))
            else:
                raise Stop("unknown command %02x at byte %d" % (cmd, start))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--verts", action="store_true")
    ap.add_argument("--draw", type=int, default=0)
    ap.add_argument("--prev", action="store_true")
    a = ap.parse_args()
    import signal
    if hasattr(signal, "SIGPIPE"):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    path, namer = a.path, None
    if os.path.isdir(path):
        folder = path
        path = os.path.join(folder, "gx_fifo_prev.bin.gz" if a.prev else "gx_fifo.bin.gz")
        if os.path.exists(os.path.join(folder, "memory.txt")):
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import dump_inspect
            namer = dump_inspect.Dump(folder).name
    with (gzip.open(path, "rb") if path.endswith(".gz") else open(path, "rb")) as f:
        d = f.read()
    if d[:5] != b"MKGX1":
        sys.exit("%s is not a draw command record" % path)
    out = Listing(a, namer)
    at, total = 8, 0
    while at + 8 <= len(d):
        kind, size = struct.unpack("<II", d[at:at + 8])
        p = d[at + 8:at + 8 + size]
        at += 8 + size
        if kind == 1:
            out.note(p.decode("utf-8", "replace"))
        elif kind == 2:
            out.state(p)
        elif kind == 0:
            total += size
            try:
                out.stream(p)
            except Stop as e:
                out.problem("%s; the rest of this piece (%d bytes) is not listed" % (e, size))
    if a.summary or a.draw == 0:
        c = out.counts
        print("\n%d bytes of commands: %d draws (%d vertices), %d BP, %d CP, %d XF, %d indexed XF loads, %d Aurora, %d notes" %
              (total, out.draws, out.verts, c.get("bp", 0), c.get("cp", 0), c.get("xf", 0), c.get("indx", 0), c.get("aurora", 0), c.get("note", 0)))
    for p in out.problems:
        print("problem: " + p)


if __name__ == "__main__":
    main()
