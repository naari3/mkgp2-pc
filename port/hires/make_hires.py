#!/usr/bin/env python3
"""Makes the high-resolution 2D texture folder of the PC port from the user's
own disc image (docs/port/hires_textures.md).

    python3 port/hires/make_hires.py --iso <disc image> --out <folder>

Every texture palette file (*.tpl) on the disc is decoded and written back
enlarged as PNG, named the way Dolphin names custom textures
(tex1_<w>x<h>_<texel hash>[_<palette hash>]_<format>.png), which is what the
rendering library looks for when the game loads a texture
(port/gfx/gx_shim.cpp, MKGP2PC_HIRES=1).

  - Sprites, HUD and menu pictures: the steps that the texture formats' few
    colour levels left in gradients are smoothed out (never by more than half
    a level, so nothing that was in the picture is lost), then the picture is
    enlarged with a ringing-free Lanczos filter and its edges are tightened.
  - The text fonts (tpl2_sysFONT_16/24_*: one small texture per character):
    kana and kanji are drawn anew from a rounded gothic outline font, each
    placed and sized after the original bitmap. Latin letters, digits and
    signs are of another design in the game: they keep the original shape,
    enlarged with a tight edge.

The output is derived from the game's data: it stays on the user's machine
and is never committed.

Needs Python 3 with numpy and Pillow (with FreeType); the xxhash module makes
it faster. The outline font is M PLUS Rounded 1c (SIL Open Font License),
downloaded on first use unless --font names a file.
"""
import argparse
import hashlib
import math
import os
import struct
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image, ImageDraw, ImageFont

try:
    import xxhash

    def xxh64(data):
        return xxhash.xxh64_intdigest(data)
except ImportError:  # slow, but the result is the same
    _M = (1 << 64) - 1
    _P1, _P2, _P3, _P4, _P5 = (0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9,
                               0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5)

    def _rotl(x, r):
        return ((x << r) | (x >> (64 - r))) & _M

    def _round(acc, lane):
        return (_rotl((acc + lane * _P2) & _M, 31) * _P1) & _M

    def xxh64(data):
        n = len(data)
        p = 0
        if n >= 32:
            v = [(_P1 + _P2) & _M, _P2, 0, (-_P1) & _M]
            for lanes in struct.iter_unpack("<4Q", data[:n - n % 32]):
                v = [_round(v[i], lanes[i]) for i in range(4)]
            p = n - n % 32
            h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & _M
            for x in v:
                h = ((h ^ _round(0, x)) * _P1 + _P4) & _M
        else:
            h = _P5
        h = (h + n) & _M
        while p + 8 <= n:
            h = (_rotl(h ^ _round(0, struct.unpack_from("<Q", data, p)[0]), 27) * _P1 + _P4) & _M
            p += 8
        if p + 4 <= n:
            h = (_rotl(h ^ (struct.unpack_from("<I", data, p)[0] * _P1 & _M), 23) * _P2 + _P3) & _M
            p += 4
        while p < n:
            h = (_rotl(h ^ (data[p] * _P5 & _M), 11) * _P1) & _M
            p += 1
        h ^= h >> 33
        h = (h * _P2) & _M
        h ^= h >> 29
        h = (h * _P3) & _M
        return h ^ (h >> 32)

FONT_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/mplusrounded1c/MPLUSRounded1c-ExtraBold.ttf"
GAME_ID = b"GGPJ02"  # the disc header of Mario Kart Arcade GP 2 (Japan); its main.dol is GNLJ82
# main.dol of GNLJ82 (config/GNLJ82/symbols.txt)
ADDR_PATHS, N_PATHS = 0x80350508, 4804       # kResourcePathTable
ADDR_RESOURCES, N_RESOURCES = 0x80422208, 11008  # kResourceTableMain, 0x28 bytes each
ADDR_GLYPHS, N_GLYPHS = 0x804E5590, 739      # lbl_804E5590, 16 bytes each


# --------------------------------------------------------------- disc image

