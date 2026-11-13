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
    `__destroy_new_array(p, dtor)` (added 2026-11-10; done upstream in
    `72ea434`). MWCC's `new[]`
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
21. Text stored as numbers inside word and pointer arrays (added
    2026-11-10; done upstream `2d64875` and `ff5aee2`, `TEXT_WORD(...)` in
    `include/game/types/text_word.h`). The mirror image of request 15: objects typed as
    `void *[]`, `int[]` or `unsigned int[]` (usually because some of their
    words are relocated pointers) that also hold strings, written as 32-bit
    numbers. On the host the bytes of each word come out reversed. First
    failure: `lbl_8037D7DC` in `src/game/autodata/rodata_8037D61C.c:83` is
    `void *const [9] = { 0x65665F73, 0x745F6469, ... }`, i.e.
    "ef_st_dino_sazanami.dat" followed by three pointers; the demo race asks
    the disc for "s_feid_ts_onnaza.ima" forever.
    Wanted: such objects as structs with `char` members for the text.
    A scan for arrays (no string literal in the initialiser) with two or more
    consecutive hex words whose four bytes are all printable ASCII or
    trailing NULs finds 233 objects in 39 files, among them `lbl_80356D60`
    (46 words in a row), `Tornado_FlightTick_rodata_8032FE8C` (30),
    `lbl_803DB630` (12), `DebugOverlay_GetDaytimeLabel_data_8040E1C4` (11),
    `lbl_8039DFC4` (10); some hits are floats that happen to look like text.
