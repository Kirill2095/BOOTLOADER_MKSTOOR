/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2/4 crypto: SHA-256 (FIPS 180-4) and RSA-SHA256
 * PKCS#1 v1.5 signature verification, implemented in bounded C so the
 * boot pipeline can measure and verify images without the Rust modules.
 *
 * The SHA-256 core is a straightforward table-driven reference
 * implementation; correctness is pinned by host unit tests
 * (tests/sha256_vec.c — FIPS 180-4 short/long vectors). The RSA path uses
 * little-endian 32-bit limb Montgomery-free modexp with square-and-multiply
 * over plain bignum arithmetic: signatures are only verified once per boot
 * over <= 256-byte operands, so constant-time performance is not on the
 * critical path (the design rule that matters here is *rejecting* anything
 * malformed rather than mis-verifying it).
 */
#include <mkstoor/types.h>
#include <mkstoor/crypto.h>

/* ------------------------------------------------------------------------ */
/* SHA-256                                                                   */
/* ------------------------------------------------------------------------ */
static const u32 K256[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c19588u, 0x92caf7c4u,
        0xa2ff8300u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aaacu, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline u32 ror32(u32 x, unsigned n)
{
        return (x >> n) | (x << ((32 - n) & 31));
}

#define CH(x, y, z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BS0_256(x)    (ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22))
#define BS1_256(x)    (ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25))
#define SS0_256(x)    (ror32(x, 7) ^ ror32(x, 18) ^ ((x) >> 3))
#define SS1_256(x)    (ror32(x, 17) ^ ror32(x, 19) ^ ((x) >> 10))

static void sha256_block(struct mks_sha256_ctx *ctx, const u8 *p)
{
        u32 w[64];
        for (int i = 0; i < 16; i++)
                w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) |
                       ((u32)p[4 * i + 2] << 8) | (u32)p[4 * i + 3];
        for (int i = 16; i < 64; i++)
                w[i] = SS1_256(w[i - 2]) + w[i - 7] + SS0_256(w[i - 15]) +
                       w[i - 16];

        u32 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2],
            d = ctx->state[3], e = ctx->state[4], f = ctx->state[5],
            g = ctx->state[6], h = ctx->state[7];

        for (int i = 0; i < 64; i++) {
                u32 t1 = h + BS1_256(e) + CH(e, f, g) + K256[i] + w[i];
                u32 t2 = BS0_256(a) + MAJ(a, b, c);
                h = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
        }

        ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
        ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
        ctx->state[6] += g; ctx->state[7] += h;
}

void mks_sha256_init(struct mks_sha256_ctx *ctx)
{
        ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
        ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
        ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
        ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
        ctx->bitlen = 0;
        ctx->buflen = 0;
}

void mks_sha256_update(struct mks_sha256_ctx *ctx, const void *data, usize len)
{
        const u8 *p = data;
        while (len) {
                usize take = 64 - ctx->buflen;
                if (take > len)
                        take = len;
                for (usize i = 0; i < take; i++)
                        ctx->buf[ctx->buflen + i] = p[i];
                ctx->buflen += take;
                p += take;
                len -= take;
                if (ctx->buflen == 64) {
                        sha256_block(ctx, ctx->buf);
                        ctx->bitlen += 512;
                        ctx->buflen = 0;
                }
        }
}

void mks_sha256_final(struct mks_sha256_ctx *ctx, u8 out[32])
{
        u64 total_bits = ctx->bitlen + (u64)ctx->buflen * 8;

        u8 pad = 0x80;
        mks_sha256_update(ctx, &pad, 1);
        u8 zero = 0;
        while (ctx->buflen != 56)
                mks_sha256_update(ctx, &zero, 1);

        u8 len_be[8];
        for (int i = 0; i < 8; i++)
                len_be[i] = (u8)(total_bits >> (56 - 8 * i));
        mks_sha256_update(ctx, len_be, 8);

        for (int i = 0; i < 8; i++) {
                out[4 * i]     = (u8)(ctx->state[i] >> 24);
                out[4 * i + 1] = (u8)(ctx->state[i] >> 16);
                out[4 * i + 2] = (u8)(ctx->state[i] >> 8);
                out[4 * i + 3] = (u8)(ctx->state[i]);
        }
}

