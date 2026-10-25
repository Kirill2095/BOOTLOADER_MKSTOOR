/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — module registry & capability API (design philosophy #1).
 *
 * The core is a microkernel: filesystems, crypto and scripting are modules
 * that receive a *capability-limited* API vector. A fs module can read raw
 * blocks and allocate tracked frames but cannot touch the GUI or boot the
 * kernel; only the boot pipeline holds the exec capability.
 */
#ifndef MKSTOOR_MODULE_H
#define MKSTOOR_MODULE_H

#include <mkstoor/types.h>

struct mks_block_dev;   /* opaque device handle (core/blockio.c) */
struct mks_file;        /* opaque open file handle               */

/* Capability vectors, one struct per privilege class. */
struct mks_caps_fs {
        /* Read `count` logical blocks starting at `lba` into `buf`.
         * buf must be caller-owned and sized count*block_size. */
        mkstoor_err_t (*read_blocks)(struct mks_block_dev *dev, u64 lba,
                                     void *buf, u32 count);
        u32  (*block_size)(struct mks_block_dev *dev);
        u64  (*block_count)(struct mks_block_dev *dev);
        void *(*alloc)(usize size);              /* tracked, owner-tagged */
        void (*free)(void *p, usize size);
};

struct mks_caps_crypto {
        /* Streaming SHA-256 over an in-memory buffer. digest: 32 bytes. */
        mkstoor_err_t (*sha256)(const void *data, usize len, u8 digest[32]);
        /* RSA-2048/SHA-256 signature verify against a DER-encoded public key. */
        mkstoor_err_t (*rsa_verify)(const u8 *msg_hash, const u8 *sig, usize sig_len,
                                    const u8 *pubkey_der, usize der_len);
};

struct mks_caps_exec {
        /* Boot a Linux image with initrd/cmdline (EFI handover). */
        mkstoor_err_t (*boot_kernel)(const void *kernel, usize klen,
                                     const void *initrd, usize ilen,
                                     const char *cmdline);
};

typedef enum {
        MKS_MOD_FS     = 1 << 0,
        MKS_MOD_CRYPTO = 1 << 1,
        MKS_MOD_EXEC   = 1 << 2,
} mks_cap_mask_t;

struct mks_api {
        const struct mks_caps_fs     *fs;     /* NULL unless granted  */
        const struct mks_caps_crypto *crypto; /* NULL unless granted  */
        const struct mks_caps_exec   *exec;   /* NULL unless granted  */
};

struct mks_module_info {
        const char *name;        /* e.g. "fs.fat"                            */
        const char *version;
        mks_cap_mask_t wants;    /* capabilities requested at registration   */
        /* Entry point invoked by the loader after capability grant. */
        mkstoor_err_t (*init)(const struct mks_api *api);
};

MKSTOOR_API mkstoor_err_t mks_module_register(const struct mks_module_info *mod);
MKSTOOR_API mkstoor_err_t mks_module_init_all(void);
MKSTOOR_API const struct mks_module_info *mks_module_find(const char *name);
MKSTOOR_API usize mks_module_count(void);

/* Total tracked allocations attributed to a module (leak reporting). */
MKSTOOR_API u64 mks_module_bytes_owned(const char *name);

#endif /* MKSTOOR_MODULE_H */
