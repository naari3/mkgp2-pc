/* Byte-order conversion of disc data, done once when a file has been loaded. */
#ifndef MKGP2PC_DATA_H
#define MKGP2PC_DATA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* File kinds the game tells apart at its load sites: the numbers of
 * PORT_DATA_LOADED in the game's include/game/types/port_hooks.h. */
enum {
  MKGP2PC_DATA_TPL = 1,            /* TEXLoadPalette, before offsets become pointers */
  MKGP2PC_DATA_ARCHIVE_HEADER = 2, /* HSD archive: the 0x20-byte header alone */
  MKGP2PC_DATA_ARCHIVE = 3,        /* HSD archive body, before ArchiveRelocate */
  MKGP2PC_DATA_PATH_TABLE = 4,     /* CourseData_LoadPathTable */
  MKGP2PC_DATA_GHOST = 5,          /* TaMode_FrameUpdate */
  MKGP2PC_DATA_SPRITE_ANIM = 6,    /* ani_*.bin, before fn_801A0D90 relocates it; size 0 */
  MKGP2PC_DATA_COURSE_BGM = 7,     /* CourseBgm_Load; size 0 */
  MKGP2PC_DATA_COLLISION = 8,      /* CollisionFile_Load, before the pointer fix-up */
  MKGP2PC_DATA_DEMO_INPUT = 9,     /* Scene_80164614 */
  MKGP2PC_DATA_SOUND_FILE = 10,    /* MusyX pool / proj / sdir / samp */
  MKGP2PC_DATA_STREAM_HEADER = 11, /* 0x60-byte DSP header */
};

/* Called by the game right after it has a whole file in memory (inflated if
 * it was compressed) and before it looks inside. Converts the big-endian
 * fields the game reads through structs to host order, in place. Pixel and
 * vertex payloads that go to GX stay as they are: GX takes console formats. */
void mkgp2pc_data_loaded(int kind, void* data, uint32_t size);

static inline uint16_t mkgp2pc_be16(const void* p) {
  const uint8_t* b = (const uint8_t*)p;
  return (uint16_t)(b[0] << 8 | b[1]);
}
static inline uint32_t mkgp2pc_be32(const void* p) {
  const uint8_t* b = (const uint8_t*)p;
  return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

#ifdef __cplusplus
}
#endif

#endif
