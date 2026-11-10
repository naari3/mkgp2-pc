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
