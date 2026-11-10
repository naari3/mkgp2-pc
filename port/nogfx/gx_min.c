/* GX functions whose result the game uses for something other than drawing,
 * for the build without rendering. Everything else in GX is a generated stub
 * there; with Aurora linked, Aurora provides these. */
#include <stdint.h>

/* Bytes a texture of the given size and format takes: the image is stored
 * in tiles of 8x8, 8x4 or 4x4 texels, 32 bytes each (64 for the 32-bit
 * formats), and each mipmap level is rounded up to whole tiles. */
uint32_t GXGetTexBufferSize(uint16_t width, uint16_t height, uint32_t format,
                            uint8_t mipmap, uint8_t max_lod) {
  uint32_t shift_x, shift_y, tile_bytes = 32, size = 0, w = width, h = height;
  int level, levels = mipmap ? max_lod : 1;

  switch (format) {
  case 0x0: case 0x8: case 0xE: case 0x20: case 0x30: /* I4, C4, CMPR */
    shift_x = 3, shift_y = 3;
    break;
  case 0x1: case 0x2: case 0x9: case 0x11: case 0x27: case 0x28: case 0x29: case 0x2A:
  case 0x39: case 0x3A:                               /* I8, IA4, C8, Z8 */
    shift_x = 3, shift_y = 2;
    break;
  case 0x6: case 0x16:                                /* RGBA8, Z24X8 */
    shift_x = 2, shift_y = 2, tile_bytes = 64;
    break;
  default:                                            /* IA8, RGB565, RGB5A3, C14X2, Z16 */
    shift_x = 2, shift_y = 2;
    break;
  }
  for (level = 0; level < levels; level++) {
    uint32_t tiles_x = (w + (1u << shift_x) - 1) >> shift_x;
    uint32_t tiles_y = (h + (1u << shift_y) - 1) >> shift_y;
    size += tiles_x * tiles_y * tile_bytes;
    if (w == 1 && h == 1) break;
    w = w > 1 ? w >> 1 : 1;
    h = h > 1 ? h >> 1 : 1;
  }
  return size;
}
