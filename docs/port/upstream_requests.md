# Source fixes the port needs from mkgp2-decomp

Changes that have to be made upstream because they edit matched sources. All
of them should leave the MWCC objects byte-identical. Port-side shims
(`port/compat`) cannot cover them. Find the affected files with

    python3 port/tools/compile_census.py game -v

Strike an item out when it has been merged from upstream.

## Done

- `operator new(unsigned long)` to `size_t` (upstream `393e3ab`, 2026-11-10).

## Open (2026-11-10, upstream `393e3ab`)

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
