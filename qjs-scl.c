/*
 * dynascript native modules backed by secure-c-libs -- shared framework.
 * See qjs-scl.h for the arena-per-object memory model.
 */
#include "qjs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Registry of class ids the framework owns, so the shared close() method can
 * validate `this` before treating its opaque as a JSSclResource (a foreign
 * object passed via close.call(x) must not be reinterpreted -- and a foreign
 * class may store a non-pointer opaque, so a magic-tag check would be unsafe).
 *
 * Registration happens only from js_scl_init_all(), which the CLI calls on the
 * main context at startup (worker threads do NOT register scl modules), so
 * there is no concurrent mutation of this table. Class ids are process-unique
 * (JS_NewClassID) and never reset, so the table accumulates across runtimes in
 * a long-lived multi-runtime embed; the cap is sized well beyond the number of
 * scl classes so registration never silently drops (which would make close() a
 * no-op). A per-JSRuntime registry is the architectural follow-up. */
#define JS_SCL_MAX_CLASSES 256
static JSClassID js_scl_class_ids[JS_SCL_MAX_CLASSES];
static int js_scl_n_classes;

static int js_scl_is_our_class(JSClassID id)
{
    int i;
    for (i = 0; i < js_scl_n_classes; i++)
        if (js_scl_class_ids[i] == id)
            return 1;
    return 0;
}

scl_allocator_t *js_scl_arena_new(JSContext *ctx)
{
    scl_allocator_t *a = scl_alloc_arena_create(scl_allocator_default(),
                                                JS_SCL_ARENA_INIT,
                                                JS_SCL_ARENA_MAX,
                                                JS_SCL_ARENA_ALIGN);
    if (!a)
        JS_ThrowOutOfMemory(ctx);
    return a;
}

/* Idempotent teardown: run the module dispose, then reclaim the whole arena. */
static void js_scl_resource_release(JSSclResource *r)
{
    if (!r || r->closed)
        return;
    r->closed = 1;
    if (r->dispose && r->native)
        r->dispose(r->native, r->arena);
    if (r->arena)
        scl_alloc_arena_destroy(r->arena);
    r->arena = NULL;
    r->native = NULL;
}

void js_scl_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id;
    JSSclResource *r = JS_GetAnyOpaque(val, &id);
    /* only reached for our classes (set in their JSClassDef), so `r` is ours */
    if (r) {
        js_scl_resource_release(r);
        js_free_rt(rt, r);
    }
}

JSValue js_scl_resource_wrap(JSContext *ctx, JSClassID class_id,
                             scl_allocator_t *arena, void *native,
                             JSSclDisposeFunc dispose)
{
    JSValue obj;
    JSSclResource *r;

    obj = JS_NewObjectClass(ctx, class_id);
    if (JS_IsException(obj))
        goto fail;
    r = js_mallocz(ctx, sizeof(*r));
    if (!r) {
        JS_FreeValue(ctx, obj);
        goto fail;
    }
    r->arena = arena;
    r->native = native;
    r->dispose = dispose;
    r->closed = 0;
    JS_SetOpaque(obj, r);
    return obj;
 fail:
    if (dispose && native)
        dispose(native, arena);
    if (arena)
        scl_alloc_arena_destroy(arena);
    return JS_EXCEPTION;
}

JSSclResource *js_scl_resource_get(JSContext *ctx, JSValueConst this_val,
                                   JSClassID class_id)
{
    JSSclResource *r = JS_GetOpaque2(ctx, this_val, class_id);
    if (!r)
        return NULL; /* JS_GetOpaque2 already threw */
    if (r->closed) {
        JS_ThrowTypeError(ctx, "use of a closed native resource");
        return NULL;
    }
    return r;
}

/* close() / dispose(): explicit deterministic release. Safe for any `this`. */
static JSValue js_scl_method_close(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSClassID id;
    JSSclResource *r = JS_GetAnyOpaque(this_val, &id);
    if (r && js_scl_is_our_class(id))
        js_scl_resource_release(r);
    return JS_UNDEFINED;
}

static JSValue js_scl_getter_closed(JSContext *ctx, JSValueConst this_val)
{
    JSClassID id;
    JSSclResource *r = JS_GetAnyOpaque(this_val, &id);
    if (r && js_scl_is_our_class(id))
        return JS_NewBool(ctx, r->closed);
    return JS_ThrowTypeError(ctx, "not a native resource");
}

/* close/dispose/[Symbol.dispose]/closed installed on every scl resource proto.
 * [Symbol.dispose] makes these work with `using` and DisposableStack.use(). */
static const JSCFunctionListEntry js_scl_common_funcs[] = {
    JS_CFUNC_DEF("close", 0, js_scl_method_close),
    JS_CFUNC_DEF("dispose", 0, js_scl_method_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, js_scl_method_close),
    JS_CGETSET_DEF("closed", js_scl_getter_closed, NULL),
};

void js_scl_class_common(JSContext *ctx, JSClassID class_id, JSValue proto)
{
    if (js_scl_n_classes < JS_SCL_MAX_CLASSES)
        js_scl_class_ids[js_scl_n_classes++] = class_id;
    JS_SetPropertyFunctionList(ctx, proto, js_scl_common_funcs,
                               countof(js_scl_common_funcs));
}

int js_scl_init_all(JSContext *ctx)
{
    if (js_scl_init_structures(ctx))
        return -1;
    if (js_scl_init_structures_ext(ctx))
        return -1;
#ifdef CONFIG_SCL_MODULE_HTTP
    if (js_scl_init_http(ctx))
        return -1;
#endif
#ifdef CONFIG_SCL_MODULE_ML
    if (js_scl_init_ml(ctx))
        return -1;
#endif
#ifdef CONFIG_SCL_MODULE_DOCPARSE
    if (js_scl_init_docparse(ctx))
        return -1;
#endif
    return 0;
}

#endif /* CONFIG_SCL_MODULES */