mkstoor_err_t mks_sha256_buf(const void *data, usize len, u8 out[32])
{
        if (!out || (!data && len))
                return MKSTOOR_ERR_INVALID_ARG;
        struct mks_sha256_ctx ctx;
        mks_sha256_init(&ctx);
        mks_sha256_update(&ctx, data, len);
        mks_sha256_final(&ctx, out);
        return MKSTOOR_OK;
}

/* ------------------------------------------------------------------------ */
/* Bignum (little-endian u32 limbs) for RSA verify                           */
/* ------------------------------------------------------------------------ */
#define BN_MAX_LIMBS 16 /* 512 bits: enough for RSA-1024..RSA-4096 modexp */

typedef struct {
        u32 d[BN_MAX_LIMBS];
        usize n; /* significant limb count, >= 1 */
} bn;

static void bn_zero(bn *x)
{
        for (usize i = 0; i < BN_MAX_LIMBS; i++)
                x->d[i] = 0;
        x->n = 1;
}

static void bn_from_be(bn *x, const u8 *src, usize len)
{
        bn_zero(x);
        for (usize i = 0; i < len; i++) {
                /* value = value*256 + src[i], big-endian stream */
                u64 carry = src[i];
                for (usize l = 0; l < BN_MAX_LIMBS; l++) {
                        u64 v = (u64)x->d[l] * 256 + carry;
                        x->d[l] = (u32)v;
                        carry = v >> 32;
                }
        }
        /* normalise limb count */
        x->n = BN_MAX_LIMBS;
        while (x->n > 1 && x->d[x->n - 1] == 0)
                x->n--;
}

static int bn_is_zero(const bn *x)
{
        for (usize i = 0; i < x->n; i++)
                if (x->d[i])
                        return 0;
        return 1;
}

static int bn_cmp(const bn *a, const bn *b)
{
        usize n = MAX(a->n, b->n);
        for (usize i = n; i-- > 0;) {
                u32 av = i < a->n ? a->d[i] : 0;
                u32 bv = i < b->n ? b->d[i] : 0;
                if (av != bv)
                        return av > bv ? 1 : -1;
        }
        return 0;
}

/* x -= y, requires x >= y */
static void bn_sub(bn *x, const bn *y)
{
        i64 borrow = 0;
        for (usize i = 0; i < BN_MAX_LIMBS; i++) {
                i64 v = (i64)x->d[i] - (i < y->n ? (i64)y->d[i] : 0) - borrow;
                if (v < 0) {
                        v += ((i64)1 << 32);
                        borrow = 1;
                } else {
                        borrow = 0;
                }
                x->d[i] = (u32)v;
        }
        x->n = BN_MAX_LIMBS;
        while (x->n > 1 && x->d[x->n - 1] == 0)
                x->n--;
}

/* rem window width in limbs: BN_MAX_LIMBS + 1 so that (rem<<32 | digit)
 * never loses bits while reducing a double-width product modulo m. */
#define BN_REM_LIMBS (BN_MAX_LIMBS + 1)

typedef struct {
u32 d[BN_REM_LIMBS];
} bnw; /* wide remainder cell */

static void bnw_zero(bnw *x)
{
for (usize i = 0; i < BN_REM_LIMBS; i++)
x->d[i] = 0;
}

/* w <<= 1, returns the bit pushed past the top (always 0 here because the
 * window has one spare limb and callers keep w < m*2^32). */
static void bnw_shl1(bnw *w)
{
u32 carry = 0;
for (usize i = 0; i < BN_REM_LIMBS; i++) {
u32 nc = w->d[i] >> 31;
w->d[i] = (w->d[i] << 1) | carry;
carry = nc;
}
}

