/*
 * A small JPEG of a picture, for the dump's frame_small.jpg: a copy of the
 * frame that is light enough to pass around (frame.png keeps every pixel).
 * Baseline JPEG, no chroma subsampling, the standard's example tables
 * (ITU T.81 annex K). The picture is first shrunk by a whole factor so that
 * it is no wider than asked.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const uint8_t kQuantY[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
static const uint8_t kQuantC[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
                                    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
static const uint8_t kDcBitsY[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t kDcBitsC[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t kAcBitsY[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D};
static const uint8_t kAcValsY[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
    0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA};
static const uint8_t kAcBitsC[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t kAcValsC[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1,
    0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
    0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
    0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA};

typedef struct {
  uint16_t code[256];
  uint8_t len[256];
} Huff;
static void huff_build(Huff* h, const uint8_t* bits, const uint8_t* vals) {
  unsigned code = 0, k = 0;
  memset(h, 0, sizeof(*h));
  for (int len = 1; len <= 16; len++) {
    for (int i = 0; i < bits[len - 1]; i++) {
      h->code[vals[k]] = (uint16_t)code++;
      h->len[vals[k]] = (uint8_t)len;
      k++;
    }
    code <<= 1;
  }
}

typedef struct {
  FILE* f;
  uint32_t acc;
  int n;
} Bits;
static void bits_put(Bits* b, unsigned code, int len) {
  b->acc = (b->acc << len) | (code & ((1u << len) - 1));
  b->n += len;
  while (b->n >= 8) {
    uint8_t byte = (uint8_t)(b->acc >> (b->n - 8));
    fputc(byte, b->f);
    if (byte == 0xFF) {
      fputc(0, b->f);
    }
    b->n -= 8;
  }
}
static void put16(FILE* f, unsigned v) {
  fputc((int)(v >> 8), f);
  fputc((int)(v & 0xFF), f);
}
static void put_dht(FILE* f, int id, const uint8_t* bits, const uint8_t* vals, int count) {
  put16(f, 0xFFC4);
  put16(f, (unsigned)(2 + 1 + 16 + count));
  fputc(id, f);
  fwrite(bits, 1, 16, f);
  fwrite(vals, 1, (size_t)count, f);
}

static float s_cos[8][8];
/* one 8x8 block: transform, quantise, entropy-code; returns the DC value */
static int block_put(Bits* b, const float* px, const uint8_t* quant, int lastDc, const Huff* dc, const Huff* ac) {
  float tmp[64];
  int q[64];
  for (int y = 0; y < 8; y++) {
    for (int u = 0; u < 8; u++) {
      float s = 0;
      for (int x = 0; x < 8; x++) {
        s += px[y * 8 + x] * s_cos[x][u];
      }
      tmp[y * 8 + u] = s;
    }
  }
  for (int u = 0; u < 8; u++) {
    for (int v = 0; v < 8; v++) {
      float s = 0;
      for (int y = 0; y < 8; y++) {
        s += tmp[y * 8 + u] * s_cos[y][v];
      }
      s *= 0.25f * (u == 0 ? 0.70710678f : 1.0f) * (v == 0 ? 0.70710678f : 1.0f);
      q[v * 8 + u] = (int)lroundf(s / (float)quant[v * 8 + u]);
    }
  }
  int diff = q[0] - lastDc;
  int mag = diff < 0 ? -diff : diff, size = 0;
  while (mag != 0) {
    size++;
    mag >>= 1;
  }
  bits_put(b, dc->code[size], dc->len[size]);
  if (size != 0) {
    bits_put(b, (unsigned)(diff < 0 ? diff - 1 : diff), size);
  }
  int run = 0;
  for (int i = 1; i < 64; i++) {
    int v = q[kZigzag[i]];
    if (v == 0) {
      run++;
      continue;
    }
    while (run > 15) {
      bits_put(b, ac->code[0xF0], ac->len[0xF0]);
      run -= 16;
    }
    mag = v < 0 ? -v : v;
    size = 0;
    while (mag != 0) {
      size++;
      mag >>= 1;
    }
    bits_put(b, ac->code[run * 16 + size], ac->len[run * 16 + size]);
    bits_put(b, (unsigned)(v < 0 ? v - 1 : v), size);
    run = 0;
  }
  if (run != 0) {
    bits_put(b, ac->code[0], ac->len[0]);
  }
  return q[0];
}

