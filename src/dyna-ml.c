/*
 * dyna:ml -- native machine learning, self-contained and in-repo (no external
 * deps). A from-scratch replacement for the old secure-c-libs binding, with the
 * exact same JS API:
 *
 *   import { LinearRegression, LogisticRegression, KMeans } from "dyna:ml";
 *   const m = new LinearRegression();
 *   try { m.fit([[1],[2],[3]], [3,5,7]); print(m.predict([[4]])[0]); }
 *   finally { m.close(); }        // deterministic free
 *
 * Memory model (see dyna-nat.h): each model is one malloc-backed native struct
 * that owns its coefficient/centroid buffers; disposal frees them and the struct
 * (no arena, no GC tracing). JS array inputs are COPIED into short-lived,
 * contiguous C double buffers, the math runs in C, and results are COPIED back
 * into fresh JS Arrays at the boundary -- no native pointer ever escapes.
 *
 * Reentrancy discipline (critical): every method coerces ALL its JS array args
 * into C buffers FIRST, THEN resolves the native handle via dyn_res_native
 * (which throws if the model was closed), with no JS-invoking call between the
 * resolve and the native use. Reading a JS array can run user valueOf/Proxy code
 * that close()s `this`; resolving first would be a use-after-free.
 *
 * Algorithms: LinearRegression is closed-form OLS via the normal equations
 * (Gaussian elimination with partial pivoting, tiny ridge for conditioning);
 * LogisticRegression is full-batch gradient descent on the sigmoid; KMeans is
 * Lloyd's algorithm with a seeded k-means++ initialization.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_LOGREG_LR       0.1     /* gradient-descent step size */
#define DYN_LOGREG_ITERS    3000    /* full-batch iterations */
#define DYN_KMEANS_MAX_ITER 300     /* Lloyd iterations cap */
#define DYN_RIDGE           1e-9    /* diagonal load for OLS conditioning */

/* ---------- JS <-> C marshalling -------------------------------------------
 *
 * Two ingest paths for the training matrix X and vector y:
 *
 *   1. Array (of Array | of Float64Array):  each row is COPIED into a fresh,
 *      contiguous row-major double buffer (owned = free after use). A
 *      Float64Array row is bulk-memcpy'd from its backing buffer; a plain Array
 *      row is read cell-by-cell. Nothing native escapes.
 *
 *   2. Flat Float64Array + explicit (rows, cols):  the backing double buffer is
 *      aliased ZERO-COPY (owned = 0, never freed here) -- no per-cell JS crossing
 *      at all. Valid only for the synchronous span in which no JS runs, so the
 *      alias is taken as the LAST JS-touching step and the math runs with no JS
 *      call before the buffer is done being read (see the fit/predict methods).
 *
 * y is ALWAYS copied into an owned buffer (from a Float64Array it is a memcpy),
 * so it never leaves a dangling alias while a later arg's JS reads run. Reentrancy
 * rule still holds: all arg coercion precedes resolving the native handle.
 *
 * Detection: a Float64Array is any typed array with an 8-byte element (bpe == 8);
 * a BigInt64Array/BigUint64Array shares that width and would be misread (still
 * memory-safe, bounded by the buffer) -- callers must pass a Float64Array. This
 * mirrors dyna-simd.c, which treats any 4-byte typed array as Float32Array. */

typedef struct {
    double *data;   /* rows*cols row-major doubles */
    size_t rows, cols;
    int owned;      /* 1: malloc'd (free); 0: aliases a JS ArrayBuffer */
} dyn_matrix_t;

static void dyn_matrix_free(dyn_matrix_t *mx)
{
    if (mx->owned)
        free(mx->data);
    mx->data = NULL;
}

/* Resolve a Float64Array to its backing double* and element count. Returns 0, or
 * -1 (throwing) for a non-typed-array, a non-8-byte element type, a detached
 * buffer, or an out-of-bounds view. The pointer aliases the JS buffer and is
 * valid only while no JS runs. */
static int dyn_ml_get_f64(JSContext *ctx, JSValueConst v, double **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 8) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a Float64Array");
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
    *pp = (double *)(base + off);
    *pn = len / 8;
    return 0;
}

