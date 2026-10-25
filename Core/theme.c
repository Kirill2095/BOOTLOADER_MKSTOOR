/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 3: theme engine (see mkstoor/theme.h).
 *
 * Layering: compiled-in defaults ("mkstoor-dark") -> [theme] table from
 * mkstoor.toml -> runtime overrides via the Lua UI capability (scripting/).
 * Each layer only touches keys it actually specifies, so a partial config
 * never loses the rest of the palette. Colour tokens are CSS-style hex:
 * "#rrggbb", "rrggbb" or an integer literal 0xRRGGBB in TOML.
 */
#include <mkstoor/theme.h>
#include <mkstoor/console.h>

static int hexval(char c)
{
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
}

mkstoor_err_t mks_theme_parse_color(const char *s, mks_rgb_t *out)
{
        if (!s || !out)
                return MKSTOOR_ERR_INVALID_ARG;

        /* Skip optional "0x"/"0X" prefix (integer form from TOML). */
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                s += 2;
        if (*s == '#')
                s++;

        usize n = 0;
        u32 v = 0;
        for (; s[n]; n++) {
                int h = hexval(s[n]);
                if (h < 0)
                        return MKSTOOR_ERR_CORRUPT;
                if (n >= 8)
                        return MKSTOOR_ERR_CORRUPT; /* absurdly long */
                v = (v << 4) | (u32)h;
        }
        if (n != 6)
                return MKSTOOR_ERR_CORRUPT; /* must be exactly rrggbb */
        *out = v & 0xFFFFFFu;
        return MKSTOOR_OK;
}

void mks_theme_default(struct mks_theme *out)
{
        if (!out)
                return;
        strncpy_safe(out->name, sizeof(out->name), "mkstoor-dark");
        out->background     = mks_rgb(0x10, 0x14, 0x1b); /* near-black   */
        out->panel_bg       = mks_rgb(0x1b, 0x22, 0x2e); /* slate card   */
        out->panel_border   = mks_rgb(0x3a, 0x46, 0x58);
        out->text_normal    = mks_rgb(0xc8, 0xd2, 0xe0);
        out->text_selected  = mks_rgb(0xff, 0xff, 0xff);
        out->accent         = mks_rgb(0x4f, 0xa3, 0xff); /* MKSTOOR blue */
        out->muted          = mks_rgb(0x6b, 0x7a, 0x90);

        out->panel_width_ratio_pct = 60;
        out->row_height            = 32;
        out->padding               = 16;
        out->corner_radius         = 8;
        out->font_scale            = 2;
        out->show_logo             = true;
        out->pointer_enabled       = true;
}

/* --- helpers over the parsed doc ---------------------------------------- */
static const struct mks_toml_value *theme_key(const struct mks_toml_doc *doc,
                                              const char *key)
{
        char path[MKS_TOML_MAX_PATH];
        usize kl = strlen(key);
        if (kl + 8 >= sizeof(path))
                return NULL;
        memcpy(path, "theme.", 6);
        memcpy(path + 6, key, kl + 1);
        return mks_toml_get(doc, path);
}

static void apply_color(const struct mks_toml_doc *doc, const char *key,
                        mks_rgb_t *dst)
{
        const struct mks_toml_value *v = theme_key(doc, key);
        if (!v)
                return;
        mks_rgb_t c;
        if (v->kind == MKS_TOML_STR &&
            MKSTOOR_IS_OK(mks_theme_parse_color(v->str, &c))) {
                *dst = c;
                return;
        }
        if (v->kind == MKS_TOML_INT && (u64)v->i <= 0xFFFFFF) {
                *dst = (mks_rgb_t)v->i;
                return;
        }
        mks_warn("theme", "ignoring malformed colour for '%s'", key);
}

static void apply_u32(const struct mks_toml_doc *doc, const char *key,
                      u32 *dst)
{
        const struct mks_toml_value *v = theme_key(doc, key);
        if (v && v->kind == MKS_TOML_INT && v->i >= 0)
                *dst = (u32)v->i;
}

static void apply_bool(const struct mks_toml_doc *doc, const char *key,
                       bool *dst)
{
        const struct mks_toml_value *v = theme_key(doc, key);
        if (v && v->kind == MKS_TOML_BOOL)
                *dst = v->b;
}

mkstoor_err_t mks_theme_from_toml(struct mks_theme *th,
                                  const struct mks_toml_doc *doc)
{
        if (!th || !doc)
                return MKSTOOR_ERR_INVALID_ARG;

        const struct mks_toml_value *name = theme_key(doc, "name");
        if (name && name->kind == MKS_TOML_STR)
                strncpy_safe(th->name, sizeof(th->name), name->str);

        apply_color(doc, "background",     &th->background);
        apply_color(doc, "panel_bg",       &th->panel_bg);
        apply_color(doc, "panel_border",   &th->panel_border);
        apply_color(doc, "text_normal",    &th->text_normal);
        apply_color(doc, "text_selected",  &th->text_selected);
        apply_color(doc, "accent",         &th->accent);
        apply_color(doc, "muted",          &th->muted);

        apply_u32(doc, "panel_width",      &th->panel_width_ratio_pct);
        apply_u32(doc, "row_height",       &th->row_height);
        apply_u32(doc, "padding",          &th->padding);
        apply_u32(doc, "corner_radius",    &th->corner_radius);
        apply_u32(doc, "font_scale",       &th->font_scale);
        apply_bool(doc, "show_logo",       &th->show_logo);
        apply_bool(doc, "pointer",         &th->pointer_enabled);

        /* Sanity clamps: a hostile/garbled config must not take down the
         * renderer. Width ratio is a percentage; scale at least 1. */
        if (th->panel_width_ratio_pct < 20) th->panel_width_ratio_pct = 20;
        if (th->panel_width_ratio_pct > 100) th->panel_width_ratio_pct = 100;
        if (th->row_height < 12)  th->row_height = 12;
        if (th->row_height > 256) th->row_height = 256;
        if (th->padding > 128)    th->padding = 128;
        if (th->corner_radius > 64) th->corner_radius = 64;
        if (th->font_scale == 0)  th->font_scale = 1;
        if (th->font_scale > 8)   th->font_scale = 8;

        mks_info("theme", "active theme '%s'", th->name);
        return MKSTOOR_OK;
}
