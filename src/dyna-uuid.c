/*
 * dyna:uuid -- UUID generation/parsing per RFC 9562 (the 2024 spec that
 * supersedes RFC 4122). Self-contained, in-repo (no external deps).
 *
 *   import { v4, v7, v3, v5, parse, validate, version, variant,
 *            bytes, fromBytes, NIL, MAX } from "dyna:uuid";
 *
 *   v4();                 // -> "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" (random)
 *   v7();                 // -> time-ordered (48-bit ms prefix, version 7)
 *   v5(NAMESPACE_DNS, "www.example.com"); // -> SHA-1 name-based
 *   v3(NAMESPACE_DNS, "www.example.com"); // -> MD5 name-based
 *   parse("{2ED6657D-E927-568B-95E1-2665A8AEA6A2}"); // -> canonical lowercase
 *   validate("not-a-uuid");   // -> false
 *   version("...-7xxx-...");   // -> 7      (the version nibble)
 *   variant("...");            // -> "RFC4122" | "NCS" | "Microsoft" | "Future"
 *   bytes("00000000-...");     // -> Uint8Array(16)
 *   fromBytes(new Uint8Array(16)); // -> canonical string
 *
 * Bit layout (RFC 9562 sec.4): 128 bits / 16 bytes. The version nibble is the
 * high nibble of byte 6 -- b[6] = (b[6] & 0x0f) | (ver << 4); the variant bits
 * are the top of byte 8 -- b[8] = (b[8] & 0x3f) | 0x80 (variant "10xx", the
 * standard OSF DCE / RFC 9562 variant). v7 additionally places a 48-bit
 * big-endian Unix-millisecond timestamp (CLOCK_REALTIME) in bytes 0..5; the
 * remaining 74 bits (rand_a in b[6..7], rand_b in b[8..15], minus the version
 * and variant bits) are OS-random each call (RFC 9562 "Method 0" -- fully
 * random rand_a/rand_b, no added intra-millisecond counter, so there is no
 * cross-thread shared state to race on). Across a generation loop the embedded
 * timestamp is non-decreasing because CLOCK_REALTIME is.
 *
 * Canonical string is lowercase hex with hyphens at the 8-4-4-4-12 positions
 * (36 chars). PARSE POLICY (mirrors Go's google/uuid Parse -- the four forms it
 * accepts, but stricter on braces: matching '{' '}' are required, not merely
 * the leading brace): parse()/validate()/version()/variant()/bytes() accept
 *   - 36: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"                (canonical)
 *   - 45: "urn:uuid:" + canonical           (prefix compared case-insensitively)
 *   - 38: "{" + canonical + "}"                                (braced)
 *   - 32: "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"           (raw hex, no hyphens)
 * hex digits are accepted in either case; any other length, a misplaced/missing
 * hyphen, a non-hex digit, or an unmatched brace/prefix is rejected. parse()
 * always returns the canonical LOWERCASE form; validate() returns a bool
 * (a non-string argument is simply not a UUID -> false, never a throw).
 *
 * variant() names follow google/uuid's classification of byte 8:
 *   (b8 & 0xc0)==0x80 -> "RFC4122"  (the 10xx standard variant this file emits)
 *   (b8 & 0xe0)==0xc0 -> "Microsoft"
 *   (b8 & 0xe0)==0xe0 -> "Future"
 *   otherwise (0xxx)  -> "NCS"      (Apollo NCS backward-compat; incl. the NIL)
 *
 * v3/v5 accept a namespace as EITHER a UUID string (any of the four parse
 * forms) or a raw 16-byte view, and a name as EITHER a string (hashed as its
 * UTF-8 bytes) or a byte view; MD5/SHA-1 are the small self-contained
 * implementations below (verified against the RFC 9562 appendix / Python
 * `uuid` module vectors in tests/test_uuid.js).
 *
 * Reentrancy/ownership (CLAUDE.md sec.5): these are plain functions -- no
 * `this`, no closable native resource -- so no re-entrant valueOf can free a
 * handle mid-call; still, every argument is coerced to a C local FIRST and
 * every owned C string / malloc'd scratch buffer is freed on every path,
 * including every error path. Every returned Uint8Array/string is a fresh copy;
 * no native pointer escapes into a JS value.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_UUID)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Canonical hyphenated string is exactly 36 chars (no NUL stored/needed). */