/* Length of a JS array into *out_len, or -1 (throws TypeError) if not an array. */
static int dyn_ml_len(JSContext *ctx, JSValueConst v, size_t *out_len)
{
    JSValue lval;
    uint32_t len;
    int ret;

    if (!JS_IsArray(ctx, v)) {
        JS_ThrowTypeError(ctx, "expected an Array");
        return -1;
    }
    lval = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(lval))
        return -1;
    ret = JS_ToUint32(ctx, &len, lval);
    JS_FreeValue(ctx, lval);
    if (ret)
        return -1;
    *out_len = len;
    return 0;
}

/* Read `n` numbers from JS array `arr` into out[0..n). 0 on success, -1 throws. */
static int dyn_ml_read_row(JSContext *ctx, JSValueConst arr, double *out,
                           size_t n)
{
    size_t j;
    for (j = 0; j < n; j++) {
        double x;
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)j);
        if (JS_IsException(v))
            return -1;
        if (JS_ToFloat64(ctx, &x, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
        out[j] = x;
    }
    return 0;
}

/* Element count of one X row (a plain Array or a Float64Array) into *out, or -1
 * (throwing). A Float64Array is not a JS Array, so JS_IsArray disambiguates. */
static int dyn_ml_row_len(JSContext *ctx, JSValueConst row, size_t *out)
{
    if (JS_IsArray(ctx, row))
        return dyn_ml_len(ctx, row, out);
    {
        double *rp;
        return dyn_ml_get_f64(ctx, row, &rp, out);
    }
}

/* Copy `cols` numbers of one X row into dst. A Float64Array row is a bulk memcpy
 * from its backing buffer (used and released with no JS in between); a plain
 * Array row is read cell-by-cell. Returns 0, or -1 (throwing). */
static int dyn_ml_read_row_generic(JSContext *ctx, JSValueConst row, double *dst,
                                    size_t cols)
{
    if (JS_IsArray(ctx, row)) {
        size_t rlen;
        if (dyn_ml_len(ctx, row, &rlen))
            return -1;
        if (rlen != cols) {
            JS_ThrowTypeError(ctx,
                "every row of X must have the same length");
            return -1;
        }
        return dyn_ml_read_row(ctx, row, dst, cols);
    }
    {
        double *rp;
        size_t rn;
        if (dyn_ml_get_f64(ctx, row, &rp, &rn))
            return -1;
        if (rn != cols) {
            JS_ThrowTypeError(ctx, "every row of X must have the same length");
            return -1;
        }
        memcpy(dst, rp, cols * sizeof(double));
        return 0;
    }
}

/* Ingest an Array-of-(Array|Float64Array) X into a fresh owned row-major buffer.
 * Sets mx->{data,rows,cols}, owned=1. -1 (throwing) on error. */
static int dyn_ml_ingest_matrix_array(JSContext *ctx, JSValueConst x,
                                      dyn_matrix_t *mx)
{
    size_t rows, cols, count, i;
    double *data;
    JSValue row0;
    int err;

    if (dyn_ml_len(ctx, x, &rows))
        return -1;
    if (rows == 0) {
        JS_ThrowTypeError(ctx, "X must have at least one row");
        return -1;
    }
    row0 = JS_GetPropertyUint32(ctx, x, 0);
    if (JS_IsException(row0))
        return -1;
    err = dyn_ml_row_len(ctx, row0, &cols);
    JS_FreeValue(ctx, row0);
    if (err)
        return -1;
    if (cols == 0) {
        JS_ThrowTypeError(ctx, "X rows must have at least one feature");
        return -1;
    }
    if (rows > SIZE_MAX / cols ||
        (count = rows * cols) > SIZE_MAX / sizeof(double)) {
        JS_ThrowRangeError(ctx, "X is too large");
        return -1;
    }
    data = (double *)malloc(count * sizeof(double));
    if (!data) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < rows; i++) {
        JSValue row = JS_GetPropertyUint32(ctx, x, (uint32_t)i);
        if (JS_IsException(row)) {
            free(data);
            return -1;
        }
        err = dyn_ml_read_row_generic(ctx, row, data + i * cols, cols);
        JS_FreeValue(ctx, row);
        if (err) {
            free(data);
            return -1;
        }
    }
    mx->data = data;
    mx->rows = rows;
    mx->cols = cols;
    mx->owned = 1;
    return 0;
}

/* Ingest a flat Float64Array X as a ZERO-COPY alias with explicit (rows, cols).
 * Sets mx->{data,rows,cols}, owned=0. The alias must be the last JS-touching
 * step before the math (no JS may run while it is held). -1 (throwing) on error. */
