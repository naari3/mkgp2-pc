/* Byte-order conversion of disc data, done once when a file has been loaded. */
#ifndef MKGP2PC_DATA_H
#define MKGP2PC_DATA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* File kinds the game tells apart at its load sites (PORT_DATA_LOADED). */
enum {
  MKGP2PC_DATA_TPL = 1, /* texture palette, before the game turns offsets into pointers */
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
