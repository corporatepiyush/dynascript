/* DisposableStack / AsyncDisposableStack (ECMAScript Explicit Resource
   Management). The sync DisposableStack is implemented here; the async
   variant reuses the shared state layout and helpers below. */

typedef struct JSDisposableResource {
    JSValue value;   /* [[ResourceValue]] */
    JSValue method;  /* [[DisposeMethod]] (always callable) */
    BOOL async;      /* [[Hint]] is async-dispose */
} JSDisposableResource;

typedef struct JSDisposableStackData {
    BOOL disposed;   /* [[DisposableState]]: FALSE = pending, TRUE = disposed */
    int count;
    int size;
    JSDisposableResource *resources; /* [[DisposeCapability]] stack (append order) */
} JSDisposableStackData;

static void js_disposable_stack_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSDisposableStackData *s = p->u.opaque;
    int i;

    if (s) {
        for (i = 0; i < s->count; i++) {
            JS_FreeValueRT(rt, s->resources[i].value);
            JS_FreeValueRT(rt, s->resources[i].method);
        }
        js_free_rt(rt, s->resources);
        js_free_rt(rt, s);
    }
}

static void js_disposable_stack_mark(JSRuntime *rt, JSValueConst val,
                                     JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSDisposableStackData *s = p->u.opaque;
    int i;

    if (s) {
        for (i = 0; i < s->count; i++) {
            JS_MarkValue(rt, s->resources[i].value, mark_func);
            JS_MarkValue(rt, s->resources[i].method, mark_func);
        }
    }
}

/* Create a SuppressedError wrapping (error, suppressed). Consumes both
   argument references. */
static JSValue js_new_suppressed_error(JSContext *ctx, JSValue error,
                                       JSValue suppressed)
{
    JSValue obj;

    obj = JS_NewObjectProtoClass(ctx, ctx->native_error_proto[JS_SUPPRESSED_ERROR],
                                 JS_CLASS_ERROR);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, suppressed);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_error, error,
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_suppressed, suppressed,
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return obj;
}

/* GetDisposeMethod(V, sync-dispose): returns JS_UNDEFINED if the property is
   null/undefined, throws if it exists but is not callable. */
static JSValue js_get_dispose_method(JSContext *ctx, JSValueConst obj,
                                     JSAtom atom)
{
    JSValue method = JS_GetProperty(ctx, obj, atom);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    if (JS_IsUndefined(method) || JS_IsNull(method)) {
        JS_FreeValue(ctx, method);
        return JS_UNDEFINED;
    }
    if (!JS_IsFunction(ctx, method)) {
        JS_FreeValue(ctx, method);
        return JS_ThrowTypeError(ctx, "dispose method is not a function");
    }
    return method;
}

/* Append a resource to the capability stack. Consumes value and method. */
static int js_disposable_stack_push(JSContext *ctx, JSDisposableStackData *s,
                                    JSValue value, JSValue method, BOOL async)
{
    if (s->count >= s->size) {
        int new_size = s->size ? s->size * 2 : 4;
        JSDisposableResource *nr = js_realloc(ctx, s->resources,
                                              sizeof(*nr) * new_size);
        if (!nr) {
            JS_FreeValue(ctx, value);
            JS_FreeValue(ctx, method);
            return -1;
        }
        s->resources = nr;
        s->size = new_size;
    }
    s->resources[s->count].value = value;
    s->resources[s->count].method = method;
    s->resources[s->count].async = async;
    s->count++;
    return 0;
}

static JSDisposableStackData *js_disposable_stack_get(JSContext *ctx,
                                                      JSValueConst this_val,
                                                      BOOL async)
{
    JSClassID class_id = async ? JS_CLASS_ASYNC_DISPOSABLE_STACK
                               : JS_CLASS_DISPOSABLE_STACK;
    return JS_GetOpaque2(ctx, this_val, class_id);
}

static JSValue js_disposable_stack_constructor(JSContext *ctx,
                                               JSValueConst new_target,
                                               int argc, JSValueConst *argv,
                                               int magic)
{
    JSValue obj;
    JSDisposableStackData *s;
    JSClassID class_id = magic ? JS_CLASS_ASYNC_DISPOSABLE_STACK
                               : JS_CLASS_DISPOSABLE_STACK;

    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "constructor requires 'new'");
    obj = js_create_from_ctor(ctx, new_target, class_id);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    s = js_mallocz(ctx, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, s);
    return obj;
}

/* AddDisposableResource with an explicit (already callable) method, hint sync
   or async. value is borrowed; method ownership is transferred. */
