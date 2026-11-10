#!/usr/bin/env python3
"""Compile the decompiled sources to host objects and count what a link needs.

Second measurement after compile_census.py. Every file the host compiler
accepts is compiled to an object (32-bit little-endian, -O0, nothing is
linked or run). From the objects' symbol tables it reports

  * symbols defined more than once (per-TU copies that the matching build
    keeps apart with local scope or section tricks),
  * symbols the chosen directories use but do not define, grouped by the
    part of the original program that owns them (config/GNLJ82/symbols.txt
    and splits.txt), which is the list of SDK entry points the PC build has
    to get from Aurora, port/sdk or port/platform.

    python3 port/tools/link_census.py                 # game, sound, ui, util, resource
    python3 port/tools/link_census.py --list dolphin/gx   # undefined symbols owned by src/dolphin/gx
"""
import argparse
import glob
import collections
import concurrent.futures
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compile_census as cc  # noqa: E402
import mwcc_cxx_abi
import mwcc_byvalue
import mwcc_data_layout  # noqa: E402
import mwcc_int_div  # noqa: E402
import mwcc_struct_return  # noqa: E402
import mwcc_fifo  # noqa: E402

ROOT = cc.ROOT
OUT = os.path.join(ROOT, "build", "pc-census")
DEFAULT_DIRS = ["game", "sound", "ui", "util", "resource"]
# Decompiled files the PC build does not compile because port/ replaces them
# function for function.
REPLACED = {
    "src/triforce/amexi.c",  # port/platform/triforce/baseboard.c
}


DATA_OBJECTS = mwcc_data_layout.original_objects(ROOT)
# --os windows: 32-bit Windows objects (COFF, MinGW runtime). Set in main().
WIN = False
WIN_TARGET = "i686-w64-windows-gnu"
# --gfx: objects for the rendering executable, which CMake links with Aurora
# (port/gfx/CMakeLists.txt). Set in main().
GFX = False
# port/nogfx files that port/gfx replaces
GFX_REPLACED = ("video_min.c", "gx_min.c")
# GX functions whose objects the game sizes differently from Aurora; the
# link sends the game's calls to port/gfx/gx_shim.cpp (ld --wrap).
GFX_WRAPPED = ("GXInitTexObj", "GXInitTexObjCI", "GXInitTexObjLOD", "GXLoadTexObj",
               "GXInitTlutObj", "GXLoadTlut", "GXSetArray", "GXInvalidateTexAll")


def asm_name(name):
    """A name that begins with L is an assembler-local label in COFF
    assembly text. Generated assembly defines such a symbol under a longer
    name, which win_plain_names turns back in the object."""
    return "mkgp2pc_asmL_" + name if WIN and name.startswith("L") else name


def win_plain_names(obj, asm_only=False):
    """COFF on x86 puts an underscore before every C name. Take it off again
    in the object, so that everything after this point (rename tables, linker
    scripts, generated assembly) works on the same names as the ELF build.
    Names the Windows libraries own keep theirs; the link step bridges the
    few the game uses (see win_host_bridges)."""
    out = subprocess.run(["llvm-nm", "--format=posix", obj], capture_output=True, text=True).stdout
    names = sorted({l.split()[0] for l in out.splitlines() if l.startswith("_") and len(l.split()) >= 2})
    names = [n for n in names if not n.startswith(("__imp_", "___chkstk", "__alloca", "___main"))]
    if asm_only:
        names = []
    asm = sorted({l.split()[0] for l in out.splitlines() if l.startswith("mkgp2pc_asmL_")})
    if not names and not asm:
        return
    with open(obj + ".plain", "w") as f:
        f.writelines(f"{n} {n[1:]}\n" for n in names)
        f.writelines(f"{n} {n[len('mkgp2pc_asmL_'):]}\n" for n in asm)
    subprocess.run(["llvm-objcopy", "--redefine-syms=" + obj + ".plain", obj], check=True)


def link_gfx_windows(stubs, thunks, cxxrt, replaced, aurora_fns, aurora_data, port_objs, objs, defsyms, script, bss_ld, game_defined):
    """Objects and link inputs of the rendering executable for 32-bit
    Windows. The game's and the port's objects have plain names; Aurora's
    libraries and everything CMake compiles have the COFF underscore. The
    linker's --wrap is not used: the game reaches a wrapped function through
    a jump to the glue's __wrap_ function, and __real_ is a jump to Aurora's."""
    wrapped = set(GFX_WRAPPED)
    data = sorted(n for n in replaced if n in aurora_data)
    # the glue, compiled here so that its names can be matched to both sides
    r = subprocess.run(["llvm-nm", "-g", "--defined-only", "--format=posix"] + port_objs, capture_output=True, text=True)
    plain = game_defined | {l.split()[0] for l in r.stdout.splitlines() if len(l.split()) >= 2}
    glue_objs, glue_def = [], set()
    # SDL's headers (Aurora's event API shows them): where the Aurora build has them
    sdl_inc = []
    cache = os.path.join(ROOT, "build", "aurora-win32", "CMakeCache.txt")
    for line in open(cache) if os.path.exists(cache) else []:
        m = re.match(r"(SDL_SOURCE_DIR|sdl_SOURCE_DIR|FETCHCONTENT_SOURCE_DIR_SDL)\w*:\w+=(.+)", line)
        if m and os.path.isdir(os.path.join(m.group(2), "include")):
            sdl_inc = [os.path.join(m.group(2), "include")]
    aurora = os.path.join(ROOT, "port", "extern", "aurora")
    for src in sorted(glob.glob(os.path.join(ROOT, "port", "gfx", "*.c*"))):
        if os.path.basename(src).startswith("dawn_"):
            continue   # needs Dawn's headers: CMake compiles it
        o = os.path.join(OUT, "gfx__" + os.path.basename(src) + ".o")
        cxx = src.endswith(".cpp")
        r = subprocess.run(["clang++" if cxx else "clang", "-target", WIN_TARGET, "-O2", "-g", "-msse2", "-c", src, "-o", o,
                            "-DAURORA", "-DAURORA_ENABLE_GX", "-DTARGET_PC=1", "-I" + os.path.join(aurora, "include")]
                           + ["-I" + d for d in sdl_inc]
                           + (["-std=gnu++20", "-femulated-tls"] if cxx else []), capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[:3000])
            return 1
        out = subprocess.run(["llvm-nm", "-g", "--format=posix", o], capture_output=True, text=True).stdout
        with open(o + ".plain", "w") as f:
            for line in out.splitlines():
                q = line.split()
                if len(q) < 2 or not q[0].startswith("_"):
                    continue
                n = q[0][1:]
                if n.startswith(("__wrap_", "__real_")) or n.startswith("_Z"):
                    continue
                if q[1] != "U":
                    glue_def.add(n)
                if q[1] != "U" or n in plain:
                    f.write(f"{q[0]} {n}\n")
        subprocess.run(["llvm-objcopy", "--redefine-syms=" + o + ".plain", o], check=True)
        glue_objs.append(o)
    with open(stubs, "a") as f:
        def jump(name, to):
            f.write(f'__asm__(".text\\n.globl {name}\\n{name}: jmp {to}\\n");\n')
        for n in cxxrt:
            jump(n, "_" + n)
        for n in sorted(n for n in replaced if n in aurora_fns and n not in wrapped and n not in glue_def):
            jump(n, "_" + n)
        for n in sorted(wrapped):
            jump(n, "___wrap_" + n)
            jump("___real_" + n, "_" + n)
        jump("_main", "___wrap_main")
        jump("___real_main", "main")
    gen = list(glue_objs)
    for src in (stubs, thunks):
        o = src[:-2] + ".o"
        r = subprocess.run(["clang", "-target", WIN_TARGET, "-O0", "-g", "-fcommon", "-c", src, "-o", o], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[:2000])
            return 1
        win_plain_names(o, asm_only=True)
        gen.append(o)
    link = os.path.join(OUT, "gfx_link.rsp")
    with open(link, "w") as f:
        f.write("\n".join(["-static", "-fcommon", "-Wl,--large-address-aware", "-Wl,-T," + script, "-Wl,-T," + bss_ld]
                          + gen + port_objs + objs + defsyms + [f"-Wl,--defsym,{n}=_{n}" for n in data]
                          + ["-lwinpthread", "-Wl,--allow-multiple-definition"]) + "\n")
    print(f"link inputs for CMake: {os.path.relpath(link, ROOT)}")
    return 0


