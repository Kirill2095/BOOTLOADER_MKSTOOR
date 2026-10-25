/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: TOML parser (subset grammar; see mkstoor/toml.h).
 * The pipeline glue in pipeline.c depends on these entry points, so they
 * live in their own translation unit for host-side fuzz/unit testing.
 */
#include <mkstoor/toml.h>

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static bool is_keych(char c)
{
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
}

void strncpy_safe(char *dst, usize cap, const char *src)
{
        usize i = 0;
        for (; i + 1 < cap && src[i]; i++)
                dst[i] = src[i];
        dst[i] = '\0';
}

static mkstoor_err_t add_entry(struct mks_toml_doc *doc, const char *path,
                               struct mks_toml_value v)
{
        if (doc->n >= MKS_TOML_MAX_ENTRIES)
                return MKSTOOR_ERR_NO_MEMORY;
        if (strlen(path) >= MKS_TOML_MAX_PATH)
                return MKSTOOR_ERR_CORRUPT;
        strncpy_safe(doc->path[doc->n], MKS_TOML_MAX_PATH, path);
        doc->vals[doc->n] = v;
        doc->n++;
        return MKSTOOR_OK;
}

/* Parse one value token starting at *pp; advances *pp past it. Strings are
 * unescaped in place (never grows), integers and booleans parsed directly. */
static mkstoor_err_t parse_value(char **pp, struct mks_toml_value *v)
{
        char *p = *pp;
        while (*p && is_ws(*p))
                p++;

        if (*p == '"') {
                p++;
                char *start = p, *w = p;
                while (*p && *p != '"') {
                        if (*p == '\\') {
                                p++;
                                switch (*p) {
                                case 'n':  *w++ = '\n'; break;
                                case 't':  *w++ = '\t'; break;
                                case '"':  *w++ = '"';  break;
                                case '\\': *w++ = '\\'; break;
                                default:   return MKSTOOR_ERR_CORRUPT;
                                }
                                if (!*p)
                                        return MKSTOOR_ERR_CORRUPT;
                                p++;
                        } else {
                                *w++ = *p++;
                        }
                        if ((usize)(w - start) >= MKS_TOML_MAX_STR)
                                return MKSTOOR_ERR_BUFFER_TOO_SMALL;
                }
                if (*p != '"')
                        return MKSTOOR_ERR_CORRUPT;
                *w = 0;
                p++;
                v->kind = MKS_TOML_STR;
                v->str = start;
                *pp = p;
                return MKSTOOR_OK;
        }

        if (strncmp(p, "true", 4) == 0) {
                v->kind = MKS_TOML_BOOL; v->b = true;  *pp = p + 4; return MKSTOOR_OK;
        }
        if (strncmp(p, "false", 5) == 0) {
                v->kind = MKS_TOML_BOOL; v->b = false; *pp = p + 5; return MKSTOOR_OK;
        }

        i64 val = 0;
        bool neg = false;
        char *q = p;
        if (*q == '-') { neg = true; q++; }
        if (!(q[0] >= '0' && q[0] <= '9'))
                return MKSTOOR_ERR_CORRUPT;
        while (q[0] >= '0' && q[0] <= '9')
                val = val * 10 + (*q++ - '0');
        v->kind = MKS_TOML_INT;
        v->i = neg ? -val : val;
        *pp = q;
        return MKSTOOR_OK;
}

mkstoor_err_t mks_toml_parse(char *text, usize len, struct mks_toml_doc *doc)
{
        UNUSED(len);
        doc->n = 0;
        doc->arena_used = 0;

        char table[MKS_TOML_MAX_PATH] = "";
        char keypath[MKS_TOML_MAX_PATH];
        char *p = text;

        while (*p) {
                while (*p == '\n' || *p == '\r' || is_ws(*p))
                        p++;
                if (!*p)
                        break;
                if (*p == '#') {                    /* comment line */
                        while (*p && *p != '\n')
                                p++;
                        continue;
                }

                if (*p == '[') {                    /* [table] or [[array]] */
                        bool array = (p[1] == '[');
                        p += array ? 2 : 1;
                        char *name = p;
                        while (*p && *p != ']') {
                                if (!is_keych(*p) && *p != '.')
                                        return MKSTOOR_ERR_CORRUPT;
                                p++;
                        }
                        if (!*p)
                                return MKSTOOR_ERR_CORRUPT;
                        *p++ = 0;
                        if (array) {
                                if (*p++ != ']')
                                        return MKSTOOR_ERR_CORRUPT;
                        }
                        usize nl = strlen(name);
                        if (nl + 1 >= sizeof(table))
                                return MKSTOOR_ERR_CORRUPT;
                        strncpy_safe(table, sizeof(table), name);
                        /* append '.' separator lazily when building key paths */
                        table[nl] = '.';
                        table[nl + 1] = 0;
                        continue;
                }

                /* key = value */
                if (!is_keych(*p))
                        return MKSTOOR_ERR_CORRUPT;
                char *key = p;
                while (*p && is_keych(*p))
                        p++;
                if (!*p)
                        return MKSTOOR_ERR_CORRUPT;
                *p++ = 0;
                while (*p && is_ws(*p))
                        p++;
                if (*p != '=')
                        return MKSTOOR_ERR_CORRUPT;
                p++;

                struct mks_toml_value v;
                MKS_RETURN_IF_ERR(parse_value(&p, &v));

                while (*p && is_ws(*p))
                        p++;
                if (*p == '#') {
                        while (*p && *p != '\n')
                                p++;
                } else if (*p && *p != '\n') {
                        return MKSTOOR_ERR_CORRUPT;     /* trailing garbage */
                }

                usize tl = strlen(table), kl = strlen(key);
                if (tl + kl + 1 >= sizeof(keypath))
                        return MKSTOOR_ERR_CORRUPT;
                memcpy(keypath, table, tl);
                memcpy(keypath + tl, key, kl + 1);
                MKS_RETURN_IF_ERR(add_entry(doc, keypath, v));
        }
        return MKSTOOR_OK;
}

const struct mks_toml_value *
mks_toml_get(const struct mks_toml_doc *doc, const char *dotted_key)
{
        for (usize i = 0; i < doc->n; i++)
                if (strcmp(doc->path[i], dotted_key) == 0)
                        return &doc->vals[i];
        return NULL;
}
