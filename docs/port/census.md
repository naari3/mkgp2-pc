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

## 13. MainGameLoop is reached; data byte order and the C++ object model (2026-11-10, upstream `de53dc8`)

With the load hooks upstream added (`PORT_DATA_LOADED`, kinds 1-11 in
`include/game/types/port_hooks.h`, notes in upstream
`docs/notes/byte_order.md`) and two converters in
`port/platform/data/data_loaded.c` (TPL, sprite animation), the first scene
is built (`CourseModeInit`, 27 textures and its sprite animations) and
`main` enters `MainGameLoop`. The first frame stops at its first virtual
call: `MainGameLoop.cpp:73` `Scene_Draw(self->current)` -> `scene->Draw()`
jumps to 0.

Why: the scene's vtable is `lbl_8049A734`, a table written out as data in
MWCC's layout (RTTI pointer, offset, then the functions from +8) and stored
into the object as it is. `Scene_Draw.cpp` is C++ compiled by the host
compiler, which expects the Itanium layout (the object points at the first
function). The same object is seen through both conventions.

This is one of a family. MWCC's C++ object model against the host's:

| | MWCC (CW for GC) | Itanium (clang / GCC) |
| --- | --- | --- |
| where the object's vtable pointer points | start of the table; functions from +8 | first function; RTTI and offset before it |
| where the vtable pointer sits in an object | after the data members of the class that introduces the first virtual (upstream `docs/notes/approachA-roadmap.md`) | offset 0 |
| virtual destructor | one slot, one function with a hidden flag | two slots (complete, deleting) |
| constructor | returns `this` | returns nothing |
| pointer to member function | 12 bytes | 8 bytes |

The link-time bridges of section 12 cover constructors and non-virtual
destructors. The vtable rows are not solved. They matter wherever one object
is built or called by C-style code in one unit and by C++ in another, which
this code base does all the time (hand-written tables: 315 distinct vtable
names are attached to C++ classes by the rename table alone).

Options, not yet decided:
1. Rewrite the host compiler's output to MWCC's conventions. LLVM IR shows
   virtual calls and vtable address points explicitly (`%vfn = getelementptr
   ptr %vtable, N`; `getelementptr (@_ZTV..., 0, 0, 2)`), so a pass can move
   the address point to the table start and add 2 to every slot. Destructor
   slots need the class of each call (obtainable with `-fwhole-program-vtables`
   type tests or the vtable layout dump); the position of the vtable pointer
   inside objects needs struct rewriting and is the hard part.
2. Change the sources upstream so that, off MWCC, polymorphic classes are
   plain structs with an explicit vtable pointer member and virtual calls go
   through a macro. Large (about 700 class copies), but it makes the layout
   explicit and compiler-independent, MSVC included.
3. A compiler ABI that matches better. The Microsoft ABI has the single
   destructor slot with a flag; nothing has the vtable pointer after the
   members.

Data byte order status: converters exist for kinds 1 (TPL) and 6 (sprite
animation). HSD archives (kind 3, 1,918 `.dat` files) are the big one: the
converter has to walk the object graph from the roots with the standard
sysdolphin descriptor layouts.

## 14. The main loop runs (2026-11-10, upstream `d0d86cd`)

`build/pc-census/mkgp2-nogfx` now gets through the boot sequence and keeps
running `MainGameLoop`: 300 iterations (scene draw, scene update, per-frame
state) were counted with a breakpoint at `MainGameLoop.cpp:73`, the scene
state is 4, and nothing crashes for as long as it was left running (about a
minute). Nothing is drawn: every GX call is a counted stub.

What it took after section 13:

- C++ virtual calls: `port/tools/mwcc_cxx_abi.py` rewrites the LLVM IR of
  each C++ unit so that objects point at the start of the vtable and a
  virtual destructor has one slot (frozen in this scope until the C++ ABI
  decision in `docs/port/cxx_abi_decision.md`).
- Pointers to member functions: upstream `include/game/types/pmf.h`
  (request 14).
- Once virtual calls reached their targets, the boot-time checks really ran,
  and the hardware stand-ins had to be right:
  - the baseboard's operator switch byte is active low; with it reading 0
    the game went into the test menu;
  - a JVS node's answer comes back one baseboard frame after the request
    (`jvsio.c` matches packets against the requests of the frame before);
    with same-frame answers the bus enumerated zero nodes and the steering
    check failed with E20;
  - the steering drive board on serial port 0
    (`port/platform/triforce/drive_board.c`): answers "C01" while output
    bit 0x80 is high and "C06" while it is low, which is what the power-on
    check waits for. Inferred from `src/game/StrPcb_Tick.c` only;
  - the baseboard DIP switch 0x01 is on by default: the game then skips the
    card reader, printer and camera checks (`BootDispatcher.cpp`). Without
    it the printer check waits for the test button.
  `MKGP2PC_TRACE_JVS=n` prints the first n baseboard frames with JVS
  packets.

All hardware facts so far come from the decompiled game and am* libraries;
nothing was taken from Dolphin.

Known wrong but not yet in the way: vtable pointer position for the classes
whose data members come before their first virtual function (MWCC puts the
pointer at that position, measured with GC/1.3.2; about 17 base-less classes
in `src` and `include`), data files other than TPL and sprite animation
(HSD archives first), `AnimState_Init(self, 0.0f)` (upstream
`docs/notes/port_abi.md`).

## 15. HSD archives and the simple record files are converted (2026-11-10)

