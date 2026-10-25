/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: ext4 read-only reader (C core).
 *
 * Supports the subset Linux distros actually ship kernels on:
 *   - ext2/3/4 superblock v1/v2 geometry, 128- and 64-byte inodes
 *   - extents (INCOMPAT_EXTENTS) for regular files
 *   - indirect blocks (legacy) up to triple-indirect
 *   - dir_index (htree) NOT required: linear directory scan only
 * Feature-flag handling is strict: any unknown *incompat* feature aborts
 * the mount with MKSTOOR_ERR_UNSUPPORTED rather than misparsing data
 * (the BootHole lesson: parsers must refuse what they don't understand).
 */
#ifndef MKSTOOR_EXT4_H
#define MKSTOOR_EXT4_H

#include <mkstoor/types.h>

struct mks_block_dev;

typedef struct mks_ext4_fs mks_ext4_fs_t;

MKSTOOR_API mkstoor_err_t mks_ext4_mount(struct mks_block_dev *dev,
                                         mks_ext4_fs_t **out_fs);
MKSTOOR_API void          mks_ext4_unmount(mks_ext4_fs_t *fs);

MKSTOOR_API mkstoor_err_t mks_ext4_read_file(mks_ext4_fs_t *fs, const char *path,
                                             void **out_buf, usize *out_len);
MKSTOOR_API void mks_ext4_free_file(void *buf);

MKSTOOR_API bool mks_ext4_exists(mks_ext4_fs_t *fs, const char *path);

#endif /* MKSTOOR_EXT4_H */
