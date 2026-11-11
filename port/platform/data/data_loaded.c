/* Byte-order conversion of disc data: one function per file format. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mkgp2pc/data.h"

static void swap16(void* p) {
  uint16_t v = mkgp2pc_be16(p);
  memcpy(p, &v, 2);
}
static void swap32(void* p) {
  uint32_t v = mkgp2pc_be32(p);
  memcpy(p, &v, 4);
}
static void swap32n(uint8_t* p, int n) {
  while (n--) {
    swap32(p);
    p += 4;
  }
}

/*
 * TPL: header { magic 0x0020AF30, count, descriptor offset }, descriptors
 * { image header offset, CLUT header offset }, image header { u16 height,
 * u16 width, u32 format, u32 data offset, u32 wrapS, wrapT, minFilter,
 * magFilter, f32 lodBias, u8 edgeLod, minLod, maxLod, unpacked }, CLUT header
 * { u16 entries, u8 unpacked, u8 pad, u32 format, u32 data offset }.
 * Texel and palette data are left in console byte order.
 */
static int seen(uint32_t* done, uint32_t* n, uint32_t offset) {
  for (uint32_t i = 0; i < *n; i++) {
    if (done[i] == offset) {
      return 1;
    }
  }
  done[(*n)++] = offset;
  return 0;
}

static void tpl(uint8_t* d, uint32_t size) {
  if (size < 12 || mkgp2pc_be32(d) != 0x0020AF30u) {
    return; /* not a TPL, or already converted */
  }
  swap32n(d, 3);
  uint32_t count, table, n = 0;
  memcpy(&count, d + 4, 4);
  memcpy(&table, d + 8, 4);
  if (count > (size - table) / 8) {
    count = (size - table) / 8;
  }
  /* several descriptors may share one header: convert each header once */
  uint32_t* done = (uint32_t*)malloc((count * 2 + 1) * sizeof(uint32_t));
  for (uint32_t i = 0; i < count; i++) {
    uint8_t* desc = d + table + i * 8;
    uint32_t image = mkgp2pc_be32(desc), clut = mkgp2pc_be32(desc + 4);
    swap32n(desc, 2);
    if (image && image + 0x24 <= size && !seen(done, &n, image)) {
      uint8_t* h = d + image;
      swap16(h);
      swap16(h + 2);
      swap32n(h + 4, 7);
    }
    if (clut && clut + 0xC <= size && !seen(done, &n, clut)) {
      uint8_t* h = d + clut;
      swap16(h);
      swap32n(h + 4, 2);
    }
  }
  free(done);
}

/*
 * Sprite animation (`ani_*.bin`), types in include/game/SpriteAnimTraversal_private.h.
 * Header (0x20): five words, f32 length, u32 frames offset, u32 resources
 * offset. Frame (0x54): u8 flags (bit 0 = last of its array), s8 layer,
 * s8 count, u8, s16 resource, s16, u32 child offset, f32 time, f32 x, y,
 * scaleX, scaleY, rotation, f32 red, green, blue, alpha, s16 colour offsets
 * x4, f32 matrix[6]. Resources: s16 list ended by -1.
 */
typedef struct {
  uint32_t* offsets;
  uint32_t count, room;
} Visited;

static int visit(Visited* v, uint32_t offset) {
  for (uint32_t i = 0; i < v->count; i++) {
    if (v->offsets[i] == offset) {
      return 0;
    }
  }
  if (v->count == v->room) {
    v->room = v->room ? v->room * 2 : 64;
    v->offsets = (uint32_t*)realloc(v->offsets, v->room * sizeof(uint32_t));
  }
  v->offsets[v->count++] = offset;
  return 1;
}

static void sprite_frames(uint8_t* d, uint32_t offset, Visited* v, int depth) {
  if (offset == 0 || depth > 64 || !visit(v, offset)) {
    return;
  }
  for (uint8_t* f = d + offset;; f += 0x54) {
    uint32_t child = mkgp2pc_be32(f + 8);
    swap16(f + 4);
    swap16(f + 6);
    swap32n(f + 8, 11);    /* child, time, x .. alpha */
    for (int i = 0; i < 4; i++) {
      swap16(f + 0x34 + i * 2);
    }
    swap32n(f + 0x3C, 6);  /* matrix */
    sprite_frames(d, child, v, depth + 1);
    if (f[0] & 1) {
      break;
    }
  }
}

/* The two "goods" popups of the coin tally (sp_next_goods.bin,
 * sp_rare_goods.bin) list pieces of the English textures on the disc, while
 * their positions fit the Japanese pictures and the program hides their
 * digit placeholders by the Japanese ids (SetAnimParam(0x1931, ...) and so
 * on, which find nothing in the lists as they are). With the lists from the
 * disc the popup reads "coins until next prize!" with the digits on top of
 * the words. The user reports that as wrong against the arcade, so the
 * lists the program expects are put in here. How the arcade arrives at them
 * is not known (docs/port/census.md section 31); MKGP2PC_GOODS_DISC=1 keeps
 * the lists of the disc. Called with the file still big-endian. */