def win_bss_sizes(objs):
    """COFF symbols carry no size. A common symbol's value is its size; an
    object in .bss reaches up to the next symbol of that section."""
    sizes = {}

    def one(obj):
        out = subprocess.run(["i686-w64-mingw32-objdump", "-h", "-t", obj], capture_output=True, text=True).stdout
        bss, bss_size, syms, local = None, 0, [], {}
        for line in out.splitlines():
            m = re.match(r"\s*(\d+) \.bss\s+([0-9a-f]+) ", line)
            if m:
                bss, bss_size = int(m.group(1)) + 1, int(m.group(2), 16)
                continue
            m = re.match(r"\[\s*\d+\]\(sec\s*(-?\d+)\)\(fl 0x\w+\)\(ty\s+\w+\)\(scl\s+(\d+)\) \(nx \d+\) 0x([0-9a-f]+) (\S+)", line)
            if not m:
                continue
            sec, scl, val, name = int(m.group(1)), int(m.group(2)), int(m.group(3), 16), m.group(4)
            if sec == 0 and scl == 2 and val:
                local[name] = val
            elif bss is not None and sec == bss and name != ".bss":
                syms.append((val, name, scl))
        syms.sort()
        for i, (val, name, scl) in enumerate(syms):
            end = next((v for v, _, _ in syms[i + 1:] if v > val), bss_size)
            if scl == 2:
                local[name] = end - val
        return local
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        for local in ex.map(one, objs):
            for n, z in local.items():
                sizes[n] = max(sizes.get(n, 0), z)
    return sizes


_WIN_LINKAGE = re.compile(r"^((?:define|@[\w.$\"]+ =) [^\n]*?)\b(?:linkonce_odr|weak_odr|linkonce|weak) ", re.M)
_WIN_COMDAT = re.compile(r"^\$[^\n]* = comdat [a-z]+\n|,? comdat(?:\(\$[^)\n]*\))?(?=[ ,\n{])", re.M)


def win_ir(text):
    """Two differences of 32-bit Windows objects taken out again:

    Member functions take `this` in ecx there; the decompiled code calls many
    of them through plain C declarations, so every function uses the C
    convention.

    An inline function is a COMDAT section, and the linker keeps one such
    section per section name, whatever its symbol is called by then. The
    rename tables are per unit (the same inline destructor is `dtor_X` in
    one unit and keeps its C++ name in the next), so one copy would survive
    under one of the names only. They become ordinary definitions; the link
    takes the first of each name (--allow-multiple-definition), which is how
    the weak symbols of the ELF build behave."""
    text = text.replace(" x86_thiscallcc ", " ")
    text = _WIN_COMDAT.sub("", text)
    return _WIN_LINKAGE.sub(r"\1", text)


