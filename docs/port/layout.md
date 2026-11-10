# Layout of the PC port

Written 2026-11-10 with the first build skeleton. Aurora facts below were read
from the pinned submodule commit (`f77492f`, 2026-11-05), not from memory.

## Directory map

```
port/
  CMakeLists.txt        top of the PC build (cmake -S port -B build/pc)
  app/main.c            executable entry point; Aurora main loop
  extern/aurora/        git submodule, encounter/aurora (MIT)
  sdk/                  SDK stubs: entry points Aurora lacks, no real code yet
    include/mkgp2pc/stub.h    MKGP2PC_STUB() marker
    src/                one file per SDK library (ai.c, ax.c, exi.c, ...)
  platform/             our PC replacements for hardware Aurora does not cover
    include/mkgp2pc/platform.h
    audio/              hardware end under MusyX (AI / DSP / ARAM)
    triforce/           JVS, coin, backup RAM, card reader, media board
docs/port/              these notes
```

Rule of thumb: everything the port adds goes under `port/` or `docs/port/`.
`src/`, `include/`, `config/`, `tools/` and `configure.py` belong to upstream
and arrive by merge. That keeps `git merge upstream/main` conflict-free; the
skeleton touched no upstream file except the port section at the top of
`README.md` (already ours), and added `.gitmodules` and
`.github/workflows/port.yml`.

## Third-party code

| What | Where | Licence | How it is pinned |
|---|---|---|---|
| Aurora | `port/extern/aurora` (submodule) | MIT | submodule commit |
| Dawn, SDL3, nod, fmt, Dear ImGui, abseil, xxHash, zlib, libpng, FreeType, SQLite, zstd, Tracy | fetched by Aurora's CMake at configure time, not in this repository | their own (permissive) | versions fixed inside the Aurora commit |

Aurora is a submodule and not a copy so that upstream fixes come by moving one
commit, and so that fixes we need can go to Aurora itself. If we ever need
local Aurora changes, fork it and point the submodule at the fork; do not
patch files inside the submodule in place.

## Who provides which SDK library

"Game side" is the directory in `src/` holding the decompiled original.

| SDK area | Game side | PC build uses |
|---|---|---|
| GX, GD | `src/dolphin/gx` | Aurora (`aurora::gx`, `aurora::gd`) on Dawn/WebGPU |
| VI | `src/dolphin/vi` | Aurora (`aurora::vi`) |
| PAD, SI | `src/dolphin/pad`, `si` | Aurora (`aurora::pad`, `aurora::si`) on SDL3 |
| DVD | `src/dolphin/dvd` | Aurora (`aurora::dvd`), reads the disc image through nod |
| CARD | `src/dolphin/card` | Aurora (`aurora::card`) |
| MTX | `src/dolphin/mtx` | Aurora (`aurora::mtx`) |
| OS | `src/dolphin/os` | Aurora (`aurora::os`) for what it has; the rest in `port/sdk` |
| THP | `src/thp` | Aurora (`aurora::thp`) or the decompiled C, to be decided |
| AR / ARQ | `src/dolphin/ar` | Aurora for AR (ARAM as a host buffer); ARQ from the decompiled C or `port/sdk` |
| AI, DSP, AX | `src/dolphin/ai`, `dsp` | `port/platform/audio` |
| MusyX | `src/musyx/runtime` | decompiled C, with its hardware end in `port/platform/audio` |
| EXI | `src/dolphin/exi` | `port/sdk` stubs; device traffic handled in `port/platform/triforce` |
| am*, jvs*, ac*, dvdlow (Triforce) | `src/triforce` | `port/platform/triforce` |
| HSD (sysdolphin) | `src/sysdolphin/baselib` | decompiled C on top of Aurora GX |
| MSL C / C++ runtime, TRK, zlib | `src/MSL_C`, `Runtime.PPCEABI.H`, `TRK_MINNOW_DOLPHIN`, `zlib` | host libc / libstdc++ / zlib; not compiled |
| DB, base, demo, amcstubs, odemu* | `src/dolphin/...`, `src/odemuexi2` | dropped or stubbed in `port/sdk` |

The right-hand column is the plan, not a tested state. Only the first rows are
even linked today, and nothing calls them yet except `GXSetCopyClear`.

## Conventions

### SDK stubs (`port/sdk`)

See `port/sdk/README.md`. In short: one file per SDK library, every stub
starts with `MKGP2PC_STUB();`, first hit is logged, and the executable prints
how many distinct stubs were reached when it exits.

### Platform back ends (`port/platform`)

One subdirectory per device family, one source file per back end
(`audio/audio_null.c`, later `audio/audio_sdl.c`), all implementing the hooks
declared in `mkgp2pc/platform.h`. A `null` back end always exists so the game
can run without the device. Per-family notes and the list of devices are in
`port/platform/audio/README.md` and `port/platform/triforce/README.md`.

### Names

Port-only C identifiers start with `mkgp2pc_`, macros with `MKGP2PC_`, CMake
targets are `mkgp2pc::<name>`. Game and SDK names stay exactly as upstream
has them.

## Open questions for when game code joins the build

Not decided here on purpose; they depend on the type clean-up still running
upstream.

1. Header precedence. Both `include/dolphin` (ours, matching) and
   `port/extern/aurora/include/dolphin` (Aurora) exist. Aurora's must win for
   the libraries Aurora implements, because its structs differ; ours are still
   needed for AI, AX, EXI, am* and others. Expect a per-library include
   arrangement, not a single include path order.
2. 32-bit or 64-bit host. `readiness.md` recommends 32-bit little-endian
   first so struct layouts and HSD in-place relocation stay valid. Aurora's
   prebuilt Dawn package has to exist for that target, or Dawn is built from
   source (`AURORA_DAWN_PROVIDER=vendor`, slow).
3. CodeWarrior-only syntax (`asm` functions, `#pragma`, `__declspec`,
   `: 0x80001234` absolute addresses): a compatibility header force-included
   by the PC build, kept under `port/`.
4. Where the list of game sources lives: `port/cmake/game_sources.cmake`,
   grown unit by unit, with CI compiling exactly that list.
