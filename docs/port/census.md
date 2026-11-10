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
