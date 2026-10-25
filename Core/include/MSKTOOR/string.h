/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — freestanding string/memory surface (core/string.c).
 *
 * The EFI image links with -nostdlib; these are the ONLY libc primitives we
 * implement and the ONLY ones other translation units may call. <string.h>
 * declarations are compatible so host-side unit tests can link against the
 * very same objects without a shim.
 */
#ifndef MKSTOOR_STRING_H
#define MKSTOOR_STRING_H

#include <mkstoor/types.h>
#include <stdarg.h>

MKSTOOR_API void *memcpy(void *dst, const void *src, usize n);
MKSTOOR_API void *memmove(void *dst, const void *src, usize n);
MKSTOOR_API void *memset(void *dst, int c, usize n);
MKSTOOR_API int   memcmp(const void *a, const void *b, usize n);

MKSTOOR_API usize strlen(const char *s);
MKSTOOR_API usize strnlen_safe(const char *s, usize max);
MKSTOOR_API int   strcmp(const char *a, const char *b);
MKSTOOR_API int   strncmp(const char *a, const char *b, usize n);
MKSTOOR_API char *strchr(const char *s, int c);

/* Copy src into dst[cap]; always NUL-terminates when cap > 0.
 * Returns the number of chars copied (excl. NUL). This is the ONLY string
 * copy primitive allowed in the tree — plain strncpy is banned because it
 * does not guarantee termination. */
MKSTOOR_API usize strncpy_safe(char *dst, usize cap, const char *src);

/* Minimal vsnprintf (see core/string.c for the supported conversion set).
 * Always NUL-terminates out[] when cap > 0; returns the would-be length
 * (which may exceed cap-1, like C99). */
MKSTOOR_API int mks_vsnprintf(char *out, usize cap, const char *fmt,
                              va_list ap);
MKSTOOR_API int mks_snprintf(char *out, usize cap, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

#endif /* MKSTOOR_STRING_H */
