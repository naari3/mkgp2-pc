# mkgp2-pc: port notes

## What this repository is

A native PC port of Mario Kart Arcade GP2 (`GNLJ82`) built from the source of
the matching decompilation, [naari3/mkgp2-decomp](https://github.com/naari3/mkgp2-decomp).
It began as a full-history copy of mkgp2-decomp `main` at `a66588b`
(2026-11-10) and merges from it regularly (steps in the top-level README).

Status (2026-11-10): build skeleton only. `port/` holds a CMake project that
links Aurora and opens a window; SDK stub and platform back end directories
exist with their conventions but only `null` back ends. No game source is
compiled for PC yet, and no file under `src/` or `include/` has been changed
for the port: that starts after upstream finishes its struct and data-name
clean-up.

| Document | Content |
|---|---|
| [`build.md`](build.md) | How to build, run and test the PC executable; updating Aurora |
| [`census.md`](census.md) | Measured: how much of the source a host compiler accepts, and what a link would need |
| [`64bit.md`](64bit.md) | Assessment: what assumes 4-byte pointers, counted, and the ways to a 64-bit build |
| [`layout.md`](layout.md) | Directory map, which SDK library comes from where, stub and back end conventions, open questions |
| [`readiness.md`](readiness.md) | Assessment of how far the decomp is from a port: measured gaps, comparable projects, staged plan (estimates) |
| [`widescreen.md`](widescreen.md) | Survey: what a 16:9 display needs (projection, culling, 2D layer, whole-screen effects), options |
| [`hires_textures.md`](hires_textures.md) | High-resolution 2D textures (fonts, sprites): the switch, how the folder is made from the user's disc image, the filters |
| [`launcher.md`](launcher.md) | The settings program `mkgp2-launcher.exe`: what it sets, the ini file, `MKGP2PC_KEYS`, how it is built |

## What goes where

| Work | Repository |
|---|---|
| Matching, asm to C at 100 %, symbol names, splits, data ownership | mkgp2-decomp, arrives here by merge |
| Type and header clean-up that keeps `main.dol` matching | mkgp2-decomp first, when practical |
| Non-matching C for asm-only functions, portability changes that break matching, second (PC) build target, platform layer, byte-swapping loaders, Triforce replacements | here |

## Rules that carry over from upstream

- Never commit a disc image, `main.dol`, game assets, compiler binaries or
  genuine Metrowerks MSL headers (`tools/fetch_msl.py` fetches those into
  `build/`).
- OpenGP is GPL-3. Do not copy its code or text. Facts (addresses, hardware
  behaviour) are fine.
- Upstream code is CC0. Third-party code brought in for the port (for example
  Aurora, MIT) keeps its own licence and goes under its own directory.

## Rules that do not carry over

These belong to mkgp2-decomp and are not used here:

- unit-claim issues and the one-TU-at-a-time claim etiquette
- the agent message board (`docs/agents/`, `tools/agent_board.py`) and the
  `n3` / `om` sides; the SessionStart hook that prints board messages comes
  along with the merge and can be ignored
- "land asm to C only at 100 %": a port needs working C, matching or not
- the progress dashboard, the `progress-data` branch and the Build workflow
  (guarded to run only in mkgp2-decomp, see `.github/workflows/build.yml`)

Do not open claims, post on the board, or comment on other contributors'
issues in mkgp2-decomp on behalf of work done here.
