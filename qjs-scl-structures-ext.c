/*
 * scl:structures2 -- additional native data structures backed by secure-c-libs.
 *
 *   import { BloomFilter, Heap, RingBuffer, Deque } from "scl:structures2";
 *   const b = new BloomFilter(1000, 0.01);
 *   try { b.add("x"); if (b.contains("x")) print("maybe present"); }
 *   finally { b.close(); }        // deterministic free (arena destroyed)
 *
 * This is an extension companion to scl:structures; it defines its own init
 * function (js_scl_init_structures_ext) and its own class ids, so it can be
 * merged with the base module later without collision.
 *
 * Same arena-per-object memory model as qjs-scl.h: every object owns a private
 * SCL arena; .close()/[Symbol.dispose] runs the matching scl_*_destroy and then
 * destroys the arena (O(1) reclaim). Scalars are copied to/from JS numbers and
 * C strings are freed at the call boundary, so nothing native escapes into JS.
 *
 * The four structures were chosen because each has a single-buffer (or, for the
 * deque, single contiguous ring) teardown -- scl_*_destroy frees one arena block
 * with no recursion or fixed-size traversal stack -- so both the close() and the
 * finalizer paths stay ASan-clean regardless of element count.
 */
#include "qjs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_bloom.h"
#include "scl_heap.h"
#include "scl_ringbuf.h"
#include "scl_deque.h"
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Every element buffer here stores IEEE-754 doubles, matching the JS Number
 * boundary. The heap comparators read through memcpy so they stay valid on the
 * structure's packed (unaligned) byte storage. Returning (x>y)-(x<y) gives a
 * NaN-tolerant total-ish order with no branch on equality. */
static int js_scl_cmp_double_min(const void *a, const void *b)
{
    double x, y;
    memcpy(&x, a, sizeof(x));
    memcpy(&y, b, sizeof(y));
    return (x > y) - (x < y);
}

static int js_scl_cmp_double_max(const void *a, const void *b)
{
    double x, y;
    memcpy(&x, a, sizeof(x));
    memcpy(&y, b, sizeof(y));
    return (x < y) - (x > y);
}

/* ---------- BloomFilter: probabilistic string set (scl_bloom) ---------- */

static JSClassID js_scl_bloom_class_id;

static void js_scl_bloom_dispose(void *native, scl_allocator_t *arena)
{
    scl_bloom_destroy(arena, (scl_bloom_t *)native);
}

static const JSClassDef js_scl_bloom_class = {
    "BloomFilter",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_bloom_ctor(JSContext *ctx, JSValueConst new_target,
                                 int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_bloom_t *bf;
    int64_t expected;
    double fpr = 0.01;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "BloomFilter(expectedItems, "
                                      "falsePositiveRate=0.01): expectedItems "
                                      "is required");
    if (JS_ToInt64(ctx, &expected, argv[0]))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToFloat64(ctx, &fpr, argv[1]))
            return JS_EXCEPTION;
    }
    if (expected <= 0)
        return JS_ThrowRangeError(ctx, "BloomFilter: expectedItems must be > 0");
    if (!(fpr > 0.0 && fpr < 1.0))
        return JS_ThrowRangeError(ctx,
                                  "BloomFilter: falsePositiveRate must be "
                                  "in the open interval (0, 1)");

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    bf = (scl_bloom_t *)arena->malloc_fn(arena->state, sizeof(*bf), 16);
    if (!bf || scl_bloom_init(arena, bf, (size_t)expected, fpr,
                              scl_bloom_hash_murmur) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_bloom_class_id, arena, bf,
                                js_scl_bloom_dispose);
}

static scl_bloom_t *bloom_this(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val, js_scl_bloom_class_id);
    return r ? (scl_bloom_t *)r->native : NULL;
}

/* coerce args before resolving the native handle -- coercion can run JS that
 * close()s this object (see the note in qjs-scl-structures.c) */
static JSValue js_scl_bloom_add(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    scl_bloom_t *bf;
    const char *s;
    size_t len;
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    bf = bloom_this(ctx, this_val);
    if (!bf) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (scl_bloom_insert(bf, s, len) != SCL_OK) {
        JS_FreeCString(ctx, s);
        return JS_ThrowInternalError(ctx, "BloomFilter.add failed");
    }
    JS_FreeCString(ctx, s);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_scl_bloom_contains(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    scl_bloom_t *bf;
    const char *s;
    size_t len;
    bool present;
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    bf = bloom_this(ctx, this_val);
    if (!bf) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    present = scl_bloom_maybe_contains(bf, s, len);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, present);
}

