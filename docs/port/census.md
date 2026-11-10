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
- `(C++ mangled, host ABI)` and the linkage line are one problem: MWCC lets a
  `.c` file turn C++ on half way, so the same function is declared with C
  linkage in one file and C++ linkage in another, and under MWCC the names
  still meet (address-named symbols, link order). On PC they do not. Needs a
  rule (for example: everything the game defines gets C linkage unless it is
  a class member) and probably upstream header work.
- Only three symbols are defined twice, so per-TU duplicate definitions are
  not a link problem.

## 3. Not measured yet

- 64-bit. Everything above is 32-bit, where struct layouts and pointers
  stored in loaded data keep their size.
- Endianness: no loader swaps anything yet.
- Behaviour. The C sides taken through `NON_MATCHING` were never executed.
- Aurora's prebuilt Dawn exists for x86_64 and aarch64 only (checked
  2026-11-10); a 32-bit executable needs Dawn built from source.