static int dyn_ml_ingest_matrix_flat(JSContext *ctx, JSValueConst x,
                                     size_t rows, size_t cols, dyn_matrix_t *mx)
{
    double *data;
    size_t total;

    if (rows == 0 || cols == 0) {
        JS_ThrowTypeError(ctx,
            "a flat Float64Array X requires positive (rows, cols)");
        return -1;
    }
    if (rows > SIZE_MAX / cols) {
        JS_ThrowRangeError(ctx, "X is too large");
        return -1;
    }
    if (dyn_ml_get_f64(ctx, x, &data, &total)) /* throws if not Float64Array */
        return -1;
    if (total != rows * cols) {
        JS_ThrowTypeError(ctx,
            "flat Float64Array length must equal rows*cols");
        return -1;
    }
    mx->data = data;
    mx->rows = rows;
    mx->cols = cols;
    mx->owned = 0;
    return 0;
}

/* Ingest y (an Array or a Float64Array) of exactly `expect` entries into a fresh
 * OWNED buffer (from a Float64Array: a memcpy). Sets *pout (caller frees). y is
 * never aliased, so no dangling view survives a later arg's JS reads. -1 throws. */
static int dyn_ml_ingest_vector(JSContext *ctx, JSValueConst y, size_t expect,
                                double **pout)
{
    size_t n;
    double *out;

    if (JS_IsArray(ctx, y)) {
        if (dyn_ml_len(ctx, y, &n))
            return -1;
        if (n != expect) {
            JS_ThrowTypeError(ctx,
                "y length must equal the number of rows in X");
            return -1;
        }
        out = (double *)malloc((n ? n : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (dyn_ml_read_row(ctx, y, out, n)) {
            free(out);
            return -1;
        }
    } else {
        double *src;
        if (dyn_ml_get_f64(ctx, y, &src, &n)) /* throws if not Float64Array */
            return -1;
        if (n != expect) {
            JS_ThrowTypeError(ctx,
                "y length must equal the number of rows in X");
            return -1;
        }
        out = (double *)malloc((n ? n : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(out, src, n * sizeof(double));
    }
    *pout = out;
    return 0;
}

/* Ingest X for a method that also takes a y vector (fit). Handles arg ordering so
 * that any zero-copy alias is taken AFTER every JS-running coercion:
 *   - Array X: read X (owned copy) FIRST, then y with expect = X.rows.
 *   - flat Float64Array X: coerce (rows, cols) and read y FIRST, then alias X.
 * `argv` are the method args; rc/cc are the indices of the rows/cols args used
 * only for the flat form. Sets *mx (caller dyn_matrix_free) and *py (caller
 * free). -1 (throwing) on error. */
static int dyn_ml_ingest_Xy(JSContext *ctx, JSValueConst xv, JSValueConst yv,
                            JSValueConst rows_arg, JSValueConst cols_arg,
                            dyn_matrix_t *mx, double **py)
{
    if (JS_IsArray(ctx, xv)) {
        if (dyn_ml_ingest_matrix_array(ctx, xv, mx))
            return -1;
        if (dyn_ml_ingest_vector(ctx, yv, mx->rows, py)) {
            dyn_matrix_free(mx);
            return -1;
        }
        return 0;
    } else {
        int64_t rows64, cols64;
        if (JS_ToInt64(ctx, &rows64, rows_arg) ||
            JS_ToInt64(ctx, &cols64, cols_arg))
            return -1;
        if (rows64 <= 0 || cols64 <= 0) {
            JS_ThrowTypeError(ctx,
                "flat Float64Array X requires positive (rows, cols) args");
            return -1;
        }
        if (dyn_ml_ingest_vector(ctx, yv, (size_t)rows64, py))
            return -1;
        if (dyn_ml_ingest_matrix_flat(ctx, xv, (size_t)rows64,
                                      (size_t)cols64, mx)) {
            free(*py);
            *py = NULL;
            return -1;
        }
        return 0;
    }
}

/* Ingest X for a method with no y (predict / kmeans.fit). For a flat
 * Float64Array, (rows, cols) come from rows_arg/cols_arg. Sets *mx (caller
 * dyn_matrix_free). -1 (throwing) on error. */
static int dyn_ml_ingest_X(JSContext *ctx, JSValueConst xv,
                           JSValueConst rows_arg, JSValueConst cols_arg,
                           dyn_matrix_t *mx)
{
    if (JS_IsArray(ctx, xv))
        return dyn_ml_ingest_matrix_array(ctx, xv, mx);
    {
        int64_t rows64, cols64;
        if (JS_ToInt64(ctx, &rows64, rows_arg) ||
            JS_ToInt64(ctx, &cols64, cols_arg))
            return -1;
        if (rows64 <= 0 || cols64 <= 0) {
            JS_ThrowTypeError(ctx,
                "flat Float64Array X requires positive (rows, cols) args");
            return -1;
        }
        return dyn_ml_ingest_matrix_flat(ctx, xv, (size_t)rows64,
                                         (size_t)cols64, mx);
    }
}

/* Fresh JS Array copied from a native double buffer. */
static JSValue dyn_ml_doubles_to_js(JSContext *ctx, const double *v, size_t n)
{
    size_t i;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        if (JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                                 JS_NewFloat64(ctx, v[i])) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

/* Fresh JS Array copied from a native int buffer. */
static JSValue dyn_ml_ints_to_js(JSContext *ctx, const int *v, size_t n)
{
    size_t i;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        if (JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                                 JS_NewInt32(ctx, v[i])) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

/* ---------- linear algebra: solve (A w = b) in place, A is p*p row-major ----- */

/* Gaussian elimination with partial pivoting. On return b holds the solution.
 * Returns 0, or -1 if the system is singular (near-zero pivot). */
static int dyn_solve(double *A, double *b, size_t p)
{
    size_t col, r, c, pivot;

    for (col = 0; col < p; col++) {
        double maxv = fabs(A[col * p + col]);
        pivot = col;
        for (r = col + 1; r < p; r++) {
            double v = fabs(A[r * p + col]);
            if (v > maxv) {
                maxv = v;
                pivot = r;
            }
        }
        if (maxv < 1e-300)
            return -1;
        if (pivot != col) {
            for (c = 0; c < p; c++) {
                double t = A[col * p + c];
                A[col * p + c] = A[pivot * p + c];
                A[pivot * p + c] = t;
            }
            double tb = b[col];
            b[col] = b[pivot];
            b[pivot] = tb;
        }
        for (r = col + 1; r < p; r++) {
            double f = A[r * p + col] / A[col * p + col];
            for (c = col; c < p; c++)
                A[r * p + c] -= f * A[col * p + c];
            b[r] -= f * b[col];
        }
    }
    for (r = p; r-- > 0;) {
        double s = b[r];
        for (c = r + 1; c < p; c++)
            s -= A[r * p + c] * b[c];
        b[r] = s / A[r * p + r];
    }
    return 0;
}

/* ---------- LinearRegression: closed-form OLS (normal equations) ------------ */

typedef struct {
    int fitted;
    size_t n_features;
    double *coef;      /* n_features weights */
    double intercept;
} dyn_linreg_t;

static JSClassID dyn_linreg_class_id;

static void dyn_linreg_dispose(void *native)
{
    dyn_linreg_t *m = (dyn_linreg_t *)native;
    if (m) {
        free(m->coef);
        free(m);
    }
}

static const JSClassDef dyn_linreg_class = {
    "LinearRegression",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_linreg_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_linreg_t *m;

    (void)new_target; (void)argc; (void)argv;
    m = (dyn_linreg_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_res_wrap(ctx, dyn_linreg_class_id, m, dyn_linreg_dispose);
}

/* Solve OLS with an intercept: design column p-1 is the constant 1. Stores the
 * fitted coef/intercept into `m`. Returns 0, or -1 (throwing) on OOM/singular. */
static int dyn_linreg_solve(JSContext *ctx, dyn_linreg_t *m, const double *X,
                            const double *y, size_t rows, size_t cols)
{
    size_t p = cols + 1;
    double *AtA, *Aty, *coef;
    size_t i, a, bcol;

    AtA = (double *)calloc(p * p, sizeof(double));
    Aty = (double *)calloc(p, sizeof(double));
    coef = (double *)malloc(cols * sizeof(double));
    if (!AtA || !Aty || !coef) {
        free(AtA); free(Aty); free(coef);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    /* Accumulate A^T A and A^T y where each design row is [x_i..., 1]. */
    for (i = 0; i < rows; i++) {
        const double *xi = X + i * cols;
        for (a = 0; a < p; a++) {
            double va = (a < cols) ? xi[a] : 1.0;
            Aty[a] += va * y[i];
            for (bcol = a; bcol < p; bcol++) {
                double vb = (bcol < cols) ? xi[bcol] : 1.0;
                AtA[a * p + bcol] += va * vb;
            }
        }
    }
    /* Mirror the symmetric upper triangle and add a tiny ridge for stability. */
    for (a = 0; a < p; a++) {
        for (bcol = a + 1; bcol < p; bcol++)
            AtA[bcol * p + a] = AtA[a * p + bcol];
        AtA[a * p + a] += DYN_RIDGE;
    }
    if (dyn_solve(AtA, Aty, p)) {
        free(AtA); free(Aty); free(coef);
        JS_ThrowInternalError(ctx, "LinearRegression: singular system");
        return -1;
    }
    for (a = 0; a < cols; a++)
        coef[a] = Aty[a];
    free(m->coef);
    m->coef = coef;
    m->intercept = Aty[cols];
    m->n_features = cols;
    m->fitted = 1;
    free(AtA);
    free(Aty);
    return 0;
}

static JSValue dyn_linreg_fit(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_linreg_t *m;
    dyn_matrix_t mx = {0};
    double *y = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fit(X, y) requires two arguments");
    /* For a flat Float64Array X, fit(X, y, rows, cols). Guard the optional
     * shape args by argc (argv is only padded up to the declared .length). */
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    /* Coerce ALL args to C buffers BEFORE resolving the handle (any zero-copy
     * alias is taken last, with no JS between it and the math below). */
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg, &mx, &y))
        return JS_EXCEPTION;
    m = (dyn_linreg_t *)dyn_res_native(ctx, this_val, dyn_linreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    if (dyn_linreg_solve(ctx, m, mx.data, y, mx.rows, mx.cols)) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_linreg_predict(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_linreg_t *m;
    dyn_matrix_t mx = {0};
    double *yout = NULL;
    size_t i, j;
    JSValue result;
    JSValueConst rows_arg, cols_arg;

    rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_linreg_t *)dyn_res_native(ctx, this_val, dyn_linreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "X has %u features, model expects %u",
            (unsigned)mx.cols, (unsigned)m->n_features);
    }
    yout = (double *)malloc(mx.rows * sizeof(double));
    if (!yout) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < mx.rows; i++) {
        double acc = m->intercept;
        const double *xi = mx.data + i * mx.cols;
        for (j = 0; j < mx.cols; j++)
            acc += m->coef[j] * xi[j];
        yout[i] = acc;
    }
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    result = dyn_ml_doubles_to_js(ctx, yout, mx.rows);
    free(yout);
    return result;
}

static const JSCFunctionListEntry dyn_linreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_linreg_fit),
    JS_CFUNC_DEF("predict", 1, dyn_linreg_predict),
};

/* ---------- LogisticRegression: binary classifier (gradient descent) -------- */

typedef struct {
    int fitted;
    size_t n_features;
    double *coef;      /* n_features weights */
    double intercept;
} dyn_logreg_t;

static JSClassID dyn_logreg_class_id;

static void dyn_logreg_dispose(void *native)
{
    dyn_logreg_t *m = (dyn_logreg_t *)native;
    if (m) {
        free(m->coef);
        free(m);
    }
}

static const JSClassDef dyn_logreg_class = {
    "LogisticRegression",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_logreg_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_logreg_t *m;

    (void)new_target; (void)argc; (void)argv;
    m = (dyn_logreg_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_res_wrap(ctx, dyn_logreg_class_id, m, dyn_logreg_dispose);
}

static inline double dyn_sigmoid(double z)
{
    if (z >= 0.0) {
        double e = exp(-z);
        return 1.0 / (1.0 + e);
    } else {
        double e = exp(z);
        return e / (1.0 + e);
    }
}

/* Full-batch gradient descent on the mean cross-entropy. Labels are read as 0/1
 * (any y != 0 counts as class 1). Returns 0, or -1 (throwing) on OOM. */
static int dyn_logreg_train(JSContext *ctx, dyn_logreg_t *m, const double *X,
                            const double *y, size_t rows, size_t cols)
{
    double *w, *gw;
    double b = 0.0;
    size_t i, j, it;

    w = (double *)calloc(cols, sizeof(double));
    gw = (double *)malloc(cols * sizeof(double));
    if (!w || !gw) {
        free(w); free(gw);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (it = 0; it < DYN_LOGREG_ITERS; it++) {
        double gb = 0.0;
        for (j = 0; j < cols; j++)
            gw[j] = 0.0;
        for (i = 0; i < rows; i++) {
            const double *xi = X + i * cols;
            double z = b;
            double target, err;
            for (j = 0; j < cols; j++)
                z += w[j] * xi[j];
            target = (y[i] != 0.0) ? 1.0 : 0.0;
            err = dyn_sigmoid(z) - target;
            for (j = 0; j < cols; j++)
                gw[j] += err * xi[j];
            gb += err;
        }
        for (j = 0; j < cols; j++)
            w[j] -= DYN_LOGREG_LR * gw[j] / (double)rows;
        b -= DYN_LOGREG_LR * gb / (double)rows;
    }
    free(gw);
    free(m->coef);
    m->coef = w;
    m->intercept = b;
    m->n_features = cols;
    m->fitted = 1;
    return 0;
}

static JSValue dyn_logreg_fit(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_logreg_t *m;
    dyn_matrix_t mx = {0};
    double *y = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg, &mx, &y))
        return JS_EXCEPTION;
    m = (dyn_logreg_t *)dyn_res_native(ctx, this_val, dyn_logreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    if (dyn_logreg_train(ctx, m, mx.data, y, mx.rows, mx.cols)) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    return JS_DupValue(ctx, this_val);
}

/* Shared body: predict class labels (proba=0) or probabilities (proba=1).
 * For a flat Float64Array X, predict(X, rows, cols). */
static JSValue dyn_logreg_predict_impl(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int proba)
{
    dyn_logreg_t *m;
    dyn_matrix_t mx = {0};
    double *yout = NULL;
    size_t rows, i, j;
    JSValue result;
    JSValueConst rows_arg, cols_arg;

    rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_logreg_t *)dyn_res_native(ctx, this_val, dyn_logreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "X has %u features, model expects %u",
            (unsigned)mx.cols, (unsigned)m->n_features);
    }
    rows = mx.rows;
    yout = (double *)malloc(rows * sizeof(double));
    if (!yout) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        double z = m->intercept;
        const double *xi = mx.data + i * mx.cols;
        for (j = 0; j < mx.cols; j++)
            z += m->coef[j] * xi[j];
        /* sigmoid(z) > 0.5 <=> z > 0, so labels avoid a redundant exp(). */
        yout[i] = proba ? dyn_sigmoid(z) : (z > 0.0 ? 1.0 : 0.0);
    }
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    result = dyn_ml_doubles_to_js(ctx, yout, rows);
    free(yout);
    return result;
}

static JSValue dyn_logreg_predict(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 0);
}

