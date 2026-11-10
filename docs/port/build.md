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

On Windows run the same commands from a "x64 Native Tools Command Prompt for
VS 2022" (or open `port/` as a CMake folder in Visual Studio). Dawn and SDL3
come as prebuilt DLLs there and are copied next to `mkgp2-pc.exe`.

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
