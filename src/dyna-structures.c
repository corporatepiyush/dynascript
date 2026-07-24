/*
 * dyna:structures -- native data structures, self-contained and in-repo.
 *
 *   import { Vector, HashMap } from "dyna:structures";
 *   const v = new Vector();
 *   try { v.push({x: 1}); v.push("s"); print(v.get(0).x, v.length); }
 *   finally { v.close(); }              // deterministic free
 *
 *   const m = new HashMap();
 *   try { m.set("k", 42); if (m.has("k")) print(m.get("k")); }
 *   finally { m.close(); }
 *
 * This is a from-scratch replacement for the old secure-c-libs binding
 * (dyna-structures.c). It keeps the same JS surface -- class names,
 * method names, arities, return types and error behavior -- but drops the
 * per-object SCL arena for the plain-malloc resource model of dyna-nat.h.
 *
 * Ownership: a container stores JS values by JS_DupValue and owns those
 * references. Every stored value is released exactly once -- on overwrite,
 * removal, or disposal. Disposal runs with no JSContext (see DynDisposeFunc),
 * so each container caches the JSRuntime at construction and frees its values
 * with JS_FreeValueRT. Nothing native escapes into the JS heap: results handed
 * back to JS are fresh dups, and the native storage is freed at teardown.
 *
 * Reentrancy: every method coerces its JS arguments to C locals FIRST, then
 * resolves the native handle (dyn_res_native / dyn_res_get, which reject a
 * closed resource), with no JS-invoking call in between. Argument coercion can
 * run arbitrary user JS (valueOf/toString/@@toPrimitive) that close()s `this`;
 * resolving afterward makes that a clean throw instead of a use-after-free.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ================= Vector: growable array of JS values ================= */

typedef struct {
    JSRuntime *rt;   /* cached at construction so dispose() can free values */
    JSValue *items;
    uint32_t count;
    uint32_t cap;
} dyn_vector_t;

/* Grow so that at least `need` slots exist. Doubles capacity; guards against
 * uint32/size_t overflow of the element count. Returns 0 or -1. */
static int dyn_vector_reserve(dyn_vector_t *v, uint32_t need)
{
    uint32_t ncap;
    JSValue *ni;

    if (need <= v->cap)
        return 0;
    if (need > UINT32_MAX / (uint32_t)sizeof(JSValue))
        return -1;
    ncap = v->cap ? v->cap : 8;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2 ||
            ncap > UINT32_MAX / (uint32_t)sizeof(JSValue) / 2) {
            ncap = need; /* final step: exact, no further doubling */
            break;
        }
        ncap *= 2;
    }
    ni = (JSValue *)realloc(v->items, (size_t)ncap * sizeof(JSValue));
    if (!ni)
        return -1;
    v->items = ni;
    v->cap = ncap;
    return 0;
}

static void dyn_vector_free(void *native)
{
    dyn_vector_t *v = (dyn_vector_t *)native;
    uint32_t i;

    if (!v)
        return;
    for (i = 0; i < v->count; i++)
        JS_FreeValueRT(v->rt, v->items[i]);
    free(v->items);
    free(v);
}

static JSClassID dyn_vector_class_id;

static const JSClassDef dyn_vector_class = {
    "Vector",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_vector_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_vector_t *v;

    (void)new_target; (void)argc; (void)argv;
    v = (dyn_vector_t *)malloc(sizeof(*v));
    if (!v)
        return JS_ThrowOutOfMemory(ctx);
    v->rt = JS_GetRuntime(ctx);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
    return dyn_res_wrap(ctx, dyn_vector_class_id, v, dyn_vector_free);
}

