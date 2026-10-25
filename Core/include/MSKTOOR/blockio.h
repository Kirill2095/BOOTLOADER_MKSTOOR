/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: block device abstraction over EFI_BLOCK_IO. */
#ifndef MKSTOOR_BLOCKIO_H
#define MKSTOOR_BLOCKIO_H

#include <mkstoor/types.h>
#include <mkstoor/efi.h>

/* Opaque handle handed to fs modules through the capability vector. */
struct mks_block_dev {
        EFI_BLOCK_IO_PROTOCOL *bio;
        EFI_HANDLE             handle;
        const char            *owner; /* accounting tag */
};

/* Enumerate every connected block device with media present. */
MKSTOOR_API mkstoor_err_t mks_blockio_enumerate(EFI_SYSTEM_TABLE *st,
                                                struct mks_block_dev **out,
                                                usize *count);

MKSTOOR_API u32 mks_block_size(const struct mks_block_dev *dev);
MKSTOOR_API u64 mks_block_count(const struct mks_block_dev *dev);

/* Capability-vector implementation (core/module.c wires these in). */
mkstoor_err_t mks_bio_read_blocks(struct mks_block_dev *dev, u64 lba,
                                  void *buf, u32 count);
u32  mks_bio_block_size(struct mks_block_dev *dev);
u64  mks_bio_block_count(struct mks_block_dev *dev);

#endif /* MKSTOOR_BLOCKIO_H */
