# port/nogfx

Bring-up harness, not part of the shipped PC build.

`python3 port/tools/link_census.py --link` links the decompiled game as a
32-bit Linux executable with no graphics and every missing SDK function
stubbed. The files here give that executable just enough of a GameCube to
get through boot: memory at the console's addresses, an arena, time,
interrupts as no-ops. Aurora provides most of this in the real build
(`port/CMakeLists.txt` does not compile this directory); what Aurora lacks
moves to `port/sdk` once it has a proper implementation.