22. Constructor called with its float and integer arguments in another
    order than the definition (added 2026-11-10; done upstream `31f6a3c`,
    `ARGS_ORDER((mwcc), (host))` in `include/game/types/arg_order.h`; the
    port's rename lookup ignores parameter order for these). `RaceSceneC` does
    `new RaceCamera(lbl_806D80C0, 0x200, lbl_806D8184, 0x200, lights)`
    (`Scene_8014E0C4.cpp:4965`, float, int, float, int, pointer); the
    function behind it is `clShadow::clShadow(int width, int height,
    HSD_LObj *lobj, float far, float near)` (`Camera_Create.cpp:90`). On the
    console floats and integers travel in separate registers, so the order
    between the two kinds does not matter; on the host it does. Wanted: the
    caller's class declared with the definition's order, and the earlier
    argument-order sweep extended to constructors that reach their
    definition through `tools/extab_user_renames.json`.
23. Built-in HSD descriptors typed as words, and arguments passed through
    r3 between two calls (added 2026-11-10; done, upstream 08f7eb7).
    (a) `g_courseDataInstance` (`src/game/autodata/data_8040E780.c`) is the
    light descriptor that `GetCourseDataPtr()` returns and
    `HSD_LObjLoadDesc()` reads. It is a `void *[7]` whose third and fourth
    words are really `u16 flags = 0x000D; u16 attnflags; GXColor color`. On
    a little-endian host `flags` reads 0, the shadow camera
    (`Camera_Create.cpp:95`) cannot tell what kind of light it has and
    asserts. Wanted: the object as a struct with those field widths, and a
    look at other static HSD descriptors typed the same way.
    (b) Six callers write `GetCourseDataPtr(); renderTarget =
    HSD_LObjLoadDesc();` with no argument (`BiribiriLand.c:827`,
    `RivalRun.c:1984`, `JumpDistanceMode_Init.c:366`,
    `ReverseChallenge_Init.inc:172`, `NokoNoko_801287F8.cpp:1001`,
    `SuikaBall_801313D0.cpp:1154`); the console keeps the first call's
    result in r3, a host does not. Wanted: the nested call, or a host form.
24. Shift-JIS patterns of the card name check typed as words and as byte
    strings (added 2026-11-10; done, upstream b5862a5, which also adds
    `NAMECODE_STRCMP` and friends for the port to define). `NameCheck_ScanBannedWords`
    (`src/game/NameCheck_CardName.c:3445`) holds names as `NameCode`
    (`unsigned short`, one Shift-JIS character each) and searches them with
    the byte functions `strstr` / `strcmp`. Its patterns in
    `src/game/autodata/sdata2_806D3060.c` are `const unsigned int X =
    0x82A60000;` (`lbl_806D46E4` and its neighbours) or `const char[8]`
    (`lbl_806D4714`, `lbl_806D4720`, `lbl_806D4740`). On a little-endian
    host the first kind begins with a zero byte, `strstr` matches the empty
    pattern everywhere and the scan runs off its buffer. Wanted: every
    operand that is compared with `NameCode` data as a `NameCode` array.
    Left for the port side: names with codes below 0x100, whose byte view
    starts with a zero on the console only.
25. Float and integer parameters declared in another order than defined,
    C names (added 2026-11-10; done, upstream 1d181d0). Same class as item 22, found with
    `port/tools/arg_order_census.py`, which compares every declaration in
    the host IR with the definition. I = integer or pointer, F = float:
    `fn_802090F0` (defined IFFFII in `anon_80208E30_nm1.cpp`, declared
    IIIFFF in `auto_ONKARTHIT_block.c`), `CarObject_ApplyInput` (IIFFF,
    declared IFFFI in eight units), `CarObject_ApplyDriftBoost` (IFI, IIF
    in three), `InputCmd_PushSample` (IFFFI, IIFFF in one),
    `KartMovement_PhysicsStep` (IIFFF, IFFFI in one), and `AnimState_Init`
    (second parameter a pointer, declared float in two units). The first
    stops the race scene while it builds the player's kart.
26. Calls that pass fewer arguments than the definition takes (added
    2026-11-10; done, upstream f294028). The console leaves an argument register alone until
    it is written, so `EnemyRunType1_Init(void **self)` calls
    `EnemyRun_BaseInit(self)` and the base constructor still finds the four
    further arguments its own caller put in r4 to r8; a host reads stack
    garbage (the race scene stops while it builds the first AI kart). The
    second list of `port/tools/arg_order_census.py`, kept in
    `docs/port/arg_count_census.txt`: 52 functions, among them destructors
    called without their flag and `ItemObject_GetGroundTypeAt` (five
    parameters, three passed in about sixty units).
27. A scalar local whose address goes to a callee that writes a vector
    (added 2026-11-10; done, upstream 22e9c48). `PathManager_UpdateAll.c` declares
    `float u, t;` and passes `&u` as the `outPos` of
    `Math_Segment2DIntersect`, which writes `outPos[0..2]`. On the host
    the two extra floats land on the local `finish`.
28. Definitions that take fewer parameters than their callers pass and
    forward the rest in the registers (added 2026-11-10; done, upstream
    0a6f6ef and 5938fbd).
    `clItemBoxResponder_CmdGetWorldPos(clItemBoxResponder *self)` calls
    `self->GetWorldPos()`; its caller passes `(responder, &x, &y, &z)` and
    the virtual function stores through those three pointers. The third
    list of `port/tools/arg_order_census.py` (in
    `docs/port/arg_count_census.txt`) has every function called with more
    arguments than it takes; each needs a look at whether its body forwards
    them.
29. Initialized data defined as bytes and read through a wider type
    (added 2026-11-10; done, upstream 1ef6e6d and 92d47ab). `lbl_806D97E8` is `const unk8[6]` in
    `src/game/autodata/sdata2_806D7998.c` and
    `src/game/sprite/fn_801A1798.c` reads it as a structure of three
    `short`; the bytes keep the console's order, so the race queued voice
    id 0x6E02 for 0x026E (frame 1411 of the scripted race). A scan of the
    byte-typed, non-zero autodata objects against the `extern` declarations
    of other units also finds `VSMode_FrameUpdate_data_806CF148` (one byte,
    read as `unsigned int`) and `lbl_802EB4E0` (1,284 bytes, `NamCamPool`).
    Wanted: the definition in the type its users read.
30. Float-returning functions declared `void` by a caller, and the
    reverse (added 2026-11-10; done upstream in `1f1cec5`, `7459791`,
    tool `tools/port_abi/return_class.py`). On 32-bit x86 a float result is
    returned on the x87 register stack. A caller that takes the function
    for `void` never pops it; after eight such calls the stack is full and
    every float load produces NaN. `fn_8019EA8C`
    (`src/game/kartmove/carobject.cpp`, returns `float`) is declared `void`
    in `KartMovement_PhysicsStep.cpp` and called seven times per step: the
    player kart's position was NaN after its first physics step, and with
    it the race camera (no 3D picture in the race).
    `KartEffectFadeTransit_GetActiveValue` is the reverse (integer result,
    declared `float` in `auto_ONKARTHIT_block.c`).

31. A struct passed by value through a function-pointer cast to a function
    whose definition takes a pointer (added 2026-11-10; the one instance,
    `StlList_RemoveByValueField`, done upstream in `8cfa617`). The census
    `tools/port_abi/byvalue_struct.py` found 369 more places of the same
    kind in plain prototypes. Those are NOT converted upstream: the port
    passes every struct argument by address (`port/tools/mwcc_byvalue.py`),
    which is what MWCC does, so both spellings meet.
32. The seven MusyX units that did not compile on the host (added
    2026-11-11; done upstream in `b210d84`). Not linked by the port in the
    end: the MusyX PC target in `port/extern/musyx` answers `snd*`.
33. Shift-JIS text in char strings read two bytes at a time through
    `*(unsigned short *)p` (added 2026-11-11; done upstream in `3eec64d`,
    `include/game/types/sjis_word.h`: `SJIS_WORD`, `SJIS_WORD_LOAD`, defined
    for the host in `port/compat/mwcc_compat.h`). On a little-endian host
    the lead-byte test `& 0x8000` and the glyph lookup saw the bytes
    swapped: the name "マリオ" was drawn as "マリI". Places:
    `TexMgr_SetTexture.cpp`, `TitleNameHud_nm1.cpp`, `RankPanel_80144430.cpp`.
34. `fn_8020A734` (`Tween::Set(float from, float to, short frames)`) is
    declared `(void *, int frames, float from, float to)` in about 30 units
    (added 2026-11-11). The host callee reads the frame count as `from`:
    every HUD tween set through those declarations was garbage (the
    remaining-time digits at a fraction of their size). The port's census
    did not follow a C++ definition to its renamed C name; fixed
    (`port/tools/arg_order_census.py`), this is the only function it lists.
35. `src/game/CObjProject.c` calls `GXProject(*matrix, projection, viewport,
    outX, outY, outDepth, x, y, z)` through a declaration in another order
    than the SDK's `GXProject(x, y, z, mtx, pm, vp, sx, sy, sz)` (asked
    2026-11-11). On the host the pointers arrive where the floats are read;
    the attract mode crashes in `GXProject`. Also asked: a sweep for local
    declarations of library functions (SDK, MSL, HSD) whose float/integer
    order differs from the real prototype, which the port's census does not
    cover.