`port/platform/data/hsd_archive.c` converts an HSD archive in place at the
`PORT_DATA_ARCHIVE` hook: relocation, public and extern tables, every pointer
field the relocation table names, then the typed fields found by walking the
graph from `scene_data` and the `*_joint` symbols (scene, model sets, joints,
display / material / texture / polygon objects, vertex descriptions,
envelopes, shape sets, splines, reference objects, the four animation trees,
camera, lights, fog). Layouts are the sysdolphin ones in the game tree's
`libs/melee/src/sysdolphin/baselib` headers.

Checked outside the game on all 1,917 archives of the disc (220 MB):
- a Python prototype of the walk compared the pointer fields it expects with
  the relocation tables: 506,051 of 512,437 entries are fields of reached
  blocks; the rest sit in blocks nothing points at (every archive carries an
  unreferenced copy of its scene struct, some carry unreferenced lights);
- the C converter ran over all of them under ASan and UBSan with no finding
  and met nothing it does not handle. Only these occur in the data: public
  symbols `scene_data` and `*_joint`; reference objects of the joint and
  IK-hint kinds; perspective cameras; no particle joints, no render
  animations, no expressions or bytecode. Two archives have fog.

Two layout facts the headers did not give: `scene_data`'s camera and fog
fields point at a pair { descriptor, animation list }, and a shape set's
vertex description may point into the middle of the list its polygon object
uses (lists are therefore converted element by element).

Left in console byte order on purpose, to be settled with the GX layer:
vertex buffers, display lists, texture images and palettes, shape index
lists. `pobj.c` reads display lists and vertex data on the CPU for skinned
and shape-animated meshes, so that code will need either converted buffers
or byte-wise reads. Animation track data is a byte stream; how `fobj.c`
assembles multi-byte values from it has not been checked on a little-endian
host yet.

Also converted now: course path tables (kind 4), ghost recordings (5), demo
input recordings (9). Not yet: course BGM table (7), collision files (8),
MusyX files (10), stream headers (11).

## 16. Past the card flow, into the class select (2026-11-10)

- MSL containers: `port/compat/include/{vector,list,deque}` are stand-ins
  with MSL's object layout (vector `{capacity, size, data}`, list `{size,
  end node}`, deque 28 bytes), enough for the six units written against the
  MSL headers. The real headers do not compile with clang.
- Exceptions: the units that contain `throw` or `try` (nine) are compiled
  with exceptions on and the link takes the host C++ runtime. Nothing is
  expected to throw. The IR pass passes the destructor flag on `invoke` as
  well as `call`; nothing else about it changed.
- Generated stubs went from 385 to 300 functions.
- Scripted input: entries must lie inside the repeat period
  (`"500 coin; 1000 an1=ffff; 1100 an1=0; 1500 sw=00,00,20; 1600
  sw=00,00,00; period 2000"`); entries beyond the period never fire.
- With a coin the menu goes attract -> card flow (9) at retrace 5001 ->
  class select (11) at retrace 5221. The card flow is real code now; with
  the DIP switch for "no card devices" it finishes at once.
- The class select stays in its first state. Cause: its rodata is a byte
  array and the code reads ints out of it (upstream request 15); 877 such
  objects exist.
- Layout difference found on the way: the Itanium ABI lets a derived class
  put its members into the tail padding of a non-POD base, MWCC does not.
  `GpLabelPanel` is 0x94 bytes under MWCC and smaller on the host, so
  `GpPleaseInit.cpp` fails its own size check. Not handled; belongs with the
  class layout work upstream (`docs/port/cxx_classes.md`).
- Windows target check of the IR pass, done on Linux by compiling only
  (`-target i686-w64-windows-gnu`, no link): 915 of 918 game C++ units
  compile and pass through `mwcc_cxx_abi.py`, the same three fail as for
  Linux before the exception retry. One difference mattered: that target
  passes `this` in a register for member functions (`x86_thiscallcc`), which
  cannot work with vtables written as data that hold C functions, and it
  hid all 440 destructor-slot calls from the pass. The pass now takes that
  convention off member functions, after which the counts match the Linux
  build (440 destructor calls, 35 wrappers). Still to do on a real Windows
  machine: link and run.
- Data kinds: the stream header (11) and the course collision files (8) are
  converted now. A collision file is 32-bit words throughout: header 0x2C,
  16-byte cells, 0x70-byte triangles, 0x20-byte walls; on the 35 files of
  the disc these records and their zero terminators account for every byte.
  Left: the course BGM table (7; the hook has no size and no reader of the
  records is decompiled by name yet) and the MusyX files (10; only useful
  together with a sound back end).
- HSD CPU-side reads checked: `fobj.c` assembles every value from single
  bytes, so key frame data needs no conversion. `pobj.c` reads display lists
  and index lists byte by byte; only the three shape animation helpers read
  16-bit and float vertex data directly (upstream request 17).
- Graphics in the 32-bit process: Aurora takes Dawn as a prebuilt package,
  and those exist for 64-bit targets only (`AuroraDawnProvider.cmake`).
  Building Dawn from source needs `chromium.googlesource.com`, which the
  cloud environment cannot reach. So a 32-bit Dawn has to be built somewhere
  else (GitHub Actions or the Windows machine) before real GX can be wired
  into the 32-bit game.

## 17. Through the menus to the pre-race scene (2026-11-10, upstream `55ea6d8`)

- Upstream typed the byte-array data that holds numbers (request 15) and
  fixed `TransitionEffect.cpp` (request 16).
- Data layout: the game reads some data relative to another object of the
  same unit (`Scene3D_Init` takes the string 0xBC bytes after
  `lbl_802E96B8`). `port/tools/mwcc_data_layout.py` gives arrays and structs
  MWCC's minimum alignment of 4 in every unit (C units now also go through
  LLVM IR) and keeps each generated data unit in one section, in definition
  order. `port/tools/data_layout_check.py <exe>` compares object distances
  with `symbols.txt`: 238 units, 16,257 pairs, 2 units still differ
  (`data_804EE3A8.c`, `sdata2_806DB6E0.c`).
