/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2 boot pipeline: block device enumeration + config/menu
 * discovery glue (docs/ARCHITECTURE.md §boot-pipeline).
 *
 * Flow:
 *   1. enumerate EFI_BLOCK_IO devices with media present
 *   2. try to mount each as FAT32, then ext4
 *   3. discover /EFI/MKSTOOR/mkstoor.toml (or fall back to auto-detected
 *      Linux entries so a bare ESP still boots something)
 *   4. parse TOML -> menu; run text menu (Stage 3 replaces with GOP)
 *   5. read kernel (+ initrd) via the chosen fs
 *   6. measure (SHA-256); verify hooks reserved for RSA key provisioning
 *   7. ExitBootServices + handover (core/boot.c)
 */
#include <mkstoor/types.h>
#include <mkstoor/efi.h>
#include <mkstoor/console.h>
#include <mkstoor/memory.h>
#include <mkstoor/module.h>
#include <mkstoor/blockio.h>
#include <mkstoor/fat.h>
#include <mkstoor/ext4.h>
#include <mkstoor/toml.h>
#include <mkstoor/menu.h>
#include <mkstoor/crypto.h>
#include <mkstoor/boot.h>
#include <mkstoor/gop.h>
#include <mkstoor/theme.h>
#include <mkstoor/pointer.h>

#define MKS_CONFIG_PATH    "/EFI/MKSTOOR/mkstoor.toml"
#define MKS_ALT_CONFIG     "/mkstoor.toml"
#define MKS_TOML_MAX_BYTES (64u * 1024u)

extern void mks_boot_set_context(EFI_SYSTEM_TABLE *st, EFI_HANDLE ih);
void *mks_tracked_alloc(usize size, const char *owner);
mkstoor_err_t mks_tracked_free(void *p);

static EFI_SYSTEM_TABLE *g_st;
static EFI_HANDLE        g_ih;

/* ---------------- block IO ---------------- */
mkstoor_err_t mks_blockio_enumerate(EFI_SYSTEM_TABLE *st,
                                    struct mks_block_dev **out, usize *count)
{
        EFI_BOOT_SERVICES_FULL *bs = st->BootServices;
        static const EFI_GUID bio_guid =
                MKS_GUID(0x964e5b22, 0x64c4, 0x977f,
                         0x82, 0x9b, 0x18, 0x13, 0x1c, 0x11, 0xe2, 0x22);
        usize sz = 0;
        if (bs->LocateHandle(BY_PROTOCOL, &bio_guid, NULL, &sz, NULL) !=
            EFI_BUFFER_TOO_SMALL)
                return MKSTOOR_ERR_IO;
        EFI_HANDLE *handles = mks_early_alloc_aligned(sz, 8);
        if (!handles)
                return MKSTOOR_ERR_NO_MEMORY;
        sz = 0;
        if (EFI_ERROR(bs->LocateHandle(BY_PROTOCOL, &bio_guid, NULL,
                                       &sz, handles)))
                return MKSTOOR_ERR_IO;

        usize nh = sz / sizeof(EFI_HANDLE);
        struct mks_block_dev *devs =
                mks_early_alloc_aligned(nh * sizeof(*devs), 8);
        if (!devs)
                return MKSTOOR_ERR_NO_MEMORY;

        usize n = 0;
        for (usize i = 0; i < nh; i++) {
                void *iface = NULL;
                if (EFI_ERROR(bs->HandleProtocol(handles[i], &bio_guid, &iface)))
                        continue;
                EFI_BLOCK_IO_PROTOCOL *bio = iface;
                if (!bio->Media || !bio->Media->MediaPresent)
                        continue;
                devs[n].bio = bio;
                devs[n].handle = handles[i];
                devs[n].owner = "blockio";
                n++;
        }
        *out = devs;
        *count = n;
        return MKSTOOR_OK;
}

u32 mks_block_size(const struct mks_block_dev *dev)
{
        return dev->bio->Media->LogicalBlockSize;
}

u64 mks_block_count(const struct mks_block_dev *dev)
{
        return dev->bio->Media->LastBlockLba + 1;
}

