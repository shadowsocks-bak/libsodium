
/*
 * AES256-GCM, based on original code by Romain Dolbeau
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "crypto_aead_aes256gcm_aesni.h"
#include "export.h"
#include "utils.h"

#ifdef HAVE_WMMINTRIN_H

#pragma GCC target("sse")
#pragma GCC target("sse2")
#pragma GCC target("ssse3")
#pragma GCC target("sse4.1")
#pragma GCC target("aes")
#pragma GCC target("pclmul")

#ifndef __SSE4_1__
# define __SSE4_1__
#endif
#ifndef __AES__
# define __AES__
#endif
#ifndef __PCLMUL__
# define __PCLMUL__
#endif
#include <immintrin.h>

#if defined(__INTEL_COMPILER) || defined(_bswap64)
#elif defined(_MSC_VER)
# define _bswap64(a) _byteswap_uint64(a)
#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
# define _bswap64(a) __builtin_bswap64(a)
#else
static inline uint64_t
_bswap64(const uint64_t x)
{
    return
        ((x << 56) & 0xFF00000000000000UL) | ((x << 40) & 0x00FF000000000000UL) |
        ((x << 24) & 0x0000FF0000000000UL) | ((x <<  8) & 0x000000FF00000000UL) |
        ((x >>  8) & 0x00000000FF000000UL) | ((x >> 24) & 0x0000000000FF0000UL) |
        ((x >> 40) & 0x000000000000FF00UL) | ((x >> 56) & 0x00000000000000FFUL);
}
#endif

typedef struct context {
    CRYPTO_ALIGN(16) unsigned char H[16];
    __m128i          rkeys[16];
} context;

static inline void
aesni_key256_expand(const unsigned char *key, __m128 rkeys[16])
{
    __m128 key0 = _mm_loadu_ps((const float *) (key + 0));
    __m128 key1 = _mm_loadu_ps((const float *) (key + 16));
    __m128 temp0, temp1, temp2, temp4;
    int    idx = 0;

    rkeys[idx++] = key0;
    temp0 = key0;
    temp2 = key1;
    temp4 = _mm_setzero_ps();

/* why single precision floating-point rather than integer instructions ?
     because _mm_shuffle_ps takes two inputs, while _mm_shuffle_epi32 only
     takes one - it doesn't perform the same computation...
     _mm_shuffle_ps takes the lower 64 bits of the result from the first
     operand, and the higher 64 bits of the result from the second operand
     (in both cases, all four input floats are accessible).
     I don't like the non-orthogonal naming scheme :-(

     This is all strongly inspired by the openssl assembly code.
  */
#define BLOCK1(IMM)                                                 \
    temp1 = (__m128)_mm_aeskeygenassist_si128((__m128i) temp2, IMM);\
    rkeys[idx++] = temp2;                                           \
    temp4 = _mm_shuffle_ps(temp4, temp0, 0x10);                     \
    temp0 = _mm_xor_ps(temp0, temp4);                               \
    temp4 = _mm_shuffle_ps(temp4, temp0, 0x8c);                     \
    temp0 = _mm_xor_ps(temp0, temp4);                               \
    temp1 = _mm_shuffle_ps(temp1, temp1, 0xff);                     \
    temp0 = _mm_xor_ps(temp0, temp1)

#define BLOCK2(IMM)                                                 \
    temp1 = (__m128)_mm_aeskeygenassist_si128((__m128i) temp0, IMM);\
    rkeys[idx++] = temp0;                                           \
    temp4 = _mm_shuffle_ps(temp4, temp2, 0x10);                     \
    temp2 = _mm_xor_ps(temp2, temp4);                               \
    temp4 = _mm_shuffle_ps(temp4, temp2, 0x8c);                     \
    temp2 = _mm_xor_ps(temp2, temp4);                               \
    temp1 = _mm_shuffle_ps(temp1, temp1, 0xaa);                     \
    temp2 = _mm_xor_ps(temp2, temp1)

    BLOCK1(0x01);
    BLOCK2(0x01);

    BLOCK1(0x02);
    BLOCK2(0x02);

    BLOCK1(0x04);
    BLOCK2(0x04);

    BLOCK1(0x08);
    BLOCK2(0x08);

    BLOCK1(0x10);
    BLOCK2(0x10);

    BLOCK1(0x20);
    BLOCK2(0x20);

    BLOCK1(0x40);
    rkeys[idx++] = temp0;
}