#define DYN_UUID_STRLEN 36

/* ---------- OS entropy (same mechanism as src/dyna-random.c) ---------- */

#if defined(__APPLE__) || defined(__FreeBSD__)
static void dyn_uuid_entropy(void *buf, size_t n)
{
    arc4random_buf(buf, n);
}
#else
#include <sys/random.h> /* getrandom on Linux */
#include <errno.h>
static void dyn_uuid_entropy(void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            /* Last-resort fallback: never block a script on entropy. */
            while (n-- > 0)
                *p++ = (uint8_t)rand();
            return;
        }
        p += got;
        n -= (size_t)got;
    }
}
#endif

/* ---------- hex + canonical formatting ---------- */

static const char dyn_uuid_hex[] = "0123456789abcdef";

static int dyn_uuid_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Render 16 bytes as lowercase 8-4-4-4-12 into out[36] (no terminator). */
static void dyn_uuid_format(const uint8_t b[16], char out[DYN_UUID_STRLEN])
{
    int i, j = 0;
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[j++] = '-';
        out[j++] = dyn_uuid_hex[b[i] >> 4];
        out[j++] = dyn_uuid_hex[b[i] & 0x0f];
    }
}

/* ---------- parsing ---------- */

/* ASCII case-insensitive compare of the first n bytes; 0 iff equal. */
static int dyn_uuid_ci_ne(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 1;
    }
    return 0;
}

static int dyn_uuid_hexpair(const char *p, uint8_t *out)
{
    int hi = dyn_uuid_hexval((unsigned char)p[0]);
    int lo = dyn_uuid_hexval((unsigned char)p[1]);
    if (hi < 0 || lo < 0)
        return -1;
    *out = (uint8_t)((hi << 4) | lo);
    return 0;
}

/* Parse a 36-char canonical "8-4-4-4-12" span at `s` into b[16]. */
static int dyn_uuid_parse_canonical(const char *s, uint8_t b[16])
{
    static const int pos[16] = {
        0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34
    };
    int i;
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
        return -1;
    for (i = 0; i < 16; i++)
        if (dyn_uuid_hexpair(s + pos[i], &b[i]))
            return -1;
    return 0;
}

/* Parse any of the four accepted forms into b[16]. 0 on success, -1 if
 * malformed. `len` is the exact byte length (embedded NUL safe). */
static int dyn_uuid_parse_bytes(const char *s, size_t len, uint8_t b[16])
{
    int i;
    switch (len) {
    case 36:
        return dyn_uuid_parse_canonical(s, b);
    case 45: /* urn:uuid:xxxxxxxx-... */
        if (dyn_uuid_ci_ne(s, "urn:uuid:", 9))
            return -1;
        return dyn_uuid_parse_canonical(s + 9, b);
    case 38: /* {xxxxxxxx-...} */
        if (s[0] != '{' || s[37] != '}')
            return -1;
        return dyn_uuid_parse_canonical(s + 1, b);
    case 32: /* raw hex, no hyphens */
        for (i = 0; i < 16; i++)
            if (dyn_uuid_hexpair(s + i * 2, &b[i]))
                return -1;
        return 0;
    default:
        return -1;
    }
}

static const char *dyn_uuid_variant_name(uint8_t b8)
{
    if ((b8 & 0xc0) == 0x80) return "RFC4122";
    if ((b8 & 0xe0) == 0xc0) return "Microsoft";
    if ((b8 & 0xe0) == 0xe0) return "Future";
    return "NCS";
}