- With the scripted input the menu now runs attract -> card (9) -> class
  (11) -> character (13) -> figure (19) -> cup (21) -> round (27) -> "GP
  please" (37), in 8,900 retraces. `select_mario.dat` and the other menu
  archives load and convert.
- Stop: flow 37 is `clFlowGpPlease`, a generated stub, because
  `GpPleaseInit.cpp` does not compile (base class tail padding, upstream
  request 18). The first draw of the uninitialised object crashes.
- Request 18 landed (`c72769e`); flow 37 then failed in `card_pack`, which
  reads strings 0x12A4 and more bytes after `lbl_8039C718`: an anchor in one
  source file, the targets two units further on. The host link now
  reproduces the original data layout as a whole instead of per unit:
  - initialised data: every object of `symbols.txt` gets an input section
    of its own (`mw.<section>.<name>`, set in the IR) and a generated linker
    script puts it at its original offset in `.mw_rodata`, `.mw_data`,
    `.mw_sdata`, `.mw_sdata2` (26,828 of 26,908 placed; the rest are larger
    on the host than the room they have, `build/pc-census/data_layout.notes`);
  - zero-initialised data: one block per original section, and the script
    assigns every object's name to its original offset in it (the game
    clears 0x1EC00 bytes of `g_itemObjectManager`, which `symbols.txt`
    gives 0x1E4CB bytes; on the host this used to wipe the neighbours).
  This only holds for the 32-bit build, where object sizes match.
- With that the attract mode goes further by itself: it starts the demo
  race, loads the item models, the course model (`dn_stadium_long_a.dat`)
  and the collision file. Stop: a `void` function whose caller uses the
  result (upstream request 19).


## 18. The demo race runs without rendering (2026-11-10, upstream `08f7eb7`)

The attract mode now builds the whole demo race (course, eight karts, HUD)
and keeps running it: update and draw paths of the race scene execute every
frame, with GX still stubbed. Checked for 1,000 frames after the race starts
(retrace 4,947 to 5,968). What it took, in the order the run stopped:

- Upstream requests 19 to 23 (`docs/port/upstream_requests.md`): results of
  `void` functions, `new[]` paired with `delete[]`, text and HSD descriptors
  typed with their real field widths, constructor argument order, arguments
  that only travelled through r3.
- `__assert` is sysdolphin's handler (file, line, expression). Its unit holds
  PowerPC assembly and is not built, so the calls bound to the C library's
  function of the same name with other parameters. Defined in
  `port/nogfx/os_min.c`.
- `GXGetTexBufferSize` returns a real size in the build without rendering
  (`port/nogfx/gx_min.c`); HSD allocates its shadow texture from it.
- Template names in `tools/extab_user_renames.json`
  (`erase__12ListImpl<Pv>F...` -> `DoublyLinkedList_EraseRange`) are now
  paired with the host's names (`port/tools/mwcc_names.py`). The shared list
  functions used by about a hundred units were stubs until then. 382 of the
  533 template keys are understood.
- Matrix, vector and quaternion maths come from Aurora's
  `lib/dolphin/mtx/*.c`. Aurora has the `C_` functions only; the link wires
  the `PS` names, and the five names this game gives them (`Vec3_Dot`, ...),
  to those. Built with `NDEBUG`: the game normalises zero vectors, which the
  console does without complaint. Still missing: `PSMTX44Copy`,
  `PSMTX44Identity`.
- A vtable whose destructors the compiler found identical lists the pair as
  D2/D0 instead of D1/D0; the destructor-slot rule of
  `port/tools/mwcc_cxx_abi.py` now takes both (22 vtables). Before, `Update()`
  of the course's map effect called its destructor.
- The game has data objects called `errno` and `time`. Exported from the
  executable they replaced the C library's symbols for the shared libraries
  as well, and libm crashed storing an error code. The link keeps them local
  (version script).
- Integer division by zero: PowerPC does not trap, x86 does. The game does
  `rand() % counts[i]` with empty entries (`DNFlash_tail.cpp:121`) and then
  discards the result. `port/tools/mwcc_int_div.py` turns every division by a
  non-constant into a call that returns what the console returns (87 units).
  A build that does not go through LLVM IR needs another answer for this.

Not converted yet and reported at run time: data kind 10 (MusyX project
files). Functions still stubbed in this build: 228, of which 100 are GX.

## 19. From a coin to the start of a real race (2026-11-10, upstream `b5862a5`)

The scripted session (section 16) now leaves the demo race, goes through the
six menu scenes and the pre-race scene, and enters the race scene (flow 42),
which starts loading its course. What that needed:

- `delete p` through a class that has no debug description in the unit: the
  destructor slot is learnt from the `delete` itself
  (`port/tools/mwcc_cxx_abi.py`).
- The slot of a virtual destructor is no longer read from the debug
  description. The compiler writes index 0 there wherever the destructor is
  declared; the slot is now the gap the other virtual functions leave. Until
  this, every class whose destructor is not its first virtual function had
  its calls shifted by one (a call of the second virtual function became a
  deleting destructor call).
- `-fno-strict-return`: `card_pack` is declared with a result and returns
  none; C++ puts a trap instruction at the end of such a function.
- Wide strings: the game's `wchar_t` has 16 bits. `wcscpy`, `wcslen`,
  `wcscmp`, `wcsncpy`, `wcschr`, `wmemset` are in `port/compat/wchar16.c`
  and stay local to the executable. `vswprintf` is still a stub.
