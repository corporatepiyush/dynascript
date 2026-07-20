/*
 * scl:random -- native random number generation backed by secure-c-libs.
 *
 *   import { Random, uuid } from "scl:random";
 *   const r = new Random(42);                    // deterministic when seeded
 *   try {
 *     r.nextU64();      // BigInt  in [0, 2^64)   (full 64-bit, lossless)
 *     r.nextU53();      // Number  in [0, 2^53)   (top 53 bits, exact)
 *     r.nextFloat();    // Number  in [0, 1)
 *     r.nextBounded(6); // uniform in [0, 6)      (unbiased rejection sampling)
 *     r.fill(new Uint8Array(16));                 // fill raw bytes in place
 *   } finally { r.close(); }                      // deterministic free (arena)
 *   const id = uuid();  // "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" (RFC 4122 v4)
 *
 * Backing PRNG is xoshiro256** (256-bit state, splitmix64 seeding). Each Random
 * owns a private SCL arena (see dynajs-scl.h) that holds its scl_rand_prng_t; the
 * struct is POD, so disposal is a single arena destroy -- no per-node free.
 * Native results are copied into fresh JS values at the boundary; no arena
 * pointer ever escapes into the JS heap.
 *
 * uint64 representation choice (documented, type-stable): a 64-bit draw does not
 * always fit a JS Number (safe integers stop at 2^53), so nextU64() ALWAYS
 * returns a BigInt -- lossless and predictable, never a Number-or-BigInt union.
 * nextU53() is the fast Number path (the top 53 bits, always exact). nextBounded
 * mirrors its argument's type: a Number bound (<= 2^53) yields a Number, a
 * BigInt bound yields a BigInt, so the result never silently loses precision.
 */
#include "dynajs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_rand_prng.h"   /* scl_rand_prng_t, init/next/next_double/... */
#include "scl_rand_uuid.h"   /* scl_rand_uuid_generate / _to_string */
#include "scl_stdlib.h"      /* scl_rand_u64 (system CSPRNG, unseeded ctor) */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* 2^53: largest integer a JS Number represents exactly. A Number bound above
 * this cannot be trusted (consecutive integers are no longer distinct), so
 * nextBounded() rejects it and asks for a BigInt instead. */
#define JS_SCL_RANDOM_MAX_SAFE 9007199254740992.0

/* 36 hex/hyphen chars + NUL: exactly what scl_rand_uuid_to_string writes. */
#define JS_SCL_UUID_STRLEN 37

/* ---------- Random: seedable PRNG (scl_rand_prng, xoshiro256**) ---------- */

static JSClassID js_scl_random_class_id;

/* scl_rand_prng_t is POD and lives entirely in the arena, so no per-object
 * teardown is needed beyond the arena destroy the framework already runs. */
static const JSClassDef js_scl_random_class = {
    "Random",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_random_ctor(JSContext *ctx, JSValueConst new_target,
                                  int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_rand_prng_t *rng;
    uint64_t seed;

    /* Resolve the seed to a C local BEFORE allocating anything (so a throwing
     * coercion leaks no arena). A given seed is deterministic; an omitted or
     * undefined seed is drawn from the system CSPRNG. A Number seed and the
     * equal BigInt seed (e.g. 42 and 42n) map to the same stream. */
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        seed = scl_rand_u64();
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

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    rng = (scl_rand_prng_t *)arena->malloc_fn(arena->state, sizeof(*rng),
                                              JS_SCL_ARENA_ALIGN);
    if (!rng || scl_rand_prng_init(rng, seed) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_random_class_id, arena, rng, NULL);
}

/* NOTE (the whole discipline): coerce every JS argument to a C local BEFORE
 * resolving the native handle. Coercion runs arbitrary JS (valueOf/@@toPrimitive
 * /Proxy) which may close() this very object and free the arena; resolving after
 * coercion means js_scl_resource_get() observes r->closed and throws instead of
 * touching a freed arena. No JS-invoking call may sit between resolve and use. */

static JSValue js_scl_random_next_u64(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val,
                                           js_scl_random_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, scl_rand_prng_next((scl_rand_prng_t *)r->native));
}

static JSValue js_scl_random_next_u53(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val,
                                           js_scl_random_class_id);
    uint64_t v;
    if (!r)
        return JS_EXCEPTION;
    v = scl_rand_prng_next((scl_rand_prng_t *)r->native);
    /* top 53 bits: in [0, 2^53), always exact as a JS Number. */
    return JS_NewInt64(ctx, (int64_t)(v >> 11));
}

static JSValue js_scl_random_next_float(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val,
                                           js_scl_random_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx,
                         scl_rand_prng_next_double((scl_rand_prng_t *)r->native));
}

