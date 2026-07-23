/*
 * dynajs:random -- seedable PRNG + RFC 4122 v4 UUIDs. Self-contained, in-repo.
 *
 *   import { Random, uuid } from "dynajs:random";
 *   const r = new Random(42);        // deterministic when seeded
 *   try {
 *     r.nextU64();      // BigInt in [0, 2^64)     (full 64-bit, lossless)
 *     r.nextU53();      // Number in [0, 2^53)     (top 53 bits, exact)
 *     r.nextFloat();    // Number in [0, 1)
 *     r.nextBounded(6); // uniform in [0, 6)       (unbiased rejection sampling)
 *     r.fill(new Uint8Array(16));
 *   } finally { r.close(); }
 *   const id = uuid();  // "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
 *
 * PRNG: xoshiro256** (256-bit state) seeded through splitmix64. The native
 * object is just its 256-bit state (POD), so disposal is a single free(). The
 * unseeded constructor and uuid() draw from OS entropy. Native results are
 * copied into fresh JS values at the boundary -- nothing native escapes.
 *
 * uint64 representation (type-stable): a 64-bit draw does not always fit a JS
 * Number, so nextU64() ALWAYS returns a BigInt (lossless), nextU53() is the fast
 * exact Number path, and nextBounded() mirrors its argument's type.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_RANDOM)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_RANDOM_MAX_SAFE 9007199254740992.0 /* 2^53 */
#define DYN_UUID_STRLEN     37                 /* 36 chars + NUL */

/* ---------- OS entropy (unseeded PRNG + uuid) ---------- */

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h> /* arc4random_buf */
static void dyn_os_entropy(void *buf, size_t n)
{
    arc4random_buf(buf, n);
}
#else
#include <sys/random.h> /* getrandom on Linux */
#include <errno.h>
static void dyn_os_entropy(void *buf, size_t n)
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

/* ---------- xoshiro256** core ---------- */

typedef struct {
    uint64_t s[4];
} dyn_prng_t;

