/* Iterator */

static JSValue js_iterator_constructor_getset(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv,
                                              int magic,
                                              JSValue *func_data)
{
    int ret;

    if (argc > 0) { // if setter
        if (!JS_IsObject(argv[0]))
            return JS_ThrowTypeErrorNotAnObject(ctx);
        ret = JS_DefinePropertyValue(ctx, this_val, JS_ATOM_constructor,
                                     JS_DupValue(ctx, argv[0]),
                                     JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        if (ret < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    } else {
        return JS_DupValue(ctx, func_data[0]);
    }
}

static JSValue js_iterator_constructor(JSContext *ctx, JSValueConst new_target,
                                       int argc, JSValueConst *argv)
{
    JSObject *p;

    if (JS_TAG_OBJECT != JS_VALUE_GET_TAG(new_target))
        return JS_ThrowTypeError(ctx, "constructor requires 'new'");
    p = JS_VALUE_GET_OBJ(new_target);
    if (p->class_id == JS_CLASS_C_FUNCTION &&
        p->u.cfunc.c_function.generic == js_iterator_constructor) {
        return JS_ThrowTypeError(ctx, "abstract class not constructable");
    }
    return js_create_from_ctor(ctx, new_target, JS_CLASS_ITERATOR);
}

// note: deliberately doesn't use space-saving bit fields for
// |index|, |count| and |running| because tcc miscompiles them
typedef struct JSIteratorConcatData {
    int index, count;             // elements (not pairs!) in values[] array
    BOOL running;
    JSValue iter, next, values[]; // array of (object, method) pairs
} JSIteratorConcatData;

static void js_iterator_concat_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorConcatData *it = p->u.iterator_concat_data;
    if (it) {
        JS_FreeValueRT(rt, it->iter);
        JS_FreeValueRT(rt, it->next);
        for (int i = it->index; i < it->count; i++)
            JS_FreeValueRT(rt, it->values[i]);
        js_free_rt(rt, it);
    }
}

static void js_iterator_concat_mark(JSRuntime *rt, JSValueConst val,
                                    JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorConcatData *it = p->u.iterator_concat_data;
    if (it) {
        JS_MarkValue(rt, it->iter, mark_func);
        JS_MarkValue(rt, it->next, mark_func);
        for (int i = it->index; i < it->count; i++)
            JS_MarkValue(rt, it->values[i], mark_func);
    }
}

static JSValue js_iterator_concat_next(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv,
                                       int *pdone, int magic)
{
    JSValue iter, item, next, val, *obj, *meth, ret;
    JSIteratorConcatData *it;
    int done;

    *pdone = FALSE;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_CONCAT);
    if (!it)
        return JS_EXCEPTION;
    if (it->running)
        return JS_ThrowTypeError(ctx, "already running");

    it->running = TRUE;
    for(;;) {
        if (it->index >= it->count) {
            *pdone = TRUE;
            ret = JS_UNDEFINED;
            break;
        }
        obj = &it->values[it->index + 0];
        meth = &it->values[it->index + 1];
        iter = it->iter;
        if (JS_IsUndefined(iter)) {
            iter = JS_GetIterator2(ctx, *obj, *meth);
            if (JS_IsException(iter))
                goto fail;
            it->iter = iter;
        }
        next = it->next;
        if (JS_IsUndefined(next)) {
            next = JS_GetProperty(ctx, iter, JS_ATOM_next);
            if (JS_IsException(next))
                goto fail;
            it->next = next;
        }
        item = JS_IteratorNext2(ctx, iter, next, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done == 0) {
            ret = item;
            break;
        } else if (done == 2) {
            val = JS_GetProperty(ctx, item, JS_ATOM_done);
            if (JS_IsException(val)) {
                JS_FreeValue(ctx, item);
            fail:
                ret = JS_EXCEPTION;
                break;
            }
            done = JS_ToBoolFree(ctx, val);
            if (done)
                goto done_next;
            ret = JS_GetProperty(ctx, item, JS_ATOM_value);
            JS_FreeValue(ctx, item);
            break;
        } else {
        done_next:
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, iter);
            JS_FreeValue(ctx, next);
            it->iter = JS_UNDEFINED;
            it->next = JS_UNDEFINED;
            JS_FreeValue(ctx, *meth);
            JS_FreeValue(ctx, *obj);
            it->index += 2;
        }
    }
    it->running = FALSE;
    return ret;
}

static JSValue js_iterator_concat_return(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSIteratorConcatData *it;
    JSValue ret;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_CONCAT);
    if (!it)
        return JS_EXCEPTION;
    if (it->running)
        return JS_ThrowTypeError(ctx, "already running");
    ret = JS_UNDEFINED;
    if (!JS_IsUndefined(it->iter)) {
        it->running = TRUE;
        ret = JS_GetProperty(ctx, it->iter, JS_ATOM_return);
        if (JS_IsException(ret)) {
            it->running = FALSE;
            return JS_EXCEPTION;
        }
        ret = JS_CallFree(ctx, ret, it->iter, 0, NULL);
        it->running = FALSE;
    }
    while (it->index < it->count)
        JS_FreeValue(ctx, it->values[it->index++]);
    JS_FreeValue(ctx, it->iter);
    JS_FreeValue(ctx, it->next);
    it->iter = JS_UNDEFINED;
    it->next = JS_UNDEFINED;
    return ret;
}

static const JSCFunctionListEntry js_iterator_concat_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_iterator_concat_next, 0 ),
    JS_CFUNC_DEF("return", 0, js_iterator_concat_return ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Iterator Concat", JS_PROP_CONFIGURABLE ),
};

static JSValue js_iterator_concat(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSIteratorConcatData *it;
    JSValue obj, method;

    it = js_malloc(ctx, sizeof(*it) + 2*argc * sizeof(it->values[0]));
    if (!it)
        return JS_EXCEPTION;
    it->running = FALSE;
    it->index = 0;
    it->count = 0;
    it->iter = JS_UNDEFINED;
    it->next = JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        JSValueConst obj = argv[i];
        if (!JS_IsObject(obj)) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            goto fail;
        }
        method = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_iterator);
        if (JS_IsException(method))
            goto fail;
        if (!JS_IsFunction(ctx, method)) {
            JS_ThrowTypeError(ctx, "not a function");
            JS_FreeValue(ctx, method);
            goto fail;
        }
        it->values[it->count++] = JS_DupValue(ctx, obj);
        it->values[it->count++] = method;
    }
    obj = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_CONCAT);
    if (JS_IsException(obj))
        goto fail;
    JS_SetOpaque(obj, it);
    return obj;
fail:
    for (int i = 0; i < it->count; i++)
        JS_FreeValue(ctx, it->values[i]);
    js_free(ctx, it);
    return JS_EXCEPTION;
}

static JSValue js_iterator_from(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValueConst obj = argv[0];
    JSValue method, iter, wrapper;
    JSIteratorWrapData *it;
    int ret;

    if (!JS_IsObject(obj)) {
        if (!JS_IsString(obj))
            return JS_ThrowTypeError(ctx, "Iterator.from called on non-object");
    }
    method = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_iterator);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    if (JS_IsNull(method) || JS_IsUndefined(method)) {
        iter = JS_DupValue(ctx, obj);
    } else {
        iter = JS_GetIterator2(ctx, obj, method);
        JS_FreeValue(ctx, method);
        if (JS_IsException(iter))
            return JS_EXCEPTION;
    }

    wrapper = JS_UNDEFINED;
    method = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(method))
        goto fail;

    ret = JS_OrdinaryIsInstanceOf(ctx, iter, ctx->iterator_ctor);
    if (ret < 0)
        goto fail;
    if (ret) {
        JS_FreeValue(ctx, method);
        return iter;
    }
    
    wrapper = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_WRAP);
    if (JS_IsException(wrapper))
        goto fail;
    it = js_malloc(ctx, sizeof(*it));
    if (!it)
        goto fail;
    it->wrapped_iter = iter;
    it->wrapped_next = method;
    JS_SetOpaque(wrapper, it);
    return wrapper;

 fail:
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, wrapper);
    return JS_EXCEPTION;
}

typedef enum JSIteratorHelperKindEnum {
    JS_ITERATOR_HELPER_KIND_DROP,
    JS_ITERATOR_HELPER_KIND_EVERY,
    JS_ITERATOR_HELPER_KIND_FILTER,
    JS_ITERATOR_HELPER_KIND_FIND,
    JS_ITERATOR_HELPER_KIND_FLAT_MAP,
    JS_ITERATOR_HELPER_KIND_FOR_EACH,
    JS_ITERATOR_HELPER_KIND_MAP,
    JS_ITERATOR_HELPER_KIND_SOME,
    JS_ITERATOR_HELPER_KIND_TAKE,
} JSIteratorHelperKindEnum;

typedef struct JSIteratorHelperData {
    JSValue obj;
    JSValue next;
    JSValue func; // predicate (filter) or mapper (flatMap, map)
    JSValue inner; // innerValue (flatMap)
    int64_t count; // limit (drop, take) or counter (filter, map, flatMap)
    JSIteratorHelperKindEnum kind : 8;
    uint8_t executing : 1;
    uint8_t done : 1;
} JSIteratorHelperData;

static JSValue js_create_iterator_helper(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic)
{
    JSValueConst func;
    JSValue obj, method;
    int64_t count;
    JSIteratorHelperData *it;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    func = JS_UNDEFINED;
    count = 0;

    switch(magic) {
    case JS_ITERATOR_HELPER_KIND_DROP:
    case JS_ITERATOR_HELPER_KIND_TAKE:
        {
            JSValue v;
            double dlimit;
            v = JS_ToNumber(ctx, argv[0]);
            if (JS_IsException(v))
                goto fail;
            // Check for Infinity.
            if (JS_ToFloat64(ctx, &dlimit, v)) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
            if (isnan(dlimit)) {
                JS_FreeValue(ctx, v);
                goto range_error;
            }
            if (!isfinite(dlimit)) {
                JS_FreeValue(ctx, v);
                if (dlimit < 0)
                    goto range_error;
                else
                    count = MAX_SAFE_INTEGER;
            } else {
                v = JS_ToIntegerFree(ctx, v);
                if (JS_IsException(v))
                    goto fail;
                if (JS_ToInt64Free(ctx, &count, v))
                    goto fail;
            }
            if (count < 0)
                goto range_error;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FILTER:
    case JS_ITERATOR_HELPER_KIND_FLAT_MAP:
    case JS_ITERATOR_HELPER_KIND_MAP:
        {
            func = argv[0];
            if (check_function(ctx, func))
                goto fail;
        }
        break;
    default:
        abort();
        break;
    }

    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        goto fail;
    obj = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_HELPER);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, method);
        goto fail;
    }
    it = js_malloc(ctx, sizeof(*it));
    if (!it) {
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, method);
        goto fail;
    }
    it->kind = magic;
    it->obj = JS_DupValue(ctx, this_val);
    it->func = JS_DupValue(ctx, func);
    it->next = method;
    it->inner = JS_UNDEFINED;
    it->count = count;
    it->executing = 0;
    it->done = 0;
    JS_SetOpaque(obj, it);
    return obj;
range_error:
    JS_ThrowRangeError(ctx, "must be positive");
fail:
    JS_IteratorClose(ctx, this_val, TRUE);
    return JS_EXCEPTION;
}

static JSValue js_iterator_proto_func(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic)
{
    JSValue item, method, ret, func, index_val, r;
    JSValueConst args[2];
    int64_t idx;
    int done;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    func = JS_UNDEFINED;
    method = JS_UNDEFINED;
    
    if (check_function(ctx, argv[0]))
        goto fail;
    func = JS_DupValue(ctx, argv[0]);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        goto fail_no_close;

    r = JS_UNDEFINED;

    switch(magic) {
    case JS_ITERATOR_HELPER_KIND_EVERY:
        {
            r = JS_TRUE;
            for (idx = 0; /*empty*/; idx++) {
                item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    break;
                index_val = JS_NewInt64(ctx, idx);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret))
                    goto fail;
                if (!JS_ToBoolFree(ctx, ret)) {
                    if (JS_IteratorClose(ctx, this_val, FALSE) < 0)
                        r = JS_EXCEPTION;
                    else
                        r = JS_FALSE;
                    break;
                }
                index_val = JS_UNDEFINED;
                ret = JS_UNDEFINED;
                item = JS_UNDEFINED;
            }
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FIND:
        {
            for (idx = 0; /*empty*/; idx++) {
                item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    break;
                index_val = JS_NewInt64(ctx, idx);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret)) {
                    JS_FreeValue(ctx, item);
                    goto fail;
                }
                if (JS_ToBoolFree(ctx, ret)) {
                    if (JS_IteratorClose(ctx, this_val, FALSE) < 0) {
                        JS_FreeValue(ctx, item);
                        r = JS_EXCEPTION;
                    } else {
                        r = item;
                    }
                    break;
                }
                JS_FreeValue(ctx, item);
                index_val = JS_UNDEFINED;
                ret = JS_UNDEFINED;
                item = JS_UNDEFINED;
            }
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FOR_EACH:
        {
            for (idx = 0; /*empty*/; idx++) {
                item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    break;
                index_val = JS_NewInt64(ctx, idx);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret))
                    goto fail;
                JS_FreeValue(ctx, ret);
                index_val = JS_UNDEFINED;
                ret = JS_UNDEFINED;
                item = JS_UNDEFINED;
            }
        }
        break;
    case JS_ITERATOR_HELPER_KIND_SOME:
        {
            r = JS_FALSE;
            for (idx = 0; /*empty*/; idx++) {
                item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    break;
                index_val = JS_NewInt64(ctx, idx);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret))
                    goto fail;
                if (JS_ToBoolFree(ctx, ret)) {
                    if (JS_IteratorClose(ctx, this_val, FALSE) < 0)
                        r = JS_EXCEPTION;
                    else
                        r = JS_TRUE;
                    break;
                }
                index_val = JS_UNDEFINED;
                ret = JS_UNDEFINED;
                item = JS_UNDEFINED;
            }
        }
        break;
    default:
        abort();
        break;
    }

    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, method);
    return r;
 fail:
    JS_IteratorClose(ctx, this_val, TRUE);
 fail_no_close:
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

static JSValue js_iterator_proto_reduce(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSValue item, method, ret, func, index_val, acc;
    JSValueConst args[3];
    int64_t idx;
    int done;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    acc = JS_UNDEFINED;
    func = JS_UNDEFINED;
    method = JS_UNDEFINED;
    if (check_function(ctx, argv[0]))
        goto exception;
    func = JS_DupValue(ctx, argv[0]);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        goto exception;
    if (argc > 1) {
        acc = JS_DupValue(ctx, argv[1]);
        idx = 0;
    } else {
        acc = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(acc))
            goto exception_no_close;
        if (done) {
            JS_ThrowTypeError(ctx, "empty iterator");
            goto exception;
        }
        idx = 1;
    }
    for (/* empty */; /*empty*/; idx++) {
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto exception_no_close;
        if (done)
            break;
        index_val = JS_NewInt64(ctx, idx);
        args[0] = acc;
        args[1] = item;
        args[2] = index_val;
        ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
        JS_FreeValue(ctx, item);
        JS_FreeValue(ctx, index_val);
        if (JS_IsException(ret))
            goto exception;
        JS_FreeValue(ctx, acc);
        acc = ret;
        index_val = JS_UNDEFINED;
        ret = JS_UNDEFINED;
        item = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, method);
    return acc;
 exception:
    JS_IteratorClose(ctx, this_val, TRUE);
 exception_no_close:
    JS_FreeValue(ctx, acc);
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

