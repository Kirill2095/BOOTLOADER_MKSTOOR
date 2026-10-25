/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — console subsystem (Stage 1 foundation). */
#include <mkstoor/console.h>
#include <mkstoor/efi.h>
#include <stdarg.h>

#ifndef MKSTOOR_LOG_LEVEL
#define MKSTOOR_LOG_LEVEL 2
#endif

extern int mks_vsnprintf(char *out, usize cap, const char *fmt, va_list ap);

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_out;
static u64 g_ticks_ms; /* monotonic-ish millisecond clock fed by BS->Stall */

void mks_console_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *conout)
{
        g_out = conout;
        if (g_out)
                g_out->ClearScreen(g_out);
}

/* Called from the timer tick path (core/timer.c) and boot services stall. */
void mks_console_add_ticks(u64 ms)
{
        g_ticks_ms += ms;
}

u64 mks_console_ticks_ms(void)
{
        return g_ticks_ms;
}

void mks_console_clear(void)
{
        if (g_out)
                g_out->ClearScreen(g_out);
}

static void out_ascii(const char *s)
{
        if (!g_out)
                return;
        CHAR16 w[128];
        usize i = 0;
        for (; *s; s++) {
                if (*s == '\n')
                        w[i++] = '\r', w[i++] = '\n';
                else if (*s == '\t')
                        w[i++] = ' ', w[i++] = ' ', w[i++] = ' ', w[i++] = ' ';
                else
                        w[i++] = (CHAR16)(u8)*s;
                if (i >= ARRAY_SIZE(w) - 2) {
                        w[i] = 0;
                        g_out->OutputString(g_out, w);
                        i = 0;
                }
        }
        if (i) {
                w[i] = 0;
                g_out->OutputString(g_out, w);
        }
}

void mks_printf(const char *fmt, ...)
{
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        mks_vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        out_ascii(buf);
}

void mks_log(mks_log_level_t level, const char *subsys, const char *fmt, ...)
{
        static const char *tag[] = { "ERR", "WRN", "INF", "DBG", "TRC" };
        if ((int)level > MKSTOOR_LOG_LEVEL)
                return;

        char msg[384];
        va_list ap;
        va_start(ap, fmt);
        mks_vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        u64 ms = g_ticks_ms;
        mks_printf("[%5llu.%06llu] %-4s: %s: %s\n",
                   ms / 1000, (ms % 1000) * 1000, subsys, tag[level], msg);
}

mkstoor_err_t mks_console_wait_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin)
{
        if (!conin)
                return MKSTOOR_ERR_NOT_IMPLEMENTED;
        EFI_INPUT_KEY key;
        for (;;) {
                EFI_STATUS st = conin->ReadKeyStroke(conin, &key);
                if (st == EFI_SUCCESS)
                        return MKSTOOR_OK;
                if (st != EFI_NOT_READY)
                        return MKSTOOR_ERR_IO;
        }
}
