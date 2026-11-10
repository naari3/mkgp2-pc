/* The game's wide-character functions. Its C library has a 16-bit wchar_t;
 * the host's functions of the same names work on 32-bit units (Linux) and
 * cannot stand in. Defined here under the plain names, so the game's calls
 * bind to them; the link keeps the names out of the dynamic symbol table so
 * that the host libraries go on using their own (port/tools/link_census.py).
 * vswprintf is not here yet. */
#include <stddef.h>
#include <stdint.h>

typedef uint16_t mw_wchar;

size_t wcslen(const mw_wchar* s) {
  size_t n = 0;
  while (s[n] != 0) {
    n++;
  }
  return n;
}

mw_wchar* wcscpy(mw_wchar* dst, const mw_wchar* src) {
  mw_wchar* d = dst;
  while ((*d++ = *src++) != 0) {
  }
  return dst;
}

/* Like strncpy: stops copying at the terminator and fills the rest with 0. */
mw_wchar* wcsncpy(mw_wchar* dst, const mw_wchar* src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i] != 0; i++) {
    dst[i] = src[i];
  }
  for (; i < n; i++) {
    dst[i] = 0;
  }
  return dst;
}

int wcscmp(const mw_wchar* a, const mw_wchar* b) {
  while (*a != 0 && *a == *b) {
    a++;
    b++;
  }
  return (int)*a - (int)*b;
}

mw_wchar* wcschr(const mw_wchar* s, mw_wchar c) {
  for (;; s++) {
    if (*s == c) {
      return (mw_wchar*)s;
    }
    if (*s == 0) {
      return NULL;
    }
  }
}

mw_wchar* wmemset(mw_wchar* dst, mw_wchar c, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    dst[i] = c;
  }
  return dst;
}