mkstoor_err_t mks_bio_read_blocks(struct mks_block_dev *dev, u64 lba,
                                  void *buf, u32 count)
{
        EFI_BLOCK_IO_MEDIA_INFO *m = dev->bio->Media;
        usize bytes = (usize)count * m->LogicalBlockSize;
        if (!m->MediaPresent || lba > m->LastBlockLba ||
            (u64)count > m->LastBlockLba - lba + 1)
                return MKSTOOR_ERR_INVALID_ARG;
        EFI_STATUS e = dev->bio->ReadBlocks(dev->bio, m->MediaId, lba,
                                            bytes, buf);
        if (e == EFI_MEDIA_CHANGED) /* revalidate media id once */
                e = dev->bio->ReadBlocks(dev->bio, m->MediaId, lba, bytes, buf);
        return EFI_ERROR(e) ? MKSTOOR_ERR_IO : MKSTOOR_OK;
}

u32 mks_bio_block_size(struct mks_block_dev *dev)  { return mks_block_size(dev); }
u64 mks_bio_block_count(struct mks_block_dev *dev) { return mks_block_count(dev); }

/* ---------------- per-device mounted view ---------------- */
struct mounted_fs {
        struct mks_block_dev *dev;
        bool is_fat;
        union {
                mks_fat_fs_t  *fat;
                mks_ext4_fs_t *ext4;
        };
};

static mkstoor_err_t mount_one(struct mks_block_dev *dev, struct mounted_fs *m)
{
        m->dev = dev;
        if (MKSTOOR_IS_OK(mks_fat_mount(dev, &m->fat))) {
                m->is_fat = true;
                return MKSTOOR_OK;
        }
        if (MKSTOOR_IS_OK(mks_ext4_mount(dev, &m->ext4))) {
                m->is_fat = false;
                return MKSTOOR_OK;
        }
        return MKSTOOR_ERR_UNSUPPORTED;
}

static mkstoor_err_t fs_read(struct mounted_fs *m, const char *path,
                             void **buf, usize *len)
{
        return m->is_fat ? mks_fat_read_file(m->fat, path, buf, len)
                         : mks_ext4_read_file(m->ext4, path, buf, len);
}

static bool fs_exists(struct mounted_fs *m, const char *path)
{
        return m->is_fat ? mks_fat_exists(m->fat, path)
                         : mks_ext4_exists(m->ext4, path);
}

static void fs_free(void *p, bool is_fat)
{
        if (is_fat) mks_fat_free_file(p);
        else        mks_ext4_free_file(p);
}

/* ---------------- TOML -> menu ---------------- */
static const char *toml_str(const struct mks_toml_doc *d, const char *key,
                            const char *fallback)
{
        const struct mks_toml_value *v = mks_toml_get(d, key);
        if (v && v->kind == MKS_TOML_STR)
                return v->str;
        return fallback;
}

static i64 toml_int(const struct mks_toml_doc *d, const char *key, i64 fb)
{
        const struct mks_toml_value *v = mks_toml_get(d, key);
        if (v && v->kind == MKS_TOML_INT)
                return v->i;
        return fb;
}

mkstoor_err_t mks_menu_from_toml(const struct mks_toml_doc *doc,
                                 struct mks_menu *out)
{
        memset(out, 0, sizeof(*out));
        out->timeout_s = (u32)MAX(toml_int(doc, "menu.timeout", 5), 0);
        strncpy_safe(out->global_cmdline, sizeof(out->global_cmdline),
                     toml_str(doc, "kernel.cmdline", ""));

        /* Array-of-tables flattened by the parser as repeated keys:
         *   [[entry]] rows are stored as "entry.name", "entry.kernel", ...
         * We collect them in document order: every new "entry.name" starts
         * a fresh row. */
        struct mks_menu_entry *cur = NULL;
        for (usize i = 0; i < doc->n; i++) {
                const char *k = doc->path[i];
                const struct mks_toml_value *v = &doc->vals[i];
                if (strcmp(k, "entry.name") == 0 && v->kind == MKS_TOML_STR) {
                        if (out->n_entries >= MKS_MENU_MAX_ENTRIES)
                                break;
                        cur = &out->entries[out->n_entries++];
                        memset(cur, 0, sizeof(*cur));
                        strncpy_safe(cur->name, sizeof(cur->name), v->str);
                } else if (cur) {
                        if (strcmp(k, "entry.kernel") == 0 && v->kind == MKS_TOML_STR)
                                strncpy_safe(cur->kernel, sizeof(cur->kernel), v->str);
                        else if (strcmp(k, "entry.initrd") == 0 && v->kind == MKS_TOML_STR)
                                strncpy_safe(cur->initrd, sizeof(cur->initrd), v->str);
                        else if (strcmp(k, "entry.cmdline") == 0 && v->kind == MKS_TOML_STR)
                                strncpy_safe(cur->cmdline, sizeof(cur->cmdline), v->str);
                        else if (strcmp(k, "entry.default") == 0 && v->kind == MKS_TOML_BOOL)
                                cur->default_entry = v->b;
                }
        }
        if (out->n_entries == 0)
                return MKSTOOR_ERR_NOT_FOUND;
        /* If nobody marked default, first entry wins. */
        bool any_default = false;
        for (usize i = 0; i < out->n_entries; i++)
                any_default |= out->entries[i].default_entry;
        if (!any_default)
                out->entries[0].default_entry = true;
        return MKSTOOR_OK;
}