static void goods_lists(uint8_t* d, uint32_t resources) {
  static const uint16_t next_disc[] = {0x1140, 0x1141, 0x1142, 0x114E, 0x114F, 0xFFFF};
  static const uint16_t next_jp[] = {0x1924, 0x1925, 0x1926, 0x1931, 0x1932};
  static const uint16_t rare_disc[] = {0x114C, 0x114E, 0x114F, 0x1150, 0x1151, 0xFFFF};
  static const uint16_t rare_jp[] = {0x1930, 0x1931, 0x1932, 0x1933, 0x1934};
  const uint16_t* to = NULL;
  int next = 1, rare = 1;
  if (resources == 0 || getenv("MKGP2PC_GOODS_DISC") != NULL) {
    return;
  }
  for (int i = 0; i < 6; i++) {
    uint16_t id = mkgp2pc_be16(d + resources + 2 * i);
    next = next && id == next_disc[i];
    rare = rare && id == rare_disc[i];
  }
  /* E_sp_next_goods.bin has the same list; its first piece sits at x = 89,
   * the Japanese layout's at 0. */
  if (next && mkgp2pc_be32(d + 0x30) == 0) {
    to = next_jp;
  } else if (rare) {
    to = rare_jp;
  }
  if (to != NULL) {
    for (int i = 0; i < 5; i++) {
      d[resources + 2 * i] = (uint8_t)(to[i] >> 8);
      d[resources + 2 * i + 1] = (uint8_t)to[i];
    }
  }
}

static void sprite_anim(uint8_t* d) {
  uint32_t frames = mkgp2pc_be32(d + 0x18), resources = mkgp2pc_be32(d + 0x1C);
  goods_lists(d, resources);
  Visited v = {0, 0, 0};
  swap32n(d, 8);
  sprite_frames(d, frames, &v, 0);
  free(v.offsets);
  if (resources) {
    for (uint8_t* r = d + resources;; r += 2) {
      int last = mkgp2pc_be16(r) == 0xFFFF;
      swap16(r);
      if (last) {
        break;
      }
    }
  }
}

/* Course path table: u32 offsets from the file start, ended by 0; each leads
 * to a list of { int flag; float x, y, z; } ended by an entry with flag < 0. */
static void path_table(uint8_t* d, uint32_t size) {
  uint8_t* done = (uint8_t*)calloc(size / 4 + 1, 1); /* lists may share entries */
  if (done == NULL) {
    return;
  }
  for (uint32_t i = 0; i + 4 <= size && !done[i / 4]; i += 4) {
    uint32_t o = mkgp2pc_be32(d + i);
    done[i / 4] = 1;
    swap32(d + i);
    if (o == 0) {
      break;
    }
    for (; o + 16 <= size && !done[o / 4]; o += 16) {
      int32_t flag = (int32_t)mkgp2pc_be32(d + o);
      done[o / 4] = 1;
      swap32n(d + o, 4);
      if (flag < 0) {
        break;
      }
    }
  }
  free(done);
}

/* 16-byte records. words: how many leading 32-bit words to swap. */
static void records16(uint8_t* d, uint32_t size, int words) {
  for (uint32_t o = 0; o + 16 <= size; o += 16) {
    swap32n(d + o, words);
  }
}

void mkgp2pc_hsd_archive_header(void* header);
void mkgp2pc_hsd_archive(void* archive);

/* Counts loads: file data replaces older data at the same addresses, and
 * caches keyed by address (the GX layer's textures) must notice. */
unsigned mkgp2pc_data_generation;

void mkgp2pc_data_loaded(int kind, void* data, uint32_t size) {
  mkgp2pc_data_generation++;
  switch (kind) {
  case MKGP2PC_DATA_ARCHIVE_HEADER:
    mkgp2pc_hsd_archive_header(data);
    break;
  case MKGP2PC_DATA_ARCHIVE:
    mkgp2pc_hsd_archive(data);
    break;
  case MKGP2PC_DATA_PATH_TABLE:
    path_table((uint8_t*)data, size);
    break;
  case MKGP2PC_DATA_GHOST: /* TaGhostFrame: three floats, four bytes */
    records16((uint8_t*)data, size, 3);
    break;
  case MKGP2PC_DATA_DEMO_INPUT: /* TraceInput163: three floats, an int */
    records16((uint8_t*)data, size, 4);
    break;
  case MKGP2PC_DATA_COLLISION:
    /* Course collision grid: a 0x2C-byte header (cell counts, cell size,
     * bounds, origin, offset of the cell table), 16-byte cells {triangle
     * offset, count, wall offset, count}, 0x70-byte triangles and 0x20-byte
     * walls. Every field is a 32-bit float or integer; checked on the 35
     * files of the disc, where these records and their zero terminators
     * account for every byte. */
    swap32n((uint8_t*)data, (int)(size / 4));
    break;
  case MKGP2PC_DATA_STREAM_HEADER: {
    /* DSP ADPCM header (StreamInfo): samples, nibbles, rate; loop flag,
     * format; loop start, loop end, current address; then 16-bit fields
     * (coefficients, gain, predictor state) to the end */
    uint8_t* d = (uint8_t*)data;
    if (size >= 0x60) {
      swap32n(d, 3);
      swap16(d + 0x0C);
      swap16(d + 0x0E);
      swap32n(d + 0x10, 3);
      for (uint32_t o = 0x1C; o < 0x60; o += 2) {
        swap16(d + o);
      }
    }
    break;
  }
  case 7:
    /* PORT_DATA_COURSE_BGM: the file is loaded and freed, and
     * CourseBgm_Load counts its leading records whose first word is zero
     * (the same in either byte order); no unit reads anything else in it
     * (only CourseBgm_Load.c and CourseBgm_Free.c name g_courseBgm). The
     * load site gives no size. Left as it is. */
    break;
  case MKGP2PC_DATA_TPL:
    tpl((uint8_t*)data, size);
    break;
  case MKGP2PC_DATA_SPRITE_ANIM:
    sprite_anim((uint8_t*)data);
    break;
  default: {
    static unsigned told;
    if (kind >= 0 && kind < 32 && !(told & (1u << kind))) {
      told |= 1u << kind;
      fprintf(stderr, "mkgp2pc_data_loaded: kind %d is not converted yet\n", kind);
    }
    break;
  }
  }
}
