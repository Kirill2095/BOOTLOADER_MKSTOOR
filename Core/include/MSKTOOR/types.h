/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — fundamental types and error codes.
 *
 * Design rules (docs/CODING_STYLE.md):
 *   - Every fallible function returns mkstoor_err_t explicitly.
 *   - No untracked allocation: callers own buffers they pass in.
 *   - All public symbols carry MKSTOOR_API / MKSTOOR_LOCAL visibility.
 */
#ifndef MKSTOOR_TYPES_H
#define MKSTOOR_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;
typedef size_t   usize;

#define MKSTOOR_API   __attribute__((visibility("default")))
#define MKSTOOR_LOCAL __attribute__((visibility("hidden")))

/* ------------------------------------------------------------------------ */
/* Error model                                                               */
/* ------------------------------------------------------------------------ */
typedef enum mkstoor_err {
        MKSTOOR_OK                  = 0,
        MKSTOOR_ERR_GENERIC         = 1, /* catch-all, must be refined over time */
        MKSTOOR_ERR_NOT_IMPLEMENTED = 2, /* roadmap item not built yet           */
        MKSTOOR_ERR_NO_MEMORY       = 3,
        MKSTOOR_ERR_INVALID_ARG     = 4,
        MKSTOOR_ERR_NOT_FOUND       = 5,
        MKSTOOR_ERR_IO              = 6, /* firmware block-io / device failure   */
        MKSTOOR_ERR_CORRUPT       = 7, /* on-disk structure failed validation  */
        MKSTOOR_ERR_UNSUPPORTED   = 8, /* valid but beyond our feature set     */
        MKSTOOR_ERR_SECURITY      = 9, /* signature / bounds / policy failure  */
        MKSTOOR_ERR_BUFFER_TOO_SMALL = 10,
} mkstoor_err_t;

#define MKSTOOR_IS_OK(e)   ((e) == MKSTOOR_OK)
#define MKSTOOR_IS_ERR(e)  ((e) != MKSTOOR_OK)

/* Propagation helper mirroring Rust's `?` operator. */
#define MKS_RETURN_IF_ERR(expr)                                    \
        do {                                                       \
                mkstoor_err_t __e = (expr);                        \
                if (MKSTOOR_IS_ERR(__e))                           \
                        return __e;                                \
        } while (0)

MKSTOOR_API const char *mkstoor_strerror(mkstoor_err_t err);

/* ------------------------------------------------------------------------ */
/* Small compile-time utilities                                              */
/* ------------------------------------------------------------------------ */
#define ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((typeof(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((typeof(x))(a) - 1))
#define MIN(a, b)      ((a) < (b) ? (a) : (b))
#define MAX(a, b)      ((a) > (b) ? (a) : (b))

#define UNUSED(x) ((void)(x))

#endif /* MKSTOOR_TYPES_H */
