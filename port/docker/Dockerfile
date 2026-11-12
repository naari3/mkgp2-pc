# Build environment for the 32-bit Windows executable (mkgp2-gfx.exe),
# cross-built on Linux: clang with the i686 MinGW-w64 runtime.
# The three Python packages are for port/hires/make_hires.py
# (docs/port/hires_textures.md).
# Usage: docs/port/build.md, "Building the Windows executable in a container".
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git curl zip unzip python3 pkg-config \
      cmake ninja-build make \
      clang clang-tools lld llvm \
      gcc-multilib g++-multilib lib32z1-dev \
      gcc-mingw-w64-i686-win32 g++-mingw-w64-i686-win32 \
      binutils-mingw-w64-i686 mingw-w64-i686-dev \
      python3-numpy python3-pil python3-xxhash \
    && rm -rf /var/lib/apt/lists/*

# The repository is mounted from the host and built as the host user.
RUN git config --system --add safe.directory '*'
ENV HOME=/tmp
WORKDIR /work
