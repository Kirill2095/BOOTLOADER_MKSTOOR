/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: SHA-256 (FIPS 180-4) + RSA-SHA256 PKCS#1 v1.5 verify.
 * Small, constant-flow-ish reference implementations; the Rust modules will
 * replace them on the hot path in a later stage, but the boot pipeline must
 * be able to measure and verify with only the C core present.
 */
#ifndef MKSTOOR_CRYPTO_H
#define MKSTOOR_CRYPTO_H

#include <mkstoor/types.h>

#define MKS_SHA256_DIGEST_LEN 32

struct mks_sha256_ctx {
        u32 state[8];
        u64 bitlen;
        u8  buf[64];
        usize buflen;
};

MKSTOOR_API void mks_sha256_init(struct mks_sha256_ctx *ctx);
MKSTOOR_API void mks_sha256_update(struct mks_sha256_ctx *ctx,
                                   const void *data, usize len);
MKSTOOR_API void mks_sha256_final(struct mks_sha256_ctx *ctx, u8 out[32]);
MKSTOOR_API mkstoor_err_t mks_sha256_buf(const void *data, usize len,
                                         u8 out[32]);

/* Modular arithmetic helpers for RSA (little-endian limbs). */
typedef struct {
        u32 *d;      /* limbs, little-endian */
        usize n;     /* limb count           */
} mks_bn_t;

/* Verify an RSA-PKCS1v1.5 signature over `msg` (not its hash) given the
 * public key as raw (n, e) big-endian byte strings. Returns MKSTOOR_OK if
 * and only if the recovered digest equals sha256(msg). */
MKSTOOR_API mkstoor_err_t mks_rsa_sha256_verify(const void *msg, usize msg_len,
                                                const u8 *sig, usize sig_len,
                                                const u8 *n_be, usize n_len,
                                                const u8 *e_be, usize e_len);

#endif /* MKSTOOR_CRYPTO_H */