- Upstream request 24: the card name check compares 16-bit Shift-JIS codes
  with byte functions; its patterns are 16-bit arrays now, and the calls go
  through `NAMECODE_*` macros (`include/game/types/namecode_str.h`) that a
  build may point at big-endian-view helpers: `port/compat/namecode_be.c`
  (since 2026-11-11), defined in `mwcc_compat.h`.
- `inline` is defined away for C units: a plain `inline` function that is
  called without being inlined needs an ordinary definition (C99 gives
  none, GNU89 gives none for `extern inline`, and the sources use both).
- Struct results (`port/tools/mwcc_struct_return.py`): the hidden result
  pointer is an ordinary first argument, as with MWCC. The shared list
  functions are declared both as returning an iterator and as `void` with
  an explicit result pointer; on x86 those are two conventions and the
  stack ended up 4 bytes off.
- The arena starts at 0x806ECFA0 as on the console (it started at
  0x80800000, 1.1 MB less heap).

- `wchar_t` is 16 bits (`-fshort-wchar`, with the wide string functions the
  game uses in `port/compat/wchar16.c`). With 32 bits every structure that
  holds a name was larger than the size the game allocates for it, and the
  writes past the end broke the heap; that was the "out of memory" while
  the race scene loaded `YI_land_short_a.dat`.
- Functions that MWCC folded into one share one entry of the rename table;
  only the leaf among the ones a unit defines takes the name.
- The rename table is also searched without regard to parameter order and
  to what pointers point at (NON_MATCHING sources spell some constructor
  parameters differently).

The scene change to the race (flow 42, retrace 9236) now loads the course
and constructs the player's kart. Open: `fn_802090F0` and five more
functions are declared with their float and integer parameters in another
order than they are defined (upstream request 25).

## 20. The real race runs; a 32-bit Windows build links (2026-11-10, upstream `f294028`)

The scripted session (coin, menus, section 16) now builds the whole race
scene (course, player kart, AI karts, HUD) and the race itself runs about
430 frames (retrace 9236 to 9665) before the first finish-line check stops
it.

What it took:

- Float and integer parameters declared in another order than defined
  (upstream request 25) and calls that pass fewer arguments than the
  definition takes, relying on argument registers passing through a wrapper
  (request 26). `port/tools/arg_order_census.py` lists both from the host
  IR; run it after every merge. Its second list also shows destructors
  "called with one argument": those go through the constructor / destructor
  bridges and are fine.
- `__dynamic_cast` of the MWCC runtime (`fn_80271340`) is implemented in
  `port/platform/runtime/mwcc_runtime.c`. One class can have several
  type_info objects in the image, so types are compared by name.
- `cos(double)` / `sin(double)` declared inside C++ regions were stubs that
  returned 0 (`port/compat/cxx_math.c`).

Open: `PathManager_UpdateAll` passes the address of one `float` to
`Math_Segment2DIntersect`, which writes three through it (request 27).

### `--os windows`

`link_census.py --os windows --link` builds the same program for
`i686-w64-windows-gnu`. Everything after compilation works on names, so
the mode makes a COFF object look like the ELF one first:

- COFF puts an underscore before every C name. It is taken off again in
  each object (`win_plain_names`); the few names the game takes from the C
  and C++ runtimes get a one-instruction bridge (`memcpy: jmp _memcpy`) in
  the generated stubs. The port's own objects are renamed only where they
  define a name or use one of the game's.
- An inline function is a COMDAT section there, and GNU ld keeps one such
  section per section name whatever its symbol is called by then. The
  rename tables are per unit, so the copy that survived could carry the
  wrong name. In the IR they become ordinary definitions and the link
  takes the first (`--allow-multiple-definition`), like the weak symbols
  of the ELF build.
- `x86_thiscallcc` is removed: member functions are called through C
  declarations all over the decompiled code.
- A name that begins with `L` is an assembler-local label in COFF assembly,
  also when it comes from an `__asm__("...")` label (clang 18 crashes on
  `LObjList_Destroy`). Generated code spells such a name
  `mkgp2pc_asmL_<name>` and the object is renamed afterwards.
- GNU ld gives every `NOLOAD` section of a PE image the same address; the
  blocks for the original `.bss` / `.sbss` / `.sbss2` layout are ordinary
  zero-filled sections there.
- COFF symbols have no size; the sizes the `.bss` layout needs come from
  the distance to the next symbol (`win_bss_sizes`).
- `-mno-ms-bitfields`, so that bit-fields are laid out as on Linux.
- Main memory at 0x80000000 needs `--large-address-aware` and
  `VirtualAlloc`; the image is made writable with `VirtualProtect`
  (`port/nogfx/os_min.c`). `pread` is replaced in `port/nogfx/dvd_min.c`.

The data layout of the executable agrees with the Linux one object for
object. Differences that remain: `double` and 64-bit integers are aligned
to 8 bytes inside structures on Windows (as on the console) and to 4 on
Linux; zlib is not linked (`inflate` is a stub). Not run yet: there is no
way to run a Windows program in the build container.


## 21. First pictures (2026-11-10, upstream `5938fbd`)

The game is linked with Aurora and draws: under Xvfb with Mesa's software
GL the boot logo screen, the attract fly-through of a course with the karts
and the character introduction screens come out as on the console. The
race of section 20 meanwhile ran 1,136 frames before the stop fixed by
upstream request 28.

How the rendering executable differs from the bring-up one
(`link_census.py --gfx`, glue in `port/gfx/`):

- **Frame and video.** `port/gfx/video_aurora.c` replaces
  `port/nogfx/video_min.c`: `VIWaitForRetrace` ends the Aurora frame, pumps
  the window events and begins the next one. `main` is wrapped
  (`--wrap=main`) so that Aurora starts after the C++ static constructors
  of the host libraries.
