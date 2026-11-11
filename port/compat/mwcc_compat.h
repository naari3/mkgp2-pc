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

/* Display lists and vertex buffers read on the CPU
 * (include/game/types/gx_data.h): they stay big-endian for the GX layer. */
static inline float mkgp2pc_gxdata_f32(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    union { unsigned int u; float f; } v;
    v.u = ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) | ((unsigned int)b[2] << 8) | b[3];
    return v.f;
}
#define GXDATA_U16(p) \
    ((unsigned short)((((const unsigned char *)(p))[0] << 8) | ((const unsigned char *)(p))[1]))
#define GXDATA_SET_U16(p, v) \
    ((void)(((unsigned char *)(p))[0] = (unsigned char)((v) >> 8), ((unsigned char *)(p))[1] = (unsigned char)(v)))
#define GXDATA_F32(p) mkgp2pc_gxdata_f32(p)

/* Cabinet-link slot fields that cross a byte boundary
 * (include/game/types/pcb_link.h): taken from the bytes, as the console's
 * 16- and 32-bit views see them. get2/set2 handle a field of `bits` bits
 * that ends `low` bits above the bottom of the big-endian pair b[0] b[1]. */
static inline unsigned int mkgp2pc_pcb_get2(const void *p, int low, int bits) {
    const unsigned char *b = (const unsigned char *)p;
    return ((((unsigned int)b[0] << 8) | b[1]) >> low) & ((1u << bits) - 1);
}
static inline void mkgp2pc_pcb_set2(void *p, int low, int bits, unsigned int v) {
    unsigned char *b = (unsigned char *)p;
    unsigned int mask = ((1u << bits) - 1) << low;
    unsigned int w = ((((unsigned int)b[0] << 8) | b[1]) & ~mask) | ((v << low) & mask);
    b[0] = (unsigned char)(w >> 8);
    b[1] = (unsigned char)w;
}
#define PCB_HDR_SCENE(w) mkgp2pc_pcb_get2((const unsigned char *)&(w) + 1, 7, 8)
#define PCB_HDR_SET_SCENE(h, v) mkgp2pc_pcb_set2((unsigned char *)&(h) + 1, 7, 8, (unsigned int)(v))
#define PCB_SLOT_WINS(s, field) mkgp2pc_pcb_get2((const unsigned char *)(s) + 6, 5, 4)
#define PCB_CLASS_SET_WINS(c, v) mkgp2pc_pcb_set2(&(c), 5, 4, (unsigned int)(v))
#define PCB_CHOICE_SET_TRACK(c, v) mkgp2pc_pcb_set2(&(c), 4, 7, (unsigned int)(v))
#define PCB_FLAGS_ID(f, view) mkgp2pc_pcb_get2(&(f), 7, 3)
#define PCB_FLAGS_SET_ID(f, view, v) mkgp2pc_pcb_set2(&(f), 7, 3, (unsigned int)(v))
#define PCB_FLAGS16_ID(h) mkgp2pc_pcb_get2(&(h), 7, 3)

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
