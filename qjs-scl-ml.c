/*
 * scl:ml -- native machine learning backed by secure-c-libs.
 *
 *   import { LinearRegression, LogisticRegression, KMeans } from "scl:ml";
 *   const m = new LinearRegression();
 *   try { m.fit([[1],[2],[3]], [2,4,6]); print(m.predict([[4]])[0]); }
 *   finally { m.close(); }        // deterministic free (arena destroyed)
 *
 * Memory model (see qjs-scl.h): each model owns a private SCL arena that backs
 * the native model handle and its internal scratch allocator; disposing it is a
 * single arena destroy. JS array inputs are COPIED into a short-lived per-call
 * arena, wrapped as an SCL dataset, and the native predictions are COPIED back
 * into fresh JS Arrays -- no arena pointer ever escapes into the JS heap. The
 * per-call arena is destroyed before returning, so repeated fit()/predict()
 * does not grow the model arena.
 */
#include "qjs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_ml.h"
#include "scl_ml_linear.h"
#include "scl_ml_logistic.h"
#include "scl_ml_kmeans.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------- JS <-> SCL marshalling (all copies; nothing native escapes) ----- */

/* Length of a JS array, or -1 (throws TypeError) if `v` is not an array. */
static int js_scl_ml_len(JSContext *ctx, JSValueConst v, size_t *out_len)
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
static int js_scl_ml_read_row(JSContext *ctx, JSValueConst arr,
                              SCL_ML_FLOAT *out, size_t n)
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
        out[j] = (SCL_ML_FLOAT)x;
    }
    return 0;
}

/* Copy JS matrix X (Array of Array of number) into a fresh row-major buffer
 * (tightly packed) allocated from `a`. Sets pdata, prows, pcols. -1 throws. */
static int js_scl_ml_read_matrix(JSContext *ctx, scl_allocator_t *a,
                                 JSValueConst x, SCL_ML_FLOAT **pdata,
                                 size_t *prows, size_t *pcols)
{
    size_t rows, cols, count, bytes, i;
    SCL_ML_FLOAT *data;
    JSValue row0;
    int err;

    if (js_scl_ml_len(ctx, x, &rows))
        return -1;
    if (rows == 0) {
        JS_ThrowTypeError(ctx, "X must have at least one row");
        return -1;
    }
    row0 = JS_GetPropertyUint32(ctx, x, 0);
    if (JS_IsException(row0))
        return -1;
    err = js_scl_ml_len(ctx, row0, &cols);
    JS_FreeValue(ctx, row0);
    if (err)
        return -1;
    if (cols == 0) {
        JS_ThrowTypeError(ctx, "X rows must have at least one feature");
        return -1;
    }
    if (scl_mul_overflow(rows, cols, &count) ||
        scl_mul_overflow(count, sizeof(SCL_ML_FLOAT), &bytes)) {
        JS_ThrowRangeError(ctx, "X is too large");
        return -1;
    }
    data = (SCL_ML_FLOAT *)scl_alloc(a, bytes, SCL_ML_ALIGNMENT);
    if (!data) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < rows; i++) {
        size_t rlen;
        JSValue row = JS_GetPropertyUint32(ctx, x, (uint32_t)i);
        if (JS_IsException(row))
            return -1;
        if (js_scl_ml_len(ctx, row, &rlen)) {
            JS_FreeValue(ctx, row);
            return -1;
        }
        if (rlen != cols) {
            JS_FreeValue(ctx, row);
            JS_ThrowTypeError(ctx, "every row of X must have the same length");
            return -1;
        }
        err = js_scl_ml_read_row(ctx, row, data + i * cols, cols);
        JS_FreeValue(ctx, row);
        if (err)
            return -1;
    }
    *pdata = data;
    *prows = rows;
    *pcols = cols;
    return 0;
}

/* Copy JS vector y (Array of number) of exactly `expect` entries into a fresh
 * buffer from `a`. -1 throws. (expect <= UINT32_MAX so *4 cannot overflow.) */
