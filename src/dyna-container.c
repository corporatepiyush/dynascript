/*
 * dyna:container -- native container data structures, self-contained and
 * in-repo. Mirrors Go's container/heap, container/list, container/ring.
 *
 *   import { Heap, List, Ring } from "dyna:container";
 *
 *   const h = new Heap((a, b) => a - b);        // min-heap; (a,b)=>b-a => max-heap
 *   try { h.push(3); h.push(1); h.push(2); print(h.pop(), h.pop(), h.pop()); }
 *   finally { h.close(); }                       // -> 1 2 3
 *
 *   const l = new List();
 *   try { l.pushBack(1); l.pushFront(0); print(l.toArray()); }
 *   finally { l.close(); }                        // -> [0, 1]
 *
 *   const r = new Ring(3);
 *   try { r.push(1); r.push(2); r.push(3); r.push(4); print(r.toArray()); }
 *   finally { r.close(); }                        // -> [2, 3, 4] (1 overwritten)
 *
 * Ownership (same discipline as dyna-structures.c): every container stores
 * JS values by JS_DupValue and owns those references; each is released
 * exactly once, on removal/overwrite or disposal. Disposal runs with no
 * JSContext, so every container caches the JSRuntime at construction and
 * frees with JS_FreeValueRT. Nothing native escapes: results handed back to
 * JS are fresh dups (or an owned transfer for pop-like removals), and every
 * method coerces its JS arguments to C locals FIRST, then resolves the
 * native handle (dyn_res_native, which rejects a closed resource), with no
 * JS-invoking call in between -- a valueOf/toString can close() `this`
 * mid-coercion; resolving afterward turns that into a clean throw instead of
 * a use-after-free.
 *
 * Heap's comparator is a harder version of the same problem, because unlike
 * dyna-sort.c's sort()/binarySearch() (plain, transient functions whose
 * comparator argument is kept alive for the whole call by the interpreter's
 * own argument stack), a Heap's comparator is STORED in the container and
 * invoked again on every later push()/pop() -- and the comparator's own body
 * can call close() on the very heap it is ordering. Three rules together
 * make that safe:
 *
 *   1. dyn_heap_cmp_call dups the comparator function AND both compared
 *      operands before every JS_Call, and frees the dups only after the call
 *      returns. Without this, a comparator like `(a,b) => { h.close(); return
 *      a - b }` would free the container's only reference to itself (h->cmp)
 *      and to its stored operands (h->items[...]) WHILE that very call is
 *      still executing -- a use-after-free on the running function object
 *      and/or its arguments. The extra local reference keeps everything
 *      alive for the duration regardless of what close() frees underneath.
 *   2. Every sift loop re-resolves the resource (dyn_res_native) right after
 *      each comparator call and aborts immediately if it comes back closed.
 *      A closed resource means dispose() already ran and freed the dyn_heap_t
 *      struct and its items[] array -- the old pointer must never be touched
 *      again, so every loop reassigns its `h` from the re-resolve and only
 *      proceeds when it is non-NULL.
 *   3. A `busy` flag is set for the duration of a push()/pop() sift and
 *      checked at the top of both: a comparator that reenters push()/pop() on
 *      the SAME heap (rather than closing it) is rejected with a clean throw
 *      instead of being allowed to resize/mutate items[] out from under the
 *      in-progress sift (which would corrupt indices, not just crash).
 *
 * peek()/size/length never call the comparator and need none of this.
 *
 * A comparator that simply THROWS (without closing) is safe by the same
 * mechanism: dyn_heap_sift_up/down propagate the failure and stop touching
 * the heap. Note what that does and does not guarantee: push() appends and
 * counts its new element BEFORE the sift, so a throw mid-sift leaves that
 * element present exactly once (never lost or duplicated) but possibly not
 * fully bubbled into place -- the same "partially completed, not corrupted"
 * contract Array.prototype.sort has for a throwing comparator. A later,
 * successful pop()/push() is not guaranteed to immediately resume perfectly
 * ascending order from such a state, only to keep working correctly and to
 * never lose or duplicate an element.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CONTAINER)

#include <stdint.h>
#include <stdlib.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ================= Heap: binary heap ordered by a JS comparator ========= */

