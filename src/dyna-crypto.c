/*
 * dyna:crypto -- cryptographic hashes, HMAC and CRC. Self-contained, in-repo
 * (no external deps): every primitive is implemented from its published
 * specification (FIPS 180-4 for SHA-1/224/256/384/512, RFC 1321 for MD5,
 * RFC 2104 for HMAC, IEEE 802.3 for CRC-32), verified against the standard
 * test vectors in tests/test_crypto.js.
 *
 *   import { sha256, sha256Hex, hmacHex, crc32, Hasher } from "dyna:crypto";
 *
 *   sha256Hex("abc");                       // "ba7816bf..."
 *   sha256(bytes);                          // Uint8Array (32 bytes)
 *   hmacHex("sha256", key, "Hi There");     // RFC 4231 vector
 *   crc32("123456789");                     // 0xCBF43926
 *
 *   const h = new Hasher("sha256");         // streaming
 *   h.update("ab"); h.update(moreBytes);
 *   const d = h.digest();                   // Uint8Array; h stays usable
 *   h.close();                              // or `using h = new Hasher(...)`
 *
 * Input model: every data/key argument is a string (hashed as its UTF-8 bytes),
 * an ArrayBuffer, or any TypedArray/DataView (its raw backing bytes). A string
 * and the equivalent Uint8Array hash identically. Digests are returned either
 * as a fresh Uint8Array (`sha256`) or a lowercase hex string (`sha256Hex`).
 *
 * Memory / reentrancy discipline (CLAUDE.md): the one-shot functions are plain
 * functions with no closable resource; each materialises its argument to owned
 * C memory (string) or a live backing pointer (buffer) and then runs pure C.
 * `hmac` copies the key into its block-sized pad BEFORE coercing the (large)
 * data argument, so a later user-JS coercion can never dangle the key pointer.
 * The streaming Hasher is a dyna-nat resource (malloc-backed, freed by
 * close()/[Symbol.dispose]/finalizer): update() coerces its data argument to
 * C locals FIRST and only THEN resolves the native handle, so a valueOf/
 * toString that closes `this` mid-coercion yields a clean "closed" TypeError,
 * never a use-after-free. Native digest bytes are copied into fresh JS values
 * at the boundary; no native pointer escapes into the JS heap.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ==================================================================== *
 *  fixed-width byte load/store (host-endianness independent)            *
 * ==================================================================== */

static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static inline uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t load_be64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}
static inline void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void store_be64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}

/* Working state of any supported hash: MD5/SHA-1/SHA-224/SHA-256 use w32[],
 * SHA-384/SHA-512 use w64[]. The generic driver never touches it directly --
 * each algorithm's init/compress/extract cast this to their word type. */
typedef union {
    uint32_t w32[8];
    uint64_t w64[8];
} dyn_hash_state;

/* ==================================================================== *
 *  MD5 (RFC 1321)                                                       *
 * ==================================================================== */

static const uint32_t md5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_init(dyn_hash_state *s)
{
    s->w32[0] = 0x67452301; s->w32[1] = 0xefcdab89;
    s->w32[2] = 0x98badcfe; s->w32[3] = 0x10325476;
}

static void md5_compress(dyn_hash_state *s, const uint8_t *block)
{
    uint32_t m[16], a = s->w32[0], b = s->w32[1], c = s->w32[2], d = s->w32[3];
    int i;
    for (i = 0; i < 16; i++)
        m[i] = load_le32(block + i * 4);
    for (i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d);  g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;           g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);        g = (7 * i) & 15; }
        f += a + md5_K[i] + m[g];
        a = d; d = c; c = b;
        b += rotl32(f, md5_s[i]);
    }
    s->w32[0] += a; s->w32[1] += b; s->w32[2] += c; s->w32[3] += d;
}

static void md5_extract(const dyn_hash_state *s, uint8_t *out)
{
    store_le32(out,      s->w32[0]);
    store_le32(out + 4,  s->w32[1]);
    store_le32(out + 8,  s->w32[2]);
    store_le32(out + 12, s->w32[3]);
}

/* ==================================================================== *
 *  SHA-1 (FIPS 180-4 sec 6.1)                                           *
 * ==================================================================== */

