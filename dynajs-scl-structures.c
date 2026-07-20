/*
 * scl:structures -- native data structures backed by secure-c-libs.
 *
 *   import { Vector, HashMap } from "scl:structures";
 *   const v = new Vector();
 *   try { v.push(1.5); v.push(2.5); print(v.get(0), v.length); }
 *   finally { v.close(); }        // deterministic free (arena destroyed)
 *
 * Each object owns a private SCL arena (see dynajs-scl.h). Native values are
 * copied to/from JS numbers at the boundary, so nothing native escapes.
 */
#include "dynajs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_array.h"
#include "scl_hash.h"
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------- Vector: dynamic array of doubles (scl_array) ---------- */

static JSClassID js_scl_vector_class_id;

static void js_scl_vector_dispose(void *native, scl_allocator_t *arena)
{
    scl_array_destroy(arena, (scl_array_t *)native);
}

static const JSClassDef js_scl_vector_class = {
    "Vector",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_vector_ctor(JSContext *ctx, JSValueConst new_target,
                                  int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_array_t *arr;

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    arr = (scl_array_t *)arena->malloc_fn(arena->state, sizeof(*arr), 16);
    if (!arr || scl_array_init(arena, arr, sizeof(double), 8) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_vector_class_id, arena, arr,
                                js_scl_vector_dispose);
}

/* NOTE: coerce every JS argument to a C local BEFORE resolving the native
 * handle. Coercion can run arbitrary JS (valueOf/@@toPrimitive/Proxy) which
 * may close() this very object; resolving after coercion means
 * js_scl_resource_get() sees r->closed and throws instead of using a freed
 * arena. No JS-invoking call may sit between the resolve and the native use. */

static scl_array_t *vector_this(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val, js_scl_vector_class_id);
    return r ? (scl_array_t *)r->native : NULL;
}

static JSValue js_scl_vector_push(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSSclResource *r;
    scl_array_t *arr;
    double x;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_vector_class_id);
    if (!r)
        return JS_EXCEPTION;
    arr = (scl_array_t *)r->native;
    if (scl_array_push(r->arena, arr, &x) != SCL_OK)
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewInt64(ctx, (int64_t)arr->count);
}

static JSValue js_scl_vector_get(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    scl_array_t *arr;
    uint32_t i;
    double x;
    if (JS_ToUint32(ctx, &i, argv[0]))
        return JS_EXCEPTION;
    arr = vector_this(ctx, this_val);
    if (!arr)
        return JS_EXCEPTION;
    if (i >= arr->count)
        return JS_UNDEFINED;
    if (scl_array_get(arr, i, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_vector_set(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    scl_array_t *arr;
    uint32_t i;
    double x;
    if (JS_ToUint32(ctx, &i, argv[0]) || JS_ToFloat64(ctx, &x, argv[1]))
        return JS_EXCEPTION;
    arr = vector_this(ctx, this_val);
    if (!arr)
        return JS_EXCEPTION;
    if (i >= arr->count)
        return JS_ThrowRangeError(ctx, "index out of range");
    if (scl_array_set(arr, i, &x) != SCL_OK)
        return JS_ThrowInternalError(ctx, "set failed");
    return JS_UNDEFINED;
}

static JSValue js_scl_vector_pop(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    scl_array_t *arr = vector_this(ctx, this_val);
    double x;
    if (!arr)
        return JS_EXCEPTION;
    if (arr->count == 0 || scl_array_pop(arr, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_vector_length(JSContext *ctx, JSValueConst this_val)
{
    scl_array_t *arr = vector_this(ctx, this_val);
    if (!arr)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)arr->count);
}

static const JSCFunctionListEntry js_scl_vector_proto[] = {
    JS_CFUNC_DEF("push", 1, js_scl_vector_push),
    JS_CFUNC_DEF("get", 1, js_scl_vector_get),
    JS_CFUNC_DEF("set", 2, js_scl_vector_set),
    JS_CFUNC_DEF("pop", 0, js_scl_vector_pop),
    JS_CGETSET_DEF("length", js_scl_vector_length, NULL),
};

/* ---------- HashMap: int32 key -> double value (scl_hash) ---------- */

static JSClassID js_scl_hashmap_class_id;

static size_t js_scl_hash_i32(const void *key, size_t len)
{
    /* fibonacci hash of the 32-bit key */
    uint32_t k;
    memcpy(&k, key, sizeof(k));
    return (size_t)(k * 2654435761u);
}

static void js_scl_hashmap_dispose(void *native, scl_allocator_t *arena)
{
    scl_hash_destroy(arena, (scl_hash_t *)native);
}

static const JSClassDef js_scl_hashmap_class = {
    "HashMap",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_hashmap_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_hash_t *ht;

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    ht = (scl_hash_t *)arena->malloc_fn(arena->state, sizeof(*ht), 16);
    if (!ht || scl_hash_init(arena, ht, sizeof(int32_t), sizeof(double), 16,
                             js_scl_hash_i32, scl_hash_eq_mem) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_hashmap_class_id, arena, ht,
                                js_scl_hashmap_dispose);
}

static JSValue js_scl_hashmap_set(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSSclResource *r;
    int32_t k;
    double v;
    if (JS_ToInt32(ctx, &k, argv[0]) || JS_ToFloat64(ctx, &v, argv[1]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_hashmap_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (scl_hash_insert(r->arena, (scl_hash_t *)r->native, &k, &v) != SCL_OK)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_scl_hashmap_get(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSSclResource *r;
    int32_t k;
    double v;
    if (JS_ToInt32(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_hashmap_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (scl_hash_get((scl_hash_t *)r->native, &k, &v) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, v);
}

static JSValue js_scl_hashmap_has(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSSclResource *r;
    int32_t k;
    if (JS_ToInt32(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_hashmap_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, scl_hash_contains((scl_hash_t *)r->native, &k));
}

static JSValue js_scl_hashmap_delete(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSSclResource *r;
    int32_t k;
    if (JS_ToInt32(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_hashmap_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, scl_hash_remove(r->arena, (scl_hash_t *)r->native, &k) == SCL_OK);
}

static JSValue js_scl_hashmap_size(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val, js_scl_hashmap_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)scl_hash_count((scl_hash_t *)r->native));
}

static const JSCFunctionListEntry js_scl_hashmap_proto[] = {
    JS_CFUNC_DEF("set", 2, js_scl_hashmap_set),
    JS_CFUNC_DEF("get", 1, js_scl_hashmap_get),
    JS_CFUNC_DEF("has", 1, js_scl_hashmap_has),
    JS_CFUNC_DEF("delete", 1, js_scl_hashmap_delete),
    JS_CGETSET_DEF("size", js_scl_hashmap_size, NULL),
};

/* ---------- module registration ---------- */

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

static int js_scl_structures_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (register_class(ctx, m, &js_scl_vector_class_id, &js_scl_vector_class,
                       js_scl_vector_proto, countof(js_scl_vector_proto),
                       js_scl_vector_ctor, "Vector") < 0)
        return -1;
    if (register_class(ctx, m, &js_scl_hashmap_class_id, &js_scl_hashmap_class,
                       js_scl_hashmap_proto, countof(js_scl_hashmap_proto),
                       js_scl_hashmap_ctor, "HashMap") < 0)
        return -1;
    return 0;
}

int js_scl_init_structures(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:structures",
                                   js_scl_structures_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Vector");
    JS_AddModuleExport(ctx, m, "HashMap");
    return 0;
}

#endif /* CONFIG_SCL_MODULES */
