/*
 * dynajs:sort -- native sorting + binary search. Self-contained, in-repo (no
 * external deps).
 *
 *   import { sort, binarySearch } from "dynajs:sort";
 *   const s = sort([3, 1, 2]);        // -> new ascending Array; input NOT mutated
 *   const i = binarySearch(s, 2);     // -> index, or -1 if absent
 *   sort(arr, (a, b) => b - a);       // optional comparator (runs user JS)
 *   binarySearch(arr, x, (a, b) => a - b);
 *
 * These are TRANSIENT plain functions (no `this`, no closable native resource):
 * each call materialises the WHOLE JS Array into a private C buffer FIRST, then
 * sorts/searches, then copies results into FRESH JS values before returning.
 * Nothing native escapes into the JS heap, so peak RSS stays flat across calls.
 *
 * Two paths, chosen by whether a comparator is supplied:
 *
 *   - NO comparator: every element is coerced ONCE with JS_ToFloat64 into a
 *     C double[] and sorted by a total order over IEEE-754 doubles (NaN sorts
 *     to the END -- all NaNs equal, -0 == +0). sort() returns a Number Array;
 *     binarySearch() coerces the target the same way. This is a genuine total
 *     order, which merge sort and binary search both require.
 *
 *   - WITH comparator: elements are snapshotted as owned JSValues and only an
 *     index array is permuted; the comparator (arbitrary user JS) may mutate the
 *     original array or free unrelated state -- our snapshot and index buffer are
 *     C locals no JS value can reach, so a re-entrant callback cannot corrupt the
 *     sort. sort() returns the original elements in comparator order. A NaN
 *     comparator result is treated as 0 ("equal"), matching Array.prototype.sort.
 *
 * Sorting is a bottom-up merge sort: STABLE (equal elements keep input order) and
 * iterative (no recursion, so no stack blow-up on huge arrays). Coercion runs
 * arbitrary JS but every input is fully captured before any native comparison;
 * every owned JSValue is released on every path.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SORT)

#include <stdint.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------- shared helpers ---------- */

/* Total order over doubles: NaN last, all NaNs equal, -0 == +0. */
static int dyn_sort_cmp_double(double a, double b)
{
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
 * Array. Runs before any native work, per the module's memory discipline. */
static int dyn_sort_arr_len(JSContext *ctx, JSValueConst v, uint32_t *out_len)
{
    JSValue lval;
    int ret, isarr;

    isarr = JS_IsArray(ctx, v); /* tri-state: <0 = exception (revoked proxy) */
    if (isarr < 0)
        return -1;
    if (!isarr) {
        JS_ThrowTypeError(ctx, "dynajs:sort: expected an Array");
        return -1;
    }
    lval = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(lval))
        return -1;
    ret = JS_ToUint32(ctx, out_len, lval);
    JS_FreeValue(ctx, lval);
    return ret ? -1 : 0;
}

/* Resolve an optional comparator at argv[idx]. *pcmp is set to the callable
 * value (borrowed) or JS_UNDEFINED when none is supplied. Returns -1 (throwing
 * TypeError) if the argument is present, not undefined, and not callable --
 * matching Array.prototype.sort. */
static int dyn_sort_get_cmp(JSContext *ctx, int argc, JSValueConst *argv,
                            int idx, JSValueConst *pcmp)
{
    if (argc <= idx || JS_IsUndefined(argv[idx])) {
        *pcmp = JS_UNDEFINED;
        return 0;
    }
    if (!JS_IsFunction(ctx, argv[idx])) {
        JS_ThrowTypeError(ctx, "dynajs:sort: comparator is not a function");
        return -1;
    }
    *pcmp = argv[idx];
    return 0;
}

/* ---------- comparator path (owned JSValue snapshot + index permute) ---------- */

/* Snapshot the whole Array into a fresh owned JSValue[len]. Every getter runs
 * here, BEFORE any comparison. On failure frees the partial snapshot and returns
 * NULL with an exception pending. */