/* ---------- byte-view boundary (Uint8Array/Int8Array/Uint8ClampedArray or a
 * plain ArrayBuffer -> raw pointer + length), same shape as dyna:encoding's
 * dyn_enc_view. Pure structural query, never invokes JS. ---------- */
static int dyn_uuid_view(JSContext *ctx, JSValueConst v, uint8_t **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        *pp = base;
        *pn = ab;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx)); /* clear: not a plain ArrayBuffer */

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1; /* not a TypedArray either; propagate that TypeError */
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a Uint8Array or ArrayBuffer");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1; /* detached mid-resolve; JS_GetArrayBuffer already threw */
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

/* Build a fresh Uint8Array copying 16 bytes (never aliases a native pointer). */
static JSValue dyn_uuid_new_u8array(JSContext *ctx, const uint8_t *data, size_t len)
{
    JSValue ab, out;
    JSValueConst ta_args[3];

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

/* ---------- MD5 (RFC 1321) -- self-contained, standard byte-order digest ---- */

static inline uint32_t dyn_rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void dyn_md5_block(uint32_t *h, const uint8_t *block)
{
    static const uint32_t K[64] = {
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
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static const uint8_t S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    uint32_t M[16], A = h[0], B = h[1], C = h[2], D = h[3];
    int i;

    for (i = 0; i < 16; i++)
        M[i] = (uint32_t)block[4 * i] | ((uint32_t)block[4 * i + 1] << 8) |
               ((uint32_t)block[4 * i + 2] << 16) | ((uint32_t)block[4 * i + 3] << 24);
    for (i = 0; i < 64; i++) {
        uint32_t F;
        int g;
        if (i < 16)      { F = (B & C) | (~B & D);       g = i; }
        else if (i < 32) { F = (D & B) | (~D & C);       g = (5 * i + 1) & 15; }
        else if (i < 48) { F = B ^ C ^ D;                g = (3 * i + 5) & 15; }
        else             { F = C ^ (B | ~D);             g = (7 * i) & 15; }
        F = F + A + K[i] + M[g];
        A = D; D = C; C = B;
        B = B + dyn_rotl32(F, S[i]);
    }
    h[0] += A; h[1] += B; h[2] += C; h[3] += D;
}

static void dyn_sha1_block(uint32_t *h, const uint8_t *block)
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
               ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    for (i = 16; i < 80; i++)
        w[i] = dyn_rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20)      { f = (b & c) | (~b & d);              k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                      k = 0xCA62C1D6; }
        t = dyn_rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = dyn_rotl32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

typedef void (*dyn_hash_block_fn)(uint32_t *h, const uint8_t *block);

/* Message-schedule + padding driver shared by MD5 (little-endian length) and
 * SHA-1 (big-endian length): full 64-byte blocks then a padded tail. */
static void dyn_hash_run(const uint8_t *msg, size_t len, uint32_t *h,
                         dyn_hash_block_fn block, int big_endian_len)
{
    uint64_t bitlen = (uint64_t)len * 8;
    uint8_t buf[64];
    size_t i = 0, p;
    int j;

    while (len - i >= 64) {
        block(h, msg + i);
        i += 64;
    }
    p = len - i;
    memcpy(buf, msg + i, p);
    buf[p++] = 0x80;
    if (p > 56) {
        memset(buf + p, 0, 64 - p);
        block(h, buf);
        memset(buf, 0, 56);
    } else {
        memset(buf + p, 0, 56 - p);
    }
    for (j = 0; j < 8; j++)
        buf[56 + j] = big_endian_len ? (uint8_t)(bitlen >> (8 * (7 - j)))
                                     : (uint8_t)(bitlen >> (8 * j));
    block(h, buf);
}

static void dyn_md5(const uint8_t *msg, size_t len, uint8_t out[16])
{
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    int j;
    dyn_hash_run(msg, len, h, dyn_md5_block, 0);
    for (j = 0; j < 4; j++) {
        out[4 * j]     = (uint8_t)h[j];
        out[4 * j + 1] = (uint8_t)(h[j] >> 8);
        out[4 * j + 2] = (uint8_t)(h[j] >> 16);
        out[4 * j + 3] = (uint8_t)(h[j] >> 24);
    }
}

static void dyn_sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    int j;
    dyn_hash_run(msg, len, h, dyn_sha1_block, 1);
    for (j = 0; j < 5; j++) {
        out[4 * j]     = (uint8_t)(h[j] >> 24);
        out[4 * j + 1] = (uint8_t)(h[j] >> 16);
        out[4 * j + 2] = (uint8_t)(h[j] >> 8);
        out[4 * j + 3] = (uint8_t)h[j];
    }
}