/** single, by-the-book AES encryption with AES-NI */
static inline void
aesni_encrypt1(unsigned char *out, __m128i nv, const __m128i rkeys[16])
{
    __m128i temp = _mm_xor_si128(nv, rkeys[0]);
    int     i;

#pragma unroll(13)
    for (i = 1; i < 14; i++) {
        temp = _mm_aesenc_si128(temp, rkeys[i]);
    }
    temp = _mm_aesenclast_si128(temp, rkeys[14]);
    _mm_store_si128((__m128i *) out, temp);
}

/** multiple-blocks-at-once AES encryption with AES-NI ;
    on Haswell, aesenc as a latency of 7 and a througput of 1
    so the sequence of aesenc should be bubble-free, if you
    have at least 8 blocks. Let's build an arbitratry-sized
    function */
/* Step 1 : loading the nonce */
/* load & increment the n vector (non-vectorized, unused for now) */
#define NVx(a)                                                               \
    __m128i nv##a = _mm_shuffle_epi8(_mm_load_si128((const __m128i *) n), pt); \
    n[3]++

/* Step 2 : define value in round one (xor with subkey #0, aka key) */
#define TEMPx(a) \
    __m128i temp##a = _mm_xor_si128(nv##a, rkeys[0])

/* Step 3: one round of AES */
#define AESENCx(a) \
    temp##a = _mm_aesenc_si128(temp##a, rkeys[i])

/* Step 4: last round of AES */
#define AESENCLASTx(a) \
    temp##a = _mm_aesenclast_si128(temp##a, rkeys[14])