static JSValue js_iterator_proto_toArray(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSValue item, method, result;
    int64_t idx;
    int done;

    result = JS_UNDEFINED;
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    result = JS_NewArray(ctx);
    if (JS_IsException(result))
        goto exception;
    for (idx = 0; /*empty*/; idx++) {
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto exception;
        if (done)
            break;
        if (JS_DefinePropertyValueInt64(ctx, result, idx, item,
                                        JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            goto exception;
    }
    if (JS_SetProperty(ctx, result, JS_ATOM_length, JS_NewUint32(ctx, idx)) < 0)
        goto exception;
    JS_FreeValue(ctx, method);
    return result;
exception:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

static JSValue js_iterator_proto_iterator(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    return JS_DupValue(ctx, this_val);
}

static JSValue js_iterator_proto_get_toStringTag(JSContext *ctx, JSValueConst this_val)
{
    return JS_AtomToString(ctx, JS_ATOM_Iterator);
}

static JSValue js_iterator_proto_set_toStringTag(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    int res;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (js_same_value(ctx, this_val, ctx->class_proto[JS_CLASS_ITERATOR]))
        return JS_ThrowTypeError(ctx, "Cannot assign to read only property");
    res = JS_GetOwnProperty(ctx, NULL, this_val, JS_ATOM_Symbol_toStringTag);
    if (res < 0)
        return JS_EXCEPTION;
    if (res) {
        if (JS_SetProperty(ctx, this_val, JS_ATOM_Symbol_toStringTag, JS_DupValue(ctx, val)) < 0)
            return JS_EXCEPTION;
    } else {
        if (JS_DefinePropertyValue(ctx, this_val, JS_ATOM_Symbol_toStringTag, JS_DupValue(ctx, val), JS_PROP_C_W_E) < 0)
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

/* Iterator Helper */

static void js_iterator_helper_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorHelperData *it = p->u.iterator_helper_data;
    if (it) {
        JS_FreeValueRT(rt, it->obj);
        JS_FreeValueRT(rt, it->func);
        JS_FreeValueRT(rt, it->next);
        JS_FreeValueRT(rt, it->inner);
        js_free_rt(rt, it);
    }
}

static void js_iterator_helper_mark(JSRuntime *rt, JSValueConst val,
                                   JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorHelperData *it = p->u.iterator_helper_data;
    if (it) {
        JS_MarkValue(rt, it->obj, mark_func);
        JS_MarkValue(rt, it->func, mark_func);
        JS_MarkValue(rt, it->next, mark_func);
        JS_MarkValue(rt, it->inner, mark_func);
    }
}

static JSValue js_iterator_helper_next(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv,
                                      int *pdone, int magic)
{
    JSIteratorHelperData *it;
    JSValue ret;

    *pdone = FALSE;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_HELPER);
    if (!it)
        return JS_EXCEPTION;
    if (it->executing)
        return JS_ThrowTypeError(ctx, "cannot invoke a running iterator");
    if (it->done) {
        *pdone = TRUE;
        return JS_UNDEFINED;
    }

    it->executing = 1;

    switch (it->kind) {
    case JS_ITERATOR_HELPER_KIND_DROP:
        {
            JSValue item, method;
            if (magic == GEN_MAGIC_NEXT) {
                method = JS_DupValue(ctx, it->next);
            } else {
                method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                if (JS_IsException(method))
                    goto fail;
                if (JS_IsUndefined(method) || JS_IsNull(method)) {
                    /* IteratorClose: underlying iterator has no 'return' */
                    *pdone = TRUE;
                    ret = JS_UNDEFINED;
                    goto done;
                }
            }
            while (it->count > 0) {
                it->count--;
                item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
                if (JS_IsException(item)) {
                    JS_FreeValue(ctx, method);
                    goto fail_no_close;
                }
                JS_FreeValue(ctx, item);
                if (magic == GEN_MAGIC_RETURN)
                    *pdone = TRUE;
                if (*pdone) {
                    JS_FreeValue(ctx, method);
                    ret = JS_UNDEFINED;
                    goto done;
                }
            }

            item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
            JS_FreeValue(ctx, method);
            if (JS_IsException(item))
                goto fail_no_close;
            ret = item;
            goto done;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FILTER:
        {
            JSValue item, method, selected, index_val;
            JSValueConst args[2];
            if (magic == GEN_MAGIC_NEXT) {
                method = JS_DupValue(ctx, it->next);
            } else {
                method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                if (JS_IsException(method))
                    goto fail;
                if (JS_IsUndefined(method) || JS_IsNull(method)) {
                    /* IteratorClose: underlying iterator has no 'return' */
                    *pdone = TRUE;
                    ret = JS_UNDEFINED;
                    goto done;
                }
            }
        filter_again:
            item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
            if (JS_IsException(item)) {
                JS_FreeValue(ctx, method);
                goto fail_no_close;
            }
            if (*pdone || magic == GEN_MAGIC_RETURN) {
                JS_FreeValue(ctx, method);
                ret = item;
                goto done;
            }
            index_val = JS_NewInt64(ctx, it->count++);
            args[0] = item;
            args[1] = index_val;
            selected = JS_Call(ctx, it->func, JS_UNDEFINED, countof(args), args);
            JS_FreeValue(ctx, index_val);
            if (JS_IsException(selected)) {
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, method);
                goto fail;
            }
            if (JS_ToBoolFree(ctx, selected)) {
                JS_FreeValue(ctx, method);
                ret = item;
                goto done;
            }
            JS_FreeValue(ctx, item);
            goto filter_again;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FLAT_MAP:
        {
            JSValue item, method, index_val, iter;
            JSValueConst args[2];
        flat_map_again:
            if (JS_IsUndefined(it->inner)) {
                if (magic == GEN_MAGIC_NEXT) {
                    method = JS_DupValue(ctx, it->next);
                } else {
                    method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                    if (JS_IsException(method))
                        goto fail;
                    if (JS_IsUndefined(method) || JS_IsNull(method)) {
                        /* IteratorClose: underlying iterator has no 'return' */
                        *pdone = TRUE;
                        ret = JS_UNDEFINED;
                        goto done;
                    }
                }
                item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
                JS_FreeValue(ctx, method);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (*pdone || magic == GEN_MAGIC_RETURN) {
                    ret = item;
                    goto done;
                }
                index_val = JS_NewInt64(ctx, it->count++);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, it->func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret))
                    goto fail;
                if (!JS_IsObject(ret)) {
                    JS_FreeValue(ctx, ret);
                    JS_ThrowTypeError(ctx, "not an object");
                    goto fail;
                }
                method = JS_GetProperty(ctx, ret, JS_ATOM_Symbol_iterator);
                if (JS_IsException(method)) {
                    JS_FreeValue(ctx, ret);
                    goto fail;
                }
                if (JS_IsNull(method) || JS_IsUndefined(method)) {
                    JS_FreeValue(ctx, method);
                    iter = ret;
                } else {
                    iter = JS_GetIterator2(ctx, ret, method);
                    JS_FreeValue(ctx, method);
                    JS_FreeValue(ctx, ret);
                    if (JS_IsException(iter))
                        goto fail;
                }

                it->inner = iter;
            }

            if (magic == GEN_MAGIC_NEXT)
                method = JS_GetProperty(ctx, it->inner, JS_ATOM_next);
            else
                method = JS_GetProperty(ctx, it->inner, JS_ATOM_return);
            if (JS_IsException(method)) {
            inner_fail:
                JS_IteratorClose(ctx, it->inner, FALSE);
                JS_FreeValue(ctx, it->inner);
                it->inner = JS_UNDEFINED;
                goto fail;
            }
            if (magic == GEN_MAGIC_RETURN && (JS_IsUndefined(method) || JS_IsNull(method))) {
                goto inner_end;
            } else {
                item = JS_IteratorNext(ctx, it->inner, method, 0, NULL, pdone);
                JS_FreeValue(ctx, method);
                if (JS_IsException(item))
                    goto inner_fail;
            }
            if (*pdone) {
            inner_end:
                *pdone = FALSE; // The outer iterator must continue.
                JS_IteratorClose(ctx, it->inner, FALSE);
                JS_FreeValue(ctx, it->inner);
                it->inner = JS_UNDEFINED;
                goto flat_map_again;
            }
            ret = item;
            goto done;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_MAP:
        {
            JSValue item, method, index_val;
            JSValueConst args[2];
            if (magic == GEN_MAGIC_NEXT) {
                method = JS_DupValue(ctx, it->next);
            } else {
                method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                if (JS_IsException(method))
                    goto fail;
                if (JS_IsUndefined(method) || JS_IsNull(method)) {
                    /* IteratorClose: underlying iterator has no 'return' */
                    *pdone = TRUE;
                    ret = JS_UNDEFINED;
                    goto done;
                }
            }
            item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
            JS_FreeValue(ctx, method);
            if (JS_IsException(item))
                goto fail_no_close;
            if (*pdone || magic == GEN_MAGIC_RETURN) {
                ret = item;
                goto done;
            }
            index_val = JS_NewInt64(ctx, it->count++);
            args[0] = item;
            args[1] = index_val;
            ret = JS_Call(ctx, it->func, JS_UNDEFINED, countof(args), args);
            JS_FreeValue(ctx, index_val);
            JS_FreeValue(ctx, item);
            if (JS_IsException(ret))
                goto fail;
            goto done;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_TAKE:
        {
            JSValue item, method;
            if (it->count > 0) {
                if (magic == GEN_MAGIC_NEXT) {
                    method = JS_DupValue(ctx, it->next);
                } else {
                    method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                    if (JS_IsException(method))
                        goto fail;
                    if (JS_IsUndefined(method) || JS_IsNull(method)) {
                        /* IteratorClose: underlying iterator has no 'return' */
                        *pdone = TRUE;
                        ret = JS_UNDEFINED;
                        goto done;
                    }
                }
                it->count--;
                item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
                JS_FreeValue(ctx, method);
                if (JS_IsException(item))
                    goto fail_no_close;
                ret = item;
                goto done;
            }

            *pdone = TRUE;
            if (JS_IteratorClose(ctx, it->obj, FALSE))
                ret = JS_EXCEPTION;
            else
                ret = JS_UNDEFINED;
            goto done;
        }
        break;
    default:
        abort();
    }

 done:
    it->done = magic == GEN_MAGIC_NEXT ? *pdone : 1;
    it->executing = 0;
    return ret;
 fail:
    /* close the iterator object, preserving pending exception */
    JS_IteratorClose(ctx, it->obj, TRUE);
 fail_no_close:
    ret = JS_EXCEPTION;
    goto done;
}

static const JSCFunctionListEntry js_iterator_funcs[] = {
    JS_CFUNC_DEF("concat", 0, js_iterator_concat ),
    JS_CFUNC_DEF("from", 1, js_iterator_from ),
};

static const JSCFunctionListEntry js_iterator_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("drop", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_DROP ),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_FILTER ),
    JS_CFUNC_MAGIC_DEF("flatMap", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_FLAT_MAP ),
    JS_CFUNC_MAGIC_DEF("map", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_MAP ),
    JS_CFUNC_MAGIC_DEF("take", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_TAKE ),
    JS_CFUNC_MAGIC_DEF("every", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_EVERY ),
    JS_CFUNC_MAGIC_DEF("find", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_FIND),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_FOR_EACH ),
    JS_CFUNC_MAGIC_DEF("some", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_SOME ),
    JS_CFUNC_DEF("reduce", 1, js_iterator_proto_reduce ),
    JS_CFUNC_DEF("toArray", 0, js_iterator_proto_toArray ),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, js_iterator_proto_iterator ),
    JS_CGETSET_DEF("[Symbol.toStringTag]", js_iterator_proto_get_toStringTag, js_iterator_proto_set_toStringTag),
};

