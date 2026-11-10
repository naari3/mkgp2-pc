# SDK stubs: Dolphin / Triforce SDK entry points the game calls that Aurora
# does not provide and that have no real PC replacement yet.
# Convention: docs/port/layout.md, "SDK stubs".
add_library(mkgp2pc_sdk STATIC
  src/stub_log.c
)
add_library(mkgp2pc::sdk ALIAS mkgp2pc_sdk)
target_include_directories(mkgp2pc_sdk PUBLIC include)