36. `src/game/dlistutil/*.c` and `include/game/DListBoundsPrivate.h` read
    and write GX stream data on the CPU through host-order casts (asked
    2026-11-11; done upstream in `7d2320a`,
    `include/game/types/gx_data.h`). Display lists and vertex buffers are the one part of a
    model file the port leaves big-endian, because the GX layer consumes
    them in that form. Three kinds of access, 13 units:
    the vertex count of a primitive (`*(unsigned short *)(p + 1)`, also
    inside `CmdSize` for command 0x10), a 16-bit vertex index read or
    written inside a display list (`*(unsigned short *)(p + entry->offset)`,
    `SetIndex` / `GetIndex`), and position floats read from a vertex buffer
    (`v = d->array; ... v[0], v[1], v[2]`). On a little-endian host a count
    of 3 is read as 768, the walk leaves the list and ends in
    `OSPanic("miyazakishare.cpp", 259, "Unknown DLCommand")` (seen twice
    while course 8 loads, from `Caster::Caster`); the bounds it computed
    before that are garbage too. Wanted: access macros in the style of
    `SJIS_WORD` (for example `GXDATA_U16(p)`, `GXDATA_SET_U16(p, v)`,
    `GXDATA_F32(p)`), plain dereferences under MWCC, defined for the host
    in `port/compat/mwcc_compat.h`; and a search for other CPU-side readers
    of display lists or vertex buffers outside `dlistutil`.
