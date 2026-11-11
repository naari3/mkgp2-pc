#!/bin/bash
# Builds mkgp2-gfx.exe (32-bit Windows, with rendering and sound) from the
# repository root, inside the container of port/docker/Dockerfile or on a
# Linux machine with the same packages. Steps and settings are those of
# docs/port/build.md ("32-bit Windows, with rendering"); this file only
# strings them together.
#
#   port/docker/build-win32.sh [output directory]
#
# The executable and the DLLs it needs end up in the output directory
# (default build/pc-gfx-win/dist).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
DIST=${1:-$ROOT/build/pc-gfx-win/dist}
DIRS="game sound ui util resource triforce sysdolphin"
DAWN_URL=https://github.com/naari3/mkgp2-pc/releases/download/dawn32-1155e0ed531126f33a1279afa029349651ca1c93/dawn-windows-x86.tar.gz
TOOLCHAIN=$ROOT/port/gfx/cmake/mingw-i686.cmake
MINGW_LIB=/usr/i686-w64-mingw32/lib

step() { printf '\n== %s\n' "$*"; }

# Library names Aurora links that MinGW-w64 spells differently or lacks.
EXTRA=$ROOT/build/mingw-libshim
mkdir -p "$EXTRA"
ln -sf "$MINGW_LIB/libdxgi.a" "$EXTRA/libDXGI.a"
if [ ! -e "$EXTRA/libcomsuppw.a" ]; then
  echo 'void mkgp2pc_comsuppw_empty(void) {}' > "$EXTRA/comsuppw.c"
  clang -target i686-w64-windows-gnu -c "$EXTRA/comsuppw.c" -o "$EXTRA/comsuppw.o"
  i686-w64-mingw32-ar rcs "$EXTRA/libcomsuppw.a" "$EXTRA/comsuppw.o"
fi

# Keep the host's Linux packages out of the cross build.
mkdir -p "$ROOT/build/win32-empty-pkgconfig"
export PKG_CONFIG_LIBDIR=$ROOT/build/win32-empty-pkgconfig

AURORA_ARGS=(
  -G Ninja
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_C_FLAGS=-msse2
  "-DCMAKE_CXX_FLAGS=-msse2 -Wno-c++11-narrowing -femulated-tls -I$ROOT/port/gfx/cmake/mingw-case"
  "-DCMAKE_EXE_LINKER_FLAGS=-L$EXTRA" "-DCMAKE_SHARED_LINKER_FLAGS=-L$EXTRA"
  -DAURORA_ENABLE_DVD=OFF -DAURORA_ENABLE_CARD=OFF -DAURORA_ENABLE_THP=OFF
  -DAURORA_CACHE_USE_ZSTD=OFF -DFT_DISABLE_ZLIB=ON
  -DAURORA_SDL3_PROVIDER=vendor
  -DAURORA_DAWN_PROVIDER=package "-DAURORA_DAWN_PACKAGE_URL=$DAWN_URL"
)

step "Aurora (standalone; link_census.py reads its symbol lists)"
cmake -S port/extern/aurora -B build/aurora-win32 "${AURORA_ARGS[@]}"
# Aurora's example program may fail to link; only the libraries are needed.
cmake --build build/aurora-win32 -- -k 0 || echo "(some Aurora targets failed; continuing with the libraries)"

step "MusyX"
cmake -S port/extern/musyx -B build/musyx-win32 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" "-DCMAKE_C_FLAGS=-D_Bool='unsigned long'" \
  -DMUSYX_ENABLE_SDL=OFF -DMUSYX_VERSION_PATCH=2
cmake --build build/musyx-win32

step "Game objects"
# shellcheck disable=SC2086
python3 port/tools/link_census.py $DIRS --gfx --os windows --link

step "mkgp2-gfx.exe"
cmake -S port/gfx -B build/pc-gfx-win/cmake "${AURORA_ARGS[@]}"
cmake --build build/pc-gfx-win/cmake --target mkgp2-gfx

step "Collect"
EXE=build/pc-gfx-win/cmake/mkgp2-gfx.exe
python3 port/tools/pe_check.py "$EXE"
mkdir -p "$DIST"
cp "$EXE" "$DIST/"
for dll in webgpu_dawn.dll dxcompiler.dll dxil.dll SDL3.dll libpng16.dll libzlib1.dll; do
  found=$(find build/pc-gfx-win/cmake -name "$dll" -print -quit)
  if [ -z "$found" ]; then
    echo "missing: $dll" >&2
    exit 1
  fi
  cp "$found" "$DIST/"
done
cp port/gfx/package/run-gfx.cmd port/gfx/package/KEYS.txt "$DIST/"
ls -l "$DIST"