static int js_scl_ml_read_vector(JSContext *ctx, scl_allocator_t *a,
                                 JSValueConst y, size_t expect,
                                 SCL_ML_FLOAT **pout)
{
    size_t n;
    SCL_ML_FLOAT *out;

    if (js_scl_ml_len(ctx, y, &n))
        return -1;
    if (n != expect) {
        JS_ThrowTypeError(ctx, "y length must equal the number of rows in X");
        return -1;
    }
    out = (SCL_ML_FLOAT *)scl_alloc(a, n * sizeof(SCL_ML_FLOAT), SCL_ML_ALIGNMENT);
    if (!out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (js_scl_ml_read_row(ctx, y, out, n))
        return -1;
    *pout = out;
    return 0;
}

/* Build a wrapped, finiteness-validated dataset in `a` from X (+ optional y).
 * The buffers live in `a`; the lazy column-major view is intentionally NOT
 * built -- no bound algorithm reads it. -1 throws. */
static int js_scl_ml_build(JSContext *ctx, scl_allocator_t *a, JSValueConst x,
                           JSValueConst y, int has_y, scl_ml_dataset_t *ds)
{
    SCL_ML_FLOAT *data, *targets = NULL;
    size_t rows, cols;
    scl_error_t err;

    if (js_scl_ml_read_matrix(ctx, a, x, &data, &rows, &cols))
        return -1;
    if (has_y && js_scl_ml_read_vector(ctx, a, y, rows, &targets))
        return -1;
    if (scl_ml_dataset_wrap(ds, data, targets, rows, cols) != SCL_OK) {
        JS_ThrowInternalError(ctx, "dataset wrap failed");
        return -1;
    }
    err = scl_ml_dataset_prepare(ds, NULL); /* NaN/Inf check; no column view */
    if (err != SCL_OK) {
        JS_ThrowTypeError(ctx, "invalid data: %s", scl_error_string(err));
        return -1;
    }
    return 0;
}

/* Fresh JS Array copied from a native float buffer. */
static JSValue js_scl_ml_floats_to_js(JSContext *ctx, const SCL_ML_FLOAT *v,
                                      size_t n)
{
    size_t i;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        if (JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                                 JS_NewFloat64(ctx, (double)v[i])) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

/* Fresh JS Array copied from a native int buffer. */
static JSValue js_scl_ml_ints_to_js(JSContext *ctx, const int *v, size_t n)
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

/* ---------- shared fit / predict (uniform arena discipline in one place) ----- */

typedef enum { ML_LINREG, ML_LOGREG, ML_KMEANS } ml_kind_t;

static scl_error_t js_scl_ml_do_fit(ml_kind_t kind, void *model,
                                    const scl_ml_dataset_t *ds)
{
    switch (kind) {
    case ML_LINREG:
        return scl_ml_linear_fit((scl_ml_linear_t *)model, ds);
    case ML_LOGREG:
        return scl_ml_logistic_fit((scl_ml_logistic_t *)model, ds);
    case ML_KMEANS:
        return scl_ml_kmeans_fit((scl_ml_kmeans_t *)model, ds);
    }
    return SCL_ERR_INVALID_ARG;
}

/* fit(X[, y]) for every model. Returns `this` for chaining. */
static JSValue js_scl_ml_fit(JSContext *ctx, JSValueConst this_val,
                             JSClassID cid, ml_kind_t kind, int argc,
                             JSValueConst *argv)
{
    JSSclResource *r;
    int has_y = (kind != ML_KMEANS);
    scl_allocator_t *ta;
    scl_ml_dataset_t ds;
    scl_error_t err;

    if (has_y && argc < 2)
        return JS_ThrowTypeError(ctx, "fit(X, y) requires two arguments");
    ta = js_scl_arena_new(ctx);
    if (!ta)
        return JS_EXCEPTION;
    if (js_scl_ml_build(ctx, ta, argv[0], has_y ? argv[1] : JS_UNDEFINED,
                        has_y, &ds)) {
        scl_alloc_arena_destroy(ta);
        return JS_EXCEPTION;
    }
    /* resolve AFTER the build: reading the JS X/y arrays can run user JS that
       close()s this model; resource_get throws if so, before touching native */
    r = js_scl_resource_get(ctx, this_val, cid);
    if (!r) {
        scl_ml_dataset_destroy(&ds, ta);
        scl_alloc_arena_destroy(ta);
        return JS_EXCEPTION;
    }
    err = js_scl_ml_do_fit(kind, r->native, &ds);
    scl_ml_dataset_destroy(&ds, ta);
    scl_alloc_arena_destroy(ta);
    if (err != SCL_OK)
        return JS_ThrowInternalError(ctx, "fit failed: %s",
                                     scl_error_string(err));
    return JS_DupValue(ctx, this_val);
}

/* predict(X) for the real-valued models (linear/logistic, incl. proba). */
static JSValue js_scl_ml_predict_real(JSContext *ctx, JSValueConst this_val,
                                      JSClassID cid, ml_kind_t kind, int proba,
                                      JSValueConst x)
{
    JSSclResource *r;
    scl_allocator_t *ta;
    scl_ml_dataset_t ds;
    SCL_ML_FLOAT *yout;
    scl_error_t err;
    JSValue result;
    size_t n;

    ta = js_scl_arena_new(ctx);
    if (!ta)
        return JS_EXCEPTION;
    if (js_scl_ml_build(ctx, ta, x, JS_UNDEFINED, 0, &ds)) {
        scl_alloc_arena_destroy(ta);
        return JS_EXCEPTION;
    }
    /* resolve AFTER the build (which reads the JS X array and may close us) */
    r = js_scl_resource_get(ctx, this_val, cid);
    if (!r) {
        scl_ml_dataset_destroy(&ds, ta);
        scl_alloc_arena_destroy(ta);
        return JS_EXCEPTION;
    }
    n = ds.n_rows;
    yout = (SCL_ML_FLOAT *)scl_alloc(ta, n * sizeof(SCL_ML_FLOAT),
                                     SCL_ML_ALIGNMENT);
    if (!yout) {
        scl_ml_dataset_destroy(&ds, ta);
        scl_alloc_arena_destroy(ta);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (kind == ML_LINREG)
        err = scl_ml_linear_predict((scl_ml_linear_t *)r->native, &ds, yout);
    else if (proba)
        err = scl_ml_logistic_predict_proba((scl_ml_logistic_t *)r->native, &ds,
                                             yout);
    else
        err = scl_ml_logistic_predict((scl_ml_logistic_t *)r->native, &ds, yout);
    if (err != SCL_OK) {
        scl_ml_dataset_destroy(&ds, ta);
        scl_alloc_arena_destroy(ta);
        return JS_ThrowInternalError(ctx, "predict failed: %s",
                                     scl_error_string(err));
    }
    result = js_scl_ml_floats_to_js(ctx, yout, n); /* copy out before free */
    scl_ml_dataset_destroy(&ds, ta);
    scl_alloc_arena_destroy(ta);
    return result;
}

/* ---------- LinearRegression: closed-form OLS (normal equations) ------------ */

static JSClassID js_scl_linreg_class_id;

static void js_scl_linreg_dispose(void *native, scl_allocator_t *arena)
{
    (void)arena; /* framework destroys the arena after this returns */
    scl_ml_linear_free((scl_ml_linear_t *)native);
}

static const JSClassDef js_scl_linreg_class = {
    "LinearRegression",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_linreg_ctor(JSContext *ctx, JSValueConst new_target,
                                  int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_ml_linear_t *model = NULL;
    scl_ml_linear_params_t p = SCL_ML_LINEAR_PARAMS_DEFAULT();

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    p.alloc = arena;
    p.solver = SCL_ML_SOLVER_NORMAL_EQ; /* exact, deterministic OLS */
    if (scl_ml_linear_new(&model, p) != SCL_OK || !model) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_linreg_class_id, arena, model,
                                js_scl_linreg_dispose);
}

static JSValue js_scl_linreg_fit(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_scl_ml_fit(ctx, this_val, js_scl_linreg_class_id, ML_LINREG, argc,
                         argv);
}

static JSValue js_scl_linreg_predict(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    return js_scl_ml_predict_real(ctx, this_val, js_scl_linreg_class_id,
                                  ML_LINREG, 0, argv[0]);
}

static const JSCFunctionListEntry js_scl_linreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, js_scl_linreg_fit),
    JS_CFUNC_DEF("predict", 1, js_scl_linreg_predict),
};

/* ---------- LogisticRegression: binary classification (SGD) ----------------- */

static JSClassID js_scl_logreg_class_id;

static void js_scl_logreg_dispose(void *native, scl_allocator_t *arena)
{
    (void)arena;
    scl_ml_logistic_free((scl_ml_logistic_t *)native);
}

static const JSClassDef js_scl_logreg_class = {
    "LogisticRegression",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_logreg_ctor(JSContext *ctx, JSValueConst new_target,
                                  int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_ml_logistic_t *model = NULL;
    scl_ml_logistic_params_t p = SCL_ML_LOGISTIC_PARAMS_DEFAULT();

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    p.alloc = arena;
    if (scl_ml_logistic_new(&model, p) != SCL_OK || !model) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_logreg_class_id, arena, model,
                                js_scl_logreg_dispose);
}

static JSValue js_scl_logreg_fit(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_scl_ml_fit(ctx, this_val, js_scl_logreg_class_id, ML_LOGREG, argc,
                         argv);
}

static JSValue js_scl_logreg_predict(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    return js_scl_ml_predict_real(ctx, this_val, js_scl_logreg_class_id,
                                  ML_LOGREG, 0, argv[0]);
}

static JSValue js_scl_logreg_predict_proba(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    return js_scl_ml_predict_real(ctx, this_val, js_scl_logreg_class_id,
                                  ML_LOGREG, 1, argv[0]);
}

static const JSCFunctionListEntry js_scl_logreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, js_scl_logreg_fit),
    JS_CFUNC_DEF("predict", 1, js_scl_logreg_predict),
    JS_CFUNC_DEF("predictProba", 1, js_scl_logreg_predict_proba),
};

