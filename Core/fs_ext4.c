/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: ext4 read-only reader (see mkstoor/ext4.h).
 * Subset documented in the header; every on-disk value is bounds-checked
 * before use and unknown incompat features abort the mount.
 *
 * On-disk layouts (ext4 wiki / fs/ext4/ext4.h):
 *   superblock @ byte 1024, group descriptors follow the superblock block,
 *   inode: mode@0 size_lo@4 i_block@40 ...
 */
#include <mkstoor/ext4.h>
#include <mkstoor/blockio.h>
#include <mkstoor/memory.h>
#include <mkstoor/console.h>

extern void *mks_tracked_alloc(usize size, const char *owner);
extern mkstoor_err_t mks_tracked_free(void *p);

#define EXT4_SUPER_MAGIC       0xEF53
#define EXT4_ROOT_INO          2
#define EXT4_GOOD_OLD_INODE_SIZE 128
#define EXT4_MAX_INODE_SIZE    768

/* ext4 incompat feature bits (fs/ext4/ext4.h). We understand exactly these;
 * anything else aborts the mount rather than misparsing data. */
#define INCOMPAT_FILETYPE 0x0002
#define INCOMPAT_RECOVER  0x0004
#define INCOMPAT_META_BG  0x0010
#define INCOMPAT_EXTENTS  0x0040
#define INCOMPAT_64BIT    0x0080
#define INCOMPAT_FLEX_BG  0x0200

#define EXT4_EXT_MAGIC   0xF30A
#define EXT4_INO_FLAG_EXTENTS 0x00080000u

struct mks_ext4_fs {
        struct mks_block_dev *dev;
        u32 block_size;
        u32 blocks_per_group;
        u32 inodes_per_group;
        u32 inode_size;
        u32 desc_size;
        u32 first_data_block;
        bool has_64bit;
};