static void sha1_init(dyn_hash_state *s)
{
    s->w32[0] = 0x67452301; s->w32[1] = 0xEFCDAB89; s->w32[2] = 0x98BADCFE;
    s->w32[3] = 0x10325476; s->w32[4] = 0xC3D2E1F0;
}

static void sha1_compress(dyn_hash_state *s, const uint8_t *block)
{
    uint32_t w[80];
    uint32_t a = s->w32[0], b = s->w32[1], c = s->w32[2], d = s->w32[3], e = s->w32[4];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = load_be32(block + i * 4);
    for (i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        t = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }
    s->w32[0] += a; s->w32[1] += b; s->w32[2] += c; s->w32[3] += d; s->w32[4] += e;
}

static void sha1_extract(const dyn_hash_state *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 5; i++)
        store_be32(out + i * 4, s->w32[i]);
}

/* ==================================================================== *
 *  SHA-224 / SHA-256 (FIPS 180-4 sec 6.2)                               *
 * ==================================================================== */

static const uint32_t sha256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_init(dyn_hash_state *s)
{
    s->w32[0] = 0x6a09e667; s->w32[1] = 0xbb67ae85; s->w32[2] = 0x3c6ef372; s->w32[3] = 0xa54ff53a;
    s->w32[4] = 0x510e527f; s->w32[5] = 0x9b05688c; s->w32[6] = 0x1f83d9ab; s->w32[7] = 0x5be0cd19;
}

static void sha224_init(dyn_hash_state *s)
{
    s->w32[0] = 0xc1059ed8; s->w32[1] = 0x367cd507; s->w32[2] = 0x3070dd17; s->w32[3] = 0xf70e5939;
    s->w32[4] = 0xffc00b31; s->w32[5] = 0x68581511; s->w32[6] = 0x64f98fa7; s->w32[7] = 0xbefa4fa4;
}

static void sha256_compress(dyn_hash_state *s, const uint8_t *block)
{
    uint32_t w[64];
    uint32_t a = s->w32[0], b = s->w32[1], c = s->w32[2], d = s->w32[3];
    uint32_t e = s->w32[4], f = s->w32[5], g = s->w32[6], h = s->w32[7];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = load_be32(block + i * 4);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->w32[0] += a; s->w32[1] += b; s->w32[2] += c; s->w32[3] += d;
    s->w32[4] += e; s->w32[5] += f; s->w32[6] += g; s->w32[7] += h;
}

static void sha256_extract(const dyn_hash_state *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 8; i++)
        store_be32(out + i * 4, s->w32[i]);
}

static void sha224_extract(const dyn_hash_state *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 7; i++) /* SHA-224 = SHA-256 truncated to 224 bits */
        store_be32(out + i * 4, s->w32[i]);
}

/* ==================================================================== *
 *  SHA-384 / SHA-512 (FIPS 180-4 sec 6.4)                               *
 * ==================================================================== */

static const uint64_t sha512_K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static void sha512_init(dyn_hash_state *s)
{
    s->w64[0] = 0x6a09e667f3bcc908ULL; s->w64[1] = 0xbb67ae8584caa73bULL;
    s->w64[2] = 0x3c6ef372fe94f82bULL; s->w64[3] = 0xa54ff53a5f1d36f1ULL;
    s->w64[4] = 0x510e527fade682d1ULL; s->w64[5] = 0x9b05688c2b3e6c1fULL;
    s->w64[6] = 0x1f83d9abfb41bd6bULL; s->w64[7] = 0x5be0cd19137e2179ULL;
}

static void sha384_init(dyn_hash_state *s)
{
    s->w64[0] = 0xcbbb9d5dc1059ed8ULL; s->w64[1] = 0x629a292a367cd507ULL;
    s->w64[2] = 0x9159015a3070dd17ULL; s->w64[3] = 0x152fecd8f70e5939ULL;
    s->w64[4] = 0x67332667ffc00b31ULL; s->w64[5] = 0x8eb44a8768581511ULL;
    s->w64[6] = 0xdb0c2e0d64f98fa7ULL; s->w64[7] = 0x47b5481dbefa4fa4ULL;
}

