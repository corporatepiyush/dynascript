/*
 * dynascript native modules -- self-contained, in-repo (no external deps).
 *
 * This is the shared framework every native `dynajs:*` module builds on. It is a
 * from-scratch, simplified replacement for the old secure-c-libs binding layer:
 * there is NO per-object arena and NO external allocator. A native resource is
 * just an opaque pointer plus a dispose callback that frees whatever the module
 * allocated (with plain libc malloc/free, which is thread-safe -- important for
 * modules like the HTTP server whose worker threads allocate concurrently).
 *
 * Memory model: a JS wrapper object owns one native pointer. JavaScript releases
 * it deterministically via .close()/.dispose()/[Symbol.dispose]; the class
 * finalizer is only a safety net for a leaked object. Native results are copied
 * into JS values at the call boundary -- no native pointer escapes into the JS
 * heap. Every method must coerce its JS args to C locals FIRST, then resolve the
 * native handle (dyn_res_get, which rejects a closed resource), with no
 * JS-invoking call in between (coercion can run user JS that close()s `this`).
 */
#ifndef DYNAJS_NAT_H
#define DYNAJS_NAT_H

#include "dynajs.h"

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>

/* Teardown callback a module supplies: free `native` and everything it owns. */
typedef void (*DynDisposeFunc)(void *native);

/* Opaque payload of every native-resource JS object. */
typedef struct {
    void *native;             /* the module's native object (module-owned) */
    DynDisposeFunc dispose;   /* module teardown; may be NULL */
    int closed;               /* 1 once released (idempotent) */
} DynResource;

/* Wrap `native` as a JS object of class `class_id` whose proto already carries
 * close()/[Symbol.dispose]/closed. Takes ownership: on any later disposal the
 * framework runs `dispose(native)`. On error runs `dispose(native)` and returns
 * JS_EXCEPTION (so the caller never double-frees). */
JSValue dyn_res_wrap(JSContext *ctx, JSClassID class_id, void *native,
                     DynDisposeFunc dispose);

/* Fetch the live resource for `this`, or throw (and return NULL) if the object
 * is closed or of the wrong class. */
DynResource *dyn_res_get(JSContext *ctx, JSValueConst this_val,
                         JSClassID class_id);

/* Convenience: resolve `this` to its live native pointer, or NULL (throwing). */
static inline void *dyn_res_native(JSContext *ctx, JSValueConst this_val,
                                   JSClassID class_id)
{
    DynResource *r = dyn_res_get(ctx, this_val, class_id);
    return r ? r->native : NULL;
}

/* Install close()/dispose()/[Symbol.dispose]/`closed` on `proto` and register
 * `class_id` as framework-owned. Call once per class from the module init. */
void dyn_res_class_common(JSContext *ctx, JSClassID class_id, JSValue proto);

/* The shared finalizer (disposes if still open). Reference it from JSClassDef. */
void dyn_res_finalizer(JSRuntime *rt, JSValue val);

/* Helper for classes: create id, class, proto (with common funcs + protos),
 * constructor, and export it from module `m`. Returns 0 or -1. */
int dyn_register_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                       const JSClassDef *def,
                       const JSCFunctionListEntry *proto_funcs, int n_funcs,
                       JSCFunction *ctor_fn, const char *name);

/* Per-family module initializers (each defined in its own translation unit).
 * A family is compiled in iff its CONFIG_NATIVE_MODULE_<X> flag is set. */
int js_nat_init_structures(JSContext *ctx);
int js_nat_init_structures_ext(JSContext *ctx);
#ifdef CONFIG_NATIVE_MODULE_HTTP
int js_nat_init_http(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_ML
int js_nat_init_ml(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_DOCPARSE
int js_nat_init_docparse(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_COMPRESS
int js_nat_init_compress(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_RANDOM
int js_nat_init_random(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SORT
int js_nat_init_sort(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SEARCH
int js_nat_init_search(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES3
int js_nat_init_structures3(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SIMD
int js_nat_init_simd(JSContext *ctx);
#endif
#if defined(CONFIG_IO_URING) && defined(__linux__)
int js_nat_init_uring(JSContext *ctx); /* dynajs:uring disk I/O (Linux only) */
/* io_uring high-queue-depth whole-file read (0 ok, caller free()s *out). */
int dyn_uring_read_all(const char *path, char **out, size_t *outlen);
#endif
#ifdef CONFIG_NATIVE_MODULE_FILE
int js_nat_init_file(JSContext *ctx); /* dynajs:file buffered reader/writer */
#endif
#ifdef CONFIG_NATIVE_MODULE_TEXT
int js_nat_init_text(JSContext *ctx); /* dynajs:text SIMD byte/text utilities */
#endif
#ifdef CONFIG_NATIVE_MODULE_PATH
int js_nat_init_path(JSContext *ctx); /* dynajs:path POSIX path utilities */
#endif
#ifdef CONFIG_NATIVE_MODULE_STRINGS
int js_nat_init_strings(JSContext *ctx); /* dynajs:strings Go+JS string utilities */
#endif

/* Register every compiled-in native module in `ctx`. Called from the CLI. */
int js_nat_init_all(JSContext *ctx);

#endif /* CONFIG_NATIVE_MODULES */
#endif /* DYNAJS_NAT_H */