class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(0x440)
        if head[:6] != GAME_ID:
            raise SystemExit("%s: not a %s disc image (id %r)" % (path, GAME_ID.decode(), head[:6]))
        dol, fst, fst_size = struct.unpack_from(">III", head, 0x420)
        self.f.seek(fst)
        t = self.f.read(fst_size)
        n = struct.unpack_from(">I", t, 8)[0]
        names = t[n * 12:]
        self.files = {}
        dirs = []
        for i in range(1, n):
            while dirs and i >= dirs[-1][0]:
                dirs.pop()
            word, off, size = struct.unpack_from(">III", t, i * 12)
            name = names[word & 0xFFFFFF:].split(b"\0", 1)[0].decode("shift_jis", "replace")
            if word >> 24:
                dirs.append((size, name))
            else:
                self.files["/".join([d[1] for d in dirs] + [name])] = (off, size)
        self.f.seek(dol)
        h = self.f.read(0x100)
        offs = struct.unpack_from(">18I", h, 0)
        addrs = struct.unpack_from(">18I", h, 0x48)
        sizes = struct.unpack_from(">18I", h, 0x90)
        self.sections = []
        for o, a, s in zip(offs, addrs, sizes):
            if s:
                self.f.seek(dol + o)
                self.sections.append((a, self.f.read(s)))

    def read(self, name):
        off, size = self.files[name]
        self.f.seek(off)
        return self.f.read(size)

    def mem(self, addr, size):
        for a, d in self.sections:
            if a <= addr and addr + size <= a + len(d):
                return d[addr - a:addr - a + size]
        raise KeyError(hex(addr))

    def _cstr(self, addr):
        for a, d in self.sections:
            if a <= addr < a + len(d):
                return d[addr - a:d.index(b"\0", addr - a)].decode("ascii", "replace")
        raise KeyError(hex(addr))


def glyph_table(disc):
    """{file name (lower case): (character, cell width, cell height, size class)}
    for the main texture and the outline texture ('_a') of every character of
    the 16 and 24 pixel fonts."""
    paths = struct.unpack(">%dI" % N_PATHS, disc.mem(ADDR_PATHS, N_PATHS * 4))
    res = disc.mem(ADDR_RESOURCES, N_RESOURCES * 0x28)
    raw = disc.mem(ADDR_GLYPHS, N_GLYPHS * 16)
    out = {}
    for i in range(N_GLYPHS):
        _, code, r16, r24, _ = struct.unpack_from(">HHIII", raw, i * 16)
        try:
            ch = (bytes([code >> 8, code & 255]) if code > 255 else bytes([code])).decode("cp932")
        except UnicodeDecodeError:
            ch = None
        for cls, r in ((16, r16), (24, r24)):
            sx, sy = struct.unpack_from(">ff", res, r * 0x28 + 0x0C)
            pi = struct.unpack_from(">h", res, r * 0x28 + 0x16)[0]
            name = disc._cstr(paths[pi]).lower()
            if "sysfont_%d_" % cls not in name:
                continue
            # the half-width entries of the table name the same textures
            out.setdefault(name, (ch, int(sx), int(sy), cls, False))
            out.setdefault(name[:-4] + "_a.tpl", (ch, int(sx), int(sy), cls, True))
    return out


# ---------------------------------------------------------- texture palettes

def tpl_bytes(data):
    """A *.tpl of this game is either plain or 8 bytes followed by a zlib stream."""
    if data[:4] != b"\x00\x20\xaf\x30":
        try:
            data = zlib.decompress(data[8:])
        except zlib.error:
            return None
        if data[:4] != b"\x00\x20\xaf\x30":
            return None
    return data


def tpl_entries(d):
    count, table = struct.unpack_from(">II", d, 4)
    out = []
    for i in range(count):
        image, clut = struct.unpack_from(">II", d, table + i * 8)
        if image == 0:
            continue
        h, w, fmt, off = struct.unpack_from(">HHII", d, image)
        pal = None
        if clut:
            n, _, pfmt, poff = struct.unpack_from(">HHII", d, clut)
            pal = (n, pfmt, poff)
        out.append(dict(w=w, h=h, fmt=fmt, off=off, pal=pal))
    return out


BLOCK = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16), 6: (4, 4, 32),
         8: (8, 8, 4), 9: (8, 4, 8), 10: (4, 4, 16), 14: (8, 8, 4)}


def tex_size(w, h, fmt):
    bw, bh, bpp = BLOCK[fmt]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bpp // 8