/* ---------- generation: v4 / v7 ---------- */

static JSValue dyn_uuid_v4(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    uint8_t b[16];
    char s[DYN_UUID_STRLEN];
    (void)this_val; (void)argc; (void)argv;

    dyn_uuid_entropy(b, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40); /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10xx */
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

static JSValue dyn_uuid_v7(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    uint8_t b[16];
    char s[DYN_UUID_STRLEN];
    struct timespec ts;
    uint64_t ms;
    (void)this_val; (void)argc; (void)argv;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: clock_gettime failed");
    ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    dyn_uuid_entropy(b, sizeof(b)); /* rand_a (b6..7) + rand_b (b8..15) */
    b[0] = (uint8_t)(ms >> 40);
    b[1] = (uint8_t)(ms >> 32);
    b[2] = (uint8_t)(ms >> 24);
    b[3] = (uint8_t)(ms >> 16);
    b[4] = (uint8_t)(ms >> 8);
    b[5] = (uint8_t)ms;
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x70); /* version 7 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10xx */
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

/* ---------- generation: v3 (MD5) / v5 (SHA-1), name-based ---------- */

/* Resolve a namespace argument: a UUID string (any parse form) or a 16-byte
 * view -> ns[16]. Returns 0 or -1 (throwing). */
static int dyn_uuid_namespace(JSContext *ctx, JSValueConst v, uint8_t ns[16])
{
    if (JS_IsString(v)) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, v);
        int r;
        if (!s)
            return -1;
        r = dyn_uuid_parse_bytes(s, len, ns);
        JS_FreeCString(ctx, s);
        if (r) {
            JS_ThrowSyntaxError(ctx, "v3/v5: invalid namespace UUID");
            return -1;
        }
        return 0;
    }
    {
        uint8_t *p;
        size_t n;
        if (dyn_uuid_view(ctx, v, &p, &n))
            return -1;
        if (n != 16) {
            JS_ThrowTypeError(ctx, "v3/v5: namespace must be a UUID string or 16-byte view");
            return -1;
        }
        memcpy(ns, p, 16);
        return 0;
    }
}

/* magic: version (3 = MD5, 5 = SHA-1). */
static JSValue dyn_uuid_named(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    uint8_t ns[16], b[16], digest[20];
    const uint8_t *name = NULL;
    const char *owned = NULL;
    size_t namelen = 0;
    uint8_t *msg;
    char s[DYN_UUID_STRLEN];
    (void)this_val; (void)argc;

    /* Coerce both args to C locals FIRST. */
    if (dyn_uuid_namespace(ctx, argv[0], ns))
        return JS_EXCEPTION;
    if (JS_IsString(argv[1])) {
        owned = JS_ToCStringLen(ctx, &namelen, argv[1]);
        if (!owned)
            return JS_EXCEPTION;
        name = (const uint8_t *)owned;
    } else {
        uint8_t *p;
        size_t n;
        if (dyn_uuid_view(ctx, argv[1], &p, &n))
            return JS_EXCEPTION;
        name = p;
        namelen = n;
    }

    msg = (uint8_t *)malloc(16 + namelen);
    if (!msg) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(msg, ns, 16);
    if (namelen)
        memcpy(msg + 16, name, namelen);
    if (magic == 5) {
        dyn_sha1(msg, 16 + namelen, digest);
        memcpy(b, digest, 16);
    } else {
        dyn_md5(msg, 16 + namelen, b);
    }
    free(msg);
    if (owned)
        JS_FreeCString(ctx, owned);

    b[6] = (uint8_t)((b[6] & 0x0f) | (magic << 4)); /* version 3 or 5 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);         /* variant 10xx */
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

/* ---------- parse / validate / version / variant / bytes / fromBytes ------- */

static JSValue dyn_uuid_parse(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    char out[DYN_UUID_STRLEN];
    int r;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "parse: invalid UUID string");
    dyn_uuid_format(b, out);
    return JS_NewStringLen(ctx, out, DYN_UUID_STRLEN);
}

