/*
 * Vertex submission on PC.
 *
 * The SDK header (include/dolphin/gx/GXVert.h) defines GXPosition3f32 and
 * friends as inline stores to the write-gather pipe at 0xCC008000. A PC
 * renderer needs them as calls. With MKGP2PC_GFX the declarations come from
 * Aurora's header (its TARGET_PC half); without it (the no-graphics
 * bring-up build) the SDK header is used unchanged and the stores land in
 * plain memory.
 */
#ifdef MKGP2PC_GFX
#ifndef TARGET_PC
#define TARGET_PC 1
#define MKGP2PC_UNDEF_TARGET_PC
#endif
#include "../../../../extern/aurora/include/dolphin/gx/GXVert.h"
#ifdef MKGP2PC_UNDEF_TARGET_PC
#undef TARGET_PC
#undef MKGP2PC_UNDEF_TARGET_PC
#endif
/* the SDK header's guard, so that it is not read after this one */
#ifndef _DOLPHIN_GX_GXVERT_H_
#define _DOLPHIN_GX_GXVERT_H_
#endif
#else
#include_next <dolphin/gx/GXVert.h>
#endif