static const JSCFunctionListEntry js_iterator_helper_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_iterator_helper_next, GEN_MAGIC_NEXT ),
    JS_ITERATOR_NEXT_DEF("return", 0, js_iterator_helper_next, GEN_MAGIC_RETURN ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Iterator Helper", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_array_unscopables_funcs[] = {
    JS_PROP_BOOL_DEF("at", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("copyWithin", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("entries", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("fill", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("find", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("findIndex", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("findLast", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("findLastIndex", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("flat", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("flatMap", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("includes", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("keys", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("toReversed", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("toSorted", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("toSpliced", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("values", TRUE, JS_PROP_C_W_E),
};

/* ============================================================================
 * SugarJS 2.0 / RamdaJS 0.32 non-ECMAScript utilities, installed as `_`-prefixed
 * NON-ENUMERABLE methods on Array.prototype (see SUGAR_RAMDA_NATIVE.md). They
 * operate on any array-like `this` via JS_ToObject + js_get_length64, exactly
 * like the standard methods; nothing native escapes. Phase 1, batch 1.
 * ========================================================================== */

static uint64_t xorshift64star(uint64_t *pstate); /* the Math.random PRNG, below */
/* the Map/Set value hasher + -0 normaliser (symbol.inc.c, later in the unity build) */
static uint32_t map_hash_key(JSValueConst key, int hash_bits);
static JSValueConst map_normalize_key_const(JSContext *ctx, JSValueConst key);

/* read element i of a (possibly array-like) object; *pval is owned, set to
 * JS_UNDEFINED for a missing index. returns -1 (exception) or 0. */
static int js_array_ext_getel(JSContext *ctx, JSValueConst obj, int64_t i,
                              JSValue *pval)
{
    int present = JS_TryGetPropertyInt64(ctx, obj, i, pval);
    if (present < 0)
        return -1;
    if (!present)
        *pval = JS_UNDEFINED;
    return 0;
}

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

/* Match a Sugar/Ramda "matcher" against an element: a function -> ToBool(fn(el));
 * otherwise SameValueZero(matcher, el). Returns 1, 0, or -1 (exception). */
static int js_array_ext_match(JSContext *ctx, JSValueConst match,
                              JSValueConst el)
{
    if (JS_IsFunction(ctx, match)) {
        JSValue r = JS_Call(ctx, match, JS_UNDEFINED, 1, &el);
        int b;
        if (JS_IsException(r))
            return -1;
        b = JS_ToBool(ctx, r);
        JS_FreeValue(ctx, r);
        return b;
    }
    return JS_SameValueZero(ctx, match, el) ? 1 : 0;
}

/* _count(match?) -> length with no argument; else the number of elements the
 * matcher accepts (a value by SameValueZero, or a predicate function). */
static JSValue js_array_ext_count(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, i, c = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        ret = JS_NewInt64(ctx, len);
        goto done;
    }
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el))
            goto done;
        m = js_array_ext_match(ctx, argv[0], el);
        JS_FreeValue(ctx, el);
        if (m < 0)
            goto done;
        c += m;
    }
    ret = JS_NewInt64(ctx, c);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _none / _any / _all (magic 0/1/2) against a value or predicate matcher. */
static JSValue js_array_ext_quantify(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, i;
    JSValueConst match = argc > 0 ? argv[0] : JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el))
            goto done;
        m = js_array_ext_match(ctx, match, el);
        JS_FreeValue(ctx, el);
        if (m < 0)
            goto done;
        if (magic == 1 && m) { ret = JS_TRUE;  goto done; } /* any: found  */
        if (magic == 0 && m) { ret = JS_FALSE; goto done; } /* none: found */
        if (magic == 2 && !m){ ret = JS_FALSE; goto done; } /* all: missed */
    }
    ret = (magic == 1) ? JS_FALSE : JS_TRUE; /* any->false, none/all->true */
 done:
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
    int64_t len, i, jy = 0, jn = 0;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    yes = JS_NewArray(ctx);
    no = JS_NewArray(ctx);
    if (JS_IsException(yes) || JS_IsException(no))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        m = js_array_ext_match(ctx, matcher, el);
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
    if (n <= 0) { JS_FreeValue(ctx, obj); return JS_ThrowRangeError(ctx, "_splitEvery: n must be a positive integer"); }
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
    int64_t len, i = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (magic < 2) {                        /* scan from the front */
        for (i = 0; i < len; i++) {
            JSValue el;
            int m;
            if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
            m = js_array_ext_match(ctx, matcher, el);
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
            m = js_array_ext_match(ctx, matcher, el);
            JS_FreeValue(ctx, el);
            if (m < 0) goto done;
            if (!m) break;
        }
        ret = (magic == 2) ? js_array_ext_build_range(ctx, obj, i, len)
                           : js_array_ext_build_range(ctx, obj, 0, i);
    }
 done:
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
    JSValue obj, result, ret = JS_EXCEPTION;
    JSValueConst matcher = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el)) { JS_FreeValue(ctx, result); goto done; }
        m = js_array_ext_match(ctx, matcher, el);
        if (m < 0) { JS_FreeValue(ctx, el); JS_FreeValue(ctx, result); goto done; }
        if (!m) {
            if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
        } else {
            JS_FreeValue(ctx, el);
        }
    }
    ret = result;
 done:
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

static const JSCFunctionListEntry js_array_ext_funcs[] = {
    JS_CFUNC_DEF("_isEmpty", 0, js_array_ext_isEmpty ),
    JS_CFUNC_DEF("_first", 0, js_array_ext_first ),
    JS_CFUNC_DEF("_last", 0, js_array_ext_last ),
    JS_CFUNC_MAGIC_DEF("_sum", 0, js_array_ext_sum_avg, 0 ),
    JS_CFUNC_MAGIC_DEF("_average", 0, js_array_ext_sum_avg, 1 ),
    JS_ALIAS_DEF("_mean", "_average" ),
    JS_CFUNC_DEF("_compact", 0, js_array_ext_compact ),
    JS_CFUNC_DEF("_count", 1, js_array_ext_count ),
    JS_CFUNC_MAGIC_DEF("_none", 1, js_array_ext_quantify, 0 ),
    JS_CFUNC_MAGIC_DEF("_any", 1, js_array_ext_quantify, 1 ),
    JS_CFUNC_MAGIC_DEF("_all", 1, js_array_ext_quantify, 2 ),
    JS_CFUNC_MAGIC_DEF("_min", 1, js_array_ext_minmax, 0 ),
    JS_CFUNC_MAGIC_DEF("_max", 1, js_array_ext_minmax, 1 ),
    JS_CFUNC_MAGIC_DEF("_take", 1, js_array_ext_take, 0 ),
    JS_CFUNC_MAGIC_DEF("_drop", 1, js_array_ext_take, 1 ),
    JS_CFUNC_MAGIC_DEF("_takeLast", 1, js_array_ext_take, 2 ),
    JS_CFUNC_MAGIC_DEF("_dropLast", 1, js_array_ext_take, 3 ),
    JS_CFUNC_DEF("_sortBy", 1, js_array_ext_sortby ),
    JS_CFUNC_DEF("_groupBy", 1, js_array_ext_groupby ),
    JS_CFUNC_DEF("_shuffle", 0, js_array_ext_shuffle ),
    JS_CFUNC_DEF("_sample", 0, js_array_ext_sample ),
    JS_CFUNC_DEF("_unique", 1, js_array_ext_unique ),
    JS_ALIAS_DEF("_uniq", "_unique" ),
    JS_CFUNC_DEF("_uniqBy", 1, js_array_ext_unique ),
    JS_CFUNC_MAGIC_DEF("_intersect", 1, js_array_ext_setop, 0 ),
    JS_ALIAS_DEF("_intersection", "_intersect" ),
    JS_CFUNC_MAGIC_DEF("_difference", 1, js_array_ext_setop, 1 ),
    JS_CFUNC_MAGIC_DEF("_without", 1, js_array_ext_setop, 2 ),
    JS_CFUNC_DEF("_union", 1, js_array_ext_union ),
    JS_CFUNC_DEF("_partition", 1, js_array_ext_partition ),
    JS_CFUNC_DEF("_pluck", 1, js_array_ext_pluck ),
    JS_CFUNC_DEF("_zip", 1, js_array_ext_zip ),
    JS_CFUNC_DEF("_zipWith", 2, js_array_ext_zipwith ),
    JS_CFUNC_DEF("_intersperse", 1, js_array_ext_intersperse ),
    JS_CFUNC_DEF("_flatten", 0, js_array_ext_flatten ),
    JS_CFUNC_DEF("_transpose", 0, js_array_ext_transpose ),
    JS_CFUNC_DEF("_xprod", 1, js_array_ext_xprod ),
    JS_CFUNC_DEF("_aperture", 1, js_array_ext_aperture ),
    JS_CFUNC_DEF("_splitEvery", 1, js_array_ext_splitevery ),
    JS_CFUNC_DEF("_splitAt", 1, js_array_ext_splitat ),
    JS_CFUNC_DEF("_adjust", 2, js_array_ext_adjust ),
    JS_CFUNC_DEF("_update", 2, js_array_ext_update ),
    JS_CFUNC_DEF("_move", 2, js_array_ext_move ),
    JS_CFUNC_DEF("_swap", 2, js_array_ext_swap ),
    JS_CFUNC_DEF("_nth", 1, js_array_ext_nth ),
    JS_CFUNC_DEF("_init", 0, js_array_ext_init ),
    JS_CFUNC_DEF("_tail", 0, js_array_ext_tail ),
    JS_ALIAS_DEF("_head", "_first" ),
    JS_CFUNC_MAGIC_DEF("_takeWhile", 1, js_array_ext_whilst, 0 ),
    JS_CFUNC_MAGIC_DEF("_dropWhile", 1, js_array_ext_whilst, 1 ),
    JS_CFUNC_MAGIC_DEF("_takeLastWhile", 1, js_array_ext_whilst, 2 ),
    JS_CFUNC_MAGIC_DEF("_dropLastWhile", 1, js_array_ext_whilst, 3 ),
    JS_CFUNC_DEF("_append", 1, js_array_ext_append ),
    JS_CFUNC_DEF("_prepend", 1, js_array_ext_prepend ),
    JS_CFUNC_DEF("_reject", 1, js_array_ext_reject ),
    JS_CFUNC_DEF("_insert", 2, js_array_ext_insert ),
    JS_CFUNC_DEF("_insertAll", 2, js_array_ext_insertall ),
    JS_CFUNC_DEF("_removeAt", 1, js_array_ext_removeat ),
    JS_CFUNC_DEF("_zipObj", 1, js_array_ext_zipobj ),
    JS_CFUNC_DEF("_fromPairs", 0, js_array_ext_frompairs ),
    JS_CFUNC_DEF("_median", 0, js_array_ext_median ),
    JS_CFUNC_DEF("_product", 0, js_array_ext_product ),
    JS_CFUNC_DEF("_scan", 2, js_array_ext_scan ),
    JS_CFUNC_DEF("_countBy", 1, js_array_ext_countby ),
    JS_CFUNC_DEF("_indexBy", 1, js_array_ext_indexby ),
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

/* Number */

static JSValue js_number_constructor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue val, obj;
    if (argc == 0) {
        val = JS_NewInt32(ctx, 0);
    } else {
        val = JS_ToNumeric(ctx, argv[0]);
        if (JS_IsException(val))
            return val;
        switch(JS_VALUE_GET_TAG(val)) {
        case JS_TAG_SHORT_BIG_INT:
            val = JS_NewInt64(ctx, JS_VALUE_GET_SHORT_BIG_INT(val));
            if (JS_IsException(val))
                return val;
            break;
        case JS_TAG_BIG_INT:
            {
                JSBigInt *p = JS_VALUE_GET_PTR(val);
                double d;
                d = js_bigint_to_float64(ctx, p);
                JS_FreeValue(ctx, val);
                val = JS_NewFloat64(ctx, d);
            }
            break;
        default:
            break;
        }
    }
    if (!JS_IsUndefined(new_target)) {
        obj = js_create_from_ctor(ctx, new_target, JS_CLASS_NUMBER);
        if (!JS_IsException(obj))
            JS_SetObjectData(ctx, obj, val);
        return obj;
    } else {
        return val;
    }
}

#if 0
static JSValue js_number___toInteger(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    return JS_ToIntegerFree(ctx, JS_DupValue(ctx, argv[0]));
}

static JSValue js_number___toLength(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    int64_t v;
    if (JS_ToLengthFree(ctx, &v, JS_DupValue(ctx, argv[0])))
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, v);
}
#endif

static JSValue js_number_isNaN(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (!JS_IsNumber(argv[0]))
        return JS_FALSE;
    return js_global_isNaN(ctx, this_val, argc, argv);
}

static JSValue js_number_isFinite(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    if (!JS_IsNumber(argv[0]))
        return JS_FALSE;
    return js_global_isFinite(ctx, this_val, argc, argv);
}

static JSValue js_number_isInteger(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    int ret;
    ret = JS_NumberIsInteger(ctx, argv[0]);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_number_isSafeInteger(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    double d;
    if (!JS_IsNumber(argv[0]))
        return JS_FALSE;
    if (unlikely(JS_ToFloat64(ctx, &d, argv[0])))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, is_safe_integer(d));
}

static const JSCFunctionListEntry js_number_funcs[] = {
    /* global ParseInt and parseFloat should be defined already or delayed */
    JS_ALIAS_BASE_DEF("parseInt", "parseInt", 0 ),
    JS_ALIAS_BASE_DEF("parseFloat", "parseFloat", 0 ),
    JS_CFUNC_DEF("isNaN", 1, js_number_isNaN ),
    JS_CFUNC_DEF("isFinite", 1, js_number_isFinite ),
    JS_CFUNC_DEF("isInteger", 1, js_number_isInteger ),
    JS_CFUNC_DEF("isSafeInteger", 1, js_number_isSafeInteger ),
    JS_PROP_DOUBLE_DEF("MAX_VALUE", 1.7976931348623157e+308, 0 ),
    JS_PROP_DOUBLE_DEF("MIN_VALUE", 5e-324, 0 ),
    JS_PROP_DOUBLE_DEF("NaN", NAN, 0 ),
    JS_PROP_DOUBLE_DEF("NEGATIVE_INFINITY", -INFINITY, 0 ),
    JS_PROP_DOUBLE_DEF("POSITIVE_INFINITY", INFINITY, 0 ),
    JS_PROP_DOUBLE_DEF("EPSILON", 2.220446049250313e-16, 0 ), /* ES6 */
    JS_PROP_DOUBLE_DEF("MAX_SAFE_INTEGER", 9007199254740991.0, 0 ), /* ES6 */
    JS_PROP_DOUBLE_DEF("MIN_SAFE_INTEGER", -9007199254740991.0, 0 ), /* ES6 */
    //JS_CFUNC_DEF("__toInteger", 1, js_number___toInteger ),
    //JS_CFUNC_DEF("__toLength", 1, js_number___toLength ),
};

static JSValue js_thisNumberValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_IsNumber(this_val))
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_NUMBER) {
            if (JS_IsNumber(p->u.object_data))
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a number");
}

static JSValue js_number_valueOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_thisNumberValue(ctx, this_val);
}

static int js_get_radix(JSContext *ctx, JSValueConst val)
{
    int radix;
    if (JS_ToInt32Sat(ctx, &radix, val))
        return -1;
    if (radix < 2 || radix > 36) {
        JS_ThrowRangeError(ctx, "radix must be between 2 and 36");
        return -1;
    }
    return radix;
}

static JSValue js_number_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    JSValue val;
    int base, flags;
    double d;

    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (magic || JS_IsUndefined(argv[0])) {
        base = 10;
    } else {
        base = js_get_radix(ctx, argv[0]);
        if (base < 0)
            goto fail;
    }
    if (JS_VALUE_GET_TAG(val) == JS_TAG_INT) {
        char buf1[70];
        int len;
        len = i64toa_radix(buf1, JS_VALUE_GET_INT(val), base);
        return js_new_string8_len(ctx, buf1, len);
    }
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    flags = JS_DTOA_FORMAT_FREE;
    if (base != 10)
        flags |= JS_DTOA_EXP_DISABLED;
    return js_dtoa2(ctx, d, base, 0, flags);
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_number_toFixed(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue val;
    int f, flags;
    double d;

    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToInt32Sat(ctx, &f, argv[0]))
        return JS_EXCEPTION;
    if (f < 0 || f > 100)
        return JS_ThrowRangeError(ctx, "invalid number of digits");
    if (fabs(d) >= 1e21)
        flags = JS_DTOA_FORMAT_FREE;
    else
        flags = JS_DTOA_FORMAT_FRAC;
    return js_dtoa2(ctx, d, 10, f, flags);
}

static JSValue js_number_toExponential(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValue val;
    int f, flags;
    double d;

    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_ToInt32Sat(ctx, &f, argv[0]))
        return JS_EXCEPTION;
    if (!isfinite(d)) {
        return JS_ToStringFree(ctx,  __JS_NewFloat64(ctx, d));
    }
    if (JS_IsUndefined(argv[0])) {
        flags = JS_DTOA_FORMAT_FREE;
        f = 0;
    } else {
        if (f < 0 || f > 100)
            return JS_ThrowRangeError(ctx, "invalid number of digits");
        f++;
        flags = JS_DTOA_FORMAT_FIXED;
    }
    return js_dtoa2(ctx, d, 10, f, flags | JS_DTOA_EXP_ENABLED);
}

static JSValue js_number_toPrecision(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue val;
    int p;
    double d;

    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToFloat64Free(ctx, &d, val))
        return JS_EXCEPTION;
    if (JS_IsUndefined(argv[0]))
        goto to_string;
    if (JS_ToInt32Sat(ctx, &p, argv[0]))
        return JS_EXCEPTION;
    if (!isfinite(d)) {
    to_string:
        return JS_ToStringFree(ctx,  __JS_NewFloat64(ctx, d));
    }
    if (p < 1 || p > 100)
        return JS_ThrowRangeError(ctx, "invalid number of digits");
    return js_dtoa2(ctx, d, 10, p, JS_DTOA_FORMAT_FIXED);
}