bool mkgp2pc_jpeg_write(const char* path, const uint8_t* rgba, uint32_t srcW, uint32_t srcH, uint32_t maxWidth, int quality) {
  if (srcW == 0 || srcH == 0 || maxWidth == 0) {
    return false;
  }
  const uint32_t k = (srcW + maxWidth - 1) / maxWidth; /* shrink factor */
  const uint32_t w = srcW / k, h = srcH / k;
  if (w == 0 || h == 0 || w > 65535 || h > 65535) {
    return false;
  }
  uint8_t* small = (uint8_t*)malloc((size_t)w * h * 3);
  FILE* f = small != NULL ? fopen(path, "wb") : NULL;
  if (f == NULL) {
    free(small);
    return false;
  }
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      uint32_t sum[3] = {0, 0, 0};
      for (uint32_t j = 0; j < k; j++) {
        const uint8_t* s = rgba + (((size_t)y * k + j) * srcW + (size_t)x * k) * 4;
        for (uint32_t i = 0; i < k; i++, s += 4) {
          sum[0] += s[0];
          sum[1] += s[1];
          sum[2] += s[2];
        }
      }
      uint8_t* d = small + ((size_t)y * w + x) * 3;
      d[0] = (uint8_t)(sum[0] / (k * k));
      d[1] = (uint8_t)(sum[1] / (k * k));
      d[2] = (uint8_t)(sum[2] / (k * k));
    }
  }

  uint8_t quant[2][64];
  quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
  const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
  for (int i = 0; i < 64; i++) {
    int a = (kQuantY[i] * scale + 50) / 100, c = (kQuantC[i] * scale + 50) / 100;
    quant[0][i] = (uint8_t)(a < 1 ? 1 : a > 255 ? 255 : a);
    quant[1][i] = (uint8_t)(c < 1 ? 1 : c > 255 ? 255 : c);
  }
  for (int x = 0; x < 8; x++) {
    for (int u = 0; u < 8; u++) {
      s_cos[x][u] = cosf((float)(2 * x + 1) * (float)u * 3.14159265f / 16.0f);
    }
  }
  static Huff dcY, dcC, acY, acC;
  huff_build(&dcY, kDcBitsY, kDcVals);
  huff_build(&dcC, kDcBitsC, kDcVals);
  huff_build(&acY, kAcBitsY, kAcValsY);
  huff_build(&acC, kAcBitsC, kAcValsC);

  put16(f, 0xFFD8);
  static const uint8_t app0[] = {0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
  fwrite(app0, 1, sizeof(app0), f);
  for (int t = 0; t < 2; t++) {
    put16(f, 0xFFDB);
    put16(f, 67);
    fputc(t, f);
    for (int i = 0; i < 64; i++) {
      fputc(quant[t][kZigzag[i]], f);
    }
  }
  put16(f, 0xFFC0);
  put16(f, 17);
  fputc(8, f);
  put16(f, h);
  put16(f, w);
  fputc(3, f);
  for (int c = 0; c < 3; c++) {
    fputc(c + 1, f);
    fputc(0x11, f);
    fputc(c == 0 ? 0 : 1, f);
  }
  put_dht(f, 0x00, kDcBitsY, kDcVals, 12);
  put_dht(f, 0x10, kAcBitsY, kAcValsY, 162);
  put_dht(f, 0x01, kDcBitsC, kDcVals, 12);
  put_dht(f, 0x11, kAcBitsC, kAcValsC, 162);
  put16(f, 0xFFDA);
  put16(f, 12);
  fputc(3, f);
  for (int c = 0; c < 3; c++) {
    fputc(c + 1, f);
    fputc(c == 0 ? 0x00 : 0x11, f);
  }
  fputc(0, f);
  fputc(63, f);
  fputc(0, f);

  Bits bits = {f, 0, 0};
  int last[3] = {0, 0, 0};
  for (uint32_t by = 0; by < h; by += 8) {
    for (uint32_t bx = 0; bx < w; bx += 8) {
      float comp[3][64];
      for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
          /* the edge pixels repeat beyond the picture */
          uint32_t sx = bx + x < w ? bx + x : w - 1, sy = by + y < h ? by + y : h - 1;
          const uint8_t* s = small + ((size_t)sy * w + sx) * 3;
          float r = s[0], g = s[1], bl = s[2];
          comp[0][y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * bl - 128.0f;
          comp[1][y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * bl;
          comp[2][y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * bl;
        }
      }
      last[0] = block_put(&bits, comp[0], quant[0], last[0], &dcY, &acY);
      last[1] = block_put(&bits, comp[1], quant[1], last[1], &dcC, &acC);
      last[2] = block_put(&bits, comp[2], quant[1], last[2], &dcC, &acC);
    }
  }
  bits_put(&bits, 0x7F, 7); /* fill the last byte with ones */
  put16(f, 0xFFD9);
  bool ok = ferror(f) == 0;
  fclose(f);
  free(small);
  return ok;
}