typedef struct {
    JSRuntime *rt;    /* cached at construction so dispose() can free values */
    JSValue cmp;      /* dup'd comparator function; borrowed during calls */
    JSValue *items;   /* binary-heap array; items[0] is the root (min per cmp) */
    uint32_t count;
    uint32_t cap;
    int busy;         /* 1 while push()/pop() is invoking the comparator: see
                       * the module header for why this must block reentrant
                       * push()/pop() on the same heap. */
} dyn_heap_t;

/* Grow so at least `need` slots exist; same doubling + overflow-guard shape
 * as dyna-structures.c's dyn_vector_reserve. Returns 0 or -1. */
static int dyn_heap_reserve(dyn_heap_t *h, uint32_t need)
{
    uint32_t ncap;
    JSValue *ni;

    if (need <= h->cap)
        return 0;
    if (need > UINT32_MAX / (uint32_t)sizeof(JSValue))
        return -1;
    ncap = h->cap ? h->cap : 8;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2 ||
            ncap > UINT32_MAX / (uint32_t)sizeof(JSValue) / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    ni = (JSValue *)realloc(h->items, (size_t)ncap * sizeof(JSValue));
    if (!ni)
        return -1;
    h->items = ni;
    h->cap = ncap;
    return 0;
}

static void dyn_heap_free(void *native)
{
    dyn_heap_t *h = (dyn_heap_t *)native;
    uint32_t i;

    if (!h)
        return;
    for (i = 0; i < h->count; i++)
        JS_FreeValueRT(h->rt, h->items[i]);
    JS_FreeValueRT(h->rt, h->cmp);
    free(h->items);
    free(h);
}

static JSClassID dyn_heap_class_id;

static const JSClassDef dyn_heap_class = {
    "Heap",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_heap_ctor(JSContext *ctx, JSValueConst new_target,
                             int argc, JSValueConst *argv)
{
    dyn_heap_t *h;

    (void)new_target;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Heap(comparator) requires a comparator function");
    h = (dyn_heap_t *)malloc(sizeof(*h));
    if (!h)
        return JS_ThrowOutOfMemory(ctx);
    h->rt = JS_GetRuntime(ctx);
    h->cmp = JS_DupValue(ctx, argv[0]);
    h->items = NULL;
    h->count = 0;
    h->cap = 0;
    h->busy = 0;
    return dyn_res_wrap(ctx, dyn_heap_class_id, h, dyn_heap_free);
}

/* Invoke the user comparator cmp(a, b). Once *err is set, returns 0 without
 * calling JS again. Dups cmp/a/b for the duration of the call -- see the
 * module header rule 1 for why this is required (not just defensive) for a
 * STORED comparator, unlike dyna-sort.c's transient one. A NaN or 0 result
 * maps to "equal", matching Array.prototype.sort / dyna-sort.c. */
static int dyn_heap_cmp_call(JSContext *ctx, JSValueConst cmp,
                             JSValueConst a, JSValueConst b, int *err)
{
    JSValueConst args[2];
    JSValue dcmp, da, db, r;
    double d;

    if (*err)
        return 0;
    dcmp = JS_DupValue(ctx, cmp);
    da = JS_DupValue(ctx, a);
    db = JS_DupValue(ctx, b);
    args[0] = da;
    args[1] = db;
    r = JS_Call(ctx, dcmp, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, dcmp);
    JS_FreeValue(ctx, da);
    JS_FreeValue(ctx, db);
    if (JS_IsException(r)) {
        *err = 1;
        return 0;
    }
    if (JS_ToFloat64(ctx, &d, r)) {
        JS_FreeValue(ctx, r);
        *err = 1;
        return 0;
    }
    JS_FreeValue(ctx, r);
    if (d < 0)
        return -1;
    if (d > 0)
        return 1;
    return 0;
}

/* Sift items[idx] up toward the root while it compares less than its parent.
 * Returns 0 on success, -1 with an exception pending (comparator threw, or
 * the heap was closed by the comparator -- module header rules 1+2). `h`
 * must be the CURRENTLY resolved, open resource for `this_val`. */
static int dyn_heap_sift_up(JSContext *ctx, JSValueConst this_val,
                            dyn_heap_t *h, uint32_t idx)
{
    int err = 0;

    h->busy = 1;
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        int c = dyn_heap_cmp_call(ctx, h->cmp, h->items[idx], h->items[parent],
                                  &err);
        /* Re-resolve unconditionally: the comparator may have closed the
         * heap (freeing h and h->items) regardless of c/err. A successful
         * re-resolve also proves h's address is unchanged (the struct is
         * malloc'd once and never moved while open). */
        h = (dyn_heap_t *)dyn_res_native(ctx, this_val, dyn_heap_class_id);
        if (!h)
            return -1;
        if (err) {
            h->busy = 0;
            return -1;
        }
        if (c >= 0)
            break; /* heap property holds */
        {
            JSValue tmp = h->items[idx];
            h->items[idx] = h->items[parent];
            h->items[parent] = tmp;
        }
        idx = parent;
    }
    h->busy = 0;
    return 0;
}