def compile_one(path, target):
    obj = os.path.join(OUT, path.replace(os.sep, "__") + ".o")
    # -fshort-wchar: the game's wchar_t has 16 bits (card names, panel
    # labels); with the host's 32 the structs that hold such text grow and
    # no longer fit the sizes other units allocate for them.
    # C-linkage inline functions (C++): MWCC emits an inline function whose
    # address the unit's exception tables take, and other units call those
    # by name (fn_80120A08 in SpriteHandle.cpp). A host compiler drops an
    # inline function its own unit does not use (clang has no
    # -fkeep-inline-functions), so the unit is compiled through a wrapper
    # that takes their addresses.
    cmd = ["clang", "-target", target, "-c", "-O0", "-g", "-fno-exceptions", "-fno-rtti", "-fcommon", "-fshort-wchar"] + (["-mno-ms-bitfields"] if WIN else ["-malign-double"]) + (["-DMKGP2PC_GFX"] if GFX else []) + cc.base_flags(path)[1:]
    src_in = path
    if path.endswith(".cpp"):
        keep = sorted(set(re.findall(r'^extern "C" inline [\w \*]+?\b(\w+)\(', open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read(), re.M)))
        if keep:
            wrap = obj[:-2] + ".keep.cpp"
            with open(wrap, "w") as f:
                f.write(f'#include "{os.path.join(ROOT, path)}"\n')
                for i, n in enumerate(keep):
                    f.write(f"static void *const mkgp2pc_keep_{i} __attribute__((used)) = (void *)&{n};\n")
            src_in = wrap
    cmd += ["-include", cc.COMPAT]
    # -Dinline=: in C, MWCC also emits an `inline` function as an ordinary
    # one where it is called without being inlined. C99 leaves the caller of
    # a plain `inline` function without a definition, GNU89 the caller of an
    # `extern inline` one, and the sources use both spellings.
    cmd += ["-x", "c++", "-std=gnu++98"] if cc.is_cxx(path, target) else ["-x", "c", "-std=gnu99", "-Dinline=", "-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int", "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types"]
    if cc.is_cxx(path, target):
        # C++: through LLVM IR, rewritten to MWCC's vtable conventions
        # (port/tools/mwcc_cxx_abi.py, docs/port/census.md section 13)
        ll = obj[:-2] + ".ll"
        # -fno-strict-return: a function declared with a result that runs off
        # its end returns whatever is in the register, as on the console; the
        # default in C++ is a trap instruction there (card_pack does this).
        irflags = ["-S", "-emit-llvm", "-fstandalone-debug", "-fno-discard-value-names", "-flto", "-fwhole-program-vtables",
                   "-fno-strict-return"]
        r = subprocess.run([c for c in cmd if c != "-c"] + irflags + [src_in, "-o", ll], cwd=ROOT, capture_output=True, text=True, errors="replace")
        if r.returncode != 0 and "with exceptions disabled" in r.stderr:
            # the few units that throw or catch (also through headers) are
            # compiled with exceptions on; the host C++ runtime unwinds
            cmd = ["-fexceptions" if c == "-fno-exceptions" else c for c in cmd]
            r = subprocess.run([c for c in cmd if c != "-c"] + irflags + [src_in, "-o", ll], cwd=ROOT, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            return path, None
        notes = []
        if os.path.exists(obj[:-2] + ".abi-notes"):
            os.remove(obj[:-2] + ".abi-notes")
        text, _ = mwcc_cxx_abi.rewrite(open(ll, encoding="utf-8", errors="surrogateescape").read(), notes.append)
        text, _ = mwcc_data_layout.rewrite(text, DATA_OBJECTS)
        text, _ = mwcc_int_div.rewrite(text)
        text, _ = mwcc_struct_return.rewrite(text)
        text, _ = mwcc_byvalue.rewrite(text)
        if WIN:
            text = win_ir(text)
        if GFX:
            text, _ = mwcc_fifo.rewrite(text)
        open(ll, "w", encoding="utf-8", errors="surrogateescape").write(text)
        if notes:
            with open(obj[:-2] + ".abi-notes", "w") as f:
                f.write("\n".join(sorted(set(notes))) + "\n")
        r = subprocess.run(["clang", "-target", target, "-c", "-Wno-override-module", ll, "-o", obj], cwd=ROOT, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            with open(obj[:-2] + ".abi-error", "w") as f:
                f.write(r.stderr)
            return path, None
        if WIN:
            win_plain_names(obj)
        return path, obj
    # C: through LLVM IR as well, for MWCC's data alignment
    # (port/tools/mwcc_data_layout.py)
    ll = obj[:-2] + ".ll"
    r = subprocess.run([c for c in cmd if c != "-c"] + ["-S", "-emit-llvm", path, "-o", ll], cwd=ROOT, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        return path, None
    text, n = mwcc_data_layout.rewrite(open(ll, encoding="utf-8", errors="surrogateescape").read(), DATA_OBJECTS)
    text, _ = mwcc_int_div.rewrite(text)
    text, _ = mwcc_struct_return.rewrite(text)
    text, _ = mwcc_byvalue.rewrite(text)
    if GFX:
        text, _ = mwcc_fifo.rewrite(text)
    open(ll, "w", encoding="utf-8", errors="surrogateescape").write(text)
    r = subprocess.run(["clang", "-target", target, "-c", "-Wno-override-module", ll, "-o", obj], cwd=ROOT, capture_output=True, text=True, errors="replace")
    if WIN and r.returncode == 0:
        win_plain_names(obj)
    return path, (obj if r.returncode == 0 else None)


def owners():
    """symbol name -> unit path (as in splits.txt), by address."""
    ranges = []  # (start, end, unit)
    unit = None
    for line in open(os.path.join(ROOT, "config", "GNLJ82", "splits.txt"), encoding="utf-8"):
        if line and not line[0].isspace() and line.rstrip().endswith(":") and line.strip() != "Sections:":
            unit = line.strip()[:-1]
            continue
        m = re.search(r"start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)", line)
        if m and unit:
            ranges.append((int(m.group(1), 16), int(m.group(2), 16), unit))
    ranges.sort()
    starts = [r[0] for r in ranges]
    import bisect
    table = {}
    for line in open(os.path.join(ROOT, "config", "GNLJ82", "symbols.txt"), encoding="utf-8"):
        m = re.match(r"(\S+) = \S+:0x([0-9A-Fa-f]+);", line)
        if not m:
            continue
        addr = int(m.group(2), 16)
        i = bisect.bisect_right(starts, addr) - 1
        table[m.group(1)] = ranges[i][2] if i >= 0 and addr < ranges[i][1] else "(no split: auto unit)"
    return table


def symbol_info():
    """symbol name -> (type, size) from symbols.txt."""
    info = {}
    for line in open(os.path.join(ROOT, "config", "GNLJ82", "symbols.txt"), encoding="utf-8"):
        m = re.match(r"(\S+) = \S+:0x[0-9A-Fa-f]+;(.*)", line)
        if m:
            t = re.search(r"type:(\w+)", m.group(2))
            z = re.search(r"size:0x([0-9A-Fa-f]+)", m.group(2))
            info[m.group(1)] = (t.group(1) if t else "", int(z.group(1), 16) if z else 0)
    return info


def write_stubs(missing, groups, path):
    """One C file that defines every missing symbol: functions log their first
    call through MKGP2PC_STUB-style bookkeeping and return 0, objects are
    zero-filled with the size symbols.txt gives."""
    info = symbol_info()
    host = set()
    # Anything the 32-bit host C library exports is taken from there (memset,
    # memcpy, ...), except the wide-character functions: wchar_t is 16 bits in
    # the game and 32 bits on Linux, so those need their own implementations.
    for lib in ("/usr/lib32/libc.so.6", "/usr/lib32/libm.so.6", "/usr/lib32/libz.so.1"):
        if os.path.exists(lib):
            r = subprocess.run(["llvm-nm", "-D", "--defined-only", "--format=posix", lib],
                               capture_output=True, text=True)
            for line in r.stdout.splitlines():
                n = line.split()[0].split("@")[0]
                if n in missing and not re.match(r"(wcs|wmem|sw|vsw|mb|wc)", n):
                    host.add(n)
    out = ["/* Generated by port/tools/link_census.py --link. Do not edit. */",
           # (every name is spelled with __asm__: the object then has it as written
           # here on COFF too, where a C name would get an underscore)
           'void mkgp2pc_stub_hit(int* seen, const char* func, const char* file, int line) __asm__("mkgp2pc_stub_hit");', ""]
    if WIN:
        # The same choice as on Linux (the names the 32-bit glibc has), as
        # far as the MinGW runtime has them too; those carry the COFF
        # underscore, which the game's objects no longer do.
        libdir = "/usr/i686-w64-mingw32/lib/"
        r = subprocess.run(["llvm-nm", "-g", "--defined-only", "--format=posix"] +
                           [libdir + n for n in ("libmsvcrt.a", "libmingwex.a", "libmingw32.a")],
                           capture_output=True, text=True)
        have = {l.split()[0][1:] for l in r.stdout.splitlines()
                if len(l.split()) >= 2 and l.startswith("_") and l.split()[1] == "T"}
        lost = sorted(host - have)
        host &= have
        if lost:
            print("in glibc but not in the MinGW runtime (stubbed): " + " ".join(lost))
        for name in sorted(host):
            out.append(f'__asm__(".text\\n.globl {name}\\n{name}: jmp _{name}\\n");')
    nfun = nobj = 0
    for i, name in enumerate(sorted(missing)):
        if name in host:
            continue
        kind, size = info.get(name, ("function", 0))
        if kind == "object":
            out.append(f'char mkgp2pc_obj_{i}[{max(size, 4)}] __asm__("{asm_name(name)}") __attribute__((aligned(8)));')
            nobj += 1
        else:
            out.append(f'int mkgp2pc_fn_{i}(void) __asm__("{asm_name(name)}");')
            out.append(f'int mkgp2pc_fn_{i}(void) {{ static int seen; mkgp2pc_stub_hit(&seen, "{name}", "generated", 0); return 0; }}')
            nfun += 1
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    return nfun, nobj


# A unit's file-local object that other units reach through an address-named
# extern (the matching build only needs the addresses to agree). On PC the
# static is made global under the extern's name. Hand-kept until a tool
# derives the pairs from the MWCC objects; see docs/port/upstream_requests.md.
STATIC_ALIASES = {
    # extern name: (defining file, name of the static there)
    "jvsio_bss_80683280": ("src/triforce/jvsio.c", "tx"),
    "jvsio_bss_8067E2A0": ("src/triforce/jvsio.c", "cmd"),
}


def expose_statics(built):
    n = 0
    for ext, (src, local) in STATIC_ALIASES.items():
        obj = built.get(src)
        if obj:
            subprocess.run(["i686-w64-mingw32-objcopy" if WIN else "llvm-objcopy", "--globalize-symbol=" + local,
                            "--redefine-sym", f"{local}={ext}", obj], check=True)
            n += 1
    return n


GAME_NEW_DELETE = {
    "_Znwj": "mkgp2pc_game_new", "_Znaj": "mkgp2pc_game_new_array",
    "_ZdlPv": "mkgp2pc_game_delete", "_ZdaPv": "mkgp2pc_game_delete_array",
}


def unsuffix(sym):
    """`Foo_p17` -> `Foo`, also inside Itanium names (`_Z7Foo_p17v` -> `_Z3Foov`).

    Merged translation units upstream keep clashing per-file declarations
    apart with a `_pN` suffix (tools/merge_tus.py); the matching build renames
    the symbols back in the object. The PC objects need the same."""
    if not re.search(r"_p\d+", sym):
        return sym
    if not sym.startswith("_Z"):
        return re.sub(r"_p\d+$", "", sym)
    out, i = [], 0
    while i < len(sym):
        m = re.match(r"\d+", sym[i:])
        if m:
            n = int(m.group())
            name = sym[i + len(m.group()):i + len(m.group()) + n]
            mm = re.match(r"^([A-Za-z_]\w*?)_p\d+$", name)
            if len(name) == n and mm:
                out.append(f"{len(mm.group(1))}{mm.group(1)}")
                i += len(m.group()) + n
                continue
            if len(name) == n and re.match(r"^[A-Za-z_]\w*$", name):
                out.append(m.group() + name)
                i += len(m.group()) + n
                continue
        out.append(sym[i])
        i += 1
    return "".join(out)


def unsuffix_objects(objs):
    """Rename `_pN` symbols in place; returns how many names changed."""
    names = set()
    for i in range(0, len(objs), 200):
        out = subprocess.run(["llvm-nm", "-g", "--format=posix"] + objs[i:i + 200],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            f = line.split()
            if len(f) >= 2 and "_p" in f[0]:
                names.add(f[0])
    table = {n: unsuffix(n) for n in names}
    table = {a: b for a, b in table.items() if a != b}
    renamed = len(table)
    # The game's global operator new / delete (its own allocator) get names
    # of their own. Under the C++ names they would also serve every C++
    # library in the executable (libstdc++, Aurora), before the game's heap
    # exists and with memory the game then frees behind their back.
    table.update(GAME_NEW_DELETE)
    path = os.path.join(OUT, "unsuffix.syms")
    with open(path, "w") as f:
        for a, b in sorted(table.items()):
            f.write(f"{a} {b}\n")

    def one(obj):
        subprocess.run(["llvm-objcopy", "--redefine-syms=" + path, obj], check=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        list(ex.map(one, objs))
    return renamed


def cxx_renames(built):
    """Apply the matching build's per-unit C++ symbol renames to the host objects.

    tools/extab_user_renames.json says, unit by unit, which C name a C++
    symbol really is (`__ct__11InputObjectFi` -> `InputObj_Ctor_Internal`).
    The host objects carry Itanium names, so both spellings are reduced to one
    comparable form (mwcc_names.py). Constructors and destructors do not map
    one to one (Itanium has C1/C2 and D0/D1/D2, MWCC one function with a hidden
    flag for destructors and a returned `this` for constructors), so they are
    renamed to intermediate names that cxx_thunks() bridges.
    Returns (symbols renamed, MWCC names with no counterpart in the object).
    """
    import json
    import mwcc_names
    path = os.path.join(ROOT, "tools", "extab_user_renames.json")
    if not os.path.exists(path):
        return 0, 0
    table = collections.defaultdict(dict)
    plain_table = collections.defaultdict(dict)
    for unit, entries in json.load(open(path), object_pairs_hook=lambda p: p):
        if not isinstance(entries, list):
            continue
        for k, v in entries:
            c = mwcc_names.canon_mwcc(k)
            if c and not v.startswith("@"):
                table[unit][c] = v
            elif not c and re.fullmatch(r"[A-Za-z]\w*", k) and "__" not in k and re.fullmatch(r"(global:)?[A-Za-z]\w*", v):
                # a C-linkage function or object the unit writes under a
                # descriptive name (`SceneListInsert` -> `fn_801F4DF0`)
                plain_table[unit][k] = v.split(":")[-1]
                table[unit]  # (the unit takes part even with no C++ entry)
    # Upstream writes a few constructors with ARGS_ORDER (the MWCC order in
    # the table key, the definition's order on the host), so a second lookup
    # ignores the order of the parameters.
    def loose(c):
        m = re.match(r"(.*)\((.*)\)(.*)$", c)
        return c if not m else "%s(%s)%s" % (m.group(1), ",".join(sorted(m.group(2).split(","))), m.group(3))
    table2 = {}
    for unit, t in table.items():
        seen = collections.Counter(loose(c) for c in t)
        table2[unit] = {loose(c): (c, v) for c, v in t.items() if seen[loose(c)] == 1}
    # The C written for a function that does not match yet may declare a
    # constructor's pointer parameters with other pointee types than the
    # matching side the table was made from (`Normal3D(void *, int)` for
    # `__ct__8Normal3DFPCci`). Third lookup: every pointer parameter alike.
    def ptrs(c):
        m = re.match(r"(.*)\((.*)\)(.*)$", c)
        if not m:
            return c
        ps = ["ptr" if x.rstrip("const").endswith(("*", "&")) else x for x in m.group(2).split(",")]
        return "%s(%s)%s" % (m.group(1), ",".join(ps), m.group(3))
    table3 = {}
    for unit, t in table.items():
        seen = collections.Counter(ptrs(c) for c in t)
        table3[unit] = {ptrs(c): (c, v) for c, v in t.items() if seen[ptrs(c)] == 1}
    jobs = [(u, built[u]) for u in sorted(set(table) | set(plain_table)) if built.get(u)]
    done = [0, 0]

    def one(job):
        unit, obj = job
        out = subprocess.run(["llvm-nm", "--format=posix", obj], capture_output=True, text=True).stdout
        syms = sorted({l.split()[0] for l in out.splitlines() if l.startswith("_Z")})
        plain_lines = [f"{k} {v}\n" for k, v in sorted(plain_table[unit].items())
                       if re.search(r"^%s [A-Za-z] " % re.escape(k), out, re.M) and not re.search(r"^%s [A-Za-z] " % re.escape(v), out, re.M)]
        if not syms:
            if plain_lines:
                open(obj + ".cxxsyms", "w").writelines(plain_lines)
                subprocess.run(["llvm-objcopy", "--redefine-syms=" + obj + ".cxxsyms", obj], check=True)
            return len(plain_lines), len(table[unit])
        dem = subprocess.run(["c++filt"], input="\n".join(syms) + "\n", capture_output=True, text=True).stdout.splitlines()
        lines, hit = [], set()
        # table entries that have their exact counterpart in this object
        table_hit = {mwcc_names.canon_itanium(d) for d in dem} & set(table[unit])
        for sym, d in zip(syms, dem):
            c = mwcc_names.canon_itanium(d)
            target = table[unit].get(c)
            if not target and c and loose(c) in table2[unit]:
                c, target = table2[unit][loose(c)]
            if not target and c and ptrs(c) in table3[unit] and table3[unit][ptrs(c)][0] not in table_hit:
                c, target = table3[unit][ptrs(c)]
            if not target:
                continue
            hit.add(c)
            m = re.search(r"([CD])([0-2])E", sym)
            if "::~" in d and m and m.group(1) == "D":
                target = f"mkgp2pc_dt{m.group(2)}__{target}"
            elif m and m.group(1) == "C" and not d.startswith("vtable"):
                head = d.split("(")[0].split("::")
                if len(head) >= 2 and head[-1] == head[-2]:
                    target = ("mkgp2pc_ct__" if m.group(2) == "1" else "mkgp2pc_ct2__") + target
            lines.append(f"{sym} {target}\n")
        # MWCC folds identical functions, so the table can give two classes'
        # functions one name (a derived class's empty destructor and its
        # base's: `__dt__13WarpInitValueFv` and `__dt__13WarpZoneValueFv`
        # are both dtor_800A9CC8). Here they are two functions, one calling
        # the other; under one name the call would reach itself. Only the
        # one that calls none of the others takes the name.
        groups = collections.defaultdict(lambda: collections.defaultdict(list))
        for line in lines:
            sym, target = line.split()
            groups[re.sub(r"^mkgp2pc_(ct2?|dt[012])__", "", target)][re.sub(r"[CD][0-2]E", "XE", sym)].append(line)
        shared = {t: g for t, g in groups.items() if len(g) > 1}
        if shared and os.path.exists(obj[:-2] + ".ll"):
            text = open(obj[:-2] + ".ll", encoding="utf-8", errors="replace").read()
            calls = {}
            for m in re.finditer(r"^define [^@]*@([\w.$]+)\(.*?^}", text, re.M | re.S):
                calls[m.group(1)] = set(re.findall(r"@([\w.$]+)\(", m.group(0))) - {m.group(1)}
            drop = set()
            for t, g in shared.items():
                # (references to functions defined elsewhere are not part of this)
                members = {k: {l.split()[0] for l in ls} & set(calls) for k, ls in g.items()}
                members = {k: v for k, v in members.items() if v}
                if len(members) < 2:
                    continue
                leaves = [k for k in members
                          if not any(calls.get(sym, set()) & other
                                     for sym in members[k] for k2, other in members.items() if k2 != k)]
                keep = leaves[0] if len(leaves) == 1 else sorted(members)[0]
                for k, ls in g.items():
                    if k != keep and k in members:
                        drop.update(ls)
            lines = [l for l in lines if l not in drop]
        lines += plain_lines
        if lines:
            f = obj + ".cxxsyms"
            open(f, "w").writelines(lines)
            subprocess.run(["llvm-objcopy", "--redefine-syms=" + f, obj], check=True)
        return len(lines), len(set(table[unit]) - hit)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        for a, b in ex.map(one, jobs):
            done[0] += a
            done[1] += b
    return done[0], done[1]


def cxx_thunks(defined, weak, undefined, path):
    """Bridges for the intermediate constructor / destructor names.

    MWCC: `T* ctor(T* this, ...)` returns this; `dtor(T* this, short flag)`
    with flag > 0 = destroy and delete, 0 = base part only, < 0 = destroy.
    Itanium: constructors return nothing, D0 deletes, D1 destroys, D2 is the
    base part. Returns (defsym list, names the thunks define, names they use).
    """
    have = lambda n: n in defined or n in weak
    names = collections.defaultdict(set)
    for n in list(defined) + list(weak) + list(undefined):
        m = re.match(r"mkgp2pc_(ct2?|dt[012])__(.+)$", n)
        if m:
            names[m.group(2)].add(m.group(1))
    out = ["/* generated by port/tools/link_census.py: C++ constructor / destructor bridges */\n"]
    defsyms, defs, uses = [], set(), set()
    for n in sorted(names):
        kinds = names[n]
        ct = [k for k in ("ct", "ct2") if have(f"mkgp2pc_{k}__{n}")]
        if kinds & {"ct", "ct2"}:
            if ct and not have(n):
                # copy 64 bytes of arguments, call, return `this`
                out.append(f'__asm__(".text\\n.globl {asm_name(n)}\\n{asm_name(n)}:\\n push %ebp\\n mov %esp,%ebp\\n push %esi\\n push %edi\\n'
                           f' sub $72,%esp\\n lea 8(%ebp),%esi\\n mov %esp,%edi\\n mov $16,%ecx\\n rep movsl\\n'
                           f' call mkgp2pc_{ct[0]}__{n}\\n mov 8(%ebp),%eax\\n add $72,%esp\\n pop %edi\\n pop %esi\\n pop %ebp\\n ret\\n");\n')
                defs.add(n)
            for k in ("ct", "ct2"):
                sym = f"mkgp2pc_{k}__{n}"
                if sym in undefined and not have(sym):
                    defsyms.append(f"-Wl,--defsym={sym}={'mkgp2pc_' + ct[0] + '__' + n if ct else n}")
                    defs.add(sym)
                    if not ct:
                        uses.add(n)
        dts = [k for k in ("dt0", "dt1", "dt2") if have(f"mkgp2pc_{k}__{n}")]
        if kinds & {"dt0", "dt1", "dt2"}:
            if dts:
                pick = lambda want: next(k for k in want if k in dts)
                for k in dts:
                    out.append(f'extern void mkgp2pc_{k}__{n}(void*) __asm__("mkgp2pc_{k}__{n}");\n')
                if not have(n):
                    out.append(f'void* {n}(void* self, short flag) __asm__("{asm_name(n)}");\n')
                    out.append(f"void* {n}(void* self, short flag) {{ if (self) {{ if (flag > 0) "
                               f"mkgp2pc_{pick(('dt0', 'dt1', 'dt2'))}__{n}(self); else if (flag == 0) "
                               f"mkgp2pc_{pick(('dt2', 'dt1', 'dt0'))}__{n}(self); else "
                               f"mkgp2pc_{pick(('dt1', 'dt2', 'dt0'))}__{n}(self); }} return self; }}\n")
                    defs.add(n)
                for k in ("dt0", "dt1", "dt2"):
                    sym = f"mkgp2pc_{k}__{n}"
                    if sym in undefined and not have(sym):
                        order = {"dt0": ("dt1", "dt2"), "dt1": ("dt2", "dt0"), "dt2": ("dt1", "dt0")}[k]
                        defsyms.append(f"-Wl,--defsym={sym}=mkgp2pc_{pick(order)}__{n}")
                        defs.add(sym)
            else:
                out.append(f'extern void* {n}(void*, short) __asm__("{asm_name(n)}");\n')
                uses.add(n)
                for k, flag in (("dt0", 1), ("dt1", -1), ("dt2", 0)):
                    sym = f"mkgp2pc_{k}__{n}"
                    if sym in undefined:
                        out.append(f'void {sym}(void* self) __asm__("{sym}");\n')
                        out.append(f"void {sym}(void* self) {{ {n}(self, {flag}); }}\n")
                        defs.add(sym)
    open(path, "w").writelines(out)
    return defsyms, defs, uses


def group_of(unit):
    parts = unit.split("/")
    if parts[0] in ("dolphin", "sysdolphin", "musyx") and len(parts) > 2:
        return "/".join(parts[:2])
    return parts[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*", default=DEFAULT_DIRS)
    ap.add_argument("--list", metavar="GROUP", help="print the undefined symbols owned by this group")
    ap.add_argument("--link", action="store_true",
                    help="generate stubs for everything missing and link build/pc-census/mkgp2-nogfx "
                         "(32-bit Linux host, needs gcc-multilib; no graphics, no SDK)")
    ap.add_argument("--dups", action="store_true", help="print symbols defined more than once")
    ap.add_argument("--os", choices=("linux", "windows"), default="linux",
                    help="windows: 32-bit Windows objects and executable in build/pc-census-win (with --gfx: build/pc-gfx-win) "
                         "(needs the i686 MinGW-w64 toolchain)")
    ap.add_argument("--gfx", action="store_true",
                    help="objects and link inputs for the rendering executable in build/pc-gfx "
                         "(Linux; needs Aurora's libraries in build/aurora32, see docs/port/build.md)")
    args = ap.parse_args()
    target = cc.TARGET
    global OUT, WIN, GFX
    if args.gfx:
        GFX = True
        OUT = os.path.join(ROOT, "build", "pc-gfx")
    if args.os == "windows":
        WIN, target = True, WIN_TARGET
        OUT = os.path.join(ROOT, "build", "pc-gfx-win" if GFX else "pc-census-win")
    os.makedirs(OUT, exist_ok=True)

    files = []
    for d in args.dirs:
        for base, _, names in os.walk(os.path.join(ROOT, "src", d)):
            files += [os.path.relpath(os.path.join(base, n), ROOT) for n in names if n.endswith((".c", ".cpp"))]
    files = sorted(f for f in files if f.replace(os.sep, "/") not in REPLACED)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        built = dict(ex.map(lambda p: compile_one(p, target), files))
    objs = [o for o in built.values() if o]

    renamed = unsuffix_objects(objs)
    expose_statics(built)
    cxx_done, cxx_unmatched = cxx_renames(built)

    defined = collections.defaultdict(list)
    undefined = collections.defaultdict(set)
    for i in range(0, len(objs), 200):
        out = subprocess.run(["llvm-nm", "-g", "--format=posix"] + objs[i:i + 200],
                             capture_output=True, text=True).stdout
        cur = None
        for line in out.splitlines():
            if line.endswith(":") and " " not in line:
                cur = line[:-1]
                continue
            f = line.split()
            if len(f) < 2:
                continue
            name, kind = f[0], f[1]
            if kind == "U":
                undefined[name].add(cur)
            elif kind in "TDBRSGC":   # strong definitions and commons; weak (W, V) inline copies are fine
                defined[name].append((cur, kind))

    weak = set()
    dups = {n: v for n, v in defined.items() if len(v) > 1 and not all(k == "C" for _, k in v)}
    missing = {n: u for n, u in undefined.items() if n not in defined}
    # weak definitions satisfy references too
    for i in range(0, len(objs), 200):
        out = subprocess.run(["llvm-nm", "-g", "--format=posix"] + objs[i:i + 200], capture_output=True, text=True).stdout
        for line in out.splitlines():
            f = line.split()
            if len(f) >= 2 and f[1] in "WVwv":
                weak.add(f[0])
    thunks = os.path.join(OUT, "cxx_thunks.c")
    thunk_defsyms, thunk_defs, thunk_uses = cxx_thunks(defined, weak, undefined, thunks)
    for n in thunk_defs:
        defined[n].append((thunks, "T"))
    for n in thunk_uses:
        undefined[n].add(thunks)
    missing = {n: u for n, u in undefined.items() if n not in defined and n not in weak}

    own = owners()
    groups = collections.defaultdict(list)
    for n in missing:
        if n.startswith("_Z"):
            groups["(C++ mangled, host ABI)"].append(n)
        elif n in own:
            groups[group_of(own[n])].append(n)
        else:
            groups["(not in symbols.txt)"].append(n)

    # A name used with C++ linkage in one file and defined with C linkage in
    # another (or the reverse) can never link: MWCC `#pragma cplusplus on`
    # regions leave the same header declared both ways.
    plain = {}
    for n in list(defined) + list(weak):
        m = re.match(r"_Z(\d+)", n)
        if m:
            k = int(m.group(1))
            plain.setdefault(n[m.end():m.end() + k], n)
    # alias[missing name] = the definition that exists under the other linkage
    alias = {n: plain[n] for n in missing if not n.startswith("_Z") and n in plain}
    for n in missing:
        m = re.match(r"_Z(\d+)", n)
        if m and (n[m.end():m.end() + int(m.group(1))] in defined or n[m.end():m.end() + int(m.group(1))] in weak):
            alias[n] = n[m.end():m.end() + int(m.group(1))]
    # `name_extN`: a unit's private prototype of `name` under a macro name
    # (the per-function files merged into one unit keep them apart that way)
    for n in missing:
        m = re.match(r"(\w+?)_ext\d+$", n)
        if m and n not in alias and (m.group(1) in defined or m.group(1) in weak):
            alias[n] = m.group(1)
    # A member function named the MWCC way in a vtable written as data
    # (`Draw__10MinimapHudFv`): the one member of that class with that name.
    for n in missing:
        m = re.match(r"([A-Za-z]\w*?)__(\d+)(\w+)$", n)
        if not m or n in alias:
            continue
        k = int(m.group(2))
        cls, rest = m.group(3)[:k], m.group(3)[k:]
        if len(cls) == k and rest.startswith(("F", "CF")):
            head = f"_ZN{'K' if rest.startswith('C') else ''}{k}{cls}{len(m.group(1))}{m.group(1)}E"
            cands = [d for d in list(defined) + list(weak) if d.startswith(head)]
            if len(cands) == 1:
                alias[n] = cands[0]
    # The race context's constructor is written under its address name
    # (RaceContext_InitDefaults, src/game/SetCourseParams.c); the unit's
    # second view of the class calls it as a constructor.
    if "_ZN11RaceContextC1Ev" in missing and "RaceContext_InitDefaults" in defined:
        alias["_ZN11RaceContextC1Ev"] = "RaceContext_InitDefaults"
    linkage = list(alias)
    # Same free function declared with other parameter types in the calling
    # file (per-file prototypes): the mangled names differ only after the
    # function name. Wire it when exactly one definition has that name.
    by_name = collections.defaultdict(set)
    for n in list(defined) + list(weak):
        m = re.match(r"_Z(\d+)", n)
        if m:
            by_name[n[m.end():m.end() + int(m.group(1))]].add(n)
    proto = {}
    for n in missing:
        m = re.match(r"_Z(\d+)", n)
        if m and n not in alias:
            cands = by_name.get(n[m.end():m.end() + int(m.group(1))], ())
            if len(cands) == 1:
                proto[n] = next(iter(cands))
    alias.update(proto)
    print(f"objects built: {len(objs)} / {len(files)}; `_pN` symbol names folded back: {renamed}")
    print(f"C++ symbols renamed as the matching build does: {cxx_done} "
          f"(table entries with no counterpart in the object: {cxx_unmatched}); bridges: {len(thunk_defs)}")
    print(f"external symbols defined: {len(defined)}; defined more than once: {len(dups)}")
    print(f"used but not defined by these directories: {len(missing)}")
    for g in sorted(groups, key=lambda g: -len(groups[g])):
        print(f"  {len(groups[g]):>5}  {g}")
    print(f"of those, same function under the other language linkage (C vs C++): {len(linkage)}")
    print(f"of those, C++ function defined once under other parameter types: {len(proto)}")
    if args.link:
        # Real implementations written for the port win over generated stubs.
        # -malign-double: `double` and 64-bit integers aligned to 8 bytes inside
        # structures, as on the console and on 32-bit Windows (the i386 Linux
        # default is 4)
        HOSTCC = ["-target", WIN_TARGET] if WIN else ["-m32", "-malign-double"]
        if GFX:
            HOSTCC.append("-DMKGP2PC_GFX")
        port_objs = []
        for sub in ("sdk", "platform", "compat", "nogfx"):
            for base, _, names in os.walk(os.path.join(ROOT, "port", sub)):
                for n in sorted(names):
                    if GFX and sub == "nogfx" and n in GFX_REPLACED:
                        continue
                    if n.endswith(".c"):
                        src = os.path.join(base, n)
                        obj = os.path.join(OUT, "port__" + os.path.relpath(src, ROOT).replace(os.sep, "__") + ".o")
                        r = subprocess.run(["clang"] + HOSTCC + ["-O0", "-g", "-c", src, "-o", obj,
                                            "-I" + os.path.join(ROOT, "port", "sdk", "include"),
                                            "-I" + os.path.join(ROOT, "port", "platform", "include")],
                                           capture_output=True, text=True)
                        if r.returncode != 0:
                            print(r.stderr[:2000])
                            return 1
                        port_objs.append(obj)
        # Matrix and vector maths come from Aurora, as in the full build (its
        # GX is not linked here, these files stand alone).
        aurora = os.path.join(ROOT, "port", "extern", "aurora")
        for n in ("mtx.c", "mtx44.c", "mtxvec.c", "quat.c", "vec.c"):
            src = os.path.join(aurora, "lib", "dolphin", "mtx", n)
            obj = os.path.join(OUT, "aurora__mtx__" + n + ".o")
            # NDEBUG: the retail functions do not check their arguments, and
            # the game does normalise zero vectors (the result is not finite).
            r = subprocess.run(["clang"] + HOSTCC + ["-O0", "-g", "-DNDEBUG", "-c", src, "-o", obj,
                                "-I" + os.path.join(aurora, "include")], capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stderr[:2000])
                return 1
            port_objs.append(obj)
        if WIN:
            # plain names for what the port defines and for what it uses of
            # the game; the names of the Windows libraries stay as they are
            r = subprocess.run(["llvm-nm", "-g", "--format=posix"] + port_objs, capture_output=True, text=True)
            pdef = {l.split()[0] for l in r.stdout.splitlines() if len(l.split()) >= 2 and l.split()[1] != "U"}
            pall = {l.split()[0] for l in r.stdout.splitlines() if len(l.split()) >= 2}
            table = os.path.join(OUT, "port.plain")
            with open(table, "w") as f:
                for n in sorted(pall):
                    if n in defined or n in undefined or n in weak:
                        continue   # spelled with __asm__ in the port source: already the game's name
                    if n.startswith("_") and (n in pdef or n[1:] in defined or n[1:] in weak):
                        f.write(f"{n} {n[1:]}\n")
            for o in port_objs:
                subprocess.run(["llvm-objcopy", "--redefine-syms=" + table, o], check=True)
        r = subprocess.run(["llvm-nm", "-g", "--defined-only", "--format=posix"] + port_objs,
                           capture_output=True, text=True)
        ours = {l.split()[0] for l in r.stdout.splitlines() if len(l.split()) >= 2}
        if GFX:
            # what Aurora's libraries and port/gfx provide is not stubbed
            adir = "aurora-win32" if WIN else "aurora32"
            libs = [f for f in (os.path.join(ROOT, "build", adir, f"libaurora_{n}.a") for n in ("core", "gx", "vi"))
                    if os.path.exists(f)]   # the ones port/gfx/CMakeLists.txt links
            if not libs:
                print(f"build/{adir} has no Aurora libraries; build them first (docs/port/build.md)")
                return 1
            r = subprocess.run(["llvm-nm", "-g", "--defined-only", "--format=posix"] + libs, capture_output=True, text=True)
            anames = {l.split()[0]: l.split()[1] for l in r.stdout.splitlines() if len(l.split()) >= 2 and l.split()[1] in "TDBR"}
            if WIN:   # Aurora's objects keep the COFF underscore
                anames = {n[1:]: k for n, k in anames.items() if n.startswith("_")}
            aurora_fns = {n for n, k in anames.items() if k == "T"} - ours
            aurora_data = set(anames) - aurora_fns - ours
            ours |= set(anames)
            for src in sorted(glob.glob(os.path.join(ROOT, "port", "gfx", "*.c*"))):
                ours |= set(re.findall(r"^[A-Za-z_][\w \*]*?\b(\w+)\([^;{]*\)\s*\{", open(src).read(), re.M))
        # Aurora has the portable C_ functions only; the paired-single PS
        # names (and the names this game gives five of them,
        # src/dolphin/mtx/__mtx_names.h) are the same functions here.
        game_names = {"Vec3_Subtract": "C_VECSubtract", "Vec3_Normalize": "C_VECNormalize",
                      "Vec3_Magnitude": "C_VECMag", "Vec3_Dot": "C_VECDotProduct",
                      "Vec3_Distance": "C_VECDistance"}
        maths = 0
        for n in sorted(missing):
            t = game_names.get(n) or ("C_" + n[2:] if n.startswith("PS") else None)
            if t in ours and n not in ours:
                alias[n] = t
                maths += 1
        print(f"paired-single maths names wired to Aurora's C functions: {maths}")
        replaced = sorted(n for n in missing if n in ours)
        missing = {n: u for n, u in missing.items() if n not in ours}
        # the host C++ runtime (linked for the units that throw) has these
        cxxrt = sorted(n for n in missing if n.startswith(("__cxa_", "__gxx_personality", "_Unwind_")))
        missing = {n: u for n, u in missing.items()
                   if not n.startswith(("__cxa_", "__gxx_personality", "_Unwind_")) and n != "__dso_handle"}
        print(f"implemented under port/: {len(replaced)} of the missing symbols")
        # A call that only fails to meet its definition because of C vs C++
        # linkage is wired to that definition instead of a stub (same calling
        # convention on this target).
        # A name that ends in an address (`lbl_806D11B8`, left over when a
        # merged unit's `_pN` name is folded back) is the symbols.txt symbol
        # at that address under its current name.
        at = {}
        for line in open(os.path.join(ROOT, "config", "GNLJ82", "symbols.txt"), encoding="utf-8"):
            m = re.match(r"(\S+) = \S+:0x([0-9A-Fa-f]+);", line)
            if m:
                at.setdefault(int(m.group(2), 16), m.group(1))
        info = symbol_info()
        by_addr = 0
        for n in sorted(missing):
            m = re.search(r"_(8[0-9A-Fa-f]{7})$", n)
            if not m or n in info or n in alias:
                continue
            c = at.get(int(m.group(1), 16))
            if c and c != n:
                alias[n] = c
                by_addr += 1
                if c not in defined and c not in weak and c not in ours:
                    missing.setdefault(c, set())
        print(f"address-named references resolved to the symbol at that address: {by_addr}")
        defsyms = [f"-Wl,--defsym={n}={t}" for n, t in sorted(alias.items()) if n in missing] + thunk_defsyms
        missing = {n: u for n, u in missing.items() if n not in alias}
        print(f"wired to an existing definition (linkage or prototype mismatch): {len(defsyms)}")
        # Non-virtual thunks (`this -= N`, then the function): the host
        # compiler emits them only in the unit that defines the function, and
        # that unit often models the class without its second base.
        renamed = {}
        for f in glob.glob(os.path.join(OUT, "*.cxxsyms")):
            for line in open(f):
                a, b = line.split()
                renamed[a] = b
        this_thunks = []
        for n in sorted(missing):
            m = re.match(r"_ZThn(\d+)_(.+)$", n)
            if not m:
                continue
            t = renamed.get("_Z" + m.group(2), "_Z" + m.group(2))
            if t in defined or t in weak or t.startswith("mkgp2pc_"):
                this_thunks.append((n, int(m.group(1)), t))
        missing = {n: u for n, u in missing.items() if n not in {t[0] for t in this_thunks}}
        print(f"non-virtual thunks generated: {len(this_thunks)}")
        stubs = os.path.join(OUT, "stubs.c")
        nfun, nobj = write_stubs(missing, groups, stubs)
        with open(stubs, "a") as f:
            for n, off, t in this_thunks:
                f.write(f'__asm__(".text\\n.globl {n}\\n{n}:\\n subl ${off}, 4(%esp)\\n jmp {t}\\n");\n')
        print(f"stubs generated: {nfun} functions, {nobj} objects")
        exe = os.path.join(OUT, "mkgp2-nogfx.exe" if WIN else "mkgp2-nogfx")
        rsp = os.path.join(OUT, "objects.rsp")
        with open(rsp, "w") as f:
            f.write("\n".join(objs) + "\n")
        # original data layout (port/tools/mwcc_data_layout.py)
        inputs = collections.defaultdict(list)
        for i in range(0, len(objs), 200):
            if WIN:
                rr = subprocess.run(["i686-w64-mingw32-objdump", "-h"] + objs[i:i + 200], capture_output=True, text=True)
                for line in rr.stdout.splitlines():
                    m = re.match(r"\s*\d+ mw\.\w+\.(\w+)\s+([0-9a-f]+) .* 2\*\*(\d+)$", line)
                    if m:
                        inputs[m.group(1)].append((int(m.group(2), 16), 1 << int(m.group(3))))
                continue
            rr = subprocess.run(["readelf", "-SW"] + objs[i:i + 200], capture_output=True, text=True)
            for line in rr.stdout.splitlines():
                m = re.match(r"\s*\[\s*\d+\] mw\.\w+\.(\w+)\s+\w+\s+[0-9a-f]+ [0-9a-f]+ ([0-9a-f]+) .* (\d+)$", line)
                if m:
                    inputs[m.group(1)].append((int(m.group(2), 16), int(m.group(3))))
        lnotes = []
        script = os.path.join(OUT, "data_layout.ld")
        with open(script, "w") as f:
            f.write(mwcc_data_layout.linker_script(DATA_OBJECTS, inputs, lnotes))
        with open(os.path.join(OUT, "data_layout.notes"), "w") as f:
            f.write("\n".join(lnotes) + "\n")
        host_sizes = {}
        if WIN:
            host_sizes = win_bss_sizes(objs)
        for i in range(0, len(objs) if not WIN else 0, 200):
            rr = subprocess.run(["nm", "-S", "--defined-only", "--format=posix"] + objs[i:i + 200] + ([stubs] if False else []), capture_output=True, text=True)
            for line in rr.stdout.splitlines():
                q = line.split()
                if len(q) == 4 and q[1] in "BbCcDdRr":
                    host_sizes[q[0]] = max(host_sizes.get(q[0], 0), int(q[3], 16))
        bnotes = []
        bss_ld = os.path.join(OUT, "bss_layout.ld")
        with open(bss_ld, "w") as f:
            f.write(mwcc_data_layout.bss_script(mwcc_data_layout.original_bss(ROOT), host_sizes, bnotes, noload=not WIN))
        with open(os.path.join(OUT, "bss_layout.notes"), "w") as f:
            f.write("\n".join(bnotes) + "\n")
        print(f"zero-initialised objects kept out of the original layout (larger here): {len(bnotes)} ({os.path.relpath(OUT, ROOT)}/bss_layout.notes)")
        print(f"data objects placed at their original distances: {len(inputs) - len(lnotes)} of {len(inputs)} (rest: {os.path.relpath(OUT, ROOT)}/data_layout.notes)")
        # The game has data objects called `errno` and `time` (its C library's
        # own). Exported from the executable they would replace the host C
        # library's symbols of those names for the shared libraries too (libm
        # then stores an error code through the game's `errno` as if it were
        # a thread-local offset). Keep them out of the dynamic symbol table.
        private = os.path.join(OUT, "private_names.ver")
        host_names = sorted(n for n in ("errno", "time") if n in info) + [
            "wcslen", "wcscpy", "wcsncpy", "wcscmp", "wcschr", "wmemset"]  # port/compat/wchar16.c
        with open(private, "w") as f:
            f.write("{ local: %s };\n" % " ".join(n + ";" for n in host_names))
        if GFX and WIN:
            return link_gfx_windows(stubs, thunks, cxxrt, replaced, aurora_fns, aurora_data, port_objs, objs, defsyms,
                                    script, bss_ld, set(defined) | set(weak))
        if GFX:
            gen = []
            for src in (stubs, thunks):
                o = src[:-2] + ".o"
                r = subprocess.run(["clang", "-m32", "-O0", "-g", "-fcommon", "-c", src, "-o", o], capture_output=True, text=True)
                if r.returncode != 0:
                    print(r.stderr[:2000])
                    return 1
                gen.append(o)
            link = os.path.join(OUT, "gfx_link.rsp")
            with open(link, "w") as f:
                f.write("\n".join(["-no-pie", "-fcommon", "-Wl,--version-script=" + private, "-Wl,-T," + script, "-Wl,-T," + bss_ld]
                                  + gen + port_objs + objs + defsyms + ["-Wl,--wrap=" + n for n in GFX_WRAPPED + ("main",)]
                                  + ["-lz", "-lm", "-lpthread", "-Wl,--allow-multiple-definition", "-Wl,--no-warn-rwx-segments"]) + "\n")
            print(f"link inputs for CMake: {os.path.relpath(link, ROOT)}")
            return 0
        if WIN:
            # the host C++ runtime's names carry the COFF underscore
            with open(stubs, "a") as f:
                for n in cxxrt:
                    f.write(f'__asm__(".text\\n.globl {n}\\n{n}: jmp _{n}\\n");\n')
                # and the C runtime starts the program at its own spelling of main
                f.write('__asm__(".text\\n.globl _main\\n_main: jmp main\\n");\n')
            gen = []
            for src in (stubs, thunks):
                o = src[:-2] + ".o"
                r = subprocess.run(["clang", "-target", WIN_TARGET, "-O0", "-g", "-fcommon", "-c", src, "-o", o], capture_output=True, text=True)
                if r.returncode != 0:
                    print(r.stderr[:2000])
                    return 1
                win_plain_names(o, asm_only=True)
                gen.append(o)
            # --large-address-aware: main memory is mapped at 0x80000000
            # (port/nogfx/os_min.c). Static, so that the executable needs no
            # MinGW runtime DLLs next to it.
            cmd = ["clang", "-target", WIN_TARGET, "-g", "-static", "-Wl,--large-address-aware", "-Wl,-T," + script, "-Wl,-T," + bss_ld,
                   "-o", exe] + gen + port_objs + ["@" + rsp] + defsyms + ["-lstdc++", "-lwinpthread", "-Wl,--allow-multiple-definition"]
        else:
          cmd = ["clang", "-m32", "-O0", "-g", "-fcommon", "-no-pie", "-Wl,--version-script=" + private, "-Wl,-T," + script, "-Wl,-T," + bss_ld, "-o", exe, stubs, thunks] + port_objs + [
                 "@" + rsp] + defsyms + ["-lm", "-lpthread", "-lstdc++"] + (["-lz"] if os.path.exists("/usr/lib32/libz.so") else []) + [ "-Wl,--allow-multiple-definition", "-Wl,--no-warn-rwx-segments"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        errs = [l for l in r.stderr.splitlines() if "error" in l or "undefined" in l]
        print(f"link: {'ok -> ' + os.path.relpath(exe, ROOT) if r.returncode == 0 else 'FAILED'}")
        seen_err = set()
        errs = [l for l in errs if not (re.sub(r"^.*(undefined reference to .*)$", r"\1", l) in seen_err or seen_err.add(re.sub(r"^.*(undefined reference to .*)$", r"\1", l)))]
        for l in errs[:60]:
            print("  " + l[:200])
    if args.dups:
        for n in sorted(dups):
            print(f"dup {n}: {len(dups[n])} definitions")
    if args.list:
        for n in sorted(groups.get(args.list, [])):
            print(f"{n}  ({len(missing[n])} users)")
    return 0


if __name__ == "__main__":
    import signal
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    sys.exit(main())
