/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — module registry & capability dispatch (Stage 1 skeleton,
 * fully exercised by the Stage 2 fs/crypto/exec modules).
 */
#include <mkstoor/module.h>
#include <mkstoor/memory.h>
#include <mkstoor/console.h>

#define MKS_MODULES_MAX 24

static const struct mks_module_info *g_mods[MKS_MODULES_MAX];
static usize g_nmods;

/* Capability vectors implemented by the core and handed out on grant. */
extern const struct mks_caps_fs     mks_caps_fs_impl;
extern const struct mks_caps_crypto mks_caps_crypto_impl;
extern const struct mks_caps_exec   mks_caps_exec_impl;

mkstoor_err_t mks_module_register(const struct mks_module_info *mod)
{
        if (!mod || !mod->name || !mod->init)
                return MKSTOOR_ERR_INVALID_ARG;
        for (usize i = 0; i < g_nmods; i++)
                if (g_mods[i]->name == mod->name ||
                    strcmp(g_mods[i]->name, mod->name) == 0)
                        return MKSTOOR_ERR_INVALID_ARG; /* duplicate */
        if (g_nmods >= MKS_MODULES_MAX)
                return MKSTOOR_ERR_NO_MEMORY;
        g_mods[g_nmods++] = mod;
        return MKSTOOR_OK;
}

const struct mks_module_info *mks_module_find(const char *name)
{
        for (usize i = 0; i < g_nmods; i++)
                if (strcmp(g_mods[i]->name, name) == 0)
                        return g_mods[i];
        return NULL;
}

usize mks_module_count(void)
{
        return g_nmods;
}

mkstoor_err_t mks_module_init_all(void)
{
        mkstoor_err_t first_err = MKSTOOR_OK;
        for (usize i = 0; i < g_nmods; i++) {
                const struct mks_module_info *m = g_mods[i];
                struct mks_api api = { 0 };

                /* Capability grant: only what the module declared it wants. */
                if (m->wants & MKS_MOD_FS)
                        api.fs = &mks_caps_fs_impl;
                if (m->wants & MKS_MOD_CRYPTO)
                        api.crypto = &mks_caps_crypto_impl;
                if (m->wants & MKS_MOD_EXEC)
                        api.exec = &mks_caps_exec_impl;

                mkstoor_err_t e = m->init(&api);
                if (MKSTOOR_IS_ERR(e)) {
                        mks_warn("mod", "%s init failed: %s", m->name,
                                 mkstoor_strerror(e));
                        if (first_err == MKSTOOR_OK)
                                first_err = e;
                } else {
                        mks_dbg("mod", "%s v%s ready", m->name, m->version);
                }
        }
        return first_err;
}

/* ------------------------------------------------------------------------ */
/* Tracked allocation wrappers backing the fs capability vector.            */
/* A fixed registry maps base pointer -> {pages, owner} so module frees are */
/* checked and accounted; untracked pointers are refused, not corrupted.    */
/* ------------------------------------------------------------------------ */
#define MKS_TRACK_MAX 64
struct track_rec {
        void *base;
        u64   pages;
        const char *owner;
};
static struct track_rec g_track[MKS_TRACK_MAX];

void *mks_tracked_alloc(usize size, const char *owner)
{
        if (!owner)
                owner = "core";
        u64 pages = (size + MKS_PAGE_SIZE - 1) / MKS_PAGE_SIZE;
        void *p = mks_frames_alloc(pages, owner);
        if (!p)
                return NULL;
        for (usize i = 0; i < MKS_TRACK_MAX; i++) {
                if (!g_track[i].base) {
                        g_track[i] = (struct track_rec){ p, pages, owner };
                        return p;
                }
        }
        mks_frames_free(p, pages, owner);
        return NULL; /* tracking table full: fail loudly rather than leak */
}

mkstoor_err_t mks_tracked_free(void *p)
{
        for (usize i = 0; i < MKS_TRACK_MAX; i++) {
                if (g_track[i].base == p) {
                        mks_frames_free(p, g_track[i].pages, g_track[i].owner);
                        g_track[i] = (struct track_rec){ 0 };
                        return MKSTOOR_OK;
                }
        }
        return MKSTOOR_ERR_INVALID_ARG; /* never handed out by us */
}

/* Capability vectors: implemented in blockio.c / crypto.c / boot.c. */
extern mkstoor_err_t mks_bio_read_blocks(struct mks_block_dev *dev, u64 lba,
                                         void *buf, u32 count);
extern u32  mks_bio_block_size(struct mks_block_dev *dev);
extern u64  mks_bio_block_count(struct mks_block_dev *dev);
void *mks_tracked_alloc(usize size, const char *owner);
mkstoor_err_t mks_tracked_free(void *p);

static void *caps_alloc(usize size) { return mks_tracked_alloc(size, "module"); }
static void  caps_free(void *p, usize size) { UNUSED(size); mks_tracked_free(p); }

const struct mks_caps_fs mks_caps_fs_impl = {
        .read_blocks  = mks_bio_read_blocks,
        .block_size   = mks_bio_block_size,
        .block_count  = mks_bio_block_count,
        .alloc        = caps_alloc,
        .free         = caps_free,
};