/* ---------- KMeans: unsupervised clustering (Lloyd) ------------------------- */

static JSClassID js_scl_kmeans_class_id;

static void js_scl_kmeans_dispose(void *native, scl_allocator_t *arena)
{
    (void)arena;
    scl_ml_kmeans_free((scl_ml_kmeans_t *)native);
}

static const JSClassDef js_scl_kmeans_class = {
    "KMeans",
    .finalizer = js_scl_finalizer,
};

/* new KMeans(nClusters = 8, seed = -1) */
static JSValue js_scl_kmeans_ctor(JSContext *ctx, JSValueConst new_target,
                                  int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_ml_kmeans_t *model = NULL;
    scl_ml_kmeans_params_t p = SCL_ML_KMEANS_PARAMS_DEFAULT();

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        int32_t k;
        if (JS_ToInt32(ctx, &k, argv[0]))
            return JS_EXCEPTION;
        if (k < 1)
            return JS_ThrowRangeError(ctx, "nClusters must be >= 1");
        p.n_clusters = (size_t)k;
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        int32_t seed;
        if (JS_ToInt32(ctx, &seed, argv[1]))
            return JS_EXCEPTION;
        p.random_seed = seed;
    }
    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    p.alloc = arena;
    if (scl_ml_kmeans_new(&model, p) != SCL_OK || !model) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_kmeans_class_id, arena, model,
                                js_scl_kmeans_dispose);
}

