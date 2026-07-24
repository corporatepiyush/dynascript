/* Array.prototype: standard methods + SugarJS/RamdaJS non-ES extensions (SUGAR_RAMDA_NATIVE.md).
 *
 * Unity-build fragment: #included into src/dynajs.c, never compiled alone.
 * Split out of the former object_array_iterator.inc.c (byte-identical token
 * stream preserved; see MODULARIZATION.md). */
/* Build a fresh array from obj[start..end) (both already clamped, start<=end).
 * Uses a pre-sized fast array + a direct bulk dup when the source is a fast
 * array (the common case; matches js_array_slice's speed), falling back to the
 * generic per-index read otherwise. js_allocate_fast_array pre-fills every slot
 * with JS_UNDEFINED, so bailing mid-fill and freeing the array is safe. */
static JSValue js_array_ext_build_range(JSContext *ctx, JSValueConst obj,
                                        int64_t start, int64_t end)
{
    JSValue arr, *arrp, *pval;
    JSObject *p;
    int64_t i, n = end - start;
    uint32_t count32;

    if (n <= 0)
        return JS_NewArray(ctx);
    arr = js_allocate_fast_array(ctx, n);
    if (JS_IsException(arr))
        return arr;
    p = JS_VALUE_GET_OBJ(arr);
    pval = p->u.array.u.values;
    if (js_get_fast_array(ctx, obj, &arrp, &count32) && (int64_t)count32 >= end) {
        for (i = start; i < end; i++, pval++)
            *pval = JS_DupValue(ctx, arrp[i]);
    } else {
        for (i = start; i < end; i++, pval++) {
            if (JS_TryGetPropertyInt64(ctx, obj, i, pval) < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        }
    }
    return arr;
}

/* _isEmpty() -> length === 0 */
static JSValue js_array_ext_isEmpty(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj;
    int64_t len;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, obj);
    return JS_NewBool(ctx, len == 0);
}

/* _first(n?) -> first element (undefined if empty), or a new array of the first
 * n elements when n is given. */
static JSValue js_array_ext_first(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, n;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        if (len == 0)
            ret = JS_UNDEFINED;
        else if (js_array_ext_getel(ctx, obj, 0, &ret))
            ret = JS_EXCEPTION;
        goto done;
    }
    if (JS_ToInt64Sat(ctx, &n, argv[0]))
        goto done;
    if (n < 0)
        n = 0;
    if (n > len)
        n = len;
    ret = js_array_ext_build_range(ctx, obj, 0, n);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _last(n?) -> last element (undefined if empty), or a new array of the last n
 * elements (in original order) when n is given. */
static JSValue js_array_ext_last(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, n;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        if (len == 0)
            ret = JS_UNDEFINED;
        else if (js_array_ext_getel(ctx, obj, len - 1, &ret))
            ret = JS_EXCEPTION;
        goto done;
    }
    if (JS_ToInt64Sat(ctx, &n, argv[0]))
        goto done;
    if (n < 0)
        n = 0;
    if (n > len)
        n = len;
    ret = js_array_ext_build_range(ctx, obj, len - n, len);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* shared reducer for _sum / _average: accumulate each element coerced to a
 * double. magic 0 = sum, 1 = average (empty average is 0). */
static JSValue js_array_ext_sum_avg(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, i;
    double acc = 0;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    {   /* fast path: a contiguous all-numeric fast array. The homogeneity scan
         * and the sum loop read only tags/payloads and run NO user JS, so
         * holding arrp across them cannot use-after-free. */
        JSValue *arrp;
        uint32_t count;
        if (js_get_fast_array(ctx, obj, &arrp, &count) && (int64_t)count == len) {
            int homogeneous = 1;
            for (i = 0; i < len; i++) {
                int t = JS_VALUE_GET_TAG(arrp[i]);
                if (t != JS_TAG_INT && !JS_TAG_IS_FLOAT64(t)) { homogeneous = 0; break; }
            }
            if (homogeneous) {
                for (i = 0; i < len; i++) {
                    JSValue v = arrp[i];
                    acc += (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
                             ? (double)JS_VALUE_GET_INT(v) : JS_VALUE_GET_FLOAT64(v);
                }
                if (magic == 1) acc = len ? acc / (double)len : 0;
                ret = JS_NewFloat64(ctx, acc);
                goto done;
            }
        }
    }
    for (i = 0; i < len; i++) {
        JSValue v;
        double d;
        int r;
        if (js_array_ext_getel(ctx, obj, i, &v))
            goto done;
        r = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (r)
            goto done;
        acc += d;
    }
    if (magic == 1)
        acc = len ? acc / (double)len : 0;
    ret = JS_NewFloat64(ctx, acc);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _compact() -> a new array with null and undefined removed. */
static JSValue js_array_ext_compact(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, a, ret = JS_EXCEPTION;
    int64_t len, i, j = 0;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue v;
        if (js_array_ext_getel(ctx, obj, i, &v)) {
            JS_FreeValue(ctx, a);
            goto done;
        }
        if (JS_IsNull(v) || JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        if (JS_DefinePropertyValueInt64(ctx, a, j++, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            goto done;
        }
    }
    ret = a;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* Apply a Sugar/Ramda "mapper" to an element: undefined -> the element itself;
 * a function -> fn(element); anything else -> element[key] (a property name).
 * Returns an owned JSValue or JS_EXCEPTION. `el` is borrowed. */
static JSValue js_array_ext_mapval(JSContext *ctx, JSValueConst map,
                                   JSValueConst el)
{
    if (JS_IsUndefined(map))
        return JS_DupValue(ctx, el);
    if (JS_IsFunction(ctx, map))
        return JS_Call(ctx, map, JS_UNDEFINED, 1, &el);
    {
        JSAtom a = JS_ValueToAtom(ctx, map);
        JSValue v;
        if (a == JS_ATOM_NULL)
            return JS_EXCEPTION;
        v = JS_GetProperty(ctx, el, a);
        JS_FreeAtom(ctx, a);
        return v;
    }
}

/* Prepared "matcher" — the overloaded dispatch shared by every matcher method
 * (_count/_none/_any/_all/_partition/_reject/_takeWhile/_dropWhile/...). The
 * matcher KIND is resolved ONCE (js_ext_matcher_begin) so the per-element test
 * has no re-resolution cost; in particular a RegExp's `.test` method is looked
 * up a single time, not once per element. Kinds: a function -> ToBool(fn(el));
 * a RegExp -> regex.test(String(el)) (Sugar overload) — detected by class id so
 * a duck-typed {test(){}} is NOT a regex; otherwise SameValueZero(matcher,el). */
typedef struct JSExtMatcher {
    JSValueConst matcher;   /* borrowed */
    JSValue regex_test;     /* owned; JS_UNDEFINED unless kind == 2 */
    int kind;               /* 0 = value, 1 = function, 2 = RegExp */
} JSExtMatcher;

static int js_ext_matcher_begin(JSContext *ctx, JSExtMatcher *pm,
                                JSValueConst matcher)
{
    pm->matcher = matcher;
    pm->regex_test = JS_UNDEFINED;
    if (JS_IsFunction(ctx, matcher)) {
        pm->kind = 1;
    } else if (JS_VALUE_GET_TAG(matcher) == JS_TAG_OBJECT &&
               JS_VALUE_GET_OBJ(matcher)->class_id == JS_CLASS_REGEXP) {
        pm->kind = 2;
        pm->regex_test = JS_GetPropertyStr(ctx, matcher, "test");
        if (JS_IsException(pm->regex_test)) { pm->regex_test = JS_UNDEFINED; return -1; }
    } else {
        pm->kind = 0;
    }
    return 0;
}

/* Returns 1 (match), 0 (no match), or -1 (exception). */
static int js_ext_matcher_test(JSContext *ctx, JSExtMatcher *pm, JSValueConst el)
{
    switch (pm->kind) {
    case 1: {
        JSValue r = JS_Call(ctx, pm->matcher, JS_UNDEFINED, 1, &el);
        int b;
        if (JS_IsException(r))
            return -1;
        b = JS_ToBool(ctx, r);
        JS_FreeValue(ctx, r);
        return b;
    }
    case 2: {
        JSValue str = JS_ToString(ctx, el), r;
        int b;
        if (JS_IsException(str))
            return -1;
        r = JS_Call(ctx, pm->regex_test, pm->matcher, 1, (JSValueConst *)&str);
        JS_FreeValue(ctx, str);
        if (JS_IsException(r))
            return -1;
        b = JS_ToBool(ctx, r);
        JS_FreeValue(ctx, r);
        return b;
    }
    default:
        return JS_SameValueZero(ctx, pm->matcher, el) ? 1 : 0;
    }
}

static void js_ext_matcher_end(JSContext *ctx, JSExtMatcher *pm)
{
    JS_FreeValue(ctx, pm->regex_test);
    pm->regex_test = JS_UNDEFINED;
}

/* _count(match?) -> length with no argument; else the number of elements the
 * matcher accepts (a value by SameValueZero, or a predicate function). */
static JSValue js_array_ext_count(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i, c = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        ret = JS_NewInt64(ctx, len);
        goto done;
    }
    if (js_ext_matcher_begin(ctx, &pm, argv[0]))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el))
            goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        JS_FreeValue(ctx, el);
        if (m < 0)
            goto done;
        c += m;
    }
    ret = JS_NewInt64(ctx, c);
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _none / _any / _all (magic 0/1/2) against a value or predicate matcher. */
static JSValue js_array_ext_quantify(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    JSValue obj, ret = JS_EXCEPTION;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i;
    JSValueConst match = argc > 0 ? argv[0] : JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (js_ext_matcher_begin(ctx, &pm, match))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el))
            goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        JS_FreeValue(ctx, el);
        if (m < 0)
            goto done;
        if (magic == 1 && m) { ret = JS_TRUE;  goto done; } /* any: found  */
        if (magic == 0 && m) { ret = JS_FALSE; goto done; } /* none: found */
        if (magic == 2 && !m){ ret = JS_FALSE; goto done; } /* all: missed */
    }
    ret = (magic == 1) ? JS_FALSE : JS_TRUE; /* any->false, none/all->true */
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _min / _max (magic 0/1): return the ELEMENT whose mapped value (undefined =
 * identity, a function, or a property name) is numerically smallest/largest
 * (first on a tie). Empty -> undefined. */