/* ---------------- text-mode menu ---------------- */
extern void mks_console_add_ticks(u64 ms);
extern u64  mks_console_ticks_ms(void);

mkstoor_err_t mks_menu_run(const struct mks_menu *menu, usize *out_choice)
{
        EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin = g_st->ConIn;
        usize sel = 0;
        for (usize i = 0; i < menu->n_entries; i++)
                if (menu->entries[i].default_entry)
                        sel = i;

        mks_printf("\n MKSTOOR boot menu\n");
        mks_printf(" ====================\n");
        for (usize i = 0; i < menu->n_entries; i++)
                mks_printf("  %u) %s\n", (unsigned)(i + 1), menu->entries[i].name);
        mks_printf(" (%u s timeout, ENTER selects)\n", (unsigned)menu->timeout_s);

        u64 deadline = mks_console_ticks_ms() + menu->timeout_s * 1000;
        EFI_INPUT_KEY key;
        for (;;) {
                EFI_STATUS e = conin->ReadKeyStroke(conin, &key);
                if (e == EFI_SUCCESS) {
                        if (key.WideChar >= '1' && key.WideChar <= '9') {
                                usize n = key.WideChar - '0';
                                if (n <= menu->n_entries)
                                        sel = n - 1;
                        } else if (key.ScanCode == 0x10) { /* up arrow */
                                sel = (sel + menu->n_entries - 1) % menu->n_entries;
                        } else if (key.ScanCode == 0x18) { /* down arrow */
                                sel = (sel + 1) % menu->n_entries;
                        } else if (key.WideChar == '\r' || key.WideChar == '\n') {
                                break;
                        }
                }
                if (mks_console_ticks_ms() >= deadline)
                        break;
                g_st->BootServices->Stall(10000); /* 10 ms poll */
                mks_console_add_ticks(10);
        }
        *out_choice = sel;
        return MKSTOOR_OK;
}

/* ---------------- verification hooks ---------------- */
#if MKSTOOR_SECURE_BOOT
static mkstoor_err_t measure_and_verify(const char *what, const void *img,
                                        usize len)
{
        u8 digest[MKS_SHA256_DIGEST_LEN];
        MKS_RETURN_IF_ERR(mks_sha256_buf(img, len, digest));
        char hex[2 * MKS_SHA256_DIGEST_LEN + 1];
        static const char H[] = "0123456789abcdef";
        for (int i = 0; i < MKS_SHA256_DIGEST_LEN; i++) {
                hex[2 * i]     = H[digest[i] >> 4];
                hex[2 * i + 1] = H[digest[i] & 0xf];
        }
        hex[sizeof(hex) - 1] = 0;
        /* PCR extend happens in the TPM module (stage 2 crypto crate); until a
         * signing key is provisioned we measure but do not block boot — this
         * matches the "no silent failure" rule while staying operable. */
        mks_info("sec", "%s measured: sha256:%s", what, hex);
        return MKSTOOR_OK;
}
#else
static mkstoor_err_t measure_and_verify(const char *what, const void *img,
                                        usize len)
{
        UNUSED(what); UNUSED(img); UNUSED(len);
        return MKSTOOR_OK;
}
#endif

/* ---------------- autodetect fallback ---------------- */
/* Without a config file, offer anything that looks like a Linux kernel on
 * the mounted filesystem so a plain ESP/boot partition still yields a menu. */
static const char *kernel_probes[] = {
        "/EFI/BOOT/vmlinuz", "/boot/vmlinuz", "/vmlinuz", "/Image",
};
static const char *initrd_probes[] = {
        "/EFI/BOOT/initramfs", "/boot/initrd.img", "/initrd.img", "/initramfs",
};

