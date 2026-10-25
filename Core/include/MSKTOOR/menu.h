/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: menu & boot pipeline orchestration. */
#ifndef MKSTOOR_MENU_H
#define MKSTOOR_MENU_H

#include <mkstoor/types.h>
#include <mkstoor/toml.h>

#define MKS_MENU_MAX_ENTRIES 16
#define MKS_MENU_NAME_LEN    64
#define MKS_MENU_PATH_LEN    192

struct mks_menu_entry {
        char name[MKS_MENU_NAME_LEN];     /* label shown in the menu   */
        char kernel[MKS_MENU_PATH_LEN];   /* path on its device        */
        char initrd[MKS_MENU_PATH_LEN];   /* may be empty              */
        char cmdline[MKS_MENU_PATH_LEN];  /* appended to global        */
        bool default_entry;
};

struct mks_menu {
        struct mks_menu_entry entries[MKS_MENU_MAX_ENTRIES];
        usize n_entries;
        u32 timeout_s;
        char global_cmdline[MKS_MENU_PATH_LEN];
};

/* Build a menu from a parsed TOML document (docs/CONFIG_FORMAT.md). */
MKSTOOR_API mkstoor_err_t mks_menu_from_toml(const struct mks_toml_doc *doc,
                                             struct mks_menu *out);

/* Render the text-mode menu and return the chosen index (Stage 3 replaces
 * this with the GOP renderer). Blocks until selection or timeout. */
MKSTOOR_API mkstoor_err_t mks_menu_run(const struct mks_menu *menu,
                                       usize *out_choice);

/* Stage 3: graphical (GOP + theme + pointer) menu. Returns
 * MKSTOOR_ERR_NOT_FOUND when no framebuffer is available so callers can
 * fall back to mks_menu_run(). `theme` may be NULL for built-in defaults. */
struct mks_theme;
MKSTOOR_API mkstoor_err_t mks_menu_run_graphical(const struct mks_menu *menu,
                                                 const struct mks_theme *theme,
                                                 usize *out_choice);

#endif /* MKSTOOR_MENU_H */