static JSValue *dyn_sort_read_values(JSContext *ctx, JSValueConst arr,
                                     uint32_t len)
{
    JSValue *elems;
    size_t bytes = (size_t)len * sizeof(JSValue);
    uint32_t i;

    if (bytes / sizeof(JSValue) != len) { /* size_t multiply overflow */
        JS_ThrowRangeError(ctx, "dynajs:sort: array too large");
        return NULL;
    }
    elems = js_malloc(ctx, bytes);
    if (!elems)
        return NULL; /* js_malloc threw OOM */
    for (i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(v)) {
            while (i-- > 0)
                JS_FreeValue(ctx, elems[i]);
            js_free(ctx, elems);
            return NULL;
        }
        elems[i] = v;
    }
    return elems;
}

/* Invoke the user comparator cmp(a, b). Once *err is set, returns 0 without
 * calling JS again (bounds the damage of a throwing comparator). A NaN or 0
 * result maps to "equal", matching Array.prototype.sort. */
static int dyn_sort_cmp_call(JSContext *ctx, JSValueConst cmp,
                             JSValueConst a, JSValueConst b, int *err)
{
    JSValueConst args[2];
    JSValue r;
    double d;

    if (*err)
        return 0;
    args[0] = a;
    args[1] = b;
    r = JS_Call(ctx, cmp, JS_UNDEFINED, 2, args);
    if (JS_IsException(r)) {
        *err = 1;
        return 0;
    }
    if (JS_ToFloat64(ctx, &d, r)) {
        JS_FreeValue(ctx, r);
        *err = 1;
        return 0;
    }
    JS_FreeValue(ctx, r);
    if (d < 0)
        return -1;
    if (d > 0)
        return 1;
    return 0;
}

/* Stable bottom-up merge sort of the index array `idx` (values 0..n-1) by
 * comparing elems[idx[..]] with the user comparator. `tmp` is scratch of length
 * n. Returns -1 if the comparator threw (exception pending), else 0. */
static int dyn_sort_indices(JSContext *ctx, JSValueConst cmp,
                            const JSValue *elems, uint32_t *idx, uint32_t *tmp,
                            uint32_t n)
{
    uint32_t width;
    int err = 0;

    for (width = 1; width < n; width *= 2) {
        uint32_t i;
        for (i = 0; i < n; i += 2 * width) {
            uint32_t lo = i;
            uint32_t mid = (i + width < n) ? i + width : n;
            uint32_t hi = (i + 2 * width < n) ? i + 2 * width : n;
            uint32_t l = lo, r = mid, k = lo;
            while (l < mid && r < hi) {
                /* <= 0 keeps the left (lower-index) run first => stable. */
                if (dyn_sort_cmp_call(ctx, cmp, elems[idx[l]],
                                      elems[idx[r]], &err) <= 0)
                    tmp[k++] = idx[l++];
                else
                    tmp[k++] = idx[r++];
            }
            while (l < mid)
                tmp[k++] = idx[l++];
            while (r < hi)
                tmp[k++] = idx[r++];
        }
        memcpy(idx, tmp, (size_t)n * sizeof(uint32_t));
        if (err)
            return -1;
    }
    return 0;
}

/* sort(array, cmp) with a comparator -> new Array of the original elements in
 * comparator order. Input is NOT mutated. */
