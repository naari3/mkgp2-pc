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
