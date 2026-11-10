# Source fixes the port needs from mkgp2-decomp

Changes that have to be made upstream because they edit matched sources. All
of them should leave the MWCC objects byte-identical. Port-side shims
(`port/compat`) cannot cover them. Find the affected files with

    python3 port/tools/compile_census.py game -v

Strike an item out when it has been merged from upstream.

## Done

- `operator new(unsigned long)` to `size_t` (upstream `393e3ab`, 2026-11-10).
- Items 1 to 4 below (upstream `fe3eca4`, 2026-11-10), except
  `VolumeCalibration.cpp:145`: `clCab::Update` is asm with no C side at all.
  Off MWCC `__DSPRegs[]` / `__DIRegs[]` in `CardCreate_Page.cpp` are `extern`
  and need storage from the port if anything uses them.

## Open

Items 1 to 9 were all merged on 2026-11-10 (upstream `da0081e`); the text is
kept for reference. Item 10 is open.

1. Absolute-address variables written `T name : 0xCC008000;`
   (`CardCreate_Page.cpp`, `ConfettiField.c`, `SpriteHandlePool.c`,
   `SuikaBall_801313D0.cpp`, `EnmyFaceEffect.cpp`): use the existing
   `AT_ADDRESS(addr)` macro from `include/dolphin/types.h`.
2. 19 game files carry a private copy of the MWCC `va_list` struct and
   `#define va_start(ap, fmt) ((void)fmt, __builtin_va_info(&ap))`
   (`Debug.c`, `DebugTextOutput.c`, `DrawText.c`, `PushPrompt.c`,
   `CardPrinterChecker_Draw.c`, `misc3d/fn_80167C60.cpp` and 12 siblings,
   `miyazakiconsole/fn_8017FF10.c`): include `<stdarg.h>` instead, or guard
   the private copy with `#ifdef __MWERKS__`.
3. `asm void fn(...);` prototypes left outside the `NON_MATCHING` guard
   (`CSSceneGetTitle_8014ADB4.cpp:449`, `RaceHud_80147470.cpp:440`,
   `SceneList_80157940.cpp:217`, `SceneList_80157EA8.cpp:229`,
   `VolumeCalibration.cpp:145`, `fn_80157DE0.cpp:208`): under `NON_MATCHING`
   the prototype must not say `asm`.
4. Parameters named `this` in `.c` files that are C++ on PC
   (`SceneRender.c:32`, `auto_800A8F4C_block.c:132`);
   `extern "C" static inline` (`mapeffect/EffectFA_Ctor.cpp:10`).
5. Value-less `return;` in non-void functions (matching_rules §54):
   `ItemHandler_Star.c:78`, `Item_OrbitAnchorKart.c:26`,
   `NokoNoko_CheckHitAndKill.c:66`, `sprite/fn_801A1798.c:8469`. C++ rejects
   it. Needs `#ifdef __MWERKS__ return; #else return <what r3 holds>; #endif`,
   decided per function.
6. One symbol declared with different types or language linkage inside one
   TU (needs a person): `g_carObjects` (`TaMode_FrameUpdate_private.h:57`),
   `g_pInputState` (`clFlowKart_Init.c:199`), `clFlowCourse_data_8049B388`
   (`clFlowCourse.cpp:74`), `g_ccClass` (`Scene_8015BF18.cpp:5`),
   `operator new` linkage (`DNBress_breath.cpp:16`, `clItemBox.cpp:17`),
   C-vs-C++ redeclarations (`ItemSelect.c:160`, `PCBCheckRecv.c:2329`,
   `ShadowBillboard.c:1010`, `clMiyoshiCardCreate.c:176`).
7. Calls that do not match the callee's prototype in the same TU ("no
   matching function" once the file is C++): `BiribiriLand.c:339`,
   `RivalRun.c:703`, `Sci2Card_BuildRetryReceiptLine.c:3924`,
   `SetCourseParams.c:2735`, `VSMode_FrameUpdate.c:3725`, `fn_80125948.c:233`,
   `ReverseChallenge_Dtor.inc:106`,
   `tornado_fragments/auto_TornadoEffect_ApplyI_text.inc:65`.
