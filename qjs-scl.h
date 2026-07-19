/*
 * dynascript native modules backed by secure-c-libs (opt-in: CONFIG_SCL_MODULES).
 *
 * Memory discipline (the whole point): every native object owns a private SCL
 * arena allocator. ALL of that object's native memory comes from the arena, so
 * releasing it is a single scl_alloc_arena_destroy() -- O(1), no GC tracing, no
 * per-node free walk. JavaScript releases deterministically via .close() (also
 * exposed as [Symbol.dispose]); the class finalizer only runs as a safety net
 * for objects that were leaked. Native results are copied into JS values at the
 * call boundary, so no arena pointer ever escapes into the JS heap.
 */
#ifndef QJS_SCL_H
#define QJS_SCL_H

#include "quickjs.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_common.h"        /* scl_allocator_t, scl_error_t, SCL_OK */
#include "scl_alloc_arena.h"   /* per-object arena allocator */

/* Teardown callback a module supplies: call the matching scl_<type>_destroy on
 * `native` using `arena`. The arena itself is destroyed by the framework after. */
typedef void (*JSSclDisposeFunc)(void *native, scl_allocator_t *arena);

/* Opaque payload of every native-resource JS object. */
typedef struct {
    scl_allocator_t *arena;   /* per-object arena; backing = libc default */
    void *native;             /* the SCL object, allocated from `arena` */
    JSSclDisposeFunc dispose; /* module teardown; may be NULL */
    int closed;               /* 1 once released (idempotent) */
} JSSclResource;

/* Default per-object arena geometry (small initial page, grows to 256 MiB).
 * Kept small so a short-lived object costs little even before it is closed. */
#define JS_SCL_ARENA_INIT      512
#define JS_SCL_ARENA_MAX       ((size_t)256 * 1024 * 1024)
#define JS_SCL_ARENA_ALIGN     16

/* Create a fresh per-object arena, or return NULL (throws OOM) on failure. */
scl_allocator_t *js_scl_arena_new(JSContext *ctx);

/* Wrap `native` (allocated from `arena`) as a JS object of class `class_id`
 * whose proto already carries close/[Symbol.dispose]/closed. Takes ownership:
 * on any later disposal the framework runs `dispose` then destroys `arena`.
 * On error frees arena+native and returns JS_EXCEPTION. */
JSValue js_scl_resource_wrap(JSContext *ctx, JSClassID class_id,
                             scl_allocator_t *arena, void *native,
                             JSSclDisposeFunc dispose);

/* Fetch the live resource for `this`, or throw if closed/wrong-class. */
JSSclResource *js_scl_resource_get(JSContext *ctx, JSValueConst this_val,
                                   JSClassID class_id);

/* Install close(), [Symbol.dispose] and the `closed` getter on `proto`, plus
 * the shared finalizer wiring. Call once per class from the module init. */
void js_scl_class_common(JSContext *ctx, JSClassID class_id, JSValue proto);

/* The shared finalizer (disposes if still open). Reference it from JSClassDef. */
void js_scl_finalizer(JSRuntime *rt, JSValue val);

/* Per-family module initializers (each defined in its own translation unit). */
int js_scl_init_structures(JSContext *ctx);
#ifdef CONFIG_SCL_MODULE_HTTP
int js_scl_init_http(JSContext *ctx);
#endif
#ifdef CONFIG_SCL_MODULE_ML
int js_scl_init_ml(JSContext *ctx);
#endif
#ifdef CONFIG_SCL_MODULE_DOCPARSE
int js_scl_init_docparse(JSContext *ctx);
#endif

/* Register every available scl:* module in `ctx`. Called from the CLI. */
int js_scl_init_all(JSContext *ctx);

#endif /* CONFIG_SCL_MODULES */
#endif /* QJS_SCL_H */