- **Object sizes.** With `TARGET_PC` Aurora's `GXTexObj` is 64 bytes and
  `GXTlutObj` 40; the game reserves 32 and 12. The texture and palette entry
  points are wrapped (`port/gfx/gx_shim.cpp`): the game's objects only
  record the parameters, and the real objects live in a cache keyed by data
  pointer, size, format and palette. The cache is dropped whenever file
  data is loaded (`mkgp2pc_data_generation`) and on `GXInvalidateTexAll`.
- **Array sizes.** `GXSetArray` has no size on the console; Aurora uploads
  the given size on every draw into an 8 MB frame buffer. The HSD archive
  conversion (`port/platform/data/hsd_archive.c`) notes every vertex buffer
  on its walk and takes the next relocation target as its end
  (`mkgp2pc_array_extent`). Buffers it has not seen get 64 KB. Vertex
  buffers, display lists and texture images stay big-endian, which is what
  Aurora expects.
- **Direct pipe writes.** MWCC inlines the vertex functions as stores to
  0xCC008000 (or to the `GXWGFifo` object some units declare). The game's
  headers are compiled with Aurora's `GXVert.h`
  (`port/compat/include/dolphin/gx/GXVert.h`), and the stores that are
  written out in the sources are rewritten in IR into `GXParam1*` calls
  (`port/tools/mwcc_fifo.py`).
- **operator new.** The game defines the global `operator new` and
  `delete` on top of its own heap. Linked into one program with host C++
  libraries, those would take them too, so the game's are renamed
  `mkgp2pc_game_new` and so on in every game object.

Not done: the 129 functions still stubbed in this build (`GXPeekZ`,
`PSMTX44Identity` and others show up in the log), CPU-side reads of
big-endian vertex data, sound, input from the window, Windows.

## 22. A whole play, rendered (2026-11-10, upstream `8cfa617`)

With rendering the scripted session now goes once around: coin, menus, the
real race drawn (course, kart, HUD, minimap, items, coins), time-up, the
Mario coin tally, the logo, the attract demo and the character select of the
next play. Without rendering the same session runs to the script limit.
What stood in the way after the first pictures:

- **Data read wider than defined** (request 29) and **float results declared
  void** (request 30): the second left a value on the x87 register stack at
  every call, until every float load gave NaN and the kart's position with
  it. Both fixed upstream with a census tool each.
- **Inline functions other units call.** MWCC emits an inline C++ function
  whose address the exception tables take; the host needs
  `-fkeep-inline-functions` for C++ units.
- **Vtables with two groups written as data** (CarObject): the constructor
  stored Itanium address points. `mwcc_cxx_abi.py` step 2b maps group 0 to
  the table start and group 1 to its MWCC offset.
- **std::deque members called by address name** (`fn_801E2C40`,
  `fn_801E2DF0`, `fn_801E2F28`, `fn_801E2FD4`, `fn_801E3260`,
  `fn_801E40C0`): the AI waypoint queue. As stubs they left the queue
  uninitialised and the scene teardown freed a stale pointer.
  `port/compat/deque_bridge.c` implements them on the layout of
  `port/compat/include/deque`.
- **Deleting destructors of abstract classes.** clang emits a trap; some
  instantiated classes are modelled with a pure virtual slot (clFAllDisp).
  `mwcc_cxx_abi.py` gives them the real body.
- **Struct arguments** (request 31): MWCC passes the address of a copy, so
  the sources spell the same parameter `T` in one place and `T *` in
  another. `port/tools/mwcc_byvalue.py` drops `byval` on game definitions,
  declarations and calls; a definition copies on entry. Aurora's GX
  functions (GXColor by value) keep the host convention.
- Aurora: vertex array extents come from the archive conversion
  (`mkgp2pc_array_extent`), the texture cache is dropped when data is
  reloaded, `PSMTX44Copy` / `PSMTX44Identity` are in `port/compat`.

Still open: sound, window input, data kinds 7 and 10, zlib on Windows, the
remaining stubs, and the Windows executables have not been run.

## 23. Names that did not meet, sound survey (2026-11-10, upstream `b210d84`)

Stubs that the rendered play actually called, and what they were:

- **Non-virtual thunks.** A host compiler emits `this -= N` thunks only in
  the unit that defines the virtual functions; the sources often model the
  class there without its second base. `link_census.py` generates the 16
  missing ones at link time; the AI controller's five, which a vtable written
  as data names by address, are in `port/compat/ai_thunks.c`.