8. Types used before any declaration: `EffectSteeringResetState`
   (`TmpActionShake.c`, `TmpActionSplit.c`, `TmpSplit.c`),
   `EffectActionResetState` (`TmpActionVibrate.c`), `Driver`
   (`include/game/types/EffectBus.h:6`). These are fragments included by
   another file; if they are never compiled alone they should not end in
   `.c`, or the PC build needs a list of them.
9. `src/sysdolphin/baselib` (added 2026-11-10, upstream `fe3eca4`; 45 of 53
   files compile on PC with the HSD include set and `MUST_MATCH`):
   - a function declared non-static in its header and then defined
     `static` (MWCC accepts, C compilers do not): `cobj.c:306`
     `makeProjectionMtx`, `pobj.c:976` `HSD_PObjSetMtxMark`, `texp.c:153`
     `HSD_TExpFree`, `tobj_anim.c:54` `HSD_TObjAddAnim`, `tobj_nm.c:189`
     `HSD_TObjAnim`, `video.c:130` `HSD_VIGetXFBDrawEnable`. Either drop the
     header declaration or guard the `static` with `#ifdef __MWERKS__`.
   - `initialize.c:357`: `HSD_ObjDumpStat` conflicts with its earlier
     declaration.
   - `debug.c`: `HSD_SaveContext` is PowerPC asm; the port will supply its
     own `debug.c`, nothing to do upstream unless a guard is easy.
