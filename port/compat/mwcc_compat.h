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
#define __fabsf(x) __builtin_fabsf(x)

/* rlwimi: rotate `src` left by `sh` and insert bits mb..me (IBM numbering,
 * bit 0 = most significant) into `dst`. */
static inline unsigned int mkgp2pc_rlwimi(unsigned int dst, unsigned int src, int sh, int mb, int me)
{
    unsigned int rot = sh ? (src << sh) | (src >> (32 - sh)) : src;
    unsigned int hi = 0xFFFFFFFFu >> mb;
    unsigned int lo = 0xFFFFFFFFu << (31 - me);
    unsigned int mask = mb <= me ? (hi & lo) : (hi | lo);
    return (dst & ~mask) | (rot & mask);
}
#define __rlwimi(dst, src, sh, mb, me) mkgp2pc_rlwimi((unsigned int)(dst), (unsigned int)(src), (sh), (mb), (me))

/* Shift-JIS text read two bytes at a time (include/game/types/sjis_word.h):
 * the word as the console sees it, first byte high. */
#define SJIS_WORD(p) \
    ((unsigned short)((((const unsigned char *)(p))[0] << 8) | ((const unsigned char *)(p))[1]))
#define SJIS_WORD_LOAD(dst, p) ((void)((dst) = SJIS_WORD(p)))

/* Card-name check: byte-string functions on 16-bit code strings, on the
 * console's byte view (port/compat/namecode_be.c). */
#ifdef __cplusplus
extern "C" {
#endif
int mkgp2pc_namecode_strcmp(const void *, const void *);
int mkgp2pc_namecode_strncmp(const void *, const void *, __SIZE_TYPE__);
char *mkgp2pc_namecode_strstr(const void *, const void *);
char *mkgp2pc_namecode_strcpy(void *, const void *);
char *mkgp2pc_namecode_strcat(void *, const void *);
#ifdef __cplusplus
}
#endif
#define NAMECODE_STRCMP mkgp2pc_namecode_strcmp
#define NAMECODE_STRNCMP mkgp2pc_namecode_strncmp
#define NAMECODE_STRSTR mkgp2pc_namecode_strstr
#define NAMECODE_STRCPY mkgp2pc_namecode_strcpy
#define NAMECODE_STRCAT mkgp2pc_namecode_strcat

#endif /* !__MWERKS__ */
#endif