static JSValue dyn_vector_push(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_vector_t *v;

    /* The value is stored verbatim (no coercion runs user JS), so resolving
     * before the dup is safe here. */
    (void)argc;
    v = (dyn_vector_t *)dyn_res_native(ctx, this_val, dyn_vector_class_id);
    if (!v)
        return JS_EXCEPTION;
    if (dyn_vector_reserve(v, v->count + 1))
        return JS_ThrowOutOfMemory(ctx);
    v->items[v->count++] = JS_DupValue(ctx, argv[0]);
    return JS_NewInt64(ctx, (int64_t)v->count);
}

static JSValue dyn_vector_get(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_vector_t *v;
    uint32_t i;

    (void)argc;
    if (JS_ToUint32(ctx, &i, argv[0]))
        return JS_EXCEPTION;
    v = (dyn_vector_t *)dyn_res_native(ctx, this_val, dyn_vector_class_id);
    if (!v)
        return JS_EXCEPTION;
    if (i >= v->count)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, v->items[i]);
}

static JSValue dyn_vector_set(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_vector_t *v;
    uint32_t i;

    (void)argc;
    if (JS_ToUint32(ctx, &i, argv[0]))
        return JS_EXCEPTION;
    v = (dyn_vector_t *)dyn_res_native(ctx, this_val, dyn_vector_class_id);
    if (!v)
        return JS_EXCEPTION;
    if (i >= v->count)
        return JS_ThrowRangeError(ctx, "index out of range");
    JS_FreeValueRT(v->rt, v->items[i]);
    v->items[i] = JS_DupValue(ctx, argv[1]);
    return JS_UNDEFINED;
}

static JSValue dyn_vector_pop(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_vector_t *v;

    (void)argc; (void)argv;
    v = (dyn_vector_t *)dyn_res_native(ctx, this_val, dyn_vector_class_id);
    if (!v)
        return JS_EXCEPTION;
    if (v->count == 0)
        return JS_UNDEFINED;
    /* Ownership of the popped reference transfers to the caller (no extra dup,
     * no free): the slot is simply forgotten. */
    return v->items[--v->count];
}

static JSValue dyn_vector_length(JSContext *ctx, JSValueConst this_val)
{
    dyn_vector_t *v =
        (dyn_vector_t *)dyn_res_native(ctx, this_val, dyn_vector_class_id);
    if (!v)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)v->count);
}

static const JSCFunctionListEntry dyn_vector_proto[] = {
    JS_CFUNC_DEF("push", 1, dyn_vector_push),
    JS_CFUNC_DEF("get", 1, dyn_vector_get),
    JS_CFUNC_DEF("set", 2, dyn_vector_set),
    JS_CFUNC_DEF("pop", 0, dyn_vector_pop),
    JS_CGETSET_DEF("length", dyn_vector_length, NULL),
};

/* ================= HashMap: string key -> JS value ================= */

typedef struct DynMapEntry {
    struct DynMapEntry *next;
    JSValue value;
    uint64_t hash;
    size_t key_len;
    char *key;       /* malloc'd, key_len bytes + NUL */
} DynMapEntry;

typedef struct {
    JSRuntime *rt;
    DynMapEntry **buckets;
    uint32_t n_buckets;   /* always a power of two */
    uint32_t count;
} dyn_hashmap_t;

#define DYN_MAP_INIT_BUCKETS 16

static uint64_t dyn_map_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ULL; /* FNV-1a offset basis */
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 1099511628211ULL; /* FNV prime */
    }
    return h;
}

static DynMapEntry *dyn_map_find(dyn_hashmap_t *m, const char *key,
                                 size_t klen, uint64_t h, uint32_t *pbucket)
{
    uint32_t b = (uint32_t)(h & (m->n_buckets - 1));
    DynMapEntry *e;

    if (pbucket)
        *pbucket = b;
    for (e = m->buckets[b]; e; e = e->next) {
        if (e->hash == h && e->key_len == klen &&
            memcmp(e->key, key, klen) == 0)
            return e;
    }
    return NULL;
}

/* Double the bucket count and rehash. Returns 0 or -1 (leaving the map intact
 * on failure -- the caller can still proceed, just at higher load). */
