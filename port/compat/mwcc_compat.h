#ifndef MKGP2PC_MWCC_COMPAT_H
#define MKGP2PC_MWCC_COMPAT_H

/*
 * Force-included (-include) in front of every decompiled source file when it
 * is compiled by a host compiler instead of Metrowerks CodeWarrior.
 *
 * Only things that let unmodified upstream sources parse and behave the same
 * belong here. It must stay harmless when a file is C or C++.
 */

#ifndef __MWERKS__

/* __declspec(section ".sdata"), __declspec(weak), ...: link-layout control
 * for the matching build, meaningless on PC. */
#define __declspec(x)

/* Gekko intrinsics the game uses outside the MSL headers. */
#ifdef __cplusplus
extern "C" {
#endif
/* frsqrte: must reproduce the PowerPC estimate table bit for bit, or every
 * inlined sqrtf drifts from the original. Declared here, not implemented yet
 * (planned: port/compat/ppc_math.c). */
double mkgp2pc_frsqrte(double x);
#ifdef __cplusplus
}
#endif
#define __frsqrte(x) mkgp2pc_frsqrte(x)
#define __fabs(x) __builtin_fabs(x)

#endif /* !__MWERKS__ */
#endif