static JSValue js_scl_kmeans_fit(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_scl_ml_fit(ctx, this_val, js_scl_kmeans_class_id, ML_KMEANS, argc,
                         argv);
}

/* predict(X) -> Array<int> cluster labels (int output, so not shared). */
static JSValue js_scl_kmeans_predict(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSSclResource *r;
    scl_allocator_t *ta;
    scl_ml_dataset_t ds;
    int *labels;
    scl_error_t err;
    JSValue result;
    size_t n;

    ta = js_scl_arena_new(ctx);
    if (!ta)
        return JS_EXCEPTION;
    if (js_scl_ml_build(ctx, ta, argv[0], JS_UNDEFINED, 0, &ds)) {
        scl_alloc_arena_destroy(ta);
        return JS_EXCEPTION;
    }
    /* resolve AFTER the build (which reads the JS X array and may close us) */
    r = js_scl_resource_get(ctx, this_val, js_scl_kmeans_class_id);
    if (!r) {
        scl_ml_dataset_destroy(&ds, ta);
        scl_alloc_arena_destroy(ta);
        return JS_EXCEPTION;
    }
    n = ds.n_rows;
    labels = (int *)scl_alloc(ta, n * sizeof(int), SCL_ML_ALIGNMENT);
    if (!labels) {
        scl_ml_dataset_destroy(&ds, ta);
        scl_alloc_arena_destroy(ta);
        return JS_ThrowOutOfMemory(ctx);
    }
    err = scl_ml_kmeans_predict((scl_ml_kmeans_t *)r->native, &ds, labels);
    if (err != SCL_OK) {
        scl_ml_dataset_destroy(&ds, ta);
        scl_alloc_arena_destroy(ta);
        return JS_ThrowInternalError(ctx, "predict failed: %s",
                                     scl_error_string(err));
    }
    result = js_scl_ml_ints_to_js(ctx, labels, n); /* copy out before free */
    scl_ml_dataset_destroy(&ds, ta);
    scl_alloc_arena_destroy(ta);
    return result;
}