static int bnw_ge_m(const bnw *w, const bn *m)
{
for (usize i = BN_REM_LIMBS; i-- > 0;) {
u32 mv = i < m->n ? m->d[i] : 0;
if (w->d[i] != mv)
return w->d[i] > mv;
}
return 1; /* equal */
}

static void bnw_sub_m(bnw *w, const bn *m)
{
i64 borrow = 0;
for (usize i = 0; i < BN_REM_LIMBS; i++) {
i64 v = (i64)w->d[i] - (i < m->n ? (i64)m->d[i] : 0) - borrow;
if (v < 0) {
v += ((i64)1 << 32);
borrow = 1;
} else {
borrow = 0;
}
w->d[i] = (u32)v;
}
}

/* r = a*b mod m. Schoolbook multiply into a double-width product array,
 * then bitwise long division (invariant: remainder < m before each new
 * input bit, so the shift never overflows the spare-limb window).
 * Operands must be < m; the result is fully reduced (< m). */
static void bn_mulmod(bn *r, const bn *a, const bn *b, const bn *m)
{
u32 prod[2 * BN_MAX_LIMBS];
for (usize i = 0; i < 2 * BN_MAX_LIMBS; i++)
prod[i] = 0;

usize an = MIN(a->n, (usize)BN_MAX_LIMBS);
usize bl = MIN(b->n, (usize)BN_MAX_LIMBS);
for (usize i = 0; i < an; i++) {
u64 carry = 0;
for (usize j = 0; j < bl; j++) {
u64 v = (u64)prod[i + j] + (u64)a->d[i] * b->d[j] + carry;
prod[i + j] = (u32)v;
carry = v >> 32;
}
usize p = i + bl;
while (carry && p < 2 * BN_MAX_LIMBS) {
u64 v = (u64)prod[p] + carry;
prod[p] = (u32)v;
carry = v >> 32;
p++;
}
}

/* find the topmost set bit of the product to bound the loop */
int top_limb = -1;
for (usize i = 0; i < 2 * BN_MAX_LIMBS; i++)
if (prod[i])
top_limb = (int)i;
/* iterate bits from high to low: rem = rem*2 + bit; if rem >= m: -= m */
bnw rem;
bnw_zero(&rem);
for (int limb = top_limb; limb >= 0; limb--) {
for (int bit = 31; bit >= 0; bit--) {
bnw_shl1(&rem);
rem.d[0] |= (prod[limb] >> bit) & 1;
if (bnw_ge_m(&rem, m))
bnw_sub_m(&rem, m);
}
}

for (usize i = 0; i < BN_MAX_LIMBS; i++)
r->d[i] = rem.d[i];
r->n = BN_MAX_LIMBS;
while (r->n > 1 && r->d[r->n - 1] == 0)
r->n--;
}

/* r = base^exp mod m, exp given as big-endian bytes (public exponent) */
static mkstoor_err_t bn_modexp_be(bn *r, const bn *base, const u8 *exp_be,
                                  usize exp_len, const bn *m)
{
        bn result, b;
        bn_zero(&result);
        result.d[0] = 1;
        b = *base;
        if (bn_cmp(&b, m) >= 0)
                bn_sub(&b, m);

        for (usize i = 0; i < exp_len; i++) {
                u8 byte = exp_be[i];
                for (int bit = 7; bit >= 0; bit--) {
                        bn_mulmod(&result, &result, &result, m);
                        if ((byte >> bit) & 1)
                                bn_mulmod(&result, &result, &b, m);
                        if (bn_is_zero(&result))
                                return MKSTOOR_ERR_CORRUPT; /* degenerate key */
                }
        }
        *r = result;
        return MKSTOOR_OK;
}