- **C names in the rename table.** `tools/extab_user_renames.json` also maps
  descriptive C names to address names (`SceneListInsert` -> `fn_801F4DF0`,
  the menu task list's insert); those entries are applied now.
- **Other spellings**: `name_extN` private prototypes, member functions named
  the MWCC way in data vtables (`Draw__10MinimapHudFv`), weak plain
  definitions, and two explicit aliases (`PORT_ALIASES`).
- **C-linkage inline functions**: clang has no `-fkeep-inline-functions`
  (section 22 was wrong about that); such units are compiled through a
  wrapper that takes the functions' addresses.

Generated stub functions: 207 -> 173 (no-graphics build). Several whole plays
in a row run in the scripted session.

Keyboard controls are in `port/gfx/video_aurora.c` (docs/port/build.md).

Sound, not started: all 27 units of `src/musyx` compile on the host (request
32). On the console the DSP mixes; github.com/AxioDL/musyx (CC0, the project
our MusyX sources come from) has a PC target for MusyX 2.0.2 with a software
mixer, raw GC asset loading and an optional SDL adapter
(`include/musyx/pc.h`: `sndPCConfigure`, `sndPCRender`). Plan: build it as a
library (`MUSYX_ENABLE_SDL=OFF`, `MUSYX_VERSION_PATCH=2`), leave `snd*`
unstubbed, wrap `sndInit` in `port/gfx` and feed an SDL audio stream from
`sndPCRender`. Adding it as a submodule waits for the owner's go-ahead.


## 24. Sound, start-up functions, pace (2026-11-11)

- **Sound**: the MusyX PC target (`port/extern/musyx`) answers `snd*`;
  build and device layer in docs/port/build.md. Effects and music are
  produced from the menus through the race.
- **Hand-registered start-up functions.** 29 functions are registered with
  `__declspec(section ".ctors") static void (*const x)() = fn;`. The compat
  header empties `__declspec`, the host compiler drops the unused pointer,
  and the functions never ran: the HUD's digit position table stayed zero and
  all digits of a time were drawn on one spot. `link_census.py` now writes
  one constructor that calls them in address order (27 found by name).
- **Pace.** The game advances once per retrace. `VIWaitForRetrace` now keeps
  59.94 frames a second itself (`MKGP2PC_NOLIMIT=1` turns that off). A
  recording made from the sound device while the game ran at 0.6 of real
  time (software rendering) sounded slowed down, with the effects spaced
  out; `MKGP2PC_AUDIO_DUMP=<file>` renders one frame's worth of sound per
  video frame instead (raw 16-bit stereo, 32 kHz).
- Text: Shift-JIS char strings were read two bytes at a time in host
  byte order (the player name read "マリI"); upstream request 33,
  `SJIS_WORD` defined in `mwcc_compat.h`.
- Data kind 7 (`PORT_DATA_COURSE_BGM`) needs no conversion: nothing reads
  the file beyond the count of leading zero records (`data_loaded.c`).
- Also since section 23: `vswprintf` for 16-bit units (`wchar16.c`),
  `OSTicksToCalendarTime` (host local time, `os_min.c`), all 28
  hand-registered start-up functions called.


## 25. Windows start-up, black models, memory size (2026-11-11)

- **Windows start-up** (details in build.md): PE sections on multiples of
  the section alignment (`mwcc_data_layout.py`, checked by `pe_check.py`),
  runtime names such as `___main` never stubbed, emulated TLS with a fixed
  image base, `-fpcc-struct-return` for the game's units (i686 Windows
  returns structs of up to 8 bytes in registers; callers that the port
  rewrote to the hidden-pointer form crashed in `ListImpl<void*>::erase`).
  The executable can be built on a Windows PC through WSL and docker
  (`port/docker`).
- **Black karts, drivers, Lakitu, item boxes.** Lit HSD materials set the
  channel's material alpha separately (`HSD_SetupChannel`, `GX_ALPHA0`,
  alpha 0). In the SDK `GX_COLORn` replaces only RGB and `GX_ALPHAn` only
  alpha (`src/dolphin/gx/GXLight.c`); Aurora's `GXSetChanMatColor` and
  `GXSetChanAmbColor` write all four components, so the material colour
  became black and every lit colour 0. The game's calls go through
  `port/gfx/gx_shim.cpp`, which keeps both registers.
  How it was found, for the next rendering bug: breakpoints with raw stack
  reads on the GX entry points (uniq by caller), forcing arguments from gdb
  (lighting off, number of TEV stages 1 or 2), Aurora's `EnableDebugPrints`
  (`lib/gx/gx.hpp`) for the generated WGSL, and finally printing
  `'aurora::gx::g_gxState'` after `finish` from `GXCallDisplayList`.
  Uninitialised-looking values in `GXSetTevKColor`/`GXSetTevColor` calls
  from `HSD_TExpSetReg` are normal (components no stage selects).
- **48 MB of main memory.** The Triforce board has 48 MB; `os_min.c`
  assumed 24. The heap was 14.6 MB and the attract mode used it up loading
  its third scene (`HSD_InitMemAlloc`, initialize.c:458, about 139 s on
  Windows; on Linux only without a coin). The arena now ends at 0x82F00000,
  a choice of this port: the real boot program's value is not known.
- Next stop in the attract mode: `GXProject` called with another argument
  order in `CObjProject.c` (upstream request 35).

## 26. Windows: the wrong class's vtable; display lists read on the CPU (2026-11-11)

**Crash at about 150 s on Windows only** (`fn_8014E618` jumps into
`lbl_806CF2D8`, which is data). The attract sequence had just built its
`DemoScene` (`Scene_8014E0C4.cpp`), and its constructor stored the vtable
`lbl_80419EF8` instead of `DemoScene_vtable_8048FF3C`. The game has two
unrelated classes called `DemoScene` (the other one is in
`include/game/DemoKartScene.h`, used by nine units), and each unit's
`_ZTV9DemoScene` is renamed to the right hand-written table per object
(`*.cxxsyms`). For MinGW, clang reaches data of another unit through a
pointer `.refptr.<name>` (support for automatic DLL import), a COMDAT the
linker keeps once per name, and that name is the one from before the rename:
both classes shared one `.refptr.__ZTV9DemoScene`. The ELF build has no such
indirection, so Linux was right. Fix: `-fno-auto-import` for the Windows
target (`port/tools/link_census.py`); no game object has a `.refptr` symbol
any more. Found on the PC with a write watchpoint on the object's first word.

**`OSPanic("miyazakishare.cpp", 259, "Unknown DLCommand")`** twice while a
course loads, on both systems, execution went on. The shadow code
(`src/game/dlistutil`) walks display lists and reads vertex positions on the
CPU; that data stays big-endian for the GX layer, so a vertex count of 3 was
read as 768 and the walk left the list. Upstream request 36: access macros
`GXDATA_U16`, `GXDATA_SET_U16`, `GXDATA_F32` (`include/game/types/gx_data.h`),
defined for the host in `port/compat/mwcc_compat.h`. Not converted: the
shape-animation path of the HSD library (`src/sysdolphin/baselib/pobj.c`),
to be asked for if a model turns out to use it.

