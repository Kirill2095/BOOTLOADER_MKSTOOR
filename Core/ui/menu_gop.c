/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 3: graphical boot menu renderer (docs/ARCHITECTURE.md
 * §ui). Replaces the text menu when GOP is available; the pipeline falls
 * back to mks_menu_run() otherwise, so behaviour on ancient or headless
 * firmware is unchanged.
 *
 * Layout (all geometry derived from the active theme):
 *   ┌ panel ────────────────────────────┐
 *   │  MKSTOOR          <version>       │  header + accent underline
 *   │  ▌ entry name                     │  selection bar + row_height rows
 *   │    kernel path (muted)            │
 *   │  ...                              │
 *   │  [timeout progress bar]           │  shrinks left→right as time passes
 *   │  ↑↓ select · Enter boot · click   │  footer hint line
 *   └───────────────────────────────────┘
 *
 * Input model: keyboard uses the same EFI_INPUT_KEY scancodes as the text
 * menu (0x10 up / 0x18 down / digits / Enter); pointer clicks hit-test the
 * row rectangles and a press inside a row selects it, a release inside the
 * same row boots it (press-release pairing avoids accidental activation
 * while dragging across rows).
 *
 * Redraw policy: full-frame repaint only when state changed (selection,
 * hover, second tick of the countdown, cursor movement). The LFB stays
 * owned by firmware; before handover the pipeline restores the console via
 * ConOut->ClearScreen (GOP pixels die with ExitBootServices anyway).
 */
#include <mkstoor/menu.h>
#include <mkstoor/gop.h>
#include <mkstoor/theme.h>
#include <mkstoor/font.h>
#include <mkstoor/pointer.h>
#include <mkstoor/console.h>

extern void mks_console_add_ticks(u64 ms);
extern u64  mks_console_ticks_ms(void);
extern EFI_SYSTEM_TABLE *mks_pipeline_efi_table(void); /* pipeline.c glue */

#ifndef MKSTOOR_VERSION
#define MKSTOOR_VERSION "dev"
#endif

/* --- layout cache computed once per run ---------------------------------- */
struct layout {
        u32 px, py, pw, ph;         /* panel rect                    */
        u32 row_x, row_y0;          /* first row content origin      */
        u32 row_h;                  /* per-entry row pitch           */
        u32 text_dx;                /* label x offset inside a row   */
        u32 bar_y;                  /* timeout bar top               */
};

static struct layout g_lay;
static const struct mks_theme *g_th;
static const struct mks_font *g_font;
static const struct mks_gop_info *g_gi;

static void layout_compute(const struct mks_menu *menu, struct layout *L)
{
        u32 sw = g_gi->width, sh = g_gi->height;
        u32 pw = sw * g_th->panel_width_ratio_pct / 100;
        if (pw < 320 && sw >= 320)
                pw = MIN(320u, sw);                 /* tiny screens: full width */
        u32 pad = g_th->padding;
        u32 hdr = mks_font_line_height(g_font, g_th->font_scale) + pad;
        u32 ftr = mks_font_line_height(g_font, g_th->font_scale) + pad;
        u32 rows = (u32)menu->n_entries * g_th->row_height;
        u32 ph = hdr + rows + g_th->row_height / 2 + ftr;
        if (ph > sh - 2 * pad)
                ph = sh - 2 * pad;

        L->px = (sw - pw) / 2;
        L->py = (sh - ph) / 2;
        L->pw = pw;
        L->ph = ph;
        L->row_x = L->px + pad * 2;
        L->row_y0 = L->py + hdr;
        L->row_h = g_th->row_height;
        L->text_dx = 12;                            /* gap after accent bar */
        L->bar_y = L->row_y0 + rows + g_th->row_height / 4;
}

/* Rounded-corner card: fill rect then punch quarter-circles of background
 * colour out of each corner. Cheap and exact for our axis-aligned case. */
static void draw_panel_card(void)
{
        const struct layout *L = &g_lay;
        u32 r = g_th->corner_radius;
        mks_gop_fill_rect(L->px, L->py, L->pw, L->ph, g_th->panel_bg);
        if (r == 0) {
                mks_gop_frame(L->px, L->py, L->pw, L->ph, g_th->panel_border);
                return;
        }
        /* corners: for each pixel in the r×r box, if outside the circle,
         * paint desktop colour instead. */
        static const u32 cx[4] = {0, 1, 0, 1};      /* 0=left 1=right  */
        static const u32 cy[4] = {0, 0, 1, 1};      /* 0=top  1=bottom */
        for (int k = 0; k < 4; k++) {
                u32 bx = cx[k] ? L->px + L->pw - r : L->px;
                u32 by = cy[k] ? L->py + L->ph - r : L->py;
                i32 mx = cx[k] ? (i32)(bx + r - 1) : (i32)bx; /* centre x */
                i32 my = cy[k] ? (i32)(by + r - 1) : (i32)by; /* centre y */
                for (u32 dy = 0; dy < r; dy++)
                        for (u32 dx = 0; dx < r; dx++) {
                                i32 ddx = (i32)(bx + dx) - mx;
                                i32 ddy = (i32)(by + dy) - my;
                                if (ddx * ddx + ddy * ddy >
                                    (i32)(r * r) - (i32)r)
                                        mks_gop_put(bx + dx, by + dy,
                                                    g_th->background);
                        }
        }
        mks_gop_frame(L->px, L->py, L->pw, L->ph, g_th->panel_border);
}

