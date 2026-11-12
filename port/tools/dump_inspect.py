#!/usr/bin/env python3
"""Reads a dump folder of the PC port (docs/port/dumps.md) on any machine.

A dump folder (F9, or crash_<time> when the program died) carries the game's
memory and the names of everything in the executable that wrote it, so no
executable and no debugger are needed:

    dump_inspect.py <folder>                      what is in it
    dump_inspect.py <folder> stacks [--all]       the threads (frame chain; --all: every line)
    dump_inspect.py <folder> sym <regex>          symbols by name
    dump_inspect.py <folder> addr <address>...    what an address is
    dump_inspect.py <folder> read <where> [n]     bytes, as a hex dump
    dump_inspect.py <folder> words <where> [n]    32-bit words; pointers are named
    dump_inspect.py <folder> floats <where> [n]   32-bit floats
    dump_inspect.py <folder> str <where>          a zero-terminated string
    dump_inspect.py <folder> find <hex|text>      search the memory (--text for a string)
    dump_inspect.py <folder> save <region> <file> one region, unpacked

<where> is a hexadecimal address, a symbol, or symbol+offset (hexadecimal).
Values are read in the host's byte order (little-endian); --be reads
big-endian ones, as in data that came from the disc unconverted.
"""
import argparse
import bisect
import gzip
import os
import re
import signal
import struct
import sys


class Dump:
    def __init__(self, folder):
        self.folder = folder
        self.regions = []   # (name, addr, size, file, offset)
        self.code = []      # (name, addr, size)
        self.modules = []   # (addr, size, name)
        self.exe = "?"
        self._files = {}
        self._syms = None
        path = os.path.join(folder, "memory.txt")
        if os.path.exists(path):
            for line in open(path, encoding="utf-8", errors="replace"):
                p = line.split()
                if not p or p[0].startswith("#"):
                    continue
                if p[0] == "exe":
                    self.exe = " ".join(p[1:])
                elif p[0] == "region":
                    self.regions.append((p[1], int(p[2], 16), int(p[3], 16), p[4], int(p[5])))
                elif p[0] == "code":
                    self.code.append((p[1], int(p[2], 16), int(p[3], 16)))
                elif p[0] == "module":
                    self.modules.append((int(p[1], 16), int(p[2], 16), " ".join(p[3:])))

    def _open(self, name):
        path = os.path.join(self.folder, name)
        return gzip.open(path, "rb") if name.endswith(".gz") else open(path, "rb")

    def file(self, name):
        if name not in self._files:
            with self._open(name) as f:
                self._files[name] = f.read()
        return self._files[name]

    @property
    def syms(self):
        if self._syms is None:
            self._syms = []
            for name in ("symbols.txt.gz", "symbols.txt"):
                if os.path.exists(os.path.join(self.folder, name)):
                    with self._open(name) as f:
                        for line in f.read().decode("utf-8", "replace").splitlines():
                            p = line.split(" ", 2)
                            if len(p) == 3:
                                self._syms.append((int(p[0], 16), p[1], p[2]))
                    break
            self._addrs = [s[0] for s in self._syms]
        return self._syms

    def region_of(self, addr):
        for r in self.regions:
            if r[1] <= addr < r[1] + r[2]:
                return r
        return None

    def read(self, addr, size):
        """Bytes at an address; None where the dump has none."""
        out = b""
        while size > 0:
            r = self.region_of(addr)
            if r is None:
                return out if out else None
            n = min(size, r[1] + r[2] - addr)
            at = r[4] + addr - r[1]
            out += self.file(r[3])[at:at + n]
            addr += n
            size -= n
        return out

    def name(self, addr):
        """symbol+offset, or the region, or the module; '' when unknown."""
        syms = self.syms
        inside = self.region_of(addr)
        code = next((c for c in self.code if c[1] <= addr < c[1] + c[2]), None)
        if (inside is not None and inside[0] != "mem1") or code is not None:
            i = bisect.bisect_right(self._addrs, addr) - 1
            if i >= 0:
                s = syms[i]
                return s[2] if s[0] == addr else "%s+0x%x" % (s[2], addr - s[0])
        if inside is not None:
            return "%s+0x%x" % (inside[0], addr - inside[1])
        for m in self.modules:
            if m[0] <= addr < m[0] + m[1]:
                return "%s+0x%x" % (m[2], addr - m[0])
        return ""

    def where(self, text):
        m = re.fullmatch(r"(0x)?([0-9a-fA-F]{5,16})", text)
        if m:
            return int(m.group(2), 16)
        base, _, off = text.partition("+")
        for s in self.syms:
            if s[2] == base:
                return s[0] + (int(off, 16) if off else 0)
        sys.exit("no symbol named %s (try: sym %s)" % (base, base))


