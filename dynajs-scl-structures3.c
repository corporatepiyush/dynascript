/*
 * scl:structures3 -- more native data structures backed by secure-c-libs.
 *
 *   import { LRUCache, UnionFind, SortedSet } from "scl:structures3";
 *   const c = new LRUCache(2);
 *   try { c.put(1, 10); print(c.get(1), c.size); } finally { c.close(); }
 *
 *   const uf = new UnionFind(5);
 *   try { uf.union(0, 1); print(uf.connected(0, 1), uf.count); }
 *   finally { uf.close(); }
 *
 * Each object owns a private SCL arena (see dynajs-scl.h): the native struct and
 * all of its nodes are allocated from that arena, so disposal is one O(1)
 * scl_alloc_arena_destroy after the structure's own destroy. Native values are
 * copied to/from JS numbers at the boundary, so nothing native escapes.
 *
 * MEMORY DISCIPLINE (enforced in every method): coerce ALL JS arguments to C
 * locals FIRST, THEN resolve the native handle via js_scl_resource_get (which
 * checks `closed`), with NO JS-invoking call between the resolve and the native
 * use. Argument coercion runs arbitrary JS (valueOf/@@toPrimitive/Proxy) that
 * can call this.close() and free the arena; resolving afterwards means the
 * resolve sees r->closed and throws instead of touching a freed arena.
 */
#include "dynajs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_lru.h"
#include "scl_unionfind.h"
#include "scl_skiplist.h"
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------- LRUCache: int32 key -> double value (scl_lru) ---------- */

static JSClassID js_scl_lru_class_id;

static void js_scl_lru_dispose(void *native, scl_allocator_t *arena)
{
    scl_lru_destroy(arena, (scl_lru_t *)native);
}

static const JSClassDef js_scl_lru_class = {
    "LRUCache",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_lru_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_lru_t *cache;
    int64_t capacity;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "LRUCache requires a capacity");
    if (JS_ToInt64(ctx, &capacity, argv[0]))
        return JS_EXCEPTION;
    if (capacity < 1)
        return JS_ThrowRangeError(ctx, "capacity must be >= 1");
    /* cap so the backing allocation stays within the arena (a size near 2^60
       would overflow the arena's doubling loop and hang, not OOM) */
    if ((uint64_t)capacity > JS_SCL_ARENA_MAX / 32)
        return JS_ThrowRangeError(ctx, "capacity too large");

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    cache = (scl_lru_t *)arena->malloc_fn(arena->state, sizeof(*cache), 16);
    if (!cache || scl_lru_init(arena, cache, sizeof(int32_t), sizeof(double),
                               (size_t)capacity) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_lru_class_id, arena, cache,
                                js_scl_lru_dispose);
}

