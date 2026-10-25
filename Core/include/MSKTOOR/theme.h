/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 3: theme engine.
 *
 * A theme is a resolved colour/geometry descriptor for the graphical menu.
 * Defaults are compiled in ("mkstoor-dark" palette); the [theme] table of
 * mkstoor.toml may override individual keys, and Lua scripts (Stage 3
 * scripting module) may override them at runtime through the UI capability
 * vector. Colour values accept "#rrggbb", "rrggbb" and 0xRRGGBB integers.
 */
#ifndef MKSTOOR_THEME_H
#define MKSTOOR_THEME_H

#include <mkstoor/types.h>
#include <mkstoor/gop.h>
#include <mkstoor/toml.h>

#define MKS_THEME_NAME_LEN 32

struct mks_theme {
        char     name[MKS_THEME_NAME_LEN];
        mks_rgb_t background;      /* desktop fill                */
        mks_rgb_t panel_bg;        /* menu card                   */
        mks_rgb_t panel_border;
        mks_rgb_t text_normal;
        mks_rgb_t text_selected;
        mks_rgb_t accent;          /* selection bar / progress    */
        mks_rgb_t muted;           /* hints, timeout countdown    */

        /* Geometry, all in pixels unless noted. */
        u32 panel_width_ratio_pct; /* card width / screen width   */
        u32 row_height;            /* one entry slot              */
        u32 padding;               /* inner card padding          */
        u32 corner_radius;         /* rounded-card radius         */
        u32 font_scale;            /* 1 = base bitmap font        */
        bool show_logo;
        bool pointer_enabled;      /* mouse/touch interaction     */
};

/* Fill `out` with the built-in defaults. Always succeeds. */
MKSTOOR_API void mks_theme_default(struct mks_theme *out);

/* Overlay recognised [theme] keys from a parsed TOML document. Unknown
 * keys are ignored (forward-compatible), malformed values produce a
 * warning through the console subsystem and keep the previous value.
 * Returns MKSTOOR_OK, or MKSTOOR_ERR_INVALID_ARG on NULL arguments. */
MKSTOOR_API mkstoor_err_t mks_theme_from_toml(struct mks_theme *th,
                                              const struct mks_toml_doc *doc);

/* Parse one CSS-style colour token into mks_rgb_t. Exported for tests. */
MKSTOOR_API mkstoor_err_t mks_theme_parse_color(const char *s, mks_rgb_t *out);

#endif /* MKSTOOR_THEME_H */