static JSValue dyn_logreg_predict_proba(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 1);
}

static const JSCFunctionListEntry dyn_logreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_logreg_fit),
    JS_CFUNC_DEF("predict", 1, dyn_logreg_predict),
    JS_CFUNC_DEF("predictProba", 1, dyn_logreg_predict_proba),
};

/* ---------- KMeans: Lloyd's algorithm with seeded k-means++ init ------------ */

typedef struct {
    int fitted;
    size_t k;
    size_t n_features;
    uint64_t seed;
    double *centroids; /* k * n_features */
    double inertia;
} dyn_kmeans_t;

static JSClassID dyn_kmeans_class_id;

static void dyn_kmeans_dispose(void *native)
{
    dyn_kmeans_t *m = (dyn_kmeans_t *)native;
    if (m) {
        free(m->centroids);
        free(m);
    }
}

static const JSClassDef dyn_kmeans_class = {
    "KMeans",
    .finalizer = dyn_res_finalizer,
};

/* new KMeans(nClusters = 8, seed = -1) -- seed < 0 uses a fixed deterministic
 * value so clustering is reproducible (matching the binding's default arg). */
static JSValue dyn_kmeans_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_kmeans_t *m;
    size_t k = 8;
    uint64_t seed = 0x9e3779b97f4a7c15ULL;

    (void)new_target;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        int32_t kv;
        if (JS_ToInt32(ctx, &kv, argv[0]))
            return JS_EXCEPTION;
        if (kv < 1)
            return JS_ThrowRangeError(ctx, "nClusters must be >= 1");
        k = (size_t)kv;
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        int32_t sv;
        if (JS_ToInt32(ctx, &sv, argv[1]))
            return JS_EXCEPTION;
        if (sv >= 0)
            seed = (uint64_t)sv + 0x9e3779b97f4a7c15ULL;
    }
    m = (dyn_kmeans_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->k = k;
    m->seed = seed;
    return dyn_res_wrap(ctx, dyn_kmeans_class_id, m, dyn_kmeans_dispose);
}