static inline uint64_t rotl64(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void dyn_prng_seed(dyn_prng_t *r, uint64_t seed)
{
    uint64_t sm = seed;
    r->s[0] = splitmix64(&sm);
    r->s[1] = splitmix64(&sm);
    r->s[2] = splitmix64(&sm);
    r->s[3] = splitmix64(&sm);
}

static uint64_t dyn_prng_next(dyn_prng_t *r)
{
    uint64_t *s = r->s;
    uint64_t result = rotl64(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return result;
}

/* [0, 1) with 53 bits of mantissa precision. */
static double dyn_prng_next_double(dyn_prng_t *r)
{
    return (double)(dyn_prng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

/* Uniform in [0, bound), unbiased via rejection (no modulo bias). */
static uint64_t dyn_prng_next_bounded(dyn_prng_t *r, uint64_t bound)
{
    /* threshold = (2^64) mod bound, computed as (-bound) mod bound. */
    uint64_t threshold = (0 - bound) % bound;
    for (;;) {
        uint64_t v = dyn_prng_next(r);
        if (v >= threshold)
            return v % bound;
    }
}

static void dyn_prng_fill(dyn_prng_t *r, uint8_t *dst, size_t n)
{
    while (n >= 8) {
        uint64_t v = dyn_prng_next(r);
        memcpy(dst, &v, 8);
        dst += 8;
        n -= 8;
    }
    if (n > 0) {
        uint64_t v = dyn_prng_next(r);
        memcpy(dst, &v, n);
    }
}

/* ---------- Random class ---------- */

static JSClassID dyn_random_class_id;

static void dyn_random_dispose(void *native)
{
    free(native);
}

static const JSClassDef dyn_random_class = {
    "Random",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_random_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    uint64_t seed;

    /* Resolve the seed to a C local BEFORE allocating (a throwing coercion then
     * leaks nothing). A given seed is deterministic; an omitted/undefined seed
     * is drawn from OS entropy. 42 and 42n map to the same stream. */
    (void)new_target;
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        dyn_os_entropy(&seed, sizeof(seed));
    } else if (JS_IsBigInt(ctx, argv[0])) {
        int64_t s;
        if (JS_ToBigInt64(ctx, &s, argv[0]))
            return JS_EXCEPTION;
        seed = (uint64_t)s;
    } else {
        int64_t s;
        if (JS_ToInt64(ctx, &s, argv[0]))
            return JS_EXCEPTION;
        seed = (uint64_t)s;
    }

    rng = (dyn_prng_t *)malloc(sizeof(*rng));
    if (!rng)
        return JS_ThrowOutOfMemory(ctx);
    dyn_prng_seed(rng, seed);
    return dyn_res_wrap(ctx, dyn_random_class_id, rng, dyn_random_dispose);
}

static JSValue dyn_random_next_u64(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_prng_t *rng = dyn_res_native(ctx, this_val, dyn_random_class_id);
    (void)argc; (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, dyn_prng_next(rng));
}

static JSValue dyn_random_next_u53(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_prng_t *rng = dyn_res_native(ctx, this_val, dyn_random_class_id);
    (void)argc; (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)(dyn_prng_next(rng) >> 11));
}

static JSValue dyn_random_next_float(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_prng_t *rng = dyn_res_native(ctx, this_val, dyn_random_class_id);
    (void)argc; (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_prng_next_double(rng));
}

static JSValue dyn_random_next_bounded(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    uint64_t bound, result;
    int is_bigint;

    /* Coerce the bound FIRST (JS_ToFloat64 may run user valueOf), THEN resolve.
     * A BigInt bound yields a BigInt result; a Number bound a Number -- the
     * return type follows the input and never truncates past 2^53. */
    (void)argc;
    is_bigint = JS_IsBigInt(ctx, argv[0]);
    if (is_bigint) {
        int64_t b;
        if (JS_ToBigInt64(ctx, &b, argv[0]))
            return JS_EXCEPTION;
        if (b <= 0)
            return JS_ThrowRangeError(ctx, "bound must be a positive integer");
        bound = (uint64_t)b;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (!(d >= 1.0) || d > DYN_RANDOM_MAX_SAFE)
            return JS_ThrowRangeError(ctx,
                "bound must be an integer in [1, 2^53] (use a BigInt for more)");
        bound = (uint64_t)d;
    }

    rng = dyn_res_native(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    result = dyn_prng_next_bounded(rng, bound);
    return is_bigint ? JS_NewBigUint64(ctx, result)
                     : JS_NewInt64(ctx, (int64_t)result);
}

static JSValue dyn_random_fill(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    JSValue buf_val;
    uint8_t *base;
    size_t byte_off, byte_len, ab_size;

    /* Extract the destination buffer FIRST (no user JS runs for a real
     * TypedArray; anything else throws TypeError, so a valueOf-close attack
     * never reaches native use), THEN resolve -- nothing JS-invoking between the
     * resolve and dyn_prng_fill. Bytes land in the JS-owned ArrayBuffer. */
    (void)argc;
    buf_val = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_off, &byte_len, NULL);
    if (JS_IsException(buf_val))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab_size, buf_val);
    if (!base) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }

    rng = dyn_res_native(ctx, this_val, dyn_random_class_id);
    if (!rng) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }
    if (byte_off > ab_size || byte_len > ab_size - byte_off) {
        JS_FreeValue(ctx, buf_val);
        return JS_ThrowRangeError(ctx, "typed array out of bounds");
    }
    dyn_prng_fill(rng, base + byte_off, byte_len);
    JS_FreeValue(ctx, buf_val);
    return JS_DupValue(ctx, argv[0]);
}

static const JSCFunctionListEntry dyn_random_proto[] = {
    JS_CFUNC_DEF("nextU64", 0, dyn_random_next_u64),
    JS_CFUNC_DEF("nextU53", 0, dyn_random_next_u53),
    JS_CFUNC_DEF("nextFloat", 0, dyn_random_next_float),
    JS_CFUNC_DEF("nextBounded", 1, dyn_random_next_bounded),
    JS_CFUNC_DEF("fill", 1, dyn_random_fill),
};

/* ---------- uuid(): RFC 4122 v4 from OS entropy ---------- */

static JSValue dyn_uuid(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t b[16];
    char str[DYN_UUID_STRLEN];
    int i, j = 0;

    (void)this_val; (void)argc; (void)argv;
    dyn_os_entropy(b, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40); /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10x */
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            str[j++] = '-';
        str[j++] = hex[b[i] >> 4];
        str[j++] = hex[b[i] & 0x0f];
    }
    str[j] = '\0';
    return JS_NewStringLen(ctx, str, (size_t)j);
}

/* ---------- module registration ---------- */

static int dyn_random_init_module(JSContext *ctx, JSModuleDef *m)
{
    JSValue uuid_fn;

    if (dyn_register_class(ctx, m, &dyn_random_class_id, &dyn_random_class,
                           dyn_random_proto, countof(dyn_random_proto),
                           dyn_random_ctor, "Random") < 0)
        return -1;
    uuid_fn = JS_NewCFunction(ctx, dyn_uuid, "uuid", 0);
    if (JS_SetModuleExport(ctx, m, "uuid", uuid_fn) < 0)
        return -1;
    return 0;
}

int js_nat_init_random(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dynajs:random", dyn_random_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Random");
    JS_AddModuleExport(ctx, m, "uuid");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_RANDOM */