## 27. The attract loop goes round (2026-11-11)

With section 26 the Windows build ran 8 minutes of attract mode, but stayed
on the last screen of the sequence ("ただいまカメラは使えません") for ever.
Three more places where data is seen through two views:

- **Cabinet-link header** (request 37). The scene number of a cabinet is
  written through a 32-bit bit-field and read with `word >> 15`, next to
  byte views of the same four bytes. On the host the 32-bit view fell on
  the flag bits of the next byte, the scene a cabinet read back for itself
  was garbage, and `PcbSync_AllKartsInValidSelectState()` never let the
  screen end. It only showed with the frame limiter on: unpaced, the link
  code had not yet filled the slot when the check ran. Host accessors work
  on bytes (`port/compat/mwcc_compat.h`, `PCB_*`).
- **Byte-in-word tables** (request 38). `(void *)0x8000000` in a `void *[]`
  is the byte 8 on the console and a word whose first byte is 0 here. The
  item-in-hand animation table was read without its end marker and the
  second round of the loop crashed in `AnimState_InitEntries`. Upstream
  retyped the animation tables, the item class and descriptor tables, 504
  joint animation tracks and more.

Checked on Linux: three rounds of the loop (about 150 s each unpaced), and
a scripted race, with no panic and no crash. The sequence, from the table
`fn_8014E804` walks: logo and notices, title, demo race (60 s), entry
prompt, character demo, entry prompt, how-to-play, camera screen (10.5 s).

How it was found: gdb Python breakpoints that print only on change
(scene index, kind and vtable in `fn_8014E804`; state in the scene's update).
A first attempt called a game function from the breakpoint and hung; print
variables only.

## 28. Rendering differences reported from play (2026-11-11)

- **Pure red backgrounds** (attract captions, Bowser close-up): not a
  loading problem. `lbl_806D7F70`, two RGB triples for the frame clear
  colour, was defined `int [2]` and read as bytes
  (`Scene_8014E0C4.cpp`, `fn_8002CFB8`): black read as (255,0,0).
  Upstream request 39; `port/tools/data_width_census.py` lists every
  definition / declaration pair that disagrees on element width. The same
  request fixed 22 `unsigned long long` item-effect masks whose halves were
  swapped and 7 colour constants of the item render callbacks.
- **Kart shadows: black bands across the ground.** The shadow is a 512x512
  image drawn into a corner of the frame buffer before the frame and copied
  out with `GXCopyTex` (`HSD_ShadowStartRender` / `HSD_ShadowEndRender`,
  called from `SceneRender_DrawObjectWithLights`). The console's frame
  buffer is 640x528 for every render mode; Aurora's is `efbHeight` (480)
  high, so the last 32 lines of the image, with the white border that
  clamping repeats outside the image, were never drawn. Fixed in the port
  layer: `__wrap_HSD_ShadowSetSize` keeps the image within 480x480
  (`port/gfx/gx_shim.cpp`). Tried first and dropped: telling Aurora the
  frame buffer is 528 high with `emulateXfb`; the picture is then scaled by
  480/528 and the copy rectangle no longer falls on whole pixels.
- **Tyres of the special karts**: `KartTireFX` loads
  `sp_tire_set_<name>.dat` correctly (vertices and joints read as in the
  normal set) and the game itself sets the wheel scale to 1000 for modes 2
  and 3 (`KartTireFX::KartTireFX`). Whether that hides them on the console
  too is not known; open.
- In the demo scene after the title (`DemoScene`) no shadow pass ran in
  the frames measured (`Frame_Begin` did not reach
  `SceneRender_DrawObjectWithLights`); races and the race intro run it
  every frame. Why, and whether the console does the same, is not checked.
- Reference read for the comparison: OpenGP (GPL-3, `1a6aae4`),
  `docs/architecture.md` and `runtime/gx`: it recompiles the game together
  with the SDK's GX library and emulates the graphics chip at the FIFO /
  register level (640x528 frame buffer, copies cleared over the copy
  rectangle only), so neither the byte-order class nor the GX-API class of
  difference exists there. Facts only; no code taken.

## 29. The play stopped on a black screen after the race (2026-11-11)

Reported from play on Windows (main `7941a3e`): after time-up the screen
stayed black with only the credit display, the main loop still running.
Reproduced without graphics on Linux with a blind script (coins, the item
button every 90 frames, accelerator held).

- Cause: upstream `7a73526` made `clResultType00` / `clResultType01` (the
  result flow after a race) real classes with compiler-emitted vtables.
  They have two polymorphic bases, and `mwcc_cxx_abi.py` rewrote only
  single-group vtables: the table kept Itanium's address point and two
  destructor slots while every caller used MWCC's slots, so the scene's
  `Draw` slot ran `Enter` and its `Update` slot ran `Draw`; the flow never
  advanced. Nothing to do with the card unit.
- MWCC's table for such a class (from the data the class replaced,
  `HudFlow_data_804EEB20`): first base's part, second base's part with its
  two header words, then the functions the class adds (`Enter`, `Leave`).
  The callers written against that data (`HudMethodScene`: slots at 0x24
  and 0x28) rely on it. The pass now emits that order and moves calls of
  the added functions accordingly (step 1b).
- Checked without graphics: the same script now goes through the result
  screens, writes and ejects the card (a new card bought by the script)
  and plays the next races, 330 s without a stop.