static JSValue js_scl_bloom_clear(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    scl_bloom_t *bf = bloom_this(ctx, this_val);
    if (!bf)
        return JS_EXCEPTION;
    scl_bloom_clear(bf);
    return JS_UNDEFINED;
}

static JSValue js_scl_bloom_count(JSContext *ctx, JSValueConst this_val)
{
    scl_bloom_t *bf = bloom_this(ctx, this_val);
    if (!bf)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)scl_bloom_count(bf));
}

static JSValue js_scl_bloom_fpr(JSContext *ctx, JSValueConst this_val)
{
    scl_bloom_t *bf = bloom_this(ctx, this_val);
    if (!bf)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, scl_bloom_false_positive_rate(bf));
}

static const JSCFunctionListEntry js_scl_bloom_proto[] = {
    JS_CFUNC_DEF("add", 1, js_scl_bloom_add),
    JS_CFUNC_DEF("contains", 1, js_scl_bloom_contains),
    JS_CFUNC_DEF("clear", 0, js_scl_bloom_clear),
    JS_CGETSET_DEF("count", js_scl_bloom_count, NULL),
    JS_CGETSET_DEF("falsePositiveRate", js_scl_bloom_fpr, NULL),
};

/* ---------- Heap: binary min/max heap of doubles (scl_heap) ---------- */

static JSClassID js_scl_heap_class_id;

static void js_scl_heap_dispose(void *native, scl_allocator_t *arena)
{
    scl_heap_destroy(arena, (scl_heap_t *)native);
}

static const JSClassDef js_scl_heap_class = {
    "Heap",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_heap_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_heap_t *h;
    scl_cmp_func_t cmp = js_scl_cmp_double_min;

    /* new Heap() -> min-heap; new Heap("max") -> max-heap. */
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        const char *mode = JS_ToCString(ctx, argv[0]);
        if (!mode)
            return JS_EXCEPTION;
        if (strcmp(mode, "max") == 0)
            cmp = js_scl_cmp_double_max;
        JS_FreeCString(ctx, mode);
    }

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    h = (scl_heap_t *)arena->malloc_fn(arena->state, sizeof(*h), 16);
    if (!h || scl_heap_init(arena, h, sizeof(double), 8, cmp) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_heap_class_id, arena, h,
                                js_scl_heap_dispose);
}

static scl_heap_t *heap_this(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val, js_scl_heap_class_id);
    return r ? (scl_heap_t *)r->native : NULL;
}

