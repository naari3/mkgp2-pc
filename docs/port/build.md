# Building the PC port

The PC build is a CMake project under [`port/`](../../port). It is separate
from the matching build (`configure.py` + `ninja` at the repository root) and
needs none of its inputs: no disc image, no `main.dol`, no CodeWarrior.

Today it produces `mkgp2-pc`, an executable that starts Aurora, opens a
640x480 window and presents a cleared frame. No game code is compiled yet.

## Requirements

- CMake 3.25 or later, Ninja (or Visual Studio 2022), a C++20 compiler
  (MSVC, Clang or GCC)
- Git, with the Aurora submodule checked out:

  ```sh
  git submodule update --init port/extern/aurora
  ```

- Internet access on the first configure. Aurora downloads its dependencies
  (prebuilt Dawn and nod, sources of SDL3, fmt, Dear ImGui and others) with
  CMake `FetchContent`. Libraries already installed on the system are used
  instead where Aurora supports that.
- Linux only: the development packages SDL3 needs to build. On Ubuntu 24.04:

  ```sh
  sudo apt-get install build-essential pkg-config cmake ninja-build \
    libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev \
    libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
    libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libegl1-mesa-dev \
    libdbus-1-dev libibus-1.0-dev libudev-dev libpipewire-0.3-dev \
    libwayland-dev libdecor-0-dev liburing-dev
  ```

## Build and run

```sh
cmake -S port -B build/pc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/pc
build/pc/mkgp2-pc
```

`build/` is already git-ignored, and `build/pc` does not collide with the
matching build's `build/GNLJ82`.

### Windows

Checked 2026-11-10 on Windows with Visual Studio Community 2022 17.14
(MSVC 19.44), CMake 3.31.6 and Ninja 1.12.1 as shipped with Visual Studio:
configure about 95 s including downloads, build about 55 s, no source changes
needed.

Run the same commands from an "x64 Native Tools Command Prompt for VS 2022"
(that is, after `vcvars64.bat`):

```bat
cmake -S port -B build/pc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/pc
ctest --test-dir build/pc --output-on-failure
build\pc\mkgp2-pc.exe
```

Pitfall: `vcvars64.bat` appends Visual Studio's own CMake and Ninja to the
END of `PATH`. If an older CMake (below 3.25) is installed elsewhere it wins
and configure stops with "CMake 3.25 or higher is required". Check with
`where cmake`, and if needed put Visual Studio's copies first:

```bat
set "VSCMAKE=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"
```

Dawn, SDL3 and nod come as prebuilt packages. Their DLLs (`webgpu_dawn.dll`,
`SDL3.dll`, `nod.dll`, `dxcompiler.dll`, `dxil.dll`, `libpng16.dll`,
`zlib1.dll`) are copied next to `mkgp2-pc.exe`, which then starts from any
shell. Aurora picks D3D12. The user directory is `%APPDATA%\mkgp2-pc`.

Not tried: the Visual Studio generator, clang-cl, the VS 2026 toolset.
Dependencies print a few warnings (C4819 on Japanese-locale systems, C4244,
C4834); the port's own code prints none.

Options of the executable:

| Option | Effect |
|---|---|
| `--headless` | no window; render offscreen with the null GPU back end |
| `--frames N` | exit after N presented frames |

## The game code (bring-up build, no graphics)

The decompiled game does not go through CMake yet. `port/tools/link_census.py`
compiles every unit of the listed `src/` directories as a 32-bit program,
applies the MWCC conventions (docs/port/census.md) and links it with the
stand-ins under `port/nogfx/` and `port/platform/`:

```sh
python3 port/tools/link_census.py game sound ui util resource triforce sysdolphin --link
MKGP2_ISO=/path/to/the.iso build/pc-census/mkgp2-nogfx
```

Linux needs clang, `gcc-multilib` and `lib32z1-dev`. No window and no sound;
the log goes to stderr. `MKGP2PC_SCRIPT` feeds inputs
(docs/port/census.md section 16).

### 32-bit Windows

```sh
python3 port/tools/link_census.py game sound ui util resource triforce sysdolphin --os windows --link
```