static JSValue js_scl_lru_put(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSSclResource *r;
    int32_t k;
    double v;
    if (JS_ToInt32(ctx, &k, argv[0]) || JS_ToFloat64(ctx, &v, argv[1]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_lru_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (scl_lru_put(r->arena, (scl_lru_t *)r->native, &k, &v) != SCL_OK)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

static JSValue js_scl_lru_get(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSSclResource *r;
    int32_t k;
    double v;
    if (JS_ToInt32(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_lru_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (scl_lru_get((scl_lru_t *)r->native, &k, &v) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, v);
}

static JSValue js_scl_lru_has(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSSclResource *r;
    int32_t k;
    if (JS_ToInt32(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_lru_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, scl_lru_contains((scl_lru_t *)r->native, &k));
}

static JSValue js_scl_lru_size(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val, js_scl_lru_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)scl_lru_count((scl_lru_t *)r->native));
}

static const JSCFunctionListEntry js_scl_lru_proto[] = {
    JS_CFUNC_DEF("put", 2, js_scl_lru_put),
    JS_CFUNC_DEF("get", 1, js_scl_lru_get),
    JS_CFUNC_DEF("has", 1, js_scl_lru_has),
    JS_CGETSET_DEF("size", js_scl_lru_size, NULL),
};

/* ---------- UnionFind: disjoint-set forest over 0..n-1 (scl_unionfind) ---- */

static JSClassID js_scl_unionfind_class_id;

static void js_scl_unionfind_dispose(void *native, scl_allocator_t *arena)
{
    scl_unionfind_destroy(arena, (scl_unionfind_t *)native);
}

static const JSClassDef js_scl_unionfind_class = {
    "UnionFind",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_unionfind_ctor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_unionfind_t *uf;
    int64_t n;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "UnionFind requires a size");
    if (JS_ToInt64(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    if (n < 1)
        return JS_ThrowRangeError(ctx, "size must be >= 1");
    /* cap so parent+rank (2 * size_t per element) stay within the arena;
       a size near 2^60 would overflow the arena's doubling loop and hang */
    if ((uint64_t)n > JS_SCL_ARENA_MAX / (2 * sizeof(size_t)))
        return JS_ThrowRangeError(ctx, "size too large");

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    uf = (scl_unionfind_t *)arena->malloc_fn(arena->state, sizeof(*uf), 16);
    if (!uf || scl_unionfind_init(arena, uf, (size_t)n) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_unionfind_class_id, arena, uf,
                                js_scl_unionfind_dispose);
}

/* Resolve + bounds-check two indices. On success stores the live struct in
 * *out and returns 0; otherwise throws (TypeError if closed, RangeError if out
 * of range) and returns -1. No JS runs between the resolve and the native read
 * of uf->count, so a coercion that closed the object is caught by the resolve. */
static int unionfind_resolve2(JSContext *ctx, JSValueConst this_val,
                              int64_t x, int64_t y, scl_unionfind_t **out)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val,
                                           js_scl_unionfind_class_id);
    scl_unionfind_t *uf;
    int64_t n;
    if (!r)
        return -1;
    uf = (scl_unionfind_t *)r->native;
    n = (int64_t)uf->count;
    if (x < 0 || x >= n || y < 0 || y >= n) {
        JS_ThrowRangeError(ctx, "index out of range [0, %lld)", (long long)n);
        return -1;
    }
    *out = uf;
    return 0;
}

static JSValue js_scl_unionfind_union(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    scl_unionfind_t *uf;
    int64_t x, y;
    if (JS_ToInt64(ctx, &x, argv[0]) || JS_ToInt64(ctx, &y, argv[1]))
        return JS_EXCEPTION;
    if (unionfind_resolve2(ctx, this_val, x, y, &uf))
        return JS_EXCEPTION;
    if (scl_unionfind_union(uf, (size_t)x, (size_t)y) != SCL_OK)
        return JS_ThrowInternalError(ctx, "union failed");
    return JS_UNDEFINED;
}

static JSValue js_scl_unionfind_connected(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    scl_unionfind_t *uf;
    int64_t x, y;
    if (JS_ToInt64(ctx, &x, argv[0]) || JS_ToInt64(ctx, &y, argv[1]))
        return JS_EXCEPTION;
    if (unionfind_resolve2(ctx, this_val, x, y, &uf))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, scl_unionfind_connected(uf, (size_t)x, (size_t)y));
}

static JSValue js_scl_unionfind_find(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSSclResource *r;
    scl_unionfind_t *uf;
    int64_t x, n;
    if (JS_ToInt64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_unionfind_class_id);
    if (!r)
        return JS_EXCEPTION;
    uf = (scl_unionfind_t *)r->native;
    n = (int64_t)uf->count;
    if (x < 0 || x >= n)
        return JS_ThrowRangeError(ctx, "index out of range [0, %lld)",
                                  (long long)n);
    return JS_NewInt64(ctx, (int64_t)scl_unionfind_find(uf, (size_t)x));
}

/* uf.count is the number of disjoint components (scl_unionfind_sets), NOT the
 * total element count (which scl_unionfind_count returns). */
static JSValue js_scl_unionfind_count(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val,
                                           js_scl_unionfind_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)scl_unionfind_sets((scl_unionfind_t *)r->native));
}

