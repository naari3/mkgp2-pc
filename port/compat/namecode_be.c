/* Byte-string functions on NameCode (16-bit Shift-JIS code) strings as the
 * console sees them: every code high byte first
 * (include/game/types/namecode_str.h, upstream request 24). Byte k of the
 * console's view is the high (k even) or low (k odd) half of code k / 2.
 * A result that points at an odd byte cannot be expressed on a little-endian
 * host as a code pointer with the same meaning; it is returned as the byte
 * address all the same (no pattern of the game starts at an odd byte of a
 * name made of full-width codes). */
#include <stddef.h>
#include <stdint.h>

static unsigned char get(const void* s, size_t k) {
  uint16_t c = ((const uint16_t*)s)[k / 2];
  return (unsigned char)((k & 1) ? c : c >> 8);
}

static void put(void* s, size_t k, unsigned char b) {
  uint16_t* c = &((uint16_t*)s)[k / 2];
  *c = (k & 1) ? (uint16_t)((*c & 0xFF00) | b) : (uint16_t)((*c & 0x00FF) | (b << 8));
}

int mkgp2pc_namecode_strncmp(const void* a, const void* b, size_t n) {
  size_t k;
  for (k = 0; k < n; k++) {
    unsigned char x = get(a, k), y = get(b, k);
    if (x != y) {
      return (int)x - (int)y;
    }
    if (x == 0) {
      break;
    }
  }
  return 0;
}

int mkgp2pc_namecode_strcmp(const void* a, const void* b) {
  return mkgp2pc_namecode_strncmp(a, b, (size_t)-1);
}

char* mkgp2pc_namecode_strstr(const void* s, const void* t) {
  size_t k, j;
  if (get(t, 0) == 0) {
    return (char*)s;
  }
  for (k = 0; get(s, k) != 0; k++) {
    for (j = 0; get(t, j) != 0 && get(s, k + j) == get(t, j); j++) {
    }
    if (get(t, j) == 0) {
      return (char*)s + k;
    }
  }
  return NULL;
}

char* mkgp2pc_namecode_strcpy(void* d, const void* s) {
  size_t k = 0;
  unsigned char b;
  do {
    b = get(s, k);
    put(d, k, b);
    k++;
  } while (b != 0);
  return (char*)d;
}

char* mkgp2pc_namecode_strcat(void* d, const void* s) {
  size_t n = 0, k = 0;
  unsigned char b;
  while (get(d, n) != 0) {
    n++;
  }
  do {
    b = get(s, k);
    put(d, n + k, b);
    k++;
  } while (b != 0);
  return (char*)d;
}