static const JSCFunctionListEntry js_number_proto_funcs[] = {
    JS_CFUNC_DEF("toExponential", 1, js_number_toExponential ),
    JS_CFUNC_DEF("toFixed", 1, js_number_toFixed ),
    JS_CFUNC_DEF("toPrecision", 1, js_number_toPrecision ),
    JS_CFUNC_MAGIC_DEF("toString", 1, js_number_toString, 0 ),
    JS_CFUNC_MAGIC_DEF("toLocaleString", 0, js_number_toString, 1 ),
    JS_CFUNC_DEF("valueOf", 0, js_number_valueOf ),
};

static JSValue js_parseInt(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    const char *str, *p;
    int radix, flags;
    JSValue ret;

    str = JS_ToCString(ctx, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &radix, argv[1])) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    if (radix != 0 && (radix < 2 || radix > 36)) {
        ret = JS_NAN;
    } else {
        p = str;
        p += skip_spaces(p);
        flags = ATOD_INT_ONLY | ATOD_ACCEPT_PREFIX_AFTER_SIGN;
        ret = js_atof(ctx, p, NULL, radix, flags);
    }
    JS_FreeCString(ctx, str);
    return ret;
}

static JSValue js_parseFloat(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    const char *str, *p;
    JSValue ret;

    str = JS_ToCString(ctx, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    p = str;
    p += skip_spaces(p);
    ret = js_atof(ctx, p, NULL, 10, 0);
    JS_FreeCString(ctx, str);
    return ret;
}

/* Boolean */
static JSValue js_boolean_constructor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue val, obj;
    val = JS_NewBool(ctx, JS_ToBool(ctx, argv[0]));
    if (!JS_IsUndefined(new_target)) {
        obj = js_create_from_ctor(ctx, new_target, JS_CLASS_BOOLEAN);
        if (!JS_IsException(obj))
            JS_SetObjectData(ctx, obj, val);
        return obj;
    } else {
        return val;
    }
}

static JSValue js_thisBooleanValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_BOOL)
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_BOOLEAN) {
            if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_BOOL)
                return p->u.object_data;
        }
    }
    return JS_ThrowTypeError(ctx, "not a boolean");
}

static JSValue js_boolean_toString(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue val = js_thisBooleanValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    return JS_AtomToString(ctx, JS_VALUE_GET_BOOL(val) ?
                       JS_ATOM_true : JS_ATOM_false);
}

static JSValue js_boolean_valueOf(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return js_thisBooleanValue(ctx, this_val);
}

static const JSCFunctionListEntry js_boolean_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_boolean_toString ),
    JS_CFUNC_DEF("valueOf", 0, js_boolean_valueOf ),
};

/* String */

static int js_string_get_own_property(JSContext *ctx,
                                      JSPropertyDescriptor *desc,
                                      JSValueConst obj, JSAtom prop)
{
    JSObject *p;
    JSString *p1;
    uint32_t idx, ch;

    /* This is a class exotic method: obj class_id is JS_CLASS_STRING */
    if (__JS_AtomIsTaggedInt(prop)) {
        p = JS_VALUE_GET_OBJ(obj);
        if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_STRING) {
            p1 = JS_VALUE_GET_STRING(p->u.object_data);
            idx = __JS_AtomToUInt32(prop);
            if (idx < p1->len) {
                if (desc) {
                    ch = string_get(p1, idx);
                    desc->flags = JS_PROP_ENUMERABLE;
                    desc->value = js_new_string_char(ctx, ch);
                    desc->getter = JS_UNDEFINED;
                    desc->setter = JS_UNDEFINED;
                }
                return TRUE;
            }
        }
    }
    return FALSE;
}

static int js_string_define_own_property(JSContext *ctx,
                                         JSValueConst this_obj,
                                         JSAtom prop, JSValueConst val,
                                         JSValueConst getter,
                                         JSValueConst setter, int flags)
{
    uint32_t idx;
    JSObject *p;
    JSString *p1, *p2;

    if (__JS_AtomIsTaggedInt(prop)) {
        idx = __JS_AtomToUInt32(prop);
        p = JS_VALUE_GET_OBJ(this_obj);
        if (JS_VALUE_GET_TAG(p->u.object_data) != JS_TAG_STRING)
            goto def;
        p1 = JS_VALUE_GET_STRING(p->u.object_data);
        if (idx >= p1->len)
            goto def;
        if (!check_define_prop_flags(JS_PROP_ENUMERABLE, flags))
            goto fail;
        /* check that the same value is configured */
        if (flags & JS_PROP_HAS_VALUE) {
            if (JS_VALUE_GET_TAG(val) != JS_TAG_STRING)
                goto fail;
            p2 = JS_VALUE_GET_STRING(val);
            if (p2->len != 1)
                goto fail;
            if (string_get(p1, idx) != string_get(p2, 0)) {
            fail:
                return JS_ThrowTypeErrorOrFalse(ctx, flags, "property is not configurable");
            }
        }
        return TRUE;
    } else {
    def:
        return JS_DefineProperty(ctx, this_obj, prop, val, getter, setter,
                                 flags | JS_PROP_NO_EXOTIC);
    }
}

static int js_string_delete_property(JSContext *ctx,
                                     JSValueConst obj, JSAtom prop)
{
    uint32_t idx;

    if (__JS_AtomIsTaggedInt(prop)) {
        idx = __JS_AtomToUInt32(prop);
        if (idx < js_string_obj_get_length(ctx, obj)) {
            return FALSE;
        }
    }
    return TRUE;
}

static const JSClassExoticMethods js_string_exotic_methods = {
    .get_own_property = js_string_get_own_property,
    .define_own_property = js_string_define_own_property,
    .delete_property = js_string_delete_property,
};

static JSValue js_string_constructor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue val, obj;
    if (argc == 0) {
        val = JS_AtomToString(ctx, JS_ATOM_empty_string);
    } else {
        if (JS_IsUndefined(new_target) && JS_IsSymbol(argv[0])) {
            JSAtomStruct *p = JS_VALUE_GET_PTR(argv[0]);
            val = JS_ConcatString3(ctx, "Symbol(", JS_AtomToString(ctx, js_get_atom_index(ctx->rt, p)), ")");
        } else {
            val = JS_ToString(ctx, argv[0]);
        }
        if (JS_IsException(val))
            return val;
    }
    if (!JS_IsUndefined(new_target)) {
        JSString *p1 = JS_VALUE_GET_STRING(val);

        obj = js_create_from_ctor(ctx, new_target, JS_CLASS_STRING);
        if (JS_IsException(obj)) {
            JS_FreeValue(ctx, val);
        } else {
            JS_SetObjectData(ctx, obj, val);
            JS_DefinePropertyValue(ctx, obj, JS_ATOM_length, JS_NewInt32(ctx, p1->len), 0);
        }
        return obj;
    } else {
        return val;
    }
}

static JSValue js_thisStringValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_STRING ||
        JS_VALUE_GET_TAG(this_val) == JS_TAG_STRING_ROPE)
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_STRING) {
            if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_STRING)
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a string");
}

static JSValue js_string_fromCharCode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    int i;
    StringBuffer b_s, *b = &b_s;

    string_buffer_init(ctx, b, argc);

    for(i = 0; i < argc; i++) {
        int32_t c;
        if (JS_ToInt32(ctx, &c, argv[i]) || string_buffer_putc16(b, c & 0xffff)) {
            string_buffer_free(b);
            return JS_EXCEPTION;
        }
    }
    return string_buffer_end(b);
}

