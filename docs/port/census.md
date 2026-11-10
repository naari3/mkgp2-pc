# Host-compiler census

How much of the decompiled source a PC compiler takes today. Re-run the two
tools after every upstream merge; numbers below are for upstream `6bfd6dd`
(2026-11-10), clang 18, 32-bit little-endian x86 target, `NON_MATCHING`
defined, `port/compat/mwcc_compat.h` force-included. Nothing here was linked
or executed: "accepted" means it parses and type-checks, not that it works.

## 1. Files accepted (`port/tools/compile_census.py`)

| Setup | Files accepted |
|---|---|
| plain clang, `.c` as C | 1993 / 2848 |
| + compat header, C++ where MWCC compiles C++, `NON_MATCHING` | 2243 / 2848 |
| + size_t = `unsigned long` (`--size-t-long`) | 2681 / 2848, `src/game` 2577 / 2664 |

What the steps fixed:

- A `.c` file is C++ for MWCC when configure.py passes `-lang=c++` or when
  `#pragma cplusplus on` appears in it or in a header it includes (468
  pragmas). The PC build has to pick the language the same way.
- `__declspec(section ...)`, `__frsqrte`, `__fabs`: macros in the compat
  header. `__frsqrte` still needs a bit-exact table implementation.
- `operator new(unsigned long)` is declared in about 460 files. PC targets
  have `size_t` = `unsigned int` (32-bit) or `unsigned long long` (Win64), and
  reject it. Right fix: spell it `size_t` upstream (same object code under
  MWCC). Requested from the decomp side 2026-11-10; until it lands the
  measurement uses a target whose `size_t` is `unsigned long`.

Still failing with all of that (167 files): 66 missing includes (mostly
`src/musyx`, `src/sysdolphin`, which need their own include paths and are not
in scope yet, plus MSL C++ headers `<list>`, `<deque>`, `<new>`), 27 PowerPC
inline asm in `include/libc/math.h`, about 20 real `asm` functions without a C
side, 14 C / C++ linkage conflicts, a handful of constructs MWCC accepts and
clang does not.

## 2. What a link would need (`port/tools/link_census.py`)

Objects for `src/game`, `sound`, `ui`, `util`, `resource` (-O0, not linked):

```
objects built: 2589 / 2684
external symbols defined: 31196; defined more than once: 3
used but not defined by these directories: 3546
   1389  game
    895  (C++ mangled, host ABI)
    576  (no split: auto unit)
    305  (not in symbols.txt)
    123  sysdolphin/baselib
     65  triforce
     60  dolphin/gx
     35  dolphin/mtx
     24  MSL_C
     23  musyx/runtime
     13  dolphin/os
      9  dolphin/dvd
      9  Runtime.PPCEABI.H
      7  thp
      3  dolphin/vi
      3  dolphin/ar
      2  init
      2  dolphin/base
      2  dolphin/ai
      1  dolphin/demo
of those, same function under the other language linkage (C vs C++): 66
```

Reading it:

- The SDK surface the game really uses is small: about 350 functions and
  objects outside the game's own directories (HSD 123, Triforce 65, GX 60,
  MTX 35, MSL 24, MusyX 23, OS 13, DVD 9, THP 7, VI 3, AR 3, AI 2). List one
  group with `--list dolphin/gx`.
- `game` and `(no split: auto unit)` are the game's own symbols: definitions
  in the files that did not compile, and objects that only exist as
  `symbols.txt` entries (mostly `.bss`) with no C definition yet.
- `(C++ mangled, host ABI)` (895) is not broken down yet: it mixes C++
  functions defined in the files that did not compile with a real problem,
  language linkage. MWCC lets a `.c` file turn C++ on half way, so the same
  function can be declared with C linkage in one file and C++ linkage in
  another; under MWCC the matching build still links (symbol names are forced
  by `symbols.txt`), on PC it cannot. The tool confirms 66 such functions by
  name; the true number is somewhere between 66 and about 900. Needs a rule
  (for example: every free function the game defines gets C linkage) and
  probably upstream header work.
- Only three symbols are defined twice, so per-TU duplicate definitions are
  not a link problem.

## 3. Not measured yet

- 64-bit. Everything above is 32-bit, where struct layouts and pointers
  stored in loaded data keep their size.
- Endianness: no loader swaps anything yet.
- Behaviour. The C sides taken through `NON_MATCHING` were never executed.
- Aurora's prebuilt Dawn exists for x86_64 and aarch64 only (checked
  2026-11-10); a 32-bit executable needs Dawn built from source.

## 4. Update, same day (port-side fixes only)

`port/compat/include/stdarg.h` and `math.h` (PC versions ahead of
`include/libc`), plus `-I.` and `-Isrc/zlib`: 2713 / 2848 files accepted
(`src/game` 2587 / 2664, `src/musyx` 23 / 27).

Reachability: starting from `main` and following symbol references between
the host objects reaches 1981 of 2589 objects. The scene and object tables
pull in nearly the whole game, so there is no small subset that boots: the
first runnable build is "everything links, missing parts stubbed".