cross-builds `build/pc-census-win/mkgp2-nogfx.exe` on Linux (clang with the
i686 MinGW-w64 runtime: `gcc-mingw-w64-i686-win32 g++-mingw-w64-i686-win32`).
The executable is static and large-address-aware; on Windows run it with
`MKGP2_ISO` set. What the mode does differently is described in
docs/port/census.md section 20. It has been linked but not run yet.

## The game code with rendering

```sh
python3 port/tools/link_census.py game sound ui util resource triforce sysdolphin --gfx --link
cmake -S port/gfx -B build/pc-gfx/cmake -G Ninja <32-bit settings> -DAURORA_DAWN_PROVIDER=package -DAURORA_DAWN_PACKAGE_URL=<url below>
cmake --build build/pc-gfx/cmake
MKGP2_ISO=/path/to/the.iso build/pc-gfx/cmake/mkgp2-gfx
```

The first command compiles the game objects and writes
`build/pc-gfx/gfx_link.rsp`; the CMake project `port/gfx` builds Aurora and
links both. `link_census.py --gfx` reads the symbol lists of a standalone
Aurora build in `build/aurora32` (same settings, source directory
`port/extern/aurora`), so that one has to exist first. The 32-bit settings
used on 64-bit Linux:
`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS="-m32 -msse2" -DCMAKE_CXX_FLAGS="-m32 -msse2 -Wno-c++11-narrowing" -DCMAKE_SYSTEM_PROCESSOR=i686 -DCMAKE_LIBRARY_ARCHITECTURE=i386-linux-gnu`
with `PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig`
and the i386 development packages of X11, Wayland, GL/EGL, udev, abseil,
freetype, sqlite3, png and zlib. Without a display it runs under Xvfb with
Mesa's software GL. `MKGP2PC_FRAMES=N` exits after N frames. What the build
does is described in docs/port/census.md section 21.

`port/docker/build-linux32.sh` runs all of this in order (standalone Aurora,
MusyX, game objects, `mkgp2-gfx`) and lists the Ubuntu 24.04 packages in its
header. Where release archives cannot be downloaded but git clones can, set
`MKGP2PC_DEPS=<dir>` holding clones named `fmt` (12.1.0), `imgui`
(v1.91.9b-docking), `sdl` (release-3.4.20), `tracy` (v0.14.1) and `xxhash`
(v0.8.3), and install `nlohmann-json3-dev`.

### 32-bit Windows, with rendering

Cross-built on Linux with clang and the MinGW-w64 runtime, like the
no-graphics Windows build. Linked, not run yet.

```sh
cmake -S port/extern/aurora -B build/aurora-win32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/port/gfx/cmake/mingw-i686.cmake <settings>
cmake --build build/aurora-win32        # the example program may fail to link; the libraries are what is needed
python3 port/tools/link_census.py game sound ui util resource triforce sysdolphin --gfx --os windows --link
cmake -S port/gfx -B build/pc-gfx-win/cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/port/gfx/cmake/mingw-i686.cmake <settings>
cmake --build build/pc-gfx-win/cmake    # mkgp2-gfx.exe
```

