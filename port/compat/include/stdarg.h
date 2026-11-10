/* PC replacement for include/libc/stdarg.h, whose non-Metrowerks branch still
 * uses the PowerPC va_list helper. Found first because port/compat/include is
 * ahead of include/libc on the include path. */
#ifndef MKGP2PC_STDARG_H
#define MKGP2PC_STDARG_H
#define _STDARG_H_ /* keep include/libc/stdarg.h out if something reaches it */

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, l) __builtin_va_arg(v, l)
#define va_copy(d, s) __builtin_va_copy(d, s)

#endif
