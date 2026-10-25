/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: FAT32 reader (see core/include/mkstoor/fat.h).
 *
 * Every on-disk number is validated against the volume geometry before it
 * is used; any inconsistency aborts with MKSTOOR_ERR_CORRUPT so a crafted
 * ESP can never make us read out of bounds or loop forever (the chain-loop
 * guard below caps cluster traversal at the data-area size).
 */
#include <mkstoor/fat.h>
#include <mkstoor/blockio.h>
#include <mkstoor/memory.h>
#include <mkstoor/console.h>

#define MKS_FAT_MAX_DEPTH   8
#define MKS_FAT_MAX_FILE    (512u * 1024u * 1024u)
#define MKS_FAT_ATTR_LFN    0x0Fu /* LFN entry attribute mask            */

struct mks_fat_fs {
        struct mks_block_dev *dev;
        u32 bytes_per_sector;
        u32 sectors_per_cluster;
        u32 reserved_sectors;
        u32 fat_count;
        u32 root_entries;         /* 0 on FAT32 */
        u32 first_data_sector;
        u32 fat_entries;          /* clusters + 2 reserved */
        u32 cluster_size;
        u32 root_cluster;         /* FAT32 only */
        u32 *fat;                 /* in-memory copy of the FAT table */
};

static mkstoor_err_t dev_read(mks_fat_fs_t *fs, u64 sector, void *buf,
                              u32 nsectors)
{
        return mks_bio_read_blocks(fs->dev, sector, buf, nsectors);
}

/* ------------------------------------------------------------------ mount */
mkstoor_err_t mks_fat_mount(struct mks_block_dev *dev, mks_fat_fs_t **out_fs)
{
        if (!dev || !out_fs)
                return MKSTOOR_ERR_INVALID_ARG;

        u32 bs = mks_block_size(dev);
        if (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096)
                return MKSTOOR_ERR_UNSUPPORTED;

        void *sec0 = mks_early_alloc(bs);
        if (!sec0)
                return MKSTOOR_ERR_NO_MEMORY;
        MKS_RETURN_IF_ERR(mks_bio_read_blocks(dev, 0, sec0, 1));

        const u8 *b = sec0;
        /* BPB offsets (FAT spec 1.0 rev A, MSBOOT.doc) */
        if (b[510] != 0x55 || b[511] != 0xAA)
                return MKSTOOR_ERR_CORRUPT;
        u16 bytes_per_sec = *(const u16 *)(b + 11);
        u8  sect_per_clu  = b[13];
        u16 reserved      = *(const u16 *)(b + 14);
        u8  nfats         = b[16];
        u16 root_ent      = *(const u16 *)(b + 17);
        u16 sect16        = *(const u16 *)(b + 19);
        u32 sect32        = *(const u32 *)(b + 32);
        u32 fat32_size    = *(const u32 *)(b + 36);
        u32 root_clu      = *(const u32 *)(b + 44);
        u8  drive_boot_sig = b[66];
        (void)drive_boot_sig; /* informational only: we never require it */

        if (bytes_per_sec == 0 || sect_per_clu == 0 ||
            (sect_per_clu & (sect_per_clu - 1)))
                return MKSTOOR_ERR_CORRUPT;
        /* bytes_per_sec must be a power of two in [512,4096] and match the
         * logical block size, otherwise cluster math below is meaningless. */
        if (bytes_per_sec != bs)
                return MKSTOOR_ERR_UNSUPPORTED;
        if (nfats == 0 || nfats > 8 || reserved == 0)
                return MKSTOOR_ERR_CORRUPT;

        u64 total_sect = sect16 ? sect16 : sect32;
        if (total_sect == 0 || total_sect > mks_block_count(dev))
                return MKSTOOR_ERR_CORRUPT;

        u64 fat_size = fat32_size ? fat32_size
                                  : *(const u16 *)(b + 22);
        if (fat_size == 0 || fat_size > total_sect)
                return MKSTOOR_ERR_CORRUPT;

        /* FAT32 iff root_ent == 0 and fat32_size != 0 (or >= 4085 clusters). */
        if (root_ent != 0 || fat32_size == 0)
                return MKSTOOR_ERR_UNSUPPORTED; /* FAT12/16: not in scope for v0.2 */

        u64 root_dir_sects = 0;
        u64 data_sects = total_sect - reserved - (u64)nfats * fat_size -
                         root_dir_sects;
        if (data_sects < (u64)sect_per_clu * 2)
                return MKSTOOR_ERR_CORRUPT;
        u64 count_clusters = data_sects / sect_per_clu;
        if (count_clusters < 65525)             /* would be FAT12/16 geometry */
                return MKSTOOR_ERR_UNSUPPORTED;

        mks_fat_fs_t *fs = mks_early_alloc(sizeof(*fs));
        if (!fs)
                return MKSTOOR_ERR_NO_MEMORY;
        memset(fs, 0, sizeof(*fs));

        fs->dev = dev;
        fs->bytes_per_sector = bs;
        fs->sectors_per_cluster = sect_per_clu;
        fs->reserved_sectors = reserved;
        fs->fat_count = nfats;
        fs->first_data_sector =
                (u32)(reserved + (u64)nfats * fat_size + root_dir_sects);
        fs->fat_entries = (u32)(count_clusters + 2);
        fs->cluster_size = bs * sect_per_clu;
        fs->root_cluster = root_clu;

        fs->fat = mks_early_alloc((usize)fs->fat_entries * sizeof(u32));
        if (!fs->fat)
                return MKSTOOR_ERR_NO_MEMORY;

        /* Load the first FAT into memory (one sector buffer reused). */
        void *rbuf = mks_early_alloc(bs);
        if (!rbuf)
                return MKSTOOR_ERR_NO_MEMORY;
        u32 per_fat_sect = (u32)fat_size;
        u32 loaded = 0;
        for (u32 s = 0; s < per_fat_sect && loaded < fs->fat_entries; s++) {
                MKS_RETURN_IF_ERR(mks_bio_read_blocks(dev, reserved + s, rbuf, 1));
                const u32 *words = rbuf;
                u32 n = bs / 4;
                for (u32 i = 0; i < n && loaded < fs->fat_entries; i++, loaded++)
                        fs->fat[loaded] = words[i] & 0x0FFFFFFFu;
        }
        if (loaded < fs->fat_entries)
                return MKSTOOR_ERR_CORRUPT;

        *out_fs = fs;
        mks_dbg("fat", "mounted: %u B/sec, %u sec/clu, %u clusters",
                (unsigned)bs, (unsigned)sect_per_clu,
                (unsigned)count_clusters);
        return MKSTOOR_OK;
}