static void sha512_compress(dyn_hash_state *s, const uint8_t *block)
{
    uint64_t w[80];
    uint64_t a = s->w64[0], b = s->w64[1], c = s->w64[2], d = s->w64[3];
    uint64_t e = s->w64[4], f = s->w64[5], g = s->w64[6], h = s->w64[7];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = load_be64(block + i * 8);
    for (i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 80; i++) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + sha512_K[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->w64[0] += a; s->w64[1] += b; s->w64[2] += c; s->w64[3] += d;
    s->w64[4] += e; s->w64[5] += f; s->w64[6] += g; s->w64[7] += h;
}

static void sha512_extract(const dyn_hash_state *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 8; i++)
        store_be64(out + i * 8, s->w64[i]);
}

static void sha384_extract(const dyn_hash_state *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 6; i++) /* SHA-384 = SHA-512 truncated to 384 bits */
        store_be64(out + i * 8, s->w64[i]);
}

/* ==================================================================== *
 *  generic hash driver (buffered absorb + Merkle-Damgard padding)       *
 * ==================================================================== */

#define DYN_MAX_BLOCK  128u
#define DYN_MAX_DIGEST 64u

typedef struct {
    const char *name;
    unsigned block_size;   /* 64 (md5/sha1/sha224/sha256) or 128 (sha384/sha512) */
    unsigned digest_size;  /* output bytes */
    unsigned len_bytes;    /* trailing length field: 8 or 16 bytes */
    int big_endian_len;    /* 1 for SHA (big-endian length), 0 for MD5 (little) */
    void (*init)(dyn_hash_state *s);
    void (*compress)(dyn_hash_state *s, const uint8_t *block);
    void (*extract)(const dyn_hash_state *s, uint8_t *out);
} dyn_hash_algo;

enum { DYN_MD5, DYN_SHA1, DYN_SHA224, DYN_SHA256, DYN_SHA384, DYN_SHA512, DYN_ALGO_COUNT };

static const dyn_hash_algo dyn_algos[DYN_ALGO_COUNT] = {
    [DYN_MD5]    = { "md5",    64, 16,  8, 0, md5_init,    md5_compress,    md5_extract },
    [DYN_SHA1]   = { "sha1",   64, 20,  8, 1, sha1_init,   sha1_compress,   sha1_extract },
    [DYN_SHA224] = { "sha224", 64, 28,  8, 1, sha224_init, sha256_compress, sha224_extract },
    [DYN_SHA256] = { "sha256", 64, 32,  8, 1, sha256_init, sha256_compress, sha256_extract },
    [DYN_SHA384] = { "sha384", 128, 48, 16, 1, sha384_init, sha512_compress, sha384_extract },
    [DYN_SHA512] = { "sha512", 128, 64, 16, 1, sha512_init, sha512_compress, sha512_extract },
};

/* Streaming state: algorithm descriptor + partial-block buffer + total length.
 * A shallow struct copy is a valid snapshot (algo points to static data), which
 * is how digest() reads the hash without destroying the resumable state. */
typedef struct {
    const dyn_hash_algo *algo;
    dyn_hash_state st;
    uint8_t buffer[DYN_MAX_BLOCK];
    unsigned buflen;
    uint64_t bytelen; /* total message bytes absorbed so far */
} dyn_hash_ctx;

static void dyn_hash_init(dyn_hash_ctx *c, const dyn_hash_algo *a)
{
    c->algo = a;
    c->buflen = 0;
    c->bytelen = 0;
    a->init(&c->st);
}

static void dyn_hash_update(dyn_hash_ctx *c, const uint8_t *data, size_t len)
{
    unsigned block = c->algo->block_size;
    c->bytelen += len;
    if (c->buflen) { /* top up a partial block first */
        unsigned need = block - c->buflen;
        unsigned take = len < need ? (unsigned)len : need;
        memcpy(c->buffer + c->buflen, data, take);
        c->buflen += take;
        data += take;
        len -= take;
        if (c->buflen == block) {
            c->algo->compress(&c->st, c->buffer);
            c->buflen = 0;
        }
    }
    while (len >= block) {
        c->algo->compress(&c->st, data);
        data += block;
        len -= block;
    }
    if (len) {
        memcpy(c->buffer, data, len);
        c->buflen = (unsigned)len;
    }
}

