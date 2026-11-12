# A 64-bit build: what stands in the way (assessment, 2026-11-12)

Asked after a crash was traced to a miscompiled 32-bit Dawn DLL: 32-bit
builds of Aurora and Dawn are a path few people use. Nothing was converted;
this note records the measurements and the options. Numbers are for main at
`0b154bf` and come from `port/tools/pointer_width_census.py` unless another
source is named.

## 1. What assumes 4-byte pointers

| what | count | who can fix it |
| --- | --- | --- |
| Files a 64-bit clang rejects outright | 31 more than on 32-bit, measured for 64-bit Linux (21 pointer-to-`int` casts in C++, 10 size checks written as negative array sizes) | decomp |
| Casts between a pointer and a narrower integer | 420 sites in 104 files | decomp (type the field or variable as a pointer) |
| Records whose size or member offsets change | 2,098 of 3,927 laid out; 1,074 of 2,650 files use one | harmless by itself, see below |
| Accesses by byte offset (`*(float *)(self + 0x1C)`, and the per-file `F(o)`, `P(o)`, `WP(o)` macros) | about 3,000 (2,695 through macros, 327 written out) in 146 files; about 550 of them read or write a pointer | decomp (real members) |
| Record names that have more than one layout already on 32-bit | 126 | decomp (struct clean-up) |
| Type names defined in more than one file | 343 names, 790 extra copies (`tools/port_readiness.py` in mkgp2-decomp) | decomp |
| Members that are filler (`char pad[0x..]`, `unk..[0x..]`) | about 1,900 lines | decomp, where the filler hides a pointer |
| Vtables written out as data | 182 of 416 (`port_readiness.py`) | decomp (real classes), bridged port-side by `mwcc_cxx_abi.py` |
| Pointers to member functions | 12-byte MWCC layout kept on every host by `include/game/types/pmf.h`; tables written as `void *x[3]` | both: the macro is host-neutral, the 12 bytes are not |
| HSD archives relocated in place | 1,917 files; 110 of 158 sysdolphin records change | port only |
| Other disc formats whose offsets become pointers in place (TPL descriptors and the like) | not counted; the loaders are the `PORT_DATA_LOADED` kinds in `port/platform/data/data_loaded.c` | port only |
| `u32` is `unsigned long` (`include/dolphin/types.h`), and 549 lines in the game spell `long` | 8 bytes on 64-bit Linux, 4 on 64-bit Windows | port (typedef override); MWCC needs `long` for symbol names |
| Main memory at its console address (`0x80000000`, 48 MB) and reads of `*(u32 *)0x800000F8` | works unchanged in a 64-bit process, the range is below 4 GB | nothing to do |
| IR passes (`mwcc_cxx_abi.py`, `mwcc_byvalue.py`, `mwcc_fifo.py`) | written for `i32` offsets and 4-byte slots, about 20 lines | port |
| Source patches under `port/patches` | 26 files, 4 add a `void *host_vptr` to a private struct | port |
| 369 struct-by-value / pointer mismatches | handled in IR, do not depend on pointer size | nothing to do |

Why a changed layout matters here although any C program survives it: the
game's objects are reached through more than one description. A class is
written once as a class and again as a private struct in the C file that
reads it, as a byte offset in a third file, and as filler in a fourth. On a
4-byte host all of them agree because all were matched against the same
machine code. On an 8-byte host only the descriptions that name the pointer
as a pointer move, and nothing reports the disagreement: the matching build
proves nothing about it, the host compiler accepts both, and the program
reads the wrong word. The vtable-pointer crashes of census section 33 were
this class of fault, one word wide.

## 2. Options

### A. Real 64-bit pointers

Every row marked "decomp" above has to reach zero, and the disc formats need
converters that rebuild each file with wide pointers instead of fixing it up
in place (the HSD walker in `port/platform/data/hsd_archive.c` already types
every block of every archive, so it can be grown into one). This is the same
debt as "one definition per class" in `cxx_abi_decision.md`; it is what the
Twilight Princess and Metroid Prime ports have for free because their source
was real C++ throughout (facts recorded there).

- Decomp side, host-neutral and byte-identical under MWCC: members instead
  of byte offsets, one definition per type, pointers typed as pointers,
  `sizeof` instead of literal sizes, real classes instead of written-out
  vtables. Each change can be checked against the matching build, but the
  matching build cannot tell whether the result is right on 64-bit.
- Port side: disc converters, pointer-to-member layout, the IR passes, the
  `u32` typedef, the patches.
- Size: months of thread work, and the end is hard to see because faults
  are silent and show up one play session at a time.

### B. 64-bit process, game code keeps 4-byte pointers

The game code is compiled with 4-byte pointers and 4-byte `long` but as
x86-64 machine code, and linked into a 64-bit program with the 64-bit Aurora
and Dawn that upstream publishes. No source change, layouts stay those of
the console, disc data stays relocated in place.

Checked here with clang 18:

- Linux: `-target x86_64-pc-linux-gnux32` gives `sizeof(struct { int; void *; void *; }) == 12`,
  and `llvm-objcopy -O elf64-x86-64` turns the object into one a 64-bit link accepts.
- Windows: clang refuses `x86_64-w64-windows-gnux32`. Taking the IR from the
  Linux target and giving it that triple, `llc` writes assembly with the
  Windows calling convention and 4-byte pointers, but with ELF directives
  mixed in, and crashes when asked for an object file. So on Windows this
  is an unsupported configuration that needs an experiment before anyone
  relies on it.

What it needs besides the compiler: everything the game can point at must
sit below 4 GB (the image, the stacks of the threads that run game code,
main memory already does), and every structure that crosses to Aurora or the
host C library with a pointer, `long` or `size_t` inside needs a shim
(`port/gfx/gx_shim.cpp` wraps 68 functions today for other reasons).
Variadic functions defined in game code are a known trap on the Windows
side.

OpenGP (GPL, facts only) runs the game in a 64-bit process without widening
anything either, by a different route: it recompiles the PowerPC code,
reserves a 4 GiB host range and addresses console memory as `base + addr`
(`docs/architecture.md` there).

- Size: two or three days to prove or kill it on Windows; if it holds,
  one to two weeks to move the build over.
- Risk: toolchain territory nobody else uses, which is the complaint about
  32-bit Dawn again in another place.

### C. Stay 32-bit and repair the libraries

What the port does today. The Dawn fault is being fixed by rebuilding the
package (thread "Fix abnormal exit during play"). Cost per fault: days.
Risk: more faults of the same kind in Dawn, drivers and D3D12 on 32-bit,
each found by a crash.

### D. Two processes

The game stays a 32-bit program without Dawn; a 64-bit program holds Aurora
and receives the GX, VI and input calls over shared memory. Keeps both
halves on well-travelled ground at the price of a call layer for a few
hundred functions and a second program to start and debug. Not sized here.

## 3. Recommendation

Stay on C for now. Do the decomp-side clean-up of section 1 for its own sake,
in the order the port trips over it, and keep `pointer_width_census.py` as
the measure; A becomes possible when its rows are near zero and is not worth
aiming at before. If 32-bit library faults keep coming after the Dawn
rebuild, run the B experiment on Windows, and take D if B fails.