## 5. Plan from here

1. Get the remaining game files through the compiler (77 in `src/game`; a
   long tail, each needs a look). Port-side where possible; real source
   errors go upstream.
2. Decide the C / C++ linkage rule and apply it (section 2).
3. Generate stubs for every symbol still undefined (`link_census.py` already
   has the list) so the game links as a 32-bit executable with no graphics,
   then run it and fix what breaks first: this is where 32-bit and
   big-endian assumptions show up.
4. Put Aurora underneath (needs 32-bit Dawn, or the move to 64-bit) and
   replace stubs group by group: OS, DVD, MTX, GX, HSD, then audio and the
   Triforce services.

Known traps recorded on the way:

- `std::list` / `std::deque` (three files) were matched against the MSL
  layout; host STL containers have other sizes, which matters wherever they
  sit inside objects with a fixed allocation size.
- `include/libc/math.h` declares `float sqrt(float)`; renamed `sqrt_f32` in
  the PC copy.
- `__frsqrte` must be implemented with the PowerPC estimate table.

## 6. Update: upstream `393e3ab` merged

`operator new` now takes `size_t` upstream (`include/game/types/size_t.h`), so
the plain 32-bit target gives the same result and `--size-t-long` is no
longer needed: 2713 / 2848 files accepted on `i686-pc-linux-gnu`.

## 7. First run without graphics (2026-11-10)

`python3 port/tools/link_census.py --link` (Linux, needs `gcc-multilib`)
compiles every accepted game file with `-O0 -g`, generates one stub per
missing symbol (2575 functions that log their first call and return 0, 937
zero-filled objects sized from `symbols.txt`), takes the C library from the
host, and links `build/pc-census/mkgp2-nogfx`, a 32-bit executable whose
entry is the game's own `main`.

It starts: `main` calls `BootDispatcher`, which runs through the first SDK
calls (all stubs) and then crashes at `BootDispatcher.cpp:377` copying a
`BootState` from a pointer a stub returned as 0. gdb shows source lines.

This is the loop for the next stage: run, look at the first crash or the
stub log, replace the stub that matters with a real implementation in
`port/sdk` or `port/platform`, repeat. `mkgp2pc_frsqrte` is a placeholder
(`port/compat/ppc_math.c`), accurate but not bit-exact.

## 8. Two ways to replace the Triforce libraries (measured, not decided)

The game calls 65 functions of the `am*` / `ac*` / `jvs*` libraries.

- Replace at that API: 65 functions to write in `port/platform/triforce`,
  each from the decompiled C in `src/triforce` (boot info, coin, backup,
  JVS input, sockets, card UART).