static JSValue js_array_ext_minmax(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    JSValue obj, best = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst map = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;
    double best_key = 0;
    int have = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto fail;
    if (JS_IsUndefined(map)) {
        /* fast path: no mapper + a contiguous all-numeric fast array. No user JS
         * runs, so holding arrp is safe; returns the winning element directly. */
        JSValue *arrp;
        uint32_t count;
        if (js_get_fast_array(ctx, obj, &arrp, &count) && (int64_t)count == len && len > 0) {
            int homogeneous = 1;
            for (i = 0; i < len; i++) {
                int t = JS_VALUE_GET_TAG(arrp[i]);
                if (t != JS_TAG_INT && !JS_TAG_IS_FLOAT64(t)) { homogeneous = 0; break; }
            }
            if (homogeneous) {
                int64_t bi = 0;
                double bk = (JS_VALUE_GET_TAG(arrp[0]) == JS_TAG_INT)
                              ? (double)JS_VALUE_GET_INT(arrp[0]) : JS_VALUE_GET_FLOAT64(arrp[0]);
                for (i = 1; i < len; i++) {
                    JSValue v = arrp[i];
                    double d = (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
                                 ? (double)JS_VALUE_GET_INT(v) : JS_VALUE_GET_FLOAT64(v);
                    if (magic == 0 ? d < bk : d > bk) { bk = d; bi = i; }
                }
                ret = JS_DupValue(ctx, arrp[bi]);
                JS_FreeValue(ctx, obj);
                return ret;
            }
        }
    }
    for (i = 0; i < len; i++) {
        JSValue el, key;
        double d;
        int r;
        if (js_array_ext_getel(ctx, obj, i, &el))
            goto fail;
        key = js_array_ext_mapval(ctx, map, el);
        if (JS_IsException(key)) {
            JS_FreeValue(ctx, el);
            goto fail;
        }
        r = JS_ToFloat64(ctx, &d, key);
        JS_FreeValue(ctx, key);
        if (r) {
            JS_FreeValue(ctx, el);
            goto fail;
        }
        if (!have || (magic == 0 ? d < best_key : d > best_key)) {
            JS_FreeValue(ctx, best);
            best = el;
            best_key = d;
            have = 1;
        } else {
            JS_FreeValue(ctx, el);
        }
    }
    ret = best;          /* transfer ownership (JS_UNDEFINED if empty) */
    best = JS_UNDEFINED;
 fail:
    JS_FreeValue(ctx, best);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _take / _drop / _takeLast / _dropLast (magic 0/1/2/3) -> a new array. n is
 * clamped to [0,len]; a negative or missing n is treated as 0. */
static JSValue js_array_ext_take(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, n;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (JS_ToInt64Sat(ctx, &n, argc > 0 ? argv[0] : JS_UNDEFINED))
        goto done;
    if (n < 0)
        n = 0;
    if (n > len)
        n = len;
    switch (magic) {
    case 0: ret = js_array_ext_build_range(ctx, obj, 0, n); break;       /* take */
    case 1: ret = js_array_ext_build_range(ctx, obj, n, len); break;     /* drop */
    case 2: ret = js_array_ext_build_range(ctx, obj, len - n, len); break;/* takeLast */
    default:ret = js_array_ext_build_range(ctx, obj, 0, len - n); break; /* dropLast */
    }
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* ---- _sortBy: decorate / stable-merge-sort / undecorate ---- */
typedef struct {
    JSValue val;      /* owned element */
    double dkey;      /* numeric sort key (is_num) */
    char *skey;       /* string sort key, owned (else) */
    uint32_t idx;     /* original index (unused: merge sort is already stable) */
    int is_num;
} DynSortItem;

static int dyn_sortby_cmp(const DynSortItem *a, const DynSortItem *b)
{
    if (a->is_num && b->is_num)
        return a->dkey < b->dkey ? -1 : a->dkey > b->dkey ? 1 : 0;
    if (!a->is_num && !b->is_num)
        return strcmp(a->skey, b->skey);
    return a->is_num ? -1 : 1; /* numbers sort before strings */
}

/* stable bottom-up merge sort; `desc` reverses the order (ties keep original
 * order in both directions). Returns 0, or -1 on OOM. */
static int dyn_sortby_sort(JSContext *ctx, DynSortItem *items, int64_t n, int desc)
{
    DynSortItem *tmp;
    int64_t width;
    if (n < 2)
        return 0;
    tmp = js_malloc(ctx, (size_t)n * sizeof(*tmp));
    if (!tmp)
        return -1;
    for (width = 1; width < n; width *= 2) {
        int64_t i;
        for (i = 0; i < n; i += 2 * width) {
            int64_t mid = i + width < n ? i + width : n;
            int64_t hi = i + 2 * width < n ? i + 2 * width : n;
            int64_t l = i, r = mid, k = i;
            while (l < mid && r < hi) {
                int c = dyn_sortby_cmp(&items[l], &items[r]);
                if (desc)
                    c = -c;
                tmp[k++] = (c <= 0) ? items[l++] : items[r++]; /* stable: left on tie */
            }
            while (l < mid) tmp[k++] = items[l++];
            while (r < hi)  tmp[k++] = items[r++];
        }
        memcpy(items, tmp, (size_t)n * sizeof(*tmp));
    }
    js_free(ctx, tmp);
    return 0;
}

static void dyn_sortby_free(JSContext *ctx, DynSortItem *items, int64_t n)
{
    int64_t i;
    for (i = 0; i < n; i++) {
        JS_FreeValue(ctx, items[i].val);
        js_free(ctx, items[i].skey);
    }
    js_free(ctx, items);
}

/* _sortBy(map?, desc?) -> a new array sorted by the mapped value (identity /
 * function / property-name). Numeric keys compare numerically, others by their
 * string form (byte order); stable; desc reverses. */
static JSValue js_array_ext_sortby(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, arr, *pval, ret = JS_EXCEPTION;
    JSValueConst map = argc > 0 ? argv[0] : JS_UNDEFINED;
    DynSortItem *items = NULL;
    JSObject *p;
    int64_t len, i;
    int desc = argc > 1 ? JS_ToBool(ctx, argv[1]) : 0;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (len == 0) { ret = JS_NewArray(ctx); goto done; }

    items = js_mallocz(ctx, (size_t)len * sizeof(*items));
    if (!items) { JS_ThrowOutOfMemory(ctx); goto done; }
    for (i = 0; i < len; i++) {
        JSValue el, key;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto fail;
        items[i].val = el;               /* owned; cleanup frees it */
        items[i].idx = (uint32_t)i;
        key = js_array_ext_mapval(ctx, map, el);
        if (JS_IsException(key)) goto fail;
        if (JS_IsNumber(key)) {
            items[i].is_num = 1;
            JS_ToFloat64(ctx, &items[i].dkey, key);
            JS_FreeValue(ctx, key);
        } else {
            const char *s = JS_ToCString(ctx, key);
            JS_FreeValue(ctx, key);
            if (!s) goto fail;
            items[i].skey = js_strdup(ctx, s);
            JS_FreeCString(ctx, s);
            if (!items[i].skey) { JS_ThrowOutOfMemory(ctx); goto fail; }
        }
    }
    if (dyn_sortby_sort(ctx, items, len, desc)) { JS_ThrowOutOfMemory(ctx); goto fail; }

    arr = js_allocate_fast_array(ctx, len);
    if (JS_IsException(arr)) goto fail;
    p = JS_VALUE_GET_OBJ(arr);
    pval = p->u.array.u.values;
    for (i = 0; i < len; i++) {
        pval[i] = items[i].val;          /* transfer ownership */
        js_free(ctx, items[i].skey);
    }
    js_free(ctx, items);
    items = NULL;
    ret = arr;
    goto done;
 fail:
    dyn_sortby_free(ctx, items, len);
    items = NULL;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _groupBy(map?) -> an object mapping each mapped key (identity / function /
 * property-name) to the array of elements that produced it. */
static JSValue js_array_ext_groupby(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSValueConst map = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    result = JS_NewObject(ctx);
    if (JS_IsException(result))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el, key, bucket;
        JSAtom atom;
        int64_t blen;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto fail;
        key = js_array_ext_mapval(ctx, map, el);
        if (JS_IsException(key)) { JS_FreeValue(ctx, el); goto fail; }
        atom = JS_ValueToAtom(ctx, key);
        JS_FreeValue(ctx, key);
        if (atom == JS_ATOM_NULL) { JS_FreeValue(ctx, el); goto fail; }
        bucket = JS_GetProperty(ctx, result, atom);
        if (JS_IsException(bucket)) { JS_FreeAtom(ctx, atom); JS_FreeValue(ctx, el); goto fail; }
        if (!JS_IsArray(ctx, bucket)) {
            JS_FreeValue(ctx, bucket);
            bucket = JS_NewArray(ctx);
            if (JS_IsException(bucket) ||
                JS_SetProperty(ctx, result, atom, JS_DupValue(ctx, bucket)) < 0) {
                JS_FreeValue(ctx, bucket); JS_FreeAtom(ctx, atom); JS_FreeValue(ctx, el); goto fail;
            }
        }
        JS_FreeAtom(ctx, atom);
        if (js_get_length64(ctx, &blen, bucket)) {
            JS_FreeValue(ctx, el); JS_FreeValue(ctx, bucket); goto fail;
        }
        if (JS_SetPropertyInt64(ctx, bucket, blen, el) < 0) { /* el consumed */
            JS_FreeValue(ctx, bucket); goto fail;
        }
        JS_FreeValue(ctx, bucket);
    }
    ret = result;
    goto done;
 fail:
    JS_FreeValue(ctx, result);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _shuffle() -> a new array with the elements in uniformly-random order
 * (Fisher-Yates over a fast-array copy, using the engine's Math.random PRNG). */
static JSValue js_array_ext_shuffle(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, arr;
    JSObject *p;
    JSValue *vals;
    int64_t len, i;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    arr = js_array_ext_build_range(ctx, obj, 0, len);
    JS_FreeValue(ctx, obj);
    if (JS_IsException(arr) || len < 2)
        return arr;
    p = JS_VALUE_GET_OBJ(arr);
    vals = p->u.array.u.values;
    for (i = len - 1; i > 0; i--) {
        int64_t j = (int64_t)(xorshift64star(&ctx->random_state) % (uint64_t)(i + 1));
        JSValue t = vals[i]; vals[i] = vals[j]; vals[j] = t;
    }
    return arr;
}

/* _sample(n?) -> a uniformly-random element (undefined if empty), or a new array
 * of n distinct random elements (n>len -> all, shuffled). */
static JSValue js_array_ext_sample(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, full, ret = JS_EXCEPTION;
    JSObject *p;
    JSValue *vals;
    int64_t len, i, n;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (argc == 0 || JS_IsUndefined(argv[0])) {          /* single element */
        if (len == 0) ret = JS_UNDEFINED;
        else {
            int64_t j = (int64_t)(xorshift64star(&ctx->random_state) % (uint64_t)len);
            if (js_array_ext_getel(ctx, obj, j, &ret)) ret = JS_EXCEPTION;
        }
        goto done;
    }
    if (JS_ToInt64Sat(ctx, &n, argv[0])) goto done;
    if (n < 0) n = 0;
    if (n > len) n = len;
    full = js_array_ext_build_range(ctx, obj, 0, len);   /* fast-array copy */
    if (JS_IsException(full)) goto done;
    p = JS_VALUE_GET_OBJ(full);
    vals = p->u.array.u.values;
    for (i = len - 1; i > 0; i--) {                      /* full Fisher-Yates */
        int64_t j = (int64_t)(xorshift64star(&ctx->random_state) % (uint64_t)(i + 1));
        JSValue t = vals[i]; vals[i] = vals[j]; vals[j] = t;
    }
    ret = js_array_ext_build_range(ctx, full, 0, n);     /* first n */
    JS_FreeValue(ctx, full);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* ============================================================================
 * DynValSet: a small open-addressing hash set of JSValues with SameValueZero
 * membership, reusing the engine's Map value-hasher. Owns its keys (dup on add,
 * free on destroy). Sized up front from a count hint (load factor <= 0.5, so no
 * resize and probing always terminates). Empty slots use JS_UNINITIALIZED, which
 * no array element or JS return value can ever be. The shared primitive behind
 * _unique/_uniqBy and the set operations. See SUGAR_RAMDA_NATIVE.md.
 * ========================================================================== */
typedef struct {
    JSValue *keys;     /* owned; JS_UNINITIALIZED = empty slot */
    uint32_t mask;     /* slots - 1 (slots is a power of two) */
    uint32_t count;
    int hash_bits;
} DynValSet;

static int dyn_valset_init(JSContext *ctx, DynValSet *s, int64_t hint)
{
    int bits = 3;
    uint32_t slots, i;
    /* size from the hint but CAP the initial table (a high-duplicate source has
     * far fewer distinct keys than elements — oversizing to 2*len wastes an
     * init+free pass over millions of empty slots); dyn_valset_resize grows it
     * on demand for genuinely large distinct sets. */
    while (((int64_t)1 << bits) < hint * 2 && bits < 16)
        bits++;
    slots = (uint32_t)1 << bits;
    s->keys = js_malloc(ctx, (size_t)slots * sizeof(JSValue));
    if (!s->keys)
        return -1;
    for (i = 0; i < slots; i++)
        s->keys[i] = JS_UNINITIALIZED;
    s->mask = slots - 1;
    s->hash_bits = bits;
    s->count = 0;
    return 0;
}

static void dyn_valset_free(JSContext *ctx, DynValSet *s)
{
    uint32_t i;
    if (!s->keys)
        return;
    for (i = 0; i <= s->mask; i++)
        JS_FreeValue(ctx, s->keys[i]); /* no-op for the UNINITIALIZED slots */
    js_free(ctx, s->keys);
    s->keys = NULL;
}

/* double the table and rehash (moves the owned keys, no dup/free). 0 or -1. */
static int dyn_valset_resize(JSContext *ctx, DynValSet *s)
{
    int new_bits = s->hash_bits + 1;
    uint32_t new_slots, new_mask, i;
    JSValue *nk;
    if (new_bits > 30)
        return 0; /* absurdly large: stay put (load still well below 1) */
    new_slots = (uint32_t)1 << new_bits;
    new_mask = new_slots - 1;
    nk = js_malloc(ctx, (size_t)new_slots * sizeof(JSValue));
    if (!nk)
        return -1;
    for (i = 0; i < new_slots; i++)
        nk[i] = JS_UNINITIALIZED;
    for (i = 0; i <= s->mask; i++) {
        JSValue k = s->keys[i];
        uint32_t h;
        if (JS_VALUE_GET_TAG(k) == JS_TAG_UNINITIALIZED)
            continue;
        h = map_hash_key(k, new_bits) & new_mask;
        while (JS_VALUE_GET_TAG(nk[h]) != JS_TAG_UNINITIALIZED)
            h = (h + 1) & new_mask;
        nk[h] = k; /* ownership moves with the value */
    }
    js_free(ctx, s->keys);
    s->keys = nk;
    s->mask = new_mask;
    s->hash_bits = new_bits;
    return 0;
}

/* add `key` (borrowed) -> 1 if newly inserted, 0 if already present, -1 on OOM.
 * Grows the table only on an actual insert at load >= 0.5 (a duplicate returns
 * early and never resizes). No user JS runs (hashing + SameValueZero only), so
 * the caller's source stays valid across the call. */
static int dyn_valset_add(JSContext *ctx, DynValSet *s, JSValueConst key)
{
    JSValueConst nk = map_normalize_key_const(ctx, key);
    uint32_t h = map_hash_key(nk, s->hash_bits) & s->mask;
    for (;;) {
        JSValue slot = s->keys[h];
        if (JS_VALUE_GET_TAG(slot) == JS_TAG_UNINITIALIZED)
            break; /* not present: this is the insertion point */
        if (JS_SameValueZero(ctx, slot, nk))
            return 0; /* already present */
        h = (h + 1) & s->mask;
    }
    if (s->count >= ((s->mask + 1) >> 1)) { /* load would reach 0.5 -> grow first */
        if (dyn_valset_resize(ctx, s))
            return -1;
        h = map_hash_key(nk, s->hash_bits) & s->mask;
        while (JS_VALUE_GET_TAG(s->keys[h]) != JS_TAG_UNINITIALIZED)
            h = (h + 1) & s->mask;
    }
    s->keys[h] = JS_DupValue(ctx, nk);
    s->count++;
    return 1;
}

static int dyn_valset_has(JSContext *ctx, DynValSet *s, JSValueConst key)
{
    JSValueConst nk = map_normalize_key_const(ctx, key);
    uint32_t h = map_hash_key(nk, s->hash_bits) & s->mask;
    for (;;) {
        JSValue slot = s->keys[h];
        if (JS_VALUE_GET_TAG(slot) == JS_TAG_UNINITIALIZED)
            return 0;
        if (JS_SameValueZero(ctx, slot, nk))
            return 1;
        h = (h + 1) & s->mask;
    }
}

/* _unique(map?) / _uniq / _uniqBy(fn) -> a new array with duplicates removed
 * (SameValueZero on the mapped value; identity / function / property-name),
 * keeping the first occurrence's element in original order. */
static JSValue js_array_ext_unique(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSValueConst map = argc > 0 ? argv[0] : JS_UNDEFINED;
    DynValSet seen;
    int64_t len, i, j = 0;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done0;
    result = JS_NewArray(ctx);
    if (JS_IsException(result))
        goto done0;
    if (dyn_valset_init(ctx, &seen, len)) { JS_ThrowOutOfMemory(ctx); JS_FreeValue(ctx, result); goto done0; }
    for (i = 0; i < len; i++) {
        JSValue el, key;
        int added;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto fail;
        key = js_array_ext_mapval(ctx, map, el);
        if (JS_IsException(key)) { JS_FreeValue(ctx, el); goto fail; }
        added = dyn_valset_add(ctx, &seen, key);
        JS_FreeValue(ctx, key);
        if (added < 0) { JS_FreeValue(ctx, el); goto fail; }
        if (added) {
            if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) goto fail;
        } else {
            JS_FreeValue(ctx, el);
        }
    }
    ret = result;
    result = JS_UNDEFINED;
 fail:
    dyn_valset_free(ctx, &seen);
    JS_FreeValue(ctx, result);
 done0:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _intersect(other)/_intersection, _difference(other), _without(other)
 * (magic 0/1/2). Builds a SameValueZero set from `other`, then filters `this`.
 * intersect/difference dedup the result; without keeps this's duplicates. */
static JSValue js_array_ext_setop(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    DynValSet set, seen;
    int have_seen = 0;
    int64_t len, olen, i, j = 0;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    if (dyn_valset_init(ctx, &set, olen)) { JS_ThrowOutOfMemory(ctx); goto done; }
    for (i = 0; i < olen; i++) {
        JSValue oe;
        int r;
        if (js_array_ext_getel(ctx, other, i, &oe)) goto fail_set;
        r = dyn_valset_add(ctx, &set, oe);
        JS_FreeValue(ctx, oe);
        if (r < 0) goto fail_set;
    }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto fail_set;
    if (magic != 2) { /* intersect/difference dedup the result */
        if (dyn_valset_init(ctx, &seen, len)) { JS_ThrowOutOfMemory(ctx); JS_FreeValue(ctx, result); goto fail_set; }
        have_seen = 1;
    }
    for (i = 0; i < len; i++) {
        JSValue el;
        int in_other, keep;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto fail_result;
        in_other = dyn_valset_has(ctx, &set, el);
        keep = (magic == 0) ? in_other : !in_other; /* intersect vs difference/without */
        if (!keep) { JS_FreeValue(ctx, el); continue; }
        if (have_seen) {
            int added = dyn_valset_add(ctx, &seen, el);
            if (added < 0) { JS_FreeValue(ctx, el); goto fail_result; } /* OOM */
            if (added == 0) { JS_FreeValue(ctx, el); continue; }        /* duplicate */
        }
        if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) goto fail_result;
    }
    ret = result;
    result = JS_UNDEFINED;
 fail_result:
    if (have_seen) dyn_valset_free(ctx, &seen);
    JS_FreeValue(ctx, result);
 fail_set:
    dyn_valset_free(ctx, &set);
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _union(other) -> the elements of this then other, SameValueZero-deduped. */
static JSValue js_array_ext_union(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    DynValSet seen;
    int64_t len, olen, i, j = 0, pass;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    if (dyn_valset_init(ctx, &seen, len + olen)) { JS_ThrowOutOfMemory(ctx); JS_FreeValue(ctx, result); goto done; }
    for (pass = 0; pass < 2; pass++) {
        JSValueConst src = pass == 0 ? obj : other;
        int64_t n = pass == 0 ? len : olen;
        for (i = 0; i < n; i++) {
            JSValue el;
            int added;
            if (js_array_ext_getel(ctx, src, i, &el)) goto fail;
            added = dyn_valset_add(ctx, &seen, el);
            if (added < 0) { JS_FreeValue(ctx, el); goto fail; }
            if (added) {
                if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) goto fail;
            } else {
                JS_FreeValue(ctx, el);
            }
        }
    }
    ret = result;
    result = JS_UNDEFINED;
 fail:
    dyn_valset_free(ctx, &seen);
    JS_FreeValue(ctx, result);
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _zip(other) -> [[this[i], other[i]], ...] truncated to the shorter length. */
static JSValue js_array_ext_zip(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    int64_t len, olen, n, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    n = len < olen ? len : olen;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < n; i++) {
        JSValue a, b, pair;
        if (js_array_ext_getel(ctx, obj, i, &a)) { JS_FreeValue(ctx, result); goto done; }
        if (js_array_ext_getel(ctx, other, i, &b)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, result); goto done; }
        pair = JS_NewArray(ctx);
        if (JS_IsException(pair)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); JS_FreeValue(ctx, result); goto done; }
        JS_DefinePropertyValueInt64(ctx, pair, 0, a, JS_PROP_C_W_E);
        JS_DefinePropertyValueInt64(ctx, pair, 1, b, JS_PROP_C_W_E);
        if (JS_DefinePropertyValueInt64(ctx, result, i, pair, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _zipWith(fn, other) -> [fn(this[i], other[i]), ...] truncated to shorter. */
static JSValue js_array_ext_zipwith(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, olen, n, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    n = len < olen ? len : olen;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < n; i++) {
        JSValue a, b, r;
        JSValueConst args[2];
        if (js_array_ext_getel(ctx, obj, i, &a)) { JS_FreeValue(ctx, result); goto done; }
        if (js_array_ext_getel(ctx, other, i, &b)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, result); goto done; }
        args[0] = a; args[1] = b;
        r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
        if (JS_IsException(r)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, i, r, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _intersperse(sep) -> a new array with sep between each pair of elements. */
static JSValue js_array_ext_intersperse(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSValueConst sep = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        if (i > 0 && JS_DefinePropertyValueInt64(ctx, result, j++, JS_DupValue(ctx, sep), JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
        if (js_array_ext_getel(ctx, obj, i, &el)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* recursive flatten; c_depth guards the C stack against pathological nesting
 * (past FLATTEN_MAX_DEPTH a nested array is emitted as-is). */
#define FLATTEN_MAX_DEPTH 512
static int js_array_ext_flatten_into(JSContext *ctx, JSValueConst result,
                                     JSValueConst arr, int64_t remaining,
                                     int64_t *j, int c_depth)
{
    int64_t len, i;
    if (js_get_length64(ctx, &len, arr)) return -1;
    for (i = 0; i < len; i++) {
        JSValue el;
        if (js_array_ext_getel(ctx, arr, i, &el)) return -1;
        if (remaining > 0 && c_depth < FLATTEN_MAX_DEPTH && JS_IsArray(ctx, el)) {
            int r = js_array_ext_flatten_into(ctx, result, el, remaining - 1, j, c_depth + 1);
            JS_FreeValue(ctx, el);
            if (r) return -1;
        } else if (JS_DefinePropertyValueInt64(ctx, result, (*j)++, el, JS_PROP_C_W_E) < 0) {
            return -1;
        }
    }
    return 0;
}

/* _flatten(depth?) -> a new array flattened to `depth` (default: fully). */
static JSValue js_array_ext_flatten(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    int64_t depth = INT64_MAX, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64Sat(ctx, &depth, argv[0])) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
        if (depth < 0) depth = 0;
    }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_array_ext_flatten_into(ctx, result, obj, depth, &j, 0)) { JS_FreeValue(ctx, result); goto done; }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _transpose() -> transpose an array of arrays (ragged: skips missing cells). */
static JSValue js_array_ext_transpose(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    int64_t nrows, maxcol = 0, r, c;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &nrows, obj)) goto done;
    for (r = 0; r < nrows; r++) {
        JSValue row;
        int64_t rl;
        if (js_array_ext_getel(ctx, obj, r, &row)) goto done;
        if (JS_IsArray(ctx, row)) {
            if (js_get_length64(ctx, &rl, row)) { JS_FreeValue(ctx, row); goto done; }
            if (rl > maxcol) maxcol = rl;
        }
        JS_FreeValue(ctx, row);
    }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (c = 0; c < maxcol; c++) {
        JSValue col = JS_NewArray(ctx);
        int64_t k = 0;
        if (JS_IsException(col)) { JS_FreeValue(ctx, result); goto done; }
        for (r = 0; r < nrows; r++) {
            JSValue row, cell;
            int64_t rl;
            if (js_array_ext_getel(ctx, obj, r, &row)) { JS_FreeValue(ctx, col); JS_FreeValue(ctx, result); goto done; }
            if (!JS_IsArray(ctx, row)) { JS_FreeValue(ctx, row); continue; }
            if (js_get_length64(ctx, &rl, row)) { JS_FreeValue(ctx, row); JS_FreeValue(ctx, col); JS_FreeValue(ctx, result); goto done; }
            if (c < rl) {
                if (js_array_ext_getel(ctx, row, c, &cell)) { JS_FreeValue(ctx, row); JS_FreeValue(ctx, col); JS_FreeValue(ctx, result); goto done; }
                if (JS_DefinePropertyValueInt64(ctx, col, k++, cell, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, row); JS_FreeValue(ctx, col); JS_FreeValue(ctx, result); goto done; }
            }
            JS_FreeValue(ctx, row);
        }
        if (JS_DefinePropertyValueInt64(ctx, result, c, col, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _partition(matcher) -> [ [elements the matcher accepts], [the rest] ]
 * (matcher = value via SameValueZero, or a predicate function). */
static JSValue js_array_ext_partition(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, yes = JS_UNDEFINED, no = JS_UNDEFINED, result, ret = JS_EXCEPTION;
    JSValueConst matcher = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i, jy = 0, jn = 0;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (js_ext_matcher_begin(ctx, &pm, matcher))
        goto done;
    yes = JS_NewArray(ctx);
    no = JS_NewArray(ctx);
    if (JS_IsException(yes) || JS_IsException(no))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        if (m < 0) { JS_FreeValue(ctx, el); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, m ? yes : no, m ? jy++ : jn++, el, JS_PROP_C_W_E) < 0)
            goto done;
    }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    JS_DefinePropertyValueInt64(ctx, result, 0, yes, JS_PROP_C_W_E); /* consumes yes */
    JS_DefinePropertyValueInt64(ctx, result, 1, no, JS_PROP_C_W_E);  /* consumes no */
    yes = no = JS_UNDEFINED;
    ret = result;
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, yes);
    JS_FreeValue(ctx, no);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _pluck(key) -> a new array of element[key] for each element (Ramda pluck). */
static JSValue js_array_ext_pluck(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSAtom key;
    int64_t len, i;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    key = JS_ValueToAtom(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (key == JS_ATOM_NULL) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el, v;
        if (js_array_ext_getel(ctx, obj, i, &el)) { JS_FreeValue(ctx, result); goto done; }
        v = JS_GetProperty(ctx, el, key);
        JS_FreeValue(ctx, el);
        if (JS_IsException(v)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, i, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, result); goto done;
        }
    }
    ret = result;
 done:
    JS_FreeAtom(ctx, key);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _xprod(other) -> cross product [[a,b], ...] for each a in this, b in other. */
static JSValue js_array_ext_xprod(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    int64_t len, olen, i, j, k = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue a;
        if (js_array_ext_getel(ctx, obj, i, &a)) { JS_FreeValue(ctx, result); goto done; }
        for (j = 0; j < olen; j++) {
            JSValue b, pair;
            if (js_array_ext_getel(ctx, other, j, &b)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, result); goto done; }
            pair = JS_NewArray(ctx);
            if (JS_IsException(pair)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); JS_FreeValue(ctx, result); goto done; }
            JS_DefinePropertyValueInt64(ctx, pair, 0, JS_DupValue(ctx, a), JS_PROP_C_W_E);
            JS_DefinePropertyValueInt64(ctx, pair, 1, b, JS_PROP_C_W_E);
            if (JS_DefinePropertyValueInt64(ctx, result, k++, pair, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, result); goto done; }
        }
        JS_FreeValue(ctx, a);
    }
    ret = result;
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _aperture(n) -> sliding windows of n consecutive elements: len-n+1 of them
 * (Ramda: n<=0 yields len-n+1 empty windows). */
static JSValue js_array_ext_aperture(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    int64_t len, n, limit, i;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &n, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    limit = len - n + 1;
    if (limit < 0) limit = 0;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < limit; i++) {
        int64_t start = i, end = i + n;
        JSValue win;
        if (end < start) end = start;   /* n<=0: empty window */
        win = js_array_ext_build_range(ctx, obj, start, end);
        if (JS_IsException(win)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, i, win, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _splitEvery(n) -> chunks of n consecutive elements (last may be short).
 * Throws RangeError for n<=0 (Ramda). */
static JSValue js_array_ext_splitevery(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    int64_t len, n, i, k = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &n, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (n <= 0) { JS_FreeValue(ctx, obj); return JS_ThrowRangeError(ctx, "splitEvery: n must be a positive integer"); }
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i += n) {
        int64_t end = i + n;
        JSValue chunk;
        if (end > len) end = len;
        chunk = js_array_ext_build_range(ctx, obj, i, end);
        if (JS_IsException(chunk)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, k++, chunk, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _splitAt(index) -> [ take(index), drop(index) ]; negative index from the end. */
static JSValue js_array_ext_splitat(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, left = JS_UNDEFINED, right = JS_UNDEFINED, result, ret = JS_EXCEPTION;
    int64_t len, idx;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (idx < 0) idx = len + idx;
    if (idx < 0) idx = 0;
    if (idx > len) idx = len;
    left = js_array_ext_build_range(ctx, obj, 0, idx);
    if (JS_IsException(left)) goto done;
    right = js_array_ext_build_range(ctx, obj, idx, len);
    if (JS_IsException(right)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    JS_DefinePropertyValueInt64(ctx, result, 0, left, JS_PROP_C_W_E);   /* consumes left */
    JS_DefinePropertyValueInt64(ctx, result, 1, right, JS_PROP_C_W_E);  /* consumes right */
    left = right = JS_UNDEFINED;
    ret = result;
 done:
    JS_FreeValue(ctx, left);
    JS_FreeValue(ctx, right);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _adjust(idx, fn) -> a copy with fn applied at idx (negative from the end);
 * an out-of-range idx yields an unchanged copy (Ramda). */
static JSValue js_array_ext_adjust(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 1 ? argv[1] : JS_UNDEFINED;
    int64_t len, idx;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_array_ext_build_range(ctx, obj, 0, len);
    if (JS_IsException(result)) goto done;
    if (idx < 0) idx += len;
    if (idx >= 0 && idx < len) {
        JSValue old, nv;
        JSValueConst arg;
        if (js_array_ext_getel(ctx, result, idx, &old)) goto done;
        arg = old;
        nv = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, old);
        if (JS_IsException(nv)) goto done;
        if (JS_SetPropertyInt64(ctx, result, idx, nv) < 0) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _update(idx, val) -> a copy with val at idx (negative from the end);
 * an out-of-range idx yields an unchanged copy (Ramda). */
static JSValue js_array_ext_update(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst val = argc > 1 ? argv[1] : JS_UNDEFINED;
    int64_t len, idx;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_array_ext_build_range(ctx, obj, 0, len);
    if (JS_IsException(result)) goto done;
    if (idx < 0) idx += len;
    if (idx >= 0 && idx < len) {
        if (JS_SetPropertyInt64(ctx, result, idx, JS_DupValue(ctx, val)) < 0) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _move(from, to) -> a copy with the item at `from` relocated to `to`
 * (negative indices from the end); out-of-range returns an unchanged copy.
 * Built as three contiguous bulk blits into a pre-sized fast array (no
 * per-element property dispatch) — the removal+insertion is expressed as
 * disjoint source ranges, so each element is dup'd exactly once. */
static JSValue js_array_ext_move(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, src = JS_UNDEFINED, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, from, to, w = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &from, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (JS_ToInt64Sat(ctx, &to, argc > 1 ? argv[1] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    src = js_array_ext_build_range(ctx, obj, 0, len);   /* stable fast copy */
    if (JS_IsException(src)) goto done;
    if (from < 0) from += len;
    if (to < 0) to += len;
    if (from < 0 || from >= len || to < 0 || to >= len || from == to) { ret = src; src = JS_UNDEFINED; goto done; }
    if (!js_get_fast_array(ctx, src, &srcp, &scount) || (int64_t)scount != len) {
        ret = src; src = JS_UNDEFINED; goto done;       /* defensive: shouldn't happen */
    }
    result = js_allocate_fast_array(ctx, len);           /* slots pre-filled UNDEFINED */
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    /* item = src[from], then blit the disjoint kept ranges around target `to`. */
    if (from < to) {
        int64_t i;
        for (i = 0; i < from; i++)       dst[w++] = JS_DupValue(ctx, srcp[i]);
        for (i = from + 1; i <= to; i++) dst[w++] = JS_DupValue(ctx, srcp[i]);
        dst[w++] = JS_DupValue(ctx, srcp[from]);
        for (i = to + 1; i < len; i++)   dst[w++] = JS_DupValue(ctx, srcp[i]);
    } else {                              /* from > to */
        int64_t i;
        for (i = 0; i < to; i++)         dst[w++] = JS_DupValue(ctx, srcp[i]);
        dst[w++] = JS_DupValue(ctx, srcp[from]);
        for (i = to; i < from; i++)      dst[w++] = JS_DupValue(ctx, srcp[i]);
        for (i = from + 1; i < len; i++) dst[w++] = JS_DupValue(ctx, srcp[i]);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, src);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _swap(i, j) -> a copy with the elements at i and j exchanged (negative
 * indices from the end); out-of-range returns an unchanged copy. */
static JSValue js_array_ext_swap(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    int64_t len, i, j;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &i, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (JS_ToInt64Sat(ctx, &j, argc > 1 ? argv[1] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_array_ext_build_range(ctx, obj, 0, len);
    if (JS_IsException(result)) goto done;
    if (i < 0) i += len;
    if (j < 0) j += len;
    if (i >= 0 && i < len && j >= 0 && j < len && i != j) {
        JSValue a, b;
        if (js_array_ext_getel(ctx, result, i, &a)) goto done;
        if (js_array_ext_getel(ctx, result, j, &b)) { JS_FreeValue(ctx, a); goto done; }
        if (JS_SetPropertyInt64(ctx, result, i, b) < 0) { JS_FreeValue(ctx, a); goto done; }
        if (JS_SetPropertyInt64(ctx, result, j, a) < 0) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _nth(i) -> element at index i (negative from the end); undefined if out of range. */
static JSValue js_array_ext_nth(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &i, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (i < 0) i += len;
    if (i < 0 || i >= len) { ret = JS_UNDEFINED; goto done; }
    if (js_array_ext_getel(ctx, obj, i, &ret)) ret = JS_EXCEPTION;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _init() -> all but the last element (Ramda init). */
static JSValue js_array_ext_init(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    ret = js_array_ext_build_range(ctx, obj, 0, len > 0 ? len - 1 : 0);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _tail() -> all but the first element (Ramda tail). */
static JSValue js_array_ext_tail(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    ret = js_array_ext_build_range(ctx, obj, len > 0 ? 1 : 0, len);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _takeWhile/_dropWhile/_takeLastWhile/_dropLastWhile(matcher): matcher is a
 * predicate function or a value (SameValueZero). magic: 0 takeWhile, 1 dropWhile,
 * 2 takeLastWhile, 3 dropLastWhile. */
static JSValue js_array_ext_whilst(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    JSValue obj, ret = JS_EXCEPTION;
    JSValueConst matcher = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (js_ext_matcher_begin(ctx, &pm, matcher)) goto done;
    if (magic < 2) {                        /* scan from the front */
        for (i = 0; i < len; i++) {
            JSValue el;
            int m;
            if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
            m = js_ext_matcher_test(ctx, &pm, el);
            JS_FreeValue(ctx, el);
            if (m < 0) goto done;
            if (!m) break;
        }
        ret = (magic == 0) ? js_array_ext_build_range(ctx, obj, 0, i)
                           : js_array_ext_build_range(ctx, obj, i, len);
    } else {                                /* scan from the back */
        for (i = len; i > 0; i--) {
            JSValue el;
            int m;
            if (js_array_ext_getel(ctx, obj, i - 1, &el)) goto done;
            m = js_ext_matcher_test(ctx, &pm, el);
            JS_FreeValue(ctx, el);
            if (m < 0) goto done;
            if (!m) break;
        }
        ret = (magic == 2) ? js_array_ext_build_range(ctx, obj, i, len)
                           : js_array_ext_build_range(ctx, obj, 0, i);
    }
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _append(x) -> a copy with x added at the end (Ramda append). */
static JSValue js_array_ext_append(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSValueConst x = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_array_ext_build_range(ctx, obj, 0, len);
    if (JS_IsException(result)) goto done;
    if (JS_DefinePropertyValueInt64(ctx, result, len, JS_DupValue(ctx, x), JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _prepend(x) -> a copy with x added at the front (Ramda prepend), built as one
 * pre-sized fast array + a bulk blit of the tail. */
static JSValue js_array_ext_prepend(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst x = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_allocate_fast_array(ctx, len + 1);       /* slots pre-filled UNDEFINED */
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    dst[0] = JS_DupValue(ctx, x);
    if (js_get_fast_array(ctx, obj, &srcp, &scount) && (int64_t)scount >= len) {
        for (i = 0; i < len; i++)
            dst[1 + i] = JS_DupValue(ctx, srcp[i]);
    } else {
        for (i = 0; i < len; i++)
            if (js_array_ext_getel(ctx, obj, i, &dst[1 + i])) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _reject(matcher) -> the elements the matcher REJECTS (complement of filter);
 * matcher is a predicate function or a value (SameValueZero). */
static JSValue js_array_ext_reject(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst matcher = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (js_ext_matcher_begin(ctx, &pm, matcher)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) { result = JS_UNDEFINED; goto done; }
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        if (m < 0) { JS_FreeValue(ctx, el); goto done; }
        if (!m) {
            if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) goto done;
        } else {
            JS_FreeValue(ctx, el);
        }
    }
    ret = result; result = JS_UNDEFINED;
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _insert(idx, elt) -> a copy with elt inserted at idx; an idx outside [0,len)
 * appends (Ramda insert). */
static JSValue js_array_ext_insert(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst elt = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, idx, i;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (idx < 0 || idx > len) idx = len;
    result = js_allocate_fast_array(ctx, len + 1);
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    if (js_get_fast_array(ctx, obj, &srcp, &scount) && (int64_t)scount >= len) {
        for (i = 0; i < idx; i++)   dst[i] = JS_DupValue(ctx, srcp[i]);
        dst[idx] = JS_DupValue(ctx, elt);
        for (i = idx; i < len; i++) dst[i + 1] = JS_DupValue(ctx, srcp[i]);
    } else {
        for (i = 0; i < idx; i++)   if (js_array_ext_getel(ctx, obj, i, &dst[i])) goto done;
        dst[idx] = JS_DupValue(ctx, elt);
        for (i = idx; i < len; i++) if (js_array_ext_getel(ctx, obj, i, &dst[i + 1])) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _insertAll(idx, elts) -> a copy with every element of elts inserted at idx;
 * an idx outside [0,len) appends (Ramda insertAll). */
static JSValue js_array_ext_insertall(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, elts, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSObject *rp;
    JSValue *dst;
    int64_t len, elen, idx, i, w = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    elts = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(elts)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &elen, elts)) goto done;
    if (idx < 0 || idx > len) idx = len;
    result = js_allocate_fast_array(ctx, len + elen);
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    for (i = 0; i < idx; i++)   if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done;
    for (i = 0; i < elen; i++)  if (js_array_ext_getel(ctx, elts, i, &dst[w++])) goto done;
    for (i = idx; i < len; i++) if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done;
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, elts);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _removeAt(idx) -> a copy without the element at idx (negative from the end);
 * out-of-range returns an unchanged copy. */
static JSValue js_array_ext_removeat(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, idx, i, w = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) {          /* out of range -> unchanged copy */
        ret = js_array_ext_build_range(ctx, obj, 0, len);
        goto done;
    }
    result = js_allocate_fast_array(ctx, len - 1);
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    if (js_get_fast_array(ctx, obj, &srcp, &scount) && (int64_t)scount >= len) {
        for (i = 0; i < len; i++) if (i != idx) dst[w++] = JS_DupValue(ctx, srcp[i]);
    } else {
        for (i = 0; i < len; i++) if (i != idx) { if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done; }
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _zipObj(values) -> an object mapping this[i] (as key) to values[i], truncated
 * to the shorter length (Ramda zipObj). */
static JSValue js_array_ext_zipobj(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, vals, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    int64_t klen, vlen, n, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &klen, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    vals = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(vals)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &vlen, vals)) goto done;
    n = klen < vlen ? klen : vlen;
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < n; i++) {
        JSValue k, v;
        JSAtom a;
        if (js_array_ext_getel(ctx, obj, i, &k)) goto done;
        a = JS_ValueToAtom(ctx, k);
        JS_FreeValue(ctx, k);
        if (a == JS_ATOM_NULL) goto done;
        if (js_array_ext_getel(ctx, vals, i, &v)) { JS_FreeAtom(ctx, a); goto done; }
        if (JS_DefinePropertyValue(ctx, result, a, v, JS_PROP_C_W_E) < 0) { JS_FreeAtom(ctx, a); goto done; }
        JS_FreeAtom(ctx, a);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, vals);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _fromPairs() -> an object built from [key, value] pairs (Ramda fromPairs);
 * later pairs win on duplicate keys. */
static JSValue js_array_ext_frompairs(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    int64_t len, i;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue pair, k, v;
        JSAtom a;
        if (js_array_ext_getel(ctx, obj, i, &pair)) goto done;
        if (js_array_ext_getel(ctx, pair, 0, &k)) { JS_FreeValue(ctx, pair); goto done; }
        a = JS_ValueToAtom(ctx, k);
        JS_FreeValue(ctx, k);
        if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, pair); goto done; }
        if (js_array_ext_getel(ctx, pair, 1, &v)) { JS_FreeAtom(ctx, a); JS_FreeValue(ctx, pair); goto done; }
        JS_FreeValue(ctx, pair);
        if (JS_DefinePropertyValue(ctx, result, a, v, JS_PROP_C_W_E) < 0) { JS_FreeAtom(ctx, a); goto done; }
        JS_FreeAtom(ctx, a);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

static int js_array_ext_cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* _median() -> the median of the elements coerced to numbers; NaN if empty.
 * Coerces every element into a C buffer FIRST (valueOf may run JS), then sorts
 * and reduces purely in C. */
static JSValue js_array_ext_median(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    double *buf = NULL, med;
    int64_t len, i;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (len == 0) { ret = JS_NewFloat64(ctx, NAN); goto done; }
    buf = js_malloc(ctx, sizeof(double) * len);
    if (!buf) goto done;
    for (i = 0; i < len; i++) {
        JSValue v;
        double d;
        int r;
        if (js_array_ext_getel(ctx, obj, i, &v)) goto done;
        r = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (r) goto done;
        buf[i] = d;
    }
    qsort(buf, len, sizeof(double), js_array_ext_cmp_double);
    med = (len & 1) ? buf[len / 2] : (buf[len / 2 - 1] + buf[len / 2]) / 2.0;
    ret = JS_NewFloat64(ctx, med);
 done:
    js_free(ctx, buf);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _product() -> the product of the elements coerced to numbers; 1 if empty. */
static JSValue js_array_ext_product(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, i;
    double acc = 1;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    for (i = 0; i < len; i++) {
        JSValue v;
        double d;
        int r;
        if (js_array_ext_getel(ctx, obj, i, &v)) goto done;
        r = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (r) goto done;
        acc *= d;
    }
    ret = JS_NewFloat64(ctx, acc);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _scan(fn, acc) -> [acc, fn(acc,x0), fn(...,x1), ...] — reduce keeping every
 * intermediate (Ramda scan); result length is len+1. */
static JSValue js_array_ext_scan(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, result, acc, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    acc = JS_DupValue(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) { JS_FreeValue(ctx, acc); goto done; }
    if (JS_DefinePropertyValueInt64(ctx, result, 0, JS_DupValue(ctx, acc), JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, acc); JS_FreeValue(ctx, result); goto done; }
    for (i = 0; i < len; i++) {
        JSValue el, nv;
        JSValueConst args[2];
        if (js_array_ext_getel(ctx, obj, i, &el)) { JS_FreeValue(ctx, acc); JS_FreeValue(ctx, result); goto done; }
        args[0] = acc; args[1] = el;
        nv = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, el);
        JS_FreeValue(ctx, acc);
        if (JS_IsException(nv)) { JS_FreeValue(ctx, result); goto done; }
        acc = nv;
        if (JS_DefinePropertyValueInt64(ctx, result, i + 1, JS_DupValue(ctx, acc), JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, acc); JS_FreeValue(ctx, result); goto done; }
    }
    JS_FreeValue(ctx, acc);
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _countBy(fn) -> object mapping each key (fn(el), or a property/identity) to
 * the count of elements with that key (Ramda countBy). */
static JSValue js_array_ext_countby(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el, key, cur;
        JSAtom a;
        int32_t c = 0;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        key = js_array_ext_mapval(ctx, fn, el);
        JS_FreeValue(ctx, el);
        if (JS_IsException(key)) goto done;
        a = JS_ValueToAtom(ctx, key);
        JS_FreeValue(ctx, key);
        if (a == JS_ATOM_NULL) goto done;
        cur = JS_GetProperty(ctx, result, a);
        if (JS_IsException(cur)) { JS_FreeAtom(ctx, a); goto done; }
        if (!JS_IsUndefined(cur) && JS_ToInt32(ctx, &c, cur)) { JS_FreeValue(ctx, cur); JS_FreeAtom(ctx, a); goto done; }
        JS_FreeValue(ctx, cur);
        if (JS_DefinePropertyValue(ctx, result, a, JS_NewInt32(ctx, c + 1), JS_PROP_C_W_E) < 0) { JS_FreeAtom(ctx, a); goto done; }
        JS_FreeAtom(ctx, a);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _indexBy(fn) -> object mapping each key (fn(el), or a property/identity) to
 * the LAST element with that key (Ramda indexBy). */
static JSValue js_array_ext_indexby(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el, key;
        JSAtom a;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        key = js_array_ext_mapval(ctx, fn, el);
        if (JS_IsException(key)) { JS_FreeValue(ctx, el); goto done; }
        a = JS_ValueToAtom(ctx, key);
        JS_FreeValue(ctx, key);
        if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, el); goto done; }
        if (JS_DefinePropertyValue(ctx, result, a, el, JS_PROP_C_W_E) < 0) { JS_FreeAtom(ctx, a); goto done; } /* consumes el; last wins */
        JS_FreeAtom(ctx, a);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* removeRange(start, count) -> a copy with `count` elements removed starting at
 * `start` (negative from the end), i.e. non-mutating splice (Ramda remove). */
static JSValue js_array_ext_removerange(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, start, count, end, i, w = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &start, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (JS_ToInt64Sat(ctx, &count, argc > 1 ? argv[1] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (start < 0) start += len;
    if (start < 0) start = 0;
    if (start > len) start = len;
    if (count < 0) count = 0;
    end = start + count;
    if (end > len) end = len;
    result = js_allocate_fast_array(ctx, len - (end - start));
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    if (js_get_fast_array(ctx, obj, &srcp, &scount) && (int64_t)scount >= len) {
        for (i = 0; i < start; i++)   dst[w++] = JS_DupValue(ctx, srcp[i]);
        for (i = end; i < len; i++)   dst[w++] = JS_DupValue(ctx, srcp[i]);
    } else {
        for (i = 0; i < start; i++)   if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done;
        for (i = end; i < len; i++)   if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* splitWhen(matcher) -> [ before, fromFirstMatchOnward ] splitting at the first
 * element the matcher accepts (Ramda splitWhen; matcher = predicate/value/regex).
 * If none match, [ all, [] ]. */
static JSValue js_array_ext_splitwhen(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, left = JS_UNDEFINED, right = JS_UNDEFINED, result, ret = JS_EXCEPTION;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (js_ext_matcher_begin(ctx, &pm, argc > 0 ? argv[0] : JS_UNDEFINED)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        JS_FreeValue(ctx, el);
        if (m < 0) goto done;
        if (m) break;
    }
    left = js_array_ext_build_range(ctx, obj, 0, i);
    if (JS_IsException(left)) goto done;
    right = js_array_ext_build_range(ctx, obj, i, len);
    if (JS_IsException(right)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    JS_DefinePropertyValueInt64(ctx, result, 0, left, JS_PROP_C_W_E);
    JS_DefinePropertyValueInt64(ctx, result, 1, right, JS_PROP_C_W_E);
    left = right = JS_UNDEFINED;
    ret = result;
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, left);
    JS_FreeValue(ctx, right);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* innerJoin(pred, other) -> the elements of this for which pred(element, y) holds
 * for some y in other (Ramda innerJoin). */
static JSValue js_array_ext_innerjoin(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, other, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst pred = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, olen, i, k, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue x;
        int matched = 0;
        if (js_array_ext_getel(ctx, obj, i, &x)) goto done;
        for (k = 0; k < olen; k++) {
            JSValue y, r;
            JSValueConst args[2];
            int b;
            if (js_array_ext_getel(ctx, other, k, &y)) { JS_FreeValue(ctx, x); goto done; }
            args[0] = x; args[1] = y;
            r = JS_Call(ctx, pred, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, y);
            if (JS_IsException(r)) { JS_FreeValue(ctx, x); goto done; }
            b = JS_ToBool(ctx, r);
            JS_FreeValue(ctx, r);
            if (b) { matched = 1; break; }
        }
        if (matched) {
            if (JS_DefinePropertyValueInt64(ctx, result, j++, x, JS_PROP_C_W_E) < 0) goto done;
        } else {
            JS_FreeValue(ctx, x);
        }
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

static const JSCFunctionListEntry js_array_ext_funcs[] = {
    JS_CFUNC_DEF("isEmpty", 0, js_array_ext_isEmpty ),
    JS_CFUNC_DEF("first", 0, js_array_ext_first ),
    JS_CFUNC_DEF("last", 0, js_array_ext_last ),
    JS_CFUNC_MAGIC_DEF("sum", 0, js_array_ext_sum_avg, 0 ),
    JS_CFUNC_MAGIC_DEF("average", 0, js_array_ext_sum_avg, 1 ),
    JS_ALIAS_DEF("mean", "average" ),
    JS_CFUNC_DEF("compact", 0, js_array_ext_compact ),
    JS_CFUNC_DEF("count", 1, js_array_ext_count ),
    JS_CFUNC_MAGIC_DEF("none", 1, js_array_ext_quantify, 0 ),
    JS_CFUNC_MAGIC_DEF("any", 1, js_array_ext_quantify, 1 ),
    JS_CFUNC_MAGIC_DEF("all", 1, js_array_ext_quantify, 2 ),
    JS_CFUNC_MAGIC_DEF("min", 1, js_array_ext_minmax, 0 ),
    JS_CFUNC_MAGIC_DEF("max", 1, js_array_ext_minmax, 1 ),
    JS_CFUNC_MAGIC_DEF("take", 1, js_array_ext_take, 0 ),
    JS_CFUNC_MAGIC_DEF("drop", 1, js_array_ext_take, 1 ),
    JS_CFUNC_MAGIC_DEF("takeLast", 1, js_array_ext_take, 2 ),
    JS_CFUNC_MAGIC_DEF("dropLast", 1, js_array_ext_take, 3 ),
    JS_CFUNC_DEF("sortBy", 1, js_array_ext_sortby ),
    JS_CFUNC_DEF("groupBy", 1, js_array_ext_groupby ),
    JS_CFUNC_DEF("shuffle", 0, js_array_ext_shuffle ),
    JS_CFUNC_DEF("sample", 0, js_array_ext_sample ),
    JS_CFUNC_DEF("unique", 1, js_array_ext_unique ),
    JS_ALIAS_DEF("uniq", "unique" ),
    JS_CFUNC_DEF("uniqBy", 1, js_array_ext_unique ),
    JS_CFUNC_MAGIC_DEF("intersect", 1, js_array_ext_setop, 0 ),
    JS_ALIAS_DEF("intersection", "intersect" ),
    JS_CFUNC_MAGIC_DEF("difference", 1, js_array_ext_setop, 1 ),
    JS_CFUNC_MAGIC_DEF("without", 1, js_array_ext_setop, 2 ),
    JS_CFUNC_DEF("union", 1, js_array_ext_union ),
    JS_CFUNC_DEF("partition", 1, js_array_ext_partition ),
    JS_CFUNC_DEF("pluck", 1, js_array_ext_pluck ),
    JS_CFUNC_DEF("zip", 1, js_array_ext_zip ),
    JS_CFUNC_DEF("zipWith", 2, js_array_ext_zipwith ),
    JS_CFUNC_DEF("intersperse", 1, js_array_ext_intersperse ),
    JS_CFUNC_DEF("flatten", 0, js_array_ext_flatten ),
    JS_CFUNC_DEF("transpose", 0, js_array_ext_transpose ),
    JS_CFUNC_DEF("xprod", 1, js_array_ext_xprod ),
    JS_CFUNC_DEF("aperture", 1, js_array_ext_aperture ),
    JS_CFUNC_DEF("splitEvery", 1, js_array_ext_splitevery ),
    JS_CFUNC_DEF("splitAt", 1, js_array_ext_splitat ),
    JS_CFUNC_DEF("adjust", 2, js_array_ext_adjust ),
    JS_CFUNC_DEF("update", 2, js_array_ext_update ),
    JS_CFUNC_DEF("move", 2, js_array_ext_move ),
    JS_CFUNC_DEF("swap", 2, js_array_ext_swap ),
    JS_CFUNC_DEF("nth", 1, js_array_ext_nth ),
    JS_CFUNC_DEF("init", 0, js_array_ext_init ),
    JS_CFUNC_DEF("tail", 0, js_array_ext_tail ),
    JS_ALIAS_DEF("head", "first" ),
    JS_CFUNC_MAGIC_DEF("takeWhile", 1, js_array_ext_whilst, 0 ),
    JS_CFUNC_MAGIC_DEF("dropWhile", 1, js_array_ext_whilst, 1 ),
    JS_CFUNC_MAGIC_DEF("takeLastWhile", 1, js_array_ext_whilst, 2 ),
    JS_CFUNC_MAGIC_DEF("dropLastWhile", 1, js_array_ext_whilst, 3 ),
    JS_CFUNC_DEF("append", 1, js_array_ext_append ),
    JS_CFUNC_DEF("prepend", 1, js_array_ext_prepend ),
    JS_CFUNC_DEF("reject", 1, js_array_ext_reject ),
    JS_CFUNC_DEF("insert", 2, js_array_ext_insert ),
    JS_CFUNC_DEF("insertAll", 2, js_array_ext_insertall ),
    JS_CFUNC_DEF("removeAt", 1, js_array_ext_removeat ),
    JS_CFUNC_DEF("zipObj", 1, js_array_ext_zipobj ),
    JS_CFUNC_DEF("fromPairs", 0, js_array_ext_frompairs ),
    JS_CFUNC_DEF("median", 0, js_array_ext_median ),
    JS_CFUNC_DEF("product", 0, js_array_ext_product ),
    JS_CFUNC_DEF("scan", 2, js_array_ext_scan ),
    JS_CFUNC_DEF("countBy", 1, js_array_ext_countby ),
    JS_CFUNC_DEF("indexBy", 1, js_array_ext_indexby ),
    JS_ALIAS_DEF("remove", "reject" ),   /* Sugar remove(matcher) == reject */
    JS_ALIAS_DEF("exclude", "reject" ),  /* Sugar exclude(matcher) == reject */
    JS_CFUNC_DEF("removeRange", 2, js_array_ext_removerange ),
    JS_CFUNC_DEF("splitWhen", 1, js_array_ext_splitwhen ),
    JS_CFUNC_DEF("innerJoin", 2, js_array_ext_innerjoin ),
};

static const JSCFunctionListEntry js_array_proto_funcs[] = {
    JS_CFUNC_DEF("at", 1, js_array_at ),
    JS_CFUNC_DEF("with", 2, js_array_with ),
    JS_CFUNC_DEF("concat", 1, js_array_concat ),
    JS_CFUNC_MAGIC_DEF("every", 1, js_array_every, special_every ),
    JS_CFUNC_MAGIC_DEF("some", 1, js_array_every, special_some ),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_array_every, special_forEach ),
    JS_CFUNC_MAGIC_DEF("map", 1, js_array_every, special_map ),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_array_every, special_filter ),
    JS_CFUNC_MAGIC_DEF("reduce", 1, js_array_reduce, special_reduce ),
    JS_CFUNC_MAGIC_DEF("reduceRight", 1, js_array_reduce, special_reduceRight ),
    JS_CFUNC_DEF("fill", 1, js_array_fill ),
    JS_CFUNC_MAGIC_DEF("find", 1, js_array_find, ArrayFind ),
    JS_CFUNC_MAGIC_DEF("findIndex", 1, js_array_find, ArrayFindIndex ),
    JS_CFUNC_MAGIC_DEF("findLast", 1, js_array_find, ArrayFindLast ),
    JS_CFUNC_MAGIC_DEF("findLastIndex", 1, js_array_find, ArrayFindLastIndex ),
    JS_CFUNC_DEF("indexOf", 1, js_array_indexOf ),
    JS_CFUNC_DEF("lastIndexOf", 1, js_array_lastIndexOf ),
    JS_CFUNC_DEF("includes", 1, js_array_includes ),
    JS_CFUNC_MAGIC_DEF("join", 1, js_array_join, 0 ),
    JS_CFUNC_DEF("toString", 0, js_array_toString ),
    JS_CFUNC_MAGIC_DEF("toLocaleString", 0, js_array_join, 1 ),
    JS_CFUNC_MAGIC_DEF("pop", 0, js_array_pop, 0 ),
    JS_CFUNC_MAGIC_DEF("push", 1, js_array_push, 0 ),
    JS_CFUNC_MAGIC_DEF("shift", 0, js_array_pop, 1 ),
    JS_CFUNC_MAGIC_DEF("unshift", 1, js_array_push, 1 ),
    JS_CFUNC_DEF("reverse", 0, js_array_reverse ),
    JS_CFUNC_DEF("toReversed", 0, js_array_toReversed ),
    JS_CFUNC_DEF("sort", 1, js_array_sort ),
    JS_CFUNC_DEF("toSorted", 1, js_array_toSorted ),
    JS_CFUNC_DEF("slice", 2, js_array_slice ),
    JS_CFUNC_DEF("splice", 2, js_array_splice ),
    JS_CFUNC_DEF("toSpliced", 2, js_array_toSpliced ),
    JS_CFUNC_DEF("copyWithin", 2, js_array_copyWithin ),
    JS_CFUNC_MAGIC_DEF("flatMap", 1, js_array_flatten, 1 ),
    JS_CFUNC_MAGIC_DEF("flat", 0, js_array_flatten, 0 ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_array_iterator, JS_ITERATOR_KIND_VALUE ),
    JS_ALIAS_DEF("[Symbol.iterator]", "values" ),
    JS_CFUNC_MAGIC_DEF("keys", 0, js_create_array_iterator, JS_ITERATOR_KIND_KEY ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_array_iterator, JS_ITERATOR_KIND_KEY_AND_VALUE ),
    JS_OBJECT_DEF("[Symbol.unscopables]", js_array_unscopables_funcs, countof(js_array_unscopables_funcs), JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_array_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_array_iterator_next, 0 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Array Iterator", JS_PROP_CONFIGURABLE ),
};

