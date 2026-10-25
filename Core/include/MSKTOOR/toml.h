/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: TOML configuration parser (subset).
 *
 * Supported grammar (docs/CONFIG_FORMAT.md):
 *   top-level keys/values, [table] and [[array-of-tables]] headers,
 *   strings "..." (with \" \\ \n escapes), integers, booleans, arrays of
 *   scalars, comments (#...), dotted keys (a.b = ...).
 * Everything else is rejected with MKSTOOR_ERR_CORRUPT — no silent skip.
 * Parser is pure C, allocation-free (fixed arena), fuzz-testable.
 */
#ifndef MKSTOOR_TOML_H
#define MKSTOOR_TOML_H

#include <mkstoor/types.h>

#define MKS_TOML_MAX_ENTRIES 128
#define MKS_TOML_MAX_PATH    96   /* "menu.entries[3].options.initrd" */
#define MKS_TOML_MAX_STR     512

typedef enum {
        MKS_TOML_STR,
        MKS_TOML_INT,
        MKS_TOML_BOOL,
} mks_toml_kind_t;

struct mks_toml_value {
        mks_toml_kind_t kind;
        const char *str;      /* valid for STR (arena-owned, NUL-terminated) */
        i64         i;        /* valid for INT                               */
        bool        b;        /* valid for BOOL                              */
};

struct mks_toml_doc {
        char path[MKS_TOML_MAX_ENTRIES][MKS_TOML_MAX_PATH];
        struct mks_toml_value vals[MKS_TOML_MAX_ENTRIES];
        usize n;
        char *arena;          /* string storage (caller-freed via doc_arena) */
        usize arena_used;
        usize arena_cap;
};

/* Parse `text` (not mutated? we do mutate: pass a writable copy) into doc.
 * doc->arena must be pre-zeroed with arena buffer attached. */
MKSTOOR_API mkstoor_err_t mks_toml_parse(char *writable_text, usize len,
                                         struct mks_toml_doc *doc);

MKSTOOR_API const struct mks_toml_value *
mks_toml_get(const struct mks_toml_doc *doc, const char *dotted_key);

#endif /* MKSTOOR_TOML_H */