static JSValue js_string_fromCodePoint(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    double d;
    int i, c;
    StringBuffer b_s, *b = &b_s;

    /* XXX: could pre-compute string length if all arguments are JS_TAG_INT */

    if (string_buffer_init(ctx, b, argc))
        goto fail;
    for(i = 0; i < argc; i++) {
        if (JS_VALUE_GET_TAG(argv[i]) == JS_TAG_INT) {
            c = JS_VALUE_GET_INT(argv[i]);
            if (c < 0 || c > 0x10ffff)
                goto range_error;
        } else {
            if (JS_ToFloat64(ctx, &d, argv[i]))
                goto fail;
            if (isnan(d) || d < 0 || d > 0x10ffff || (c = (int)d) != d)
                goto range_error;
        }
        if (string_buffer_putc(b, c))
            goto fail;
    }
    return string_buffer_end(b);

 range_error:
    JS_ThrowRangeError(ctx, "invalid code point");
 fail:
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static JSValue js_string_raw(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    // raw(temp,...a)
    JSValue cooked, val, raw;
    StringBuffer b_s, *b = &b_s;
    int64_t i, n;

    string_buffer_init(ctx, b, 0);
    raw = JS_UNDEFINED;
    cooked = JS_ToObject(ctx, argv[0]);
    if (JS_IsException(cooked))
        goto exception;
    raw = JS_ToObjectFree(ctx, JS_GetProperty(ctx, cooked, JS_ATOM_raw));
    if (JS_IsException(raw))
        goto exception;
    if (js_get_length64(ctx, &n, raw) < 0)
        goto exception;

    for (i = 0; i < n; i++) {
        val = JS_ToStringFree(ctx, JS_GetPropertyInt64(ctx, raw, i));
        if (JS_IsException(val))
            goto exception;
        string_buffer_concat_value_free(b, val);
        if (i < n - 1 && i + 1 < argc) {
            if (string_buffer_concat_value(b, argv[i + 1]))
                goto exception;
        }
    }
    JS_FreeValue(ctx, cooked);
    JS_FreeValue(ctx, raw);
    return string_buffer_end(b);

exception:
    JS_FreeValue(ctx, cooked);
    JS_FreeValue(ctx, raw);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

/* only used in test262 */
JSValue js_string_codePointRange(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint32_t start, end, i, n;
    StringBuffer b_s, *b = &b_s;

    if (JS_ToUint32(ctx, &start, argv[0]) ||
        JS_ToUint32(ctx, &end, argv[1]))
        return JS_EXCEPTION;
    end = min_uint32(end, 0x10ffff + 1);

    if (start > end) {
        start = end;
    }
    n = end - start;
    if (end > 0x10000) {
        n += end - max_uint32(start, 0x10000);
    }
    if (string_buffer_init2(ctx, b, n, end >= 0x100))
        return JS_EXCEPTION;
    for(i = start; i < end; i++) {
        string_buffer_putc(b, i);
    }
    return string_buffer_end(b);
}

#if 0
static JSValue js_string___isSpace(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    int c;
    if (JS_ToInt32(ctx, &c, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, lre_is_space(c));
}
#endif

static JSValue js_string_charCodeAt(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue val, ret;
    JSString *p;
    int idx, c;

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    if (JS_ToInt32Sat(ctx, &idx, argv[0])) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    if (idx < 0 || idx >= p->len) {
        ret = JS_NAN;
    } else {
        c = string_get(p, idx);
        ret = JS_NewInt32(ctx, c);
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_charAt(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int is_at)
{
    JSValue val, ret;
    JSString *p;
    int idx, c;

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    if (JS_ToInt32Sat(ctx, &idx, argv[0])) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    if (idx < 0 && is_at)
        idx += p->len;
    if (idx < 0 || idx >= p->len) {
        if (is_at)
            ret = JS_UNDEFINED;
        else
            ret = JS_AtomToString(ctx, JS_ATOM_empty_string);
    } else {
        c = string_get(p, idx);
        ret = js_new_string_char(ctx, c);
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_codePointAt(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue val, ret;
    JSString *p;
    int idx, c;

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    if (JS_ToInt32Sat(ctx, &idx, argv[0])) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    if (idx < 0 || idx >= p->len) {
        ret = JS_UNDEFINED;
    } else {
        c = string_getc(p, &idx);
        ret = JS_NewInt32(ctx, c);
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_concat(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue r;
    int i;

    /* XXX: Use more efficient method */
    /* XXX: This method is OK if r has a single refcount */
    /* XXX: should use string_buffer? */
    r = JS_ToStringCheckObject(ctx, this_val);
    for (i = 0; i < argc; i++) {
        if (JS_IsException(r))
            break;
        r = JS_ConcatString(ctx, r, JS_DupValue(ctx, argv[i]));
    }
    return r;
}

static int string_cmp(JSString *p1, JSString *p2, int x1, int x2, int len)
{
    int i, c1, c2;
    for (i = 0; i < len; i++) {
        if ((c1 = string_get(p1, x1 + i)) != (c2 = string_get(p2, x2 + i)))
            return c1 - c2;
    }
    return 0;
}

static int string_indexof_char(JSString *p, int c, int from)
{
    /* assuming 0 <= from <= p->len. Uses the shared SIMD dispatch table's
     * forward-search kernels (find_u8 = memchr; find_u16 = NEON/scalar). */
    int len = p->len;
    size_t r;
    if (p->is_wide_char) {
        if (c == (uint16_t)c) { /* c > 0xffff cannot be in a uint16 string */
            r = simd.find_u16(p->u.str16 + from, (uint16_t)c,
                              (size_t)(len - from));
            if (r != SIZE_MAX)
                return from + (int)r;
        }
    } else if ((c & ~0xff) == 0) {
        r = simd.find_u8(p->u.str8 + from, (uint8_t)c, (size_t)(len - from));
        if (r != SIZE_MAX)
            return from + (int)r;
    }
    return -1;
}

static int string_indexof(JSString *p1, JSString *p2, int from)
{
    /* assuming 0 <= from <= p1->len */
    int c, i, j, len1 = p1->len, len2 = p2->len;
    if (len2 == 0)
        return from;
    for (i = from, c = string_get(p2, 0); i + len2 <= len1; i = j + 1) {
        j = string_indexof_char(p1, c, i);
        if (j < 0 || j + len2 > len1)
            break;
        if (!string_cmp(p1, p2, j + 1, 1, len2 - 1))
            return j;
    }
    return -1;
}

static int64_t string_advance_index(JSString *p, int64_t index, BOOL unicode)
{
    if (!unicode || index >= p->len || !p->is_wide_char) {
        index++;
    } else {
        int index32 = (int)index;
        string_getc(p, &index32);
        index = index32;
    }
    return index;
}

/* return the position of the first invalid character in the string or
   -1 if none */
static int js_string_find_invalid_codepoint(JSString *p)
{
    int i;
    if (!p->is_wide_char)
        return -1;
    for(i = 0; i < p->len; i++) {
        uint32_t c = p->u.str16[i];
        if (is_surrogate(c)) {
            if (is_hi_surrogate(c) && (i + 1) < p->len
            &&  is_lo_surrogate(p->u.str16[i + 1])) {
                i++;
            } else {
                return i;
            }
        }
    }
    return -1;
}

static JSValue js_string_isWellFormed(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue str;
    JSString *p;
    BOOL ret;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return JS_EXCEPTION;
    p = JS_VALUE_GET_STRING(str);
    ret = (js_string_find_invalid_codepoint(p) < 0);
    JS_FreeValue(ctx, str);
    return JS_NewBool(ctx, ret);
}

static JSValue js_string_toWellFormed(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue str, ret;
    JSString *p;
    int i;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return JS_EXCEPTION;

    p = JS_VALUE_GET_STRING(str);
    /* avoid reallocating the string if it is well-formed */
    i = js_string_find_invalid_codepoint(p);
    if (i < 0)
        return str;

    ret = js_new_string16_len(ctx, p->u.str16, p->len);
    JS_FreeValue(ctx, str);
    if (JS_IsException(ret))
        return JS_EXCEPTION;

    p = JS_VALUE_GET_STRING(ret);
    for (; i < p->len; i++) {
        uint32_t c = p->u.str16[i];
        if (is_surrogate(c)) {
            if (is_hi_surrogate(c) && (i + 1) < p->len
            &&  is_lo_surrogate(p->u.str16[i + 1])) {
                i++;
            } else {
                p->u.str16[i] = 0xFFFD;
            }
        }
    }
    return ret;
}

static JSValue js_string_indexOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int lastIndexOf)
{
    JSValue str, v;
    int i, len, v_len, pos, start, stop, ret, inc;
    JSString *p;
    JSString *p1;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    v = JS_ToString(ctx, argv[0]);
    if (JS_IsException(v))
        goto fail;
    p = JS_VALUE_GET_STRING(str);
    p1 = JS_VALUE_GET_STRING(v);
    len = p->len;
    v_len = p1->len;
    if (lastIndexOf) {
        pos = len - v_len;
        if (argc > 1) {
            double d;
            if (JS_ToFloat64(ctx, &d, argv[1]))
                goto fail;
            if (!isnan(d)) {
                if (d <= 0)
                    pos = 0;
                else if (d < pos)
                    pos = d;
            }
        }
        start = pos;
        stop = 0;
        inc = -1;
    } else {
        pos = 0;
        if (argc > 1) {
            if (JS_ToInt32Clamp(ctx, &pos, argv[1], 0, len, 0))
                goto fail;
        }
        start = pos;
        stop = len - v_len;
        inc = 1;
    }
    ret = -1;
    if (len >= v_len && inc * (stop - start) >= 0) {
        for (i = start;; i += inc) {
            if (!string_cmp(p, p1, i, 0, v_len)) {
                ret = i;
                break;
            }
            if (i == stop)
                break;
        }
    }
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, v);
    return JS_NewInt32(ctx, ret);

fail:
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
}

/* return < 0 if exception or TRUE/FALSE */
static int js_is_regexp(JSContext *ctx, JSValueConst obj);

static JSValue js_string_includes(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    JSValue str, v = JS_UNDEFINED;
    int i, len, v_len, pos, start, stop, ret;
    JSString *p;
    JSString *p1;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    ret = js_is_regexp(ctx, argv[0]);
    if (ret) {
        if (ret > 0)
            JS_ThrowTypeError(ctx, "regexp not supported");
        goto fail;
    }
    v = JS_ToString(ctx, argv[0]);
    if (JS_IsException(v))
        goto fail;
    p = JS_VALUE_GET_STRING(str);
    p1 = JS_VALUE_GET_STRING(v);
    len = p->len;
    v_len = p1->len;
    pos = (magic == 2) ? len : 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &pos, argv[1], 0, len, 0))
            goto fail;
    }
    len -= v_len;
    ret = 0;
    if (magic == 0) {
        start = pos;
        stop = len;
    } else {
        if (magic == 1) {
            if (pos > len)
                goto done;
        } else {
            pos -= v_len;
        }
        start = stop = pos;
    }
    if (start >= 0 && start <= stop) {
        for (i = start;; i++) {
            if (!string_cmp(p, p1, i, 0, v_len)) {
                ret = 1;
                break;
            }
            if (i == stop)
                break;
        }
    }
 done:
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, v);
    return JS_NewBool(ctx, ret);

fail:
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
}

static int check_regexp_g_flag(JSContext *ctx, JSValueConst regexp)
{
    int ret;
    JSValue flags;

    ret = js_is_regexp(ctx, regexp);
    if (ret < 0)
        return -1;
    if (ret) {
        flags = JS_GetProperty(ctx, regexp, JS_ATOM_flags);
        if (JS_IsException(flags))
            return -1;
        if (JS_IsUndefined(flags) || JS_IsNull(flags)) {
            JS_ThrowTypeError(ctx, "cannot convert to object");
            return -1;
        }
        flags = JS_ToStringFree(ctx, flags);
        if (JS_IsException(flags))
            return -1;
        ret = string_indexof_char(JS_VALUE_GET_STRING(flags), 'g', 0);
        JS_FreeValue(ctx, flags);
        if (ret < 0) {
            JS_ThrowTypeError(ctx, "regexp must have the 'g' flag");
            return -1;
        }
    }
    return 0;
}

static JSValue js_string_match(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int atom)
{
    // match(rx), search(rx), matchAll(rx)
    // atom is JS_ATOM_Symbol_match, JS_ATOM_Symbol_search, or JS_ATOM_Symbol_matchAll
    JSValueConst O = this_val, regexp = argv[0], args[2];
    JSValue matcher, S, rx, result, str;
    int args_len;

    if (JS_IsUndefined(O) || JS_IsNull(O))
        return JS_ThrowTypeError(ctx, "cannot convert to object");

    if (JS_IsObject(regexp)) {
        matcher = JS_GetProperty(ctx, regexp, atom);
        if (JS_IsException(matcher))
            return JS_EXCEPTION;
        if (atom == JS_ATOM_Symbol_matchAll) {
            if (check_regexp_g_flag(ctx, regexp) < 0) {
                JS_FreeValue(ctx, matcher);
                return JS_EXCEPTION;
            }
        }
        if (!JS_IsUndefined(matcher) && !JS_IsNull(matcher)) {
            return JS_CallFree(ctx, matcher, regexp, 1, &O);
        }
    }
    S = JS_ToString(ctx, O);
    if (JS_IsException(S))
        return JS_EXCEPTION;
    args_len = 1;
    args[0] = regexp;
    str = JS_UNDEFINED;
    if (atom == JS_ATOM_Symbol_matchAll) {
        str = js_new_string8(ctx, "g");
        if (JS_IsException(str))
            goto fail;
        args[args_len++] = (JSValueConst)str;
    }
    rx = JS_CallConstructor(ctx, ctx->regexp_ctor, args_len, args);
    JS_FreeValue(ctx, str);
    if (JS_IsException(rx)) {
    fail:
        JS_FreeValue(ctx, S);
        return JS_EXCEPTION;
    }
    result = JS_InvokeFree(ctx, rx, atom, 1, (JSValueConst *)&S);
    JS_FreeValue(ctx, S);
    return result;
}

/* if captures != NULL, captures_val and matched are ignored. Otherwise,
   captures_len is ignored */
static int js_string_GetSubstitution(JSContext *ctx,
                                     StringBuffer *b,
                                     JSValueConst matched,
                                     JSString *sp,
                                     uint32_t position,
                                     JSValueConst captures_val,
                                     JSValueConst namedCaptures,
                                     JSValueConst rep,
                                     uint8_t **captures,
                                     uint32_t captures_len)
{
    JSValue capture, name, s;
    uint32_t len, matched_len;
    int i, j, j0, k, k1, shift;
    int c, c1;
    JSString *rp;

    if (JS_VALUE_GET_TAG(rep) != JS_TAG_STRING) {
        JS_ThrowTypeError(ctx, "not a string");
        goto exception;
    }
    shift = sp->is_wide_char;
    rp = JS_VALUE_GET_STRING(rep);

    if (captures) {
        matched_len = (captures[1] - captures[0]) >> shift;
    } else {
        captures_len = 0;
        if (!JS_IsUndefined(captures_val)) {
            if (js_get_length32(ctx, &captures_len, captures_val))
                goto exception;
        }
        if (js_get_length32(ctx, &matched_len, matched))
            goto exception;
    }

    len = rp->len;
    i = 0;
    for(;;) {
        j = string_indexof_char(rp, '$', i);
        if (j < 0 || j + 1 >= len)
            break;
        string_buffer_concat(b, rp, i, j);
        j0 = j++;
        c = string_get(rp, j++);
        if (c == '$') {
            string_buffer_putc8(b, '$');
        } else if (c == '&') {
            if (captures) {
                string_buffer_concat(b, sp, position, position + matched_len);
            } else {
                if (string_buffer_concat_value(b, matched))
                    goto exception;
            }
        } else if (c == '`') {
            string_buffer_concat(b, sp, 0, position);
        } else if (c == '\'') {
            string_buffer_concat(b, sp, position + matched_len, sp->len);
        } else if (c >= '0' && c <= '9') {
            k = c - '0';
            if (j < len) {
                c1 = string_get(rp, j);
                if (c1 >= '0' && c1 <= '9') {
                    /* This behavior is specified in ES6 and refined in ECMA 2019 */
                    /* ECMA 2019 does not have the extra test, but
                       Test262 S15.5.4.11_A3_T1..3 require this behavior */
                    k1 = k * 10 + c1 - '0';
                    if (k1 >= 1 && k1 < captures_len) {
                        k = k1;
                        j++;
                    }
                }
            }
            if (k >= 1 && k < captures_len) {
                if (captures) {
                    int start, end;
                    if (captures[2 * k] && captures[2 * k + 1]) {
                        start = (captures[2 * k] - sp->u.str8) >> shift;
                        end = (captures[2 * k + 1] - sp->u.str8) >> shift;
                        string_buffer_concat(b, sp, start, end);
                    }
                } else {
                    s = JS_GetPropertyInt64(ctx, captures_val, k);
                    if (JS_IsException(s))
                        goto exception;
                    if (!JS_IsUndefined(s)) {
                        if (string_buffer_concat_value_free(b, s))
                            goto exception;
                    }
                }
            } else {
                goto norep;
            }
        } else if (c == '<' && !JS_IsUndefined(namedCaptures)) {
            k = string_indexof_char(rp, '>', j);
            if (k < 0)
                goto norep;
            name = js_sub_string(ctx, rp, j, k);
            if (JS_IsException(name))
                goto exception;
            capture = JS_GetPropertyValue(ctx, namedCaptures, name);
            if (JS_IsException(capture))
                goto exception;
            if (!JS_IsUndefined(capture)) {
                if (string_buffer_concat_value_free(b, capture))
                    goto exception;
            }
            j = k + 1;
        } else {
        norep:
            string_buffer_concat(b, rp, j0, j);
        }
        i = j;
    }
    string_buffer_concat(b, rp, i, rp->len);
    return 0;
exception:
    return -1;
}

static JSValue js_string_replace(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv,
                                 int is_replaceAll)
{
    // replace(rx, rep)
    JSValueConst O = this_val, searchValue = argv[0], replaceValue = argv[1];
    JSValueConst args[3];
    JSValue str, search_str, replaceValue_str, repl_str;
    JSString *sp, *searchp;
    StringBuffer b_s, *b = &b_s;
    int pos, functionalReplace, endOfLastMatch;
    BOOL is_first;

    if (JS_IsUndefined(O) || JS_IsNull(O))
        return JS_ThrowTypeError(ctx, "cannot convert to object");

    search_str = JS_UNDEFINED;
    replaceValue_str = JS_UNDEFINED;
    repl_str = JS_UNDEFINED;

    if (JS_IsObject(searchValue)) {
        JSValue replacer;
        if (is_replaceAll) {
            if (check_regexp_g_flag(ctx, searchValue) < 0)
                return JS_EXCEPTION;
        }
        replacer = JS_GetProperty(ctx, searchValue, JS_ATOM_Symbol_replace);
        if (JS_IsException(replacer))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(replacer) && !JS_IsNull(replacer)) {
            args[0] = O;
            args[1] = replaceValue;
            return JS_CallFree(ctx, replacer, searchValue, 2, args);
        }
    }
    string_buffer_init(ctx, b, 0);

    str = JS_ToString(ctx, O);
    if (JS_IsException(str))
        goto exception;
    search_str = JS_ToString(ctx, searchValue);
    if (JS_IsException(search_str))
        goto exception;
    functionalReplace = JS_IsFunction(ctx, replaceValue);
    if (!functionalReplace) {
        replaceValue_str = JS_ToString(ctx, replaceValue);
        if (JS_IsException(replaceValue_str))
            goto exception;
    }

    sp = JS_VALUE_GET_STRING(str);
    searchp = JS_VALUE_GET_STRING(search_str);
    endOfLastMatch = 0;
    is_first = TRUE;
    for(;;) {
        if (unlikely(searchp->len == 0)) {
            if (is_first)
                pos = 0;
            else if (endOfLastMatch >= sp->len)
                pos = -1;
            else
                pos = endOfLastMatch + 1;
        } else {
            pos = string_indexof(sp, searchp, endOfLastMatch);
        }
        if (pos < 0) {
            if (is_first) {
                string_buffer_free(b);
                JS_FreeValue(ctx, search_str);
                JS_FreeValue(ctx, replaceValue_str);
                return str;
            } else {
                break;
            }
        }

        string_buffer_concat(b, sp, endOfLastMatch, pos);

        if (functionalReplace) {
            args[0] = search_str;
            args[1] = JS_NewInt32(ctx, pos);
            args[2] = str;
            repl_str = JS_ToStringFree(ctx, JS_Call(ctx, replaceValue, JS_UNDEFINED, 3, args));
            if (JS_IsException(repl_str))
                goto exception;
            string_buffer_concat_value_free(b, repl_str);
        } else {
            if (js_string_GetSubstitution(ctx, b, search_str, sp, pos,
                                          JS_UNDEFINED, JS_UNDEFINED, replaceValue_str,
                                          NULL, 0)) {
                goto exception;
            }
        }

        endOfLastMatch = pos + searchp->len;
        is_first = FALSE;
        if (!is_replaceAll)
            break;
    }
    string_buffer_concat(b, sp, endOfLastMatch, sp->len);
    JS_FreeValue(ctx, search_str);
    JS_FreeValue(ctx, replaceValue_str);
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

exception:
    string_buffer_free(b);
    JS_FreeValue(ctx, search_str);
    JS_FreeValue(ctx, replaceValue_str);
    JS_FreeValue(ctx, str);
    return JS_EXCEPTION;
}

static JSValue js_string_split(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    // split(sep, limit)
    JSValueConst O = this_val, separator = argv[0], limit = argv[1];
    JSValueConst args[2];
    JSValue S, A, R, T;
    uint32_t lim, lengthA;
    int64_t p, q, s, r, e;
    JSString *sp, *rp;

    if (JS_IsUndefined(O) || JS_IsNull(O))
        return JS_ThrowTypeError(ctx, "cannot convert to object");

    S = JS_UNDEFINED;
    A = JS_UNDEFINED;
    R = JS_UNDEFINED;

    if (JS_IsObject(separator)) {
        JSValue splitter;
        splitter = JS_GetProperty(ctx, separator, JS_ATOM_Symbol_split);
        if (JS_IsException(splitter))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(splitter) && !JS_IsNull(splitter)) {
            args[0] = O;
            args[1] = limit;
            return JS_CallFree(ctx, splitter, separator, 2, args);
        }
    }
    S = JS_ToString(ctx, O);
    if (JS_IsException(S))
        goto exception;
    A = JS_NewArray(ctx);
    if (JS_IsException(A))
        goto exception;
    lengthA = 0;
    if (JS_IsUndefined(limit)) {
        lim = 0xffffffff;
    } else {
        if (JS_ToUint32(ctx, &lim, limit) < 0)
            goto exception;
    }
    sp = JS_VALUE_GET_STRING(S);
    s = sp->len;
    R = JS_ToString(ctx, separator);
    if (JS_IsException(R))
        goto exception;
    rp = JS_VALUE_GET_STRING(R);
    r = rp->len;
    p = 0;
    if (lim == 0)
        goto done;
    if (JS_IsUndefined(separator))
        goto add_tail;
    if (s == 0) {
        if (r != 0)
            goto add_tail;
        goto done;
    }
    for (q = p; (q += !r) <= s - r - !r; q = p = e + r) {
        e = string_indexof(sp, rp, q);
        if (e < 0)
            break;
        T = js_sub_string(ctx, sp, p, e);
        if (JS_IsException(T))
            goto exception;
        if (JS_CreateDataPropertyUint32(ctx, A, lengthA++, T, 0) < 0)
            goto exception;
        if (lengthA == lim)
            goto done;
    }
add_tail:
    T = js_sub_string(ctx, sp, p, s);
    if (JS_IsException(T))
        goto exception;
    if (JS_CreateDataPropertyUint32(ctx, A, lengthA++, T,0 ) < 0)
        goto exception;
done:
    JS_FreeValue(ctx, S);
    JS_FreeValue(ctx, R);
    return A;

exception:
    JS_FreeValue(ctx, A);
    JS_FreeValue(ctx, S);
    JS_FreeValue(ctx, R);
    return JS_EXCEPTION;
}

static JSValue js_string_substring(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue str, ret;
    int a, b, start, end;
    JSString *p;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    p = JS_VALUE_GET_STRING(str);
    if (JS_ToInt32Clamp(ctx, &a, argv[0], 0, p->len, 0)) {
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    b = p->len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &b, argv[1], 0, p->len, 0)) {
            JS_FreeValue(ctx, str);
            return JS_EXCEPTION;
        }
    }
    if (a < b) {
        start = a;
        end = b;
    } else {
        start = b;
        end = a;
    }
    ret = js_sub_string(ctx, p, start, end);
    JS_FreeValue(ctx, str);
    return ret;
}

