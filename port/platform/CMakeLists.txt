# Platform layer: PC replacements for hardware Aurora does not cover.
# One subdirectory per device family; see docs/port/layout.md.
add_library(mkgp2pc_platform STATIC
  platform.c
  audio/audio_null.c
  triforce/triforce_null.c
)
add_library(mkgp2pc::platform ALIAS mkgp2pc_platform)
target_include_directories(mkgp2pc_platform PUBLIC include)
target_link_libraries(mkgp2pc_platform PUBLIC mkgp2pc::sdk)