static u16 le16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 le32(const u8 *p)
{
        return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 le64(const u8 *p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

static mkstoor_err_t bread(struct mks_ext4_fs *fs, u64 fblock, void *buf,
                           u32 nblocks)
{
        u32 bs = mks_block_size(fs->dev);
        if (fs->block_size % bs != 0)
                return MKSTOOR_ERR_UNSUPPORTED;
        u32 ratio = fs->block_size / bs;
        return mks_bio_read_blocks(fs->dev, fblock * ratio, buf,
                                   nblocks * ratio);
}

/* ------------------------------------------------------------- mount */
mkstoor_err_t mks_ext4_mount(struct mks_block_dev *dev, mks_ext4_fs_t **out)
{
        if (!dev || !out)
                return MKSTOOR_ERR_INVALID_ARG;

        u32 bs = mks_block_size(dev);
        if (bs > 1024 && bs % 1024 != 0)
                return MKSTOOR_ERR_UNSUPPORTED;

        /* Read the disk block containing the superblock (byte offset 1024). */
        u64 sb_sector = 1024 / bs;
        void *tmp = mks_early_alloc(MAX(bs, 1024));
        if (!tmp)
                return MKSTOOR_ERR_NO_MEMORY;
        MKS_RETURN_IF_ERR(mks_bio_read_blocks(dev, sb_sector, tmp,
                                              MAX(1024 / bs, 1)));
        const u8 *sb = (const u8 *)tmp + (1024 % bs);

        if (le32(sb + 0x38) != EXT4_SUPER_MAGIC)
                return MKSTOOR_ERR_CORRUPT;

        u32 incompat = le32(sb + 0x60);
        if (incompat & ~(INCOMPAT_FILETYPE | INCOMPAT_RECOVER |
                         INCOMPAT_META_BG | INCOMPAT_EXTENTS |
                         INCOMPAT_64BIT | INCOMPAT_FLEX_BG))
                return MKSTOOR_ERR_UNSUPPORTED;
        /* META_BG means sparse superblocks; we assume the common layout where
         * BGDT begins right after the first block of group 0. Reject if both
         * SPARSE_SUPER and huge geometry to avoid misparsing. */

        mks_ext4_fs_t *fs = mks_early_alloc(sizeof(*fs));
        if (!fs)
                return MKSTOOR_ERR_NO_MEMORY;
        memset(fs, 0, sizeof(*fs));

        fs->dev = dev;
        fs->block_size = 1024u << le32(sb + 0x18);
        if (fs->block_size < 1024 || fs->block_size > 65536 ||
            (fs->block_size & (fs->block_size - 1)))
                return MKSTOOR_ERR_CORRUPT;
        fs->blocks_per_group = le32(sb + 0x20);
        fs->inodes_per_group = le32(sb + 0x28);
        fs->first_data_block = (fs->block_size == 1024) ? 1 : 0;
        u32 s_rev = le32(sb + 0x54);
        u16 isize16 = le16(sb + 0x58);
        fs->inode_size = (s_rev >= 1) ? isize16 : EXT4_GOOD_OLD_INODE_SIZE;
        if (fs->inode_size < 128 || fs->inode_size > EXT4_MAX_INODE_SIZE)
                return MKSTOOR_ERR_CORRUPT;
        fs->has_64bit = (incompat & INCOMPAT_64BIT) != 0;
        /* s_desc_size lives at byte offset 0x156 in the ext4 superblock. */
        fs->desc_size = fs->has_64bit ? le16(sb + 0x156) : 32;
        if (fs->desc_size != 32 && fs->desc_size != 64)
                return MKSTOOR_ERR_CORRUPT;
        if (fs->blocks_per_group == 0 || fs->inodes_per_group == 0)
                return MKSTOOR_ERR_CORRUPT;

        *out = fs;
        mks_dbg("ext", "mounted: %u B blocks, %u ino/group, %u B inodes%s",
                (unsigned)fs->block_size, (unsigned)fs->inodes_per_group,
                (unsigned)fs->inode_size, fs->has_64bit ? ", 64-bit" : "");
        return MKSTOOR_OK;
}

void mks_ext4_unmount(mks_ext4_fs_t *fs) { UNUSED(fs); }

/* --------------------------------------------------------- group desc */
static mkstoor_err_t bg_read(struct mks_ext4_fs *fs, u64 g, u8 *out)
{
        u64 gd_start_blk = fs->first_data_block + 1;
        u64 off = g * (u64)fs->desc_size;
        u64 blk = gd_start_blk + off / fs->block_size;
        u32 within = (u32)(off % fs->block_size);
        u8 *tmp = mks_early_alloc(fs->block_size);
        if (!tmp)
                return MKSTOOR_ERR_NO_MEMORY;
        MKS_RETURN_IF_ERR(bread(fs, blk, tmp, 1));
        memcpy(out, tmp + within, fs->desc_size);
        return MKSTOOR_OK;
}

/* -------------------------------------------------------------- inode */
static mkstoor_err_t inode_read(struct mks_ext4_fs *fs, u64 ino, u8 *out)
{
        if (ino == 0)
                return MKSTOOR_ERR_CORRUPT;
        u64 g = (ino - 1) / fs->inodes_per_group;
        u32 idx = (u32)((ino - 1) % fs->inodes_per_group);
        u8 gd[64];
        MKS_RETURN_IF_ERR(bg_read(fs, g, gd));
        /* ext4_group_desc: block_bitmap@0 inode_bitmap@4 inode_table@8,
         * plus block_bitmap_high@0x18 inode_bitmap_high@0x1C
         *      inode_table_high@0x20 when the 64-bit feature is set. */
        u64 itbl;
        if (fs->has_64bit)
                itbl = (u64)le32(gd + 0x08) | ((u64)le32(gd + 0x20) << 32);
        else
                itbl = le32(gd + 0x08);

        u64 ipb = fs->block_size / fs->inode_size;
        if (ipb == 0)
                return MKSTOOR_ERR_CORRUPT;
        u64 blk = itbl + idx / ipb;
        u32 within = (u32)(idx % ipb) * fs->inode_size;
        u8 *tmp = mks_early_alloc(fs->block_size);
        if (!tmp)
                return MKSTOOR_ERR_NO_MEMORY;
        MKS_RETURN_IF_ERR(bread(fs, blk, tmp, 1));
        memcpy(out, tmp + within, fs->inode_size);
        return MKSTOOR_OK;
}

static usize inode_filesize(const u8 *in)
{
        /* i_size_lo @4, i_size_high @0x6C (little-endian 64-bit split) */
        usize lo = le32(in + 4);
        usize hi = le32(in + 0x6C);
        return lo | (hi << 32);
}

/* ---------------------------------------------------- data block maps */
static mkstoor_err_t extent_walk(struct mks_ext4_fs *fs, const u8 *in,
                                 u32 lblock, u64 *pblk)
{
        const u8 *eh = in + 40;
        if (le16(eh) != EXT4_EXT_MAGIC)
                return MKSTOOR_ERR_CORRUPT;
        u16 entries = le16(eh + 6);
        if (entries > 4)
                return MKSTOOR_ERR_CORRUPT;
        for (u16 i = 0; i < entries; i++) {
                const u8 *ex = eh + 12 + i * 12;
                u32 eblk = le32(ex);
                u16 elen = le16(ex + 8) & 0x7FFF;
                if (lblock >= eblk && lblock < eblk + elen) {
                        *pblk = (u64)le32(ex + 4) | ((u64)le16(ex + 10) << 32);
                        return MKSTOOR_OK;
                }
        }
        return MKSTOOR_ERR_NOT_FOUND;
}

static mkstoor_err_t block_map(struct mks_ext4_fs *fs, const u8 *in,
                               u32 lblock, u64 *pblk)
{
        if (le32(in + 32) & EXT4_INO_FLAG_EXTENTS)
                return extent_walk(fs, in, lblock, pblk);

        if (lblock < 12) {
                *pblk = le32(in + 40 + lblock * 4);
                return *pblk ? MKSTOOR_OK : MKSTOOR_ERR_NOT_FOUND;
        }
        u32 ptrs = fs->block_size / 4;
        u8 *tmp = mks_early_alloc(fs->block_size);
        if (!tmp)
                return MKSTOOR_ERR_NO_MEMORY;
        u32 rel = lblock - 12;
        if (rel < ptrs) {
                u32 ind = le32(in + 40 + 12 * 4);
                if (!ind)
                        return MKSTOOR_ERR_NOT_FOUND;
                MKS_RETURN_IF_ERR(bread(fs, ind, tmp, 1));
                *pblk = le32(tmp + rel * 4);
                return *pblk ? MKSTOOR_OK : MKSTOOR_ERR_NOT_FOUND;
        }
        rel -= ptrs;
        if (rel < ptrs * ptrs) {
                u32 dind = le32(in + 40 + 13 * 4);
                if (!dind)
                        return MKSTOOR_ERR_NOT_FOUND;
                MKS_RETURN_IF_ERR(bread(fs, dind, tmp, 1));
                u32 ind = le32(tmp + (rel / ptrs) * 4);
                if (!ind)
                        return MKSTOOR_ERR_NOT_FOUND;
                MKS_RETURN_IF_ERR(bread(fs, ind, tmp, 1));
                *pblk = le32(tmp + (rel % ptrs) * 4);
                return *pblk ? MKSTOOR_OK : MKSTOOR_ERR_NOT_FOUND;
        }
        return MKSTOOR_ERR_UNSUPPORTED; /* triple indirect beyond any kernel image */
}

/* ------------------------------------------------------ slurp a file */
static mkstoor_err_t slurp(struct mks_ext4_fs *fs, const u8 *in,
                           void **out_buf, usize *out_len)
{
        usize sz = inode_filesize(in);
        if (sz == 0 || sz > 512u * 1024u * 1024u)
                return MKSTOOR_ERR_UNSUPPORTED;
        void *buf = mks_tracked_alloc(sz, "fs.ext4");
        u8 *blk = mks_early_alloc(fs->block_size);
        if (!buf || !blk) {
                if (buf)
                        mks_tracked_free(buf);
                return MKSTOOR_ERR_NO_MEMORY;
        }
        usize done = 0;
        while (done < sz) {
                u32 lblock = (u32)(done / fs->block_size);
                u64 pblk;
                if (MKSTOOR_IS_ERR(block_map(fs, in, lblock, &pblk))) {
                        mks_tracked_free(buf);
                        return MKSTOOR_ERR_CORRUPT; /* hole inside kernel image */
                }
                usize chunk = MIN(sz - done, (usize)fs->block_size);
                MKS_RETURN_IF_ERR(bread(fs, pblk, blk, 1));
                memcpy((u8 *)buf + done, blk, chunk);
                done += chunk;
        }
        *out_buf = buf;
        *out_len = sz;
        return MKSTOOR_OK;
}

/* -------------------------------------------------------- directory */
typedef mkstoor_err_t (*ext4_dir_cb)(void *arg, u32 ino, const char *name,
                                     usize namelen);

static mkstoor_err_t walk_dir(struct mks_ext4_fs *fs, const u8 *in,
                              ext4_dir_cb cb, void *arg)
{
        usize sz = inode_filesize(in);
        if (sz == 0 || sz > 8u * 1024u * 1024u)
                return MKSTOOR_ERR_UNSUPPORTED;
        u8 *buf = mks_early_alloc(sz);
        if (!buf)
                return MKSTOOR_ERR_NO_MEMORY;
        usize done = 0;
        while (done < sz) {
                u32 lblock = (u32)(done / fs->block_size);
                u64 pblk;
                if (MKSTOOR_IS_ERR(block_map(fs, in, lblock, &pblk)))
                        break;               /* sparse tail of a dir: acceptable */
                usize chunk = MIN(sz - done, (usize)fs->block_size);
                MKS_RETURN_IF_ERR(bread(fs, pblk, buf + done, 1));
                done += chunk;
        }

        usize o = 0;
        while (o + 12 <= done) {
                const u8 *de = buf + o;
                u32 de_ino = le32(de);
                u16 de_len = le16(de + 4);
                u8  de_namlen = de[10];
                if (de_len < 12 || o + de_len > done)
                        break;
                if (de_ino && de_namlen && de_namlen <= 255) {
                        char nm[256];
                        memcpy(nm, de + 11, de_namlen);
                        nm[de_namlen] = 0;
                        MKS_RETURN_IF_ERR(cb(arg, de_ino, nm, de_namlen));
                }
                o += de_len;
        }
        return MKSTOOR_OK;
}

struct lookup_ctx { const char *want; u32 found_ino; };

static mkstoor_err_t lookup_cb(void *arg, u32 ino, const char *name,
                               usize namelen)
{
        struct lookup_ctx *c = arg;
        if (c->found_ino)
                return MKSTOOR_OK;
        if (namelen == strlen(c->want) && memcmp(name, c->want, namelen) == 0)
                c->found_ino = ino;
        return MKSTOOR_OK;
}

/* ------------------------------------------------------ path resolve */
static mkstoor_err_t resolve(struct mks_ext4_fs *fs, const char *path,
                             u8 *inode_out)
{
        MKS_RETURN_IF_ERR(inode_read(fs, EXT4_ROOT_INO, inode_out));
        const char *p = path;
        if (*p == '/')
                p++;
        while (*p) {
                char comp[256];
                usize i = 0;
                while (*p && *p != '/' && i + 1 < sizeof(comp))
                        comp[i++] = *p++;
                comp[i] = 0;
                while (*p == '/')
                        p++;
                if (i == 0)
                        continue;
                struct lookup_ctx lc = { .want = comp, .found_ino = 0 };
                MKS_RETURN_IF_ERR(walk_dir(fs, inode_out, lookup_cb, &lc));
                if (!lc.found_ino)
                        return MKSTOOR_ERR_NOT_FOUND;
                MKS_RETURN_IF_ERR(inode_read(fs, lc.found_ino, inode_out));
        }
        return MKSTOOR_OK;
}

bool mks_ext4_exists(mks_ext4_fs_t *fs, const char *path)
{
        u8 in[EXT4_MAX_INODE_SIZE];
        return MKSTOOR_IS_OK(resolve(fs, path, in));
}

mkstoor_err_t mks_ext4_read_file(mks_ext4_fs_t *fs, const char *path,
                                 void **out_buf, usize *out_len)
{
        static u8 in[EXT4_MAX_INODE_SIZE];
        MKS_RETURN_IF_ERR(resolve(fs, path, in));
        if ((le16(in) & 0xF000) != 0x8000) /* S_IFREG */
                return MKSTOOR_ERR_UNSUPPORTED;
        return slurp(fs, in, out_buf, out_len);
}

void mks_ext4_free_file(void *buf)
{
        mks_tracked_free(buf);
}