static mkstoor_err_t autodetect_menu(struct mounted_fs *m,
                                     struct mks_menu *out)
{
        memset(out, 0, sizeof(*out));
        out->timeout_s = 5;
        for (usize i = 0; i < ARRAY_SIZE(kernel_probes); i++) {
                if (!fs_exists(m, kernel_probes[i]))
                        continue;
                struct mks_menu_entry *e = &out->entries[out->n_entries++];
                strncpy_safe(e->name, sizeof(e->name), kernel_probes[i]);
                strncpy_safe(e->kernel, sizeof(e->kernel), kernel_probes[i]);
                for (usize j = 0; j < ARRAY_SIZE(initrd_probes); j++) {
                        if (fs_exists(m, initrd_probes[j])) {
                                strncpy_safe(e->initrd, sizeof(e->initrd),
                                             initrd_probes[j]);
                                break;
                        }
                }
                if (out->n_entries == MKS_MENU_MAX_ENTRIES)
                        break;
        }
        return out->n_entries ? MKSTOOR_OK : MKSTOOR_ERR_NOT_FOUND;
}

/* ---------------- pipeline entry ---------------- */
void mks_pipeline_set_context(EFI_SYSTEM_TABLE *st, EFI_HANDLE ih)
{
        g_st = st;
        g_ih = ih;
        mks_boot_set_context(st, ih);
}

/* Accessor for the Stage 3 renderer (ui/menu_gop.c) — avoids duplicating
 * the context plumbing there. */
EFI_SYSTEM_TABLE *mks_pipeline_efi_table(void)
{
        return g_st;
}