Settings: `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_FLAGS=-msse2
-DCMAKE_CXX_FLAGS="-msse2 -Wno-c++11-narrowing -femulated-tls -I$PWD/port/gfx/cmake/mingw-case"
-DAURORA_ENABLE_DVD=OFF -DAURORA_ENABLE_CARD=OFF -DAURORA_ENABLE_THP=OFF
-DAURORA_CACHE_USE_ZSTD=OFF -DFT_DISABLE_ZLIB=ON -DAURORA_SDL3_PROVIDER=vendor
-DAURORA_DAWN_PROVIDER=package -DAURORA_DAWN_PACKAGE_URL=<windows-x86 url below>`,
with `PKG_CONFIG_LIBDIR` pointing at an empty directory (otherwise the
host's Linux packages are found).

- `-femulated-tls`: the MinGW libstdc++ is built by GCC with emulated
  thread-local storage; clang's default for this target is native.
- `port/gfx/cmake/mingw-case/`: `Windows.h` and `Wbemidl.h` under the names
  Aurora includes them by, and the declaration of `SetThreadDescription`
  that MinGW-w64 11 lacks.
- Two library names Aurora links do not exist in MinGW: put a directory
  with `libDXGI.a` (a link to `libdxgi.a`) and an empty `libcomsuppw.a` on
  the linker path (`-DCMAKE_EXE_LINKER_FLAGS=-L<dir>`).
- The Dawn package is an MSVC-built DLL with a C interface;
  `port/gfx/dawn_mingw.cpp` supplies the one C++ function Aurora uses.
- No abseil, freetype or sqlite packages exist for MinGW. They build from
  source (`-DFETCHCONTENT_SOURCE_DIR_ABSEIL-CPP=...`, `_FREETYPE`,
  `_SQLITE3`; the sqlite directory holds the amalgamation `sqlite3.c`).
- The executable needs `webgpu_dawn.dll`, `dxcompiler.dll`, `dxil.dll` (from
  the Dawn package), `SDL3.dll`, `libpng16.dll` and `libzlib1.dll` (from the
  build tree's `_deps/*-build`) next to it.

In this mode `link_census.py` compiles the glue of `port/gfx` itself and
uses jumps instead of the linker's `--wrap` (the game's objects have plain
names, Aurora's the COFF underscore; see `link_gfx_windows`).

## Building the Windows executable on a Windows PC (WSL and docker)

The Windows executable is cross-built on Linux, so on a Windows PC the
build runs in a container inside WSL. Nothing is installed on Windows and
nothing in the WSL distribution changes apart from one clone and one docker
image. Needs: WSL 2 with a distribution in which `docker` works for the
default user (any Ubuntu; the build itself runs in Ubuntu 24.04 inside the
container).

```powershell
.\port\docker\wsl-build.ps1                      # builds origin/main
.\port\docker\wsl-build.ps1 -Ref win/my-branch   # a branch of this clone, committed work only
.\port\docker\wsl-build.ps1 -Distro Ubuntu-20.04 # a distribution other than the default
```

The result is `build\pc-gfx-win\dist` of the Windows clone: `mkgp2-gfx.exe`,
the six DLLs, `run-gfx.cmd`, `KEYS.txt` and `mkgp2-launcher.exe` (the settings
window, [launcher.md](launcher.md)) (the same set as the zip
described below). Start it with `run-gfx.cmd "path to the ISO"`, or set
`MKGP2_ISO` and run the executable.

What the pieces do:

- `port/docker/wsl-build.ps1` (Windows) calls `port/docker/wsl-build.sh` in
  WSL with the ref, the destination and the path of the Windows clone.
- `wsl-build.sh` keeps a clone in `~/src/github.com/naari3/mkgp2-pc` on the
  Linux file system (a build under `/mnt/c` is slow), fetches `origin` and
  the Windows clone (remote `win`), checks the ref out detached, builds the
  image `mkgp2-pc-win32` from `port/docker/Dockerfile` and runs
  `port/docker/build-win32.sh` in it as the WSL user. The log is
  `build/build-win32.log` in the WSL clone.
- `build-win32.sh` is the sequence of "32-bit Windows, with rendering" and
  "Windows cross build: the exact environment": Aurora, MusyX,
  `link_census.py --gfx --os windows --link`, `port/gfx`, `pe_check.py`,
  then the files are collected. It also runs on a plain Linux machine with
  the packages of the Dockerfile.

Measured 2026-11-11 (Ryzen 9 5900X, WSL 2, Ubuntu 20.04 host distribution,
docker 23.0.2): image 1.45 GB, WSL clone with all build trees 1.3 GB; first build from an empty
tree about 8 minutes including downloads; a rebuild with nothing changed
about 2 minutes. The image has clang 18.1.3, cmake 3.28.3,
`i686-w64-mingw32-gcc` 13-win32 and mingw-w64 11.0.1, the versions of the
cloud build machine.

Differences from the cloud build:

- The container lets Aurora's FetchContent download everything. So
  freetype is 2.14.3 (Aurora's pinned archive) instead of Ubuntu's 2.13.2,
  and abseil and sqlite come from Aurora's URLs instead of the Ubuntu
  source packages (abseil is the same release, 20240722.0).
- `clang-tools` is installed: CMake 3.28 scans SDL's C++ sources for
  modules and stops without `clang-scan-deps`.
- `link_census.py` finds SDL's headers in
  `build/aurora-win32/_deps/sdl-src/include` when
  `FETCHCONTENT_SOURCE_DIR_SDL` was not given.

Checked 2026-11-11 on Windows 11 (RTX 3060 Ti, D3D12): the executable built
this way starts with the Japanese image, opens its window and runs until
the frame limit (`MKGP2PC_FRAMES=200` and `900`). Observed: the process
then ends with exit code 0xC0000005 instead of 0, in both runs, after
Aurora's warning "Buffer mapping Aborted: Buffer was destroyed before
mapping was resolved". Assumption, not verified: the fault is in the
shutdown path after the frame limit and not specific to this build; the
cloud-built executable has not been run with `MKGP2PC_FRAMES` on Windows
for comparison.

## 32-bit Dawn packages

Aurora takes Dawn as a prebuilt package and upstream publishes 64-bit ones
only. `.github/workflows/dawn32.yml` (manual) builds the same Dawn revision
for 32-bit Windows and Linux and, with `publish`, attaches the packages to a
prerelease of this repository. For `AURORA_DAWN_REF`
`1155e0ed531126f33a1279afa029349651ca1c93`:

- https://github.com/naari3/mkgp2-pc/releases/download/dawn32-1155e0ed531126f33a1279afa029349651ca1c93-r2/dawn-windows-x86.tar.gz
- https://github.com/naari3/mkgp2-pc/releases/download/dawn32-1155e0ed531126f33a1279afa029349651ca1c93/dawn-linux-i686.tar.gz

Pass the one for the target as `-DAURORA_DAWN_PACKAGE_URL=<url>` when
configuring a 32-bit build. Not wired into the presets yet.

### The crash at `webgpu_dawn.dll` + 0xb6b58 (package without `-r2`)

Every crash record that ended at this offset (window size changes, exit
before 01791c5, and during play, the last one 2026-11-12 15:39 JST in build
6f362d2 with exception c0000005 reading address 000055c0) is one fault in
the first Windows package, a miscompilation by 32-bit MSVC:

- 0xb6b50 is `Result<ExecutionSerial, ErrorData>::Result(ExecutionSerial)`
  (`src/dawn/common/Result.h`). It reads its argument through a pointer and
  returns with `ret 4`.
- Its only two callers, at 0x13c2f2 and 0x16468a, are the last lines of
  `d3d::Queue::WaitForQueueSerialImpl` (`src/dawn/native/d3d/QueueD3D.cpp`,
  `return didComplete ? waitSerial : kWaitSerialTimeout;`) and of the D3D11
  queue's function of the same name. Both push the 64-bit serial itself
  (two words). The constructor then takes the low word of the serial as an
  address: the faulting address 0x55c0 was the serial being waited for.
- `ExecutionSerial` is `TypedInteger<..., uint64_t>`, declared
  `alignas(T)` (`src/utils/typed_integer.h`). 32-bit MSVC passes a class
  with an explicit alignment above 4 through a pointer to a copy (the other
  calls in the same function do that), but not when the argument is a
  conditional expression.

So in that package any wait for the GPU that does not return at once
crashes. `WaitForQueueSerialImpl` returns early when the serial is already
complete, which is why it only happened sometimes. Callers under D3D12:
`SwapChainD3D12.cpp` before reusing a back buffer whose last frame the GPU
has not finished (during play, when the GPU falls behind), the same file
when the swap chain is detached (size change), `ResidencyManagerD3D12.cpp`
when video memory is over budget, and queue destruction (exit). The D3D11
backend has the same fault at its own call.

The `-r2` package is built with `alignas(T)` removed from `TypedIntegerImpl`
on Windows (a step of the workflow); size and alignment of the class are
unchanged and it is passed like a plain 64-bit integer. Checked by
disassembling the `-r2` DLL: the D3D12 queue's function (slot 10 of the
`dawn::native::d3d12::Queue` vtable, 0x1013b150) now takes the serial by
value (`ret 0x14`) and stores the result in place, with no call. Not yet
run on Windows. Dawn's other
explicitly aligned classes passed by value (`VkHandle` in the Vulkan
backend, `Vector` in `Algebra.h`) were not examined.

## Test

```sh
ctest --test-dir build/pc --output-on-failure
```

One test, `smoke_headless`: starts the executable headless for three frames.
It needs no display or GPU, so it also runs in CI
(`.github/workflows/port.yml`, which runs only in this repository and only
when `port/` changes).

## Offline or restricted networks

Each fetched dependency can be pointed at a local checkout with
`-DFETCHCONTENT_SOURCE_DIR_<NAME>=<dir>` (names: `SDL`, `FMT`, `IMGUI`,
`XXHASH`, `TRACY`, ...), and Dawn / SDL3 / nod each have an
`AURORA_<DEP>_PROVIDER` switch (`vendor`, `system`, `package`). See
`port/extern/aurora/docs/building.md`.

## Updating Aurora

Aurora is pinned by the submodule commit. To move it:

```sh
git -C port/extern/aurora fetch origin
git -C port/extern/aurora checkout <commit>
git add port/extern/aurora
```

Rebuild and run the test before committing. Aurora's dependency versions are
pinned inside Aurora (`cmake/AuroraDependencyVersions.cmake`), so the
submodule commit fixes them too.

## Keyboard (rendering executable)

`port/gfx/video_aurora.c` maps the keyboard to the cabinet's controls while
the window has the focus: left / right = steering wheel, up or Z =
accelerator, down or X = brake, space or enter = item button (also "decide"
in the menus), 5 or C = coin, F1 / F2 = test / service button. Until a key
is used, `MKGP2PC_SCRIPT` may drive the inputs. Not tested with a real
keyboard yet (the cloud build machine has no input device); the same input
calls driven by the script steer and accelerate the kart.

## Magnetic card

`port/platform/triforce/card_reader.c` stands in for the cabinet's card unit
(reader/writer, printer and dispenser on the baseboard's second serial
port). The card is one file of 69 bytes, `card.bin` in the directory the
program is started from (`MKGP2PC_CARD_FILE` names another file).

- The game is started with its card service on. A new card is made by the
  game itself: choose to buy a card when it offers one; the file appears
  when the game writes the card.
- To play with the card, press **I** (script command `card`) when the game
  asks for it. The card is taken in the next time the game asks the unit to
  read, and the game hands it back (and the file is up to date) at the end
  of the play.
- A new card bought while `card.bin` exists keeps the old file as
  `card.bin.bak<n>`.
- What is printed on the card's face is not kept. Without a picture source
  for the camera (`MKGP2PC_CAMERA`, [camera.md](camera.md)) the stand-in
  turns the game's camera option off during the power-on checks.
- `MKGP2PC_SCALE=N` opens the window at N times 640x480. On Windows the
  window cannot be resized or maximized (`MKGP2PC_RESIZABLE=1` allows it
  again): every size change ended in `webgpu_dawn.dll` + 0xb6b58, on the
  render worker thread, below `Surface::Configure` called from
  `aurora::gfx::present::submit` (crash record of 2026-11-12 04:44 JST,
  build 2502a6b). The cause is a fault in the first 32-bit Dawn package
  (see "32-bit Dawn packages"); the crashes at exit (before 01791c5) and
  during play were the same one. With the `-r2` package resizing has not
  been tried on Windows yet, so the window stays fixed for now.
- `MKGP2PC_BACKEND=d3d12|d3d11|vulkan|opengl` asks Aurora for that backend
  instead of its own order. Not tried on Windows yet.
- `MKGP2PC_SHADOW_NEAREST=1`: the kart shadow is an image the game draws
  each frame (480x480 here, every kart within 300 units in it, so about
  40x90 texels a kart on the starting grid) and samples with GX_NEAR when
  magnified. Against the cabinet the port's shadow looked clearly more
  jagged (USER 2026-11-12); no hardware filter explaining that was found
  (copy filter 0,0,21,22,21,0,0, progressive output). As a deliberate
  departure the port magnifies images copied out of the frame buffer with
  GX_LINEAR; this variable gives the game's own setting back.
  `MKGP2PC_SHOW_COPY=1` (debugging) leaves the copied image on screen
  instead of the scene.
- `MKGP2PC_PIPELINE_WAIT`: Aurora compiles the pipeline for a draw's state
  on a worker thread the first time it is seen (backends with a pipeline
  thread, D3D12 among them) and can either leave the draw out until it is
  ready or make the draw wait. Leaving out made models appear one after
  another after a scene change; waiting made every pipeline first met in a
  race a stall. The port waits only around loading (the start, and 120
  frames after the game last read 128 KB or more from the disc in one
  frame); in a running race a draw with a new pipeline is left out for the
  few frames its compilation takes. `always` waits every time, `never`
  (or the older `MKGP2PC_ASYNC_PIPELINES=1`) never. census.md section 36.
- `MKGP2PC_NO_SHADER_WAIT=1`: by default the game is held at its start until
  every pipeline of Aurora's list is compiled (title bar: "preparing
  shaders, N left"); this starts it at once. The list is the user's own
  `pipeline_cache.db` plus `initial_pipeline_cache.db` from the executable's
  folder, if there is one (`port/tools/pipeline_list.py`,
  `port/tools/pipeline_sweep.sh`; census.md section 36).
- `MKGP2PC_STALL_MS=N`: when the game's own part of a frame runs longer
  than N milliseconds (150; `0` turns it off), a second thread writes every
  thread's stack to `dumps/stall_f<retrace>/stacks.txt` and a `stall:` line
  to the log, at most 20 times a run. Says where a stall is that the
  `spike:` line cannot explain.
- `MKGP2PC_SPIKE_MS=N`: frames longer than N milliseconds get a `spike:`
  line in the log with where the time went (default 25; 0 turns it off).
  Fields: comment above `spike_log` in port/gfx/video_aurora.c, background
  in census.md section 36.
- `MKGP2PC_NO_CACHE_CHECK=1` skips the start-up check of Aurora's two cache
  files (`dawn_cache.db`, `pipeline_cache.db` in the user directory). The
  check sets a damaged file aside as `<name>.broken`; without it Aurora runs
  the whole session without its shader cache (census.md section 36).
- `MKGP2PC_NO_SUPERVISOR=1` (Windows): the game runs in the process that
  was started, without the supervising process that restarts it on
  OSResetSystem (the test button); a reset then just ends the game.
  `MKGP2PC_RESET_SCRIPT_FILE=<path>`: the input script of the run after a
  reset (the scripts of the first run are dropped there). Both: census.md
  section 37, port/platform/runtime/reset.c.
- `MKGP2PC_CARD=0` starts the game with the baseboard DIP switch that turns
  the card service off, as all builds before this did. Input recordings
  (dumps) made by those builds need it to play back in step.
- `MKGP2PC_TRACE_CARD=1` logs every command the game sends to the unit.
- Format of the 69 bytes and a tool to decode or edit them: upstream
  `docs/wip/notes/card/` (`card_feasibility.md`, `mkgp2card.py`).


## Cabinet link (LAN play)

Copies of the program on one LAN, or on one PC, link up like cabinets do.
Give each a different cabinet number with `MKGP2PC_PCB_ID=1..4` (on Windows
`set MKGP2PC_PCB_ID=2` before `run-gfx.cmd`); they find each other by
themselves. `MKGP2PC_LINK=0` turns the link off. Ports (UDP 29564-29568), play
between two networks (`MKGP2PC_LINK_PEERS`), the other settings, the protocol and what has been checked
so far: [cabinet_link.md](cabinet_link.md).

- `MKGP2PC_HIRES=1`: high-resolution 2D textures from the folder `hires`
  (`MKGP2PC_HIRES_DIR` names another), `F8` switches while running. The
  folder is made from the user's disc image; see
  [hires_textures.md](hires_textures.md). Tried on Linux only.

## Pause, frame step and dumps (rendering executable)

For stopping on a frame that looks wrong and handing it over:

| Key | Does |
| --- | --- |
| `P` | pause / resume (the game's frame counter stops; the picture stays) |
| `O` | while paused: advance one frame |
| `F9` | write a dump; works running or paused |
| `F10` | full speed on / off: the game's frames run as fast as the machine can, the window is given about 60 of them a second and the drawing of the others is dropped before it reaches the rendering library (`mkgp2pc_skip_draw`, `port/gfx/gx_shim.cpp`); sound runs ahead. `MKGP2PC_FAST=1` starts that way. Presenting every frame without a limit ended inside `webgpu_dawn.dll` on Windows within seconds (2026-11-11, cause not established) |
| `Tab` | the same as `F10` while held |

A dump is a folder `dumps/<n>_f<frame>/` under the directory the executable
was started from (`MKGP2PC_DUMP_DIR` names another place; the folder is in
`.gitignore`):

- `frame.png`: the picture. Read back from Aurora's frame buffer where the
  graphics backend can (the log says "Asynchronous readback is unavailable"
  at start-up where it cannot); otherwise, on Windows, the window's own
  picture about 0.4 s later (`port/gfx/shot_win32.c`), which is the same
  frame when paused. When paused, `F9` steps the game one frame so that a
  frame is drawn for the picture. Not written on Linux without readback.
- `input.txt`: every input since the start, from the keyboard and from a
  script alike, as `<frame> <command>` lines in the `MKGP2PC_SCRIPT` syntax.
- `state.txt`: frame number, paused or not, the `MKGP2PC_*` settings.
- `frame_small.jpg`, `stacks.txt`, `memory.txt`, `mem1.bin.gz`,
  `image.bin.gz`, `symbols.txt.gz`, `gx_fifo.bin.gz`: a small copy of the
  picture, where every thread was, the game's memory, and the frame's draw
  commands. The same files (and a minidump on
  Windows) are written to `dumps/crash_<time>/` when the program dies. See
  [dumps.md](dumps.md).

Playing a dump back: `MKGP2PC_SCRIPT_FILE=<path to input.txt>` feeds the
recorded inputs at the recorded frames; add a line `<frame> pause` (or
`dump`) to stop where the dump was made. Checked on Linux: the same file
played with and without the frame limiter gives the identical picture at
frame 9000 (menus after a coin; a race has not been compared yet). What
could break it: anything the game takes from the clock rather than the
frame count. Seen so far: the same script reaches the race at different
frames on Linux without the limiter and on Windows with it, so loading
times count; two runs on the same machine with the same settings matched
(frame 9600, course introduction).

One picture is in flight at a time: a dump asked for while the previous
one's picture has not arrived yet is made on the first frame after it.

`state.txt` also lists the item shadow boards of the dumped frame (the small
textured quad the game draws under every item, `ItemObjectManager_RenderImpl`):
where each lies on a 640x480 picture according to the matrices the game
loaded, its scale and the constant alpha the game asked for (K1 is the one
that shadow uses). Comparing these lines with `frame.png` tells whether a
wrong-looking shadow is wrong in place and size or in blending. The record is
kept in `port/gfx/gx_shim.cpp` (wrappers of `GXSetProjection`,
`GXLoadPosMtxImm`, `GXSetTevKColor`, `GXCallDisplayList`).

## Sound (rendering executable)

The game's MusyX calls go to the PC target of the MusyX decompilation,
`port/extern/musyx` (github.com/AxioDL/musyx, CC0, pinned `717404d`; MusyX
2.0.2 like the game). It is built on its own, without its SDL adapter, and
`link_census.py --gfx` adds the library to the link:

    cmake -S port/extern/musyx -B build/musyx32 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=clang -DCMAKE_C_FLAGS="-m32 -malign-double -D_Bool='unsigned long'" \
      -DMUSYX_ENABLE_SDL=OFF -DMUSYX_VERSION_PATCH=2
    cmake --build build/musyx32
    # Windows: -B build/musyx-win32 -DCMAKE_TOOLCHAIN_FILE=$PWD/port/gfx/cmake/mingw-i686.cmake
    #          -DCMAKE_C_FLAGS="-D_Bool='unsigned long'" (same options otherwise)

`-D_Bool='unsigned long'`: MusyX's `bool` is 4 bytes in the game (MWCC, C89)
and `_Bool` in a C11 build; the game tests the whole register of a result.

`port/gfx/audio_musyx.c` is the device layer: the link wraps `sndInit`, and
an SDL audio stream (32 kHz stereo) pulls from `sndPCRender`. MusyX's
critical section is tied to the game's interrupt lock. To check without a
sound device: `SDL_AUDIODRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=out.raw`
(16-bit stereo, 32 kHz).

State: sound effects are produced (the logo jingle was verified in the
written file). Background music is silent so far.

### Check before handing out a Windows executable

`python3 port/tools/pe_check.py file.exe`. The Windows loader refuses an
image whose sections do not start at multiples of SectionAlignment ("This
app can't run on your PC", `Access is denied` from cmd); GNU ld writes such
an image without complaint when a linker script aligns an output section
to less than a page. The data layout scripts (`mwcc_data_layout.py`) align
their sections to 0x1000 for that reason (they used 32 until 2026-11-11, and
no executable built before that could start on Windows).

### Windows cross build: the exact environment (2026-11-11)

Machine: Ubuntu 24.04, clang / lld 18.1.3, cmake 3.28.3, ninja 1.11.1,
`i686-w64-mingw32-gcc` 13-win32 (mingw-w64 11.0.1, binutils 2.41.90).
Packages: `clang lld llvm cmake ninja-build python3 git zip pkg-config
gcc-mingw-w64-i686-win32 g++-mingw-w64-i686-win32 mingw-w64-i686-dev
binutils-mingw-w64-i686` (plus `gcc-multilib lib32z1-dev` for the Linux
builds; `link_census.py` reads `/usr/lib32/libc.so.6`, `libm.so.6`,
`libz.so.1` for the list of host C names on Windows too).

Sources given to Aurora's FetchContent (git clones at these tags):
fmt 12.1.0, imgui v1.91.9b-docking, SDL release-3.4.20, tracy v0.14.1,
xxHash v0.8.3, zlib-ng 2.3.3 (as `ZLIB`), libpng v1.6.58; and from the
Ubuntu 24.04 source packages: abseil 20240722.0, freetype 2.13.2, sqlite
3.45.1 (`./configure && make sqlite3.c`; the directory given holds
`sqlite3.c sqlite3.h sqlite3ext.h`).

`build/mingw-libshim`: `libDXGI.a` is a link to
`/usr/i686-w64-mingw32/lib/libdxgi.a`; `libcomsuppw.a` is
`i686-w64-mingw32-ar rcs` of one object compiled from an empty function.

Two trees are configured with the same options: `build/aurora-win32` from
`port/extern/aurora` (first; `link_census.py --gfx --os windows` reads
Aurora's libraries, SDL's include path and `_deps/zlib-build/libzlib.dll.a`
from it), then, after `link_census.py ... --gfx --os windows --link` wrote
the link inputs, `build/pc-gfx-win/cmake` from `port/gfx`. nlohmann_json
is the one dependency still fetched from the network at configure time.

    PKG_CONFIG_LIBDIR=<empty dir> cmake -S port/gfx -B build/pc-gfx-win/cmake -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/port/gfx/cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_FLAGS=-msse2 \
      -DCMAKE_CXX_FLAGS="-msse2 -Wno-c++11-narrowing -femulated-tls -I$PWD/port/gfx/cmake/mingw-case" \
      -DCMAKE_EXE_LINKER_FLAGS=-L$PWD/build/mingw-libshim -DCMAKE_SHARED_LINKER_FLAGS=-L$PWD/build/mingw-libshim \
      -DFT_DISABLE_ZLIB=ON -DAURORA_ENABLE_DVD=OFF -DAURORA_ENABLE_CARD=OFF -DAURORA_ENABLE_THP=OFF \
      -DAURORA_CACHE_USE_ZSTD=OFF -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=vendor \
      -DAURORA_DAWN_PACKAGE_URL=https://github.com/naari3/mkgp2-pc/releases/download/dawn32-1155e0ed531126f33a1279afa029349651ca1c93-r2/dawn-windows-x86.tar.gz \
      -DFETCHCONTENT_SOURCE_DIR_FMT=... _IMGUI _SDL _TRACY _XXHASH _ZLIB _PNG _ABSEIL-CPP _FREETYPE _SQLITE3

MusyX for Windows: the cmake line in "Sound" with
`-DCMAKE_TOOLCHAIN_FILE=$PWD/port/gfx/cmake/mingw-i686.cmake` into
`build/musyx-win32`.

Package (no script): a folder `mkgp2-gfx-win32` with `mkgp2-gfx.exe`
(from `build/pc-gfx-win/cmake`), `SDL3.dll`, `libpng16.dll`, `libzlib1.dll`
(from `_deps/*-build` there), `webgpu_dawn.dll`, `dxcompiler.dll`,
`dxil.dll` (from the Dawn package), `run-gfx.cmd`, `KEYS.txt` (both in
`port/gfx/package`); zipped.
Run `port/tools/pe_check.py` on the exe first.