/* Merkle-Damgard finalization: append 0x80, zero-pad, append the message bit
 * length in the algorithm's width/endianness, compress the final block(s), and
 * extract the digest. Operates on `c` in place (callers that must stay usable
 * pass a copy). */
static void dyn_hash_final(dyn_hash_ctx *c, uint8_t *out)
{
    const dyn_hash_algo *a = c->algo;
    unsigned block = a->block_size, lenb = a->len_bytes;
    uint64_t bits = c->bytelen << 3;
    uint64_t bits_hi = c->bytelen >> 61; /* high 64 bits of the 128-bit bit length */
    uint8_t *buf = c->buffer;
    unsigned n = c->buflen;
    int i;

    buf[n++] = 0x80;
    if (n > block - lenb) { /* not enough room for the length field: flush */
        while (n < block)
            buf[n++] = 0;
        a->compress(&c->st, buf);
        n = 0;
    }
    while (n < block - lenb)
        buf[n++] = 0;
    if (a->big_endian_len) {
        if (lenb == 16) {
            store_be64(buf + n, bits_hi);
            store_be64(buf + n + 8, bits);
        } else {
            store_be64(buf + n, bits);
        }
    } else { /* MD5: 64-bit little-endian length */
        for (i = 0; i < 8; i++)
            buf[n + i] = (uint8_t)(bits >> (8 * i));
    }
    n += lenb;
    a->compress(&c->st, buf);
    a->extract(&c->st, out);
}

static const dyn_hash_algo *dyn_find_algo(const char *name)
{
    int i;
    for (i = 0; i < DYN_ALGO_COUNT; i++)
        if (strcmp(name, dyn_algos[i].name) == 0)
            return &dyn_algos[i];
    return NULL;
}

/* ==================================================================== *
 *  HMAC (RFC 2104)                                                      *
 * ==================================================================== */

/* Derive the block-sized key K0 (zero-padded, or H(key) when key exceeds the
 * block size). `k0` must be DYN_MAX_BLOCK bytes and pre-zeroed by the caller. */
static void dyn_hmac_key0(const dyn_hash_algo *a, const uint8_t *key,
                          size_t keylen, uint8_t *k0)
{
    if (keylen > a->block_size) {
        dyn_hash_ctx c;
        dyn_hash_init(&c, a);
        dyn_hash_update(&c, key, keylen);
        dyn_hash_final(&c, k0); /* writes digest_size bytes; the rest stays 0 */
    } else {
        memcpy(k0, key, keylen);
    }
}

/* HMAC = H((K0 ^ opad) || H((K0 ^ ipad) || msg)), K0 already derived. */
static void dyn_hmac_finish(const dyn_hash_algo *a, const uint8_t *k0,
                            const uint8_t *msg, size_t msglen, uint8_t *out)
{
    uint8_t ipad[DYN_MAX_BLOCK], opad[DYN_MAX_BLOCK], inner[DYN_MAX_DIGEST];
    dyn_hash_ctx c;
    unsigned bs = a->block_size, i;

    for (i = 0; i < bs; i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }
    dyn_hash_init(&c, a);
    dyn_hash_update(&c, ipad, bs);
    dyn_hash_update(&c, msg, msglen);
    dyn_hash_final(&c, inner);

    dyn_hash_init(&c, a);
    dyn_hash_update(&c, opad, bs);
    dyn_hash_update(&c, inner, a->digest_size);
    dyn_hash_final(&c, out);
}

/* ==================================================================== *
 *  CRC-32 (reflected, bitwise -- no lookup tables / no static state)    *
 * ==================================================================== */

#define DYN_CRC32_POLY  0xEDB88320u /* IEEE 802.3 (reflected) */
#define DYN_CRC32C_POLY 0x82F63B78u /* Castagnoli (reflected) */

