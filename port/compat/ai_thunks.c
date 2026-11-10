/* Secondary-base thunks of the AI controller (0x801E8C90..0x801E8CB8).
 *
 * MWCC emits `this -= 12; b target` next to the class's vtable
 * (src/game/AI_AdjustThunks.cpp); the vtable written as data
 * (src/game/autodata/data_804E4878.c) names them by address. A host compiler
 * emits thunks only where the virtual functions themselves are defined, so
 * nothing defines these names. Targets as in the original code. */
#define THUNK(name, target) \
  __asm__(".text\n.globl " #name "\n" #name ":\n subl $12, 4(%esp)\n jmp " #target "\n")

THUNK(fn_801E8C90, fn_801E81D8);
THUNK(fn_801E8C98, AI_GrantItem);
THUNK(fn_801E8CA0, fn_801E6670);
THUNK(fn_801E8CA8, fn_801E66B4);
THUNK(fn_801E8CB0, fn_801E68B4);