static int js_disposable_add_with_method(JSContext *ctx,
                                         JSDisposableStackData *s,
                                         JSValueConst value, JSValue method,
                                         BOOL async)
{
    return js_disposable_stack_push(ctx, s, JS_DupValue(ctx, value), method,
                                    async);
}

/* DisposableStack.prototype.use / AsyncDisposableStack.prototype.use */
static JSValue js_disposable_stack_use(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int magic)
{
    BOOL async = magic;
    JSDisposableStackData *s = js_disposable_stack_get(ctx, this_val, async);
    JSValueConst value = argv[0];
    JSValue method;

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_ThrowReferenceError(ctx, "disposable stack already disposed");

    /* AddDisposableResource(cap, value, hint) with no method */
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        if (async) {
            /* async-dispose keeps null/undefined so the value is awaited */
            if (js_disposable_stack_push(ctx, s, JS_DupValue(ctx, value),
                                         JS_UNDEFINED, TRUE))
                return JS_EXCEPTION;
        }
        /* sync-dispose: null/undefined is a no-op */
        return JS_DupValue(ctx, value);
    }
    if (!JS_IsObject(value))
        return JS_ThrowTypeError(ctx, "value is not an object");

    method = JS_UNDEFINED;
    if (async) {
        method = js_get_dispose_method(ctx, value, JS_ATOM_Symbol_asyncDispose);
        if (JS_IsException(method))
            return JS_EXCEPTION;
    }
    if (JS_IsUndefined(method)) {
        JSValue sync_method = js_get_dispose_method(ctx, value,
                                                    JS_ATOM_Symbol_dispose);
        if (JS_IsException(sync_method))
            return JS_EXCEPTION;
        method = sync_method;
    }
    if (JS_IsUndefined(method))
        return JS_ThrowTypeError(ctx, "value is not disposable");
    if (js_disposable_add_with_method(ctx, s, value, method, async))
        return JS_EXCEPTION;
    return JS_DupValue(ctx, value);
}

/* Closure captured by adopt: func_data[0] = onDispose, func_data[1] = value.
   Calls onDispose(value). */
static JSValue js_disposable_adopt_closure(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv,
                                           int magic, JSValue *func_data)
{
    return JS_Call(ctx, func_data[0], JS_UNDEFINED, 1,
                   (JSValueConst *)&func_data[1]);
}

/* DisposableStack.prototype.adopt / AsyncDisposableStack.prototype.adopt */
static JSValue js_disposable_stack_adopt(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic)
{
    BOOL async = magic;
    JSDisposableStackData *s = js_disposable_stack_get(ctx, this_val, async);
    JSValueConst value = argv[0];
    JSValueConst on_dispose = argv[1];
    JSValueConst data[2];
    JSValue f;

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_ThrowReferenceError(ctx, "disposable stack already disposed");
    if (!JS_IsFunction(ctx, on_dispose))
        return JS_ThrowTypeError(ctx, "onDispose is not a function");
    data[0] = on_dispose;
    data[1] = value;
    f = JS_NewCFunctionData(ctx, js_disposable_adopt_closure, 0, 0, 2, data);
    if (JS_IsException(f))
        return JS_EXCEPTION;
    /* AddDisposableResource(cap, undefined, hint, F) */
    if (js_disposable_stack_push(ctx, s, JS_UNDEFINED, f, async))
        return JS_EXCEPTION;
    return JS_DupValue(ctx, value);
}