/* Step 5: store result */
#define STOREx(a) \
    _mm_store_si128((__m128i *) (out + (a * 16)), temp##a)

/* all the MAKE* macros are for automatic explicit unrolling */
#define MAKE4(X) \
    X(0);        \
    X(1);        \
    X(2);        \
    X(3)

#define MAKE8(X) \
    X(0);        \
    X(1);        \
    X(2);        \
    X(3);        \
    X(4);        \
    X(5);        \
    X(6);        \
    X(7)

#define COUNTER_INC2(N) (*(uint32_t *) &(N)[12]) = (2U + (((*(uint32_t *) &(N)[12]))))

/* create a function of unrolling N ; the MAKEN is the unrolling
   macro, defined above. The N in MAKEN must match N, obviously. */
#define FUNC(N, MAKEN)                                                                                \
    static inline void aesni_encrypt##N(unsigned char *out, uint32_t *n, const __m128i rkeys[16])     \
    {                                                                                                 \
        const __m128i pt = _mm_set_epi8(12, 13, 14, 15, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);        \
        int   i;                                                                                      \
                                                                                                      \
        MAKEN(NVx);                                                                                   \
        MAKEN(TEMPx);                                                                                 \
        for (i = 1; i < 14; i++) {                                                                    \
            MAKEN(AESENCx);                                                                           \
        }                                                                                             \
        MAKEN(AESENCLASTx);                                                                           \
        MAKEN(STOREx);                                                                                \
    }

FUNC(8, MAKE8)

/* all GF(2^128) fnctions are by the book, meaning this one:
   <https://software.intel.com/sites/default/files/managed/72/cc/clmul-wp-rev-2.02-2014-04-20.pdf>
*/

static inline void
addmul(unsigned char *c, const unsigned char *a, unsigned int xlen, const unsigned char *b)
{
    const __m128i rev = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const __m128i ff = _mm_set1_epi32(-1);
    __m128i       A = _mm_loadu_si128((const __m128i *) a);

    A = _mm_shuffle_epi8(A, rev);
    if (xlen < 16) { /* less than 16 useful bytes - insert zeroes where needed */
        uint64_t mask = -1ull ^ (1ull << (((16 - xlen) % 8) * 8)) - 1ull;
        __m128i  vm;

        if (xlen > 8) {
            vm = _mm_insert_epi64(ff, mask, 0);
        } else {
            vm = _mm_insert_epi64(_mm_setzero_si128(), mask, 1);
        }
        A = _mm_and_si128(vm, A);
    }
    __m128i B = _mm_loadu_si128((const __m128i *) b);
    __m128i C = _mm_loadu_si128((const __m128i *) c);
    A = _mm_xor_si128(A, C);
    __m128i tmp3 = _mm_clmulepi64_si128(A, B, 0x00);
    __m128i tmp4 = _mm_clmulepi64_si128(A, B, 0x10);
    __m128i tmp5 = _mm_clmulepi64_si128(A, B, 0x01);
    __m128i tmp6 = _mm_clmulepi64_si128(A, B, 0x11);
    __m128i tmp10 = _mm_xor_si128(tmp4, tmp5);
    __m128i tmp13 = _mm_slli_si128(tmp10, 8);
    __m128i tmp11 = _mm_srli_si128(tmp10, 8);
    __m128i tmp15 = _mm_xor_si128(tmp3, tmp13);
    __m128i tmp17 = _mm_xor_si128(tmp6, tmp11);
    __m128i tmp7 = _mm_srli_epi32(tmp15, 31);
    __m128i tmp8 = _mm_srli_epi32(tmp17, 31);
    __m128i tmp16 = _mm_slli_epi32(tmp15, 1);
    __m128i tmp18 = _mm_slli_epi32(tmp17, 1);
    __m128i tmp9 = _mm_srli_si128(tmp7, 12);
    __m128i tmp22 = _mm_slli_si128(tmp8, 4);
    __m128i tmp25 = _mm_slli_si128(tmp7, 4);
    __m128i tmp29 = _mm_or_si128(tmp16, tmp25);
    __m128i tmp19 = _mm_or_si128(tmp18, tmp22);
    __m128i tmp20 = _mm_or_si128(tmp19, tmp9);
    __m128i tmp26 = _mm_slli_epi32(tmp29, 31);
    __m128i tmp23 = _mm_slli_epi32(tmp29, 30);
    __m128i tmp32 = _mm_slli_epi32(tmp29, 25);
    __m128i tmp27 = _mm_xor_si128(tmp26, tmp23);
    __m128i tmp28 = _mm_xor_si128(tmp27, tmp32);
    __m128i tmp24 = _mm_srli_si128(tmp28, 4);
    __m128i tmp33 = _mm_slli_si128(tmp28, 12);
    __m128i tmp30 = _mm_xor_si128(tmp29, tmp33);
    __m128i tmp2 = _mm_srli_epi32(tmp30, 1);
    __m128i tmp12 = _mm_srli_epi32(tmp30, 2);
    __m128i tmp14 = _mm_srli_epi32(tmp30, 7);
    __m128i tmp34 = _mm_xor_si128(tmp2, tmp12);
    __m128i tmp35 = _mm_xor_si128(tmp34, tmp14);
    __m128i tmp36 = _mm_xor_si128(tmp35, tmp24);
    __m128i tmp31 = _mm_xor_si128(tmp30, tmp36);
    __m128i tmp21 = _mm_xor_si128(tmp20, tmp31);
    _mm_storeu_si128((__m128i *) c, tmp21);
}

/* pure multiplication, for pre-computing  powers of H */
static inline __m128i
mulv(__m128i A, __m128i B)
{
    __m128i tmp3 = _mm_clmulepi64_si128(A, B, 0x00);
    __m128i tmp4 = _mm_clmulepi64_si128(A, B, 0x10);
    __m128i tmp5 = _mm_clmulepi64_si128(A, B, 0x01);
    __m128i tmp6 = _mm_clmulepi64_si128(A, B, 0x11);
    __m128i tmp10 = _mm_xor_si128(tmp4, tmp5);
    __m128i tmp13 = _mm_slli_si128(tmp10, 8);
    __m128i tmp11 = _mm_srli_si128(tmp10, 8);
    __m128i tmp15 = _mm_xor_si128(tmp3, tmp13);
    __m128i tmp17 = _mm_xor_si128(tmp6, tmp11);
    __m128i tmp7 = _mm_srli_epi32(tmp15, 31);
    __m128i tmp8 = _mm_srli_epi32(tmp17, 31);
    __m128i tmp16 = _mm_slli_epi32(tmp15, 1);
    __m128i tmp18 = _mm_slli_epi32(tmp17, 1);
    __m128i tmp9 = _mm_srli_si128(tmp7, 12);
    __m128i tmp22 = _mm_slli_si128(tmp8, 4);
    __m128i tmp25 = _mm_slli_si128(tmp7, 4);
    __m128i tmp29 = _mm_or_si128(tmp16, tmp25);
    __m128i tmp19 = _mm_or_si128(tmp18, tmp22);
    __m128i tmp20 = _mm_or_si128(tmp19, tmp9);
    __m128i tmp26 = _mm_slli_epi32(tmp29, 31);
    __m128i tmp23 = _mm_slli_epi32(tmp29, 30);
    __m128i tmp32 = _mm_slli_epi32(tmp29, 25);
    __m128i tmp27 = _mm_xor_si128(tmp26, tmp23);
    __m128i tmp28 = _mm_xor_si128(tmp27, tmp32);
    __m128i tmp24 = _mm_srli_si128(tmp28, 4);
    __m128i tmp33 = _mm_slli_si128(tmp28, 12);
    __m128i tmp30 = _mm_xor_si128(tmp29, tmp33);
    __m128i tmp2 = _mm_srli_epi32(tmp30, 1);
    __m128i tmp12 = _mm_srli_epi32(tmp30, 2);
    __m128i tmp14 = _mm_srli_epi32(tmp30, 7);
    __m128i tmp34 = _mm_xor_si128(tmp2, tmp12);
    __m128i tmp35 = _mm_xor_si128(tmp34, tmp14);
    __m128i tmp36 = _mm_xor_si128(tmp35, tmp24);
    __m128i tmp31 = _mm_xor_si128(tmp30, tmp36);
    __m128i C = _mm_xor_si128(tmp20, tmp31);

    return C;
}

/* 4 multiply-accumulate at once; again
   <https://software.intel.com/sites/default/files/managed/72/cc/clmul-wp-rev-2.02-2014-04-20.pdf>
   for the Aggregated Reduction Method & sample code.
*/
static inline __m128i
reduce4(__m128i H0, __m128i H1, __m128i H2, __m128i H3, __m128i X0, __m128i X1,
        __m128i X2, __m128i X3, __m128i acc)
{
/*algorithm by Krzysztof Jankowski, Pierre Laurent - Intel*/
#define RED_DECL(a) __m128i H##a##_X##a##_lo, H##a##_X##a##_hi, tmp##a, tmp##a##B
    MAKE4(RED_DECL);
    __m128i       lo, hi;
    __m128i       tmp8, tmp9;
    const __m128i rev = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);

/* byte-revert the inputs & xor the first one into the accumulator */
#define RED_SHUFFLE(a) X##a = _mm_shuffle_epi8(X##a, rev)
    MAKE4(RED_SHUFFLE);
    X3 = _mm_xor_si128(X3, acc);

/* 4 low H*X (x0*h0) */
#define RED_MUL_LOW(a) H##a##_X##a##_lo = _mm_clmulepi64_si128(H##a, X##a, 0x00)
    MAKE4(RED_MUL_LOW);
    lo = _mm_xor_si128(H0_X0_lo, H1_X1_lo);
    lo = _mm_xor_si128(lo, H2_X2_lo);
    lo = _mm_xor_si128(lo, H3_X3_lo);

/* 4 high H*X (x1*h1) */
#define RED_MUL_HIGH(a) H##a##_X##a##_hi = _mm_clmulepi64_si128(H##a, X##a, 0x11)
    MAKE4(RED_MUL_HIGH);
    hi = _mm_xor_si128(H0_X0_hi, H1_X1_hi);
    hi = _mm_xor_si128(hi, H2_X2_hi);
    hi = _mm_xor_si128(hi, H3_X3_hi);

/* 4 middle H*X, using Karatsuba, i.e.
     x1*h0+x0*h1 =(x1+x0)*(h1+h0)-x1*h1-x0*h0
     we already have all x1y1 & x0y0 (accumulated in hi & lo)
     (0 is low half and 1 is high half)
  */
/* permute the high and low 64 bits in H1 & X1,
     so create (h0,h1) from (h1,h0) and (x0,x1) from (x1,x0),
     then compute (h0+h1,h1+h0) and (x0+x1,x1+x0),
     and finally multiply
  */
#define RED_MUL_MID(a)                          \
    tmp##a = _mm_shuffle_epi32(H##a, 0x4e);     \
    tmp##a##B = _mm_shuffle_epi32(X##a, 0x4e);  \
    tmp##a = _mm_xor_si128(tmp##a, H##a);       \
    tmp##a##B = _mm_xor_si128(tmp##a##B, X##a); \
    tmp##a = _mm_clmulepi64_si128(tmp##a, tmp##a##B, 0x00)
    MAKE4(RED_MUL_MID);

/* substracts x1*h1 and x0*h0 */
    tmp0 = _mm_xor_si128(tmp0, lo);
    tmp0 = _mm_xor_si128(tmp0, hi);
    tmp0 = _mm_xor_si128(tmp1, tmp0);
    tmp0 = _mm_xor_si128(tmp2, tmp0);
    tmp0 = _mm_xor_si128(tmp3, tmp0);

    /* reduction */
    tmp0B = _mm_slli_si128(tmp0, 8);
    tmp0 = _mm_srli_si128(tmp0, 8);
    lo = _mm_xor_si128(tmp0B, lo);
    hi = _mm_xor_si128(tmp0, hi);
    tmp3 = lo;
    tmp2B = hi;
    tmp3B = _mm_srli_epi32(tmp3, 31);
    tmp8 = _mm_srli_epi32(tmp2B, 31);
    tmp3 = _mm_slli_epi32(tmp3, 1);
    tmp2B = _mm_slli_epi32(tmp2B, 1);
    tmp9 = _mm_srli_si128(tmp3B, 12);
    tmp8 = _mm_slli_si128(tmp8, 4);
    tmp3B = _mm_slli_si128(tmp3B, 4);
    tmp3 = _mm_or_si128(tmp3, tmp3B);
    tmp2B = _mm_or_si128(tmp2B, tmp8);
    tmp2B = _mm_or_si128(tmp2B, tmp9);
    tmp3B = _mm_slli_epi32(tmp3, 31);
    tmp8 = _mm_slli_epi32(tmp3, 30);
    tmp9 = _mm_slli_epi32(tmp3, 25);
    tmp3B = _mm_xor_si128(tmp3B, tmp8);
    tmp3B = _mm_xor_si128(tmp3B, tmp9);
    tmp8 = _mm_srli_si128(tmp3B, 4);
    tmp3B = _mm_slli_si128(tmp3B, 12);
    tmp3 = _mm_xor_si128(tmp3, tmp3B);
    tmp2 = _mm_srli_epi32(tmp3, 1);
    tmp0B = _mm_srli_epi32(tmp3, 2);
    tmp1B = _mm_srli_epi32(tmp3, 7);
    tmp2 = _mm_xor_si128(tmp2, tmp0B);
    tmp2 = _mm_xor_si128(tmp2, tmp1B);
    tmp2 = _mm_xor_si128(tmp2, tmp8);
    tmp3 = _mm_xor_si128(tmp3, tmp2);
    tmp2B = _mm_xor_si128(tmp2B, tmp3);

    return tmp2B;
}