static int dyn_map_grow(dyn_hashmap_t *m)
{
    uint32_t nnew = m->n_buckets * 2;
    DynMapEntry **nb;
    uint32_t i;

    if (nnew < m->n_buckets) /* overflow */
        return -1;
    nb = (DynMapEntry **)calloc(nnew, sizeof(*nb));
    if (!nb)
        return -1;
    for (i = 0; i < m->n_buckets; i++) {
        DynMapEntry *e = m->buckets[i];
        while (e) {
            DynMapEntry *next = e->next;
            uint32_t b = (uint32_t)(e->hash & (nnew - 1));
            e->next = nb[b];
            nb[b] = e;
            e = next;
        }
    }
    free(m->buckets);
    m->buckets = nb;
    m->n_buckets = nnew;
    return 0;
}

static void dyn_hashmap_free(void *native)
{
    dyn_hashmap_t *m = (dyn_hashmap_t *)native;
    uint32_t i;

    if (!m)
        return;
    for (i = 0; i < m->n_buckets; i++) {
        DynMapEntry *e = m->buckets[i];
        while (e) {
            DynMapEntry *next = e->next;
            JS_FreeValueRT(m->rt, e->value);
            free(e->key);
            free(e);
            e = next;
        }
    }
    free(m->buckets);
    free(m);
}

static JSClassID dyn_hashmap_class_id;