static JSValue js_string_substr(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue str, ret;
    int a, len, n;
    JSString *p;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    p = JS_VALUE_GET_STRING(str);
    len = p->len;
    if (JS_ToInt32Clamp(ctx, &a, argv[0], 0, len, len)) {
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    n = len - a;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &n, argv[1], 0, len - a, 0)) {
            JS_FreeValue(ctx, str);
            return JS_EXCEPTION;
        }
    }
    ret = js_sub_string(ctx, p, a, a + n);
    JS_FreeValue(ctx, str);
    return ret;
}

static JSValue js_string_slice(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JSValue str, ret;
    int len, start, end;
    JSString *p;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    p = JS_VALUE_GET_STRING(str);
    len = p->len;
    if (JS_ToInt32Clamp(ctx, &start, argv[0], 0, len, len)) {
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    end = len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &end, argv[1], 0, len, len)) {
            JS_FreeValue(ctx, str);
            return JS_EXCEPTION;
        }
    }
    ret = js_sub_string(ctx, p, start, max_int(end, start));
    JS_FreeValue(ctx, str);
    return ret;
}

static JSValue js_string_pad(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int padEnd)
{
    JSValue str, v = JS_UNDEFINED;
    StringBuffer b_s, *b = &b_s;
    JSString *p, *p1 = NULL;
    int n, len, c = ' ';

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        goto fail1;
    if (JS_ToInt32Sat(ctx, &n, argv[0]))
        goto fail2;
    p = JS_VALUE_GET_STRING(str);
    len = p->len;
    if (len >= n)
        return str;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        v = JS_ToString(ctx, argv[1]);
        if (JS_IsException(v))
            goto fail2;
        p1 = JS_VALUE_GET_STRING(v);
        if (p1->len == 0) {
            JS_FreeValue(ctx, v);
            return str;
        }
        if (p1->len == 1) {
            c = string_get(p1, 0);
            p1 = NULL;
        }
    }
    if (n > JS_STRING_LEN_MAX) {
        JS_ThrowRangeError(ctx, "invalid string length");
        goto fail3;
    }
    if (string_buffer_init(ctx, b, n))
        goto fail3;
    n -= len;
    if (padEnd) {
        if (string_buffer_concat(b, p, 0, len))
            goto fail;
    }
    if (p1) {
        while (n > 0) {
            int chunk = min_int(n, p1->len);
            if (string_buffer_concat(b, p1, 0, chunk))
                goto fail;
            n -= chunk;
        }
    } else {
        if (string_buffer_fill(b, c, n))
            goto fail;
    }
    if (!padEnd) {
        if (string_buffer_concat(b, p, 0, len))
            goto fail;
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

fail:
    string_buffer_free(b);
fail3:
    JS_FreeValue(ctx, v);
fail2:
    JS_FreeValue(ctx, str);
fail1:
    return JS_EXCEPTION;
}

static JSValue js_string_repeat(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int64_t val;
    int n, len;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        goto fail;
    if (JS_ToInt64Sat(ctx, &val, argv[0]))
        goto fail;
    if (val < 0 || val > 2147483647) {
        JS_ThrowRangeError(ctx, "invalid repeat count");
        goto fail;
    }
    n = val;
    p = JS_VALUE_GET_STRING(str);
    len = p->len;
    if (len == 0 || n == 1)
        return str;
    // XXX: potential arithmetic overflow
    if (val * len > JS_STRING_LEN_MAX) {
        JS_ThrowRangeError(ctx, "invalid string length");
        goto fail;
    }
    if (string_buffer_init2(ctx, b, n * len, p->is_wide_char))
        goto fail;
    if (len == 1) {
        string_buffer_fill(b, string_get(p, 0), n);
    } else {
        while (n-- > 0) {
            string_buffer_concat(b, p, 0, len);
        }
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

fail:
    JS_FreeValue(ctx, str);
    return JS_EXCEPTION;
}

static JSValue js_string_trim(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    JSValue str, ret;
    int a, b, len;
    JSString *p;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    p = JS_VALUE_GET_STRING(str);
    a = 0;
    b = len = p->len;
    if (magic & 1) {
        while (a < len && lre_is_space(string_get(p, a)))
            a++;
    }
    if (magic & 2) {
        while (b > a && lre_is_space(string_get(p, b - 1)))
            b--;
    }
    ret = js_sub_string(ctx, p, a, b);
    JS_FreeValue(ctx, str);
    return ret;
}

/* return 0 if before the first char */
static int string_prevc(JSString *p, int *pidx)
{
    int idx, c, c1;

    idx = *pidx;
    if (idx <= 0)
        return 0;
    idx--;
    if (p->is_wide_char) {
        c = p->u.str16[idx];
        if (is_lo_surrogate(c) && idx > 0) {
            c1 = p->u.str16[idx - 1];
            if (is_hi_surrogate(c1)) {
                c = from_surrogate(c1, c);
                idx--;
            }
        }
    } else {
        c = p->u.str8[idx];
    }
    *pidx = idx;
    return c;
}

static BOOL test_final_sigma(JSString *p, int sigma_pos)
{
    int k, c1;

    /* before C: skip case ignorable chars and check there is
       a cased letter */
    k = sigma_pos;
    for(;;) {
        c1 = string_prevc(p, &k);
        if (!lre_is_case_ignorable(c1))
            break;
    }
    if (!lre_is_cased(c1))
        return FALSE;

    /* after C: skip case ignorable chars and check there is
       no cased letter */
    k = sigma_pos + 1;
    for(;;) {
        if (k >= p->len)
            return TRUE;
        c1 = string_getc(p, &k);
        if (!lre_is_case_ignorable(c1))
            break;
    }
    return !lre_is_cased(c1);
}

static JSValue js_string_toLowerCase(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int to_lower)
{
    JSValue val;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int i, c, j, l;
    uint32_t res[LRE_CC_RES_LEN_MAX];

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    if (p->len == 0)
        return val;
    if (string_buffer_init(ctx, b, p->len))
        goto fail;
    for(i = 0; i < p->len;) {
        c = string_getc(p, &i);
        if (c == 0x3a3 && to_lower && test_final_sigma(p, i - 1)) {
            res[0] = 0x3c2; /* final sigma */
            l = 1;
        } else {
            l = lre_case_conv(res, c, to_lower);
        }
        for(j = 0; j < l; j++) {
            if (string_buffer_putc(b, res[j]))
                goto fail;
        }
    }
    JS_FreeValue(ctx, val);
    return string_buffer_end(b);
 fail:
    JS_FreeValue(ctx, val);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

#ifdef CONFIG_ALL_UNICODE

/* return (-1, NULL) if exception, otherwise (len, buf) */
static int JS_ToUTF32String(JSContext *ctx, uint32_t **pbuf, JSValueConst val1)
{
    JSValue val;
    JSString *p;
    uint32_t *buf;
    int i, j, len;

    val = JS_ToString(ctx, val1);
    if (JS_IsException(val))
        return -1;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    /* UTF32 buffer length is len minus the number of correct surrogates pairs */
    buf = js_malloc(ctx, sizeof(buf[0]) * max_int(len, 1));
    if (!buf) {
        JS_FreeValue(ctx, val);
        goto fail;
    }
    for(i = j = 0; i < len;)
        buf[j++] = string_getc(p, &i);
    JS_FreeValue(ctx, val);
    *pbuf = buf;
    return j;
 fail:
    *pbuf = NULL;
    return -1;
}

static JSValue JS_NewUTF32String(JSContext *ctx, const uint32_t *buf, int len)
{
    int i;
    StringBuffer b_s, *b = &b_s;
    if (string_buffer_init(ctx, b, len))
        return JS_EXCEPTION;
    for(i = 0; i < len; i++) {
        if (string_buffer_putc(b, buf[i]))
            goto fail;
    }
    return string_buffer_end(b);
 fail:
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static int js_string_normalize1(JSContext *ctx, uint32_t **pout_buf,
                                JSValueConst val,
                                UnicodeNormalizationEnum n_type)
{
    int buf_len, out_len;
    uint32_t *buf, *out_buf;

    buf_len = JS_ToUTF32String(ctx, &buf, val);
    if (buf_len < 0)
        return -1;
    out_len = unicode_normalize(&out_buf, buf, buf_len, n_type,
                                ctx->rt, js_realloc_dbuf_rt);
    js_free(ctx, buf);
    if (out_len < 0)
        return -1;
    *pout_buf = out_buf;
    return out_len;
}

static JSValue js_string_normalize(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *form, *p;
    size_t form_len;
    int is_compat, out_len;
    UnicodeNormalizationEnum n_type;
    JSValue val;
    uint32_t *out_buf;

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;

    if (argc == 0 || JS_IsUndefined(argv[0])) {
        n_type = UNICODE_NFC;
    } else {
        form = JS_ToCStringLen(ctx, &form_len, argv[0]);
        if (!form)
            goto fail1;
        p = form;
        if (p[0] != 'N' || p[1] != 'F')
            goto bad_form;
        p += 2;
        is_compat = FALSE;
        if (*p == 'K') {
            is_compat = TRUE;
            p++;
        }
        if (*p == 'C' || *p == 'D') {
            n_type = UNICODE_NFC + is_compat * 2 + (*p - 'C');
            if ((p + 1 - form) != form_len)
                goto bad_form;
        } else {
        bad_form:
            JS_FreeCString(ctx, form);
            JS_ThrowRangeError(ctx, "bad normalization form");
        fail1:
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, form);
    }

    out_len = js_string_normalize1(ctx, &out_buf, val, n_type);
    JS_FreeValue(ctx, val);
    if (out_len < 0)
        return JS_EXCEPTION;
    val = JS_NewUTF32String(ctx, out_buf, out_len);
    js_free(ctx, out_buf);
    return val;
}

/* return < 0, 0 or > 0 */
static int js_UTF32_compare(const uint32_t *buf1, int buf1_len,
                            const uint32_t *buf2, int buf2_len)
{
    int i, len, c, res;
    len = min_int(buf1_len, buf2_len);
    for(i = 0; i < len; i++) {
        /* Note: range is limited so a subtraction is valid */
        c = buf1[i] - buf2[i];
        if (c != 0)
            return c;
    }
    if (buf1_len == buf2_len)
        res = 0;
    else if (buf1_len < buf2_len)
        res = -1;
    else
        res = 1;
    return res;
}

static JSValue js_string_localeCompare(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValue a, b;
    int cmp, a_len, b_len;
    uint32_t *a_buf, *b_buf;

    a = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(a))
        return JS_EXCEPTION;
    b = JS_ToString(ctx, argv[0]);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return JS_EXCEPTION;
    }
    a_len = js_string_normalize1(ctx, &a_buf, a, UNICODE_NFC);
    JS_FreeValue(ctx, a);
    if (a_len < 0) {
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }

    b_len = js_string_normalize1(ctx, &b_buf, b, UNICODE_NFC);
    JS_FreeValue(ctx, b);
    if (b_len < 0) {
        js_free(ctx, a_buf);
        return JS_EXCEPTION;
    }
    cmp = js_UTF32_compare(a_buf, a_len, b_buf, b_len);
    js_free(ctx, a_buf);
    js_free(ctx, b_buf);
    return JS_NewInt32(ctx, cmp);
}
#else /* CONFIG_ALL_UNICODE */
static JSValue js_string_localeCompare(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValue a, b;
    int cmp;

    a = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(a))
        return JS_EXCEPTION;
    b = JS_ToString(ctx, argv[0]);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return JS_EXCEPTION;
    }
    cmp = js_string_compare(ctx, JS_VALUE_GET_STRING(a), JS_VALUE_GET_STRING(b));
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return JS_NewInt32(ctx, cmp);
}
#endif /* !CONFIG_ALL_UNICODE */

/* also used for String.prototype.valueOf */
static JSValue js_string_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return js_thisStringValue(ctx, this_val);
}

/* String Iterator */

