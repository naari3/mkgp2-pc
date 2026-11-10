# C++ classes the port build gets wrong today

Input for the upstream pilot of `cxx_abi_decision.md`. Lists are from
mkgp2-decomp `d0d86cd` (2026-11-10).

## Vtable pointer not at the host compiler's position

MWCC (GC/1.3.2, measured) puts the vtable pointer where the first `virtual`
is declared among the data members: `struct { int a; virtual void f(); int
b; }` has it at +4 and `b` at +8; with the virtual first it is at +0; in a
class with a non-polymorphic base it comes after the base and the members
declared before the first virtual. The Itanium ABI always puts it at +0 (and
moves a non-polymorphic base behind it). The port's IR pass does not and
will not correct this; these classes need a host-neutral definition
upstream.

Classes without a base that declare a data member before their first
virtual function (found by a text scan of `src` and `include`; the member
shown is the first one):

| file | class | first member |
| --- | --- | --- |
| `src/game/RankPanel_80144430.cpp`, `include/game/CSParts.h`, `include/game/anon_8021BC44Types.h` | `clCSPartsInerface` | `int obj` |
| `src/game/Scene_8016E1C0.cpp` | `PropNode` | `PropNode *next` |
| `src/game/mapeffect/CoursePropCtor.cpp` | `CoursePropNode` | `CoursePropNode *next` |
| `src/game/mapobject/fn_8017AA80.cpp`, `fn_8017AC54.cpp` | `Obj` | `Obj *next` |
| `src/game/mapobject/fn_8017AD54.cpp`, `include/game/mapobject/mapobject.h`, `mapobject17b_private.h` | `clModel` | `clModel *next` |
| `include/game/types/SyncWaiter.h` | `SyncWaiter` | `char pad00[0x14]` |
| `src/game/HudThunk802488FC.cpp` | `HudThunkABase0` | `int a` |
| `src/game/HudThunk80248CE4.cpp` | `HudThunkBBase0` | `int a` |
| `src/game/RaceHudAdjust.cpp` | `RaceHudRootBase0` | `int a` |
| `src/game/AI_AdjustThunks.cpp` | `AIBase0` | `int a, b` |
| `src/game/RankDispControls.cpp` | `RankDispBase0` | `int a` |
| `src/game/anon_8020C974_thunk.cpp` | `RaceSetupBase0` | `char pad4[0x5c - 4]` |

The `*Base0` classes are the first bases of two-base classes whose only
purpose is to make MWCC emit this-adjusting thunks.

Not scanned yet: classes with a base where the base is not polymorphic and
the derived class introduces the first virtual (the scan counts 215 classes
with a base and a member before the first virtual, almost all of which have
a polymorphic base and are fine).

## Classes on the path of the first frames

Not listed by class yet. The units that ran in the bring-up executable up to
the menu scene and its first transition are the boot sequence
(`BootDispatcher.cpp`, `ServiceButton_Handler.cpp`, `CardTaskChecks.cpp`,
`CardTaskManager_Tick.c`), the flow dispatcher (`MainGameLoop.cpp`,
`Scene_Draw.cpp`, `fn_801B7EF8.c` with `MenuScene_Dispatch`), the input
objects (`InputObj.c`, vtable `g_InputObjectVtable`) and the scene table
`lbl_8049A734`. The first transition out of the menu constructs
`clFlowCard` (`src/game/fn_801BAA88.cpp`), which does not compile in the
port yet because it uses `std::vector` from the MSL C++ headers.

## Units that need the MSL C++ library

`<vector>`: `fn_801BAA88.cpp`. `<list>`: `anon_80203EA0_nm2.cpp`,
`anon_8020E3D0_nm1.cpp`. `<deque>`: `fn_801DEF94.cpp`, `fn_801E4AF4.cpp`,
`fn_801E5454.cpp`. (`GpPleaseInit.cpp` fails for another reason: a size
check in `GpPleaseInitPrivate.h`.) The headers are fetched, not committed
(`tools/fetch_msl.py`), and do not compile with clang as they are.
