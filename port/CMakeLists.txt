# mkgp2-pc: PC build of Mario Kart Arcade GP2.
#
# This tree is self-contained on purpose. Nothing here is read by the matching
# build (configure.py / ninja at the repository root), and nothing here edits
# src/ or include/, so merges from mkgp2-decomp never touch it.
#
#   cmake -S port -B build/pc -G Ninja
#   cmake --build build/pc
#
# See docs/port/build.md.
cmake_minimum_required(VERSION 3.25)
project(mkgp2-pc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Build out of tree, for example: cmake -S port -B build/pc")
endif ()

get_filename_component(MKGP2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# --- Aurora (GX / VI / PAD / DVD / CARD / OS / MTX), MIT, git submodule ------
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/CMakeLists.txt")
  message(FATAL_ERROR
    "port/extern/aurora is empty. Run: git submodule update --init port/extern/aurora")
endif ()
# The game reads its files through DVD, so the nod-backed DVD layer is on.
set(AURORA_ENABLE_DVD ON CACHE BOOL "Enable DVD implementation backed by nod")
# Aurora's own unit tests are not ours to run (and pull in googletest).
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "Enable Aurora tests")
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

# --- Our own layers ----------------------------------------------------------
add_subdirectory(sdk)       # SDK entry points Aurora does not provide
add_subdirectory(platform)  # audio back end, Triforce board services

# --- Executable --------------------------------------------------------------
# Game sources (../src/game and friends) are NOT compiled yet. They join this
# target unit by unit once they build with a host compiler; list them in
# cmake/game_sources.cmake when that starts.
add_executable(mkgp2-pc app/main.c)
target_link_libraries(mkgp2-pc PRIVATE
  mkgp2pc::platform
  mkgp2pc::sdk
  aurora::core
  aurora::gx
  aurora::main
  aurora::vi
  aurora::pad
  aurora::dvd
  aurora::card
  aurora::mtx
  aurora::os
)
# Windows: put the Dawn / SDL3 DLLs next to the executable (no-op elsewhere).
include(extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
aurora_copy_runtime_dlls(mkgp2-pc)

# Aurora forces BUILD_TESTING off for its dependencies, so use our own switch.
option(MKGP2PC_ENABLE_TESTS "Register the port's CTest tests" ON)
if (MKGP2PC_ENABLE_TESTS)
  enable_testing()
  # Starts without a window or GPU and renders a few frames offscreen.
  add_test(NAME smoke_headless COMMAND mkgp2-pc --headless --frames 3)
endif ()