static JSValue js_string_iterator_next(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv,
                                       BOOL *pdone, int magic)
{
    JSArrayIteratorData *it;
    uint32_t idx, c, start;
    JSString *p;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_STRING_ITERATOR);
    if (!it) {
        *pdone = FALSE;
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(it->obj))
        goto done;
    p = JS_VALUE_GET_STRING(it->obj);
    idx = it->idx;
    if (idx >= p->len) {
        JS_FreeValue(ctx, it->obj);
        it->obj = JS_UNDEFINED;
    done:
        *pdone = TRUE;
        return JS_UNDEFINED;
    }

    start = idx;
    c = string_getc(p, (int *)&idx);
    it->idx = idx;
    *pdone = FALSE;
    if (c <= 0xffff) {
        return js_new_string_char(ctx, c);
    } else {
        return js_new_string16_len(ctx, p->u.str16 + start, 2);
    }
}

/* ES6 Annex B 2.3.2 etc. */
enum {
    magic_string_anchor,
    magic_string_big,
    magic_string_blink,
    magic_string_bold,
    magic_string_fixed,
    magic_string_fontcolor,
    magic_string_fontsize,
    magic_string_italics,
    magic_string_link,
    magic_string_small,
    magic_string_strike,
    magic_string_sub,
    magic_string_sup,
};

static JSValue js_string_CreateHTML(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue str;
    const JSString *p;
    StringBuffer b_s, *b = &b_s;
    static struct { const char *tag, *attr; } const defs[] = {
        { "a", "name" }, { "big", NULL }, { "blink", NULL }, { "b", NULL },
        { "tt", NULL }, { "font", "color" }, { "font", "size" }, { "i", NULL },
        { "a", "href" }, { "small", NULL }, { "strike", NULL },
        { "sub", NULL }, { "sup", NULL },
    };

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return JS_EXCEPTION;
    string_buffer_init(ctx, b, 7);
    string_buffer_putc8(b, '<');
    string_buffer_puts8(b, defs[magic].tag);
    if (defs[magic].attr) {
        // r += " " + attr + "=\"" + value + "\"";
        JSValue value;
        int i;

        string_buffer_putc8(b, ' ');
        string_buffer_puts8(b, defs[magic].attr);
        string_buffer_puts8(b, "=\"");
        value = JS_ToStringCheckObject(ctx, argv[0]);
        if (JS_IsException(value)) {
            JS_FreeValue(ctx, str);
            string_buffer_free(b);
            return JS_EXCEPTION;
        }
        p = JS_VALUE_GET_STRING(value);
        for (i = 0; i < p->len; i++) {
            int c = string_get(p, i);
            if (c == '"') {
                string_buffer_puts8(b, "&quot;");
            } else {
                string_buffer_putc16(b, c);
            }
        }
        JS_FreeValue(ctx, value);
        string_buffer_putc8(b, '\"');
    }
    // return r + ">" + str + "</" + tag + ">";
    string_buffer_putc8(b, '>');
    string_buffer_concat_value_free(b, str);
    string_buffer_puts8(b, "</");
    string_buffer_puts8(b, defs[magic].tag);
    string_buffer_putc8(b, '>');
    return string_buffer_end(b);
}

static const JSCFunctionListEntry js_string_funcs[] = {
    JS_CFUNC_DEF("fromCharCode", 1, js_string_fromCharCode ),
    JS_CFUNC_DEF("fromCodePoint", 1, js_string_fromCodePoint ),
    JS_CFUNC_DEF("raw", 1, js_string_raw ),
};

static const JSCFunctionListEntry js_string_proto_funcs[] = {
    JS_PROP_INT32_DEF("length", 0, JS_PROP_CONFIGURABLE ),
    JS_CFUNC_MAGIC_DEF("at", 1, js_string_charAt, 1 ),
    JS_CFUNC_DEF("charCodeAt", 1, js_string_charCodeAt ),
    JS_CFUNC_MAGIC_DEF("charAt", 1, js_string_charAt, 0 ),
    JS_CFUNC_DEF("concat", 1, js_string_concat ),
    JS_CFUNC_DEF("codePointAt", 1, js_string_codePointAt ),
    JS_CFUNC_DEF("isWellFormed", 0, js_string_isWellFormed ),
    JS_CFUNC_DEF("toWellFormed", 0, js_string_toWellFormed ),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_string_indexOf, 0 ),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_string_indexOf, 1 ),
    JS_CFUNC_MAGIC_DEF("includes", 1, js_string_includes, 0 ),
    JS_CFUNC_MAGIC_DEF("endsWith", 1, js_string_includes, 2 ),
    JS_CFUNC_MAGIC_DEF("startsWith", 1, js_string_includes, 1 ),
    JS_CFUNC_MAGIC_DEF("match", 1, js_string_match, JS_ATOM_Symbol_match ),
    JS_CFUNC_MAGIC_DEF("matchAll", 1, js_string_match, JS_ATOM_Symbol_matchAll ),
    JS_CFUNC_MAGIC_DEF("search", 1, js_string_match, JS_ATOM_Symbol_search ),
    JS_CFUNC_DEF("split", 2, js_string_split ),
    JS_CFUNC_DEF("substring", 2, js_string_substring ),
    JS_CFUNC_DEF("substr", 2, js_string_substr ),
    JS_CFUNC_DEF("slice", 2, js_string_slice ),
    JS_CFUNC_DEF("repeat", 1, js_string_repeat ),
    JS_CFUNC_MAGIC_DEF("replace", 2, js_string_replace, 0 ),
    JS_CFUNC_MAGIC_DEF("replaceAll", 2, js_string_replace, 1 ),
    JS_CFUNC_MAGIC_DEF("padEnd", 1, js_string_pad, 1 ),
    JS_CFUNC_MAGIC_DEF("padStart", 1, js_string_pad, 0 ),
    JS_CFUNC_MAGIC_DEF("trim", 0, js_string_trim, 3 ),
    JS_CFUNC_MAGIC_DEF("trimEnd", 0, js_string_trim, 2 ),
    JS_ALIAS_DEF("trimRight", "trimEnd" ),
    JS_CFUNC_MAGIC_DEF("trimStart", 0, js_string_trim, 1 ),
    JS_ALIAS_DEF("trimLeft", "trimStart" ),
    JS_CFUNC_DEF("toString", 0, js_string_toString ),
    JS_CFUNC_DEF("valueOf", 0, js_string_toString ),
    JS_CFUNC_MAGIC_DEF("toLowerCase", 0, js_string_toLowerCase, 1 ),
    JS_CFUNC_MAGIC_DEF("toUpperCase", 0, js_string_toLowerCase, 0 ),
    JS_CFUNC_MAGIC_DEF("toLocaleLowerCase", 0, js_string_toLowerCase, 1 ),
    JS_CFUNC_MAGIC_DEF("toLocaleUpperCase", 0, js_string_toLowerCase, 0 ),
    JS_CFUNC_MAGIC_DEF("[Symbol.iterator]", 0, js_create_array_iterator, JS_ITERATOR_KIND_VALUE | 4 ),
    /* ES6 Annex B 2.3.2 etc. */
    JS_CFUNC_MAGIC_DEF("anchor", 1, js_string_CreateHTML, magic_string_anchor ),
    JS_CFUNC_MAGIC_DEF("big", 0, js_string_CreateHTML, magic_string_big ),
    JS_CFUNC_MAGIC_DEF("blink", 0, js_string_CreateHTML, magic_string_blink ),
    JS_CFUNC_MAGIC_DEF("bold", 0, js_string_CreateHTML, magic_string_bold ),
    JS_CFUNC_MAGIC_DEF("fixed", 0, js_string_CreateHTML, magic_string_fixed ),
    JS_CFUNC_MAGIC_DEF("fontcolor", 1, js_string_CreateHTML, magic_string_fontcolor ),
    JS_CFUNC_MAGIC_DEF("fontsize", 1, js_string_CreateHTML, magic_string_fontsize ),
    JS_CFUNC_MAGIC_DEF("italics", 0, js_string_CreateHTML, magic_string_italics ),
    JS_CFUNC_MAGIC_DEF("link", 1, js_string_CreateHTML, magic_string_link ),
    JS_CFUNC_MAGIC_DEF("small", 0, js_string_CreateHTML, magic_string_small ),
    JS_CFUNC_MAGIC_DEF("strike", 0, js_string_CreateHTML, magic_string_strike ),
    JS_CFUNC_MAGIC_DEF("sub", 0, js_string_CreateHTML, magic_string_sub ),
    JS_CFUNC_MAGIC_DEF("sup", 0, js_string_CreateHTML, magic_string_sup ),
};

static const JSCFunctionListEntry js_string_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_string_iterator_next, 0 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "String Iterator", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_string_proto_normalize[] = {
#ifdef CONFIG_ALL_UNICODE
    JS_CFUNC_DEF("normalize", 0, js_string_normalize ),
#endif
    JS_CFUNC_DEF("localeCompare", 1, js_string_localeCompare ),
};

int JS_AddIntrinsicStringNormalize(JSContext *ctx)
{
    return JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_STRING], js_string_proto_normalize,
                                      countof(js_string_proto_normalize));
}

/* Math */

/* precondition: a and b are not NaN */
static double js_fmin(double a, double b)
{
    if (a == 0 && b == 0) {
        JSFloat64Union a1, b1;
        a1.d = a;
        b1.d = b;
        a1.u64 |= b1.u64;
        return a1.d;
    } else {
        return fmin(a, b);
    }
}

/* precondition: a and b are not NaN */
static double js_fmax(double a, double b)
{
    if (a == 0 && b == 0) {
        JSFloat64Union a1, b1;
        a1.d = a;
        b1.d = b;
        a1.u64 &= b1.u64;
        return a1.d;
    } else {
        return fmax(a, b);
    }
}

static JSValue js_math_min_max(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    BOOL is_max = magic;
    double r, a;
    int i;
    uint32_t tag;

    if (unlikely(argc == 0)) {
        return __JS_NewFloat64(ctx, is_max ? -1.0 / 0.0 : 1.0 / 0.0);
    }

    tag = JS_VALUE_GET_TAG(argv[0]);
    if (tag == JS_TAG_INT) {
        int a1, r1 = JS_VALUE_GET_INT(argv[0]);
        for(i = 1; i < argc; i++) {
            tag = JS_VALUE_GET_TAG(argv[i]);
            if (tag != JS_TAG_INT) {
                r = r1;
                goto generic_case;
            }
            a1 = JS_VALUE_GET_INT(argv[i]);
            if (is_max)
                r1 = max_int(r1, a1);
            else
                r1 = min_int(r1, a1);

        }
        return JS_NewInt32(ctx, r1);
    } else {
        if (JS_ToFloat64(ctx, &r, argv[0]))
            return JS_EXCEPTION;
        i = 1;
    generic_case:
        while (i < argc) {
            if (JS_ToFloat64(ctx, &a, argv[i]))
                return JS_EXCEPTION;
            if (!isnan(r)) {
                if (isnan(a)) {
                    r = a;
                } else {
                    if (is_max)
                        r = js_fmax(r, a);
                    else
                        r = js_fmin(r, a);
                }
            }
            i++;
        }
        return JS_NewFloat64(ctx, r);
    }
}

static double js_math_sign(double a)
{
    if (isnan(a) || a == 0.0)
        return a;
    if (a < 0)
        return -1;
    else
        return 1;
}

static double js_math_round(double a)
{
    JSFloat64Union u;
    uint64_t frac_mask, one;
    unsigned int e, s;

    u.d = a;
    e = (u.u64 >> 52) & 0x7ff;
    if (e < 1023) {
        /* abs(a) < 1 */
        if (e == (1023 - 1) && u.u64 != 0xbfe0000000000000) {
            /* abs(a) > 0.5 or a = 0.5: return +/-1.0 */
            u.u64 = (u.u64 & ((uint64_t)1 << 63)) | ((uint64_t)1023 << 52);
        } else {
            /* return +/-0.0 */
            u.u64 &= (uint64_t)1 << 63;
        }
    } else if (e < (1023 + 52)) {
        s = u.u64 >> 63;
        one = (uint64_t)1 << (52 - (e - 1023));
        frac_mask = one - 1;
        u.u64 += (one >> 1) - s;
        u.u64 &= ~frac_mask; /* truncate to an integer */
    }
    /* otherwise: abs(a) >= 2^52, or NaN, +/-Infinity: no change */
    return u.d;
}

static JSValue js_math_hypot(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    double r, a;
    int i;

    r = 0;
    if (argc > 0) {
        if (JS_ToFloat64(ctx, &r, argv[0]))
            return JS_EXCEPTION;
        if (argc == 1) {
            r = fabs(r);
        } else {
            /* use the built-in function to minimize precision loss */
            for (i = 1; i < argc; i++) {
                if (JS_ToFloat64(ctx, &a, argv[i]))
                    return JS_EXCEPTION;
                r = hypot(r, a);
            }
        }
    }
    return JS_NewFloat64(ctx, r);
}

static double js_math_f16round(double a)
{
    return fromfp16(tofp16(a));
}

static double js_math_fround(double a)
{
    return (float)a;
}

static JSValue js_math_imul(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    uint32_t a, b, c;
    int32_t d;

    if (JS_ToUint32(ctx, &a, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &b, argv[1]))
        return JS_EXCEPTION;
    c = a * b;
    memcpy(&d, &c, sizeof(d));
    return JS_NewInt32(ctx, d);
}

static JSValue js_math_clz32(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    uint32_t a, r;

    if (JS_ToUint32(ctx, &a, argv[0]))
        return JS_EXCEPTION;
    if (a == 0)
        r = 32;
    else
        r = clz32(a);
    return JS_NewInt32(ctx, r);
}

typedef enum {
    SUM_PRECISE_STATE_FINITE,
    SUM_PRECISE_STATE_INFINITY,
    SUM_PRECISE_STATE_MINUS_INFINITY, /* must be after SUM_PRECISE_STATE_INFINITY */
    SUM_PRECISE_STATE_NAN, /* must be after SUM_PRECISE_STATE_MINUS_INFINITY */
} SumPreciseStateEnum;

#define SP_LIMB_BITS 56
#define SP_RND_BITS (SP_LIMB_BITS - 53)
/* we add one extra limb to avoid having to test for overflows during the sum */
#define SUM_PRECISE_ACC_LEN 39

#define SUM_PRECISE_COUNTER_INIT 250

typedef struct {
    SumPreciseStateEnum state;
    uint32_t counter;
    int n_limbs; /* 'acc' contains n_limbs and is not necessarily
                    acc[n_limb - 1] may be 0. 0 indicates minus zero
                    result when state = SUM_PRECISE_STATE_FINITE */
    int64_t acc[SUM_PRECISE_ACC_LEN];
} SumPreciseState;

static void sum_precise_init(SumPreciseState *s)
{
    memset(s->acc, 0, sizeof(s->acc));
    s->state = SUM_PRECISE_STATE_FINITE;
    s->counter = SUM_PRECISE_COUNTER_INIT;
    s->n_limbs = 0;
}