static JSValue js_scl_random_next_bounded(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    JSSclResource *r;
    uint64_t bound, result;
    int is_bigint;

    /* Coerce the bound to a C local FIRST (JS_ToFloat64 may run user valueOf),
     * THEN resolve. A BigInt bound maps to a BigInt result; a Number bound to a
     * Number result -- so the return type follows the input and never truncates
     * a value that outgrew 2^53. */
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
        if (!(d >= 1.0) || d > JS_SCL_RANDOM_MAX_SAFE)
            return JS_ThrowRangeError(ctx,
                "bound must be an integer in [1, 2^53] (use a BigInt for more)");
        bound = (uint64_t)d; /* ToInteger semantics: any fraction is truncated */
    }

    r = js_scl_resource_get(ctx, this_val, js_scl_random_class_id);
    if (!r)
        return JS_EXCEPTION;
    result = scl_rand_prng_next_bounded((scl_rand_prng_t *)r->native, bound);
    return is_bigint ? JS_NewBigUint64(ctx, result)
                     : JS_NewInt64(ctx, (int64_t)result);
}

static JSValue js_scl_random_fill(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSSclResource *r;
    JSValue buf_val;
    uint8_t *base;
    size_t byte_off, byte_len, ab_size;

    /* Extract the destination buffer FIRST. JS_GetTypedArrayBuffer runs no user
     * JS for a real TypedArray (and throws TypeError for anything else, so the
     * {valueOf(){this.close()}} attack never reaches a native use), THEN resolve
     * the handle -- with no JS between the resolve and scl_rand_prng_fill. The
     * bytes land in the JS-owned ArrayBuffer, never in the arena. */
    buf_val = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_off, &byte_len, NULL);
    if (JS_IsException(buf_val))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab_size, buf_val);
    if (!base) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }

    r = js_scl_resource_get(ctx, this_val, js_scl_random_class_id);
    if (!r) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }
    if (byte_off > ab_size || byte_len > ab_size - byte_off) {
        JS_FreeValue(ctx, buf_val);
        return JS_ThrowRangeError(ctx, "typed array out of bounds");
    }
    scl_rand_prng_fill((scl_rand_prng_t *)r->native, base + byte_off, byte_len);
    JS_FreeValue(ctx, buf_val);
    return JS_DupValue(ctx, argv[0]); /* return the filled view for chaining */
}

static const JSCFunctionListEntry js_scl_random_proto[] = {
    JS_CFUNC_DEF("nextU64", 0, js_scl_random_next_u64),
    JS_CFUNC_DEF("nextU53", 0, js_scl_random_next_u53),
    JS_CFUNC_DEF("nextFloat", 0, js_scl_random_next_float),
    JS_CFUNC_DEF("nextBounded", 1, js_scl_random_next_bounded),
    JS_CFUNC_DEF("fill", 1, js_scl_random_fill),
};

/* ---------- uuid(): plain function, RFC 4122 v4 (system entropy) ---------- */

/* Transient: no long-lived native object, so no arena. The 16 random bytes and
 * their 36-char rendering live on the C stack; only the finished string crosses
 * into JS (JS_NewString copies it). */
static JSValue js_scl_uuid(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    scl_rand_uuid_t u;
    char str[JS_SCL_UUID_STRLEN];

    if (scl_rand_uuid_generate(&u) != SCL_OK)
        return JS_ThrowInternalError(ctx, "uuid generation failed");
    if (scl_rand_uuid_to_string(&u, str) != SCL_OK)
        return JS_ThrowInternalError(ctx, "uuid formatting failed");
    return JS_NewString(ctx, str);
}

/* ---------- module registration ---------- */

static int js_scl_random_init_module(JSContext *ctx, JSModuleDef *m)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, ctor, uuid_fn;

    JS_NewClassID(&js_scl_random_class_id);
    if (JS_NewClass(rt, js_scl_random_class_id, &js_scl_random_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, js_scl_random_proto,
                               countof(js_scl_random_proto));
    js_scl_class_common(ctx, js_scl_random_class_id, proto);
    JS_SetClassProto(ctx, js_scl_random_class_id, proto);

    ctor = JS_NewCFunction2(ctx, js_scl_random_ctor, "Random", 1,
                            JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    if (JS_SetModuleExport(ctx, m, "Random", ctor) < 0)
        return -1;

    uuid_fn = JS_NewCFunction(ctx, js_scl_uuid, "uuid", 0);
    if (JS_SetModuleExport(ctx, m, "uuid", uuid_fn) < 0)
        return -1;
    return 0;
}

int js_scl_init_random(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:random",
                                   js_scl_random_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Random");
    JS_AddModuleExport(ctx, m, "uuid");
    return 0;
}

#endif /* CONFIG_SCL_MODULES */
