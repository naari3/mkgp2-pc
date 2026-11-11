#!/bin/bash
# Runs inside a WSL distribution that has docker: keeps a clone of this
# repository on the Linux file system, builds mkgp2-gfx.exe in the container
# of port/docker/Dockerfile and copies the result to a Windows directory.
# Started by port/docker/wsl-build.ps1; see docs/port/build.md.
#
#   wsl-build.sh <ref> <destination directory> [<path of the Windows clone>]
#
# <ref> is what to build: origin/main, or win/<branch> for a branch of the
# Windows clone (fetched as the remote "win" when its path is given).
set -euo pipefail

REF=${1:-origin/main}
DEST=${2:-}
WIN_REPO=${3:-}
DIR=${MKGP2PC_WSL_DIR:-$HOME/src/github.com/naari3/mkgp2-pc}
IMAGE=mkgp2-pc-win32

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
git submodule update --init port/extern/aurora port/extern/musyx
echo "building $(git log --oneline -1)"

docker build -q -t "$IMAGE" port/docker
mkdir -p build
LOG=build/build-win32.log
if docker run --rm -u "$(id -u):$(id -g)" -v "$DIR:$DIR" -w "$DIR" "$IMAGE" \
     port/docker/build-win32.sh > "$LOG" 2>&1; then
  grep -E '^== |: OK \(' "$LOG"
else
  tail -40 "$LOG"
  echo "build failed; full log: $DIR/$LOG" >&2
  exit 1
fi

if [ -n "$DEST" ]; then
  mkdir -p "$DEST"
  cp build/pc-gfx-win/dist/* "$DEST/"
  echo "copied to $DEST"
fi
