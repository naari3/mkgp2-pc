#!/bin/bash
# Builds mkgp2-hires.exe (the maker of the high-resolution texture folder,
# 32-bit Windows, docs/port/hires_textures.md) with the i686 MinGW-w64
# compiler. One source file and two public domain headers; no game code and
# no library of the port.
#
#   port/hires/build.sh [output directory]     (default build/hires-tool)
#
# For Linux: g++ -std=c++17 -O2 -pthread -o mkgp2-hires port/hires/mkgp2_hires.cpp
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../../build/hires-tool}
mkdir -p "$OUT"
# large-address-aware: 4 GB instead of 2 on 64-bit Windows; the program also
# limits how many texels it works on at a time.
i686-w64-mingw32-g++ -std=c++17 -O2 -msse2 -mfpmath=sse -municode -static -Wl,--large-address-aware \
  -o "$OUT/mkgp2-hires.exe" "$HERE/mkgp2_hires.cpp" -lurlmon
ls -l "$OUT/mkgp2-hires.exe"