static const JSCFunctionListEntry js_scl_unionfind_proto[] = {
    JS_CFUNC_DEF("union", 2, js_scl_unionfind_union),
    JS_CFUNC_DEF("connected", 2, js_scl_unionfind_connected),
    JS_CFUNC_DEF("find", 1, js_scl_unionfind_find),
    JS_CGETSET_DEF("count", js_scl_unionfind_count, NULL),
};

/* ---------- SortedSet: ordered set of doubles (scl_skiplist) --------------
 * Chosen because scl_skiplist_destroy is a plain iterative walk of the level-0
 * linked list (no recursion, no fixed on-stack array), so tearing down a large
 * set from the arena cannot overflow the C stack -- unlike scl_trie_destroy. */

static JSClassID js_scl_sortedset_class_id;

static int js_scl_cmp_double(const void *a, const void *b)
{
    double x, y;
    memcpy(&x, a, sizeof(x));
    memcpy(&y, b, sizeof(y));
    return (x > y) - (x < y);
}

static void js_scl_sortedset_dispose(void *native, scl_allocator_t *arena)
{
    scl_skiplist_destroy(arena, (scl_skiplist_t *)native);
}

static const JSClassDef js_scl_sortedset_class = {
    "SortedSet",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_sortedset_ctor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_skiplist_t *sl;

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    sl = (scl_skiplist_t *)arena->malloc_fn(arena->state, sizeof(*sl), 16);
    if (!sl || scl_skiplist_init(arena, sl, sizeof(double),
                                 js_scl_cmp_double) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_sortedset_class_id, arena, sl,
                                js_scl_sortedset_dispose);
}

static JSValue js_scl_sortedset_add(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSSclResource *r;
    double x;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_sortedset_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (scl_skiplist_insert(r->arena, (scl_skiplist_t *)r->native, &x) != SCL_OK)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

static JSValue js_scl_sortedset_has(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSSclResource *r;
    double x;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_sortedset_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, scl_skiplist_contains((scl_skiplist_t *)r->native, &x));
}

static JSValue js_scl_sortedset_delete(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSSclResource *r;
    double x;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_sortedset_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx,
        scl_skiplist_remove(r->arena, (scl_skiplist_t *)r->native, &x) == SCL_OK);
}

static JSValue js_scl_sortedset_size(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val,
                                           js_scl_sortedset_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)scl_skiplist_count((scl_skiplist_t *)r->native));
}

static const JSCFunctionListEntry js_scl_sortedset_proto[] = {
    JS_CFUNC_DEF("add", 1, js_scl_sortedset_add),
    JS_CFUNC_DEF("has", 1, js_scl_sortedset_has),
    JS_CFUNC_DEF("delete", 1, js_scl_sortedset_delete),
    JS_CGETSET_DEF("size", js_scl_sortedset_size, NULL),
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

static int js_scl_structures3_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (register_class(ctx, m, &js_scl_lru_class_id, &js_scl_lru_class,
                       js_scl_lru_proto, countof(js_scl_lru_proto),
                       js_scl_lru_ctor, "LRUCache") < 0)
        return -1;
    if (register_class(ctx, m, &js_scl_unionfind_class_id,
                       &js_scl_unionfind_class, js_scl_unionfind_proto,
                       countof(js_scl_unionfind_proto),
                       js_scl_unionfind_ctor, "UnionFind") < 0)
        return -1;
    if (register_class(ctx, m, &js_scl_sortedset_class_id,
                       &js_scl_sortedset_class, js_scl_sortedset_proto,
                       countof(js_scl_sortedset_proto),
                       js_scl_sortedset_ctor, "SortedSet") < 0)
        return -1;
    return 0;
}

int js_scl_init_structures3(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:structures3",
                                   js_scl_structures3_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "LRUCache");
    JS_AddModuleExport(ctx, m, "UnionFind");
    JS_AddModuleExport(ctx, m, "SortedSet");
    return 0;
}

#endif /* CONFIG_SCL_MODULES */
