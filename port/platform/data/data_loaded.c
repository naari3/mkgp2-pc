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

void mkgp2pc_data_loaded(int kind, void* data, uint32_t size) {
  switch (kind) {
  case MKGP2PC_DATA_TPL:
    tpl((uint8_t*)data, size);
    break;
  default:
    fprintf(stderr, "mkgp2pc_data_loaded: unknown kind %d\n", kind);
    break;
  }
}