/* Sift items[idx] down toward the leaves, swapping with the smaller child
 * while that child compares less. Same contract as dyn_heap_sift_up. */
static int dyn_heap_sift_down(JSContext *ctx, JSValueConst this_val,
                              dyn_heap_t *h, uint32_t idx)
{
    int err = 0;

    h->busy = 1;
    for (;;) {
        uint32_t l = 2 * idx + 1, r = 2 * idx + 2, smallest = idx;
        int c;

        if (l < h->count) {
            c = dyn_heap_cmp_call(ctx, h->cmp, h->items[l], h->items[smallest],
                                  &err);
            h = (dyn_heap_t *)dyn_res_native(ctx, this_val, dyn_heap_class_id);
            if (!h)
                return -1;
            if (err) {
                h->busy = 0;
                return -1;
            }
            if (c < 0)
                smallest = l;
        }
        if (r < h->count) {
            c = dyn_heap_cmp_call(ctx, h->cmp, h->items[r], h->items[smallest],
                                  &err);
            h = (dyn_heap_t *)dyn_res_native(ctx, this_val, dyn_heap_class_id);
            if (!h)
                return -1;
            if (err) {
                h->busy = 0;
                return -1;
            }
            if (c < 0)
                smallest = r;
        }
        if (smallest == idx)
            break;
        {
            JSValue tmp = h->items[idx];
            h->items[idx] = h->items[smallest];
            h->items[smallest] = tmp;
        }
        idx = smallest;
    }
    h->busy = 0;
    return 0;
}

/* push(v) -> new size. The value is stored verbatim (no coercion runs user
 * JS before we store it), so resolving before the dup is safe here -- the
 * risk is entirely in the comparator invoked by the sift below. */
static JSValue dyn_heap_push(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_heap_t *h;
    uint32_t idx;

    (void)argc;
    h = (dyn_heap_t *)dyn_res_native(ctx, this_val, dyn_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->busy)
        return JS_ThrowTypeError(ctx, "Heap: comparator must not push/pop its own heap");
    if (dyn_heap_reserve(h, h->count + 1))
        return JS_ThrowOutOfMemory(ctx);
    h->items[h->count] = JS_DupValue(ctx, argv[0]);
    idx = h->count;
    h->count++;
    if (dyn_heap_sift_up(ctx, this_val, h, idx))
        return JS_EXCEPTION;
    /* dyn_heap_sift_up returning 0 proves the heap was never closed during
     * the sift, so `h` (this function's own local, unaffected by sift_up's
     * copy of the pointer) is still valid to read. */
    return JS_NewInt64(ctx, (int64_t)h->count);
}

/* pop() -> the minimum element (per comparator), or undefined if empty. */
static JSValue dyn_heap_pop(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    dyn_heap_t *h;
    JSValue top;

    (void)argc; (void)argv;
    h = (dyn_heap_t *)dyn_res_native(ctx, this_val, dyn_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->busy)
        return JS_ThrowTypeError(ctx, "Heap: comparator must not push/pop its own heap");
    if (h->count == 0)
        return JS_UNDEFINED;
    /* Ownership of the root transfers to `top` (no dup, no free): count is
     * decremented and the slot overwritten before any comparator runs, so
     * dispose() during the sift below can never see (and re-free) it. */
    top = h->items[0];
    h->count--;
    if (h->count > 0) {
        h->items[0] = h->items[h->count];
        if (dyn_heap_sift_down(ctx, this_val, h, 0)) {
            JS_FreeValue(ctx, top); /* operation failed: we still own `top` */
            return JS_EXCEPTION;
        }
    }
    return top;
}

/* peek() -> the minimum element without removing it, or undefined if empty.
 * Never calls the comparator, so no busy check is needed. */
