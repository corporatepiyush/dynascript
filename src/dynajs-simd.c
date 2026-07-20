/*
 * scl:simd -- vectorized float kernels over TypedArrays, exposed to JS.
 *
 *   import { dot, sum, scale, axpy, add } from "scl:simd";
 *   const a = new Float32Array([1,2,3,4]), b = new Float32Array([5,6,7,8]);
 *   dot(a, b);          // 70            (sum of a[i]*b[i])
 *   sum(a);             // 10
 *   scale(a, 2);        // a *= 2, returns a
 *   axpy(y, 3, x);      // y += 3*x, returns y
 *   add(out, a, b);     // out = a + b, returns out
 *
 * All operands are Float32Array; kernels run zero-copy on the backing buffer
 * (no per-element JS<->C conversion). On arm64 they use NEON intrinsics with an
 * FMA accumulate (a scalar tail handles the remainder); elsewhere a portable
 * loop that the compiler auto-vectorizes. Note: SIMD/FMA reductions round
 * slightly differently from a naive scalar sum -- this is an explicit,
 * opt-in numeric kernel, not the implicit bit-exact path.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

#include <stddef.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Kernels come from the shared multi-ISA dispatch table (`simd`, installed once
 * by simd_init() at runtime startup): NEON/AVX2/AVX-512/SVE where available,
 * scalar otherwise. This is the same facility the engine core uses. */
#include "dynajs-simd-kernels.h"

/* ---------- JS <-> float32 backing buffer at the boundary ---------- */

/* Resolve a Float32Array argument to its backing float* and element count.
 * Returns 0 on success. Throws (and returns -1) for a non-typed-array, a
 * non-4-byte element type, or a detached buffer. The pointer is valid only for
 * the synchronous duration of the call (no JS runs before the kernel). */
static int simd_get_f32(JSContext *ctx, JSValueConst v, float **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 4) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a Float32Array");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base) /* detached */
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = (float *)(base + off);
    *pn = len / 4;
    return 0;
}

static JSValue js_simd_dot(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (na != nb)
        return JS_ThrowRangeError(ctx, "dot: length mismatch");
    return JS_NewFloat64(ctx, simd.dot(a, b, na));
}

static JSValue js_simd_sum(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, simd.sum(a, n));
}

static JSValue js_simd_scale(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    double k;
    (void)this_val; (void)argc;
    /* coerce the scalar to a C local FIRST (may run user valueOf), then resolve
     * the buffer -- no JS between resolve and the kernel. */
    if (JS_ToFloat64(ctx, &k, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.mul_s(a, a, (float)k, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_axpy(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *y, *x;
    size_t ny, nx;
    double alpha;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &y, &ny) || simd_get_f32(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    if (ny != nx)
        return JS_ThrowRangeError(ctx, "axpy: length mismatch");
    simd.axpy(y, (float)alpha, x, ny);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_add(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *a, *b;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "add: length mismatch");
    simd.add(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

static const JSCFunctionListEntry js_simd_funcs[] = {
    JS_CFUNC_DEF("dot", 2, js_simd_dot),
    JS_CFUNC_DEF("sum", 1, js_simd_sum),
    JS_CFUNC_DEF("scale", 2, js_simd_scale),
    JS_CFUNC_DEF("axpy", 3, js_simd_axpy),
    JS_CFUNC_DEF("add", 3, js_simd_add),
};

static int js_simd_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, js_simd_funcs, countof(js_simd_funcs));
}

int js_nat_init_simd(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:simd", js_simd_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, js_simd_funcs, countof(js_simd_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SIMD */
