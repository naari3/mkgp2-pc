# mkgp2-gfx: the game with Aurora as its GX and VI (32-bit Linux, and 32-bit
# Windows cross-built with MinGW-w64).
#
# The game's objects do not come from CMake. port/tools/link_census.py --gfx
# compiles them (MWCC conventions applied in LLVM IR) and writes the link
# inputs to build/pc-gfx/gfx_link.rsp; this project adds Aurora and the glue
# in this directory and links. See docs/port/build.md.
cmake_minimum_required(VERSION 3.25)
project(mkgp2-gfx LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MKGP2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if (MINGW)
  set(_mkgp2_out pc-gfx-win)
else ()
  set(_mkgp2_out pc-gfx)
endif ()
set(MKGP2_LINK_RSP "${MKGP2_ROOT}/build/${_mkgp2_out}/gfx_link.rsp" CACHE FILEPATH "link inputs from link_census.py --gfx")
if (NOT EXISTS "${MKGP2_LINK_RSP}")
  message(FATAL_ERROR "${MKGP2_LINK_RSP} is missing. Run: python3 port/tools/link_census.py <dirs> --gfx --link")
endif ()

# The game has its own DVD, CARD and THP layers.
set(AURORA_ENABLE_DVD OFF CACHE BOOL "")
set(AURORA_ENABLE_CARD OFF CACHE BOOL "")
set(AURORA_ENABLE_THP OFF CACHE BOOL "")
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "")
add_subdirectory(../extern/aurora aurora EXCLUDE_FROM_ALL)

if (MINGW)
  # link_census.py --gfx --os windows compiles the glue itself (it has to
  # match names with and without the COFF underscore); see dawn_mingw.cpp
  # for the one file left here.
  add_executable(mkgp2-gfx dawn_mingw.cpp)
  # gdi32 / user32: the dump's window picture (shot_win32.c)
  target_link_libraries(mkgp2-gfx PRIVATE dawn::webgpu_dawn gdi32 user32)
  target_compile_options(mkgp2-gfx PRIVATE -femulated-tls)
else ()
  add_executable(mkgp2-gfx video_aurora.c gx_shim.cpp audio_musyx.c shot_win32.c port_ui.cpp port_menu.cpp debug_overlay.cpp)
  target_include_directories(mkgp2-gfx PRIVATE "${MKGP2_ROOT}/port/platform/include")
endif ()
target_link_libraries(mkgp2-gfx PRIVATE aurora::core aurora::gx aurora::vi)
target_link_options(mkgp2-gfx PRIVATE "@${MKGP2_LINK_RSP}")
set_property(TARGET mkgp2-gfx PROPERTY LINK_DEPENDS "${MKGP2_LINK_RSP}")
