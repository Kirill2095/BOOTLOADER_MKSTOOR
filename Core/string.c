/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — freestanding string/memory helpers + tiny vsnprintf.
 * The image links with -nostdlib, so we provide the handful of libc
 * primitives the core actually uses. Kept deliberately small and simple
 * enough to audit by eye.
 */
#include <mkstoor/types.h>
#include <stdarg.h>

void *memcpy(void *dst, const void *src, usize n)
{
        u8 *d = dst;
        const u8 *s = src;
        for (usize i = 0; i < n; i++)
                d[i] = s[i];
        return dst;
}

void *memmove(void *dst, const void *src, usize n)
{
        u8 *d = dst;
        const u8 *s = src;
        if (d < s) {
                for (usize i = 0; i < n; i++)
                        d[i] = s[i];
        } else {
                for (usize i = n; i > 0; i--)
                        d[i - 1] = s[i - 1];
        }
        return dst;
}

void *memset(void *dst, int c, usize n)
{
        u8 *d = dst;
        for (usize i = 0; i < n; i++)
                d[i] = (u8)c;
        return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
        const u8 *x = a, *y = b;
        for (usize i = 0; i < n; i++) {
                if (x[i] != y[i])
                        return (int)x[i] - (int)y[i];
        }
        return 0;
}

usize strlen(const char *s)
{
        usize n = 0;
        while (s[n])
                n++;
        return n;
}

int strcmp(const char *a, const char *b)
{
        while (*a && *a == *b) {
                a++;
                b++;
        }
        return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, usize n)
{
        for (usize i = 0; i < n; i++) {
                if (a[i] != b[i])
                        return (int)(u8)a[i] - (int)(u8)b[i];
                if (!a[i])
                        return 0;
        }
        return 0;
}

char *strchr(const char *s, int c)
{
        for (; *s; s++) {
                if (*s == (char)c)
                        return (char *)s;
        }
        return (char)c == 0 ? (char *)s : NULL;
}

static usize strnlen_c(const char *s, usize max)
{
        usize n = 0;
        while (n < max && s[n])
                n++;
        return n;
}

char *strncpy(char *dst, const char *src, usize n)
{
        usize i = 0;
        for (; i + 1 < n && src[i]; i++)
                dst[i] = src[i];
        dst[i] = '\0';
        return dst;
}

/* ------------------------------------------------------------------------ */
/* Minimal vsnprintf: %s %c %d %u %x %X %lx/%llx %p %% with width and      */
/* zero-pad / alternate-# flags, up to 64-bit ints. Enough for logging.    */
/* ------------------------------------------------------------------------ */
int mks_vsnprintf(char *out, usize cap, const char *fmt, va_list ap)
{
        usize o = 0;
#define PUTCH(c) do { if (out && o + 1 < cap) out[o] = (c); o++; } while (0)

        for (; *fmt; fmt++) {
                if (*fmt != '%') {
                        PUTCH(*fmt);
                        continue;
                }
                fmt++;
                char pad = ' ';
                int width = 0;
                bool alt = false, lng = false;

                for (;; fmt++) {
                        if (*fmt == '#') { alt = true; continue; }
                        if (*fmt == '0') { pad = '0'; continue; }
                        break;
                }
                while (*fmt >= '0' && *fmt <= '9')
                        width = width * 10 + (*fmt++ - '0');
                if (*fmt == 'l') { lng = true; fmt++; }
                if (*fmt == 'l') { lng = true; fmt++; } /* %llu */

                switch (*fmt) {
                case 's': {
                        const char *s = va_arg(ap, const char *);
                        if (!s)
                                s = "(null)";
                        usize l = strnlen_c(s, 256);
                        while (width-- > (int)l)
                                PUTCH(' ');
                        for (usize i = 0; i < l; i++)
                                PUTCH(s[i]);
                        break;
                }
                case 'c':
                        PUTCH((char)va_arg(ap, int));
                        break;
                case 'd': {
                        long long v = lng ? va_arg(ap, long long)
                                          : va_arg(ap, int);
                        char tmp[24];
                        int ti = 0;
                        bool neg = v < 0;
                        unsigned long long uv = neg ? (unsigned long long)(-(v + 1)) + 1
                                                    : (unsigned long long)v;
                        do {
                                tmp[ti++] = (char)('0' + uv % 10);
                                uv /= 10;
                        } while (uv);
                        if (neg)
                                tmp[ti++] = '-';
                        while (width-- > ti)
                                PUTCH(pad);
                        for (int i = ti - 1; i >= 0; i--)
                                PUTCH(tmp[i]);
                        break;
                }
                case 'u':
                case 'x':
                case 'X': {
                        unsigned long long uv;
                        if (lng)
                                uv = va_arg(ap, unsigned long long);
                        else
                                uv = va_arg(ap, unsigned int);
                        char tmp[24];
                        int ti = 0;
                        unsigned base = (*fmt == 'u') ? 10 : 16;
                        const char *digits = (*fmt == 'X') ? "0123456789ABCDEF"
                                                           : "0123456789abcdef";
                        do {
                                tmp[ti++] = digits[uv % base];
                                uv /= base;
                        } while (uv);
                        if (alt && base == 16) {
                                tmp[ti++] = 'x';
                                tmp[ti++] = '0';
                        }
                        while (width-- > ti)
                                PUTCH(pad);
                        for (int i = ti - 1; i >= 0; i--)
                                PUTCH(tmp[i]);
                        break;
                }
                case 'p': {
                        unsigned long long uv = va_arg(ap, unsigned long long);
                        char tmp[24];
                        int ti = 0;
                        const char *digits = "0123456789abcdef";
                        tmp[ti++] = '0'; tmp[ti++] = 'x';
                        do {
                                tmp[ti++] = digits[uv & 0xf];
                                uv >>= 4;
                        } while (uv);
                        for (int i = ti - 1; i >= 0; i--)
                                PUTCH(tmp[i]);
                        break;
                }
                case '%':
                        PUTCH('%');
                        break;
                default:
                        PUTCH('%');
                        PUTCH(*fmt);
                        break;
                }
        }
        if (out && cap)
                out[MIN(o, cap - 1)] = '\0';
#undef PUTCH
        return (int)o;
}

const char *mkstoor_strerror(mkstoor_err_t err)
{
        switch (err) {
        case MKSTOOR_OK:                    return "OK";
        case MKSTOOR_ERR_GENERIC:           return "generic error";
        case MKSTOOR_ERR_NOT_IMPLEMENTED:   return "not implemented";
        case MKSTOOR_ERR_NO_MEMORY:         return "out of memory";
        case MKSTOOR_ERR_INVALID_ARG:       return "invalid argument";
        case MKSTOOR_ERR_NOT_FOUND:         return "not found";
        case MKSTOOR_ERR_IO:                return "I/O failure";
        case MKSTOOR_ERR_CORRUPT:           return "corrupted on-disk structure";
        case MKSTOOR_ERR_UNSUPPORTED:       return "unsupported feature";
        case MKSTOOR_ERR_SECURITY:          return "security violation";
        case MKSTOOR_ERR_BUFFER_TOO_SMALL:  return "buffer too small";
        }
        return "unknown error code";
}
