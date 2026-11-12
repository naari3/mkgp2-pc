#!/bin/bash
# Runs inside a WSL distribution that has docker: makes the high-resolution
# 2D texture folder from a disc image with port/hires/make_hires.py, in the
# container of port/docker/Dockerfile, and copies it to a Windows directory.
# Started by port/docker/wsl-hires.ps1; see docs/port/hires_textures.md.
#
#   wsl-hires.sh <ref> <disc image> <destination directory> <scale> [<path of the Windows clone> [<mode> <work directory>]]
#
# Mode "export" writes the sprites' source pictures to <work directory>/src
# instead (for an outside enlarger, wsl-hires.ps1 -Ai); mode "import" makes
# the folder with the sprites taken from <work directory>/up.
set -euo pipefail

REF=${1:-origin/main}
ISO=${2:?disc image}
DEST=${3:?destination directory}
SCALE=${4:-2}
WIN_REPO=${5:-}
MODE=${6:-make}
WORK=${7:-}
DIR=${MKGP2PC_WSL_DIR:-$HOME/src/github.com/naari3/mkgp2-pc}
IMAGE=mkgp2-pc-win32

if [ ! -f "$ISO" ]; then
  echo "no such disc image: $ISO" >&2
  exit 1
fi
if [ ! -d "$DIR/.git" ]; then
  mkdir -p "$(dirname "$DIR")"
  git clone https://github.com/naari3/mkgp2-pc "$DIR"
fi
cd "$DIR"
git fetch -q origin
# The Windows clone is only needed for a branch that exists there alone
# (-Ref win/...). Fetching it for nothing fails when that clone is a git
# worktree: its .git file holds a Windows path that WSL cannot follow.
if [ -n "$WIN_REPO" ] && [ "${REF#win/}" != "$REF" ]; then
  git remote get-url win >/dev/null 2>&1 || git remote add win "$WIN_REPO"
  git remote set-url win "$WIN_REPO"
  git fetch -q win '+refs/heads/*:refs/remotes/win/*'
fi
git checkout -q --detach "$REF"
echo "using $(git log --oneline -1)"

docker build -q -t "$IMAGE" port/docker
ISO_DIR=$(dirname "$ISO")
run() {
  docker run --rm -u "$(id -u):$(id -g)" -v "$DIR:$DIR" -v "$ISO_DIR:/iso:ro" ${WORK:+-v "$WORK:/work"} -w "$DIR" "$IMAGE" \
    python3 port/hires/make_hires.py --iso "/iso/$(basename "$ISO")" "$@"
}

if [ "$MODE" = export ]; then
  rm -rf "$WORK/src"
  mkdir -p "$WORK/src"
  run --export-sprites /work/src
  exit 0
fi

# Made on the Linux file system first (ten thousand small files), then copied.
OUT=build/hires
rm -rf "$OUT"
mkdir -p "$OUT"
if [ "$MODE" = import ]; then
  run --out "$OUT" --scale "$SCALE" --import-sprites /work/up
else
  run --out "$OUT" --scale "$SCALE"
fi

rm -rf "$DEST"
mkdir -p "$DEST"
cp -r "$OUT"/. "$DEST"/
echo "copied to $DEST"