static const JSClassDef dyn_hashmap_class = {
    "HashMap",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_hashmap_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv)
{
    dyn_hashmap_t *m;

    (void)new_target; (void)argc; (void)argv;
    m = (dyn_hashmap_t *)malloc(sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->rt = JS_GetRuntime(ctx);
    m->count = 0;
    m->n_buckets = DYN_MAP_INIT_BUCKETS;
    m->buckets = (DynMapEntry **)calloc(m->n_buckets, sizeof(*m->buckets));
    if (!m->buckets) {
        free(m);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, dyn_hashmap_class_id, m, dyn_hashmap_free);
}

static JSValue dyn_hashmap_set(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_hashmap_t *m;
    DynMapEntry *e;
    const char *key;
    size_t klen;
    uint64_t h;
    uint32_t b;

    (void)argc;
    /* Coerce the key to a C string FIRST (ToString can run user JS that
     * close()s this), THEN resolve. The value is stored by dup and never
     * coerced. */
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    m = (dyn_hashmap_t *)dyn_res_native(ctx, this_val, dyn_hashmap_class_id);
    if (!m) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    h = dyn_map_hash(key, klen);
    e = dyn_map_find(m, key, klen, h, &b);
    if (e) {
        /* Overwrite: release the previous value, keep the entry/key. */
        JSValue nv = JS_DupValue(ctx, argv[1]);
        JS_FreeValueRT(m->rt, e->value);
        e->value = nv;
        JS_FreeCString(ctx, key);
        return JS_DupValue(ctx, this_val);
    }
    /* Grow before inserting when the table is 3/4 full; recompute the bucket
     * against the new mask. A failed grow is non-fatal (insert at old size). */
    if (m->count + 1 > m->n_buckets - (m->n_buckets >> 2)) {
        if (dyn_map_grow(m) == 0)
            b = (uint32_t)(h & (m->n_buckets - 1));
    }
    e = (DynMapEntry *)malloc(sizeof(*e));
    if (!e) {
        JS_FreeCString(ctx, key);
        return JS_ThrowOutOfMemory(ctx);
    }
    e->key = (char *)malloc(klen + 1);
    if (!e->key) {
        free(e);
        JS_FreeCString(ctx, key);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(e->key, key, klen);
    e->key[klen] = '\0';
    e->key_len = klen;
    e->hash = h;
    e->value = JS_DupValue(ctx, argv[1]);
    e->next = m->buckets[b];
    m->buckets[b] = e;
    m->count++;
    JS_FreeCString(ctx, key);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_hashmap_get(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_hashmap_t *m;
    DynMapEntry *e;
    const char *key;
    size_t klen;

    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    m = (dyn_hashmap_t *)dyn_res_native(ctx, this_val, dyn_hashmap_class_id);
    if (!m) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    e = dyn_map_find(m, key, klen, dyn_map_hash(key, klen), NULL);
    JS_FreeCString(ctx, key);
    return e ? JS_DupValue(ctx, e->value) : JS_UNDEFINED;
}

static JSValue dyn_hashmap_has(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_hashmap_t *m;
    DynMapEntry *e;
    const char *key;
    size_t klen;

    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    m = (dyn_hashmap_t *)dyn_res_native(ctx, this_val, dyn_hashmap_class_id);
    if (!m) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    e = dyn_map_find(m, key, klen, dyn_map_hash(key, klen), NULL);
    JS_FreeCString(ctx, key);
    return JS_NewBool(ctx, e != NULL);
}

static JSValue dyn_hashmap_delete(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_hashmap_t *m;
    DynMapEntry *e, *prev;
    const char *key;
    size_t klen;
    uint64_t h;
    uint32_t b;

    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    m = (dyn_hashmap_t *)dyn_res_native(ctx, this_val, dyn_hashmap_class_id);
    if (!m) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    h = dyn_map_hash(key, klen);
    b = (uint32_t)(h & (m->n_buckets - 1));
    prev = NULL;
    for (e = m->buckets[b]; e; prev = e, e = e->next) {
        if (e->hash == h && e->key_len == klen &&
            memcmp(e->key, key, klen) == 0) {
            if (prev)
                prev->next = e->next;
            else
                m->buckets[b] = e->next;
            JS_FreeValueRT(m->rt, e->value);
            free(e->key);
            free(e);
            m->count--;
            JS_FreeCString(ctx, key);
            return JS_NewBool(ctx, 1);
        }
    }
    JS_FreeCString(ctx, key);
    return JS_NewBool(ctx, 0);
}

static JSValue dyn_hashmap_size(JSContext *ctx, JSValueConst this_val)
{
    dyn_hashmap_t *m =
        (dyn_hashmap_t *)dyn_res_native(ctx, this_val, dyn_hashmap_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)m->count);
}

static const JSCFunctionListEntry dyn_hashmap_proto[] = {
    JS_CFUNC_DEF("set", 2, dyn_hashmap_set),
    JS_CFUNC_DEF("get", 1, dyn_hashmap_get),
    JS_CFUNC_DEF("has", 1, dyn_hashmap_has),
    JS_CFUNC_DEF("delete", 1, dyn_hashmap_delete),
    JS_CGETSET_DEF("size", dyn_hashmap_size, NULL),
};

/* ================= module registration ================= */

static int dyn_structures_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_vector_class_id, &dyn_vector_class,
                           dyn_vector_proto, countof(dyn_vector_proto),
                           dyn_vector_ctor, "Vector") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_hashmap_class_id, &dyn_hashmap_class,
                           dyn_hashmap_proto, countof(dyn_hashmap_proto),
                           dyn_hashmap_ctor, "HashMap") < 0)
        return -1;
    return 0;
}

int js_nat_init_structures(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:structures",
                                   dyn_structures_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Vector");
    JS_AddModuleExport(ctx, m, "HashMap");
    return 0;
}

/* The framework's js_nat_init_all() references js_nat_init_structures_ext()
 * unconditionally under CONFIG_NATIVE_MODULE_STRUCTURES (the flag this file
 * turns on). The extended structures (BloomFilter/Heap/RingBuffer/Deque of
 * dyna-structures.c (ext half)) are not part of this migration slice, so this
 * registers nothing: "dyna:structures2" stays unavailable (import throws),
 * which is the correct state for an unported module. */
int js_nat_init_structures_ext(JSContext *ctx)
{
    (void)ctx;
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_STRUCTURES */
