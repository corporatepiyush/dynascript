/*
 * scl:sort -- native sorting + binary search backed by secure-c-libs.
 *
 *   import { sort, binarySearch } from "scl:sort";
 *   const s = sort([3, 1, 2]);      // -> new ascending Array; input NOT mutated
 *   const i = binarySearch(s, 2);   // -> index, or -1 if absent
 *
 * TRANSIENT operations (see qjs-scl.h): each call spins up a private SCL arena,
 * reads the WHOLE JS Array into an arena-backed double[], runs an scl_sort_*
 * over it, copies the sorted values into a FRESH JS Array, then destroys the
 * arena. Nothing native escapes into the JS heap, so peak RSS stays flat across
 * calls (one arena per call, freed before returning).
 *
 * Memory discipline: JS_ToFloat64 / JS_GetPropertyUint32 run arbitrary user JS
 * (element getters, valueOf, Proxy traps). We fully MATERIALIZE every input into
 * the private arena FIRST, and only then sort / search / copy out -- no native
 * pointer is ever used across a JS-invoking call. The arena is a C local no JS
 * value can reach, so a re-entrant callback cannot free it.
 *
 * Total order over IEEE-754 doubles (NaN handling): finite/infinite values
 * compare by magnitude; every NaN is defined GREATER than every non-NaN and
 * EQUAL to every other NaN, so NaN sorts to the END. This is a genuine total
 * order, which quicksort and binary search both require. (JS's own comparator
 * `(a,b)=>a-b` yields NaN for any NaN pair, which the spec treats as 0/"equal",
 * leaving NaN placement unspecified; we pin it down instead.) -0 and +0 compare
 * equal, matching `a-b`.
 */
#include "qjs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_sort.h"   /* scl_sort_quick(base, count, elem_size, cmp) */
#include "scl_binary.h" /* scl_search_binary_search(...) */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Total order over doubles: NaN last, all NaNs equal, -0 == +0. Returns
 * <0 / 0 / >0 like any qsort-style comparator. */
static int js_scl_sort_cmp_double(const void *pa, const void *pb)
{
    double a = *(const double *)pa;
    double b = *(const double *)pb;
    int a_nan = (a != a); /* IEEE: NaN is the only value != itself */
    int b_nan = (b != b);
    if (a_nan || b_nan)
        return a_nan - b_nan; /* NaN > number; NaN == NaN (both 1 -> 0) */
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0; /* equal, incl. -0.0 vs +0.0 */
}

/* Length of JS Array `v` into *out_len, or -1 (throws TypeError) if not an
 * Array. Coerces before any native work, per the module's memory discipline. */
static int js_scl_sort_len(JSContext *ctx, JSValueConst v, uint32_t *out_len)
{
    JSValue lval;
    int ret;

    if (!JS_IsArray(ctx, v)) {
        JS_ThrowTypeError(ctx, "scl:sort: expected an Array");
        return -1;
    }
    lval = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(lval))
        return -1;
    ret = JS_ToUint32(ctx, out_len, lval);
    JS_FreeValue(ctx, lval);
    return ret ? -1 : 0;
}

/* Read the ENTIRE JS Array `arr` (len >= 1 elements) into a fresh arena-backed
 * double[]. Every element is coerced with JS_ToFloat64 (which may run user JS)
 * BEFORE any sort/search touches native state. On success *pbuf points into the
 * arena; on error returns -1 with an exception pending. */
static int js_scl_sort_read(JSContext *ctx, scl_allocator_t *arena,
                            JSValueConst arr, uint32_t len, double **pbuf)
{
    size_t bytes;
    double *buf;
    uint32_t i;

    /* Overflow guard for count * sizeof(double); the 256 MiB arena cap
     * (js_scl_arena_new) additionally throws OOM for merely-absurd sizes. */
    if (scl_mul_overflow((size_t)len, sizeof(double), &bytes)) {
        JS_ThrowRangeError(ctx, "scl:sort: array too large");
        return -1;
    }
    buf = (double *)scl_alloc(arena, bytes, JS_SCL_ARENA_ALIGN);
    if (!buf) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < len; i++) {
        double x;
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(v))
            return -1;
        if (JS_ToFloat64(ctx, &x, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
        buf[i] = x;
    }
    *pbuf = buf;
    return 0;
}