void mks_fat_unmount(mks_fat_fs_t *fs)
{
        UNUSED(fs); /* everything lives in the early arena; freed wholesale */
}

/* ------------------------------------------------------------- cluster IO */
static u32 fat_next(mks_fat_fs_t *fs, u32 clus)
{
        if (clus < 2 || clus >= fs->fat_entries)
                return 0x0FFFFFFFu; /* sentinel-ish invalid */
        return fs->fat[clus];
}

static u64 clus_to_sector(mks_fat_fs_t *fs, u32 clus)
{
        return fs->first_data_sector +
               (u64)(clus - 2) * fs->sectors_per_cluster;
}

/* Read `len` bytes of a cluster chain starting at byte offset `off`. */
static mkstoor_err_t chain_read(mks_fat_fs_t *fs, u32 start, usize off,
                                void *dst, usize len)
{
        u32 clus = start;
        usize csize = fs->cluster_size;
        usize guard = 0;
        while (len) {
                if (clus >= 0x0FFFFFF8u || clus >= fs->fat_entries)
                        return MKSTOOR_ERR_CORRUPT;
                if (++guard > fs->fat_entries)
                        return MKSTOOR_ERR_CORRUPT; /* chain loop */
                if (off >= csize) {
                        off -= csize;
                        clus = fat_next(fs, clus);
                        continue;
                }
                usize chunk = MIN(len, csize - off);
                /* sector-aligned copy through a scratch buffer */
                u8 *scratch = mks_early_alloc(csize);
                if (!scratch)
                        return MKSTOOR_ERR_NO_MEMORY;
                MKS_RETURN_IF_ERR(dev_read(fs, clus_to_sector(fs, clus), scratch,
                                          fs->sectors_per_cluster));
                memcpy(dst, scratch + off, chunk);
                dst = (u8 *)dst + chunk;
                len -= chunk;
                off += chunk;
                if (off >= csize) {
                        off = 0;
                        clus = fat_next(fs, clus);
                }
        }
        return MKSTOOR_OK;
}