37. The cabinet-link packet header is read and written through two views
    that only agree on a big-endian machine (asked 2026-11-11; done
    upstream in `224e450`, `include/game/types/pcb_link.h`).
    `PCBCommBroadcast.c` writes it through
    `union BroadcastHeader { struct { HeaderByte first; unsigned char
    second; StateByte state; StatusByte status; } bytes; struct {
    BITFIELDS3(unsigned int upper:9, unsigned int scene:8, unsigned int
    lower:15) } word; }`; the readers use `(hdr.word >> 15) & 0xFF`
    (`PCBCheckRecv.c`, about 12 places, one spelled
    `*(unsigned int *)(PCBCheckRecv_bss_805A6154 + index * 0xA3C) >> 15`;
    `fn_801D1890.cpp` `slot->flags >> 15`) next to byte fields of the same
    header (`hdr.bits.team`, `readyBit1`, `StateByte`). On the console the
    scene number is the low 7 bits of byte 1 and the top bit of byte 2. On
    a little-endian host the 32-bit view puts it in bit 7 of byte 1 and
    bits 0-6 of byte 2, on top of the `StateByte` flags, so the scene a
    cabinet reads back for itself is garbage:
    `PcbSync_AllKartsInValidSelectState()` never becomes true and the
    attract loop stays on its last screen ("ただいまカメラは使えません")
    for ever. Wanted: the scene number through accessors defined on bytes
    for the host (for example `PCB_HDR_SCENE(p)` / `PCB_HDR_SET_SCENE(p,
    v)`, the present expressions under MWCC), and the same for the other
    multi-byte bit-field units that share a union with byte views in the
    link code: `ClassHeader.half` (`upper:7, wins:4, low:5`) in
    `PCBCommBroadcast.c`, the `half` views in `PCBCheckRecv.c` (`upper:5,
    track:7, lower:4`; `upper:6, id:3, lower:7`; `local:1, reserved:5,
    id:3, blocks:7`) and `PCBCheckProcess.c` (`upper:6, id:3, lower:7`).
    Only 17 `BITFIELDSn` units in the game are wider than a byte, so the
    rest (`clsound/fn_801913B0.c`, `misc3d/fn_80167*.cpp`, `clFlowRound.c`,
    `FaceEffect.cpp`, `Scene_8014E0C4.cpp`) is worth one look for the same
    mix of views.
38. Tables written as word arrays (`void *x[]`, `unsigned int x[]`) whose
    readers take single bytes or 16-bit halves of a word (asked
    2026-11-11; done upstream in `2012310`, `9087544`, `c530fd9`; what was
    left and why is in upstream `docs/notes/port_abi.md`, "The sweep"). `(void *)0x8000000` is the byte 0x08 followed by three
    padding bytes on the console and the word 0x08000000 on the host,
    whose first byte is 0. Seen: the item-in-hand animation table
    `lbl_80422064` in `fn_80116DD0.c`, read as `AnimEntry { char type;
    unsigned char pad[3]; void *data; }` by `AnimState_InitEntries`: the
    end marker 13 is never found and the second round of the attract mode
    crashes there. Same shape: `lbl_80421E34` / `lbl_80421EE8`
    (`fn_801152B0.c`), the two tables `TwinSpriteOverlay.c` passes to
    `AnimStatePool_Alloc` (`autodata/data_80419FB8.c`), and
    `g_itemClassTable` (`autodata/data_8041C9D4.c`, `void *[1390]` with
    words like `0x1010001`, `0xFF000000`, read through `ItemClassEntry`
    with `flag0` / `flag1` bytes). 4397 initialisers of the form
    `(void *)0xNN000000` exist, most of them in
    `RaceScene_GetPerspFovyForCup.c` (2816) and
    `autodata/rodata_80383CF8.c` (1189), then `data_8041C9D4.c`,
    `data_8041A4F8.c`, `KartItem_FlushRender.c`, `CourseEnvironment.cpp`.
    Wanted: these tables defined with the struct type their readers use
    (as request 15 did for byte arrays holding numbers, the other way
    round), found by comparing each data definition with the types other
    units declare for the same symbol or cast it to.