def _rgb5a3(v):
    v = v.astype(np.uint32)
    o = np.zeros(v.shape + (4,), np.uint8)
    opaque = (v & 0x8000) != 0
    o[..., 0] = np.where(opaque, ((v >> 10) & 31) * 255 // 31, ((v >> 8) & 15) * 17)
    o[..., 1] = np.where(opaque, ((v >> 5) & 31) * 255 // 31, ((v >> 4) & 15) * 17)
    o[..., 2] = np.where(opaque, (v & 31) * 255 // 31, (v & 15) * 17)
    o[..., 3] = np.where(opaque, 255, ((v >> 12) & 7) * 255 // 7)
    return o


def _rgb565(v):
    v = v.astype(np.uint32)
    o = np.zeros(v.shape + (4,), np.uint8)
    o[..., 0] = ((v >> 11) & 31) * 255 // 31
    o[..., 1] = ((v >> 5) & 63) * 255 // 63
    o[..., 2] = (v & 31) * 255 // 31
    o[..., 3] = 255
    return o


def _untile(a, w, h, bw, bh):
    cols, rows = (w + bw - 1) // bw, (h + bh - 1) // bh
    a = a.reshape((rows, cols, bh, bw) + a.shape[3:]).swapaxes(1, 2)
    return a.reshape((rows * bh, cols * bw) + a.shape[4:])[:h, :w]


def tex_decode(d, e):
    """RGBA8 picture as the console shows the texture, and the per-channel
    size of one colour level of the format (what smoothing may undo)."""
    w, h, fmt, off = e["w"], e["h"], e["fmt"], e["off"]
    raw = np.frombuffer(d, np.uint8, tex_size(w, h, fmt), off)

    def palette():
        n, pfmt, poff = e["pal"]
        v = np.frombuffer(d, ">u2", n, poff)
        if pfmt == 0:
            o = np.zeros((n, 4), np.uint8)
            o[:, 0:3] = (v & 255)[:, None]
            o[:, 3] = v >> 8
            return o, (0, 0, 0, 0)
        return (_rgb565(v), (8, 4, 8, 0)) if pfmt == 1 else (_rgb5a3(v), None)

    def indexed(img):
        p, step = palette()
        px = p[np.minimum(img, len(p) - 1)]
        return px, step if step is not None else _step5a3(px)

    if fmt in (0, 8):
        img = _untile(np.stack([raw >> 4, raw & 15], -1).reshape(-1, 8, 8), w, h, 8, 8)
        if fmt == 8:
            return indexed(img)
        v = (img * 17).astype(np.uint8)
        return np.stack([v, v, v, v], -1), (17, 17, 17, 17)
    if fmt in (1, 9):
        img = _untile(raw.reshape(-1, 4, 8), w, h, 8, 4)
        if fmt == 9:
            return indexed(img)
        return np.stack([img, img, img, img], -1), (0, 0, 0, 0)
    if fmt == 2:
        img = _untile(raw.reshape(-1, 4, 8), w, h, 8, 4)
        i, a = (img & 15) * 17, (img >> 4) * 17
        return np.stack([i, i, i, a], -1).astype(np.uint8), (17, 17, 17, 17)
    if fmt in (3, 4, 5, 10):
        img = _untile(raw.view(">u2").reshape(-1, 4, 4), w, h, 4, 4)
        if fmt == 3:
            i, a = (img & 255).astype(np.uint8), (img >> 8).astype(np.uint8)
            return np.stack([i, i, i, a], -1), (0, 0, 0, 0)
        if fmt == 4:
            return _rgb565(img), (8, 4, 8, 0)
        if fmt == 5:
            px = _rgb5a3(img)
            return px, _step5a3(px)
        return indexed(img & 0x3FFF)
    if fmt == 6:
        b = raw.reshape(-1, 2, 16, 2)
        px = np.stack([b[:, 0, :, 1], b[:, 1, :, 0], b[:, 1, :, 1], b[:, 0, :, 0]], -1).reshape(-1, 4, 4, 4)
        return _untile(px, w, h, 4, 4), (0, 0, 0, 0)
    if fmt == 14:
        b = raw.reshape(-1, 8)
        c0 = b[:, 0:2].copy().view(">u2")[:, 0]
        c1 = b[:, 2:4].copy().view(">u2")[:, 0]
        p0, p1 = _rgb565(c0).astype(np.int32), _rgb565(c1).astype(np.int32)
        gt = (c0 > c1)[:, None]
        p2 = np.where(gt, (2 * p0 + p1) // 3, (p0 + p1) // 2)
        p3 = np.where(gt, (2 * p1 + p0) // 3, 0)
        p3[:, 3] = np.where(gt[:, 0], 255, 0)
        pal = np.stack([p0, p1, p2, p3], 1).astype(np.uint8)
        bits = b[:, 4:8]
        idx = np.stack([(bits >> 6) & 3, (bits >> 4) & 3, (bits >> 2) & 3, bits & 3], -1).reshape(-1, 4, 4)
        px = pal[np.arange(len(pal))[:, None, None], idx]
        px = px.reshape(-1, 2, 2, 4, 4, 4).swapaxes(2, 3).reshape(-1, 8, 8, 4)
        # two end colours and two in between per 4x4 block: what was lost is
        # not a fixed step; a small allowance takes the edge off the blocks
        return _untile(px, w, h, 8, 8), (6, 4, 6, 0)
    raise ValueError("texture format %d" % fmt)


def _step5a3(px):
    """RGB5A3: 5 bits a channel where opaque, 4 bits and 3 of alpha elsewhere."""
    step = np.empty(px.shape, np.float32)
    opaque = px[..., 3] == 255
    step[..., 0:3] = np.where(opaque, 8.0, 17.0)[..., None]
    step[..., 3] = np.where(opaque, 0.0, 36.0)
    return step


def tex_key(d, e):
    """File name stem of the replacement: Dolphin's convention, as the
    rendering library reads it (aurora lib/gfx/texture_replacement.cpp)."""
    w, h, fmt, off = e["w"], e["h"], e["fmt"], e["off"]
    size = tex_size(w, h, fmt)
    if off + size > len(d):
        return None
    texels = d[off:off + size]
    name = "tex1_%dx%d_%016x" % (w, h, xxh64(texels))
    if fmt in (8, 9, 10):
        if e["pal"] is None:
            return None
        n, _, poff = e["pal"]
        if fmt == 8:
            a = np.frombuffer(texels, np.uint8)
            lo, hi = int(min((a >> 4).min(), (a & 15).min())), int(max((a >> 4).max(), (a & 15).max()))
        elif fmt == 9:
            a = np.frombuffer(texels, np.uint8)
            lo, hi = int(a.min()), int(a.max())
        else:
            a = np.frombuffer(texels, ">u2") & 0x3FFF
            lo, hi = int(a.min()), int(a.max())
        if hi >= n:
            return None
        name += "_%016x" % xxh64(d[poff + 2 * lo:poff + 2 * (hi + 1)])
    return name + "_%d" % fmt


# ------------------------------------------------------------------ filters

def _blur(a, radius):
    """Gaussian blur of a float image (one plane), edges repeated."""
    n = max(1, int(math.ceil(radius * 3)))
    k = np.exp(-0.5 * (np.arange(-n, n + 1) / radius) ** 2).astype(np.float32)
    k /= k.sum()
    for axis in (0, 1):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (n, n)
        p = np.pad(a, pad, mode="edge")
        size = a.shape[axis]
        acc = np.zeros(a.shape, np.float32)
        for i, w in enumerate(k):
            acc += w * (p[i:i + size] if axis == 0 else p[:, i:i + size])
        a = acc
    return a


def _resize(a, size, how):
    return np.asarray(Image.fromarray(a, "F").resize(size, how), np.float32)


def _local_range(a):
    """Minimum and maximum over each texel's 3x3 neighbourhood."""
    p = np.pad(a, 1, mode="edge")
    h, w = a.shape
    lo = hi = None
    for dy in range(3):
        for dx in range(3):
            s = p[dy:dy + h, dx:dx + w]
            lo = s if lo is None else np.minimum(lo, s)
            hi = s if hi is None else np.maximum(hi, s)
    return lo, hi


def smooth_levels(px, step):
    """Takes out the steps of coarse colour levels: each channel moves toward
    its neighbourhood's average, never by more than half a level, so an edge
    that is taller than one level stays where it is."""
    out = px.astype(np.float32)
    weight = (px[..., 3] > 0).astype(np.float32)
    for c in range(4):
        tol = (step[..., c] if isinstance(step, np.ndarray) else np.float32(step[c])) * 0.5
        if not np.any(tol > 0):
            continue
        orig = out[..., c].copy()
        cur = orig
        for radius in (1.0, 2.0):
            if c < 3:  # colour under no alpha is not a colour: leave it out
                den = _blur(weight, radius)
                avg = np.where(den > 1e-3, _blur(cur * weight, radius) / np.maximum(den, 1e-3), cur)
            else:
                avg = _blur(cur, radius)
            cur = np.clip(avg, orig - tol, orig + tol)
        out[..., c] = cur
    return out


def fill_unknown(px):
    """Texels that are fully transparent and black carry no colour. Give them
    the colour of what is near, so that enlarging does not draw a dark rim
    into the edge of every shape."""
    unknown = (px[..., 3] == 0) & (px[..., 0:3].max(-1) == 0)
    if not unknown.any() or unknown.all():
        return px
    out = px.copy()
    known = (~unknown).astype(np.float32)
    todo = unknown.copy()
    for radius in (1.0, 2.0, 4.0, 8.0, 16.0):
        den = _blur(known, radius)
        ok = todo & (den > 0.02)
        if ok.any():
            for c in range(3):
                num = _blur(px[..., c] * known, radius)
                out[..., c][ok] = (num / np.maximum(den, 1e-6))[ok]
            todo &= ~ok
        if not todo.any():
            break
    return out


def enlarge(px, scale, sharpen=0.5):
    """Lanczos enlargement of an RGBA float picture, kept between the
    smallest and largest value of the source neighbourhood (no ringing), and
    with edges tightened: inside a strong edge the value is pushed toward the
    nearer side."""
    h, w = px.shape[:2]
    size = (w * scale, h * scale)
    out = np.empty((h * scale, w * scale, 4), np.float32)
    for c in range(4):
        a = np.ascontiguousarray(px[..., c], np.float32)
        if a.min() == a.max():
            out[..., c] = a.flat[0]
            continue
        up = _resize(a, size, Image.LANCZOS)
        lo, hi = _local_range(a)
        lo = _resize(lo, size, Image.BILINEAR)
        hi = _resize(hi, size, Image.BILINEAR)
        up = np.clip(up, lo, hi)
        if sharpen > 0:
            span = hi - lo
            t = (up - lo) / np.maximum(span, 1e-3)
            steep = t * t * (3 - 2 * t)
            # only where the neighbourhood really is an edge, not a gradient
            k = sharpen * np.clip((span - 24.0) / 64.0, 0.0, 1.0)
            up = lo + span * (t + (steep - t) * k)
        out[..., c] = up
    return out


def enlarge_sprite(px, step, scale, fmt):
    # Block-compressed and 16-bit colour textures are mostly rendered
    # pictures: tightening edges there brings the compression blocks out.
    sharpen = {14: 0.0, 4: 0.2}.get(fmt, 0.5)
    return enlarge(fill_unknown(smooth_levels(px, step)), scale, sharpen)


def halve(px):
    h, w = px.shape[:2]
    return px.reshape(h // 2, 2, w // 2, 2, -1).mean((1, 3))


def to_u8(px):
    return np.clip(np.rint(px), 0, 255).astype(np.uint8)


# -------------------------------------------------------------------- fonts

FONT_SCALE = 4   # the characters are small: always four times, with mip levels
SS = 4           # the drawing is made at SS times the output and averaged down


class Typeface:
    """Draws a character into a cell so that it sits where the original
    bitmap's character sits."""

    def __init__(self, path):
        self.path = path
        self.fonts = {}
        self.missing = None

    def font(self, px):
        px = int(round(px))
        if px not in self.fonts:
            self.fonts[px] = ImageFont.truetype(self.path, px)
        return self.fonts[px]

    def _small(self, ch):
        im = Image.new("L", (96, 96), 0)
        ImageDraw.Draw(im).text((16, 8), ch, font=self.font(64), fill=255)
        return np.asarray(im)

    def has(self, ch):
        """Whether the font has ch (its drawing is not the missing-glyph box)."""
        if self.missing is None:
            self.missing = self._small("\uFFFF")
        a = self._small(ch)
        return bool(a.any()) and not np.array_equal(a, self.missing)

    def ink(self, ch, px):
        """Ink box of ch drawn at (0, 0) with a font of px pixels."""
        im = Image.new("L", (px * 3, px * 3), 0)
        ImageDraw.Draw(im).text((px, px), ch, font=self.font(px), fill=255)
        box = im.getbbox()
        return (box[0] - px, box[1] - px, box[2] - px, box[3] - px)

    def ink_size(self, ch, px=256):
        box = self.ink(ch, px)
        return (box[2] - box[0]) / px, (box[3] - box[1]) / px

    def draw(self, ch, em, w, h, cx, cy, scale):
        """Coverage (0..1) of ch in a w x h cell enlarged by scale, its ink
        box centred on (cx, cy) (cell texels)."""
        k = scale * SS
        px = int(round(em * k))
        box = self.ink(ch, px)
        im = Image.new("L", (w * k, h * k), 0)
        x = cx * k - (box[0] + box[2]) / 2
        y = cy * k - (box[1] + box[3]) / 2
        ImageDraw.Draw(im).text((x, y), ch, font=self.font(px), fill=255)
        a = np.asarray(im, np.float32) / 255
        return a.reshape(h * scale, SS, w * scale, SS).mean((1, 3))


def ink_box(cov, level=0.5):
    """Ink box of a coverage picture (0..1) with sub-texel edges: (x0, y0,
    x1, y1), or None for an empty cell."""
    cols, rows = cov.max(0), cov.max(1)

    def edges(v):
        idx = np.nonzero(v >= level)[0]
        if len(idx) == 0:
            return None
        a, b = int(idx[0]), int(idx[-1])
        # where the profile crosses the level, between texel centres
        lo = a + 0.5 - ((v[a] - level) / max(v[a] - v[a - 1], 1e-3) if a > 0 and v[a - 1] < level else 0.5)
        hi = b + 0.5 + ((v[b] - level) / max(v[b] - v[b + 1], 1e-3) if b + 1 < len(v) and v[b + 1] < level else 0.5)
        return lo, hi
    ex, ey = edges(cols), edges(rows)
    if ex is None or ey is None:
        return None
    return ex[0], ey[0], ex[1], ey[1]


def is_japanese(ch):
    """Kana and kanji are drawn from the outline font, whose design is close
    to the game's. The Latin letters, digits and signs of the game's fonts
    are of another, heavier design: those keep their own shapes."""
    return ch is not None and ("\u3041" <= ch <= "\u30FA" or "\u4E00" <= ch <= "\u9FFF")


def area_down(a, scale):
    h, w = a.shape
    return a.reshape(h // scale, scale, w // scale, scale).mean((1, 3))


def likeness(a, b):
    """Overlap of two coverage pictures, 1 = the same shape."""
    return float(2 * np.minimum(a, b).sum() / max(a.sum() + b.sum(), 1e-6))


def outline_of(cov, radius):
    """The outline texture of a character: its shape grown by radius texels
    of the enlarged picture, with a soft edge."""
    sigma = max(radius, 0.5)
    b = _blur(np.ascontiguousarray(cov * 255, np.float32), sigma) / 255
    level = 0.1587  # a straight edge moves out by one sigma
    ramp = 0.242 / sigma
    return np.clip((b - level) / ramp + 0.5, 0.0, 1.0)


def enlarge_coverage(cov, scale):
    """A character without a usable outline drawing: the original bitmap
    enlarged, with the soft edge made as narrow as the new texels."""
    h, w = cov.shape
    up = _resize(np.ascontiguousarray(cov, np.float32), (w * scale, h * scale), Image.BICUBIC)
    up = _blur(up, scale * 0.3)
    return np.clip((up - 0.5) * 2.2 + 0.5, 0.0, 1.0)


# --------------------------------------------------------------------- work

def write_png(path, px):
    Image.fromarray(px, "RGBA").save(path, compress_level=4)


def write_with_mips(out_dir, stem, px, levels):
    write_png(os.path.join(out_dir, stem + ".png"), to_u8(px))
    for level in range(1, levels + 1):
        px = halve(px)
        write_png(os.path.join(out_dir, "%s_mip%d.png" % (stem, level), ), to_u8(px))


def do_sprite(job):
    data, entry, stem, out_dir, scale, export_dir, import_dir = job
    px, step = tex_decode(data, entry)
    if export_dir:
        # for an outside enlarger: the colour as an opaque picture, and the
        # alpha (when it is not the same everywhere) as a grey one
        clean = to_u8(fill_unknown(smooth_levels(px, step)))
        Image.fromarray(clean[..., 0:3], "RGB").save(os.path.join(export_dir, "rgb", stem + ".png"), compress_level=1)
        if clean[..., 3].min() != clean[..., 3].max():
            Image.fromarray(clean[..., 3], "L").convert("RGB").save(
                os.path.join(export_dir, "alpha", stem + ".png"), compress_level=1)
        return entry["w"] * entry["h"]
    big = None
    if import_dir:
        big = import_sprite(import_dir, stem, px, scale)
    if big is None:
        big = enlarge_sprite(px, step, scale, entry["fmt"])
    write_with_mips(out_dir, stem, big, int(math.log2(scale)) - 1)
    return entry["w"] * entry["h"]


def import_sprite(import_dir, stem, px, scale):
    """The picture an outside enlarger made of an exported texture, brought
    to scale times the texture's size; None when there is none (the built-in
    filter is used then)."""
    h, w = px.shape[:2]
    size = (w * scale, h * scale)

    def load(kind):
        path = os.path.join(import_dir, kind, stem + ".png")
        if not os.path.exists(path):
            return None
        im = Image.open(path).convert("RGB")
        if im.width < w or im.width * h != im.height * w:
            print("%s: %dx%d does not fit a %dx%d texture, ignored" % (path, im.width, im.height, w, h))
            return None
        if im.size != size:
            im = im.resize(size, Image.BOX if im.width % size[0] == 0 else Image.LANCZOS)
        return np.asarray(im, np.float32)

    rgb = load("rgb")
    if rgb is None:
        return None
    out = np.empty((size[1], size[0], 4), np.float32)
    out[..., 0:3] = rgb
    if px[..., 3].min() == px[..., 3].max():
        out[..., 3] = px[0, 0, 3]
    else:
        alpha = load("alpha")
        if alpha is None:
            return None
        out[..., 3] = alpha.mean(-1)
    return out


_typeface = None


def do_glyph(job):
    """One character: (main texture, outline texture). Returns how it was made."""
    global _typeface
    (ch, cw, chh, cls), main, outline, out_dir, font_path, em, grow = job
    if _typeface is None and font_path:
        _typeface = Typeface(font_path)
    d, e, stem = main
    px, _ = tex_decode(d, e)
    cov = px[..., 0].astype(np.float32) / 255  # the main texture's intensity is the shape
    h, w = cov.shape
    box = ink_box(cov[:chh, :cw])
    drawn = None
    if _typeface is not None and box is not None and em and is_japanese(ch) and _typeface.has(ch):
        cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
        drawn = _typeface.draw(ch, em, w, h, cx, cy, FONT_SCALE)
        if likeness(area_down(drawn, FONT_SCALE), cov) < 0.35:  # not this character after all
            drawn = None
    how = "drawn" if drawn is not None else "enlarged"
    if drawn is None:
        drawn = enlarge_coverage(cov, FONT_SCALE)
    v = drawn * 255
    full = np.full_like(v, 255)
    write_with_mips(out_dir, stem, np.stack([v, v, v, full], -1), 2)
    if outline is not None:
        d, e, stem = outline
        if how == "drawn":
            o = outline_of(drawn, grow * FONT_SCALE) * 255
            write_with_mips(out_dir, stem, np.stack([o, o, o, o], -1), 2)
        else:
            px, _ = tex_decode(d, e)
            oi = enlarge_coverage(px[..., 0].astype(np.float32) / 255, FONT_SCALE) * 255
            oa = enlarge_coverage(px[..., 3].astype(np.float32) / 255, FONT_SCALE) * 255
            write_with_mips(out_dir, stem, np.stack([oi, oi, oi, oa], -1), 2)
    return how


def fit_typeface(face, glyphs):
    """Per size class: the font size (in original texels), from the median
    ratio of the kanji's original ink boxes to the drawings'; and how far the
    outline textures reach beyond the shape, as the reach whose outlines
    carry as much ink as the original ones."""
    em, grow = {}, {}
    for cls in (16, 24):
        ratios = []
        sample = [g for g in glyphs if g[0][3] == cls and g[0][0] and "\u4e00" <= g[0][0] <= "\u9fff"][::3]
        boxes = []
        for (ch, cw, chh, _), main, outline in sample:
            if not face.has(ch):
                continue
            px, _ = tex_decode(main[0], main[1])
            box = ink_box(px[:chh, :cw, 0].astype(np.float32) / 255)
            if box is None:
                continue
            iw, ih = face.ink_size(ch)
            ratios.append(((box[2] - box[0]) / iw + (box[3] - box[1]) / ih) / 2)
            boxes.append((ch, px.shape[1], px.shape[0], box, outline))
        if not ratios:
            continue
        em[cls] = float(np.median(ratios))
        reaches = np.arange(0.75, 3.01, 0.25)
        ink = np.zeros(len(reaches))
        target = 0.0
        for ch, w, h, box, outline in boxes[::4]:
            if outline is None:
                continue
            opx, _ = tex_decode(outline[0], outline[1])
            target += float(opx[..., 3].sum()) / 255
            drawn = face.draw(ch, em[cls], w, h, (box[0] + box[2]) / 2, (box[1] + box[3]) / 2, FONT_SCALE)
            for i, r in enumerate(reaches):
                ink[i] += float(outline_of(drawn, r * FONT_SCALE).sum()) / FONT_SCALE ** 2
        grow[cls] = float(reaches[int(np.argmin(np.abs(ink - target)))]) if target else 1.0
    return em, grow


def find_font(path, cache_dir):
    if path:
        return path if os.path.exists(path) else None
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "MPLUSRounded1c-ExtraBold.ttf")
    cached = os.path.join(cache_dir, "MPLUSRounded1c-ExtraBold.ttf")
    for p in (here, cached):
        if os.path.exists(p):
            return p
    try:
        import urllib.request
        os.makedirs(cache_dir, exist_ok=True)
        print("downloading the outline font (M PLUS Rounded 1c, SIL Open Font License)")
        with urllib.request.urlopen(FONT_URL, timeout=60) as r:
            data = r.read()
        with open(cached + ".part", "wb") as f:
            f.write(data)
        os.replace(cached + ".part", cached)
        return cached
    except Exception as ex:  # no network: the fonts are enlarged bitmaps then
        print("could not download the outline font (%s)" % ex)
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso", required=True, help="disc image of Mario Kart Arcade GP 2 (Japan)")
    ap.add_argument("--out", default="hires", help="folder to write (MKGP2PC_HIRES_DIR, 'hires' next to the executable)")
    ap.add_argument("--scale", type=int, default=2, choices=(2, 4), help="enlargement of sprites (default 2)")
    ap.add_argument("--font", help="outline font file for the text fonts (default: M PLUS Rounded 1c ExtraBold)")
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "mkgp2-pc"),
                    help="where the downloaded outline font is kept")
    ap.add_argument("--no-font", action="store_true", help="enlarge the original font bitmaps instead of drawing anew")
    ap.add_argument("--export-sprites", metavar="DIR",
                    help="only write the sprite textures' source pictures to DIR/rgb and DIR/alpha, for an outside enlarger")
    ap.add_argument("--import-sprites", metavar="DIR",
                    help="take the sprites from DIR/rgb and DIR/alpha (the exported pictures, enlarged 2, 3 or 4 times "
                         "under the same names); textures without a picture there get the built-in filter")
    ap.add_argument("--only", help="only files whose name contains this text (testing)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = ap.parse_args()

    started = time.time()
    disc = Disc(args.iso)
    if not args.export_sprites:
        os.makedirs(args.out, exist_ok=True)
    glyph_of = glyph_table(disc)

    seen = set()
    sprites, glyph_main, glyph_outline = [], {}, {}
    skipped = 0
    for name in sorted(disc.files):
        base = name.rsplit("/", 1)[-1].lower()
        if not base.endswith(".tpl") or (args.only and args.only.lower() not in base):
            continue
        d = tpl_bytes(disc.read(name))
        if d is None:
            skipped += 1
            continue
        for e in tpl_entries(d):
            if e["fmt"] not in BLOCK or e["w"] == 0 or e["h"] == 0:
                skipped += 1
                continue
            stem = tex_key(d, e)
            if stem is None:
                skipped += 1
                continue
            if stem in seen:
                continue
            seen.add(stem)
            g = glyph_of.get(base)
            if g is not None:
                key = base[:-6] + ".tpl" if g[4] else base
                (glyph_outline if g[4] else glyph_main)[key] = (g, (d, e, stem))
            else:
                sprites.append((d, e, stem, args.out, args.scale, args.export_sprites, args.import_sprites))

    glyphs = []
    for key, (g, main_tex) in glyph_main.items():
        outline = glyph_outline.pop(key, None)
        glyphs.append((g[:4], main_tex, outline[1] if outline else None))
    # an outline texture whose main texture is the same picture as another
    # character's (dropped above as a duplicate) is enlarged like a sprite
    for g, tex in glyph_outline.values():
        sprites.append(tex + (args.out, FONT_SCALE, None, None))

    if args.export_sprites:
        for kind in ("rgb", "alpha"):
            os.makedirs(os.path.join(args.export_sprites, kind), exist_ok=True)
        with ProcessPoolExecutor(args.jobs) as pool:
            list(pool.map(do_sprite, sprites, chunksize=4))
        print("%d sprite textures exported to %s" % (len(sprites), args.export_sprites))
        return

    font_path = None if args.no_font else find_font(args.font, args.cache)
    em, grow = {}, {}
    if font_path:
        em, grow = fit_typeface(Typeface(font_path), glyphs)
        print("outline font %s: size %s, outline reach %s (original texels)" % (
            os.path.basename(font_path), {k: round(v, 2) for k, v in em.items()}, {k: round(v, 2) for k, v in grow.items()}))
    elif not args.no_font:
        print("no outline font: the text fonts are enlarged bitmaps")

    glyph_jobs = [(g, m, o, args.out, font_path, em.get(g[3]), grow.get(g[3], 1.0)) for g, m, o in glyphs]
    print("%d sprite textures (x%d), %d characters (x%d), %d files skipped" % (
        len(sprites), args.scale, len(glyph_jobs), FONT_SCALE, skipped))
    sprites.sort(key=lambda j: -j[1]["w"] * j[1]["h"])
    made = {"drawn": 0, "enlarged": 0}
    with ProcessPoolExecutor(args.jobs) as pool:
        for how in pool.map(do_glyph, glyph_jobs, chunksize=16):
            made[how] += 1
        total = sum(j[1]["w"] * j[1]["h"] for j in sprites)
        done = 0
        last = time.time()
        for n in pool.map(do_sprite, sprites, chunksize=4):
            done += n
            if time.time() - last > 10:
                last = time.time()
                print("  %3d %%" % (100 * done // max(total, 1)), flush=True)
    print("characters: %d drawn from the outline font, %d enlarged from the bitmap" % (made["drawn"], made["enlarged"]))
    print("done in %.0f s: %s" % (time.time() - started, args.out))


if __name__ == "__main__":
    main()