/* DisposableStack.prototype.defer / AsyncDisposableStack.prototype.defer */
static JSValue js_disposable_stack_defer(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic)
{
    BOOL async = magic;
    JSDisposableStackData *s = js_disposable_stack_get(ctx, this_val, async);
    JSValueConst on_dispose = argv[0];

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_ThrowReferenceError(ctx, "disposable stack already disposed");
    if (!JS_IsFunction(ctx, on_dispose))
        return JS_ThrowTypeError(ctx, "onDispose is not a function");
    if (js_disposable_stack_push(ctx, s, JS_UNDEFINED,
                                 JS_DupValue(ctx, on_dispose), async))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* DisposableStack.prototype.move / AsyncDisposableStack.prototype.move */
static JSValue js_disposable_stack_move(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int magic)
{
    BOOL async = magic;
    JSDisposableStackData *s = js_disposable_stack_get(ctx, this_val, async);
    JSClassID class_id = async ? JS_CLASS_ASYNC_DISPOSABLE_STACK
                               : JS_CLASS_DISPOSABLE_STACK;
    JSDisposableStackData *ns;
    JSValue obj;

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_ThrowReferenceError(ctx, "disposable stack already disposed");
    obj = JS_NewObjectProtoClass(ctx, ctx->class_proto[class_id], class_id);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    ns = js_mallocz(ctx, sizeof(*ns));
    if (!ns) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    /* transfer the capability to the new stack, leave this one disposed */
    ns->resources = s->resources;
    ns->count = s->count;
    ns->size = s->size;
    JS_SetOpaque(obj, ns);
    s->resources = NULL;
    s->count = 0;
    s->size = 0;
    s->disposed = TRUE;
    return obj;
}

/* Synchronous DisposeResources: dispose all resources in reverse order,
   building the SuppressedError chain. Detaches the resource stack first so it
   is reentrancy-safe. Returns JS_UNDEFINED on success, JS_EXCEPTION (with the
   accumulated error thrown) otherwise. */
static JSValue js_dispose_resources_sync(JSContext *ctx,
                                         JSDisposableStackData *s)
{
    JSDisposableResource *res = s->resources;
    int count = s->count;
    JSValue pending = JS_UNDEFINED;
    BOOL has_error = FALSE;
    int i;

    s->resources = NULL;
    s->count = 0;
    s->size = 0;

    for (i = count - 1; i >= 0; i--) {
        JSValue ret = JS_Call(ctx, res[i].method, res[i].value, 0, NULL);
        JS_FreeValue(ctx, res[i].value);
        JS_FreeValue(ctx, res[i].method);
        if (JS_IsException(ret)) {
            JSValue thrown = JS_GetException(ctx);
            if (has_error) {
                pending = js_new_suppressed_error(ctx, thrown, pending);
                if (JS_IsException(pending))
                    pending = JS_GetException(ctx);
            } else {
                pending = thrown;
                has_error = TRUE;
            }
        } else {
            JS_FreeValue(ctx, ret);
        }
    }
    js_free(ctx, res);

    if (has_error)
        return JS_Throw(ctx, pending);
    return JS_UNDEFINED;
}

/* DisposableStack.prototype.dispose */
static JSValue js_disposable_stack_dispose(JSContext *ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    JSDisposableStackData *s = js_disposable_stack_get(ctx, this_val, FALSE);

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_UNDEFINED;
    s->disposed = TRUE;
    return js_dispose_resources_sync(ctx, s);
}

static JSValue js_disposable_stack_get_disposed(JSContext *ctx,
                                                JSValueConst this_val)
{
    JSDisposableStackData *s = js_disposable_stack_get(ctx, this_val, FALSE);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, s->disposed);
}

static const JSCFunctionListEntry js_disposable_stack_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("use", 1, js_disposable_stack_use, 0 ),
    JS_CFUNC_MAGIC_DEF("adopt", 2, js_disposable_stack_adopt, 0 ),
    JS_CFUNC_MAGIC_DEF("defer", 1, js_disposable_stack_defer, 0 ),
    JS_CFUNC_MAGIC_DEF("move", 0, js_disposable_stack_move, 0 ),
    JS_CFUNC_DEF("dispose", 0, js_disposable_stack_dispose ),
    JS_CGETSET_DEF("disposed", js_disposable_stack_get_disposed, NULL ),
    JS_ALIAS_DEF("[Symbol.dispose]", "dispose" ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "DisposableStack", JS_PROP_CONFIGURABLE ),
};

static const JSClassShortDef js_disposable_stack_class_def[] = {
    { JS_ATOM_DisposableStack, js_disposable_stack_finalizer, js_disposable_stack_mark }, /* JS_CLASS_DISPOSABLE_STACK */
    { JS_ATOM_AsyncDisposableStack, js_disposable_stack_finalizer, js_disposable_stack_mark }, /* JS_CLASS_ASYNC_DISPOSABLE_STACK */
};

int JS_AddIntrinsicDisposableStack(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    JSValue obj;
    JSCFunctionType ft;

    if (!JS_IsRegisteredClass(rt, JS_CLASS_DISPOSABLE_STACK)) {
        if (init_class_range(rt, js_disposable_stack_class_def,
                             JS_CLASS_DISPOSABLE_STACK,
                             countof(js_disposable_stack_class_def)))
            return -1;
    }
    ft.constructor_magic = js_disposable_stack_constructor;
    obj = JS_NewCConstructor(ctx, JS_CLASS_DISPOSABLE_STACK, "DisposableStack",
                             ft.generic, 0,
                             JS_CFUNC_constructor_magic, 0,
                             JS_UNDEFINED,
                             NULL, 0,
                             js_disposable_stack_proto_funcs,
                             countof(js_disposable_stack_proto_funcs),
                             0);
    if (JS_IsException(obj))
        return -1;
    JS_FreeValue(ctx, obj);
    return 0;
}