static void bn_to_be(const bn *x, u8 *dst, usize len)
{
        /* write exactly `len` big-endian bytes (zero-padded / low bytes kept) */
        for (usize i = 0; i < len; i++) {
                usize byte_idx = len - 1 - i; /* 0-based from LSB */
                usize limb = byte_idx / 4;
                unsigned shift = (unsigned)(byte_idx % 4) * 8;
                dst[i] = (limb < BN_MAX_LIMBS)
                         ? (u8)((x->d[limb] >> shift) & 0xff) : 0;
        }
}

/* DER DigestInfo for SHA-256 (RFC 8017 §9.2 / NIST example):
 * 30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20 */
static const u8 SHA256_DIGESTINFO_PREFIX[19] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};

/* Decode an EMSA-PKCS1-v1_5 encoded message that must carry the SHA-256
 * DigestInfo of `msg`. Returns MKSTOOR_OK only on an exact match. */
static mkstoor_err_t pkcs1v15_check(const u8 *em, usize em_len,
                                    const void *msg, usize msg_len)
{
        if (em_len < 12 || em[0] != 0x00 || em[1] != 0x01)
                return MKSTOOR_ERR_SECURITY;
        usize i = 2;
        usize ps = 0;
        while (i < em_len && em[i] == 0xff) {
                i++;
                ps++;
        }
        /* PS must be at least 8 bytes and the block ends with the 51-byte
         * DER DigestInfo for SHA-256 (19-byte prefix + 32-byte digest). */
        if (ps < 8 || i >= em_len || em[i] != 0x00)
                return MKSTOOR_ERR_SECURITY;
        if (em_len - (i + 1) != sizeof(SHA256_DIGESTINFO_PREFIX) + 32)
                return MKSTOOR_ERR_SECURITY;

        const u8 *di = em + i + 1;
        for (usize k = 0; k < sizeof(SHA256_DIGESTINFO_PREFIX); k++)
                if (di[k] != SHA256_DIGESTINFO_PREFIX[k])
                        return MKSTOOR_ERR_SECURITY;

        u8 expect[MKS_SHA256_DIGEST_LEN];
        MKS_RETURN_IF_ERR(mks_sha256_buf(msg, msg_len, expect));
        for (int k = 0; k < MKS_SHA256_DIGEST_LEN; k++)
                if (di[sizeof(SHA256_DIGESTINFO_PREFIX) + k] != expect[k])
                        return MKSTOOR_ERR_SECURITY;
        return MKSTOOR_OK;
}

mkstoor_err_t mks_rsa_sha256_verify(const void *msg, usize msg_len,
                                    const u8 *sig, usize sig_len,
                                    const u8 *n_be, usize n_len,
                                    const u8 *e_be, usize e_len)
{
        if (!msg || !sig || !n_be || !e_be)
                return MKSTOOR_ERR_INVALID_ARG;
        /* Key-size bounds: modulus must fit our limbs and match the signature. */
        if (n_len == 0 || n_len > BN_MAX_LIMBS * 4 || sig_len != n_len)
                return MKSTOOR_ERR_UNSUPPORTED;
        if (e_len == 0 || e_len > 8)
                return MKSTOOR_ERR_UNSUPPORTED;

        bn n, s;
        bn_from_be(&n, n_be, n_len);
        bn_from_be(&s, sig, sig_len);
        if (bn_is_zero(&n) || (n.d[0] & 1) == 0)
                return MKSTOOR_ERR_SECURITY; /* even/zero modulus: invalid key */
        if (bn_cmp(&s, &n) >= 0)
                return MKSTOOR_ERR_SECURITY;

        /* RSASSA-PKCS1-v1_5 verify: em = EPKCS1-ENCRYPT(sig) = sig^e mod n */
        bn rec;
        MKS_RETURN_IF_ERR(bn_modexp_be(&rec, &s, e_be, e_len, &n));

        u8 em[BN_MAX_LIMBS * 4];
        bn_to_be(&rec, em, n_len);
        return pkcs1v15_check(em, n_len, msg, msg_len);
}