#define XORx(a)                                                      \
    __m128i in##a = _mm_load_si128((const __m128i *) (in + a * 16)); \
    temp##a = _mm_xor_si128(temp##a, in##a)

#define LOADx(a)                                                     \
    __m128i in##a = _mm_load_si128((const __m128i *) (in + a * 16));

/* full encrypt & checksum 8 blocks at once */
static inline void
aesni_encrypt8full(unsigned char *out, uint32_t *n, const __m128i rkeys[16],
                   const unsigned char *in, unsigned char *accum,
                   const __m128i hv, const __m128i h2v, const __m128i h3v,
                   const __m128i h4v)
{
    const __m128i pt = _mm_set_epi8(12, 13, 14, 15, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    __m128i       accv = _mm_loadu_si128((const __m128i *) accum);
    int           i;

    MAKE8(NVx);
    MAKE8(TEMPx);
#pragma unroll(13)
    for (i = 1; i < 14; i++) {
        MAKE8(AESENCx);
    }
    MAKE8(AESENCLASTx);
    MAKE8(XORx);
    MAKE8(STOREx);
    accv = reduce4(hv, h2v, h3v, h4v, temp3, temp2, temp1, temp0, accv);
    accv = reduce4(hv, h2v, h3v, h4v, temp7, temp6, temp5, temp4, accv);
    _mm_storeu_si128((__m128i *) accum, accv);
}

/* checksum 8 blocks at once */
static inline void
aesni_addmul8full(const unsigned char *in, unsigned char *accum,
                  const __m128i hv, const __m128i h2v,
                  const __m128i h3v, const __m128i h4v)
{
    __m128i accv = _mm_loadu_si128((const __m128i *) accum);
    MAKE8(LOADx);
    accv = reduce4(hv, h2v, h3v, h4v, in3, in2, in1, in0, accv);
    accv = reduce4(hv, h2v, h3v, h4v, in7, in6, in5, in4, accv);
    _mm_storeu_si128((__m128i *) accum, accv);
}

/* decrypt 8 blocks at once */
static inline void
aesni_decrypt8full(unsigned char *out, uint32_t *n, const __m128i rkeys[16],
                   const unsigned char *in)
{
    const __m128i pt = _mm_set_epi8(12, 13, 14, 15, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    int           i;

    MAKE8(NVx);
    MAKE8(TEMPx);
#pragma unroll(13)
    for (i = 1; i < 14; i++) {
        MAKE8(AESENCx);
    }
    MAKE8(AESENCLASTx);
    MAKE8(XORx);
    MAKE8(STOREx);
}

int
crypto_aead_aes256gcm_aesni_beforenm(crypto_aead_aes256gcm_aesni_state *ctx_,
                                     const unsigned char *k)
{
    context       *ctx = (context *) ctx_;
    __m128i       *rkeys = ctx->rkeys;
    __m128i        zero = _mm_setzero_si128();
    unsigned char *H = ctx->H;

    (void) sizeof(int[(sizeof *ctx_) >= (sizeof *ctx) ? 1 : -1]);
    aesni_key256_expand(k, (__m128*) rkeys);
    aesni_encrypt1(H, zero, rkeys);

    return 0;
}

int
crypto_aead_aes256gcm_aesni_encrypt_afternm(unsigned char *c, unsigned long long *clen,
                                            const unsigned char *m, unsigned long long mlen,
                                            const unsigned char *ad, unsigned long long adlen,
                                            const unsigned char *nsec,
                                            const unsigned char *npub,
                                            const crypto_aead_aes256gcm_aesni_state *ctx_)
{
    unsigned char       H[16];
    const __m128i       rev = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const context      *ctx = (const context *) ctx_;
    const __m128i      *rkeys = ctx->rkeys;
    __m128i             Hv, H2v, H3v, H4v, accv;
    unsigned long long  i, j;
    unsigned long long  adlen_rnd64 = adlen & ~63ULL;
    unsigned long long  mlen_rnd128 = mlen & ~127ULL;
    CRYPTO_ALIGN(16) unsigned char n2[16];
    CRYPTO_ALIGN(16) unsigned char T[16];
    CRYPTO_ALIGN(16) unsigned char accum[16];
    CRYPTO_ALIGN(16) unsigned char fb[16];

    (void) nsec;
    memcpy(H, ctx->H, sizeof H);
    if (mlen > 16ULL * (1ULL << 32)) {
        abort();
    }
    memcpy(&n2[0], npub, 12);
    *(uint32_t *) &n2[12] = 0x01000000;
    aesni_encrypt1(T, _mm_load_si128((const __m128i *) n2), rkeys);

    (*(uint64_t *) &fb[0]) = _bswap64((uint64_t) (8 * adlen));
    (*(uint64_t *) &fb[8]) = _bswap64((uint64_t) (8 * mlen));

    /* we store H (and it's power) byte-reverted once and for all */
    Hv = _mm_shuffle_epi8(_mm_load_si128((const __m128i *) H), rev);
    _mm_store_si128((__m128i *) H, Hv);
    H2v = mulv(Hv, Hv);
    H3v = mulv(H2v, Hv);
    H4v = mulv(H3v, Hv);

    accv = _mm_setzero_si128();
    /* unrolled by 4 GCM (by 8 doesn't improve using reduce4) */
    for (i = 0; i < adlen_rnd64; i += 64) {
        __m128i X4 = _mm_loadu_si128((const __m128i *) (ad + i + 0));
        __m128i X3 = _mm_loadu_si128((const __m128i *) (ad + i + 16));
        __m128i X2 = _mm_loadu_si128((const __m128i *) (ad + i + 32));
        __m128i X1 = _mm_loadu_si128((const __m128i *) (ad + i + 48));
        accv = reduce4(Hv, H2v, H3v, H4v, X1, X2, X3, X4, accv);
    }
    _mm_storeu_si128((__m128i *) accum, accv);

    /* GCM remainder loop */
    for (i = adlen_rnd64; i < adlen; i += 16) {
        unsigned int blocklen = 16;

        if (i + (unsigned long long) blocklen > adlen) {
            blocklen = (unsigned int) (adlen - i);
        }
        addmul(accum, ad + i, blocklen, H);
    }

/* this only does 8 full blocks, so no fancy bounds checking is necessary*/
#define LOOPRND128                                                                                \
    {                                                                                             \
        const int iter = 8;                                                                       \
        const int lb = iter * 16;                                                                 \
                                                                                                  \
        for (i = 0; i < mlen_rnd128; i += lb) {                                                   \
            aesni_encrypt8full(c + i, (uint32_t *) n2, rkeys, m + i, accum, Hv, H2v, H3v, H4v);   \
        }                                                                                         \
    }

/* remainder loop, with the slower GCM update to accomodate partial blocks */
#define LOOPRMD128                                           \
    {                                                        \
        const int iter = 8;                                  \
        const int lb = iter * 16;                            \
                                                             \
        for (i = mlen_rnd128; i < mlen; i += lb) {           \
            CRYPTO_ALIGN(16) unsigned char outni[8 * 16];    \
            unsigned long long mj = lb;                      \
                                                             \
            aesni_encrypt8(outni, (uint32_t *) n2, rkeys);   \
            if ((i + mj) >= mlen) {                          \
                mj = mlen - i;                               \
            }                                                \
            for (j = 0; j < mj; j++) {                       \
                c[i + j] = m[i + j] ^ outni[j];              \
            }                                                \
            for (j = 0; j < mj; j += 16) {                   \
                unsigned int bl = 16;                        \
                                                             \
                if (j + (unsigned long long) bl >= mj) {     \
                    bl = (unsigned int) (mj - j);            \
                }                                            \
                addmul(accum, c + i + j, bl, H);             \
            }                                                \
        }                                                    \
    }

    n2[15] = 0;
    COUNTER_INC2(n2);
    LOOPRND128;
    LOOPRMD128;

    addmul(accum, fb, 16, H);

    for (i = 0; i < 16; ++i) {
        c[i + mlen] = T[i] ^ accum[15 - i];
    }
    if (clen != NULL) {
        *clen = mlen + 16;
    }
    return 0;
}

int
crypto_aead_aes256gcm_aesni_decrypt_afternm(unsigned char *m, unsigned long long *mlen_p,
                                            unsigned char *nsec,
                                            const unsigned char *c, unsigned long long clen,
                                            const unsigned char *ad, unsigned long long adlen,
                                            const unsigned char *npub,
                                            const crypto_aead_aes256gcm_aesni_state *ctx_)
{
    unsigned char       H[16];
    const __m128i       rev = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const context      *ctx = (const context *) ctx_;
    const __m128i      *rkeys = ctx->rkeys;
    __m128i             Hv, H2v, H3v, H4v, accv;
    unsigned long long  i, j;
    unsigned long long  adlen_rnd64 = adlen & ~63ULL;
    unsigned long long  mlen;
    unsigned long long  mlen_rnd128;
    CRYPTO_ALIGN(16) unsigned char n2[16];
    CRYPTO_ALIGN(16) unsigned char T[16];
    CRYPTO_ALIGN(16) unsigned char accum[16];
    CRYPTO_ALIGN(16) unsigned char fb[16];

    (void) nsec;
    memcpy(H, ctx->H, sizeof H);
    if (clen > 16ULL * (1ULL << 32) - 16ULL) {
        abort();
    }
    mlen = clen - 16;
    if (mlen_p != NULL) {
        *mlen_p = 0U;
    }
    memcpy(&n2[0], npub, 12);
    *(uint32_t *) &n2[12] = 0x01000000;
    aesni_encrypt1(T, _mm_load_si128((const __m128i *) n2), rkeys);

    (*(uint64_t *) &fb[0]) = _bswap64((uint64_t)(8 * adlen));
    (*(uint64_t *) &fb[8]) = _bswap64((uint64_t)(8 * mlen));

    Hv = _mm_shuffle_epi8(_mm_load_si128((const __m128i *) H), rev);
    _mm_store_si128((__m128i *) H, Hv);
    H2v = mulv(Hv, Hv);
    H3v = mulv(H2v, Hv);
    H4v = mulv(H3v, Hv);

    accv = _mm_setzero_si128();
    for (i = 0; i < adlen_rnd64; i += 64) {
        __m128i X4 = _mm_loadu_si128((const __m128i *) (ad + i + 0));
        __m128i X3 = _mm_loadu_si128((const __m128i *) (ad + i + 16));
        __m128i X2 = _mm_loadu_si128((const __m128i *) (ad + i + 32));
        __m128i X1 = _mm_loadu_si128((const __m128i *) (ad + i + 48));
        accv = reduce4(Hv, H2v, H3v, H4v, X1, X2, X3, X4, accv);
    }
    _mm_storeu_si128((__m128i *) accum, accv);

    for (i = adlen_rnd64; i < adlen; i += 16) {
        unsigned int blocklen = 16;
        if (i + (unsigned long long) blocklen > adlen) {
            blocklen = (unsigned int) (adlen - i);
        }
        addmul(accum, ad + i, blocklen, H);
    }

    mlen_rnd128 = mlen & ~127ULL;

#define LOOPACCUMDRND128                                                                          \
    {                                                                                             \
        const int iter = 8;                                                                       \
        const int lb = iter * 16;                                                                 \
        for (i = 0; i < mlen_rnd128; i += lb) {                                                   \
            aesni_addmul8full(c + i, accum, Hv, H2v, H3v, H4v);                                   \
        }                                                                                         \
    }

#define LOOPDRND128                                                                               \
    {                                                                                             \
        const int iter = 8;                                                                       \
        const int lb = iter * 16;                                                                 \
        for (i = 0; i < mlen_rnd128; i += lb) {                                                   \
            aesni_decrypt8full(m + i, (uint32_t *) n2, rkeys, c + i);                             \
        }                                                                                         \
    }

#define LOOPACCUMDRMD128                                     \
    {                                                        \
        const int iter = 8;                                  \
        const int lb = iter * 16;                            \
                                                             \
        for (i = mlen_rnd128; i < mlen; i += lb) {           \
            unsigned long long mj = lb;                      \
                                                             \
            if ((i + mj) >= mlen) {                          \
                mj = mlen - i;                               \
            }                                                \
            for (j = 0; j < mj; j += 16) {                   \
                unsigned int bl = 16;                        \
                                                             \
                if (j + (unsigned long long) bl >= mj) {     \
                    bl = (unsigned int) (mj - j);            \
                }                                            \
                addmul(accum, c + i + j, bl, H);             \
            }                                                \
        }                                                    \
    }

#define LOOPDRMD128                                          \
    {                                                        \
        const int iter = 8;                                  \
        const int lb = iter * 16;                            \
                                                             \
        for (i = mlen_rnd128; i < mlen; i += lb) {           \
            CRYPTO_ALIGN(16) unsigned char outni[8 * 16];    \
            unsigned long long mj = lb;                      \
                                                             \
            if ((i + mj) >= mlen) {                          \
                mj = mlen - i;                               \
            }                                                \
            aesni_encrypt8(outni, (uint32_t *) n2, rkeys);   \
            for (j = 0; j < mj; j++) {                       \
                m[i + j] = c[i + j] ^ outni[j];              \
            }                                                \
        }                                                    \
    }
    n2[15] = 0;

    COUNTER_INC2(n2);
    LOOPACCUMDRND128;
    LOOPACCUMDRMD128;
    addmul(accum, fb, 16, H);
    {
        unsigned char d = 0;

        for (i = 0; i < 16; i++) {
            d |= (c[i + mlen] ^ (T[i] ^ accum[15 - i]));
        }
        if (d != 0) {
            return -1;
        }
    }
    *(uint32_t *) &n2[12] = 0;
    COUNTER_INC2(n2);
    LOOPDRND128;
    LOOPDRMD128;

    if (mlen_p != NULL) {
        *mlen_p = mlen;
    }
    return 0;
}

int
crypto_aead_aes256gcm_aesni_encrypt(unsigned char *c,
                                    unsigned long long *clen_p,
                                    const unsigned char *m,
                                    unsigned long long mlen,
                                    const unsigned char *ad,
                                    unsigned long long adlen,
                                    const unsigned char *nsec,
                                    const unsigned char *npub,
                                    const unsigned char *k)
{
    crypto_aead_aes256gcm_aesni_state ctx;

    crypto_aead_aes256gcm_aesni_beforenm(&ctx, k);

    return crypto_aead_aes256gcm_aesni_encrypt_afternm
        (c, clen_p, m, mlen, ad, adlen, nsec, npub, &ctx);
}

int
crypto_aead_aes256gcm_aesni_decrypt(unsigned char *m,
                                    unsigned long long *mlen_p,
                                    unsigned char *nsec,
                                    const unsigned char *c,
                                    unsigned long long clen,
                                    const unsigned char *ad,
                                    unsigned long long adlen,
                                    const unsigned char *npub,
                                    const unsigned char *k)
{
    crypto_aead_aes256gcm_aesni_state ctx;

    crypto_aead_aes256gcm_aesni_beforenm((crypto_aead_aes256gcm_aesni_state *)
                                         &ctx, k);

    return crypto_aead_aes256gcm_aesni_decrypt_afternm
        (m, mlen_p, nsec, c, clen, ad, adlen, npub, &ctx);
}

size_t
crypto_aead_aes256gcm_aesni_keybytes(void)
{
    return crypto_aead_aes256gcm_KEYBYTES;
}

size_t
crypto_aead_aes256gcm_aesni_nsecbytes(void)
{
    return crypto_aead_aes256gcm_NSECBYTES;
}

size_t crypto_aead_aes256gcm_aesni_npubbytes(void)
{
    return crypto_aead_aes256gcm_NPUBBYTES;
}

size_t crypto_aead_aes256gcm_aesni_abytes(void)
{
    return crypto_aead_aes256gcm_ABYTES;
}

size_t crypto_aead_aes256gcm_aesni_statebytes(void)
{
    return sizeof(crypto_aead_aes256gcm_aesni_state);
}

#endif