static JSValue js_scl_heap_push(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSSclResource *r;
    double x;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_heap_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (scl_heap_push(r->arena, (scl_heap_t *)r->native, &x) != SCL_OK)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_scl_heap_pop(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    scl_heap_t *h = heap_this(ctx, this_val);
    double x;
    if (!h)
        return JS_EXCEPTION;
    if (scl_heap_pop(h, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_heap_peek(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    scl_heap_t *h = heap_this(ctx, this_val);
    double x;
    if (!h)
        return JS_EXCEPTION;
    if (scl_heap_peek(h, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_heap_size(JSContext *ctx, JSValueConst this_val)
{
    scl_heap_t *h = heap_this(ctx, this_val);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)scl_heap_count(h));
}

static JSValue js_scl_heap_empty(JSContext *ctx, JSValueConst this_val)
{
    scl_heap_t *h = heap_this(ctx, this_val);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, scl_heap_empty(h));
}

static const JSCFunctionListEntry js_scl_heap_proto[] = {
    JS_CFUNC_DEF("push", 1, js_scl_heap_push),
    JS_CFUNC_DEF("pop", 0, js_scl_heap_pop),
    JS_CFUNC_DEF("peek", 0, js_scl_heap_peek),
    JS_CGETSET_DEF("size", js_scl_heap_size, NULL),
    JS_CGETSET_DEF("empty", js_scl_heap_empty, NULL),
};

/* ---------- RingBuffer: fixed-capacity FIFO of doubles (scl_ringbuf) ---------- */

static JSClassID js_scl_ringbuf_class_id;

static void js_scl_ringbuf_dispose(void *native, scl_allocator_t *arena)
{
    scl_ringbuf_destroy(arena, (scl_ringbuf_t *)native);
}

static const JSClassDef js_scl_ringbuf_class = {
    "RingBuffer",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_ringbuf_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_ringbuf_t *rb;
    int64_t cap;
    int overwrite = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "RingBuffer(capacity, overwrite=false): "
                                      "capacity is required");
    if (JS_ToInt64(ctx, &cap, argv[0]))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        overwrite = JS_ToBool(ctx, argv[1]);
        if (overwrite < 0)
            return JS_EXCEPTION;
    }
    if (cap < 1)
        return JS_ThrowRangeError(ctx, "RingBuffer: capacity must be >= 1");

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    rb = (scl_ringbuf_t *)arena->malloc_fn(arena->state, sizeof(*rb), 16);
    if (!rb || scl_ringbuf_init(arena, rb, sizeof(double), (size_t)cap,
                                overwrite != 0) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_ringbuf_class_id, arena, rb,
                                js_scl_ringbuf_dispose);
}

static scl_ringbuf_t *ringbuf_this(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r =
        js_scl_resource_get(ctx, this_val, js_scl_ringbuf_class_id);
    return r ? (scl_ringbuf_t *)r->native : NULL;
}

static JSValue js_scl_ringbuf_push(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    scl_ringbuf_t *rb;
    double x;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    rb = ringbuf_this(ctx, this_val);
    if (!rb)
        return JS_EXCEPTION;
    /* true if accepted; false if full and not in overwrite mode. */
    return JS_NewBool(ctx, scl_ringbuf_push(rb, &x) == SCL_OK);
}

static JSValue js_scl_ringbuf_pop(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    scl_ringbuf_t *rb = ringbuf_this(ctx, this_val);
    double x;
    if (!rb)
        return JS_EXCEPTION;
    if (scl_ringbuf_pop(rb, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_ringbuf_peek(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    scl_ringbuf_t *rb;
    int64_t idx = 0;
    double x;
    if (!JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &idx, argv[0]))
            return JS_EXCEPTION;
    }
    rb = ringbuf_this(ctx, this_val);
    if (!rb)
        return JS_EXCEPTION;
    if (idx < 0)
        return JS_UNDEFINED;
    if (scl_ringbuf_peek(rb, (size_t)idx, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_ringbuf_size(JSContext *ctx, JSValueConst this_val)
{
    scl_ringbuf_t *rb = ringbuf_this(ctx, this_val);
    if (!rb)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)scl_ringbuf_count(rb));
}

static JSValue js_scl_ringbuf_capacity(JSContext *ctx, JSValueConst this_val)
{
    scl_ringbuf_t *rb = ringbuf_this(ctx, this_val);
    if (!rb)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)scl_ringbuf_capacity(rb));
}

static JSValue js_scl_ringbuf_empty(JSContext *ctx, JSValueConst this_val)
{
    scl_ringbuf_t *rb = ringbuf_this(ctx, this_val);
    if (!rb)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, scl_ringbuf_empty(rb));
}

static JSValue js_scl_ringbuf_full(JSContext *ctx, JSValueConst this_val)
{
    scl_ringbuf_t *rb = ringbuf_this(ctx, this_val);
    if (!rb)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, scl_ringbuf_full(rb));
}

static const JSCFunctionListEntry js_scl_ringbuf_proto[] = {
    JS_CFUNC_DEF("push", 1, js_scl_ringbuf_push),
    JS_CFUNC_DEF("pop", 0, js_scl_ringbuf_pop),
    JS_CFUNC_DEF("peek", 1, js_scl_ringbuf_peek),
    JS_CGETSET_DEF("size", js_scl_ringbuf_size, NULL),
    JS_CGETSET_DEF("capacity", js_scl_ringbuf_capacity, NULL),
    JS_CGETSET_DEF("empty", js_scl_ringbuf_empty, NULL),
    JS_CGETSET_DEF("full", js_scl_ringbuf_full, NULL),
};

/* ---------- Deque: double-ended queue of doubles (scl_deque) ---------- */

static JSClassID js_scl_deque_class_id;

static void js_scl_deque_dispose(void *native, scl_allocator_t *arena)
{
    scl_deque_destroy(arena, (scl_deque_t *)native);
}

static const JSClassDef js_scl_deque_class = {
    "Deque",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_deque_ctor(JSContext *ctx, JSValueConst new_target,
                                 int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_deque_t *dq;
    int64_t cap = 8;

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &cap, argv[0]))
            return JS_EXCEPTION;
        if (cap < 0)
            cap = 0; /* 0 is valid: the deque grows on first push */
    }

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    dq = (scl_deque_t *)arena->malloc_fn(arena->state, sizeof(*dq), 16);
    if (!dq || scl_deque_init(arena, dq, sizeof(double),
                              (size_t)cap) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_deque_class_id, arena, dq,
                                js_scl_deque_dispose);
}