/* Sum of squared point-to-centroid distances of the last fit (0 before fit). */
static JSValue js_scl_kmeans_inertia(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val, js_scl_kmeans_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewFloat64(
        ctx, (double)scl_ml_kmeans_get_inertia((scl_ml_kmeans_t *)r->native));
}

static const JSCFunctionListEntry js_scl_kmeans_proto[] = {
    JS_CFUNC_DEF("fit", 1, js_scl_kmeans_fit),
    JS_CFUNC_DEF("predict", 1, js_scl_kmeans_predict),
    JS_CGETSET_DEF("inertia", js_scl_kmeans_inertia, NULL),
};

/* ---------- module registration -------------------------------------------- */

static int register_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                          const JSClassDef *def,
                          const JSCFunctionListEntry *proto_funcs, int n_funcs,
                          JSCFunction *ctor_fn, const char *name)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, n_funcs);
    js_scl_class_common(ctx, *pid, proto);
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

static int js_scl_ml_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (register_class(ctx, m, &js_scl_linreg_class_id, &js_scl_linreg_class,
                       js_scl_linreg_proto, countof(js_scl_linreg_proto),
                       js_scl_linreg_ctor, "LinearRegression") < 0)
        return -1;
    if (register_class(ctx, m, &js_scl_logreg_class_id, &js_scl_logreg_class,
                       js_scl_logreg_proto, countof(js_scl_logreg_proto),
                       js_scl_logreg_ctor, "LogisticRegression") < 0)
        return -1;
    if (register_class(ctx, m, &js_scl_kmeans_class_id, &js_scl_kmeans_class,
                       js_scl_kmeans_proto, countof(js_scl_kmeans_proto),
                       js_scl_kmeans_ctor, "KMeans") < 0)
        return -1;
    return 0;
}

int js_scl_init_ml(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:ml", js_scl_ml_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "LinearRegression");
    JS_AddModuleExport(ctx, m, "LogisticRegression");
    JS_AddModuleExport(ctx, m, "KMeans");
    return 0;
}

#endif /* CONFIG_SCL_MODULES */
