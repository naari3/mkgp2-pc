#!/bin/bash
# Runs inside a WSL distribution that has docker: makes the high-resolution
# 2D texture folder from a disc image with port/hires/make_hires.py, in the
# container of port/docker/Dockerfile, and copies it to a Windows directory.
# Started by port/docker/wsl-hires.ps1; see docs/port/hires_textures.md.
#
#   wsl-hires.sh <ref> <disc image> <destination directory> <scale> [<path of the Windows clone>]
set -euo pipefail

REF=${1:-origin/main}
ISO=${2:?disc image}
DEST=${3:?destination directory}
SCALE=${4:-2}
WIN_REPO=${5:-}
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
if [ -n "$WIN_REPO" ]; then
  git remote get-url win >/dev/null 2>&1 || git remote add win "$WIN_REPO"
  git remote set-url win "$WIN_REPO"
  git fetch -q win '+refs/heads/*:refs/remotes/win/*'
fi
git checkout -q --detach "$REF"
echo "using $(git log --oneline -1)"

docker build -q -t "$IMAGE" port/docker
# Made on the Linux file system first (ten thousand small files), then copied.
OUT=build/hires
rm -rf "$OUT"
mkdir -p "$OUT" build/hires-cache
ISO_DIR=$(dirname "$ISO")
docker run --rm -u "$(id -u):$(id -g)" -v "$DIR:$DIR" -v "$ISO_DIR:/iso:ro" -w "$DIR" "$IMAGE" \
  python3 port/hires/make_hires.py --iso "/iso/$(basename "$ISO")" --out "$OUT" --scale "$SCALE" \
  --cache build/hires-cache

rm -rf "$DEST"
mkdir -p "$DEST"
cp -r "$OUT"/. "$DEST"/
echo "copied to $DEST"