/* Fresh JS Array holding buf[0..len). Consumes nothing native; on failure the
 * caller still owns the arena to destroy. */
static JSValue js_scl_sort_to_js(JSContext *ctx, const double *buf, uint32_t len)
{
    uint32_t i;
    JSValue out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    for (i = 0; i < len; i++) {
        /* JS_DefinePropertyValueUint32 consumes the value on every path. */
        if (JS_DefinePropertyValueUint32(ctx, out, i,
                                         JS_NewFloat64(ctx, buf[i]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    return out;
}

/* sort(array) -> new ascending Array (does NOT mutate the input). */
static JSValue js_scl_sort_sort(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    double *buf = NULL;
    uint32_t len;
    JSValue out;
    scl_error_t err;

    (void)this_val;
    (void)argc;

    if (js_scl_sort_len(ctx, argv[0], &len))
        return JS_EXCEPTION;
    if (len == 0)
        return JS_NewArray(ctx); /* empty -> empty; no arena needed */

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION; /* OOM already thrown */

    /* 1) Fully materialize the input into the arena BEFORE any native work. */
    if (js_scl_sort_read(ctx, arena, argv[0], len, &buf)) {
        scl_alloc_arena_destroy(arena);
        return JS_EXCEPTION;
    }

    /* 2) Sort in place: pure C, no JS, no allocation for doubles. */
    err = scl_sort_quick(buf, len, sizeof(double), js_scl_sort_cmp_double);
    if (err != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowInternalError(ctx, "scl:sort: %s",
                                     scl_error_string(err));
    }

    /* 3) Copy sorted values into a FRESH JS Array (independent of the arena). */
    out = js_scl_sort_to_js(ctx, buf, len);

    /* 4) Reclaim the arena; `out` is a fully independent deep copy. */
    scl_alloc_arena_destroy(arena);
    return out;
}

/* binarySearch(array, target) -> index of a matching element, or -1.
 * `array` is assumed sorted by the same total order sort() produces. */
static JSValue js_scl_sort_binary_search(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    double *buf = NULL;
    double key;
    uint32_t len;
    size_t idx = 0;
    scl_error_t err;

    (void)this_val;
    (void)argc;

    if (js_scl_sort_len(ctx, argv[0], &len))
        return JS_EXCEPTION;
    if (len == 0) {
        /* Coerce the key too, so a throwing/side-effecting valueOf behaves the
         * same as on the non-empty path, then report "absent". */
        if (JS_ToFloat64(ctx, &key, argv[1]))
            return JS_EXCEPTION;
        return JS_NewInt32(ctx, -1);
    }

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;

    /* Materialize the array, then the key -- both run JS; neither can reach the
     * private arena. Nothing native is read until both are captured. */
    if (js_scl_sort_read(ctx, arena, argv[0], len, &buf)) {
        scl_alloc_arena_destroy(arena);
        return JS_EXCEPTION;
    }
    if (JS_ToFloat64(ctx, &key, argv[1])) {
        scl_alloc_arena_destroy(arena);
        return JS_EXCEPTION;
    }

    err = scl_search_binary_search(buf, len, sizeof(double), &key,
                                   js_scl_sort_cmp_double, &idx);
    scl_alloc_arena_destroy(arena);

    if (err == SCL_OK)
        return JS_NewInt64(ctx, (int64_t)idx);
    return JS_NewInt32(ctx, -1); /* SCL_ERR_NOT_FOUND / SCL_ERR_EMPTY */
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry js_scl_sort_funcs[] = {
    JS_CFUNC_DEF("sort", 1, js_scl_sort_sort),
    JS_CFUNC_DEF("binarySearch", 2, js_scl_sort_binary_search),
};

static int js_scl_sort_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, js_scl_sort_funcs,
                                  countof(js_scl_sort_funcs));
}

int js_scl_init_sort(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:sort", js_scl_sort_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, js_scl_sort_funcs,
                                  countof(js_scl_sort_funcs));
}

#endif /* CONFIG_SCL_MODULES */