static JSValue dyn_uuid_validate(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    int ok;
    (void)this_val; (void)argc;

    /* A non-string is simply not a UUID -> false (never throws). */
    if (!JS_IsString(argv[0]))
        return JS_FALSE;
    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    ok = (dyn_uuid_parse_bytes(str, len, b) == 0);
    JS_FreeCString(ctx, str);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_uuid_version(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    int r;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "version: invalid UUID string");
    return JS_NewInt32(ctx, b[6] >> 4);
}

static JSValue dyn_uuid_variant(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    int r;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "variant: invalid UUID string");
    return JS_NewString(ctx, dyn_uuid_variant_name(b[8]));
}

static JSValue dyn_uuid_bytes(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    uint8_t b[16];
    int r;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "bytes: invalid UUID string");
    return dyn_uuid_new_u8array(ctx, b, 16);
}

static JSValue dyn_uuid_from_bytes(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    uint8_t *p;
    size_t n;
    char out[DYN_UUID_STRLEN];
    (void)this_val; (void)argc;

    if (dyn_uuid_view(ctx, argv[0], &p, &n))
        return JS_EXCEPTION;
    if (n != 16)
        return JS_ThrowRangeError(ctx, "fromBytes: expected exactly 16 bytes");
    dyn_uuid_format(p, out);
    return JS_NewStringLen(ctx, out, DYN_UUID_STRLEN);
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry dyn_uuid_funcs[] = {
    JS_CFUNC_DEF("v4", 0, dyn_uuid_v4),
    JS_CFUNC_DEF("v7", 0, dyn_uuid_v7),
    JS_CFUNC_MAGIC_DEF("v3", 2, dyn_uuid_named, 3),
    JS_CFUNC_MAGIC_DEF("v5", 2, dyn_uuid_named, 5),
    JS_CFUNC_DEF("parse", 1, dyn_uuid_parse),
    JS_CFUNC_DEF("validate", 1, dyn_uuid_validate),
    JS_CFUNC_DEF("version", 1, dyn_uuid_version),
    JS_CFUNC_DEF("variant", 1, dyn_uuid_variant),
    JS_CFUNC_DEF("bytes", 1, dyn_uuid_bytes),
    JS_CFUNC_DEF("fromBytes", 1, dyn_uuid_from_bytes),
    /* All-zero (NIL) and all-ones (MAX) UUIDs, canonical lowercase. */
    JS_PROP_STRING_DEF("NIL", "00000000-0000-0000-0000-000000000000", 0),
    JS_PROP_STRING_DEF("MAX", "ffffffff-ffff-ffff-ffff-ffffffffffff", 0),
    /* Predefined namespace UUIDs (RFC 9562 sec.6.6 / RFC 4122 appendix C). */
    JS_PROP_STRING_DEF("NAMESPACE_DNS",  "6ba7b810-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_URL",  "6ba7b811-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_OID",  "6ba7b812-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_X500", "6ba7b814-9dad-11d1-80b4-00c04fd430c8", 0),
};

static int dyn_uuid_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_uuid_funcs, countof(dyn_uuid_funcs));
}

int js_nat_init_uuid(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:uuid", dyn_uuid_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_uuid_funcs, countof(dyn_uuid_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_UUID */