static uint64_t dyn_km_rng(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static double dyn_km_sqdist(const double *a, const double *b, size_t d)
{
    double s = 0.0;
    size_t j;
    for (j = 0; j < d; j++) {
        double t = a[j] - b[j];
        s += t * t;
    }
    return s;
}

/* Assign every point to its nearest centroid; return summed squared distance. */
static double dyn_km_assign(const double *X, size_t rows, size_t cols,
                            const double *cent, size_t k, int *labels)
{
    double total = 0.0;
    size_t i, c;
    for (i = 0; i < rows; i++) {
        const double *xi = X + i * cols;
        double best = dyn_km_sqdist(xi, cent, cols);
        int bl = 0;
        for (c = 1; c < k; c++) {
            double d = dyn_km_sqdist(xi, cent + c * cols, cols);
            if (d < best) {
                best = d;
                bl = (int)c;
            }
        }
        if (labels)
            labels[i] = bl;
        total += best;
    }
    return total;
}

/* k-means++ seeding into `cent` (k*cols). `nearest` scratch holds each point's
 * squared distance to the closest chosen centroid so far. */
static void dyn_km_plusplus(const double *X, size_t rows, size_t cols,
                            size_t k, double *cent, double *nearest,
                            uint64_t *rng)
{
    size_t c, i;
    size_t first = (size_t)(dyn_km_rng(rng) % rows);
    memcpy(cent, X + first * cols, cols * sizeof(double));
    for (i = 0; i < rows; i++)
        nearest[i] = dyn_km_sqdist(X + i * cols, cent, cols);
    for (c = 1; c < k; c++) {
        double sum = 0.0, target;
        size_t chosen = 0;
        for (i = 0; i < rows; i++)
            sum += nearest[i];
        if (sum <= 0.0) {
            /* All remaining points coincide with chosen centroids: pick any. */
            chosen = (size_t)(dyn_km_rng(rng) % rows);
        } else {
            target = ((double)(dyn_km_rng(rng) >> 11) *
                      (1.0 / 9007199254740992.0)) * sum;
            for (i = 0; i < rows; i++) {
                target -= nearest[i];
                if (target <= 0.0) {
                    chosen = i;
                    break;
                }
                chosen = i;
            }
        }
        memcpy(cent + c * cols, X + chosen * cols, cols * sizeof(double));
        for (i = 0; i < rows; i++) {
            double d = dyn_km_sqdist(X + i * cols, cent + c * cols, cols);
            if (d < nearest[i])
                nearest[i] = d;
        }
    }
}

/* Lloyd's algorithm. Stores centroids + inertia into `m`. -1 (throwing) on OOM
 * or on fewer rows than clusters. */
static int dyn_kmeans_train(JSContext *ctx, dyn_kmeans_t *m, const double *X,
                            size_t rows, size_t cols)
{
    size_t k = m->k, i, j, c, it;
    double *cent = NULL, *sums = NULL, *nearest = NULL;
    int *labels = NULL, *prev = NULL;
    size_t *counts = NULL;
    uint64_t rng = m->seed;

    if (rows < k)
        return JS_ThrowRangeError(ctx,
            "KMeans needs at least nClusters rows"), -1;
    cent = (double *)malloc(k * cols * sizeof(double));
    sums = (double *)malloc(k * cols * sizeof(double));
    nearest = (double *)malloc(rows * sizeof(double));
    labels = (int *)malloc(rows * sizeof(int));
    prev = (int *)malloc(rows * sizeof(int));
    counts = (size_t *)malloc(k * sizeof(size_t));
    if (!cent || !sums || !nearest || !labels || !prev || !counts) {
        free(cent); free(sums); free(nearest);
        free(labels); free(prev); free(counts);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    dyn_km_plusplus(X, rows, cols, k, cent, nearest, &rng);
    for (i = 0; i < rows; i++)
        prev[i] = -1;
    for (it = 0; it < DYN_KMEANS_MAX_ITER; it++) {
        int changed = 0;
        dyn_km_assign(X, rows, cols, cent, k, labels);
        for (i = 0; i < rows; i++)
            if (labels[i] != prev[i]) {
                changed = 1;
                break;
            }
        if (!changed && it > 0)
            break;
        /* Recompute centroids as the mean of assigned points. */
        memset(sums, 0, k * cols * sizeof(double));
        for (c = 0; c < k; c++)
            counts[c] = 0;
        for (i = 0; i < rows; i++) {
            const double *xi = X + i * cols;
            int lb = labels[i];
            counts[lb]++;
            for (j = 0; j < cols; j++)
                sums[(size_t)lb * cols + j] += xi[j];
        }
        for (c = 0; c < k; c++) {
            if (counts[c] == 0) {
                /* Empty cluster: reseed to a pseudo-random point. */
                size_t r = (size_t)(dyn_km_rng(&rng) % rows);
                memcpy(cent + c * cols, X + r * cols, cols * sizeof(double));
            } else {
                for (j = 0; j < cols; j++)
                    cent[c * cols + j] = sums[c * cols + j] / (double)counts[c];
            }
        }
        memcpy(prev, labels, rows * sizeof(int));
    }
    m->inertia = dyn_km_assign(X, rows, cols, cent, k, labels);
    free(m->centroids);
    m->centroids = cent;
    m->n_features = cols;
    m->fitted = 1;
    free(sums); free(nearest); free(labels); free(prev); free(counts);
    return 0;
}

static JSValue dyn_kmeans_fit(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_kmeans_t *m;
    dyn_matrix_t mx = {0};
    JSValueConst rows_arg, cols_arg;

    /* For a flat Float64Array X, fit(X, rows, cols). */
    rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_kmeans_t *)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (dyn_kmeans_train(ctx, m, mx.data, mx.rows, mx.cols)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    return JS_DupValue(ctx, this_val);
}

/* predict(X) -> Array<int> cluster labels. */
static JSValue dyn_kmeans_predict(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_kmeans_t *m;
    dyn_matrix_t mx = {0};
    int *labels = NULL;
    size_t rows;
    JSValue result;
    JSValueConst rows_arg, cols_arg;

    rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_kmeans_t *)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "X has %u features, model expects %u",
            (unsigned)mx.cols, (unsigned)m->n_features);
    }
    rows = mx.rows;
    labels = (int *)malloc(rows * sizeof(int));
    if (!labels) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_km_assign(mx.data, rows, mx.cols, m->centroids, m->k, labels);
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    result = dyn_ml_ints_to_js(ctx, labels, rows);
    free(labels);
    return result;
}