/* --------------------------------------------------------------- names */
/* FAT dir-entry name field offsets: 0..7 base, 8..10 ext (space padded). */
static void shortname_render(const u8 *de, char *out, usize cap)
{
        usize j = 0;
        for (usize i = 0; i < 8 && de[i] != ' ' && de[i] != 0; i++) {
                if (j + 1 < cap)
                        out[j++] = (char)de[i];
        }
        if (de[8] != ' ' && de[8] != 0 && j + 1 < cap) {
                out[j++] = '.';
                for (usize i = 0; i < 3 && de[8 + i] != ' ' && de[8 + i] != 0; i++) {
                        if (j + 1 < cap)
                                out[j++] = (char)de[8 + i];
                }
        }
        out[j] = 0;
}

/* Reconstruct LFN chars from one 32-byte sequence entry (UTF-16 -> ASCII).
 * Layout: bytes 1..10 = chars 0..4, 14..24 = chars 5..9, 28..31 = chars 10..12. */
static usize lfn_from_entry(const u8 *e, char *out, usize cap, usize pos)
{
        static const usize boff[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (usize i = 0; i < 13; i++) {
                u16 ch = (u16)e[boff[i]] | ((u16)e[boff[i] + 1] << 8);
                if (ch == 0 || ch == 0xFFFF)
                        return pos; /* end of this fragment */
                if (pos >= cap - 1)
                        return pos;
                out[pos++] = (ch < 128) ? (char)ch : '?';
        }
        return pos;
}

/* -------------------------------------------------------------- lookup */
struct dir_walk {
        mks_fat_fs_t *fs;
        u32 dir_cluster;
        usize dir_len;     /* logical size of the directory stream */
};

/* Iterate all short-name entries of a directory invoking cb. */
typedef mkstoor_err_t (*dir_cb_t)(void *arg, const u8 *de, const char *lfn);

static mkstoor_err_t walk_dir(struct dir_walk *dw, dir_cb_t cb, void *arg)
{
        usize ents = dw->dir_len / 32;
        if (ents == 0)
                return MKSTOOR_OK;
        /* Directory streams are small enough to slurp whole (cap 2 MiB). */
        usize span = dw->dir_len;
        if (span > 2u * 1024u * 1024u)
                return MKSTOOR_ERR_UNSUPPORTED;
        u8 *buf = mks_early_alloc(span);
        if (!buf)
                return MKSTOOR_ERR_NO_MEMORY;
        MKS_RETURN_IF_ERR(chain_read(dw->fs, dw->dir_cluster, 0, buf, span));

        char lfn[MKS_TOML_MAX_PATH]; /* reuse generous path cap */
        usize lfn_pos = 0;
        for (usize i = 0; i < ents; i++) {
                const u8 *de = buf + i * 32;
                if (de[0] == 0x00)
                        break;                  /* end of directory */
                if (de[0] == 0xE5)
                        continue;               /* deleted */
                if (de[11] == MKS_FAT_ATTR_LFN) {
                        u8 seq = de[0] & 0x3F;
                        lfn_pos = (seq == 1) ? lfn_from_entry(de, lfn, sizeof(lfn), 0)
                                             : lfn_from_entry(de, lfn, sizeof(lfn), lfn_pos);
                        continue;
                }
                /* short entry: use LFN if present else 8.3 rendering */
                char name[16];
                const char *use = lfn;
                if (lfn_pos == 0) {
                        usize j = 0;
                        for (j = 0; j < 8 && de[j] != ' '; j++)
                                name[j] = (char)de[j];
                        if (de[8] != ' ') {
                                name[j++] = '.';
                                for (usize k = 0; k < 3 && de[8 + k] != ' '; k++)
                                        name[j++] = (char)de[8 + k];
                        }
                        name[j] = 0;
                        use = name;
                }
                MKS_RETURN_IF_ERR(cb(arg, de, use));
                lfn_pos = 0;
        }
        return MKSTOOR_OK;
}

struct find_ctx {
        const char *want;      /* single component, lowercase-compared */
        bool       is_dir;
        bool       found;
        u32        cluster;
        usize      size;
};

static mkstoor_err_t find_cb(void *arg, const u8 *de, const char *name)
{
        struct find_ctx *f = arg;
        if (f->found)
                return MKSTOOR_OK;
        bool match = false;
        if (strlen(name) == strlen(f->want)) {
                match = true;
                for (usize i = 0; name[i]; i++) {
                        char a = name[i], b = f->want[i];
                        if (a >= 'A' && a <= 'Z') a += 32;
                        if (b >= 'A' && b <= 'Z') b += 32;
                        if (a != b) { match = false; break; }
                }
        }
        if (!match)
                return MKSTOOR_OK;
        if (!(de[11] & EFI_FILE_DIRECTORY) != !(f->is_dir)) {
                /* type mismatch: dir vs file */
                return MKSTOOR_OK;
        }
        f->found = true;
        f->cluster = (u32)de[26] | ((u32)de[27] << 8) |
                     ((u32)de[20] << 16) | ((u32)de[21] << 24);
        f->size = (usize)de[28] | ((usize)de[29] << 8) |
                  ((usize)de[30] << 16) | ((usize)de[31] << 24);
        return MKSTOOR_OK;
}

/* Resolve one path component inside a directory cluster. */
static mkstoor_err_t resolve_child(mks_fat_fs_t *fs, u32 dir_cluster,
                                   usize dir_len, const char *comp,
                                   bool want_dir, struct find_ctx *f)
{
        struct dir_walk dw = { .fs = fs, .dir_cluster = dir_cluster,
                               .dir_len = dir_len };
        f->want = comp;
        f->is_dir = want_dir;
        f->found = false;
        MKS_RETURN_IF_ERR(walk_dir(&dw, find_cb, f));
        return f->found ? MKSTOOR_OK : MKSTOOR_ERR_NOT_FOUND;
}

/* Root directory size: on FAT32 it's a cluster chain, length unknown until
 * we scan; we cap the scan at 1 MiB worth of clusters. */
static usize root_stream_len(mks_fat_fs_t *fs, u32 cluster)
{
        usize len = 0;
        u32 clus = cluster;
        for (usize guard = 0; guard < 256; guard++) {
                if (clus >= 0x0FFFFFF8u)
                        break;
                len += fs->cluster_size;
                clus = fat_next(fs, clus);
        }
        return MIN(len, 1u * 1024u * 1024u);
}

static mkstoor_err_t open_path(mks_fat_fs_t *fs, const char *path,
                               bool want_dir, struct find_ctx *out)
{
        if (path[0] != '/')
                return MKSTOOR_ERR_INVALID_ARG;

        u32 cur = fs->root_cluster;
        usize cur_len = root_stream_len(fs, cur);
        bool have = false;

        const char *p = path + 1;
        while (*p) {
                char comp[MKS_TOML_MAX_PATH];
                usize i = 0;
                while (p[i] && p[i] != '/' && i + 1 < sizeof(comp)) {
                        comp[i] = p[i];
                        i++;
                }
                comp[i] = 0;
                p += i;
                while (*p == '/')
                        p++;

                struct find_ctx f;
                MKS_RETURN_IF_ERR(resolve_child(fs, cur, cur_len, comp,
                                                (*p == 0) ? want_dir : true, &f));
                cur = f.cluster;
                cur_len = f.size ? f.size : fs->cluster_size;
                have = true;
                if (f.size > MKS_FAT_MAX_FILE)
                        return MKSTOOR_ERR_UNSUPPORTED;
                *out = f;
        }
        if (!have) { /* "/" itself */
                out->found = true;
                out->cluster = fs->root_cluster;
                out->size = cur_len;
                out->is_dir = true;
        }
        return MKSTOOR_OK;
}

bool mks_fat_exists(mks_fat_fs_t *fs, const char *path)
{
        struct find_ctx f = { 0 };
        return MKSTOOR_IS_OK(open_path(fs, path, false, &f)) && f.found;
}

mkstoor_err_t mks_fat_read_file(mks_fat_fs_t *fs, const char *path,
                                void **out_buf, usize *out_len)
{
        struct find_ctx f = { 0 };
        MKS_RETURN_IF_ERR(open_path(fs, path, false, &f));
        if (!f.found)
                return MKSTOOR_ERR_NOT_FOUND;
        if (f.size == 0 || f.size > MKS_FAT_MAX_FILE)
                return MKSTOOR_ERR_UNSUPPORTED;

        void *buf = mks_tracked_alloc(f.size, "fs.fat");
        if (!buf)
                return MKSTOOR_ERR_NO_MEMORY;
        MKS_RETURN_IF_ERR(chain_read(fs, f.cluster, 0, buf, f.size));
        *out_buf = buf;
        *out_len = f.size;
        return MKSTOOR_OK;
}

extern void *mks_tracked_alloc(usize size, const char *owner);
extern mkstoor_err_t mks_tracked_free(void *p);

void mks_fat_free_file(void *buf)
{
        mks_tracked_free(buf);
}