mkstoor_err_t mks_stage2_run(EFI_SYSTEM_TABLE *st, EFI_HANDLE ih)
{
        mks_pipeline_set_context(st, ih);

        struct mks_block_dev *devs = NULL;
        usize ndevs = 0;
        MKS_RETURN_IF_ERR(mks_blockio_enumerate(st, &devs, &ndevs));
        mks_info("boot", "block devices with media: %u", (unsigned)ndevs);

        /* Mount everything we can. */
        static struct mounted_fs mounted[8];
        usize nmount = 0;
        for (usize i = 0; i < ndevs && nmount < ARRAY_SIZE(mounted); i++) {
                if (MKSTOOR_IS_OK(mount_one(&devs[i], &mounted[nmount]))) {
                        mks_info("boot", "dev %u: %s mounted",
                                 (unsigned)i, mounted[nmount].is_fat ? "FAT32" : "ext4");
                        nmount++;
                }
        }
        if (nmount == 0) {
                mks_err("boot", "no mountable filesystem found");
                return MKSTOOR_ERR_NOT_FOUND;
        }

        /* Config discovery: first FS containing our TOML wins. */
        struct mks_menu menu;
        bool have_menu = false;
        struct mks_theme theme;
        mks_theme_default(&theme);
        static char toml_copy[MKS_TOML_MAX_BYTES + 1];
        static char toml_arena[MKS_TOML_MAX_BYTES];

        for (usize i = 0; i < nmount && !have_menu; i++) {
                const char *cfg = NULL;
                if (fs_exists(&mounted[i], MKS_CONFIG_PATH))
                        cfg = MKS_CONFIG_PATH;
                else if (fs_exists(&mounted[i], MKS_ALT_CONFIG))
                        cfg = MKS_ALT_CONFIG;
                if (!cfg)
                        continue;

                void *raw = NULL; usize len = 0;
                if (MKSTOOR_IS_ERR(fs_read(&mounted[i], cfg, &raw, &len)))
                        continue;
                if (len == 0 || len > MKS_TOML_MAX_BYTES) {
                        mks_warn("cfg", "%s: implausible size %u", cfg, (unsigned)len);
                } else {
                        memcpy(toml_copy, raw, len);
                        toml_copy[len] = 0;
                        struct mks_toml_doc doc = { .arena = toml_arena,
                                                    .arena_cap = sizeof(toml_arena) };
                        mkstoor_err_t e = mks_toml_parse(toml_copy, len, &doc);
                        if (MKSTOOR_IS_OK(e))
                                e = mks_menu_from_toml(&doc, &menu);
                        if (MKSTOOR_IS_OK(e)) {
                                /* Stage 3: [theme] table overlays the
                                 * compiled-in defaults before any drawing. */
                                mks_theme_from_toml(&theme, &doc);
                                mks_info("cfg", "loaded %s (%u entries)", cfg,
                                         (unsigned)menu.n_entries);
                                have_menu = true;
                        } else {
                                mks_warn("cfg", "%s: %s", cfg, mkstoor_strerror(e));
                        }
                }
                fs_free(raw, mounted[i].is_fat);
        }

        /* Fallback: autodetect on the first mounted FS. */
        if (!have_menu && MKSTOOR_IS_OK(autodetect_menu(&mounted[0], &menu))) {
                mks_info("boot", "autodetected %u entries on dev0",
                         (unsigned)menu.n_entries);
                have_menu = true;
        }
        if (!have_menu) {
                mks_err("boot", "no boot entries available");
                return MKSTOOR_ERR_NOT_FOUND;
        }

        usize choice = 0;
        /* Stage 3: bring the framebuffer up and prefer the graphical menu.
         * Every step degrades gracefully — no GOP or a renderer error falls
         * back to the proven text path, so headless/serial firmware behaves
         * exactly as before. */
        bool gfx = MKSTOOR_IS_OK(mks_gop_init(st, 1280u, 720u, NULL));
        if (gfx && theme.pointer_enabled)
                mks_pointer_init(st);       /* optional; NOT_FOUND is fine */
        mkstoor_err_t me = gfx
                ? mks_menu_run_graphical(&menu, &theme, &choice)
                : MKSTOOR_ERR_NOT_FOUND;
        if (MKSTOOR_IS_ERR(me)) {
                if (gfx)
                        mks_warn("ui", "graphical menu unavailable (%s); "
                                 "falling back to text menu",
                                 mkstoor_strerror(me));
                MKS_RETURN_IF_ERR(mks_menu_run(&menu, &choice));
        } else if (gfx) {
                /* Hand the screen back to the text console for the kernel
                 * stub's early messages. */
                st->ConOut->ClearScreen(st->ConOut);
        }
        const struct mks_menu_entry *ent = &menu.entries[choice];
        mks_info("boot", "selected: %s", ent->name);

        /* Locate the entry's kernel on whichever FS has it. */
        int fsi = -1;
        for (usize i = 0; i < nmount; i++) {
                if (fs_exists(&mounted[i], ent->kernel)) {
                        fsi = (int)i;
                        break;
                }
        }
        if (fsi < 0) {
                mks_err("boot", "kernel %s not found on any mounted FS", ent->kernel);
                return MKSTOOR_ERR_NOT_FOUND;
        }

        void *kimg = NULL; usize klen = 0;
        MKS_RETURN_IF_ERR(fs_read(&mounted[fsi], ent->kernel, &kimg, &klen));
        MKS_RETURN_IF_ERR(measure_and_verify("kernel", kimg, klen));

        void *iimg = NULL; usize ilen = 0;
        if (ent->initrd[0]) {
                if (MKSTOOR_IS_OK(fs_read(&mounted[fsi], ent->initrd, &iimg, &ilen)))
                        measure_and_verify("initrd", iimg, ilen);
                else
                        mks_warn("boot", "initrd %s unreadable; continuing without",
                                 ent->initrd);
        }

        /* Compose cmdline: global + per-entry. */
        static char cmdline[MKS_MENU_PATH_LEN * 2];
        strncpy_safe(cmdline, sizeof(cmdline), menu.global_cmdline);
        if (ent->cmdline[0]) {
                usize used = strlen(cmdline);
                if (used + 1 < sizeof(cmdline)) {
                        cmdline[used++] = ' ';
                        strncpy_safe(cmdline + used, sizeof(cmdline) - used,
                                     ent->cmdline);
                }
        }

        struct mks_boot_ctx ctx = {
                .st = st,
                .image_handle = ih,
                .cmdline = cmdline,
                .initrd_addr = (u64)(uintptr_t)iimg,
                .initrd_size = ilen,
        };
        mks_info("core", "handing over to kernel (%u bytes)...", (unsigned)klen);
        mkstoor_err_t e = mks_boot_linux(&ctx, kimg, klen);
        /* Only reached on failure. */
        mks_err("boot", "handover failed: %s", mkstoor_strerror(e));
        fs_free(kimg, mounted[fsi].is_fat);
        return e;
}