/* Sum of squared point-to-centroid distances of the last fit (0 before fit). */
static JSValue dyn_kmeans_inertia(JSContext *ctx, JSValueConst this_val)
{
    dyn_kmeans_t *m =
        (dyn_kmeans_t *)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->fitted ? m->inertia : 0.0);
}

static const JSCFunctionListEntry dyn_kmeans_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_kmeans_fit),
    JS_CFUNC_DEF("predict", 1, dyn_kmeans_predict),
    JS_CGETSET_DEF("inertia", dyn_kmeans_inertia, NULL),
};

/* ---------- module registration -------------------------------------------- */

static int dyn_ml_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_linreg_class_id, &dyn_linreg_class,
                           dyn_linreg_proto, countof(dyn_linreg_proto),
                           dyn_linreg_ctor, "LinearRegression") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_logreg_class_id, &dyn_logreg_class,
                           dyn_logreg_proto, countof(dyn_logreg_proto),
                           dyn_logreg_ctor, "LogisticRegression") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_kmeans_class_id, &dyn_kmeans_class,
                           dyn_kmeans_proto, countof(dyn_kmeans_proto),
                           dyn_kmeans_ctor, "KMeans") < 0)
        return -1;
    return 0;
}

int js_nat_init_ml(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:ml", dyn_ml_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "LinearRegression");
    JS_AddModuleExport(ctx, m, "LogisticRegression");
    JS_AddModuleExport(ctx, m, "KMeans");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_ML */