static JSValue dyn_sort_with_cmp(JSContext *ctx, JSValueConst arr,
                                 JSValueConst cmp, uint32_t len)
{
    JSValue *elems;
    uint32_t *idx, *tmp;
    JSValue out;
    uint32_t i;

    elems = dyn_sort_read_values(ctx, arr, len);
    if (!elems)
        return JS_EXCEPTION;

    idx = js_malloc(ctx, (size_t)len * sizeof(uint32_t));
    tmp = js_malloc(ctx, (size_t)len * sizeof(uint32_t));
    if (!idx || !tmp) {
        js_free(ctx, idx);
        js_free(ctx, tmp);
        for (i = 0; i < len; i++)
            JS_FreeValue(ctx, elems[i]);
        js_free(ctx, elems);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++)
        idx[i] = i;

    if (dyn_sort_indices(ctx, cmp, elems, idx, tmp, len)) {
        out = JS_EXCEPTION;
        goto cleanup;
    }

    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        goto cleanup;
    for (i = 0; i < len; i++) {
        /* Dup: elems are freed below; DefineProperty consumes the value. */
        if (JS_DefinePropertyValueUint32(ctx, out, i,
                                         JS_DupValue(ctx, elems[idx[i]]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            out = JS_EXCEPTION;
            goto cleanup;
        }
    }

 cleanup:
    js_free(ctx, idx);
    js_free(ctx, tmp);
    for (i = 0; i < len; i++)
        JS_FreeValue(ctx, elems[i]);
    js_free(ctx, elems);
    return out;
}

/* ---------- double path (no comparator) ---------- */

/* Materialise the whole Array into a fresh double[len], coercing each element
 * ONCE with JS_ToFloat64. On failure returns NULL with an exception pending. */
static double *dyn_sort_read_doubles(JSContext *ctx, JSValueConst arr,
                                     uint32_t len)
{
    double *buf;
    size_t bytes = (size_t)len * sizeof(double);
    uint32_t i;

    if (bytes / sizeof(double) != len) { /* size_t multiply overflow */
        JS_ThrowRangeError(ctx, "dynajs:sort: array too large");
        return NULL;
    }
    buf = js_malloc(ctx, bytes);
    if (!buf)
        return NULL;
    for (i = 0; i < len; i++) {
        double x;
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(v)) {
            js_free(ctx, buf);
            return NULL;
        }
        if (JS_ToFloat64(ctx, &x, v)) {
            JS_FreeValue(ctx, v);
            js_free(ctx, buf);
            return NULL;
        }
        JS_FreeValue(ctx, v);
        buf[i] = x;
    }
    return buf;
}

/* Stable bottom-up merge sort of double[n] by the total order; `tmp` is scratch
 * of length n. */
static void dyn_sort_merge_doubles(double *a, double *tmp, uint32_t n)
{
    uint32_t width;
    for (width = 1; width < n; width *= 2) {
        uint32_t i;
        for (i = 0; i < n; i += 2 * width) {
            uint32_t lo = i;
            uint32_t mid = (i + width < n) ? i + width : n;
            uint32_t hi = (i + 2 * width < n) ? i + 2 * width : n;
            uint32_t l = lo, r = mid, k = lo;
            while (l < mid && r < hi) {
                if (dyn_sort_cmp_double(a[l], a[r]) <= 0)
                    tmp[k++] = a[l++];
                else
                    tmp[k++] = a[r++];
            }
            while (l < mid)
                tmp[k++] = a[l++];
            while (r < hi)
                tmp[k++] = a[r++];
        }
        memcpy(a, tmp, (size_t)n * sizeof(double));
    }
}

/* sort(array) with no comparator -> new ascending Number Array. */
static JSValue dyn_sort_by_double(JSContext *ctx, JSValueConst arr, uint32_t len)
{
    double *buf, *tmp;
    JSValue out;
    uint32_t i;

    buf = dyn_sort_read_doubles(ctx, arr, len);
    if (!buf)
        return JS_EXCEPTION;
    tmp = js_malloc(ctx, (size_t)len * sizeof(double));
    if (!tmp) {
        js_free(ctx, buf);
        return JS_EXCEPTION;
    }
    dyn_sort_merge_doubles(buf, tmp, len);
    js_free(ctx, tmp);

    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        js_free(ctx, buf);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        if (JS_DefinePropertyValueUint32(ctx, out, i,
                                         JS_NewFloat64(ctx, buf[i]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            js_free(ctx, buf);
            return JS_EXCEPTION;
        }
    }
    js_free(ctx, buf);
    return out;
}

/* ---------- entry points ---------- */

/* sort(array[, cmp]) -> new sorted Array (does NOT mutate the input). */
static JSValue dyn_sort_sort(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValueConst cmp;
    uint32_t len;

    (void)this_val;

    /* Resolve the (optional) comparator, then the length, both BEFORE any
     * native work; a throwing coercion leaks nothing. */
    if (dyn_sort_get_cmp(ctx, argc, argv, 1, &cmp))
        return JS_EXCEPTION;
    if (dyn_sort_arr_len(ctx, argv[0], &len))
        return JS_EXCEPTION;
    if (len == 0)
        return JS_NewArray(ctx); /* empty -> empty; nothing to coerce */

    if (JS_IsUndefined(cmp))
        return dyn_sort_by_double(ctx, argv[0], len);
    return dyn_sort_with_cmp(ctx, argv[0], cmp, len);
}

/* Binary search of a comparator-sorted snapshot for `target`. Returns an index
 * (>=0) via *pidx and 1 if found, 0 if absent, -1 on a comparator throw. */
static int dyn_sort_bsearch_cmp(JSContext *ctx, JSValueConst cmp,
                                const JSValue *elems, uint32_t len,
                                JSValueConst target, int64_t *pidx)
{
    uint32_t lo = 0, hi = len;
    int err = 0;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = dyn_sort_cmp_call(ctx, cmp, elems[mid], target, &err);
        if (err)
            return -1;
        if (c < 0)
            lo = mid + 1;
        else if (c > 0)
            hi = mid;
        else {
            *pidx = (int64_t)mid;
            return 1;
        }
    }
    return 0;
}

/* binarySearch(array, target[, cmp]) -> index of a matching element, or -1.
 * `array` is assumed sorted by the same order the matching sort() produces. */
static JSValue dyn_sort_binary_search(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValueConst cmp;
    uint32_t len;

    (void)this_val;

    if (dyn_sort_get_cmp(ctx, argc, argv, 2, &cmp))
        return JS_EXCEPTION;
    if (dyn_sort_arr_len(ctx, argv[0], &len))
        return JS_EXCEPTION;

    if (JS_IsUndefined(cmp)) {
        /* Double path: materialise the array, then the key -- both may run JS. */
        double *buf, key;
        uint32_t lo, hi;
        int64_t found = -1;

        if (len == 0) {
            /* Coerce the key too, so a side-effecting valueOf behaves the same
             * as on the non-empty path, then report "absent". */
            if (JS_ToFloat64(ctx, &key, argv[1]))
                return JS_EXCEPTION;
            return JS_NewInt32(ctx, -1);
        }
        buf = dyn_sort_read_doubles(ctx, argv[0], len);
        if (!buf)
            return JS_EXCEPTION;
        if (JS_ToFloat64(ctx, &key, argv[1])) {
            js_free(ctx, buf);
            return JS_EXCEPTION;
        }
        lo = 0;
        hi = len;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            int c = dyn_sort_cmp_double(buf[mid], key);
            if (c < 0)
                lo = mid + 1;
            else if (c > 0)
                hi = mid;
            else {
                found = (int64_t)mid;
                break;
            }
        }
        js_free(ctx, buf);
        return JS_NewInt64(ctx, found);
    } else {
        /* Comparator path: snapshot elements, binary search via user JS. */
        JSValue *elems;
        int64_t idx = -1;
        int rc;
        uint32_t i;

        if (len == 0)
            return JS_NewInt32(ctx, -1);
        elems = dyn_sort_read_values(ctx, argv[0], len);
        if (!elems)
            return JS_EXCEPTION;
        rc = dyn_sort_bsearch_cmp(ctx, cmp, elems, len, argv[1], &idx);
        for (i = 0; i < len; i++)
            JS_FreeValue(ctx, elems[i]);
        js_free(ctx, elems);
        if (rc < 0)
            return JS_EXCEPTION; /* comparator threw */
        return JS_NewInt64(ctx, rc ? idx : -1);
    }
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry dyn_sort_funcs[] = {
    JS_CFUNC_DEF("sort", 1, dyn_sort_sort),
    JS_CFUNC_DEF("binarySearch", 2, dyn_sort_binary_search),
};

static int dyn_sort_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_sort_funcs,
                                  countof(dyn_sort_funcs));
}

int js_nat_init_sort(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dynajs:sort", dyn_sort_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_sort_funcs,
                                  countof(dyn_sort_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SORT */