10. Objects one unit owns but other units reach through a second,
    address-derived name (added 2026-11-10, upstream `da0081e`). Example:
    `src/triforce/jvsio.c` has `static JvsTx tx;` and
    `src/triforce/jvsio_access.c` returns `&jvsio_bss_80683280.result`. Under
    MWCC both are address 0x80683280. On PC they are two objects, the second
    one never written, and the JVS state machine reads zeros. The bring-up
    link currently stubs 317 such names (315 in `src/game`, mostly
    `<Unit>_rodata_<ADDR>` / `_data_` / `_bss_` externs that land inside
    another unit's range, plus the two jvsio ones). List them with
    `python3 port/tools/link_census.py ... --link` and the classification
    snippet in `docs/port/census.md` section 11. Each needs one real
    definition with one name (non-static, declared in a header) or, for
    string and constant pools, a named array that both units use. Until then
    the port keeps a hand table (`STATIC_ALIASES` in `link_census.py`).
    The other 705 stubbed objects are `.bss` / `.sbss` that no C file
    defines at all; zero-filled storage of the `symbols.txt` size is the
    right definition for those, so they are not a problem for now.
11. Calls with fewer arguments than the callee takes (added 2026-11-10,
    upstream `da0081e`). `src/game/GameBoot_Init.cpp:50` calls
    `fn_80288574(0, &lbl_80598A60)` through the two-argument prototype in
    `include/game/GameBootPrivate.h:50`, but `src/triforce/ambackup_access.c`
    defines `fn_80288574(u32 chan, void* data, BackupConfig* cfg)` and
    dereferences `cfg`. The two lines before the call fill
    `GameBoot_Init_bss_805A5EB0` (8-byte id, size 0x6A20, type 0x11), which is
    that `BackupConfig`: the original passes it as the third argument and the
    match only holds because the register still has it. On PC the third
    argument is garbage and the game crashes there. Wanted: the real third
    argument at this call, and a sweep for the same pattern (a caller's local
    prototype with fewer parameters than the definition). The same sweep
    would catch the reverse (more arguments than the definition takes), which
    is harmless on PowerPC and on cdecl x86 but not on every ABI.
    Tool: `python3 port/tools/arity_census.py` (compiles every unit to LLVM
    IR and compares each direct call with the definition). At upstream
    `0ba4382` it lists 130 caller/callee pairs with fewer arguments than the
    definition (53 callees, 110 calling files), e.g.
    `PCBCheckRecv.c: DisplayContext_Init()` right after `buf = Alloc(0xD8)`
    (the argument is whatever r3 holds), `SoundMgr_PlaySE_Positional` called
    with 3 of 5, `Terrain_RaycastWall` with 0 of 4. For each pair either the
    call lacks arguments or the definition has parameters it never had;
    both need the real signature in one shared prototype.
12. Bit-fields (added 2026-11-10, upstream `0ba4382`). MWCC on PowerPC
    allocates bit-fields from the most significant bit, x86 compilers from
    the least significant. 144 bit-field members in 18 files of `src/game` /
    `include/game` are affected wherever the same storage is also read as a
    whole integer or through a second view, for example
    `src/game/PCBCheckRecv.c:2275` `union SlotFlags { u16 half; struct { u16
    local:1; u16 reserved:5; u16 id:3; u16 blocks:7; } ids; ... }` with
    `SlotId()` = `(half >> 7) & 7` and `SetSlotId()` = `ids.id = id`: on PC
    the id written is not the id read, `PCBComm_Init` takes the wrong branch
    and crashes. Wanted: every run of adjacent bit-fields declared through a
    macro that reverses the member order off MWCC, e.g.
    `BITFIELDS4(u16 local:1, u16 reserved:5, u16 id:3, u16 blocks:7)` =
    the members in this order under `__MWERKS__` and in reverse order
    otherwise (pad to the full storage unit first, or the reversed run starts
    at the wrong bit). That makes the integer value of the storage unit the
    same on both. Files: `grep -rlE ':\s*[0-9]+\s*;' src/game include/game`.
    Byte order of multi-byte fields on the wire (PCB link packets, card data)
    is a separate matter and stays with the port.
    Harder case in the same union: a second view made of `u8` bit-fields
    over the `u16` (`bits`): there the two bytes also trade places on a
    little-endian host, so the PC order is second byte first, each byte
    reversed (`blocks:7, idLow:1` then `low:2, ack:1, card:1, rx:1, tx:1,
    idHigh:1, local:1`). `union PacketHead { u32 word; struct { RoleByte
    role; u8 remaining[3]; } bytes; }` is the same kind (the role byte is the
    top byte of `word` on PowerPC).
13. Data byte order at the load sites (added 2026-11-10, upstream
    `0f0bec5`). The disc files are big-endian and the game reads them through
    native structs; a few values are little-endian in the file and swapped by
    hand. The port converts a file in place once it is in memory
    (`port/platform/data/data_loaded.c`, `mkgp2pc_data_loaded(kind, data,
    size)`), and needs to be called at the right moment. Wanted:
    - a hook macro, empty under MWCC, at every place where a file has just
      been read (and inflated, if it was compressed) and before the game
      looks inside or turns offsets into pointers:
      `PORT_DATA_LOADED(kind, pointer, size)` ->
      `mkgp2pc_data_loaded(kind, pointer, size)` off MWCC. First site:
      `src/game/TEXLoadPalette.cpp`, before `RelocatePalette(*out)`, kind 1
      (TPL). Kinds are numbered in `port/platform/include/mkgp2pc/data.h`;
      propose a number per format (HSD archive, sprite / animation `.bin`,
      course path table, ...) and I add the converter.
    - hand-written swaps of little-endian file values through one macro
      (`LE32(x)`: swap under MWCC, identity on a little-endian host), e.g.
      the size word in `TEXLoadPalette.cpp` (`((x >> 24) & 0xff) | ...`).
    - for each format, the struct types the game overlays on the file (names
      and headers), so the converter can be written from them. The loaders
      are few: `DVDLoad_ReadFileTimed.c`, `DVDLoad_ReadChunkHandle.c`,
      `DVDLoad_OpenAllocReadRelocSync.c`, `DVDFile_LoadSync.c`,
      `CourseData_LoadPathTable.cpp`, `TEXLoadPalette.cpp`, plus the
      sysdolphin archive loader. On the disc: 5,358 `.tpl`, 1,918 `.dat`
      (215 MB), 684 `.dsp`, 640 `.bin`, MusyX `pool/proj/samp/sdir`.
14. Pointers to member functions (added 2026-11-10; done upstream in
    `d0d86cd`, `include/game/types/pmf.h`). MWCC's are 12
    bytes, `{ long thisDelta; long vtableOffset; union { void *func; long
    vptrOffset; } }` with `vtableOffset == -1` for a non-virtual function
    (the layout `__ptmf_scall` in `src/Runtime` reads). Clang's and GCC's are
    8 bytes, `{ ptr, adj }`. The sources mix both views of the same bytes:
    the tables are written as data in MWCC's layout
    (`void *CardTaskWelcome_StateInit_data_803FE5F0[3] = { 0, -1, fn }` in
    `src/game/autodata/data_803FE480.c`) and read through a real
    pointer-to-member type (`CardStep step;` then `(self->*self->step)()` in
    `CardTaskChecks.cpp:49`). Off MWCC the object is also 4 bytes shorter, so
    every later member moves. First failure: the boot sequence calls 0 from
    `CardTaskWelcome_TickCounter`.
    Size: 24 pointer-to-member types in 15 files, about 10 call sites, a few
    `&Class::Method` constants (`clItemBox.cpp`).
    Wanted, in one shared header, unchanged under MWCC:
    - the type: `PMF_TYPE(Name, Class, Ret, Args)`; off MWCC a 12-byte struct
      in the layout above (a template carrying the signature is fine);
    - the call: `PMF_CALL(obj, pmf)(args)`; off MWCC adds `thisDelta`, and
      calls `func` when `vtableOffset < 0`, else the entry at
      `*(void **)(this + vptrOffset) + vtableOffset` (objects point at the
      start of MWCC-layout vtables in the port build, see census section 13);
    - the constant: `PMF_OF(&Class::Method)`; off MWCC it can be built at run
      time from the host's own `{ ptr, adj }` for non-virtual functions.
15. Byte-array data read as wider types (added 2026-11-10; done upstream in
    `67b6237` and `962b6d1`). The
    generated data units define 877 objects (37 KB) as `unsigned char
    name[N] = { 0x3F, 0x80, ... }`, and the code reads floats, ints and
    structs out of them through casts. The bytes are in the console's order,
    so every such read is wrong on a little-endian host. First failure:
    `clFlowClass_Update` copies `ClassAnims { int ids[3]; }` from
    `clFlowClass_Update_rodata_8039A5A0 + 0x3FC` (`rodata_80399830.c:702`);
    the animation ids come out byte-swapped, fail the `id < 0x1ec` check in
    `Sprite_SetupAnim`, the sprites lose their animation and the class
    select screen never leaves state 0. The same blob starts with floats
    (1.0, 0.7, 0.9, ...).
    Wanted: these objects defined with the element types the code reads
    (`float`/`int`/`short` arrays or a struct), as was done for the other
    initialised data. Where only the element width is known, an array of
    32-bit words written as numbers is already enough for the port. The
    largest ones: `lbl_80331698` (1,344), `lbl_802EB4E0` (1,284),
    `ItemObject_Tick_rodata_8032E060` (1,236), `lbl_80330D10` (1,216),
    `clFlowClass_Update_rodata_8039A5A0` (1,128), `ClSound_rodata_80379100`
    (1,108). List them with
    `grep -c "char .*\[[0-9]*\] = {" src/game/autodata/*.c`.
16. `src/game/TransitionEffect.cpp` (added 2026-11-10; done upstream in
    `55ea6d8`; new with
    upstream `6b7996e`). The destructors of `teSingleton<clScreenEffect>`
    and `TransitionOwner<...>` are explicit specializations defined at the
    end of the unit (lines 319 onwards), after `clScreenEffect` has already
    used them (line 112). Standard C++ rejects a specialization that is first
    declared after its implicit instantiation, so the unit does not compile
    with clang and the screen transitions fall back to generated stubs.
    Wanted: the specializations declared before the first use, for example
    `template <> teSingleton<clScreenEffect>::~teSingleton();` and the
    `TransitionOwner` ones right after the templates, with the definitions
    staying where they are, if that keeps the unit matching.
17. CPU reads of vertex buffers in `src/sysdolphin/baselib/pobj.c` (added
    2026-11-10; open; low priority, needed once models are drawn). The port
    leaves vertex buffers, display lists and index lists in the console's
    byte order because the GX layer reads them that way. Three helpers read
    vertex data on the CPU for shape animation: `get_shape_vertex_xyz`,
    `get_shape_normal_xyz`, `get_shape_nbt_xyz` (`memcpy` of `f32`s and
    `((u16 *) src_base)[i]`, `((s16 *) src_base)[i]`). Wanted: those reads
    through macros that are the plain read under MWCC and a byte-swapping
    read elsewhere, e.g. `PORT_BE16(p)`, `PORT_BE_F32(p)` in a shared header.
    Everything else in `pobj.c` and all of `fobj.c` already reads byte by
    byte and needs nothing.
18. `include/game/GpPleaseInitPrivate.h` (added 2026-11-10; done upstream in
    `c72769e`). This is
    the next stop of the menu: the scene after the round select (flow 37,
    `clFlowGpPlease`) is a generated stub because `GpPleaseInit.cpp` fails
    its own size check on the host. `GpPanelBase` ends with two `unsigned
    char` members at 0x84 and 0x85 and two bytes of padding; the host puts
    `short text` of `GpLabelPanel` into that padding (0x86), MWCC puts it at
    0x88. Wanted: the padding spelled as a member, `unsigned char
    pad86[2];` at the end of `GpPanelBase` (the rule now in
    `docs/notes/rtti_classes.md`).
19. Functions defined `void` whose callers use a return value (added
    2026-11-10; done upstream in `3a5fce1`). On the console the value is whatever the function
    left in r3; on the host it is garbage. First failure: the attract-mode
    race loads its course, `CECollision::CECollision` stores the result of
    `CollisionFile_FixupPointers(self->data)` (defined
    `void CollisionFile_FixupPointers(unsigned int file_base)`, declared
    `void *` by the caller) and `Collision_RegisterSpecialFaces` then follows
    a garbage pointer. Fifteen such functions in the units the port links
    (definition `void`, some caller declares a result):
    `Allocator_Identity_VoidVoid`, `CollisionFile_FixupPointers`,
    `CourseData_GetDefaultPathKey_Safe` (4 callers), `DrawEffect_SetU32CC`,
    `HSD_MemCallback4`, `NamCam_LoadImage`, `TCPConn_LogError` (8 callers),
    `Vtable_CallSlot2` (the scene update result in `MainGameLoop`),
    `fn_8014D4F0`, `fn_80169DF0`, `fn_801A1128`, `fn_801A1174`,
    `fn_801E6594`, `fn_801E93A0`, `fn_80241C08`.
    Wanted: the definition returning the value the callers read (often the
    first argument or the result of its last call), in whatever form keeps
    the match (a macro that is empty under MWCC is fine), or the callers
    corrected where the value is not really used.
20. Arrays made with `new T[n]` and destroyed by an explicit
    `__destroy_new_array(p, dtor)` (added 2026-11-10; open). MWCC's `new[]`
    puts a 16-byte header {element size, count} in front of the array and
    `__destroy_new_array` reads it. The host compiler's `new[]` writes its
    own 4-byte count instead, so the explicit call reads garbage sizes and
    frees a wrong pointer. Two call sites, both in
    `src/game/Scene_8014E0C4.cpp` (lines 3947 and 5546, element destructor
    `dtor_800869D0`, the 0x230-byte text drawer); the first one crashes when
    the attract mode leaves the title for the demo race. Wanted: the element
    type as a class with its destructor and `delete[]` at these sites (then
    both ends come from the same compiler), or the allocation side written
    with the explicit `__construct_new_array` as well.

