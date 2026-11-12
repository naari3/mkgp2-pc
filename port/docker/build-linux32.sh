#!/bin/bash
# Builds mkgp2-gfx (32-bit Linux, with rendering and sound) from the
# repository root on a 64-bit Ubuntu 24.04 machine. Steps and settings are
# those of docs/port/build.md ("The game code with rendering", "Sound");
# this file only strings them together, like build-win32.sh does for
# Windows.
#
#   port/docker/build-linux32.sh
#
# Packages (i386 architecture enabled with `dpkg --add-architecture i386`):
#   clang clang-tools lld llvm cmake ninja-build pkg-config gcc-multilib
#   g++-multilib lib32z1-dev, and the :i386 packages libx11-dev libxext-dev
#   libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev
#   libxtst-dev libxkbcommon-dev libgl1-mesa-dev libegl1-mesa-dev
#   libgl1-mesa-dri libudev-dev libwayland-dev libabsl-dev libfreetype-dev
#   libsqlite3-dev libpng-dev zlib1g-dev libasound2-dev libvulkan-dev
#   mesa-vulkan-drivers; xvfb to run without a display.
#
# The executable is build/pc-gfx/cmake/mkgp2-gfx.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
DIRS="game sound ui util resource triforce sysdolphin"
DAWN_URL=https://github.com/naari3/mkgp2-pc/releases/download/dawn32-1155e0ed531126f33a1279afa029349651ca1c93/dawn-linux-i686.tar.gz

step() { printf '\n== %s\n' "$*"; }

export PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig

AURORA_ARGS=(
  -G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  "-DCMAKE_C_FLAGS=-m32 -msse2"
  "-DCMAKE_CXX_FLAGS=-m32 -msse2 -Wno-c++11-narrowing"
  -DCMAKE_SYSTEM_PROCESSOR=i686 -DCMAKE_LIBRARY_ARCHITECTURE=i386-linux-gnu
  -DAURORA_ENABLE_DVD=OFF -DAURORA_ENABLE_CARD=OFF -DAURORA_ENABLE_THP=OFF
  -DAURORA_ENABLE_TESTS=OFF -DAURORA_CACHE_USE_ZSTD=OFF
  -DAURORA_SDL3_PROVIDER=vendor
  -DAURORA_DAWN_PROVIDER=package "-DAURORA_DAWN_PACKAGE_URL=$DAWN_URL"
)
# Where release archives cannot be downloaded (git clones still can):
# MKGP2PC_DEPS=<dir> with clones fmt, imgui, sdl, tracy, xxhash at the tags
# listed in docs/port/build.md; nlohmann-json3-dev from the system.
if [ -n "${MKGP2PC_DEPS:-}" ]; then
  AURORA_ARGS+=(
    "-DFETCHCONTENT_SOURCE_DIR_FMT=$MKGP2PC_DEPS/fmt"
    "-DFETCHCONTENT_SOURCE_DIR_IMGUI=$MKGP2PC_DEPS/imgui"
    "-DFETCHCONTENT_SOURCE_DIR_SDL=$MKGP2PC_DEPS/sdl"
    "-DFETCHCONTENT_SOURCE_DIR_TRACY=$MKGP2PC_DEPS/tracy"
    "-DFETCHCONTENT_SOURCE_DIR_XXHASH=$MKGP2PC_DEPS/xxhash"
  )
fi

step "Aurora (standalone; link_census.py reads its symbol lists)"
cmake -S port/extern/aurora -B build/aurora32 "${AURORA_ARGS[@]}"
# Aurora's example program may fail to link; only the libraries are needed.
cmake --build build/aurora32 -- -k 0 || echo "(some Aurora targets failed; continuing with the libraries)"

step "MusyX"
cmake -S port/extern/musyx -B build/musyx32 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang "-DCMAKE_C_FLAGS=-m32 -malign-double -D_Bool='unsigned long'" \
  -DMUSYX_ENABLE_SDL=OFF -DMUSYX_VERSION_PATCH=2
cmake --build build/musyx32

step "Game objects"
# shellcheck disable=SC2086
python3 port/tools/link_census.py $DIRS --gfx --link

step "mkgp2-gfx"
cmake -S port/gfx -B build/pc-gfx/cmake "${AURORA_ARGS[@]}"
cmake --build build/pc-gfx/cmake --target mkgp2-gfx
ls -l build/pc-gfx/cmake/mkgp2-gfx