static void sum_precise_renorm(SumPreciseState *s)
{
    int64_t v, carry;
    int i;
    
    carry = 0;
    for(i = 0; i < s->n_limbs; i++) {
        v = s->acc[i] + carry;
        s->acc[i] = v & (((uint64_t)1 << SP_LIMB_BITS) - 1);
        carry = v >> SP_LIMB_BITS;
    }
    /* we add a failsafe but it should be never reached in a
       reasonnable amount of time */
    if (carry != 0 && s->n_limbs < SUM_PRECISE_ACC_LEN)
        s->acc[s->n_limbs++] = carry;
}

static void sum_precise_add(SumPreciseState *s, double d)
{
    uint64_t a, m, a0, a1;
    int sgn, e, p;
    unsigned int shift;
    
    a = float64_as_uint64(d);
    sgn = a >> 63;
    e = (a >> 52) & ((1 << 11) - 1);
    m = a & (((uint64_t)1 << 52) - 1);
    if (unlikely(e == 2047)) {
        if (m == 0) {
            /* +/- infinity */
            if (s->state == SUM_PRECISE_STATE_NAN ||
                (s->state == SUM_PRECISE_STATE_MINUS_INFINITY && !sgn) ||
                (s->state == SUM_PRECISE_STATE_INFINITY && sgn)) {
                s->state = SUM_PRECISE_STATE_NAN;
            } else {
                s->state = SUM_PRECISE_STATE_INFINITY + sgn;
            }
        } else {
            /* NaN */
            s->state = SUM_PRECISE_STATE_NAN;
        }
    } else if (e == 0) {
        if (likely(m == 0)) {
            /* zero */
            if (s->n_limbs == 0 && !sgn)
                s->n_limbs = 1;
        } else {
            /* subnormal */
            p = 0;
            shift = 0;
            goto add;
        }
    } else {
        /* Note: we sum even if state != SUM_PRECISE_STATE_FINITE to
           avoid tests */
        m |= (uint64_t)1 << 52;
        shift = e - 1;
        /* 'p' is the position of a0 in acc. The division is normally
           implementation as a multiplication by the compiler. */
        p = shift / SP_LIMB_BITS;
        shift %= SP_LIMB_BITS;
    add:
        a0 = (m << shift) & (((uint64_t)1 << SP_LIMB_BITS) - 1);
        a1 = m >> (SP_LIMB_BITS - shift);
        if (!sgn) {
            s->acc[p] += a0;
            s->acc[p + 1] += a1;
        } else {
            s->acc[p] -= a0;
            s->acc[p + 1] -= a1;
        }
        s->n_limbs = max_int(s->n_limbs, p + 2);

        if (unlikely(--s->counter == 0)) {
            s->counter = SUM_PRECISE_COUNTER_INIT;
            sum_precise_renorm(s);
        }
    }
}

static double sum_precise_get_result(SumPreciseState *s)
{
    int n, shift, e, p, is_neg;
    uint64_t m, addend;
        
    if (s->state != SUM_PRECISE_STATE_FINITE) {
        switch(s->state) {
        default:
        case SUM_PRECISE_STATE_INFINITY:
            return INFINITY;
        case SUM_PRECISE_STATE_MINUS_INFINITY:
            return -INFINITY;
        case SUM_PRECISE_STATE_NAN:
            return NAN;
        }
    }

    sum_precise_renorm(s);

    /* extract the sign and absolute value */
#if 0
    {
        int i;
        printf("len=%d:", s->n_limbs);
        for(i = s->n_limbs - 1; i >= 0; i--)
            printf(" %014lx", s->acc[i]);
        printf("\n");
    }
#endif
    n = s->n_limbs;
    /* minus zero result */
    if (n == 0)
        return -0.0;
    
    /* normalize */
    while (n > 0 && s->acc[n - 1] == 0)
        n--;
    /* zero result. The spec tells it is always positive in the finite case */
    if (n == 0)
        return 0.0;
    is_neg = (s->acc[n - 1] < 0);
    if (is_neg) {
        uint64_t v, carry;
        int i;
        /* negate */
        /* XXX: do it only when needed */
        carry = 1;
        for(i = 0; i < n - 1; i++) {
            v = (((uint64_t)1 << SP_LIMB_BITS) - 1) - s->acc[i] + carry;
            carry = v >> SP_LIMB_BITS;
            s->acc[i] = v & (((uint64_t)1 << SP_LIMB_BITS) - 1);
        }
        s->acc[n - 1] = -s->acc[n - 1] + carry - 1;
        while (n > 1 && s->acc[n - 1] == 0)
            n--;
    }
    /* subnormal case */
    if (n == 1 && s->acc[0] < ((uint64_t)1 << 52))
        return uint64_as_float64(((uint64_t)is_neg << 63) | s->acc[0]); 
    /* normal case */
    e = n * SP_LIMB_BITS;
    p = n - 1;
    m = s->acc[p];
    shift = clz64(m) - (64 - SP_LIMB_BITS);
    e = e - shift - 52;
    if (shift != 0) {
        m <<= shift;
        if (p > 0) {
            int shift1;
            uint64_t nz;
            p--;
            shift1 = SP_LIMB_BITS - shift;
            nz = s->acc[p] & (((uint64_t)1 << shift1) - 1);
            m = m | (s->acc[p] >> shift1) | (nz != 0);
        }
    }
    if ((m & ((1 << SP_RND_BITS) - 1)) == (1 << (SP_RND_BITS - 1))) {
        /* see if the LSB part is non zero for the final rounding  */
        while (p > 0) {
            p--;
            if (s->acc[p] != 0) {
                m |= 1;
                break;
            }
        }
    }
    /* rounding to nearest with ties to even */
    addend = (1 << (SP_RND_BITS - 1)) - 1 + ((m >> SP_RND_BITS) & 1);
    m = (m + addend) >> SP_RND_BITS;
    /* handle overflow in the rounding */
    if (m == ((uint64_t)1 << 53))
        e++;
    if (unlikely(e >= 2047)) {
        /* infinity */
        return uint64_as_float64(((uint64_t)is_neg << 63) | ((uint64_t)2047 << 52));
    } else {
        m &= (((uint64_t)1 << 52) - 1);
        return uint64_as_float64(((uint64_t)is_neg << 63) | ((uint64_t)e << 52) | m);
    }
}

static JSValue js_math_sumPrecise(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue iter, next, item, ret;
    uint32_t tag;
    int done;
    double d;
    SumPreciseState s_s, *s = &s_s;

    iter = JS_GetIterator(ctx, argv[0], FALSE);
    if (JS_IsException(iter))
        return JS_EXCEPTION;
    ret = JS_EXCEPTION;
    next = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next))
        goto fail;
    sum_precise_init(s);
    for (;;) {
        item = JS_IteratorNext(ctx, iter, next, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done)
            break;
        tag = JS_VALUE_GET_TAG(item);
        if (JS_TAG_IS_FLOAT64(tag)) {
            d = JS_VALUE_GET_FLOAT64(item);
        } else if (tag == JS_TAG_INT) {
            d = JS_VALUE_GET_INT(item);
        } else {
            JS_FreeValue(ctx, item);
            JS_ThrowTypeError(ctx, "not a number");
            JS_IteratorClose(ctx, iter, TRUE);
            goto fail;
        }
        sum_precise_add(s, d);
    }
    ret = __JS_NewFloat64(ctx, sum_precise_get_result(s));
fail:
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, next);
    return ret;
}

/* xorshift* random number generator by Marsaglia */
static uint64_t xorshift64star(uint64_t *pstate)
{
    uint64_t x;
    x = *pstate;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *pstate = x;
    return x * 0x2545F4914F6CDD1D;
}

static void js_random_init(JSContext *ctx)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ctx->random_state = ((int64_t)tv.tv_sec * 1000000) + tv.tv_usec;
    /* the state must be non zero */
    if (ctx->random_state == 0)
        ctx->random_state = 1;
}

static JSValue js_math_random(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSFloat64Union u;
    uint64_t v;

    v = xorshift64star(&ctx->random_state);
    /* 1.0 <= u.d < 2 */
    u.u64 = ((uint64_t)0x3ff << 52) | (v >> 12);
    return __JS_NewFloat64(ctx, u.d - 1.0);
}

static const JSCFunctionListEntry js_math_funcs[] = {
    JS_CFUNC_MAGIC_DEF("min", 2, js_math_min_max, 0 ),
    JS_CFUNC_MAGIC_DEF("max", 2, js_math_min_max, 1 ),
    JS_CFUNC_SPECIAL_DEF("abs", 1, f_f, fabs ),
    JS_CFUNC_SPECIAL_DEF("floor", 1, f_f, floor ),
    JS_CFUNC_SPECIAL_DEF("ceil", 1, f_f, ceil ),
    JS_CFUNC_SPECIAL_DEF("round", 1, f_f, js_math_round ),
    JS_CFUNC_SPECIAL_DEF("sqrt", 1, f_f, sqrt ),

    JS_CFUNC_SPECIAL_DEF("acos", 1, f_f, acos ),
    JS_CFUNC_SPECIAL_DEF("asin", 1, f_f, asin ),
    JS_CFUNC_SPECIAL_DEF("atan", 1, f_f, atan ),
    JS_CFUNC_SPECIAL_DEF("atan2", 2, f_f_f, atan2 ),
    JS_CFUNC_SPECIAL_DEF("cos", 1, f_f, cos ),
    JS_CFUNC_SPECIAL_DEF("exp", 1, f_f, exp ),
    JS_CFUNC_SPECIAL_DEF("log", 1, f_f, log ),
    JS_CFUNC_SPECIAL_DEF("pow", 2, f_f_f, js_pow ),
    JS_CFUNC_SPECIAL_DEF("sin", 1, f_f, sin ),
    JS_CFUNC_SPECIAL_DEF("tan", 1, f_f, tan ),
    /* ES6 */
    JS_CFUNC_SPECIAL_DEF("trunc", 1, f_f, trunc ),
    JS_CFUNC_SPECIAL_DEF("sign", 1, f_f, js_math_sign ),
    JS_CFUNC_SPECIAL_DEF("cosh", 1, f_f, cosh ),
    JS_CFUNC_SPECIAL_DEF("sinh", 1, f_f, sinh ),
    JS_CFUNC_SPECIAL_DEF("tanh", 1, f_f, tanh ),
    JS_CFUNC_SPECIAL_DEF("acosh", 1, f_f, acosh ),
    JS_CFUNC_SPECIAL_DEF("asinh", 1, f_f, asinh ),
    JS_CFUNC_SPECIAL_DEF("atanh", 1, f_f, atanh ),
    JS_CFUNC_SPECIAL_DEF("expm1", 1, f_f, expm1 ),
    JS_CFUNC_SPECIAL_DEF("log1p", 1, f_f, log1p ),
    JS_CFUNC_SPECIAL_DEF("log2", 1, f_f, log2 ),
    JS_CFUNC_SPECIAL_DEF("log10", 1, f_f, log10 ),
    JS_CFUNC_SPECIAL_DEF("cbrt", 1, f_f, cbrt ),
    JS_CFUNC_DEF("hypot", 2, js_math_hypot ),
    JS_CFUNC_DEF("random", 0, js_math_random ),
    JS_CFUNC_SPECIAL_DEF("f16round", 1, f_f, js_math_f16round ),
    JS_CFUNC_SPECIAL_DEF("fround", 1, f_f, js_math_fround ),
    JS_CFUNC_DEF("imul", 2, js_math_imul ),
    JS_CFUNC_DEF("clz32", 1, js_math_clz32 ),
    JS_CFUNC_DEF("sumPrecise", 1, js_math_sumPrecise ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Math", JS_PROP_CONFIGURABLE ),
    JS_PROP_DOUBLE_DEF("E", 2.718281828459045, 0 ),
    JS_PROP_DOUBLE_DEF("LN10", 2.302585092994046, 0 ),
    JS_PROP_DOUBLE_DEF("LN2", 0.6931471805599453, 0 ),
    JS_PROP_DOUBLE_DEF("LOG2E", 1.4426950408889634, 0 ),
    JS_PROP_DOUBLE_DEF("LOG10E", 0.4342944819032518, 0 ),
    JS_PROP_DOUBLE_DEF("PI", 3.141592653589793, 0 ),
    JS_PROP_DOUBLE_DEF("SQRT1_2", 0.7071067811865476, 0 ),
    JS_PROP_DOUBLE_DEF("SQRT2", 1.4142135623730951, 0 ),
};

static const JSCFunctionListEntry js_math_obj[] = {
    JS_OBJECT_DEF("Math", js_math_funcs, countof(js_math_funcs), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

/* Date */

/* OS dependent. d = argv[0] is in ms from 1970. Return the difference
   between UTC time and local time 'd' in minutes */
static int getTimezoneOffset(int64_t time)
{
    time_t ti;
    int res;

    time /= 1000; /* convert to seconds */
    if (sizeof(time_t) == 4) {
        /* on 32-bit systems, we need to clamp the time value to the
           range of `time_t`. This is better than truncating values to
           32 bits and hopefully provides the same result as 64-bit
           implementation of localtime_r.
         */
        if ((time_t)-1 < 0) {
            if (time < INT32_MIN) {
                time = INT32_MIN;
            } else if (time > INT32_MAX) {
                time = INT32_MAX;
            }
        } else {
            if (time < 0) {
                time = 0;
            } else if (time > UINT32_MAX) {
                time = UINT32_MAX;
            }
        }
    }
    ti = time;
#if defined(_WIN32)
    {
        struct tm *tm;
        time_t gm_ti, loc_ti;

        tm = gmtime(&ti);
        if (!tm)
            return 0;
        gm_ti = mktime(tm);

        tm = localtime(&ti);
        if (!tm)
            return 0;
        loc_ti = mktime(tm);

        res = (gm_ti - loc_ti) / 60;
    }
#else
    {
        struct tm tm;
        localtime_r(&ti, &tm);
        res = -tm.tm_gmtoff / 60;
    }
#endif
    return res;
}

#if 0
static JSValue js___date_getTimezoneOffset(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    double dd;

    if (JS_ToFloat64(ctx, &dd, argv[0]))
        return JS_EXCEPTION;
    if (isnan(dd))
        return __JS_NewFloat64(ctx, dd);
    else
        return JS_NewInt32(ctx, getTimezoneOffset((int64_t)dd));
}

static JSValue js_get_prototype_from_ctor(JSContext *ctx, JSValueConst ctor,
                                          JSValueConst def_proto)
{
    JSValue proto;
    proto = JS_GetProperty(ctx, ctor, JS_ATOM_prototype);
    if (JS_IsException(proto))
        return proto;
    if (!JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        proto = JS_DupValue(ctx, def_proto);
    }
    return proto;
}

/* create a new date object */
static JSValue js___date_create(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue obj, proto;
    proto = js_get_prototype_from_ctor(ctx, argv[0], argv[1]);
    if (JS_IsException(proto))
        return proto;
    obj = JS_NewObjectProtoClass(ctx, proto, JS_CLASS_DATE);
    JS_FreeValue(ctx, proto);
    if (!JS_IsException(obj))
        JS_SetObjectData(ctx, obj, JS_DupValue(ctx, argv[2]));
    return obj;
}
#endif