static uint32_t dyn_crc32_calc(const uint8_t *data, size_t len, uint32_t poly)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (poly & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ==================================================================== *
 *  JS boundary: argument coercion + result builders                     *
 * ==================================================================== */

/* Coerce a data/key argument to a byte range. A string is materialised to an
 * OWNED UTF-8 buffer (*powned, release with JS_FreeCString); an ArrayBuffer or
 * TypedArray/DataView yields a zero-copy backing pointer (valid only for the
 * synchronous remainder of the call); anything else is coerced to a string via
 * ToString (which may run user JS -- callers relying on reentrancy safety must
 * therefore call this BEFORE resolving any native handle or buffer pointer that
 * must survive). Returns 0 (exactly one of *powned/borrowed pointer is set) or
 * -1 with a pending exception. */
static int dyn_crypto_data(JSContext *ctx, JSValueConst v, const uint8_t **pdata,
                           size_t *plen, const char **powned)
{
    *powned = NULL;
    if (JS_IsString(v)) {
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t *)s;
        *plen = n;
        return 0;
    }
    {
        size_t n;
        uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
        if (p) {
            *pdata = p;
            *plen = n;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx)); /* not an ArrayBuffer: retry */
    }
    {
        size_t off, len, bpe, ab_size;
        uint8_t *base;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(ab)) {
            base = JS_GetArrayBuffer(ctx, &ab_size, ab);
            JS_FreeValue(ctx, ab);
            if (!base)
                return -1; /* detached mid-resolve; already threw */
            if (off > ab_size || len > ab_size - off) {
                JS_ThrowRangeError(ctx, "typed array out of bounds");
                return -1;
            }
            *pdata = base + off;
            *plen = len;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx)); /* not a view: fall through */
    }
    {   /* generic: ToString (runs user JS) -> owned UTF-8 bytes */
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t *)s;
        *plen = n;
        return 0;
    }
}

static const char dyn_hex_digits[] = "0123456789abcdef";

/* Fresh lowercase hex string of `data[0..len)`. */
static JSValue dyn_crypto_hex(JSContext *ctx, const uint8_t *data, size_t len)
{
    char stackbuf[DYN_MAX_DIGEST * 2];
    size_t i;
    for (i = 0; i < len; i++) {
        stackbuf[i * 2]     = dyn_hex_digits[data[i] >> 4];
        stackbuf[i * 2 + 1] = dyn_hex_digits[data[i] & 0xF];
    }
    return JS_NewStringLen(ctx, stackbuf, len * 2);
}