static JSValue dyn_heap_peek(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_heap_t *h;

    (void)argc; (void)argv;
    h = (dyn_heap_t *)dyn_res_native(ctx, this_val, dyn_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->count == 0)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, h->items[0]);
}

static JSValue dyn_heap_size(JSContext *ctx, JSValueConst this_val)
{
    dyn_heap_t *h = (dyn_heap_t *)dyn_res_native(ctx, this_val, dyn_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)h->count);
}

static const JSCFunctionListEntry dyn_heap_proto[] = {
    JS_CFUNC_DEF("push", 1, dyn_heap_push),
    JS_CFUNC_DEF("pop", 0, dyn_heap_pop),
    JS_CFUNC_DEF("peek", 0, dyn_heap_peek),
    JS_CGETSET_DEF("size", dyn_heap_size, NULL),
    JS_CGETSET_DEF("length", dyn_heap_size, NULL),
};

/* ================= List: doubly-linked list ============================= */

typedef struct DynListNode {
    struct DynListNode *prev, *next;
    JSValue value;
} DynListNode;

typedef struct {
    JSRuntime *rt;
    DynListNode *head, *tail;
    uint32_t length;
} dyn_list_t;

static void dyn_list_free(void *native)
{
    dyn_list_t *l = (dyn_list_t *)native;
    DynListNode *n, *next;

    if (!l)
        return;
    for (n = l->head; n; n = next) {
        next = n->next;
        JS_FreeValueRT(l->rt, n->value);
        free(n);
    }
    free(l);
}

static JSClassID dyn_list_class_id;