39. Data defined with one element width and read with another (asked
    2026-11-11). Seen: `lbl_806D7F70`, defined `const int [2] = { 255,
    -65536 }` in `autodata/sdata2_806D7998.c` ("element type read off the
    values"), read as `unsigned char [8]` by `Scene_8014E0C4.cpp:4596` as
    two background colours (0,0,0) and (255,255,255) for `fn_8002CFB8`.
    On the host the first colour reads (255,0,0): the pure red background
    behind the attract-mode captions. `port/tools/data_width_census.py`
    lists every symbol whose definition and some `extern` declaration
    disagree on the scalar element width (707 symbols, list in
    `docs/port/data_width_census.txt`). 552 of them are the banned-word
    entries of `NameCheck_CardName.c` (`short [4]` against `char []`).
    Others that look like real byte tables: `KartDriver_InitFull_data_803F5928`
    (`unsigned int [30]` of packed bytes), `CardCreditFlow_rodata_803EEF00`,
    the `NokoNokoChallenge_GetCoinDivisor_rodata_*` group. A float pool
    declared `char []` by a reader is harmless while the reader casts back
    to `float` / `Vec`, and wrong when the pool also holds strings or byte
    tables. Wanted: the definition typed the way the readers read it
    (bytes as bytes, strings as strings), at least for every entry whose
    readers take bytes.
40. Derived classes whose first member the host compiler puts into the
    tail padding of the base (asked 2026-11-11). MWCC starts a derived
    class after `sizeof(base)`; the Itanium C++ ABI reuses the padding at
    the end of a base that has virtual functions. Seen: `PanelIcon :
    PanelItem` in `RankPanel_80144430.cpp` (`PanelItem` ends with two
    `unsigned char` at 0x84, size 0x88): `resId` is at 0x88 in the game
    and at 0x86 on the host, while `fn_801BAA88.cpp` builds the object
    with a flat view. The "round 1" banner of the course introduction
    then draws picture 0 (a 640x480 colour chart) across the screen.
    `port/tools/tail_padding_census.py` lists them from the DWARF of the
    linked executable (`docs/port/tail_padding_census.txt`, 8 classes):
    `PanelItem`, `FadeObj` (two units), `clGetCoinLine` (three units),
    `clFlowCCInterface`, `SceneRoot163`. Wanted: the padding at the end
    of each of those bases named as a member (`unsigned char pad86[2];`),
    which keeps the MWCC layout and makes the host's the same.
41. Maths functions declared with `float` parameters (asked 2026-11-11).
    16 units declare `extern double atan2(float y, float x);` (14),
    `extern double atan(float x);` and `extern double tan(float x);`
    (`OrthoCam.c`). On the console a float and a double argument travel in
    the same register; on the host the caller pushes 4 bytes per argument
    and the library reads 8, so the result is garbage: every yaw and pitch
    taken from a matrix or between two points (`Mtx44_GetYawOfPosZ*`,
    `Mtx44_GetPitchOf*`, `Vec3_PitchTo`, `Vec3_HorizontalYawTo`,
    `CourseMarker.cpp`, `LapBannerScene.c`, two effect tick functions,
    `DrawEffect_KartItemTrail_ExplodeAndPropagate.c`) and the frustum of
    the camera that shows the prize item (`OrthoCam.c`). Found in the
    clang IR of the port build (`declare double @atan2(float, float)`).
    Wanted: the real prototypes (`double atan2(double, double)` and so
    on), if the MWCC output stays the same.
42. ZoneObject lists defined as words (asked 2026-11-11).
    `src/game/autodata/data_803F60B4.c` holds the lists that
    `CourseScene_Load` reaches through `CourseZone::objects`
    (`void *lbl_803F6258[36] = { (void *)0xFFFFFF, name, joint, ...}`).
    The reader is `struct ZoneObject { signed char kind, variant; short
    slot; const char *filename, *jointName; }`. On the host the first byte
    of 0x00FFFFFF is 0xFF, the list terminator, so every list is empty, no
    zone slot is created and the parts of a course that zones switch
    (Yoshi Park: fences, tunnel, warp dash boards) are never drawn.
    Wanted: the lists typed as `ZoneObject` or their first words written
    with `TEXT_WORD`, as request 38 did for
    `CourseScene_Load_data_803F5CE8`; and the search of request 38 re-run
    following pointer fields of typed tables into the autodata tables.

43. **Out-normal written into an int** (done, upstream 7c9fac4).
    `Terrain_GetGroundHeightWithPenetration` writes three floats through its
    third argument; `ItemObject_GetGroundTypeAt` and its inline copy in
    `ItemObjectManager_RenderImpl.c` passed the address of an `int`. On the
    console the extra eight bytes fall on unused stack; here they fall on
    the neighbouring locals (in the render function the item's saved height,
    which is what the shadow's size and darkness are worked out from). Seen
    as item shadows ten times too large and fully black in Windows dumps
    (`state.txt`: board scale about 20, constant alpha 255). Upstream check:
    `tools/port_abi/param_address.py`.

45. **Name entry key tables read as 16-bit numbers** (done, upstream
    1a792a2). `src/game/fn_801BAA88.cpp`: the keyboard tables behind
    `lbl_8049A8F8` are Shift-JIS text; the three reads (BuildKeys, and the
    compare and store of the kana mark lookup) now use `SJIS_WORD`. Before
    that the grid of keys was empty on the PC (user report 2026-11-11).

46. **`new` as a parameter name in `include/dolphin/card.h`** (done,
    upstream 225c9b5). `src/triforce/acsocket_nm4.c` (`acConnect`) turns C++
    on with a pragma; the PC build compiles such a unit as C++ from the
    first line and stopped in `card.h` (`CARDRename(..., const char* new)`).
    The unit was "NOT BUILT", `acConnect` a generated stub returning 0, and
    no connect ever reached the network board (docs/port/cabinet_link.md).
    Parameters renamed to `newName`; declarations only.

47. **Course vote read through a 16-bit view** (done, upstream 00de9ef).
    `clFlowCourse.cpp` read the track of the cabinet link's vote result as
    `(*(unsigned short *)p >> 4) & 0x7F`; request 37 had only given the
    setter a macro. `PCB_CHOICE_TRACK(h)` in `pcb_link.h`, bytes on the host
    (`port/compat/mwcc_compat.h`). Two linked copies crashed in course
    selection.

48. **Versus mode's string pool as words** (done, upstream 81e7e44).
    `sdata_806CEFE0_rodata_80320D9C[247]` in `VSMode_FrameUpdate.c`, all
    text, now `TEXT_WORD`; `tools/port_abi/text_words.py` had skipped
    definitions that begin with `__declspec(section ...)`.

## Port-side source patches (2026-11-11)

Corrections that exist only because of the PC do not go upstream any more
(owner's decision, 18:11Z and 18:17Z: host-neutral rewrites such as
`TEXT_WORD` or `BITFIELDS` are welcome in the decompilation, PC-only
behaviour is not). Where the arcade program reads or writes outside an
object and only works through the console's stack layout, the correction is
a patch in `port/patches/<source path>.patch`, applied to a copy of the
source in the build folder just before it is compiled
(`port/tools/source_patches.py`, called from `link_census.py`). A patch whose
context no longer matches stops the build.

| patch | what the arcade program does | on the PC |
|---|---|---|
| `src/game/fn_801F9014.cpp.patch` | the text setter for 16-bit codes (card names) is also given Shift-JIS char strings, the same bytes on the console | decides per text which of the two it is and reads the words accordingly (docs/port/text_encoding.md) |
| `src/game/MukadeItem.c.patch` | `MukadeItem_Tick_HeldAndHoming_3839` copies 11 heights to the stack and indexes them with `KartCharacterParam_GetIdentityIndex` (0..12); characters 11 and 12 read two stack words the function never writes | the neighbouring word held NaN bits, the centipede's position became NaN; the patch gives the two extra entries the value 0 |
| `GabyouItemHeld.cpp`, `GabyouItemReturn.cpp`, `KartFxBoost.cpp` | the same 11-entry stack copies indexed 0..12 (audit in upstream docs/notes/port_abi.md) | two zero entries follow the copy |
| `objview/fn_80198ABC.c` | `lbl_8037DD48[12]` indexed 0..12; entry 12 is the float after the table, 1.0f | index 12 gives 1.0f |
| `ItemObject_GetGroundTypeAt.c`, `ItemObjectManager_RenderImpl.c` | request 43: a three-float normal written through the address of an int (removed upstream in 4b81ff2) | a `float[3]` receives it |
| `ItemHandler_Star.c`, `Item_OrbitAnchorKart.c`, `NokoNoko_CheckHitAndKill.c`, `sprite/fn_801A1798.c` | `return;` in functions with a result (removed upstream in 4b81ff2) | the value the console leaves in r3 is returned |
| `DrawNamCamImage.c` | draws the camera picture for the player's photo with matrix slot 0, which it loads but does not select | another slot is current on the PC and the photo was a piece of the background; `GXSetCurrentMtx(0)` is added (docs/port/camera.md) |
| `GameBoot_Init.cpp` | reads the cabinet number (test menu, PCB ID) from the backup record | `mkgp2pc_boot_cabinet()` may replace it first: `MKGP2PC_PCB_ID`, because the backup memory is not kept between runs (docs/port/cabinet_link.md) |
| `PCBCheckRecv.c` | `BootPCBCheck_Init` starts the 60 second screen "PCB ID : n CHECKING..." that waits until all four boards have connected | `mkgp2pc_boot_check_frames()` gives the length: 0 unless `MKGP2PC_BOOT_CHECK` asks for the screen. Nothing reads a result of the wait; the link is kept up every frame afterwards (docs/port/cabinet_link.md) |
| `fn_801B76C8.cpp`, `fn_801B785C.cpp`, `fn_801BA1A4.cpp`, `fn_801BA318.cpp`, `PCBCheckRecv.c` (second hunk), `GameBoot_Init.cpp` | a confirmed coin leads to the entry screen only when another cabinet is connected; the entry screen closes when all connected cabinets have entered or after 15 s | network versus on demand: the question after the coin, the link switched on and a free cabinet number taken at that moment, the entry screen held open until the button (docs/port/cabinet_link.md, `port/platform/triforce/linkflow.c`) |
| `VSMode_FrameUpdate.c` | the host of a versus race waits a retrace, up to four a frame, whenever a client reports two or more unplayed host frames | over a connection with jitter or loss those reports jump without the client being behind and the race ran at 0.7 to 0.8 of real time; the host asks `mkgp2pc_link_backlog()` instead, which only counts a report that has stayed up (docs/port/cabinet_link.md, "The host's waiting rule"). `MKGP2PC_LINK_PACE=0` gives the arcade rule back. Same patch: only the host moves karts on its own account, a client steps its kart with the input that has been to the host and back; in the port every cabinet steps its own kart with the newest input and reports its position, which the host takes (`mkgp2pc_link_kart`, "Every cabinet drives its own kart"; `MKGP2PC_LINK_OWNKART=0`), and each cabinet holds its countdown after the host's start signal for a time worked out from the delays, so that all karts leave together on the host (`mkgp2pc_link_start_due`, "The start"; `MKGP2PC_LINK_START=0`) |
| `mapobject/fn_8017BC24.c`, `mapobject/fn_8017ACBC.c`, `mapobject/fn_8017AB28.cpp` | these are `clModel::Append`, `clModel::Update` and `clModel::Draw`, written as C functions over a private struct with MWCC's layout of `clModel` (`next` at +0, vtable pointer at +0x1C) | the objects are built by C++ code, where the host compiler puts the vtable pointer first; the private structs get a leading pointer member (docs/port/census.md section 33). Goes away when upstream writes the three as members of the class |
| `misc3d/fn_8016E7E4.c` | the list append for `clKPCharaData` (Bowser cup course props, built in `mapeffect/CoursePropCtor.cpp`) is a C function over `Node { next }`, MWCC's layout (vtable pointer at +0x10) | `Node` gets a leading pointer member (census.md section 33). The two `Update` functions are members of the class upstream since cc6706a and need no patch |
| `MainGameLoop.cpp` | the current flow changes only when its own update says so | at the top of each frame the port may ask for another flow (`mkgp2pc_nav_flow_hook`; the port's menu, docs/port/menu.md) |
| `fn_801B7EF8.c` | `MenuScene_Dispatch` builds the next selection screen from the number the current one returns | the number passes through `mkgp2pc_nav_scene_hook`, which can give the screen before instead; the flow's constructor reports its first screen |
| `ItemSelectState.cpp` | the destructor stores the three chosen items in the player record even when none was chosen (-1) | not stored when unchosen: the screen can now be left backwards |
| `Debug.c`, `DebugTextOutput.c`, `main.c` | `OSReport` and `OSPanic` are empty, `OverlayText` formats a string and drops it, `main` starts scene 3 or 12 | the ends of the debug paths call `port/platform/debug/debug.c`: log, overlay drawing, `MKGP2PC_START_SCENE` (docs/port/debug.md) |
| `SpriteHandlePool.c` | `fn_8011E31C` draws one sprite of the flat list, nothing tells which | it first writes the pool index and the resource id to `mkgp2pc_wide_sprite`, `mkgp2pc_wide_sprite_res` (port/platform/platform.c): the wide picture moves the corner parts of menu screens by resource id (docs/port/widescreen.md) |
| 11 units with `PORT_DATA_LOADED` | the port's hook after a file is read, for byte order (removed upstream in 573f65a) | the calls and `port/compat/include/game/types/port_hooks.h` |