static scl_deque_t *deque_this(JSContext *ctx, JSValueConst this_val)
{
    JSSclResource *r = js_scl_resource_get(ctx, this_val, js_scl_deque_class_id);
    return r ? (scl_deque_t *)r->native : NULL;
}

static JSValue js_scl_deque_push_front(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSSclResource *r;
    double x;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_deque_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (scl_deque_push_front(r->arena, (scl_deque_t *)r->native, &x) != SCL_OK)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_scl_deque_push_back(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSSclResource *r;
    double x;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_deque_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (scl_deque_push_back(r->arena, (scl_deque_t *)r->native, &x) != SCL_OK)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_scl_deque_pop_front(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    scl_deque_t *dq = deque_this(ctx, this_val);
    double x;
    if (!dq)
        return JS_EXCEPTION;
    if (scl_deque_pop_front(dq, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_deque_pop_back(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    scl_deque_t *dq = deque_this(ctx, this_val);
    double x;
    if (!dq)
        return JS_EXCEPTION;
    if (scl_deque_pop_back(dq, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_deque_peek_front(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    scl_deque_t *dq = deque_this(ctx, this_val);
    double x;
    if (!dq)
        return JS_EXCEPTION;
    if (scl_deque_peek_front(dq, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_deque_peek_back(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    scl_deque_t *dq = deque_this(ctx, this_val);
    double x;
    if (!dq)
        return JS_EXCEPTION;
    if (scl_deque_peek_back(dq, &x) != SCL_OK)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, x);
}

static JSValue js_scl_deque_size(JSContext *ctx, JSValueConst this_val)
{
    scl_deque_t *dq = deque_this(ctx, this_val);
    if (!dq)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)scl_deque_count(dq));
}

static JSValue js_scl_deque_empty(JSContext *ctx, JSValueConst this_val)
{
    scl_deque_t *dq = deque_this(ctx, this_val);
    if (!dq)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, scl_deque_empty(dq));
}

static const JSCFunctionListEntry js_scl_deque_proto[] = {
    JS_CFUNC_DEF("pushFront", 1, js_scl_deque_push_front),
    JS_CFUNC_DEF("pushBack", 1, js_scl_deque_push_back),
    JS_CFUNC_DEF("popFront", 0, js_scl_deque_pop_front),
    JS_CFUNC_DEF("popBack", 0, js_scl_deque_pop_back),
    JS_CFUNC_DEF("peekFront", 0, js_scl_deque_peek_front),
    JS_CFUNC_DEF("peekBack", 0, js_scl_deque_peek_back),
    JS_CGETSET_DEF("size", js_scl_deque_size, NULL),
    JS_CGETSET_DEF("empty", js_scl_deque_empty, NULL),
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

static int js_scl_structures_ext_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (register_class(ctx, m, &js_scl_bloom_class_id, &js_scl_bloom_class,
                       js_scl_bloom_proto, countof(js_scl_bloom_proto),
                       js_scl_bloom_ctor, "BloomFilter") < 0)
        return -1;
    if (register_class(ctx, m, &js_scl_heap_class_id, &js_scl_heap_class,
                       js_scl_heap_proto, countof(js_scl_heap_proto),
                       js_scl_heap_ctor, "Heap") < 0)
        return -1;
    if (register_class(ctx, m, &js_scl_ringbuf_class_id, &js_scl_ringbuf_class,
                       js_scl_ringbuf_proto, countof(js_scl_ringbuf_proto),
                       js_scl_ringbuf_ctor, "RingBuffer") < 0)
        return -1;
    if (register_class(ctx, m, &js_scl_deque_class_id, &js_scl_deque_class,
                       js_scl_deque_proto, countof(js_scl_deque_proto),
                       js_scl_deque_ctor, "Deque") < 0)
        return -1;
    return 0;
}

int js_scl_init_structures_ext(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:structures2",
                                   js_scl_structures_ext_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "BloomFilter");
    JS_AddModuleExport(ctx, m, "Heap");
    JS_AddModuleExport(ctx, m, "RingBuffer");
    JS_AddModuleExport(ctx, m, "Deque");
    return 0;
}

#endif /* CONFIG_SCL_MODULES */
