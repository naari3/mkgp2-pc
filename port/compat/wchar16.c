/* The game's wide-character functions. Its C library has a 16-bit wchar_t;
 * the host's functions of the same names work on 32-bit units (Linux) and
 * cannot stand in. Defined here under the plain names, so the game's calls
 * bind to them; the link keeps the names out of the dynamic symbol table so
 * that the host libraries go on using their own (port/tools/link_census.py).
 */
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

/* vswprintf for 16-bit units, C99 rules as in the game's library: %s takes
 * a byte string, %ls a wide one. Every other conversion is formatted by the
 * host's snprintf from the same specification and widened. Returns the
 * number of units written, or -1 when the result does not fit in n. */
/* MinGW's <stdio.h> defines an inline vswprintf of its own. */
#define vswprintf mkgp2pc_host_vswprintf
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#undef vswprintf

int vswprintf(mw_wchar* dst, size_t n, const mw_wchar* fmt, va_list ap) {
  size_t out = 0;
#define PUT(ch)                 \
  do {                          \
    if (out + 1 >= n) {         \
      if (n) dst[out < n ? out : n - 1] = 0; \
      return -1;                \
    }                           \
    dst[out++] = (mw_wchar)(ch); \
  } while (0)
  while (*fmt != 0) {
    char spec[32], tmp[400];
    size_t k = 0;
    int lcount = 0, width = -1, prec = -1, left = 0;
    const char* p;
    if (*fmt != '%') {
      PUT(*fmt++);
      continue;
    }
    fmt++;
    if (*fmt == '%') {
      PUT(*fmt++);
      continue;
    }
    spec[k++] = '%';
    while (*fmt && strchr("-+ #0", (int)*fmt) && *fmt < 0x80 && k < 20) {
      if (*fmt == '-') left = 1;
      spec[k++] = (char)*fmt++;
    }
    if (*fmt == '*') {
      width = va_arg(ap, int);
      fmt++;
      k += (size_t)snprintf(spec + k, 8, "%d", width);
    } else {
      width = 0;
      while (*fmt >= '0' && *fmt <= '9' && k < 24) {
        width = width * 10 + (*fmt - '0');
        spec[k++] = (char)*fmt++;
      }
    }
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      if (*fmt == '*') {
        prec = va_arg(ap, int);
        fmt++;
      } else {
        while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
      }
      k += (size_t)snprintf(spec + k, 8, ".%d", prec);
    }
    while (*fmt == 'l' || *fmt == 'h' || *fmt == 'L') {
      if (*fmt == 'l') lcount++;
      fmt++;
    }
    if (*fmt == 0) break;
    tmp[0] = 0;
    switch (*fmt) {
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
        if (lcount >= 2) {
          spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = (char)*fmt; spec[k] = 0;
          snprintf(tmp, sizeof tmp, spec, va_arg(ap, long long));
        } else {
          spec[k++] = (char)*fmt; spec[k] = 0;
          snprintf(tmp, sizeof tmp, spec, va_arg(ap, int));
        }
        break;
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        spec[k++] = (char)*fmt; spec[k] = 0;
        snprintf(tmp, sizeof tmp, spec, va_arg(ap, double));
        break;
      case 'p':
        snprintf(tmp, sizeof tmp, "%p", va_arg(ap, void*));
        break;
      case 'c': {
        int pad = width > 1 ? width - 1 : 0, c = va_arg(ap, int);
        if (!left) while (pad-- > 0) PUT(' ');
        PUT(c);
        if (left) while (pad-- > 0) PUT(' ');
        fmt++;
        continue;
      }
      case 's':
        if (lcount) {
          const mw_wchar* w = va_arg(ap, const mw_wchar*);
          size_t len = 0;
          int pad;
          static const mw_wchar null_w[] = {'(', 'n', 'u', 'l', 'l', ')', 0};
          if (!w) w = null_w;
          while (w[len] != 0 && (prec < 0 || len < (size_t)prec)) len++;
          pad = width > (int)len ? width - (int)len : 0;
          if (!left) while (pad-- > 0) PUT(' ');
          for (k = 0; k < len; k++) PUT(w[k]);
          if (left) while (pad-- > 0) PUT(' ');
          fmt++;
          continue;
        }
        spec[k++] = 's'; spec[k] = 0;
        p = va_arg(ap, const char*);
        snprintf(tmp, sizeof tmp, spec, p ? p : "(null)");
        break;
      default:
        PUT(*fmt);
        fmt++;
        continue;
    }
    fmt++;
    for (p = tmp; *p; p++) PUT((unsigned char)*p);
  }
  if (n == 0) return -1;
  dst[out] = 0;
  return (int)out;
#undef PUT
}