static void draw_header(void)
{
        const struct layout *L = &g_lay;
        u32 y = L->py + g_th->padding / 2;
        u32 adv = mks_font_draw_text(g_font, L->px + g_th->padding, y,
                                     "MKSTOOR", g_th->accent,
                                     g_th->panel_bg, L->pw - 2 * g_th->padding);
        char tail[48];
        strncpy_safe(tail, sizeof(tail), MKSTOOR_VERSION);
        usize tl = strlen(tail);
        if (tl) {
                /* right-align version string */
                u32 tw = mks_font_text_width(g_font, tail);
                if (tw < L->pw - adv)
                        mks_font_draw_text(g_font,
                                           L->px + L->pw - g_th->padding - tw,
                                           y, tail, g_th->muted,
                                           g_th->panel_bg, 0);
        }
        /* accent underline across the card */
        mks_gop_fill_rect(L->px + g_th->padding,
                          L->row_y0 - g_th->padding / 2,
                          L->pw - 2 * g_th->padding, 2, g_th->accent);
}

static bool row_contains(u32 row, i32 x, i32 y)
{
        u32 ry = g_lay.row_y0 + row * g_lay.row_h;
        return x >= (i32)g_lay.px && x < (i32)(g_lay.px + g_lay.pw) &&
               y >= (i32)ry && y < (i32)(ry + g_lay.row_h);
}

static void draw_row(const struct mks_menu *menu, u32 row, bool selected)
{
        const struct mks_menu_entry *e = &menu->entries[row];
        u32 ry = g_lay.row_y0 + row * g_lay.row_h;
        mks_rgb_t bg = selected ? g_th->accent : g_th->panel_bg;
        mks_rgb_t fg = selected ? g_th->text_selected : g_th->text_normal;

        mks_gop_fill_rect(g_lay.px + 1, ry, g_lay.pw - 2, g_lay.row_h, bg);
        if (selected)
                mks_gop_vline(g_lay.px + 2, ry, g_lay.row_h,
                              g_th->text_selected);

        u32 ty = ry + (g_lay.row_h -
                      mks_font_line_height(g_font, g_th->font_scale)) / 2;
        u32 avail = g_lay.pw - 2 * g_th->padding - g_lay.text_dx;
        mks_font_draw_text(g_font, g_lay.row_x + g_lay.text_dx, ty,
                           e->name, fg, bg, avail);
}

static void draw_timeout_bar(u32 remaining_ms, u32 total_ms)
{
        const struct layout *L = &g_lay;
        u32 w = L->pw - 2 * L->row_x + 2 * L->px; /* inner width */
        w = L->pw - 2 * g_th->padding;
        u32 bh = 4;
        mks_gop_fill_rect(L->px + g_th->padding, L->bar_y, w, bh,
                          g_th->muted);
        u32 fill = total_ms ? (u32)((u64)remaining_ms * w / total_ms) : 0;
        if (fill)
                mks_gop_fill_rect(L->px + g_th->padding, L->bar_y, fill, bh,
                                  g_th->accent);
}

static void draw_footer(const struct mks_menu *menu, usize sel)
{
        const struct layout *L = &g_lay;
        u32 y = L->py + L->ph - g_th->padding -
                mks_font_line_height(g_font, g_th->font_scale) / 2;
        char line[MKS_MENU_NAME_LEN + 64];
        const char *hint = "Up/Down select | Enter boot | 1-9 jump";
        mks_font_draw_text(g_font, L->px + g_th->padding, y, hint,
                           g_th->muted, g_th->panel_bg,
                           L->pw - 2 * g_th->padding);
        UNUSED(menu); UNUSED(sel); UNUSED(line);
}

static void draw_cursor(void)
{
        if (!g_th->pointer_enabled || !mks_pointer_available())
                return;
        i32 x, y;
        mks_pointer_pos(&x, &y);
        /* 9x9 hollow diamond-ish sprite; XOR-free: two solid rects make a
         * cross that reads on both dark and light backgrounds. */
        mks_gop_fill_rect((u32)x - 4, (u32)y, 9, 1, g_th->text_selected);
        mks_gop_fill_rect((u32)x, (u32)y - 4, 1, 9, g_th->text_selected);
        mks_gop_fill_rect((u32)x - 1, (u32)y - 1, 3, 3, g_th->accent);
}