static const JSClassDef dyn_list_class = {
    "List",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_list_ctor(JSContext *ctx, JSValueConst new_target,
                             int argc, JSValueConst *argv)
{
    dyn_list_t *l;

    (void)new_target; (void)argc; (void)argv;
    l = (dyn_list_t *)malloc(sizeof(*l));
    if (!l)
        return JS_ThrowOutOfMemory(ctx);
    l->rt = JS_GetRuntime(ctx);
    l->head = NULL;
    l->tail = NULL;
    l->length = 0;
    return dyn_res_wrap(ctx, dyn_list_class_id, l, dyn_list_free);
}

static JSValue dyn_list_push_front(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_list_t *l;
    DynListNode *n;

    (void)argc;
    l = (dyn_list_t *)dyn_res_native(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    n = (DynListNode *)malloc(sizeof(*n));
    if (!n)
        return JS_ThrowOutOfMemory(ctx);
    n->value = JS_DupValue(ctx, argv[0]);
    n->prev = NULL;
    n->next = l->head;
    if (l->head)
        l->head->prev = n;
    else
        l->tail = n;
    l->head = n;
    l->length++;
    return JS_NewInt64(ctx, (int64_t)l->length);
}

static JSValue dyn_list_push_back(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_list_t *l;
    DynListNode *n;

    (void)argc;
    l = (dyn_list_t *)dyn_res_native(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    n = (DynListNode *)malloc(sizeof(*n));
    if (!n)
        return JS_ThrowOutOfMemory(ctx);
    n->value = JS_DupValue(ctx, argv[0]);
    n->next = NULL;
    n->prev = l->tail;
    if (l->tail)
        l->tail->next = n;
    else
        l->head = n;
    l->tail = n;
    l->length++;
    return JS_NewInt64(ctx, (int64_t)l->length);
}

static JSValue dyn_list_pop_front(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_list_t *l;
    DynListNode *n;
    JSValue v;

    (void)argc; (void)argv;
    l = (dyn_list_t *)dyn_res_native(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    if (!l->head)
        return JS_UNDEFINED;
    n = l->head;
    l->head = n->next;
    if (l->head)
        l->head->prev = NULL;
    else
        l->tail = NULL;
    l->length--;
    v = n->value; /* ownership transfers to the caller: no dup, no free */
    free(n);
    return v;
}

static JSValue dyn_list_pop_back(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_list_t *l;
    DynListNode *n;
    JSValue v;

    (void)argc; (void)argv;
    l = (dyn_list_t *)dyn_res_native(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    if (!l->tail)
        return JS_UNDEFINED;
    n = l->tail;
    l->tail = n->prev;
    if (l->tail)
        l->tail->next = NULL;
    else
        l->head = NULL;
    l->length--;
    v = n->value;
    free(n);
    return v;
}

static JSValue dyn_list_front(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_list_t *l;

    (void)argc; (void)argv;
    l = (dyn_list_t *)dyn_res_native(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    if (!l->head)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, l->head->value);
}

static JSValue dyn_list_back(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_list_t *l;

    (void)argc; (void)argv;
    l = (dyn_list_t *)dyn_res_native(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    if (!l->tail)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, l->tail->value);
}

static JSValue dyn_list_length(JSContext *ctx, JSValueConst this_val)
{
    dyn_list_t *l = (dyn_list_t *)dyn_res_native(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)l->length);
}

/* toArray() -> fresh Array snapshot, head to tail, each element dup'd.
 * Walking the list touches no user JS (JS_DupValue / defining a property on
 * a fresh plain Array never invoke JS), so this needs no reentrancy care. */
static JSValue dyn_list_to_array(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_list_t *l;
    DynListNode *n;
    JSValue arr;
    uint32_t i;

    (void)argc; (void)argv;
    l = (dyn_list_t *)dyn_res_native(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    i = 0;
    for (n = l->head; n; n = n->next) {
        if (JS_DefinePropertyValueUint32(ctx, arr, i++, JS_DupValue(ctx, n->value),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

/* [Symbol.iterator]() -> delegate to a fresh snapshot Array's own iterator
 * (Array.prototype.values, the same function object Array's own
 * [Symbol.iterator] aliases to). Simplest correct option: iteration sees a
 * stable snapshot rather than a live (and harder to reason about under
 * concurrent mutation) view. */
static JSValue dyn_list_iterator(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue arr, fn, iter;

    arr = dyn_list_to_array(ctx, this_val, argc, argv);
    if (JS_IsException(arr))
        return arr;
    fn = JS_GetPropertyStr(ctx, arr, "values");
    if (JS_IsException(fn)) {
        JS_FreeValue(ctx, arr);
        return fn;
    }
    iter = JS_Call(ctx, fn, arr, 0, NULL);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, arr);
    return iter;
}

static const JSCFunctionListEntry dyn_list_proto[] = {
    JS_CFUNC_DEF("pushFront", 1, dyn_list_push_front),
    JS_CFUNC_DEF("pushBack", 1, dyn_list_push_back),
    JS_CFUNC_DEF("popFront", 0, dyn_list_pop_front),
    JS_CFUNC_DEF("popBack", 0, dyn_list_pop_back),
    JS_CFUNC_DEF("front", 0, dyn_list_front),
    JS_CFUNC_DEF("back", 0, dyn_list_back),
    JS_CFUNC_DEF("toArray", 0, dyn_list_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_list_iterator),
    JS_CGETSET_DEF("length", dyn_list_length, NULL),
};

/* ================= Ring: fixed-capacity circular buffer ================= */

typedef struct {
    JSRuntime *rt;
    JSValue *items;  /* fixed-size storage, length == cap, every slot always
                      * holds a valid JSValue (JS_UNDEFINED or a dup'd one) */
    uint32_t cap;    /* fixed capacity, > 0, set once at construction */
    uint32_t start;  /* physical index of the logically-oldest element */
    uint32_t count;  /* number of valid elements, 0..cap */
} dyn_ring_t;

static void dyn_ring_free(void *native)
{
    dyn_ring_t *g = (dyn_ring_t *)native;
    uint32_t i;

    if (!g)
        return;
    for (i = 0; i < g->cap; i++)
        JS_FreeValueRT(g->rt, g->items[i]);
    free(g->items);
    free(g);
}

static JSClassID dyn_ring_class_id;

static const JSClassDef dyn_ring_class = {
    "Ring",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_ring_ctor(JSContext *ctx, JSValueConst new_target,
                             int argc, JSValueConst *argv)
{
    dyn_ring_t *g;
    int64_t n;
    uint32_t cap, i;

    (void)new_target;
    if (argc < 1 || JS_ToInt64(ctx, &n, argv[0]))
        return JS_ThrowTypeError(ctx, "Ring(capacity) requires an integer capacity");
    if (n <= 0 || (uint64_t)n > UINT32_MAX / (uint32_t)sizeof(JSValue))
        return JS_ThrowRangeError(ctx, "Ring: capacity out of range");
    cap = (uint32_t)n;

    g = (dyn_ring_t *)malloc(sizeof(*g));
    if (!g)
        return JS_ThrowOutOfMemory(ctx);
    g->items = (JSValue *)malloc((size_t)cap * sizeof(JSValue));
    if (!g->items) {
        free(g);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < cap; i++)
        g->items[i] = JS_UNDEFINED;
    g->rt = JS_GetRuntime(ctx);
    g->cap = cap;
    g->start = 0;
    g->count = 0;
    return dyn_res_wrap(ctx, dyn_ring_class_id, g, dyn_ring_free);
}

/* push(v) -> new length. Overwrites the oldest element once the ring is
 * full. The value is stored verbatim, so resolving before the dup is safe
 * (no coercion of `v` runs user JS). */
static JSValue dyn_ring_push(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_ring_t *g;
    uint32_t phys;

    (void)argc;
    g = (dyn_ring_t *)dyn_res_native(ctx, this_val, dyn_ring_class_id);
    if (!g)
        return JS_EXCEPTION;
    if (g->count < g->cap) {
        phys = (g->start + g->count) % g->cap;
        g->items[phys] = JS_DupValue(ctx, argv[0]);
        g->count++;
    } else {
        phys = g->start;
        JS_FreeValueRT(g->rt, g->items[phys]);
        g->items[phys] = JS_DupValue(ctx, argv[0]);
        g->start = (g->start + 1) % g->cap;
    }
    return JS_NewInt64(ctx, (int64_t)g->count);
}

/* get(i) -> the i-th logical element (0 = oldest), or undefined if out of
 * range. Coerces the index FIRST (a valueOf could close() this), then
 * resolves -- mirrors Vector.get in dyna-structures.c exactly. */
static JSValue dyn_ring_get(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    dyn_ring_t *g;
    int64_t i;

    (void)argc;
    if (JS_ToInt64(ctx, &i, argv[0]))
        return JS_EXCEPTION;
    g = (dyn_ring_t *)dyn_res_native(ctx, this_val, dyn_ring_class_id);
    if (!g)
        return JS_EXCEPTION;
    if (i < 0 || (uint64_t)i >= g->count)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, g->items[(g->start + (uint32_t)i) % g->cap]);
}

static JSValue dyn_ring_length(JSContext *ctx, JSValueConst this_val)
{
    dyn_ring_t *g = (dyn_ring_t *)dyn_res_native(ctx, this_val, dyn_ring_class_id);
    if (!g)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)g->count);
}

static JSValue dyn_ring_capacity(JSContext *ctx, JSValueConst this_val)
{
    dyn_ring_t *g = (dyn_ring_t *)dyn_res_native(ctx, this_val, dyn_ring_class_id);
    if (!g)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)g->cap);
}

/* toArray() -> fresh Array snapshot, oldest to newest, each element dup'd. */
static JSValue dyn_ring_to_array(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_ring_t *g;
    JSValue arr;
    uint32_t i;

    (void)argc; (void)argv;
    g = (dyn_ring_t *)dyn_res_native(ctx, this_val, dyn_ring_class_id);
    if (!g)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < g->count; i++) {
        JSValue v = JS_DupValue(ctx, g->items[(g->start + i) % g->cap]);
        if (JS_DefinePropertyValueUint32(ctx, arr, i, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static const JSCFunctionListEntry dyn_ring_proto[] = {
    JS_CFUNC_DEF("push", 1, dyn_ring_push),
    JS_CFUNC_DEF("get", 1, dyn_ring_get),
    JS_CFUNC_DEF("toArray", 0, dyn_ring_to_array),
    JS_CGETSET_DEF("length", dyn_ring_length, NULL),
    JS_CGETSET_DEF("capacity", dyn_ring_capacity, NULL),
};

/* ================= module registration ================= */

static int dyn_container_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_heap_class_id, &dyn_heap_class,
                           dyn_heap_proto, countof(dyn_heap_proto),
                           dyn_heap_ctor, "Heap") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_list_class_id, &dyn_list_class,
                           dyn_list_proto, countof(dyn_list_proto),
                           dyn_list_ctor, "List") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_ring_class_id, &dyn_ring_class,
                           dyn_ring_proto, countof(dyn_ring_proto),
                           dyn_ring_ctor, "Ring") < 0)
        return -1;
    return 0;
}

int js_nat_init_container(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:container",
                                  dyn_container_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Heap");
    JS_AddModuleExport(ctx, m, "List");
    JS_AddModuleExport(ctx, m, "Ring");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_CONTAINER */