/* Fresh Uint8Array copying `data[0..len)` (never aliases native memory). */
static JSValue dyn_crypto_u8array(JSContext *ctx, const uint8_t *data, size_t len)
{
    static const uint8_t zero_stub = 0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (len == 0)
        data = &zero_stub;
    ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

/* ==================================================================== *
 *  one-shot hash functions (magic = algorithm index; hex vs binary)     *
 * ==================================================================== */

static JSValue dyn_crypto_hash(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int magic)
{
    const dyn_hash_algo *a = &dyn_algos[magic];
    const uint8_t *data;
    size_t len;
    const char *owned;
    dyn_hash_ctx c;
    uint8_t digest[DYN_MAX_DIGEST];
    (void)this_val; (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    dyn_hash_init(&c, a);
    dyn_hash_update(&c, data, len);
    dyn_hash_final(&c, digest);
    if (owned)
        JS_FreeCString(ctx, owned);
    return dyn_crypto_u8array(ctx, digest, a->digest_size);
}

static JSValue dyn_crypto_hash_hex(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv, int magic)
{
    const dyn_hash_algo *a = &dyn_algos[magic];
    const uint8_t *data;
    size_t len;
    const char *owned;
    dyn_hash_ctx c;
    uint8_t digest[DYN_MAX_DIGEST];
    (void)this_val; (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    dyn_hash_init(&c, a);
    dyn_hash_update(&c, data, len);
    dyn_hash_final(&c, digest);
    if (owned)
        JS_FreeCString(ctx, owned);
    return dyn_crypto_hex(ctx, digest, a->digest_size);
}

/* ==================================================================== *
 *  HMAC (magic = 1 -> hex, 0 -> Uint8Array)                             *
 * ==================================================================== */

static JSValue dyn_crypto_hmac(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int magic)
{
    const char *aname;
    const dyn_hash_algo *a;
    const uint8_t *kp, *dp;
    size_t kl, dl;
    const char *kown, *down;
    uint8_t k0[DYN_MAX_BLOCK], out[DYN_MAX_DIGEST];
    (void)this_val; (void)argc;

    /* 1. algorithm name (before any buffer pointer is held) */
    aname = JS_ToCString(ctx, argv[0]);
    if (!aname)
        return JS_EXCEPTION;
    a = dyn_find_algo(aname);
    JS_FreeCString(ctx, aname);
    if (!a)
        return JS_ThrowTypeError(ctx, "hmac: unknown algorithm");

    /* 2. key -> block-sized K0 immediately (owned/stable), then release the key.
     * Consuming the key before coercing `data` means a later user-JS coercion
     * can never dangle a borrowed key pointer. */
    if (dyn_crypto_data(ctx, argv[1], &kp, &kl, &kown))
        return JS_EXCEPTION;
    memset(k0, 0, sizeof(k0));
    dyn_hmac_key0(a, kp, kl, k0);
    if (kown)
        JS_FreeCString(ctx, kown);

    /* 3. data (may run user JS; only its own borrowed pointer is live now) */
    if (dyn_crypto_data(ctx, argv[2], &dp, &dl, &down))
        return JS_EXCEPTION;
    dyn_hmac_finish(a, k0, dp, dl, out); /* pure C; no JS between resolve and use */
    if (down)
        JS_FreeCString(ctx, down);

    return magic ? dyn_crypto_hex(ctx, out, a->digest_size)
                 : dyn_crypto_u8array(ctx, out, a->digest_size);
}

/* ==================================================================== *
 *  CRC-32 / CRC-32C (magic selects the polynomial)                      *
 * ==================================================================== */

static JSValue dyn_crypto_crc32(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, int magic)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    uint32_t crc;
    (void)this_val; (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    crc = dyn_crc32_calc(data, len, magic ? DYN_CRC32C_POLY : DYN_CRC32_POLY);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewUint32(ctx, crc);
}

/* ==================================================================== *
 *  streaming Hasher (dyna-nat resource)                               *
 * ==================================================================== */

static JSClassID dyn_hasher_class_id;

static void dyn_hasher_dispose(void *native)
{
    free(native);
}

static const JSClassDef dyn_hasher_class = {
    "Hasher",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_hasher_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                               JSValueConst *argv)
{
    const char *name;
    const dyn_hash_algo *a;
    dyn_hash_ctx *h;
    (void)new_target;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "Hasher(algorithm) requires an algorithm name");
    name = JS_ToCString(ctx, argv[0]); /* coerce first (nothing allocated yet) */
    if (!name)
        return JS_EXCEPTION;
    a = dyn_find_algo(name);
    JS_FreeCString(ctx, name);
    if (!a)
        return JS_ThrowTypeError(ctx,
            "Hasher: unknown algorithm (md5|sha1|sha224|sha256|sha384|sha512)");

    h = (dyn_hash_ctx *)malloc(sizeof(*h));
    if (!h)
        return JS_ThrowOutOfMemory(ctx);
    dyn_hash_init(h, a);
    return dyn_res_wrap(ctx, dyn_hasher_class_id, h, dyn_hasher_dispose);
}

/* update(data): absorb bytes; returns `this` for chaining. Coerces the data
 * argument to C locals BEFORE resolving the native handle (CLAUDE.md), so a
 * valueOf/toString that closes `this` mid-coercion yields a clean "closed"
 * error, never a use-after-free. */
static JSValue dyn_hasher_update(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    dyn_hash_ctx *h;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned)) /* may run user JS */
        return JS_EXCEPTION;
    h = (dyn_hash_ctx *)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    dyn_hash_update(h, data, len); /* pure C; no JS between resolve and use */
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_DupValue(ctx, this_val);
}

/* digest()/digestHex(): finalize a COPY so the streaming state stays usable
 * (further update()s and repeated digests are well-defined). No JS args, so
 * resolving first is safe. */
static JSValue dyn_hasher_digest(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    dyn_hash_ctx *h, copy;
    uint8_t digest[DYN_MAX_DIGEST];
    (void)argc; (void)argv;

    h = (dyn_hash_ctx *)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    copy = *h;
    dyn_hash_final(&copy, digest);
    return dyn_crypto_u8array(ctx, digest, h->algo->digest_size);
}