static void render(const struct mks_menu *menu, usize sel,
                   u32 remaining_ms, u32 total_ms)
{
        mks_gop_clear(g_th->background);
        draw_panel_card();
        draw_header();
        for (u32 i = 0; i < (u32)menu->n_entries; i++)
                draw_row(menu, i, i == sel);
        draw_timeout_bar(remaining_ms, total_ms);
        draw_footer(menu, sel);
        draw_cursor();
}

/* ---------------- public entry ---------------- */
mkstoor_err_t mks_menu_run_graphical(const struct mks_menu *menu,
                                     const struct mks_theme *theme,
                                     usize *out_choice)
{
        if (!menu || !theme || !out_choice || menu->n_entries == 0)
                return MKSTOOR_ERR_INVALID_ARG;
        if (!mks_gop_available())
                return MKSTOOR_ERR_NOT_FOUND;

        EFI_SYSTEM_TABLE *st = mks_pipeline_efi_table();
        if (!st)
                return MKSTOOR_ERR_GENERIC;

        g_th = theme;
        g_font = mks_font_builtin();
        g_gi = mks_gop_get();
        layout_compute(menu, &g_lay);

        usize sel = 0;
        for (usize i = 0; i < menu->n_entries; i++)
                if (menu->entries[i].default_entry)
                        sel = i;

        u32 total_ms = menu->timeout_s * 1000u;
        u64 deadline = mks_console_ticks_ms() + total_ms;
        usize last_sel = ~(usize)0;
        u32 last_sec = 0xFFFFFFFFu;
        i32 last_cx = -1, last_cy = -1;
        usize press_row = ~(usize)0;   /* row under an active button press */

        for (;;) {
                bool dirty = false;

                /* keyboard */
                EFI_INPUT_KEY key;
                EFI_STATUS ke = st->ConIn->ReadKeyStroke(st->ConIn, &key);
                if (ke == EFI_SUCCESS) {
                        if (key.WideChar >= '1' && key.WideChar <= '9') {
                                usize n = (usize)(key.WideChar - '0');
                                if (n <= menu->n_entries) {
                                        sel = n - 1;
                                        dirty = true;
                                }
                        } else if (key.ScanCode == 0x10) { /* up */
                                sel = (sel + menu->n_entries - 1) %
                                      menu->n_entries;
                                dirty = true;
                        } else if (key.ScanCode == 0x18) { /* down */
                                sel = (sel + 1) % menu->n_entries;
                                dirty = true;
                        } else if (key.WideChar == '\r' ||
                                   key.WideChar == '\n') {
                                break;
                        }
                }

                /* pointer */
                struct mks_pointer_event pe;
                if (mks_pointer_poll(&pe)) {
                        dirty = true;
                        if (pe.pressed) {
                                press_row = ~(usize)0;
                                for (u32 i = 0; i < (u32)menu->n_entries; i++)
                                        if (row_contains(i, pe.x, pe.y))
                                                press_row = i;
                        }
                        if (pe.released && press_row != ~(usize)0 &&
                            row_contains(press_row, pe.x, pe.y)) {
                                sel = press_row;
                                press_row = ~(usize)0;
                                break;          /* click-to-boot like GRUB GUI */
                        }
                        if (pe.moved && press_row == ~(usize)0) {
                                for (u32 i = 0; i < (u32)menu->n_entries; i++)
                                        if (row_contains(i, pe.x, pe.y) &&
                                            (usize)i != sel) {
                                                sel = i;      /* hover-select */
                                                dirty = true;
                                                break;
                                        }
                        }
                }

                /* timeout */
                u64 now = mks_console_ticks_ms();
                u32 remaining = now >= deadline ? 0
                                                 : (u32)(deadline - now);
                u32 sec = remaining / 1000u;
                if (sec != last_sec) {
                        last_sec = sec;
                        dirty = true;           /* countdown bar moves each s */
                }

                i32 cx, cy;
                mks_pointer_pos(&cx, &cy);
                if (cx != last_cx || cy != last_cy) {
                        last_cx = cx; last_cy = cy;
                        dirty = true;           /* repaint to move sprite */
                }

                if (sel != last_sel) {
                        last_sel = sel;
                        dirty = true;
                }
                if (dirty)
                        render(menu, sel, remaining, total_ms);

                if (remaining == 0)
                        break;

                st->BootServices->Stall(10000);   /* 10 ms cadence */
                mks_console_add_ticks(10);
        }

        *out_choice = sel;
        return MKSTOOR_OK;
}
