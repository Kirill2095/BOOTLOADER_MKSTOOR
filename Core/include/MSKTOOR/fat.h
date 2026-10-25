/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: FAT32 filesystem reader (C core).
 *
 * Scope: the ESP is virtually always FAT32, so we implement a bounded,
 * fully-validated FAT32 reader natively in the core. It never dereferences
 * an on-disk value without bounds-checking it against the volume geometry
 * first (design invariant #3 of the architecture notes). Long filenames
 * (VFAT) are reconstructed from their UTF-16 entries; ASCII names compare
 * case-insensitively per the FAT spec.
 *
 * Not supported (returns MKSTOOR_ERR_UNSUPPORTED): FAT12/FAT16 volumes,
 * directory nesting deeper than MKS_FAT_MAX_DEPTH, streams > 512 MiB.
 */
#ifndef MKSTOOR_FAT_H
#define MKSTOOR_FAT_H

#include <mkstoor/types.h>

struct mks_block_dev; /* opaque, see core/blockio.h */

typedef struct mks_fat_fs mks_fat_fs_t;

/* Mount: reads & validates BPB, builds in-memory FAT table. */
MKSTOOR_API mkstoor_err_t mks_fat_mount(struct mks_block_dev *dev,
                                        mks_fat_fs_t **out_fs);
MKSTOOR_API void          mks_fat_unmount(mks_fat_fs_t *fs);

/* Open a file by POSIX-ish path ("/EFI/MKSTOOR/mkstoor.toml").
 * Caller owns `out_buf` afterwards and must release with mks_fat_free_file(). */
MKSTOOR_API mkstoor_err_t mks_fat_read_file(mks_fat_fs_t *fs, const char *path,
                                            void **out_buf, usize *out_len);
MKSTOOR_API void mks_fat_free_file(void *buf);

/* Existence probe used by the config discovery logic. */
MKSTOOR_API bool mks_fat_exists(mks_fat_fs_t *fs, const char *path);

#endif /* MKSTOOR_FAT_H */