static JSValue dyn_hasher_digest_hex(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv)
{
    dyn_hash_ctx *h, copy;
    uint8_t digest[DYN_MAX_DIGEST];
    (void)argc; (void)argv;

    h = (dyn_hash_ctx *)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    copy = *h;
    dyn_hash_final(&copy, digest);
    return dyn_crypto_hex(ctx, digest, h->algo->digest_size);
}

/* reset(): return the hasher to its initial state (reuse without reallocating). */
static JSValue dyn_hasher_reset(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    dyn_hash_ctx *h;
    (void)argc; (void)argv;

    h = (dyn_hash_ctx *)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    dyn_hash_init(h, h->algo);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_hasher_get_algorithm(JSContext *ctx, JSValueConst this_val)
{
    dyn_hash_ctx *h = (dyn_hash_ctx *)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewString(ctx, h->algo->name);
}

static JSValue dyn_hasher_get_digest_size(JSContext *ctx, JSValueConst this_val)
{
    dyn_hash_ctx *h = (dyn_hash_ctx *)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int32_t)h->algo->digest_size);
}

static const JSCFunctionListEntry dyn_hasher_proto[] = {
    JS_CFUNC_DEF("update", 1, dyn_hasher_update),
    JS_CFUNC_DEF("digest", 0, dyn_hasher_digest),
    JS_CFUNC_DEF("digestHex", 0, dyn_hasher_digest_hex),
    JS_CFUNC_DEF("reset", 0, dyn_hasher_reset),
    JS_CGETSET_DEF("algorithm", dyn_hasher_get_algorithm, NULL),
    JS_CGETSET_DEF("digestSize", dyn_hasher_get_digest_size, NULL),
};

/* ==================================================================== *
 *  module registration                                                  *
 * ==================================================================== */

static const JSCFunctionListEntry dyn_crypto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("md5",    1, dyn_crypto_hash,     DYN_MD5),
    JS_CFUNC_MAGIC_DEF("md5Hex", 1, dyn_crypto_hash_hex, DYN_MD5),
    JS_CFUNC_MAGIC_DEF("sha1",    1, dyn_crypto_hash,     DYN_SHA1),
    JS_CFUNC_MAGIC_DEF("sha1Hex", 1, dyn_crypto_hash_hex, DYN_SHA1),
    JS_CFUNC_MAGIC_DEF("sha224",    1, dyn_crypto_hash,     DYN_SHA224),
    JS_CFUNC_MAGIC_DEF("sha224Hex", 1, dyn_crypto_hash_hex, DYN_SHA224),
    JS_CFUNC_MAGIC_DEF("sha256",    1, dyn_crypto_hash,     DYN_SHA256),
    JS_CFUNC_MAGIC_DEF("sha256Hex", 1, dyn_crypto_hash_hex, DYN_SHA256),
    JS_CFUNC_MAGIC_DEF("sha384",    1, dyn_crypto_hash,     DYN_SHA384),
    JS_CFUNC_MAGIC_DEF("sha384Hex", 1, dyn_crypto_hash_hex, DYN_SHA384),
    JS_CFUNC_MAGIC_DEF("sha512",    1, dyn_crypto_hash,     DYN_SHA512),
    JS_CFUNC_MAGIC_DEF("sha512Hex", 1, dyn_crypto_hash_hex, DYN_SHA512),

    JS_CFUNC_MAGIC_DEF("hmac",    3, dyn_crypto_hmac, 0),
    JS_CFUNC_MAGIC_DEF("hmacHex", 3, dyn_crypto_hmac, 1),

    JS_CFUNC_MAGIC_DEF("crc32",  1, dyn_crypto_crc32, 0),
    JS_CFUNC_MAGIC_DEF("crc32c", 1, dyn_crypto_crc32, 1),
};

static int dyn_crypto_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_hasher_class_id, &dyn_hasher_class,
                           dyn_hasher_proto, countof(dyn_hasher_proto),
                           dyn_hasher_ctor, "Hasher") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_crypto_funcs,
                                  countof(dyn_crypto_funcs));
}

int js_nat_init_crypto(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:crypto", dyn_crypto_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Hasher");
    return JS_AddModuleExportList(ctx, m, dyn_crypto_funcs,
                                  countof(dyn_crypto_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_CRYPTO */
