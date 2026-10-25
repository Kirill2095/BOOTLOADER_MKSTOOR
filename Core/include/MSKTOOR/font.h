/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 3: built-in bitmap font (6x8 grid cells).
 *
 * The renderer needs text before any external font file can be trusted, so
 * we ship a compact procedural 6x8 glyph set covering printable ASCII
 * (0x20..0x7E). Each glyph is six columns of an 8-bit column bitmap
 * (LSB = top row). This is deliberately not a pixel-perfect clone of the
 * classic IBM VGA ROM font — it is our own legible design, generated from
 * stroke tables in ui/font_data.c and validated by tests/font_check.c on
 * the host (every glyph must have >= 1 lit pixel except space).
 *
 * A PSF/PSF2 loader will supersede this table once distro console fonts
 * need to be honoured (roadmap stage 4); the API below already supports
 * swapping the active face without touching call sites.
 */
#ifndef MKSTOOR_FONT_H
#define MKSTOOR_FONT_H

#include <mkstoor/types.h>
#include <mkstoor/gop.h>

#define MKS_FONT_WIDTH  6
#define MKS_FONT_HEIGHT 8
#define MKS_FONT_FIRST  0x20   /* first printable codepoint        */
#define MKS_FONT_LAST   0x7E   /* last one                         */

struct mks_font {
        const char *name;
        u8 width, height;
        /* height/8 rounded up, times width bytes per glyph, row-major. */
        const u8 *bits;
};

MKSTOOR_API const struct mks_font *mks_font_builtin(void);

/* Draw one codepoint at (x,y) with `fg` over `bg`; non-fatal for glyphs
 * outside the covered range (draws a hollow replacement box instead). */
MKSTOOR_API void mks_font_draw_char(const struct mks_font *f, u32 x, u32 y,
                                    char c, mks_rgb_t fg, mks_rgb_t bg);

/* Draw a NUL-terminated string; returns the advance width in pixels.
 * `max_w` clips drawing at the right edge (0 = unlimited). */
MKSTOOR_API u32 mks_font_draw_text(const struct mks_font *f, u32 x, u32 y,
                                   const char *s, mks_rgb_t fg, mks_rgb_t bg,
                                   u32 max_w);

/* Text metrics helpers used for layout. */
MKSTOOR_API u32 mks_font_text_width(const struct mks_font *f, const char *s);
MKSTOOR_API u32 mks_font_line_height(const struct mks_font *f, u32 scale);

#endif /* MKSTOOR_FONT_H */
