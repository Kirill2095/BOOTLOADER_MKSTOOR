/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — console / logging subsystem (Stage 1 foundation). */
#ifndef MKSTOOR_CONSOLE_H
#define MKSTOOR_CONSOLE_H

#include <mkstoor/types.h>
#include <mkstoor/efi.h>

/* Log severity; MKSTOOR_LOG_LEVEL is a compile-time ceiling. */
typedef enum {
        MKS_LOG_ERROR = 0,
        MKS_LOG_WARN  = 1,
        MKS_LOG_INFO  = 2,
        MKS_LOG_DEBUG = 3,
        MKS_LOG_TRACE = 4,
} mks_log_level_t;

MKSTOOR_API void mks_console_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *conout);
MKSTOOR_API void mks_console_clear(void);

/* printf-style output (tiny builtin vsnprintf, core/string.c). */
MKSTOOR_API void mks_printf(const char *fmt, ...)
        __attribute__((format(printf, 1, 2)));

/* Kernel-style timestamped log line: "[    0.000123] sub : message". */
MKSTOOR_API void mks_log(mks_log_level_t level, const char *subsys,
                         const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

#define mks_err(subsys, fmt, ...)   mks_log(MKS_LOG_ERROR, subsys, fmt, ##__VA_ARGS__)
#define mks_warn(subsys, fmt, ...)  mks_log(MKS_LOG_WARN,  subsys, fmt, ##__VA_ARGS__)
#define mks_info(subsys, fmt, ...)  mks_log(MKS_LOG_INFO,  subsys, fmt, ##__VA_ARGS__)
#define mks_dbg(subsys, fmt, ...)   mks_log(MKS_LOG_DEBUG, subsys, fmt, ##__VA_ARGS__)

/* Blocking single-key press ("press any key"). */
MKSTOOR_API mkstoor_err_t mks_console_wait_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin);

#endif /* MKSTOOR_CONSOLE_H */