- The four other two-group tables the build emits (`AIController`,
  `RankDisp`, `RaceHudRoot`, `RaceSetupMI`) exist only because of a key
  function; nothing refers to them (their objects use the tables written
  as data).

## 30. Black screen after a finished race: units that did not compile (2026-11-11)

Second report from play (Windows, `593009c`): black screen with only the
credit display after a race the player finished, log line
`stub: fn_8014CEAC (generated:0)`.

- Cause: five units stopped compiling when upstream turned their classes
  into real classes with RTTI and spelled casts as `dynamic_cast<T *>`
  (`CSSceneGetTitle_8014ADB4.cpp`, `anon_801FEBD8.cpp`,
  `anon_80202148.cpp`, `anon_80204474.cpp`, `fn_801F9F04.cpp`; the port
  compiles with `-fno-rtti`). `link_census.py` left a failing unit out
  without a word and every function of it became a stub returning 0.
  `fn_8014CEAC` creates the scene shown first after a finished race
  (`clCSSceneAccolade`), so the result flow had no scene. The other four
  hold menu tasks and card screens. The first report (section 29) was a
  race lost on time, which takes another branch.
- `link_census.py` compiles such a unit with RTTI on, and prints
  `NOT BUILT: <path>` for every unit left out.
- The cast cannot go to the host runtime (it reads a type_info below the
  address point, which the pass moves; data-written tables carry MWCC's
  type_info instead). `mwcc_cxx_abi.py` sends it to
  `mkgp2pc_dynamic_cast` (`port/platform/runtime/mwcc_runtime.c`) with
  the plain name of the target class, and puts a description in MWCC's
  `{ name, bases }` form into the second header word of every table it
  emits (step 1c). The first word stays the offset to the complete
  object, so `fn_80271340` treats host-laid-out classes as before (cast
  succeeds); only the new function checks them. Classes in a namespace or
  with template names get no description and their casts succeed.
- Checked without graphics, with `g_raceEnded` forced to 1 from gdb when
  the result flow is constructed (the blind script cannot finish a race):
  result flow states 0 (accolade), 2, 4, 6 are entered and left, the card
  is written and ejected, the next play starts. The 32-bit Windows
  no-graphics executable links. Not checked: the rendering builds.
- Still a stub that play reaches: `dtor_801C1B78` (a folded destructor of
  `KeyVecBase<KeyGroup>` and others; called from `CardPrintOwner.cpp` and
  `anon_8021BC44_nm1.cpp`). It returns without freeing; not looked into.

## 31. Reports from play, 2026-11-11 evening

- **Name entry: empty key grid** (fixed, upstream request 45). The keyboard
  tables are Shift-JIS text read as 16-bit numbers. Script that reaches the
  screen: coins at frames 5823, 5906, 6000 and an item press every 300
  frames from 6300 to 8100, card service on.
- **Window resize or maximize on Windows ends in webgpu_dawn.dll** (worked
  around, cause unknown): see `MKGP2PC_SCALE` in build.md.
- **"coins until next prize!" popup after the coin tally, in English, digits
  on top of the text** (open; reproduced on Linux, card service on, at the
  end of the first race). What is known:
  - The popup is sprite 0x149, `sp_next_goods.bin`. Its resource list is
    0x1140, 0x1141, 0x1142, 0x114E, 0x114F: pieces of
    `tpl2_E_sp_next_goods.tpl` (English text) and the digits 0 and 1 of
    `tpl2_E_sp_rare_goods.tpl`.
  - The positions in that file fit the Japanese picture
    (`tpl2_JP_sp_next_goods.tpl`, ids 0x1924..0x1926: one line, then a short
    piece, two digits, a short piece). `E_sp_next_goods.bin` has the same
    ids and the English positions.
  - The program hides the two digit placeholders with
    `SetAnimParam(0x1931, ...)` and `(0x1932, ...)`: the Japanese digit ids.
    With the list from the disc these calls find nothing, and the
    placeholders stay visible under the real digits (seen in the picture as
    doubled outlines).
  - So the program expects the list 0x1924, 0x1925, 0x1926, 0x1931, 0x1932.
    No such list is in main.dol or anywhere on the disc (searched as bytes),
    and nothing in the load and draw chain rewrites ids:
    `Sprite_InitWithAnim`, `fn_801A1494`, `fn_801A0D90`, `fn_8019FA30`,
    `fn_801A1174`, `SpriteLayer_SetResource`, `ResourceSlot_Load`,
    `TEXLoadPalette`, the `ResourceTable_*` readers. The anim id remap
    (`lbl_806CFD00`) is empty and only used when the region byte is not 0.
  - Of the 495 animation files on the disc, five without the `E_` prefix
    point at English textures: `sp_next_goods.bin`, `sp_rare_goods.bin`,
    `tab_at_namCAM_msg.bin`, and the unused `sp_coin_mileage.bin` and
    `sp_coin_mileage_bar.bin` (the scene uses the `JP_sp_coin_mileage_*`
    files).
  - `isJapanese()` returns 0 on the arcade as well; 0 is the Japanese
    branch (comment in `GetTextureByIdAndLang.c`).
  Port side (data_loaded.c, `goods_lists`): the lists of `sp_next_goods.bin`
  and `sp_rare_goods.bin` are replaced at load by the ids the program hides
  (0x1924, 0x1925, 0x1926, 0x1931, 0x1932 and 0x1930..0x1934). Linux then
  shows the Japanese popup with the digits in their gap and no placeholder
  under them. `MKGP2PC_GOODS_DISC=1` keeps the disc's lists.
  The user reports this as wrong against the arcade. How the arcade gets
  the Japanese ids is not found. A run of the original in an emulator at
  this screen would settle what the list is at draw time.