- Or compile `src/triforce` as it is (70 of 71 files already compile; add
  `triforce` to the `link_census.py` directories) and emulate the hardware
  underneath. The missing set then becomes 12 EXI functions, `SITransfer` /
  `SIIsChanBusy`, 18 DVD functions and 37 OS functions (threads, alarms,
  semaphores, message queues, which Aurora's OS does not have). The original
  coin, backup and JVS logic is kept, at the price of emulating the baseboard
  and media board protocols on EXI and JVS frames on SI.

Both link today. The second keeps more original behaviour and has the smaller
surface; whether its state machines come up without real boards has to be
tried.

## 9. Bring-up state at the end of 2026-11-10

`python3 port/tools/link_census.py game sound ui util resource triforce sysdolphin --link`
links the game together with the decompiled Triforce libraries and HSD
(2786 of 2848 files compile; `src/sysdolphin` 45 / 53 with its own include
set). `port/nogfx/os_min.c` maps 24 MB at `0x80000000`, so console addresses
stay valid in the 32-bit process, and supplies arena, time and interrupt
no-ops.

The run now gets through the am* boot calls and stops in `Alloc(4)`
(`BootDispatcher.cpp:396`): the game heap is created by `HSD_InitComponent`,
and `sysdolphin/baselib/initialize.c` is one of the eight HSD files that do
not compile yet (see `upstream_requests.md` item 9).

Also noted: `rand` / `srand` must be the MSL ones (`src/MSL_C/rand.c`), not
the host's, or anything seeded diverges from the console.

## 10. After upstream `df21da4` (same day)

Files accepted: 2807 / 2848 (`src/game` 2633 / 2664, `src/sysdolphin`
52 / 53). The bring-up link now also

- folds the `_pN` suffixes of merged translation units back
  (`tools/merge_tus.py` upstream renames them in the MWCC objects; 2132
  names), and
- wires a call to its definition when the two only differ in C / C++
  linkage (53 left after upstream's C regions).

Missing after that: 3124 symbols, of which about 2000 functions and 1000
objects get generated stubs. Two known sources inside the game itself:
files that still do not compile (for example `SetCourseParams.c`, which
defines `g_playerData`, 74 users), and functions defined `inline` in one C++
file and called from others (`fn_80120A08` in `SpriteHandle.cpp`, 56 users):
MWCC emits them, clang does not. `-fkeep-inline-functions` is not usable (it
breaks 1900 files), so those need `inline` dropped outside MWCC or a list.

Run: `main` -> `BootDispatcher` -> am* boot -> `HSD_InitComponent`, which now
runs (arena, `GXInit`, disc ID from `port/nogfx/os_min.c`) and stops at
`initialize.c:99` because the render mode pointer is NULL: `fn_80255640`
(acLib init) reports failure, since nothing answers on EXI where the Triforce
baseboard should be. Next step is that device.

## 11. Baseboard and JVS up (2026-11-10, upstream `da0081e`)

- `port/platform/triforce/baseboard.c` replaces `src/triforce/amexi.c`
  (EXI backup store; the original reads EXI bytes as big-endian words, so it
  must not be compiled on PC). `ambackup` initialises against it.
- `port/platform/triforce/jvs_si.c` implements `SITransfer` /
  `SIIsChanBusy`: the baseboard's SI frame protocol and one JVS I/O board
  with neutral inputs, written from `src/triforce/jvsio.c`. `jvsio.c`,
  `amjvs*.c` run unchanged on top.
- `amsys` init (`fn_80294810`) now returns 0.

New class of problem found here: statics reached from other units through an
address-named extern (`upstream_requests.md` item 10). Classification of the
1025 stubbed objects: 705 are unowned `.bss` (fine as zero storage), 315 lie
inside a `src/game` unit that does not define that name, 2 in `jvsio.c`
(aliased by hand), 3 belong to dolphin libraries.

Next stop: `acLib_tail.cpp:98` spins on `fn_8028BAD0() == 5`, waiting for the
amjvs worker thread. `OSCreateThread` and friends are still stubs.

## 12. The boot sequence completes (2026-11-10, upstream `0f0bec5`)

`mkgp2-nogfx` now runs the game's whole `BootDispatcher` (about 4,200
frames of the boot screens, nothing drawn) and returns to `main`, which goes
on to build the first scene. What it took, in the order the run stopped:

| Stop | Cause | Answer |
| --- | --- | --- |
| `acLib_tail.cpp:98` spins | am* worker threads never ran | `port/nogfx/os_thread.c`: OS threads, semaphores, message queues, alarms on host threads; "interrupts disabled" is one global lock (a state, not a nesting count) |
| JVS init state 3 | sense line never reported | `jvs_si.c`: status bit 0 = a node without an address |
| `ScopedTimer` divides by zero | bus clock read at 0x800000F8 | low-memory globals filled in `os_min.c` |
| amsys state 0, later the wait for the network version | no media board; the game needs board 0x29 with network board 0xA9 | `port/platform/triforce/mediaboard.c`: registers, two mailboxes, interrupt bits; address / version / serial commands answered, every socket command answered "error". Replaces `src/triforce/dvdcmd.c` |
| `fn_80288574` reads garbage | call with fewer arguments than the definition | upstream request 11; `port/tools/arity_census.py` found 130 pairs, fixed upstream |
| write to `lbl_806D2EB8` faults | written object defined `const` (.sdata2) | image made writable at start (bring-up); the console has no read-only memory |
| 1,008 C++ names undefined | constructors / destructors / methods are C functions in other units | `link_census.py` applies `tools/extab_user_renames.json` to the host objects (`mwcc_names.py` pairs MWCC and Itanium names) and generates bridges: Itanium C1/C2, D0/D1/D2 against MWCC's one constructor returning `this` and one destructor with a flag. Function stubs 1,943 -> 460 |
| `PCBComm_Init` wrong branch | bit-field order | upstream request 12, fixed upstream (`BITFIELDS` macros) |
| `lbl_806D11B8` was a function stub | address-named leftover of a merged unit | resolved to the `symbols.txt` symbol at that address (79 names) |
| text drawing | ROM font, write-gather pipe at 0xCC008000, no disc | bring-up stand-ins in `port/nogfx`: blank font, plain memory at 0xCC000000, files read from the disc image (`MKGP2_ISO`), VI retrace and GX draw-done callbacks |

Run it: `MKGP2_ISO=/path/to/disc.iso build/pc-census/mkgp2-nogfx`.

Where it stops now: `main` -> `FlowDispatcher_Create` -> first scene
constructor -> `TEXLoadPalette("tpl2_JP_sp_joutyuu_a.tpl")` retries forever.
The file starts with a little-endian size that the game byte-swaps by hand
(correct on PowerPC, wrong here), and everything after it (TPL headers,
sprite and animation tables, HSD archives) is big-endian data that the game
reads through native structs. This is the next layer of the port: data byte
order. `MainGameLoop` is the call right after `FlowDispatcher_Create`.

Known gaps behind this run, not yet problems: vtables written by hand in C
(MWCC layout: RTTI pointer and offset first, functions from +8) against
compiler-made Itanium vtables; `throw` in two units (built with
`-fno-exceptions`); thread priorities are not modelled; the 12-byte MWCC
pointer-to-member.
