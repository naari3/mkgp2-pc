#!/bin/bash
# Builds mkgp2-launcher.exe (the settings program, 32-bit Windows) with the
# i686 MinGW-w64 compiler. No game code and no library of the port goes into
# it, so it builds on its own in a few seconds.
#
#   port/launcher/build.sh [output directory]     (default build/launcher)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../../build/launcher}
mkdir -p "$OUT"
(cd "$HERE" && i686-w64-mingw32-windres launcher.rc -O coff -o "$OUT/launcher.res")
i686-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static \
  -finput-charset=UTF-8 -fexec-charset=UTF-8 \
  -o "$OUT/mkgp2-launcher.exe" "$HERE/launcher.c" "$OUT/launcher.res" \
  -lcomctl32 -lcomdlg32 -lshell32
rm -f "$OUT/launcher.res"
ls -l "$OUT/mkgp2-launcher.exe"