def cmd_summary(d, a):
    print("executable:", d.exe)
    for r in d.regions:
        print("  region %-22s %08x  %8d bytes  (%s)" % (r[0], r[1], r[2], r[3]))
    for c in d.code:
        print("  code   %-22s %08x  %8d bytes  (not in the dump: it is the executable's)" % c)
    print("  %d symbols, %d modules" % (len(d.syms), len(d.modules)))
    for name in sorted(os.listdir(d.folder)):
        print("  file %-18s %9d bytes" % (name, os.path.getsize(os.path.join(d.folder, name))))
    if os.path.exists(os.path.join(d.folder, "stacks.txt")):
        print()
        cmd_stacks(d, argparse.Namespace(all=False))


def cmd_stacks(d, a):
    for line in open(os.path.join(d.folder, "stacks.txt"), encoding="utf-8", errors="replace"):
        if a.all or not line.startswith("  ? "):
            print(line.rstrip("\n"))


def cmd_sym(d, a):
    rx = re.compile(a.regex)
    for s in d.syms:
        if rx.search(s[2]):
            print("%08x %s %s" % s)


def cmd_addr(d, a):
    for t in a.address:
        v = d.where(t)
        print("%08x  %s" % (v, d.name(v) or "(nothing known there)"))


def cmd_read(d, a):
    addr = d.where(a.where)
    data = d.read(addr, a.count)
    if data is None:
        sys.exit("%08x is not in the dump" % addr)
    print("%08x  %s" % (addr, d.name(addr)))
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        print("%08x  %-47s  %s" % (addr + i, row.hex(" "), "".join(chr(b) if 32 <= b < 127 else "." for b in row)))


def cmd_words(d, a):
    addr = d.where(a.where)
    data = d.read(addr, a.count * 4)
    if data is None:
        sys.exit("%08x is not in the dump" % addr)
    for i in range(0, len(data) - 3, 4):
        v = struct.unpack(">I" if a.be else "<I", data[i:i + 4])[0]
        label = d.name(addr + i)
        print("%08x  %08x  %-36s %s" % (addr + i, v, d.name(v) if v >= 0x10000 else "", label if "+" not in label or i == 0 else ""))


def cmd_floats(d, a):
    addr = d.where(a.where)
    data = d.read(addr, a.count * 4)
    if data is None:
        sys.exit("%08x is not in the dump" % addr)
    for i in range(0, len(data) - 3, 4):
        print("%08x  %g" % (addr + i, struct.unpack(">f" if a.be else "<f", data[i:i + 4])[0]))


def cmd_str(d, a):
    addr = d.where(a.where)
    data = d.read(addr, 1024) or b""
    print(repr(data.split(b"\0")[0].decode("shift_jis", "replace")))


def cmd_find(d, a):
    needle = a.what.encode("shift_jis") if a.text else bytes.fromhex(a.what)
    shown = 0
    for r in d.regions:
        data = d.file(r[3])[r[4]:r[4] + r[2]]
        at = data.find(needle)
        while at >= 0 and shown < a.limit:
            print("%08x  %s" % (r[1] + at, d.name(r[1] + at)))
            shown += 1
            at = data.find(needle, at + 1)
    if shown == a.limit:
        print("(stopped after %d; --limit for more)" % a.limit)


def cmd_save(d, a):
    for r in d.regions:
        if r[0] == a.region:
            open(a.file, "wb").write(d.file(r[3])[r[4]:r[4] + r[2]])
            print("%s: %d bytes from %08x" % (a.file, r[2], r[1]))
            return
    sys.exit("no region named %s" % a.region)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("stacks"); p.add_argument("--all", action="store_true"); p.set_defaults(fn=cmd_stacks)
    p = sub.add_parser("sym"); p.add_argument("regex"); p.set_defaults(fn=cmd_sym)
    p = sub.add_parser("addr"); p.add_argument("address", nargs="+"); p.set_defaults(fn=cmd_addr)
    p = sub.add_parser("read"); p.add_argument("where"); p.add_argument("count", nargs="?", type=lambda x: int(x, 0), default=256); p.set_defaults(fn=cmd_read)
    for name, fn in (("words", cmd_words), ("floats", cmd_floats)):
        p = sub.add_parser(name); p.add_argument("where"); p.add_argument("count", nargs="?", type=lambda x: int(x, 0), default=32)
        p.add_argument("--be", action="store_true"); p.set_defaults(fn=fn)
    p = sub.add_parser("str"); p.add_argument("where"); p.set_defaults(fn=cmd_str)
    p = sub.add_parser("find"); p.add_argument("what"); p.add_argument("--text", action="store_true")
    p.add_argument("--limit", type=int, default=50); p.set_defaults(fn=cmd_find)
    p = sub.add_parser("save"); p.add_argument("region"); p.add_argument("file"); p.set_defaults(fn=cmd_save)
    a = ap.parse_args()
    if hasattr(signal, "SIGPIPE"):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)   # `| head`
    d = Dump(a.folder)
    (a.fn if a.cmd else cmd_summary)(d, a)


if __name__ == "__main__":
    main()
