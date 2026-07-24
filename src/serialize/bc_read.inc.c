JSValue JS_ReadObject(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                       int flags)
{
    BCReaderState ss, *s = &ss;
    JSValue obj;

    ctx->binary_object_count += 1;
    ctx->binary_object_size += buf_len;

    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->buf_start = buf;
    s->buf_end = buf + buf_len;
    s->ptr = buf;
    s->allow_bytecode = ((flags & JS_READ_OBJ_BYTECODE) != 0);
    s->is_rom_data = ((flags & JS_READ_OBJ_ROM_DATA) != 0);
    s->allow_sab = ((flags & JS_READ_OBJ_SAB) != 0);
    s->allow_reference = ((flags & JS_READ_OBJ_REFERENCE) != 0);
    if (s->allow_bytecode)
        s->first_atom = JS_ATOM_END;
    else
        s->first_atom = 1;
    if (JS_ReadObjectAtoms(s)) {
        obj = JS_EXCEPTION;
    } else {
        obj = JS_ReadObjectRec(s);
    }
    bc_reader_free(s);
    return obj;
}

/*******************************************************************/
/* runtime functions & objects */

static JSValue js_string_constructor(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
static JSValue js_boolean_constructor(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv);
static JSValue js_number_constructor(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);

static int check_function(JSContext *ctx, JSValueConst obj)
{
    if (likely(JS_IsFunction(ctx, obj)))
        return 0;
    JS_ThrowTypeError(ctx, "not a function");
    return -1;
}

static int check_exception_free(JSContext *ctx, JSValue obj)
{
    JS_FreeValue(ctx, obj);
    return JS_IsException(obj);
}

static JSAtom find_atom(JSContext *ctx, const char *name)
{
    JSAtom atom;
    int len;

    if (*name == '[') {
        name++;
        len = strlen(name) - 1;
        /* We assume 8 bit non null strings, which is the case for these
           symbols */
        for(atom = JS_ATOM_Symbol_toPrimitive; atom < JS_ATOM_END; atom++) {
            JSAtomStruct *p = ctx->rt->atom_array[atom];
            JSString *str = p;
            if (str->len == len && !memcmp(str->u.str8, name, len))
                return JS_DupAtom(ctx, atom);
        }
        abort();
    } else {
        atom = JS_NewAtom(ctx, name);
    }
    return atom;
}

static JSValue JS_NewObjectProtoList(JSContext *ctx, JSValueConst proto,
                                     const JSCFunctionListEntry *fields, int n_fields)
{
    JSValue obj;
    obj = JS_NewObjectProtoClassAlloc(ctx, proto, JS_CLASS_OBJECT, n_fields);
    if (JS_IsException(obj))
        return obj;
    if (JS_SetPropertyFunctionList(ctx, obj, fields, n_fields)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue JS_InstantiateFunctionListItem2(JSContext *ctx, JSObject *p,
                                               JSAtom atom, void *opaque)
{
    const JSCFunctionListEntry *e = opaque;
    JSValue val, proto;

    switch(e->def_type) {
    case JS_DEF_CFUNC:
        val = JS_NewCFunction2(ctx, e->u.func.cfunc.generic,
                               e->name, e->u.func.length, e->u.func.cproto, e->magic);
        break;
    case JS_DEF_PROP_STRING:
        val = JS_NewAtomString(ctx, e->u.str);
        break;
    case JS_DEF_OBJECT:
        /* XXX: could add a flag */
        if (atom == JS_ATOM_Symbol_unscopables)
            proto = JS_NULL;
        else
            proto = ctx->class_proto[JS_CLASS_OBJECT];
        val = JS_NewObjectProtoList(ctx, proto,
                                    e->u.prop_list.tab, e->u.prop_list.len);
        break;
    default:
        abort();
    }
    return val;
}

static int JS_InstantiateFunctionListItem(JSContext *ctx, JSValueConst obj,
                                          JSAtom atom,
                                          const JSCFunctionListEntry *e)
{
    JSValue val;
    int prop_flags = e->prop_flags;

    switch(e->def_type) {
    case JS_DEF_ALIAS: /* using autoinit for aliases is not safe */
        {
            JSAtom atom1 = find_atom(ctx, e->u.alias.name);
            switch (e->u.alias.base) {
            case -1:
                val = JS_GetProperty(ctx, obj, atom1);
                break;
            case 0:
                val = JS_GetProperty(ctx, ctx->global_obj, atom1);
                break;
            case 1:
                val = JS_GetProperty(ctx, ctx->class_proto[JS_CLASS_ARRAY], atom1);
                break;
            default:
                abort();
            }
            JS_FreeAtom(ctx, atom1);
            if (JS_IsException(val))
                return -1;
            if (atom == JS_ATOM_Symbol_toPrimitive) {
                /* Symbol.toPrimitive functions are not writable */
                prop_flags = JS_PROP_CONFIGURABLE;
            } else if (atom == JS_ATOM_Symbol_hasInstance) {
                /* Function.prototype[Symbol.hasInstance] is not writable nor configurable */
                prop_flags = 0;
            }
        }
        break;
    case JS_DEF_CFUNC:
        if (atom == JS_ATOM_Symbol_toPrimitive) {
            /* Symbol.toPrimitive functions are not writable */
            prop_flags = JS_PROP_CONFIGURABLE;
        } else if (atom == JS_ATOM_Symbol_hasInstance) {
            /* Function.prototype[Symbol.hasInstance] is not writable nor configurable */
            prop_flags = 0;
        }
        if (JS_DefineAutoInitProperty(ctx, obj, atom, JS_AUTOINIT_ID_PROP,
                                      (void *)e, prop_flags) < 0)
            return -1;
        return 0;
    case JS_DEF_CGETSET: /* XXX: use autoinit again ? */
    case JS_DEF_CGETSET_MAGIC:
        {
            JSValue getter, setter;
            char buf[64];

            getter = JS_UNDEFINED;
            if (e->u.getset.get.generic) {
                snprintf(buf, sizeof(buf), "get %s", e->name);
                getter = JS_NewCFunction2(ctx, e->u.getset.get.generic,
                                          buf, 0, e->def_type == JS_DEF_CGETSET_MAGIC ? JS_CFUNC_getter_magic : JS_CFUNC_getter,
                                          e->magic);
                if (JS_IsException(getter))
                    return -1;
            }
            setter = JS_UNDEFINED;
            if (e->u.getset.set.generic) {
                snprintf(buf, sizeof(buf), "set %s", e->name);
                setter = JS_NewCFunction2(ctx, e->u.getset.set.generic,
                                          buf, 1, e->def_type == JS_DEF_CGETSET_MAGIC ? JS_CFUNC_setter_magic : JS_CFUNC_setter,
                                          e->magic);
                if (JS_IsException(setter)) {
                    JS_FreeValue(ctx, getter);
                    return -1;
                }
            }
            if (JS_DefinePropertyGetSet(ctx, obj, atom, getter, setter, prop_flags) < 0)
                return -1;
            return 0;
        }
        break;
    case JS_DEF_PROP_INT32:
        val = JS_NewInt32(ctx, e->u.i32);
        break;
    case JS_DEF_PROP_INT64:
        val = JS_NewInt64(ctx, e->u.i64);
        break;
    case JS_DEF_PROP_DOUBLE:
        val = __JS_NewFloat64(ctx, e->u.f64);
        break;
    case JS_DEF_PROP_UNDEFINED:
        val = JS_UNDEFINED;
        break;
    case JS_DEF_PROP_ATOM:
        val = JS_AtomToValue(ctx, e->u.i32);
        break;
    case JS_DEF_PROP_BOOL:
        val = JS_NewBool(ctx, e->u.i32);
        break;
    case JS_DEF_PROP_STRING:
    case JS_DEF_OBJECT:
        if (JS_DefineAutoInitProperty(ctx, obj, atom, JS_AUTOINIT_ID_PROP,
                                      (void *)e, prop_flags) < 0)
            return -1;
        return 0;
    default:
        abort();
    }
    if (JS_DefinePropertyValue(ctx, obj, atom, val, prop_flags) < 0)
        return -1;
    return 0;
}

int JS_SetPropertyFunctionList(JSContext *ctx, JSValueConst obj,
                               const JSCFunctionListEntry *tab, int len)
{
    int i, ret;

    for (i = 0; i < len; i++) {
        const JSCFunctionListEntry *e = &tab[i];
        JSAtom atom = find_atom(ctx, e->name);
        if (atom == JS_ATOM_NULL)
            return -1;
        ret = JS_InstantiateFunctionListItem(ctx, obj, atom, e);
        JS_FreeAtom(ctx, atom);
        if (ret)
            return -1;
    }
    return 0;
}

int JS_AddModuleExportList(JSContext *ctx, JSModuleDef *m,
                           const JSCFunctionListEntry *tab, int len)
{
    int i;
    for(i = 0; i < len; i++) {
        if (JS_AddModuleExport(ctx, m, tab[i].name))
            return -1;
    }
    return 0;
}

int JS_SetModuleExportList(JSContext *ctx, JSModuleDef *m,
                           const JSCFunctionListEntry *tab, int len)
{
    int i;
    JSValue val;

    for(i = 0; i < len; i++) {
        const JSCFunctionListEntry *e = &tab[i];
        switch(e->def_type) {
        case JS_DEF_CFUNC:
            val = JS_NewCFunction2(ctx, e->u.func.cfunc.generic,
                                   e->name, e->u.func.length, e->u.func.cproto, e->magic);
            break;
        case JS_DEF_PROP_STRING:
            val = JS_NewString(ctx, e->u.str);
            break;
        case JS_DEF_PROP_INT32:
            val = JS_NewInt32(ctx, e->u.i32);
            break;
        case JS_DEF_PROP_INT64:
            val = JS_NewInt64(ctx, e->u.i64);
            break;
        case JS_DEF_PROP_DOUBLE:
            val = __JS_NewFloat64(ctx, e->u.f64);
            break;
        case JS_DEF_OBJECT:
            val = JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_OBJECT],
                                        e->u.prop_list.tab, e->u.prop_list.len);
            break;
        default:
            abort();
        }
        if (JS_SetModuleExport(ctx, m, e->name, val))
            return -1;
    }
    return 0;
}

/* Note: 'func_obj' is not necessarily a constructor */
static int JS_SetConstructor2(JSContext *ctx,
                              JSValueConst func_obj,
                              JSValueConst proto,
                              int proto_flags, int ctor_flags)
{
    if (JS_DefinePropertyValue(ctx, func_obj, JS_ATOM_prototype,
                               JS_DupValue(ctx, proto), proto_flags) < 0)
        return -1;
    if (JS_DefinePropertyValue(ctx, proto, JS_ATOM_constructor,
                               JS_DupValue(ctx, func_obj),
                               ctor_flags) < 0)
        return -1;
    set_cycle_flag(ctx, func_obj);
    set_cycle_flag(ctx, proto);
    return 0;
}

/* return 0 if OK, -1 if exception */
int JS_SetConstructor(JSContext *ctx, JSValueConst func_obj,
                      JSValueConst proto)
{
    return JS_SetConstructor2(ctx, func_obj, proto,
                              0, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

#define JS_NEW_CTOR_NO_GLOBAL   (1 << 0) /* don't create a global binding */
#define JS_NEW_CTOR_PROTO_CLASS (1 << 1) /* the prototype class is 'class_id' instead of JS_CLASS_OBJECT */
#define JS_NEW_CTOR_PROTO_EXIST (1 << 2) /* the prototype is already defined */
#define JS_NEW_CTOR_READONLY    (1 << 3) /* read-only constructor field */

/* Return the constructor and. Define it as a global variable unless
   JS_NEW_CTOR_NO_GLOBAL is set. The new class inherit from
   parent_ctor if it is not JS_UNDEFINED. if class_id is != -1,
   class_proto[class_id] is set. */
static JSValue JS_NewCConstructor(JSContext *ctx, int class_id, const char *name,
                                  JSCFunction *func, int length, JSCFunctionEnum cproto, int magic,
                                  JSValueConst parent_ctor,
                                  const JSCFunctionListEntry *ctor_fields, int n_ctor_fields,
                                  const JSCFunctionListEntry *proto_fields, int n_proto_fields,
                                  int flags)
{
    JSValue ctor = JS_UNDEFINED, proto, parent_proto;
    int proto_class_id, proto_flags, ctor_flags;

    proto_flags = 0;
    if (flags & JS_NEW_CTOR_READONLY) {
        ctor_flags = JS_PROP_CONFIGURABLE;
    } else {
        ctor_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
    }
    
    if (JS_IsUndefined(parent_ctor)) {
        parent_proto = JS_DupValue(ctx, ctx->class_proto[JS_CLASS_OBJECT]);
        parent_ctor = ctx->function_proto;
    } else {
        parent_proto = JS_GetProperty(ctx, parent_ctor, JS_ATOM_prototype);
        if (JS_IsException(parent_proto))
            return JS_EXCEPTION;
    }
    
    if (flags & JS_NEW_CTOR_PROTO_EXIST) {
        proto = JS_DupValue(ctx, ctx->class_proto[class_id]);
    } else {
        if (flags & JS_NEW_CTOR_PROTO_CLASS)
            proto_class_id = class_id;
        else
            proto_class_id = JS_CLASS_OBJECT;
        /* one additional field: constructor */
        proto = JS_NewObjectProtoClassAlloc(ctx, parent_proto, proto_class_id,
                                            n_proto_fields + 1);
        if (JS_IsException(proto))
            goto fail;
        if (class_id >= 0)
            ctx->class_proto[class_id] = JS_DupValue(ctx, proto);
    }
    if (JS_SetPropertyFunctionList(ctx, proto, proto_fields, n_proto_fields))
        goto fail;

    /* additional fields: name, length, prototype */
    ctor = JS_NewCFunction3(ctx, func, name, length, cproto, magic, parent_ctor,
                            n_ctor_fields + 3);
    if (JS_IsException(ctor))
        goto fail;
    if (JS_SetPropertyFunctionList(ctx, ctor, ctor_fields, n_ctor_fields))
        goto fail;
    if (!(flags & JS_NEW_CTOR_NO_GLOBAL)) {
        if (JS_DefinePropertyValueStr(ctx, ctx->global_obj, name,
                                      JS_DupValue(ctx, ctor),
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            goto fail;
    }
    JS_SetConstructor2(ctx, ctor, proto, proto_flags, ctor_flags);

    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, parent_proto);
    return ctor;
 fail:
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, parent_proto);
    JS_FreeValue(ctx, ctor);
    return JS_EXCEPTION;
}

static JSValue js_global_eval(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    return JS_EvalObject(ctx, ctx->global_obj, argv[0], JS_EVAL_TYPE_INDIRECT, -1);
}

static JSValue js_global_isNaN(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double d;

    if (unlikely(JS_ToFloat64(ctx, &d, argv[0])))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isnan(d));
}

static JSValue js_global_isFinite(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    double d;
    if (unlikely(JS_ToFloat64(ctx, &d, argv[0])))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isfinite(d));
}

/* Object class */

static JSValue JS_ToObject(JSContext *ctx, JSValueConst val)
{
    int tag = JS_VALUE_GET_NORM_TAG(val);
    JSValue obj;

    switch(tag) {
    default:
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
        return JS_ThrowTypeError(ctx, "cannot convert to object");
    case JS_TAG_OBJECT:
    case JS_TAG_EXCEPTION:
        return JS_DupValue(ctx, val);
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        obj = JS_NewObjectClass(ctx, JS_CLASS_BIG_INT);
        goto set_value;
    case JS_TAG_INT:
    case JS_TAG_FLOAT64:
        obj = JS_NewObjectClass(ctx, JS_CLASS_NUMBER);
        goto set_value;
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        /* XXX: should call the string constructor */
        {
            JSValue str;
            str = JS_ToString(ctx, val); /* ensure that we never store a rope */
            if (JS_IsException(str))
                return JS_EXCEPTION;
            obj = JS_NewObjectClass(ctx, JS_CLASS_STRING);
            if (!JS_IsException(obj)) {
                JS_DefinePropertyValue(ctx, obj, JS_ATOM_length,
                                       JS_NewInt32(ctx, JS_VALUE_GET_STRING(str)->len), 0);
                JS_SetObjectData(ctx, obj, JS_DupValue(ctx, str));
            }
            JS_FreeValue(ctx, str);
            return obj;
        }
    case JS_TAG_BOOL:
        obj = JS_NewObjectClass(ctx, JS_CLASS_BOOLEAN);
        goto set_value;
    case JS_TAG_SYMBOL:
        obj = JS_NewObjectClass(ctx, JS_CLASS_SYMBOL);
    set_value:
        if (!JS_IsException(obj))
            JS_SetObjectData(ctx, obj, JS_DupValue(ctx, val));
        return obj;
    }
}

static JSValue JS_ToObjectFree(JSContext *ctx, JSValue val)
{
    JSValue obj = JS_ToObject(ctx, val);
    JS_FreeValue(ctx, val);
    return obj;
}

static int js_obj_to_desc(JSContext *ctx, JSPropertyDescriptor *d,
                          JSValueConst desc)
{
    JSValue val, getter, setter;
    int flags;

    if (!JS_IsObject(desc)) {
        JS_ThrowTypeErrorNotAnObject(ctx);
        return -1;
    }
    flags = 0;
    val = JS_UNDEFINED;
    getter = JS_UNDEFINED;
    setter = JS_UNDEFINED;
    if (JS_HasProperty(ctx, desc, JS_ATOM_enumerable)) {
        JSValue prop = JS_GetProperty(ctx, desc, JS_ATOM_enumerable);
        if (JS_IsException(prop))
            goto fail;
        flags |= JS_PROP_HAS_ENUMERABLE;
        if (JS_ToBoolFree(ctx, prop))
            flags |= JS_PROP_ENUMERABLE;
    }
    if (JS_HasProperty(ctx, desc, JS_ATOM_configurable)) {
        JSValue prop = JS_GetProperty(ctx, desc, JS_ATOM_configurable);
        if (JS_IsException(prop))
            goto fail;
        flags |= JS_PROP_HAS_CONFIGURABLE;
        if (JS_ToBoolFree(ctx, prop))
            flags |= JS_PROP_CONFIGURABLE;
    }
    if (JS_HasProperty(ctx, desc, JS_ATOM_value)) {
        flags |= JS_PROP_HAS_VALUE;
        val = JS_GetProperty(ctx, desc, JS_ATOM_value);
        if (JS_IsException(val))
            goto fail;
    }
    if (JS_HasProperty(ctx, desc, JS_ATOM_writable)) {
        JSValue prop = JS_GetProperty(ctx, desc, JS_ATOM_writable);
        if (JS_IsException(prop))
            goto fail;
        flags |= JS_PROP_HAS_WRITABLE;
        if (JS_ToBoolFree(ctx, prop))
            flags |= JS_PROP_WRITABLE;
    }
    if (JS_HasProperty(ctx, desc, JS_ATOM_get)) {
        flags |= JS_PROP_HAS_GET;
        getter = JS_GetProperty(ctx, desc, JS_ATOM_get);
        if (JS_IsException(getter) ||
            !(JS_IsUndefined(getter) || JS_IsFunction(ctx, getter))) {
            JS_ThrowTypeError(ctx, "invalid getter");
            goto fail;
        }
    }
    if (JS_HasProperty(ctx, desc, JS_ATOM_set)) {
        flags |= JS_PROP_HAS_SET;
        setter = JS_GetProperty(ctx, desc, JS_ATOM_set);
        if (JS_IsException(setter) ||
            !(JS_IsUndefined(setter) || JS_IsFunction(ctx, setter))) {
            JS_ThrowTypeError(ctx, "invalid setter");
            goto fail;
        }
    }
    if ((flags & (JS_PROP_HAS_SET | JS_PROP_HAS_GET)) &&
        (flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE))) {
        JS_ThrowTypeError(ctx, "cannot have setter/getter and value or writable");
        goto fail;
    }
    d->flags = flags;
    d->value = val;
    d->getter = getter;
    d->setter = setter;
    return 0;
 fail:
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, getter);
    JS_FreeValue(ctx, setter);
    return -1;
}

static __exception int JS_DefinePropertyDesc(JSContext *ctx, JSValueConst obj,
                                             JSAtom prop, JSValueConst desc,
                                             int flags)
{
    JSPropertyDescriptor d;
    int ret;

    if (js_obj_to_desc(ctx, &d, desc) < 0)
        return -1;

    ret = JS_DefineProperty(ctx, obj, prop,
                            d.value, d.getter, d.setter, d.flags | flags);
    js_free_desc(ctx, &d);
    return ret;
}

static __exception int JS_ObjectDefineProperties(JSContext *ctx,
                                                 JSValueConst obj,
                                                 JSValueConst properties)
{
    JSValue props, desc;
    JSObject *p;
    JSPropertyEnum *atoms;
    uint32_t len, i;
    int ret = -1;

    if (!JS_IsObject(obj)) {
        JS_ThrowTypeErrorNotAnObject(ctx);
        return -1;
    }
    desc = JS_UNDEFINED;
    props = JS_ToObject(ctx, properties);
    if (JS_IsException(props))
        return -1;
    p = JS_VALUE_GET_OBJ(props);
    /* XXX: not done in the same order as the spec */
    if (JS_GetOwnPropertyNamesInternal(ctx, &atoms, &len, p, JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        goto exception;
    for(i = 0; i < len; i++) {
        JS_FreeValue(ctx, desc);
        desc = JS_GetProperty(ctx, props, atoms[i].atom);
        if (JS_IsException(desc))
            goto exception;
        if (JS_DefinePropertyDesc(ctx, obj, atoms[i].atom, desc, JS_PROP_THROW) < 0)
            goto exception;
    }
    ret = 0;

exception:
    JS_FreePropertyEnum(ctx, atoms, len);
    JS_FreeValue(ctx, props);
    JS_FreeValue(ctx, desc);
    return ret;
}

static JSValue js_object_constructor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue ret;
    if (!JS_IsUndefined(new_target) &&
        JS_VALUE_GET_OBJ(new_target) !=
        JS_VALUE_GET_OBJ(JS_GetActiveFunction(ctx))) {
        ret = js_create_from_ctor(ctx, new_target, JS_CLASS_OBJECT);
    } else {
        int tag = JS_VALUE_GET_NORM_TAG(argv[0]);
        switch(tag) {
        case JS_TAG_NULL:
        case JS_TAG_UNDEFINED:
            ret = JS_NewObject(ctx);
            break;
        default:
            ret = JS_ToObject(ctx, argv[0]);
            break;
        }
    }
    return ret;
}

static JSValue js_object_create(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValueConst proto, props;
    JSValue obj;

    proto = argv[0];
    if (!JS_IsObject(proto) && !JS_IsNull(proto))
        return JS_ThrowTypeError(ctx, "not a prototype");
    obj = JS_NewObjectProto(ctx, proto);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    props = argv[1];
    if (!JS_IsUndefined(props)) {
        if (JS_ObjectDefineProperties(ctx, obj, props)) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    return obj;
}

static JSValue js_object_getPrototypeOf(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int magic)
{
    JSValueConst val;

    val = argv[0];
    if (JS_VALUE_GET_TAG(val) != JS_TAG_OBJECT) {
        /* ES6 feature non compatible with ES5.1: primitive types are
           accepted */
        if (magic || JS_VALUE_GET_TAG(val) == JS_TAG_NULL ||
            JS_VALUE_GET_TAG(val) == JS_TAG_UNDEFINED)
            return JS_ThrowTypeErrorNotAnObject(ctx);
    }
    return JS_GetPrototype(ctx, val);
}

static JSValue js_object_setPrototypeOf(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSValueConst obj;
    obj = argv[0];
    if (JS_SetPrototypeInternal(ctx, obj, argv[1], TRUE) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, obj);
}

/* magic = 1 if called as Reflect.defineProperty */
static JSValue js_object_defineProperty(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int magic)
{
    JSValueConst obj, prop, desc;
    int ret, flags;
    JSAtom atom;

    obj = argv[0];
    prop = argv[1];
    desc = argv[2];

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);
    atom = JS_ValueToAtom(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL))
        return JS_EXCEPTION;
    flags = 0;
    if (!magic)
        flags |= JS_PROP_THROW;
    ret = JS_DefinePropertyDesc(ctx, obj, atom, desc, flags);
    JS_FreeAtom(ctx, atom);
    if (ret < 0) {
        return JS_EXCEPTION;
    } else if (magic) {
        return JS_NewBool(ctx, ret);
    } else {
        return JS_DupValue(ctx, obj);
    }
}

static JSValue js_object_defineProperties(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    // defineProperties(obj, properties)
    JSValueConst obj = argv[0];

    if (JS_ObjectDefineProperties(ctx, obj, argv[1]))
        return JS_EXCEPTION;
    else
        return JS_DupValue(ctx, obj);
}

/* magic = 1 if called as __defineSetter__ */
static JSValue js_object___defineGetter__(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv, int magic)
{
    JSValue obj;
    JSValueConst prop, value, get, set;
    int ret, flags;
    JSAtom atom;

    prop = argv[0];
    value = argv[1];

    obj = JS_ToObject(ctx, this_val);
    if (JS_IsException(obj))
        return JS_EXCEPTION;

    if (check_function(ctx, value)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    atom = JS_ValueToAtom(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    flags = JS_PROP_THROW |
        JS_PROP_HAS_ENUMERABLE | JS_PROP_ENUMERABLE |
        JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE;
    if (magic) {
        get = JS_UNDEFINED;
        set = value;
        flags |= JS_PROP_HAS_SET;
    } else {
        get = value;
        set = JS_UNDEFINED;
        flags |= JS_PROP_HAS_GET;
    }
    ret = JS_DefineProperty(ctx, obj, atom, JS_UNDEFINED, get, set, flags);
    JS_FreeValue(ctx, obj);
    JS_FreeAtom(ctx, atom);
    if (ret < 0) {
        return JS_EXCEPTION;
    } else {
        return JS_UNDEFINED;
    }
}

static JSValue js_object_getOwnPropertyDescriptor(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv, int magic)
{
    JSValueConst prop;
    JSAtom atom;
    JSValue ret, obj;
    JSPropertyDescriptor desc;
    int res, flags;

    if (magic) {
        /* Reflect.getOwnPropertyDescriptor case */
        if (JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT)
            return JS_ThrowTypeErrorNotAnObject(ctx);
        obj = JS_DupValue(ctx, argv[0]);
    } else {
        obj = JS_ToObject(ctx, argv[0]);
        if (JS_IsException(obj))
            return obj;
    }
    prop = argv[1];
    atom = JS_ValueToAtom(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL))
        goto exception;
    ret = JS_UNDEFINED;
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        res = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(obj), atom);
        if (res < 0)
            goto exception;
        if (res) {
            ret = JS_NewObject(ctx);
            if (JS_IsException(ret))
                goto exception1;
            flags = JS_PROP_C_W_E | JS_PROP_THROW;
            if (desc.flags & JS_PROP_GETSET) {
                if (JS_DefinePropertyValue(ctx, ret, JS_ATOM_get, JS_DupValue(ctx, desc.getter), flags) < 0
                ||  JS_DefinePropertyValue(ctx, ret, JS_ATOM_set, JS_DupValue(ctx, desc.setter), flags) < 0)
                    goto exception1;
            } else {
                if (JS_DefinePropertyValue(ctx, ret, JS_ATOM_value, JS_DupValue(ctx, desc.value), flags) < 0
                ||  JS_DefinePropertyValue(ctx, ret, JS_ATOM_writable,
                                           JS_NewBool(ctx, desc.flags & JS_PROP_WRITABLE), flags) < 0)
                    goto exception1;
            }
            if (JS_DefinePropertyValue(ctx, ret, JS_ATOM_enumerable,
                                       JS_NewBool(ctx, desc.flags & JS_PROP_ENUMERABLE), flags) < 0
            ||  JS_DefinePropertyValue(ctx, ret, JS_ATOM_configurable,
                                       JS_NewBool(ctx, desc.flags & JS_PROP_CONFIGURABLE), flags) < 0)
                goto exception1;
            js_free_desc(ctx, &desc);
        }
    }
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, obj);
    return ret;

exception1:
    js_free_desc(ctx, &desc);
    JS_FreeValue(ctx, ret);
exception:
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_object_getOwnPropertyDescriptors(JSContext *ctx, JSValueConst this_val,
                                                   int argc, JSValueConst *argv)
{
    //getOwnPropertyDescriptors(obj)
    JSValue obj, r;
    JSObject *p;
    JSPropertyEnum *props;
    uint32_t len, i;

    r = JS_UNDEFINED;
    obj = JS_ToObject(ctx, argv[0]);
    if (JS_IsException(obj))
        return JS_EXCEPTION;

    p = JS_VALUE_GET_OBJ(obj);
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, p,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK))
        goto exception;
    r = JS_NewObject(ctx);
    if (JS_IsException(r))
        goto exception;
    for(i = 0; i < len; i++) {
        JSValue atomValue, desc;
        JSValueConst args[2];

        atomValue = JS_AtomToValue(ctx, props[i].atom);
        if (JS_IsException(atomValue))
            goto exception;
        args[0] = obj;
        args[1] = atomValue;
        desc = js_object_getOwnPropertyDescriptor(ctx, JS_UNDEFINED, 2, args, 0);
        JS_FreeValue(ctx, atomValue);
        if (JS_IsException(desc))
            goto exception;
        if (!JS_IsUndefined(desc)) {
            if (JS_DefinePropertyValue(ctx, r, props[i].atom, desc,
                                       JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                goto exception;
        }
    }
    JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, obj);
    return r;

exception:
    JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, r);
    return JS_EXCEPTION;
}

static JSValue JS_GetOwnPropertyNames2(JSContext *ctx, JSValueConst obj1,
                                       int flags, int kind)
{
    JSValue obj, r, val, key, value;
    JSObject *p;
    JSPropertyEnum *atoms;
    uint32_t len, i, j;

    r = JS_UNDEFINED;
    val = JS_UNDEFINED;
    obj = JS_ToObject(ctx, obj1);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    p = JS_VALUE_GET_OBJ(obj);
    if (JS_GetOwnPropertyNamesInternal(ctx, &atoms, &len, p, flags & ~JS_GPN_ENUM_ONLY))
        goto exception;
    r = JS_NewArray(ctx);
    if (JS_IsException(r))
        goto exception;
    for(j = i = 0; i < len; i++) {
        JSAtom atom = atoms[i].atom;
        if (flags & JS_GPN_ENUM_ONLY) {
            JSPropertyDescriptor desc;
            int res;

            /* Check if property is still enumerable */
            res = JS_GetOwnPropertyInternal(ctx, &desc, p, atom);
            if (res < 0)
                goto exception;
            if (!res)
                continue;
            js_free_desc(ctx, &desc);
            if (!(desc.flags & JS_PROP_ENUMERABLE))
                continue;
        }
        switch(kind) {
        default:
        case JS_ITERATOR_KIND_KEY:
            val = JS_AtomToValue(ctx, atom);
            if (JS_IsException(val))
                goto exception;
            break;
        case JS_ITERATOR_KIND_VALUE:
            val = JS_GetProperty(ctx, obj, atom);
            if (JS_IsException(val))
                goto exception;
            break;
        case JS_ITERATOR_KIND_KEY_AND_VALUE:
            val = JS_NewArray(ctx);
            if (JS_IsException(val))
                goto exception;
            key = JS_AtomToValue(ctx, atom);
            if (JS_IsException(key))
                goto exception1;
            if (JS_CreateDataPropertyUint32(ctx, val, 0, key, JS_PROP_THROW) < 0)
                goto exception1;
            value = JS_GetProperty(ctx, obj, atom);
            if (JS_IsException(value))
                goto exception1;
            if (JS_CreateDataPropertyUint32(ctx, val, 1, value, JS_PROP_THROW) < 0)
                goto exception1;
            break;
        }
        if (JS_CreateDataPropertyUint32(ctx, r, j++, val, 0) < 0)
            goto exception;
    }
    goto done;

exception1:
    JS_FreeValue(ctx, val);
exception:
    JS_FreeValue(ctx, r);
    r = JS_EXCEPTION;
done:
    JS_FreePropertyEnum(ctx, atoms, len);
    JS_FreeValue(ctx, obj);
    return r;
}

static JSValue js_object_getOwnPropertyNames(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    return JS_GetOwnPropertyNames2(ctx, argv[0],
                                   JS_GPN_STRING_MASK, JS_ITERATOR_KIND_KEY);
}

static JSValue js_object_getOwnPropertySymbols(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    return JS_GetOwnPropertyNames2(ctx, argv[0],
                                   JS_GPN_SYMBOL_MASK, JS_ITERATOR_KIND_KEY);
}

static JSValue js_object_keys(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int kind)
{
    return JS_GetOwnPropertyNames2(ctx, argv[0],
                                   JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK, kind);
}

static JSValue js_object_isExtensible(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int reflect)
{
    JSValueConst obj;
    int ret;

    obj = argv[0];
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
        if (reflect)
            return JS_ThrowTypeErrorNotAnObject(ctx);
        else
            return JS_FALSE;
    }
    ret = JS_IsExtensible(ctx, obj);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_object_preventExtensions(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv, int reflect)
{
    JSValueConst obj;
    int ret;

    obj = argv[0];
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
        if (reflect)
            return JS_ThrowTypeErrorNotAnObject(ctx);
        else
            return JS_DupValue(ctx, obj);
    }
    ret = JS_PreventExtensions(ctx, obj);
    if (ret < 0)
        return JS_EXCEPTION;
    if (reflect) {
        return JS_NewBool(ctx, ret);
    } else {
        if (!ret)
            return JS_ThrowTypeError(ctx, "proxy preventExtensions handler returned false");
        return JS_DupValue(ctx, obj);
    }
}

static JSValue js_object_hasOwnProperty(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSValue obj;
    JSAtom atom;
    JSObject *p;
    BOOL ret;

    atom = JS_ValueToAtom(ctx, argv[0]); /* must be done first */
    if (unlikely(atom == JS_ATOM_NULL))
        return JS_EXCEPTION;
    obj = JS_ToObject(ctx, this_val);
    if (JS_IsException(obj)) {
        JS_FreeAtom(ctx, atom);
        return obj;
    }
    p = JS_VALUE_GET_OBJ(obj);
    ret = JS_GetOwnPropertyInternal(ctx, NULL, p, atom);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, obj);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_object_hasOwn(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue obj;
    JSAtom atom;
    JSObject *p;
    BOOL ret;

    obj = JS_ToObject(ctx, argv[0]);
    if (JS_IsException(obj))
        return obj;
    atom = JS_ValueToAtom(ctx, argv[1]);
    if (unlikely(atom == JS_ATOM_NULL)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    p = JS_VALUE_GET_OBJ(obj);
    ret = JS_GetOwnPropertyInternal(ctx, NULL, p, atom);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, obj);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_object_valueOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return JS_ToObject(ctx, this_val);
}

static JSValue js_object_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, tag;
    int is_array;
    JSAtom atom;
    JSObject *p;

    if (JS_IsNull(this_val)) {
        tag = js_new_string8(ctx, "Null");
    } else if (JS_IsUndefined(this_val)) {
        tag = js_new_string8(ctx, "Undefined");
    } else {
        obj = JS_ToObject(ctx, this_val);
        if (JS_IsException(obj))
            return obj;
        is_array = JS_IsArray(ctx, obj);
        if (is_array < 0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (is_array) {
            atom = JS_ATOM_Array;
        } else if (JS_IsFunction(ctx, obj)) {
            atom = JS_ATOM_Function;
        } else {
            p = JS_VALUE_GET_OBJ(obj);
            switch(p->class_id) {
            case JS_CLASS_STRING:
            case JS_CLASS_ARGUMENTS:
            case JS_CLASS_MAPPED_ARGUMENTS:
            case JS_CLASS_ERROR:
            case JS_CLASS_BOOLEAN:
            case JS_CLASS_NUMBER:
            case JS_CLASS_DATE:
            case JS_CLASS_REGEXP:
                atom = ctx->rt->class_array[p->class_id].class_name;
                break;
            default:
                atom = JS_ATOM_Object;
                break;
            }
        }
        tag = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_toStringTag);
        JS_FreeValue(ctx, obj);
        if (JS_IsException(tag))
            return JS_EXCEPTION;
        if (!JS_IsString(tag)) {
            JS_FreeValue(ctx, tag);
            tag = JS_AtomToString(ctx, atom);
        }
    }
    return JS_ConcatString3(ctx, "[object ", tag, "]");
}

static JSValue js_object_toLocaleString(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    return JS_Invoke(ctx, this_val, JS_ATOM_toString, 0, NULL);
}

static JSValue js_object_assign(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    // Object.assign(obj, source1)
    JSValue obj, s;
    int i;

    s = JS_UNDEFINED;
    obj = JS_ToObject(ctx, argv[0]);
    if (JS_IsException(obj))
        goto exception;
    for (i = 1; i < argc; i++) {
        if (!JS_IsNull(argv[i]) && !JS_IsUndefined(argv[i])) {
            s = JS_ToObject(ctx, argv[i]);
            if (JS_IsException(s))
                goto exception;
            if (JS_CopyDataProperties(ctx, obj, s, JS_UNDEFINED, TRUE))
                goto exception;
            JS_FreeValue(ctx, s);
        }
    }
    return obj;
exception:
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, s);
    return JS_EXCEPTION;
}

static JSValue js_object_seal(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int freeze_flag)
{
    JSValueConst obj = argv[0];
    JSObject *p;
    JSPropertyEnum *props;
    uint32_t len, i;
    int flags, desc_flags, res;

    if (!JS_IsObject(obj))
        return JS_DupValue(ctx, obj);

    res = JS_PreventExtensions(ctx, obj);
    if (res < 0)
        return JS_EXCEPTION;
    if (!res) {
        return JS_ThrowTypeError(ctx, "proxy preventExtensions handler returned false");
    }

    p = JS_VALUE_GET_OBJ(obj);
    flags = JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK;
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, p, flags))
        return JS_EXCEPTION;

    for(i = 0; i < len; i++) {
        JSPropertyDescriptor desc;
        JSAtom prop = props[i].atom;

        desc_flags = JS_PROP_THROW | JS_PROP_HAS_CONFIGURABLE;
        if (freeze_flag) {
            res = JS_GetOwnPropertyInternal(ctx, &desc, p, prop);
            if (res < 0)
                goto exception;
            if (res) {
                if (desc.flags & JS_PROP_WRITABLE)
                    desc_flags |= JS_PROP_HAS_WRITABLE;
                js_free_desc(ctx, &desc);
            }
        }
        if (JS_DefineProperty(ctx, obj, prop, JS_UNDEFINED,
                              JS_UNDEFINED, JS_UNDEFINED, desc_flags) < 0)
            goto exception;
    }
    JS_FreePropertyEnum(ctx, props, len);
    return JS_DupValue(ctx, obj);

 exception:
    JS_FreePropertyEnum(ctx, props, len);
    return JS_EXCEPTION;
}

static JSValue js_object_isSealed(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int is_frozen)
{
    JSValueConst obj = argv[0];
    JSObject *p;
    JSPropertyEnum *props;
    uint32_t len, i;
    int flags, res;

    if (!JS_IsObject(obj))
        return JS_TRUE;

    p = JS_VALUE_GET_OBJ(obj);
    flags = JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK;
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, p, flags))
        return JS_EXCEPTION;

    for(i = 0; i < len; i++) {
        JSPropertyDescriptor desc;
        JSAtom prop = props[i].atom;

        res = JS_GetOwnPropertyInternal(ctx, &desc, p, prop);
        if (res < 0)
            goto exception;
        if (res) {
            js_free_desc(ctx, &desc);
            if ((desc.flags & JS_PROP_CONFIGURABLE)
            ||  (is_frozen && (desc.flags & JS_PROP_WRITABLE))) {
                res = FALSE;
                goto done;
            }
        }
    }
    res = JS_IsExtensible(ctx, obj);
    if (res < 0)
        return JS_EXCEPTION;
    res ^= 1;
done:
    JS_FreePropertyEnum(ctx, props, len);
    return JS_NewBool(ctx, res);

exception:
    JS_FreePropertyEnum(ctx, props, len);
    return JS_EXCEPTION;
}

static JSValue js_object_fromEntries(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue obj, iter, next_method = JS_UNDEFINED;
    JSValueConst iterable;
    BOOL done;

    /*  RequireObjectCoercible() not necessary because it is tested in
        JS_GetIterator() by JS_GetProperty() */
    iterable = argv[0];

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;

    iter = JS_GetIterator(ctx, iterable, FALSE);
    if (JS_IsException(iter))
        goto fail;
    next_method = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next_method))
        goto fail;

    for(;;) {
        JSValue key, value, item;
        item = JS_IteratorNext(ctx, iter, next_method, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done)
            break;

        key = JS_UNDEFINED;
        value = JS_UNDEFINED;
        if (!JS_IsObject(item)) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            goto fail1;
        }
        key = JS_GetPropertyUint32(ctx, item, 0);
        if (JS_IsException(key))
            goto fail1;
        value = JS_GetPropertyUint32(ctx, item, 1);
        if (JS_IsException(value)) {
            JS_FreeValue(ctx, key);
            goto fail1;
        }
        if (JS_DefinePropertyValueValue(ctx, obj, key, value,
                                        JS_PROP_C_W_E | JS_PROP_THROW) < 0) {
        fail1:
            JS_FreeValue(ctx, item);
            goto fail;
        }
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    return obj;
 fail:
    if (JS_IsObject(iter)) {
        /* close the iterator object, preserving pending exception */
        JS_IteratorClose(ctx, iter, TRUE);
    }
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_object_is(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    return JS_NewBool(ctx, js_same_value(ctx, argv[0], argv[1]));
}

static JSValue JS_SpeciesConstructor(JSContext *ctx, JSValueConst obj,
                                     JSValueConst defaultConstructor)
{
    JSValue ctor, species;

    if (!JS_IsObject(obj))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    ctor = JS_GetProperty(ctx, obj, JS_ATOM_constructor);
    if (JS_IsException(ctor))
        return ctor;
    if (JS_IsUndefined(ctor))
        return JS_DupValue(ctx, defaultConstructor);
    if (!JS_IsObject(ctor)) {
        JS_FreeValue(ctx, ctor);
        return JS_ThrowTypeErrorNotAnObject(ctx);
    }
    species = JS_GetProperty(ctx, ctor, JS_ATOM_Symbol_species);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(species))
        return species;
    if (JS_IsUndefined(species) || JS_IsNull(species))
        return JS_DupValue(ctx, defaultConstructor);
    if (!JS_IsConstructor(ctx, species)) {
        JS_ThrowTypeErrorNotAConstructor(ctx, species);
        JS_FreeValue(ctx, species);
        return JS_EXCEPTION;
    }
    return species;
}

static JSValue js_object_get___proto__(JSContext *ctx, JSValueConst this_val)
{
    JSValue val, ret;

    val = JS_ToObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    ret = JS_GetPrototype(ctx, val);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_object_set___proto__(JSContext *ctx, JSValueConst this_val,
                                       JSValueConst proto)
{
    if (JS_IsUndefined(this_val) || JS_IsNull(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (!JS_IsObject(proto) && !JS_IsNull(proto))
        return JS_UNDEFINED;
    if (JS_SetPrototypeInternal(ctx, this_val, proto, TRUE) < 0)
        return JS_EXCEPTION;
    else
        return JS_UNDEFINED;
}

static JSValue js_object_isPrototypeOf(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValue obj, v1;
    JSValueConst v;
    int res;

    v = argv[0];
    if (!JS_IsObject(v))
        return JS_FALSE;
    obj = JS_ToObject(ctx, this_val);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    v1 = JS_DupValue(ctx, v);
    for(;;) {
        v1 = JS_GetPrototypeFree(ctx, v1);
        if (JS_IsException(v1))
            goto exception;
        if (JS_IsNull(v1)) {
            res = FALSE;
            break;
        }
        if (JS_VALUE_GET_OBJ(obj) == JS_VALUE_GET_OBJ(v1)) {
            res = TRUE;
            break;
        }
        /* avoid infinite loop (possible with proxies) */
        if (js_poll_interrupts(ctx))
            goto exception;
    }
    JS_FreeValue(ctx, v1);
    JS_FreeValue(ctx, obj);
    return JS_NewBool(ctx, res);

exception:
    JS_FreeValue(ctx, v1);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_object_propertyIsEnumerable(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    JSValue obj = JS_UNDEFINED, res = JS_EXCEPTION;
    JSAtom prop;
    JSPropertyDescriptor desc;
    int has_prop;

    prop = JS_ValueToAtom(ctx, argv[0]);
    if (unlikely(prop == JS_ATOM_NULL))
        goto exception;
    obj = JS_ToObject(ctx, this_val);
    if (JS_IsException(obj))
        goto exception;

    has_prop = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(obj), prop);
    if (has_prop < 0)
        goto exception;
    if (has_prop) {
        res = JS_NewBool(ctx, desc.flags & JS_PROP_ENUMERABLE);
        js_free_desc(ctx, &desc);
    } else {
        res = JS_FALSE;
    }

exception:
    JS_FreeAtom(ctx, prop);
    JS_FreeValue(ctx, obj);
    return res;
}

static JSValue js_object___lookupGetter__(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv, int setter)
{
    JSValue obj, res = JS_EXCEPTION;
    JSAtom prop = JS_ATOM_NULL;
    JSPropertyDescriptor desc;
    int has_prop;

    obj = JS_ToObject(ctx, this_val);
    if (JS_IsException(obj))
        goto exception;
    prop = JS_ValueToAtom(ctx, argv[0]);
    if (unlikely(prop == JS_ATOM_NULL))
        goto exception;

    for (;;) {
        has_prop = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(obj), prop);
        if (has_prop < 0)
            goto exception;
        if (has_prop) {
            if (desc.flags & JS_PROP_GETSET)
                res = JS_DupValue(ctx, setter ? desc.setter : desc.getter);
            else
                res = JS_UNDEFINED;
            js_free_desc(ctx, &desc);
            break;
        }
        obj = JS_GetPrototypeFree(ctx, obj);
        if (JS_IsException(obj))
            goto exception;
        if (JS_IsNull(obj)) {
            res = JS_UNDEFINED;
            break;
        }
        /* avoid infinite loop (possible with proxies) */
        if (js_poll_interrupts(ctx))
            goto exception;
    }

exception:
    JS_FreeAtom(ctx, prop);
    JS_FreeValue(ctx, obj);
    return res;
}

/* --- SugarJS 2.0 + Ramda 0.32 static Object utilities (SUGAR_RAMDA_NATIVE.md,
 * phase 4). Installed non-enumerable on the Object CONSTRUCTOR — NEVER on
 * Object.prototype (that would pollute every object and break test262). These
 * are pure data helpers: enumeration may trigger user getters (ordinary JS
 * semantics), but they hold no native handle, so there is no UAF hazard. Names
 * are all non-colliding with the ES `Object.*` statics. */

enum {
    OTYPE_UNDEFINED, OTYPE_NULL, OTYPE_BOOLEAN, OTYPE_NUMBER, OTYPE_STRING,
    OTYPE_SYMBOL, OTYPE_BIGINT, OTYPE_FUNCTION, OTYPE_ARRAY, OTYPE_DATE,
    OTYPE_REGEXP, OTYPE_ERROR, OTYPE_SET, OTYPE_MAP, OTYPE_ARGUMENTS,
    OTYPE_OBJECT,
};

static const char * const js_otype_names[] = {
    "Undefined", "Null", "Boolean", "Number", "String", "Symbol", "BigInt",
    "Function", "Array", "Date", "RegExp", "Error", "Set", "Map", "Arguments",
    "Object",
};

/* Ramda `type`-style tag, from the internal class (does NOT consult
 * Symbol.toStringTag — a deliberate, cheaper divergence). Number/Boolean/String
 * wrapper objects report the same tag as their primitives, matching Sugar. */
static int js_value_type_code(JSContext *ctx, JSValueConst v)
{
    int tag = JS_VALUE_GET_NORM_TAG(v);
    switch (tag) {
    case JS_TAG_UNDEFINED:     return OTYPE_UNDEFINED;
    case JS_TAG_NULL:          return OTYPE_NULL;
    case JS_TAG_BOOL:          return OTYPE_BOOLEAN;
    case JS_TAG_INT:
    case JS_TAG_FLOAT64:       return OTYPE_NUMBER;
    case JS_TAG_STRING:        return OTYPE_STRING;
    case JS_TAG_SYMBOL:        return OTYPE_SYMBOL;
    case JS_TAG_BIG_INT:
    case JS_TAG_SHORT_BIG_INT: return OTYPE_BIGINT;
    case JS_TAG_OBJECT: {
        JSObject *p = JS_VALUE_GET_OBJ(v);
        if (JS_IsFunction(ctx, v)) return OTYPE_FUNCTION;
        switch (p->class_id) {
        case JS_CLASS_ARRAY:            return OTYPE_ARRAY;
        case JS_CLASS_DATE:             return OTYPE_DATE;
        case JS_CLASS_REGEXP:           return OTYPE_REGEXP;
        case JS_CLASS_ERROR:            return OTYPE_ERROR;
        case JS_CLASS_SET:              return OTYPE_SET;
        case JS_CLASS_MAP:              return OTYPE_MAP;
        case JS_CLASS_ARGUMENTS:
        case JS_CLASS_MAPPED_ARGUMENTS: return OTYPE_ARGUMENTS;
        case JS_CLASS_NUMBER:           return OTYPE_NUMBER;
        case JS_CLASS_BOOLEAN:          return OTYPE_BOOLEAN;
        case JS_CLASS_STRING:           return OTYPE_STRING;
        default:                        return OTYPE_OBJECT;
        }
    }
    default:                   return OTYPE_OBJECT;
    }
}

/* Type guards: Object.isNumber/isString/... — magic is the target OTYPE_*. */
static JSValue js_object_ext_istype(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    return JS_NewBool(ctx, js_value_type_code(ctx, v) == magic);
}

/* isArray uses the real Array check so a Proxy-of-Array reports true. */
static JSValue js_object_ext_isArray(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    int r = JS_IsArray(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (r < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, r);
}

/* Ramda isNil / isNotNil — magic 0=isNil, 1=isNotNil. */
static JSValue js_object_ext_isNil(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    BOOL nil = JS_IsNull(v) || JS_IsUndefined(v);
    return JS_NewBool(ctx, magic ? !nil : nil);
}

/* Ramda type(v) -> "Number" | "String" | "Object" | ... */
static JSValue js_object_ext_type(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    int t = js_value_type_code(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    return js_new_string8(ctx, js_otype_names[t]);
}

/* Ramda defaultTo(d, v) -> v unless v is null/undefined/NaN, then d. */
static JSValue js_object_ext_defaultTo(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValueConst d = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
    BOOL use_d = JS_IsNull(v) || JS_IsUndefined(v);
    if (!use_d && JS_IsNumber(v)) {
        double dv;
        JS_ToFloat64(ctx, &dv, v);   /* cannot fail / run JS for a number */
        if (isnan(dv)) use_d = TRUE;
    }
    return JS_DupValue(ctx, use_d ? d : v);
}

/* Sugar size / isEmpty — count of own enumerable string keys. */
static JSValue js_object_ext_size(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    JSValue obj;
    JSPropertyEnum *props;
    uint32_t len;
    obj = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(obj)) return obj;
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, JS_VALUE_GET_OBJ(obj),
                                       JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, obj);
    return magic ? JS_NewBool(ctx, len == 0) : JS_NewInt64(ctx, len);
}

/* Sugar invert / Ramda invertObj — swap keys and values (last value wins).
 * Values become property keys via ToPropertyKey; keys become string values. */
static JSValue js_object_ext_invert(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, res = JS_UNDEFINED;
    JSPropertyEnum *props = NULL;
    uint32_t len, i;
    obj = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(obj)) return obj;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) goto fail;
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, JS_VALUE_GET_OBJ(obj),
                                       JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
        goto fail;
    for (i = 0; i < len; i++) {
        JSValue v = JS_GetProperty(ctx, obj, props[i].atom);
        JSAtom vkey;
        if (JS_IsException(v)) goto fail;
        vkey = JS_ValueToAtom(ctx, v);
        JS_FreeValue(ctx, v);
        if (vkey == JS_ATOM_NULL) goto fail;
        if (JS_DefinePropertyValue(ctx, res, vkey,
                                   JS_AtomToString(ctx, props[i].atom),
                                   JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, vkey);
            goto fail;
        }
        JS_FreeAtom(ctx, vkey);
    }
    JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, obj);
    return res;
 fail:
    if (props) JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* Ramda objOf(k, v) -> { [k]: v }. */
static JSValue js_object_ext_objOf(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue res;
    JSAtom a;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) return res;
    a = JS_ValueToAtom(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, res); return JS_EXCEPTION; }
    if (JS_DefinePropertyValue(ctx, res, a,
                               JS_DupValue(ctx, argc > 1 ? argv[1] : JS_UNDEFINED),
                               JS_PROP_C_W_E) < 0) {
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    JS_FreeAtom(ctx, a);
    return res;
}

/* Ramda pick(keys, o) -> new object with the listed keys that exist in o
 * (via the `in` operator, so inherited keys are included). */
static JSValue js_object_ext_pick(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValueConst keys = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue src, res = JS_UNDEFINED;
    int64_t klen, i;
    src = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(src)) return src;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) goto fail;
    if (js_get_length64(ctx, &klen, keys)) goto fail;
    for (i = 0; i < klen; i++) {
        JSValue k = JS_GetPropertyInt64(ctx, keys, i);
        JSAtom a;
        int has;
        if (JS_IsException(k)) goto fail;
        a = JS_ValueToAtom(ctx, k);
        JS_FreeValue(ctx, k);
        if (a == JS_ATOM_NULL) goto fail;
        has = JS_HasProperty(ctx, src, a);
        if (has < 0) { JS_FreeAtom(ctx, a); goto fail; }
        if (has) {
            JSValue v = JS_GetProperty(ctx, src, a);
            if (JS_IsException(v)) { JS_FreeAtom(ctx, a); goto fail; }
            if (JS_DefinePropertyValue(ctx, res, a, v, JS_PROP_C_W_E) < 0) {
                JS_FreeAtom(ctx, a); goto fail;
            }
        }
        JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, src);
    return res;
 fail:
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, src);
    return JS_EXCEPTION;
}

/* Ramda omit(keys, o) -> own enumerable string props of o not in keys. */
static JSValue js_object_ext_omit(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValueConst keys = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue src, res = JS_UNDEFINED;
    JSPropertyEnum *props = NULL;
    JSAtom *skip = NULL;
    uint32_t len = 0, i;
    int64_t klen = 0, j;
    src = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(src)) return src;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) goto fail;
    if (js_get_length64(ctx, &klen, keys)) goto fail;
    if (klen > 0) {
        skip = js_malloc(ctx, sizeof(JSAtom) * klen);
        if (!skip) goto fail;
        for (j = 0; j < klen; j++) skip[j] = JS_ATOM_NULL;
        for (j = 0; j < klen; j++) {
            JSValue k = JS_GetPropertyInt64(ctx, keys, j);
            if (JS_IsException(k)) goto fail;
            skip[j] = JS_ValueToAtom(ctx, k);
            JS_FreeValue(ctx, k);
            if (skip[j] == JS_ATOM_NULL) goto fail;
        }
    }
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, JS_VALUE_GET_OBJ(src),
                                       JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
        goto fail;
    for (i = 0; i < len; i++) {
        JSAtom a = props[i].atom;
        BOOL omit = FALSE;
        JSValue v;
        for (j = 0; j < klen; j++) if (skip[j] == a) { omit = TRUE; break; }
        if (omit) continue;
        v = JS_GetProperty(ctx, src, a);
        if (JS_IsException(v)) goto fail;
        if (JS_DefinePropertyValue(ctx, res, a, v, JS_PROP_C_W_E) < 0) goto fail;
    }
    JS_FreePropertyEnum(ctx, props, len);
    for (j = 0; j < klen; j++) JS_FreeAtom(ctx, skip[j]);
    js_free(ctx, skip);
    JS_FreeValue(ctx, src);
    return res;
 fail:
    if (props) JS_FreePropertyEnum(ctx, props, len);
    if (skip) { for (j = 0; j < klen; j++) JS_FreeAtom(ctx, skip[j]); js_free(ctx, skip); }
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, src);
    return JS_EXCEPTION;
}

/* Ramda pickBy(pred, o) -> props where pred(value, key) is truthy. */
static JSValue js_object_ext_pickBy(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValueConst pred = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue src, res = JS_UNDEFINED;
    JSPropertyEnum *props = NULL;
    uint32_t len = 0, i;
    src = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(src)) return src;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) goto fail;
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, JS_VALUE_GET_OBJ(src),
                                       JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
        goto fail;
    for (i = 0; i < len; i++) {
        JSAtom a = props[i].atom;
        JSValue v, ret, cargs[2];
        int keep;
        v = JS_GetProperty(ctx, src, a);
        if (JS_IsException(v)) goto fail;
        cargs[0] = v;
        cargs[1] = JS_AtomToString(ctx, a);
        ret = JS_Call(ctx, pred, JS_UNDEFINED, 2, (JSValueConst *)cargs);
        JS_FreeValue(ctx, cargs[1]);
        if (JS_IsException(ret)) { JS_FreeValue(ctx, v); goto fail; }
        keep = JS_ToBool(ctx, ret);
        JS_FreeValue(ctx, ret);
        if (keep) {
            if (JS_DefinePropertyValue(ctx, res, a, v, JS_PROP_C_W_E) < 0) goto fail;
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, src);
    return res;
 fail:
    if (props) JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, src);
    return JS_EXCEPTION;
}

/* Ramda toPairs(o) -> [[k, v], ...] (own enumerable string entries). */
static JSValue js_object_ext_toPairs(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    return JS_GetOwnPropertyNames2(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                                   JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK,
                                   JS_ITERATOR_KIND_KEY_AND_VALUE);
}

/* Ramda fromPairs(pairs) -> object built from an array of [k, v] pairs. */
static JSValue js_object_ext_fromPairs(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValueConst pairs = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue res;
    int64_t len, i;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) return res;
    if (js_get_length64(ctx, &len, pairs)) goto fail;
    for (i = 0; i < len; i++) {
        JSValue pair = JS_GetPropertyInt64(ctx, pairs, i);
        JSValue k, v;
        JSAtom a;
        if (JS_IsException(pair)) goto fail;
        k = JS_GetPropertyUint32(ctx, pair, 0);
        v = JS_GetPropertyUint32(ctx, pair, 1);
        JS_FreeValue(ctx, pair);
        if (JS_IsException(k) || JS_IsException(v)) {
            JS_FreeValue(ctx, k); JS_FreeValue(ctx, v); goto fail;
        }
        a = JS_ValueToAtom(ctx, k);
        JS_FreeValue(ctx, k);
        if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, v); goto fail; }
        if (JS_DefinePropertyValue(ctx, res, a, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, a); goto fail;
        }
        JS_FreeAtom(ctx, a);
    }
    return res;
 fail:
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

/* Ramda assoc(k, v, o) -> shallow copy of o with k set to v. */
static JSValue js_object_ext_assoc(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue src, res = JS_UNDEFINED;
    JSAtom a;
    src = JS_ToObject(ctx, argc > 2 ? argv[2] : JS_UNDEFINED);
    if (JS_IsException(src)) return src;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) goto fail;
    if (JS_CopyDataProperties(ctx, res, src, JS_UNDEFINED, TRUE)) goto fail;
    a = JS_ValueToAtom(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (a == JS_ATOM_NULL) goto fail;
    if (JS_DefinePropertyValue(ctx, res, a,
                               JS_DupValue(ctx, argc > 1 ? argv[1] : JS_UNDEFINED),
                               JS_PROP_C_W_E) < 0) {
        JS_FreeAtom(ctx, a); goto fail;
    }
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, src);
    return res;
 fail:
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, src);
    return JS_EXCEPTION;
}

/* Ramda dissoc(k, o) -> shallow copy of o without k. */
static JSValue js_object_ext_dissoc(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue src, res = JS_UNDEFINED;
    JSAtom a;
    src = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(src)) return src;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) goto fail;
    if (JS_CopyDataProperties(ctx, res, src, JS_UNDEFINED, TRUE)) goto fail;
    a = JS_ValueToAtom(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (a == JS_ATOM_NULL) goto fail;
    if (JS_DeleteProperty(ctx, res, a, 0) < 0) { JS_FreeAtom(ctx, a); goto fail; }
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, src);
    return res;
 fail:
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, src);
    return JS_EXCEPTION;
}

/* Ramda tap(fn, x) -> call fn(x) for its side effect, return x. */
static JSValue js_object_ext_tap(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst x = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&x);
    if (JS_IsException(ret)) return JS_EXCEPTION;
    JS_FreeValue(ctx, ret);
    return JS_DupValue(ctx, x);
}

/* ===== Object static batch 2 — deep clone / equals / merge / path ===== */

/* Shallow copy of a container: a fresh Array (same length, dup'd elements) if v
 * is an array, else a fresh plain object with v's own enumerable props. */
static JSValue js_shallow_clone_container(JSContext *ctx, JSValueConst v)
{
    if (JS_IsArray(ctx, v) > 0) {
        JSValue res;
        int64_t len, i;
        if (js_get_length64(ctx, &len, v)) return JS_EXCEPTION;
        res = JS_NewArray(ctx);
        if (JS_IsException(res)) return res;
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetPropertyInt64(ctx, v, i);
            if (JS_IsException(el)) { JS_FreeValue(ctx, res); return JS_EXCEPTION; }
            if (JS_SetPropertyInt64(ctx, res, i, el) < 0) { JS_FreeValue(ctx, res); return JS_EXCEPTION; }
        }
        return res;
    } else {
        JSValue res = JS_NewObject(ctx);
        if (JS_IsException(res)) return res;
        if (JS_CopyDataProperties(ctx, res, v, JS_UNDEFINED, TRUE)) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        return res;
    }
}

/* Ramda clone: deep-copies plain objects and arrays (recursively), and Date /
 * RegExp via their constructors. Primitives, functions and other exotics are
 * returned by reference (documented divergence — Ramda deep-copies more). */
static JSValue js_deep_clone(JSContext *ctx, JSValueConst v, int depth)
{
    int t;
    (void)depth;
    if (js_check_stack_overflow(ctx->rt, 0))
        return JS_ThrowStackOverflow(ctx);
    t = js_value_type_code(ctx, v);
    switch (t) {
    case OTYPE_ARRAY: {
        JSValue res;
        int64_t len, i;
        if (js_get_length64(ctx, &len, v)) return JS_EXCEPTION;
        res = JS_NewArray(ctx);
        if (JS_IsException(res)) return res;
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetPropertyInt64(ctx, v, i);
            JSValue c;
            if (JS_IsException(el)) { JS_FreeValue(ctx, res); return JS_EXCEPTION; }
            c = js_deep_clone(ctx, el, depth + 1);
            JS_FreeValue(ctx, el);
            if (JS_IsException(c)) { JS_FreeValue(ctx, res); return JS_EXCEPTION; }
            if (JS_SetPropertyInt64(ctx, res, i, c) < 0) { JS_FreeValue(ctx, res); return JS_EXCEPTION; }
        }
        return res;
    }
    case OTYPE_OBJECT: {
        JSValue res;
        JSPropertyEnum *props = NULL;
        uint32_t len, i;
        res = JS_NewObject(ctx);
        if (JS_IsException(res)) return res;
        if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, JS_VALUE_GET_OBJ(v),
                                           JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetProperty(ctx, v, props[i].atom);
            JSValue c;
            if (JS_IsException(el)) goto obj_fail;
            c = js_deep_clone(ctx, el, depth + 1);
            JS_FreeValue(ctx, el);
            if (JS_IsException(c)) goto obj_fail;
            if (JS_DefinePropertyValue(ctx, res, props[i].atom, c, JS_PROP_C_W_E) < 0)
                goto obj_fail;
        }
        JS_FreePropertyEnum(ctx, props, len);
        return res;
    obj_fail:
        JS_FreePropertyEnum(ctx, props, len);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    case OTYPE_DATE: {
        JSValue g, ctor, ms, res;
        double d;
        if (JS_ToFloat64(ctx, &d, v)) return JS_EXCEPTION;
        g = JS_GetGlobalObject(ctx);
        ctor = JS_GetPropertyStr(ctx, g, "Date");
        JS_FreeValue(ctx, g);
        if (JS_IsException(ctor)) return JS_EXCEPTION;
        ms = JS_NewFloat64(ctx, d);
        res = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&ms);
        JS_FreeValue(ctx, ms);
        JS_FreeValue(ctx, ctor);
        return res;
    }
    case OTYPE_REGEXP: {
        JSValue g, ctor, res;
        g = JS_GetGlobalObject(ctx);
        ctor = JS_GetPropertyStr(ctx, g, "RegExp");
        JS_FreeValue(ctx, g);
        if (JS_IsException(ctor)) return JS_EXCEPTION;
        res = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&v);
        JS_FreeValue(ctx, ctor);
        return res;
    }
    default:
        return JS_DupValue(ctx, v);
    }
}

/* Ramda equals: deep structural equality for primitives (SameValueZero, so
 * NaN==NaN), arrays, plain objects/arguments, Date (by time) and RegExp (by
 * source+flags). Other exotics (Map/Set/wrappers) compare by reference
 * (documented divergence). */
static int js_deep_equals(JSContext *ctx, JSValueConst a, JSValueConst b, int depth)
{
    int ta, tb;
    if (js_check_stack_overflow(ctx->rt, 0)) {
        JS_ThrowStackOverflow(ctx);
        return -1;
    }
    /* SameValue for the base case: NaN equals NaN, but +0 and -0 differ
     * (matches Ramda `equals`). */
    if (js_same_value(ctx, a, b))
        return 1;
    ta = js_value_type_code(ctx, a);
    tb = js_value_type_code(ctx, b);
    if (ta != tb)
        return 0;
    switch (ta) {
    case OTYPE_ARRAY: {
        int64_t la, lb, i;
        if (js_get_length64(ctx, &la, a) || js_get_length64(ctx, &lb, b))
            return -1;
        if (la != lb) return 0;
        for (i = 0; i < la; i++) {
            JSValue va = JS_GetPropertyInt64(ctx, a, i);
            JSValue vb;
            int r;
            if (JS_IsException(va)) return -1;
            vb = JS_GetPropertyInt64(ctx, b, i);
            if (JS_IsException(vb)) { JS_FreeValue(ctx, va); return -1; }
            r = js_deep_equals(ctx, va, vb, depth + 1);
            JS_FreeValue(ctx, va);
            JS_FreeValue(ctx, vb);
            if (r <= 0) return r;
        }
        return 1;
    }
    case OTYPE_OBJECT:
    case OTYPE_ARGUMENTS: {
        JSPropertyEnum *pa = NULL, *pb = NULL;
        uint32_t na, nb, i;
        int ret = 1;
        if (JS_GetOwnPropertyNamesInternal(ctx, &pa, &na, JS_VALUE_GET_OBJ(a),
                                           JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
            return -1;
        if (JS_GetOwnPropertyNamesInternal(ctx, &pb, &nb, JS_VALUE_GET_OBJ(b),
                                           JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)) {
            JS_FreePropertyEnum(ctx, pa, na);
            return -1;
        }
        if (na != nb) { ret = 0; goto obj_done; }
        for (i = 0; i < na; i++) {
            JSPropertyDescriptor desc;
            JSValue va, vb;
            int r, own;
            own = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(b), pa[i].atom);
            if (own < 0) { ret = -1; goto obj_done; }
            if (own == 0) { ret = 0; goto obj_done; }   /* b lacks a's own key */
            js_free_desc(ctx, &desc);
            va = JS_GetProperty(ctx, a, pa[i].atom);
            if (JS_IsException(va)) { ret = -1; goto obj_done; }
            vb = JS_GetProperty(ctx, b, pa[i].atom);
            if (JS_IsException(vb)) { JS_FreeValue(ctx, va); ret = -1; goto obj_done; }
            r = js_deep_equals(ctx, va, vb, depth + 1);
            JS_FreeValue(ctx, va);
            JS_FreeValue(ctx, vb);
            if (r <= 0) { ret = r; goto obj_done; }
        }
    obj_done:
        JS_FreePropertyEnum(ctx, pa, na);
        JS_FreePropertyEnum(ctx, pb, nb);
        return ret;
    }
    case OTYPE_DATE: {
        double da, db;
        if (JS_ToFloat64(ctx, &da, a) || JS_ToFloat64(ctx, &db, b))
            return -1;
        return (da == db) || (isnan(da) && isnan(db));
    }
    case OTYPE_REGEXP: {
        JSValue sa, sb, fa, fb;
        int r = 0;
        sa = JS_GetPropertyStr(ctx, a, "source");
        sb = JS_GetPropertyStr(ctx, b, "source");
        fa = JS_GetPropertyStr(ctx, a, "flags");
        fb = JS_GetPropertyStr(ctx, b, "flags");
        if (!JS_IsException(sa) && !JS_IsException(sb) &&
            !JS_IsException(fa) && !JS_IsException(fb))
            r = js_same_value(ctx, sa, sb) && js_same_value(ctx, fa, fb);
        JS_FreeValue(ctx, sa); JS_FreeValue(ctx, sb);
        JS_FreeValue(ctx, fa); JS_FreeValue(ctx, fb);
        return r;
    }
    default:
        return 0;   /* different references of an unsupported exotic */
    }
}

static JSValue js_object_ext_clone(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    return js_deep_clone(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 0);
}

static JSValue js_object_ext_equals(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    int r = js_deep_equals(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                           argc > 1 ? argv[1] : JS_UNDEFINED, 0);
    if (r < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, r);
}

static JSValue js_object_ext_identical(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    return JS_NewBool(ctx, js_same_value(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                                         argc > 1 ? argv[1] : JS_UNDEFINED));
}

/* Resolve obj[path...] where path is an array of keys or a dotted string.
 * Value is owned; JS_UNDEFINED if a step is nullish/missing; JS_EXCEPTION on
 * a real error. */
static JSValue js_object_path_get(JSContext *ctx, JSValueConst obj, JSValueConst path)
{
    JSValue cur = JS_DupValue(ctx, obj);
    if (JS_IsString(path)) {
        const char *s = JS_ToCString(ctx, path);
        const char *p;
        if (!s) { JS_FreeValue(ctx, cur); return JS_EXCEPTION; }
        p = s;
        while (*p) {
            const char *dot = strchr(p, '.');
            const char *e = dot ? dot : p + strlen(p);
            JSAtom atom;
            JSValue next;
            if (JS_IsNull(cur) || JS_IsUndefined(cur)) {
                JS_FreeValue(ctx, cur); JS_FreeCString(ctx, s); return JS_UNDEFINED;
            }
            atom = JS_NewAtomLen(ctx, p, e - p);
            if (atom == JS_ATOM_NULL) { JS_FreeValue(ctx, cur); JS_FreeCString(ctx, s); return JS_EXCEPTION; }
            next = JS_GetProperty(ctx, cur, atom);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, cur);
            if (JS_IsException(next)) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
            cur = next;
            p = dot ? dot + 1 : e;
        }
        JS_FreeCString(ctx, s);
        return cur;
    } else {
        int64_t len, i;
        if (js_get_length64(ctx, &len, path)) { JS_FreeValue(ctx, cur); return JS_EXCEPTION; }
        for (i = 0; i < len; i++) {
            JSValue k, next;
            JSAtom atom;
            if (JS_IsNull(cur) || JS_IsUndefined(cur)) { JS_FreeValue(ctx, cur); return JS_UNDEFINED; }
            k = JS_GetPropertyInt64(ctx, path, i);
            if (JS_IsException(k)) { JS_FreeValue(ctx, cur); return JS_EXCEPTION; }
            atom = JS_ValueToAtom(ctx, k);
            JS_FreeValue(ctx, k);
            if (atom == JS_ATOM_NULL) { JS_FreeValue(ctx, cur); return JS_EXCEPTION; }
            next = JS_GetProperty(ctx, cur, atom);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, cur);
            if (JS_IsException(next)) return JS_EXCEPTION;
            cur = next;
        }
        return cur;
    }
}

/* prop(k, obj) / propOr(default, k, obj) — magic 1 selects the *Or variant. */
static JSValue js_object_ext_prop(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    JSValueConst def = magic ? argv[0] : JS_UNDEFINED;
    JSValueConst key = argv[magic ? 1 : 0];
    JSValueConst obj = argv[magic ? 2 : 1];
    JSAtom atom;
    JSValue v;
    atom = JS_ValueToAtom(ctx, key);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
    v = JS_GetProperty(ctx, obj, atom);
    JS_FreeAtom(ctx, atom);
    if (JS_IsException(v)) return v;
    if (magic && JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return JS_DupValue(ctx, def); }
    return v;
}

/* path(path, obj) / pathOr(default, path, obj) — magic 1 selects *Or. */
static JSValue js_object_ext_path(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    JSValueConst def = magic ? argv[0] : JS_UNDEFINED;
    JSValueConst path = argv[magic ? 1 : 0];
    JSValueConst obj = argv[magic ? 2 : 1];
    JSValue v = js_object_path_get(ctx, obj, path);
    if (JS_IsException(v)) return v;
    if (magic && JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return JS_DupValue(ctx, def); }
    return v;
}

/* props(keys, obj) -> [obj[k] for k in keys]. */
static JSValue js_object_ext_props(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValueConst keys = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst obj = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue res;
    int64_t len, i;
    if (js_get_length64(ctx, &len, keys)) return JS_EXCEPTION;
    res = JS_NewArray(ctx);
    if (JS_IsException(res)) return res;
    for (i = 0; i < len; i++) {
        JSValue k = JS_GetPropertyInt64(ctx, keys, i);
        JSAtom atom;
        JSValue v;
        if (JS_IsException(k)) goto fail;
        atom = JS_ValueToAtom(ctx, k);
        JS_FreeValue(ctx, k);
        if (atom == JS_ATOM_NULL) goto fail;
        v = JS_GetProperty(ctx, obj, atom);
        JS_FreeAtom(ctx, atom);
        if (JS_IsException(v)) goto fail;
        if (JS_SetPropertyInt64(ctx, res, i, v) < 0) goto fail;
    }
    return res;
 fail:
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

/* paths(pathList, obj) -> [path(p, obj) for p in pathList]. */
static JSValue js_object_ext_paths(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValueConst list = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst obj = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue res;
    int64_t len, i;
    if (js_get_length64(ctx, &len, list)) return JS_EXCEPTION;
    res = JS_NewArray(ctx);
    if (JS_IsException(res)) return res;
    for (i = 0; i < len; i++) {
        JSValue p = JS_GetPropertyInt64(ctx, list, i);
        JSValue v;
        if (JS_IsException(p)) goto fail;
        v = js_object_path_get(ctx, obj, p);
        JS_FreeValue(ctx, p);
        if (JS_IsException(v)) goto fail;
        if (JS_SetPropertyInt64(ctx, res, i, v) < 0) goto fail;
    }
    return res;
 fail:
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

static void js_free_atoms(JSContext *ctx, JSAtom *atoms, int n);

/* Normalize a path (dotted string or array) into an owned JSAtom array; the
 * caller frees it with js_free_atoms. Returns 0 on success, -1 on error. */
static int js_path_to_atoms(JSContext *ctx, JSValueConst path,
                            JSAtom **out, int *out_len)
{
    JSAtom *atoms = NULL;
    int n = 0, cap = 0;
    *out = NULL; *out_len = 0;
    if (JS_IsString(path)) {
        const char *s = JS_ToCString(ctx, path), *p;
        if (!s) return -1;
        p = s;
        while (*p) {
            const char *dot = strchr(p, '.');
            const char *e = dot ? dot : p + strlen(p);
            JSAtom a;
            if (n >= cap) {
                int ncap = cap ? cap * 2 : 4;
                JSAtom *na = js_realloc(ctx, atoms, sizeof(JSAtom) * ncap);
                if (!na) { JS_FreeCString(ctx, s); goto fail; }
                atoms = na; cap = ncap;
            }
            a = JS_NewAtomLen(ctx, p, e - p);
            if (a == JS_ATOM_NULL) { JS_FreeCString(ctx, s); goto fail; }
            atoms[n++] = a;
            if (!dot) break;
            p = dot + 1;
        }
        JS_FreeCString(ctx, s);
    } else {
        int64_t len, i;
        if (js_get_length64(ctx, &len, path)) return -1;
        if (len > 0) {
            atoms = js_malloc(ctx, sizeof(JSAtom) * len);
            if (!atoms) return -1;
            cap = (int)len;
            for (i = 0; i < len; i++) {
                JSValue k = JS_GetPropertyInt64(ctx, path, i);
                JSAtom a;
                if (JS_IsException(k)) goto fail;
                a = JS_ValueToAtom(ctx, k);
                JS_FreeValue(ctx, k);
                if (a == JS_ATOM_NULL) goto fail;
                atoms[n++] = a;
            }
        }
    }
    *out = atoms; *out_len = n;
    return 0;
 fail:
    js_free_atoms(ctx, atoms, n);
    return -1;
}

static void js_free_atoms(JSContext *ctx, JSAtom *atoms, int n)
{
    int i;
    for (i = 0; i < n; i++) JS_FreeAtom(ctx, atoms[i]);
    js_free(ctx, atoms);
}

/* hasPath(path, obj) — every level exists as an own property. */
static JSValue js_object_ext_hasPath(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSAtom *atoms;
    int n, i, ok = 1;
    JSValue cur;
    if (js_path_to_atoms(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &atoms, &n))
        return JS_EXCEPTION;
    cur = JS_DupValue(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    for (i = 0; i < n; i++) {
        JSPropertyDescriptor desc;
        int own;
        JSValue next;
        if (!JS_IsObject(cur)) { ok = 0; break; }
        own = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(cur), atoms[i]);
        if (own < 0) { JS_FreeValue(ctx, cur); js_free_atoms(ctx, atoms, n); return JS_EXCEPTION; }
        if (own == 0) { ok = 0; break; }
        js_free_desc(ctx, &desc);
        next = JS_GetProperty(ctx, cur, atoms[i]);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(next)) { js_free_atoms(ctx, atoms, n); return JS_EXCEPTION; }
        cur = next;
    }
    JS_FreeValue(ctx, cur);
    js_free_atoms(ctx, atoms, n);
    return JS_NewBool(ctx, ok);
}

/* has(prop, obj) — own property (Ramda). hasIn(prop, obj) — the `in` operator. */
static JSValue js_object_ext_has(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    JSValueConst obj = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSAtom atom;
    int r;
    if (!JS_IsObject(obj)) return JS_FALSE;
    atom = JS_ValueToAtom(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
    if (magic) {                              /* hasIn: prototype chain */
        r = JS_HasProperty(ctx, obj, atom);
    } else {                                  /* has: own only */
        JSPropertyDescriptor desc;
        r = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(obj), atom);
        if (r > 0) js_free_desc(ctx, &desc);
    }
    JS_FreeAtom(ctx, atom);
    if (r < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, r);
}

/* keysIn / valuesIn — enumerable properties incl. inherited (magic 1=values). */
static JSValue js_object_ext_keysIn(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue obj, res, proto;
    JSAtom *seen = NULL;
    int seen_n = 0, seen_cap = 0;
    int64_t out_i = 0;
    obj = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(obj)) return obj;
    res = JS_NewArray(ctx);
    if (JS_IsException(res)) { JS_FreeValue(ctx, obj); return res; }
    proto = JS_DupValue(ctx, obj);
    while (JS_IsObject(proto)) {
        JSPropertyEnum *props = NULL;
        uint32_t len, i;
        JSValue nextproto;
        if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, JS_VALUE_GET_OBJ(proto),
                                           JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
            goto fail;
        for (i = 0; i < len; i++) {
            JSAtom a = props[i].atom;
            int j, dup = 0;
            JSValue item;
            for (j = 0; j < seen_n; j++) if (seen[j] == a) { dup = 1; break; }
            if (dup) continue;
            if (seen_n >= seen_cap) {
                int ncap = seen_cap ? seen_cap * 2 : 8;
                JSAtom *ns = js_realloc(ctx, seen, sizeof(JSAtom) * ncap);
                if (!ns) { JS_FreePropertyEnum(ctx, props, len); goto fail; }
                seen = ns; seen_cap = ncap;
            }
            seen[seen_n++] = JS_DupAtom(ctx, a);
            if (magic) {
                item = JS_GetProperty(ctx, obj, a);
                if (JS_IsException(item)) { JS_FreePropertyEnum(ctx, props, len); goto fail; }
            } else {
                item = JS_AtomToString(ctx, a);
            }
            if (JS_SetPropertyInt64(ctx, res, out_i++, item) < 0) {
                JS_FreePropertyEnum(ctx, props, len); goto fail;
            }
        }
        JS_FreePropertyEnum(ctx, props, len);
        nextproto = JS_GetPrototype(ctx, proto);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(nextproto)) { proto = JS_NULL; goto fail; }
        proto = nextproto;
    }
    JS_FreeValue(ctx, proto);
    js_free_atoms(ctx, seen, seen_n);
    JS_FreeValue(ctx, obj);
    return res;
 fail:
    JS_FreeValue(ctx, proto);
    js_free_atoms(ctx, seen, seen_n);
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* propEq(val, k, obj) / eqProps(k, a, b) — magic 1 selects eqProps. */
static JSValue js_object_ext_propEq(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSAtom atom;
    JSValue va, vb;
    int r;
    if (magic) {   /* eqProps(k, a, b): equals(a[k], b[k]) */
        atom = JS_ValueToAtom(ctx, argv[0]);
        if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
        va = JS_GetProperty(ctx, argv[1], atom);
        if (JS_IsException(va)) { JS_FreeAtom(ctx, atom); return JS_EXCEPTION; }
        vb = JS_GetProperty(ctx, argv[2], atom);
        JS_FreeAtom(ctx, atom);
        if (JS_IsException(vb)) { JS_FreeValue(ctx, va); return JS_EXCEPTION; }
    } else {       /* propEq(val, k, obj): equals(obj[k], val) */
        atom = JS_ValueToAtom(ctx, argv[1]);
        if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
        va = JS_GetProperty(ctx, argv[2], atom);
        JS_FreeAtom(ctx, atom);
        if (JS_IsException(va)) return JS_EXCEPTION;
        vb = JS_DupValue(ctx, argv[0]);
    }
    r = js_deep_equals(ctx, va, vb, 0);
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vb);
    if (r < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, r);
}

/* pathEq(val, path, obj): equals(path(path, obj), val). */
static JSValue js_object_ext_pathEq(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue v = js_object_path_get(ctx, argc > 2 ? argv[2] : JS_UNDEFINED,
                                   argc > 1 ? argv[1] : JS_UNDEFINED);
    int r;
    if (JS_IsException(v)) return v;
    r = js_deep_equals(ctx, v, argc > 0 ? argv[0] : JS_UNDEFINED, 0);
    JS_FreeValue(ctx, v);
    if (r < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, r);
}

/* where(spec, obj): every spec[k] is a predicate that obj[k] must satisfy.
 * whereEq(spec, obj): every obj[k] deep-equals spec[k]. magic 1 = whereEq. */
static JSValue js_object_ext_where(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    JSValueConst spec = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst obj = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue specobj;
    JSPropertyEnum *props = NULL;
    uint32_t len, i;
    int result = 1;
    specobj = JS_ToObject(ctx, spec);
    if (JS_IsException(specobj)) return specobj;
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, JS_VALUE_GET_OBJ(specobj),
                                       JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)) {
        JS_FreeValue(ctx, specobj);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len && result; i++) {
        JSValue sv = JS_GetProperty(ctx, specobj, props[i].atom);
        JSValue ov;
        if (JS_IsException(sv)) { result = -1; break; }
        ov = JS_GetProperty(ctx, obj, props[i].atom);
        if (JS_IsException(ov)) { JS_FreeValue(ctx, sv); result = -1; break; }
        if (magic) {
            int r = js_deep_equals(ctx, ov, sv, 0);
            if (r < 0) result = -1; else if (r == 0) result = 0;
        } else {
            JSValue ret = JS_Call(ctx, sv, JS_UNDEFINED, 1, (JSValueConst *)&ov);
            if (JS_IsException(ret)) result = -1;
            else { if (!JS_ToBool(ctx, ret)) result = 0; JS_FreeValue(ctx, ret); }
        }
        JS_FreeValue(ctx, sv);
        JS_FreeValue(ctx, ov);
    }
    JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, specobj);
    if (result < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, result);
}

/* Shallow merge: both keep the LEFT object's key order (Ramda), only the
 * conflict value differs. mergeRight(a,b) -> b wins; mergeLeft(a,b) -> a wins.
 * magic 0 = mergeRight/merge, 1 = mergeLeft. */
static JSValue js_object_ext_merge(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    JSValueConst a = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst b = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue res, oa = JS_UNDEFINED, ob = JS_UNDEFINED;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) return res;
    oa = JS_ToObject(ctx, a);
    if (JS_IsException(oa)) goto fail;
    ob = JS_ToObject(ctx, b);
    if (JS_IsException(ob)) goto fail;
    /* left keys first, in left order */
    if (JS_CopyDataProperties(ctx, res, oa, JS_UNDEFINED, TRUE)) goto fail;
    if (magic == 0) {
        /* mergeRight: right overwrites, right-only keys appended */
        if (JS_CopyDataProperties(ctx, res, ob, JS_UNDEFINED, TRUE)) goto fail;
    } else {
        /* mergeLeft: add only right's keys that the left lacks */
        JSPropertyEnum *props = NULL;
        uint32_t len, i;
        if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, JS_VALUE_GET_OBJ(ob),
                                           JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
            goto fail;
        for (i = 0; i < len; i++) {
            JSPropertyDescriptor desc;
            int has = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(res), props[i].atom);
            if (has < 0) { JS_FreePropertyEnum(ctx, props, len); goto fail; }
            if (has) { js_free_desc(ctx, &desc); continue; }
            {
                JSValue v = JS_GetProperty(ctx, ob, props[i].atom);
                if (JS_IsException(v)) { JS_FreePropertyEnum(ctx, props, len); goto fail; }
                if (JS_DefinePropertyValue(ctx, res, props[i].atom, v, JS_PROP_C_W_E) < 0) {
                    JS_FreePropertyEnum(ctx, props, len); goto fail;
                }
            }
        }
        JS_FreePropertyEnum(ctx, props, len);
    }
    JS_FreeValue(ctx, oa);
    JS_FreeValue(ctx, ob);
    return res;
 fail:
    JS_FreeValue(ctx, oa);
    JS_FreeValue(ctx, ob);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

/* Deep merge of two plain objects. right_wins picks the loser's value only when
 * the pair is not two plain objects (which recurse). */
static JSValue js_deep_merge(JSContext *ctx, JSValueConst l, JSValueConst r,
                             int depth, int right_wins)
{
    JSValue res;
    JSPropertyEnum *props = NULL;
    uint32_t len, i;
    if (js_check_stack_overflow(ctx->rt, 0))
        return JS_ThrowStackOverflow(ctx);
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) return res;
    if (JS_CopyDataProperties(ctx, res, l, JS_UNDEFINED, TRUE)) goto fail;
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, JS_VALUE_GET_OBJ(r),
                                       JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
        goto fail;
    for (i = 0; i < len; i++) {
        JSAtom a = props[i].atom;
        JSValue rv, lv;
        JSPropertyDescriptor desc;
        int has_l;
        rv = JS_GetProperty(ctx, r, a);
        if (JS_IsException(rv)) goto fail2;
        has_l = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(res), a);
        if (has_l < 0) { JS_FreeValue(ctx, rv); goto fail2; }
        if (has_l == 0) {
            if (JS_DefinePropertyValue(ctx, res, a, rv, JS_PROP_C_W_E) < 0) goto fail2;
            continue;
        }
        js_free_desc(ctx, &desc);
        lv = JS_GetProperty(ctx, res, a);
        if (JS_IsException(lv)) { JS_FreeValue(ctx, rv); goto fail2; }
        if (js_value_type_code(ctx, lv) == OTYPE_OBJECT &&
            js_value_type_code(ctx, rv) == OTYPE_OBJECT) {
            JSValue merged = js_deep_merge(ctx, lv, rv, depth + 1, right_wins);
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, rv);
            if (JS_IsException(merged)) goto fail2;
            if (JS_DefinePropertyValue(ctx, res, a, merged, JS_PROP_C_W_E) < 0) goto fail2;
        } else if (right_wins) {
            JS_FreeValue(ctx, lv);
            if (JS_DefinePropertyValue(ctx, res, a, rv, JS_PROP_C_W_E) < 0) goto fail2;
        } else {
            JS_FreeValue(ctx, lv);   /* keep left (already in res) */
            JS_FreeValue(ctx, rv);
        }
    }
    JS_FreePropertyEnum(ctx, props, len);
    return res;
 fail2:
    JS_FreePropertyEnum(ctx, props, len);
 fail:
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

static JSValue js_object_ext_mergeDeep(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int magic)
{
    /* magic 0 = mergeDeepRight (right wins), 1 = mergeDeepLeft (left wins) */
    JSValue oa, ob, res;
    oa = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(oa)) return oa;
    ob = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(ob)) { JS_FreeValue(ctx, oa); return ob; }
    res = js_deep_merge(ctx, oa, ob, 0, magic ? 0 : 1);
    JS_FreeValue(ctx, oa);
    JS_FreeValue(ctx, ob);
    return res;
}

/* assocPath recursion — see js_object_ext_assocPath. Missing intermediates are
 * created as plain objects (documented divergence: Ramda auto-creates arrays
 * for integer keys into undefined). */
static JSValue js_assoc_path(JSContext *ctx, JSAtom *atoms, int i, int n,
                             JSValueConst val, JSValueConst cur)
{
    JSValue container, child, newchild;
    if (i == n)
        return JS_DupValue(ctx, val);
    if (JS_IsObject(cur))
        container = js_shallow_clone_container(ctx, cur);
    else
        container = JS_NewObject(ctx);
    if (JS_IsException(container)) return container;
    if (JS_IsObject(cur)) {
        child = JS_GetProperty(ctx, cur, atoms[i]);
        if (JS_IsException(child)) { JS_FreeValue(ctx, container); return JS_EXCEPTION; }
    } else {
        child = JS_UNDEFINED;
    }
    newchild = js_assoc_path(ctx, atoms, i + 1, n, val, child);
    JS_FreeValue(ctx, child);
    if (JS_IsException(newchild)) { JS_FreeValue(ctx, container); return JS_EXCEPTION; }
    if (JS_DefinePropertyValue(ctx, container, atoms[i], newchild, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, container);
        return JS_EXCEPTION;
    }
    return container;
}

/* assocPath(path, val, obj) -> immutable deep set. */
static JSValue js_object_ext_assocPath(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSAtom *atoms;
    int n;
    JSValue res;
    if (js_path_to_atoms(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &atoms, &n))
        return JS_EXCEPTION;
    res = js_assoc_path(ctx, atoms, 0, n, argc > 1 ? argv[1] : JS_UNDEFINED,
                        argc > 2 ? argv[2] : JS_UNDEFINED);
    js_free_atoms(ctx, atoms, n);
    return res;
}

static JSValue js_dissoc_path(JSContext *ctx, JSAtom *atoms, int i, int n,
                              JSValueConst cur)
{
    JSValue container, child, newchild;
    JSPropertyDescriptor desc;
    int own;
    if (!JS_IsObject(cur))
        return JS_DupValue(ctx, cur);
    if (i == n - 1) {          /* delete the final key from a shallow clone */
        container = js_shallow_clone_container(ctx, cur);
        if (JS_IsException(container)) return container;
        if (JS_DeleteProperty(ctx, container, atoms[i], 0) < 0) {
            JS_FreeValue(ctx, container);
            return JS_EXCEPTION;
        }
        return container;
    }
    own = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(cur), atoms[i]);
    if (own < 0) return JS_EXCEPTION;
    if (own == 0)              /* path does not exist -> return unchanged copy */
        return JS_DupValue(ctx, cur);
    js_free_desc(ctx, &desc);
    child = JS_GetProperty(ctx, cur, atoms[i]);
    if (JS_IsException(child)) return JS_EXCEPTION;
    newchild = js_dissoc_path(ctx, atoms, i + 1, n, child);
    JS_FreeValue(ctx, child);
    if (JS_IsException(newchild)) return JS_EXCEPTION;
    container = js_shallow_clone_container(ctx, cur);
    if (JS_IsException(container)) { JS_FreeValue(ctx, newchild); return JS_EXCEPTION; }
    if (JS_DefinePropertyValue(ctx, container, atoms[i], newchild, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, container);
        return JS_EXCEPTION;
    }
    return container;
}

/* dissocPath(path, obj) -> immutable deep delete. */
static JSValue js_object_ext_dissocPath(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSAtom *atoms;
    int n;
    JSValue res;
    if (js_path_to_atoms(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &atoms, &n))
        return JS_EXCEPTION;
    if (n == 0)
        res = JS_DupValue(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    else
        res = js_dissoc_path(ctx, atoms, 0, n, argc > 1 ? argv[1] : JS_UNDEFINED);
    js_free_atoms(ctx, atoms, n);
    return res;
}

static const JSCFunctionListEntry js_object_ext_funcs[] = {
    /* Sugar type guards + Ramda type/nil */
    JS_CFUNC_MAGIC_DEF("isObject",    1, js_object_ext_istype, OTYPE_OBJECT ),
    JS_CFUNC_DEF("isArray",           1, js_object_ext_isArray ),
    JS_CFUNC_MAGIC_DEF("isBoolean",   1, js_object_ext_istype, OTYPE_BOOLEAN ),
    JS_CFUNC_MAGIC_DEF("isNumber",    1, js_object_ext_istype, OTYPE_NUMBER ),
    JS_CFUNC_MAGIC_DEF("isString",    1, js_object_ext_istype, OTYPE_STRING ),
    JS_CFUNC_MAGIC_DEF("isFunction",  1, js_object_ext_istype, OTYPE_FUNCTION ),
    JS_CFUNC_MAGIC_DEF("isDate",      1, js_object_ext_istype, OTYPE_DATE ),
    JS_CFUNC_MAGIC_DEF("isRegExp",    1, js_object_ext_istype, OTYPE_REGEXP ),
    JS_CFUNC_MAGIC_DEF("isError",     1, js_object_ext_istype, OTYPE_ERROR ),
    JS_CFUNC_MAGIC_DEF("isSet",       1, js_object_ext_istype, OTYPE_SET ),
    JS_CFUNC_MAGIC_DEF("isMap",       1, js_object_ext_istype, OTYPE_MAP ),
    JS_CFUNC_MAGIC_DEF("isArguments", 1, js_object_ext_istype, OTYPE_ARGUMENTS ),
    JS_CFUNC_MAGIC_DEF("isNil",       1, js_object_ext_isNil, 0 ),
    JS_CFUNC_MAGIC_DEF("isNotNil",    1, js_object_ext_isNil, 1 ),
    JS_CFUNC_DEF("type",              1, js_object_ext_type ),
    JS_CFUNC_DEF("defaultTo",         2, js_object_ext_defaultTo ),
    /* Sugar/Ramda shallow structural helpers */
    JS_CFUNC_MAGIC_DEF("size",        1, js_object_ext_size, 0 ),
    JS_CFUNC_MAGIC_DEF("isEmpty",     1, js_object_ext_size, 1 ),
    JS_CFUNC_DEF("invert",            1, js_object_ext_invert ),
    JS_CFUNC_DEF("invertObj",         1, js_object_ext_invert ),
    JS_CFUNC_DEF("objOf",             2, js_object_ext_objOf ),
    JS_CFUNC_DEF("pick",              2, js_object_ext_pick ),
    JS_CFUNC_DEF("omit",              2, js_object_ext_omit ),
    JS_CFUNC_DEF("pickBy",            2, js_object_ext_pickBy ),
    JS_CFUNC_DEF("toPairs",           1, js_object_ext_toPairs ),
    JS_CFUNC_DEF("fromPairs",         1, js_object_ext_fromPairs ),
    JS_CFUNC_DEF("assoc",             3, js_object_ext_assoc ),
    JS_CFUNC_DEF("dissoc",            2, js_object_ext_dissoc ),
    JS_CFUNC_DEF("tap",               2, js_object_ext_tap ),
    /* batch 2: deep clone / equals / merge / path */
    JS_CFUNC_DEF("clone",             1, js_object_ext_clone ),
    JS_CFUNC_DEF("equals",            2, js_object_ext_equals ),
    JS_CFUNC_DEF("identical",         2, js_object_ext_identical ),
    JS_CFUNC_MAGIC_DEF("prop",        2, js_object_ext_prop, 0 ),
    JS_CFUNC_MAGIC_DEF("propOr",      3, js_object_ext_prop, 1 ),
    JS_CFUNC_DEF("props",             2, js_object_ext_props ),
    JS_CFUNC_MAGIC_DEF("path",        2, js_object_ext_path, 0 ),
    JS_CFUNC_MAGIC_DEF("pathOr",      3, js_object_ext_path, 1 ),
    JS_CFUNC_DEF("paths",             2, js_object_ext_paths ),
    JS_CFUNC_DEF("assocPath",         3, js_object_ext_assocPath ),
    JS_CFUNC_DEF("dissocPath",        2, js_object_ext_dissocPath ),
    JS_CFUNC_DEF("hasPath",           2, js_object_ext_hasPath ),
    JS_CFUNC_MAGIC_DEF("has",         2, js_object_ext_has, 0 ),
    JS_CFUNC_MAGIC_DEF("hasIn",       2, js_object_ext_has, 1 ),
    JS_CFUNC_MAGIC_DEF("keysIn",      1, js_object_ext_keysIn, 0 ),
    JS_CFUNC_MAGIC_DEF("valuesIn",    1, js_object_ext_keysIn, 1 ),
    JS_CFUNC_MAGIC_DEF("propEq",      3, js_object_ext_propEq, 0 ),
    JS_CFUNC_MAGIC_DEF("eqProps",     3, js_object_ext_propEq, 1 ),
    JS_CFUNC_DEF("pathEq",            3, js_object_ext_pathEq ),
    JS_CFUNC_MAGIC_DEF("where",       2, js_object_ext_where, 0 ),
    JS_CFUNC_MAGIC_DEF("whereEq",     2, js_object_ext_where, 1 ),
    JS_CFUNC_MAGIC_DEF("mergeRight",  2, js_object_ext_merge, 0 ),
    JS_CFUNC_MAGIC_DEF("merge",       2, js_object_ext_merge, 0 ),
    JS_CFUNC_MAGIC_DEF("mergeLeft",   2, js_object_ext_merge, 1 ),
    JS_CFUNC_MAGIC_DEF("mergeDeepRight", 2, js_object_ext_mergeDeep, 0 ),
    JS_CFUNC_MAGIC_DEF("mergeDeepLeft",  2, js_object_ext_mergeDeep, 1 ),
};

static const JSCFunctionListEntry js_object_funcs[] = {
    JS_CFUNC_DEF("create", 2, js_object_create ),
    JS_CFUNC_MAGIC_DEF("getPrototypeOf", 1, js_object_getPrototypeOf, 0 ),
    JS_CFUNC_DEF("setPrototypeOf", 2, js_object_setPrototypeOf ),
    JS_CFUNC_MAGIC_DEF("defineProperty", 3, js_object_defineProperty, 0 ),
    JS_CFUNC_DEF("defineProperties", 2, js_object_defineProperties ),
    JS_CFUNC_DEF("getOwnPropertyNames", 1, js_object_getOwnPropertyNames ),
    JS_CFUNC_DEF("getOwnPropertySymbols", 1, js_object_getOwnPropertySymbols ),
    JS_CFUNC_MAGIC_DEF("groupBy", 2, js_object_groupBy, 0 ),
    JS_CFUNC_MAGIC_DEF("keys", 1, js_object_keys, JS_ITERATOR_KIND_KEY ),
    JS_CFUNC_MAGIC_DEF("values", 1, js_object_keys, JS_ITERATOR_KIND_VALUE ),
    JS_CFUNC_MAGIC_DEF("entries", 1, js_object_keys, JS_ITERATOR_KIND_KEY_AND_VALUE ),
    JS_CFUNC_MAGIC_DEF("isExtensible", 1, js_object_isExtensible, 0 ),
    JS_CFUNC_MAGIC_DEF("preventExtensions", 1, js_object_preventExtensions, 0 ),
    JS_CFUNC_MAGIC_DEF("getOwnPropertyDescriptor", 2, js_object_getOwnPropertyDescriptor, 0 ),
    JS_CFUNC_DEF("getOwnPropertyDescriptors", 1, js_object_getOwnPropertyDescriptors ),
    JS_CFUNC_DEF("is", 2, js_object_is ),
    JS_CFUNC_DEF("assign", 2, js_object_assign ),
    JS_CFUNC_MAGIC_DEF("seal", 1, js_object_seal, 0 ),
    JS_CFUNC_MAGIC_DEF("freeze", 1, js_object_seal, 1 ),
    JS_CFUNC_MAGIC_DEF("isSealed", 1, js_object_isSealed, 0 ),
    JS_CFUNC_MAGIC_DEF("isFrozen", 1, js_object_isSealed, 1 ),
    JS_CFUNC_DEF("fromEntries", 1, js_object_fromEntries ),
    JS_CFUNC_DEF("hasOwn", 2, js_object_hasOwn ),
};

static const JSCFunctionListEntry js_object_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_object_toString ),
    JS_CFUNC_DEF("toLocaleString", 0, js_object_toLocaleString ),
    JS_CFUNC_DEF("valueOf", 0, js_object_valueOf ),
    JS_CFUNC_DEF("hasOwnProperty", 1, js_object_hasOwnProperty ),
    JS_CFUNC_DEF("isPrototypeOf", 1, js_object_isPrototypeOf ),
    JS_CFUNC_DEF("propertyIsEnumerable", 1, js_object_propertyIsEnumerable ),
    JS_CGETSET_DEF("__proto__", js_object_get___proto__, js_object_set___proto__ ),
    JS_CFUNC_MAGIC_DEF("__defineGetter__", 2, js_object___defineGetter__, 0 ),
    JS_CFUNC_MAGIC_DEF("__defineSetter__", 2, js_object___defineGetter__, 1 ),
    JS_CFUNC_MAGIC_DEF("__lookupGetter__", 1, js_object___lookupGetter__, 0 ),
    JS_CFUNC_MAGIC_DEF("__lookupSetter__", 1, js_object___lookupGetter__, 1 ),
};

/* Function class */

static JSValue js_function_proto(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return JS_UNDEFINED;
}

/* XXX: add a specific eval mode so that Function("}), ({") is rejected */
static JSValue js_function_constructor(JSContext *ctx, JSValueConst new_target,
                                       int argc, JSValueConst *argv, int magic)
{
    JSFunctionKindEnum func_kind = magic;
    int i, n, ret;
    JSValue s, proto, obj = JS_UNDEFINED;
    StringBuffer b_s, *b = &b_s;

    string_buffer_init(ctx, b, 0);
    string_buffer_putc8(b, '(');

    if (func_kind == JS_FUNC_ASYNC || func_kind == JS_FUNC_ASYNC_GENERATOR) {
        string_buffer_puts8(b, "async ");
    }
    string_buffer_puts8(b, "function");

    if (func_kind == JS_FUNC_GENERATOR || func_kind == JS_FUNC_ASYNC_GENERATOR) {
        string_buffer_putc8(b, '*');
    }
    string_buffer_puts8(b, " anonymous(");

    n = argc - 1;
    for(i = 0; i < n; i++) {
        if (i != 0) {
            string_buffer_putc8(b, ',');
        }
        if (string_buffer_concat_value(b, argv[i]))
            goto fail;
    }
    string_buffer_puts8(b, "\n) {\n");
    if (n >= 0) {
        if (string_buffer_concat_value(b, argv[n]))
            goto fail;
    }
    string_buffer_puts8(b, "\n})");
    s = string_buffer_end(b);
    if (JS_IsException(s))
        goto fail1;

    obj = JS_EvalObject(ctx, ctx->global_obj, s, JS_EVAL_TYPE_INDIRECT, -1);
    JS_FreeValue(ctx, s);
    if (JS_IsException(obj))
        goto fail1;
    if (!JS_IsUndefined(new_target)) {
        /* set the prototype */
        proto = JS_GetProperty(ctx, new_target, JS_ATOM_prototype);
        if (JS_IsException(proto))
            goto fail1;
        if (!JS_IsObject(proto)) {
            JSContext *realm;
            JS_FreeValue(ctx, proto);
            realm = JS_GetFunctionRealm(ctx, new_target);
            if (!realm)
                goto fail1;
            proto = JS_DupValue(ctx, realm->class_proto[func_kind_to_class_id[func_kind]]);
        }
        ret = JS_SetPrototypeInternal(ctx, obj, proto, TRUE);
        JS_FreeValue(ctx, proto);
        if (ret < 0)
            goto fail1;
    }
    return obj;

 fail:
    string_buffer_free(b);
 fail1:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static __exception int js_get_length32(JSContext *ctx, uint32_t *pres,
                                       JSValueConst obj)
{
    JSValue len_val;
    len_val = JS_GetProperty(ctx, obj, JS_ATOM_length);
    if (JS_IsException(len_val)) {
        *pres = 0;
        return -1;
    }
    return JS_ToUint32Free(ctx, pres, len_val);
}

static __exception int js_get_length64(JSContext *ctx, int64_t *pres,
                                       JSValueConst obj)
{
    JSValue len_val;
    len_val = JS_GetProperty(ctx, obj, JS_ATOM_length);
    if (JS_IsException(len_val)) {
        *pres = 0;
        return -1;
    }
    return JS_ToLengthFree(ctx, pres, len_val);
}

static void free_arg_list(JSContext *ctx, JSValue *tab, uint32_t len)
{
    uint32_t i;
    for(i = 0; i < len; i++) {
        JS_FreeValue(ctx, tab[i]);
    }
    js_free(ctx, tab);
}

/* XXX: should use ValueArray */
static JSValue *build_arg_list(JSContext *ctx, uint32_t *plen,
                               JSValueConst array_arg)
{
    uint32_t len, i;
    int64_t len64;
    JSValue *tab, ret;
    JSObject *p;

    if (JS_VALUE_GET_TAG(array_arg) != JS_TAG_OBJECT) {
        JS_ThrowTypeError(ctx, "not a object");
        return NULL;
    }
    if (js_get_length64(ctx, &len64, array_arg))
        return NULL;
    if (len64 > JS_MAX_LOCAL_VARS) {
        // XXX: check for stack overflow?
        JS_ThrowRangeError(ctx, "too many arguments in function call (only %d allowed)",
                           JS_MAX_LOCAL_VARS);
        return NULL;
    }
    len = len64;
    /* avoid allocating 0 bytes */
    tab = js_mallocz(ctx, sizeof(tab[0]) * max_uint32(1, len));
    if (!tab)
        return NULL;
    p = JS_VALUE_GET_OBJ(array_arg);
    if ((p->class_id == JS_CLASS_ARRAY || p->class_id == JS_CLASS_ARGUMENTS || p->class_id == JS_CLASS_MAPPED_ARGUMENTS) &&
        p->fast_array &&
        len == p->u.array.count) {
        if (p->class_id == JS_CLASS_MAPPED_ARGUMENTS) {
            for(i = 0; i < len; i++) {
                tab[i] = JS_DupValue(ctx, *p->u.array.u.var_refs[i]->pvalue);
            }
        } else {
            for(i = 0; i < len; i++) {
                tab[i] = JS_DupValue(ctx, p->u.array.u.values[i]);
            }
        }
    } else {
        for(i = 0; i < len; i++) {
            ret = JS_GetPropertyUint32(ctx, array_arg, i);
            if (JS_IsException(ret)) {
                free_arg_list(ctx, tab, i);
                return NULL;
            }
            tab[i] = ret;
        }
    }
    *plen = len;
    return tab;
}

/* magic value: 0 = normal apply, 1 = apply for constructor, 2 =
   Reflect.apply */
static JSValue js_function_apply(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    JSValueConst this_arg, array_arg;
    uint32_t len;
    JSValue *tab, ret;

    if (check_function(ctx, this_val))
        return JS_EXCEPTION;
    this_arg = argv[0];
    array_arg = argv[1];
    if ((JS_VALUE_GET_TAG(array_arg) == JS_TAG_UNDEFINED ||
         JS_VALUE_GET_TAG(array_arg) == JS_TAG_NULL) && magic != 2) {
        return JS_Call(ctx, this_val, this_arg, 0, NULL);
    }
    tab = build_arg_list(ctx, &len, array_arg);
    if (!tab)
        return JS_EXCEPTION;
    if (magic & 1) {
        ret = JS_CallConstructor2(ctx, this_val, this_arg, len, (JSValueConst *)tab);
    } else {
        ret = JS_Call(ctx, this_val, this_arg, len, (JSValueConst *)tab);
    }
    free_arg_list(ctx, tab, len);
    return ret;
}

static JSValue js_function_call(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc <= 0) {
        return JS_Call(ctx, this_val, JS_UNDEFINED, 0, NULL);
    } else {
        return JS_Call(ctx, this_val, argv[0], argc - 1, argv + 1);
    }
}

static JSValue js_function_bind(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSBoundFunction *bf;
    JSValue func_obj, name1, len_val;
    JSObject *p;
    int arg_count, i, ret;

    if (check_function(ctx, this_val))
        return JS_EXCEPTION;

    func_obj = JS_NewObjectProtoClass(ctx, ctx->function_proto,
                                 JS_CLASS_BOUND_FUNCTION);
    if (JS_IsException(func_obj))
        return JS_EXCEPTION;
    p = JS_VALUE_GET_OBJ(func_obj);
    p->is_constructor = JS_IsConstructor(ctx, this_val);
    arg_count = max_int(0, argc - 1);
    bf = js_malloc(ctx, sizeof(*bf) + arg_count * sizeof(JSValue));
    if (!bf)
        goto exception;
    bf->func_obj = JS_DupValue(ctx, this_val);
    bf->this_val = JS_DupValue(ctx, argv[0]);
    bf->argc = arg_count;
    for(i = 0; i < arg_count; i++) {
        bf->argv[i] = JS_DupValue(ctx, argv[i + 1]);
    }
    p->u.bound_function = bf;

    /* XXX: the spec could be simpler by only using GetOwnProperty */
    ret = JS_GetOwnProperty(ctx, NULL, this_val, JS_ATOM_length);
    if (ret < 0)
        goto exception;
    if (!ret) {
        len_val = JS_NewInt32(ctx, 0);
    } else {
        len_val = JS_GetProperty(ctx, this_val, JS_ATOM_length);
        if (JS_IsException(len_val))
            goto exception;
        if (JS_VALUE_GET_TAG(len_val) == JS_TAG_INT) {
            /* most common case */
            int len1 = JS_VALUE_GET_INT(len_val);
            if (len1 <= arg_count)
                len1 = 0;
            else
                len1 -= arg_count;
            len_val = JS_NewInt32(ctx, len1);
        } else if (JS_VALUE_GET_NORM_TAG(len_val) == JS_TAG_FLOAT64) {
            double d = JS_VALUE_GET_FLOAT64(len_val);
            if (isnan(d)) {
                d = 0.0;
            } else {
                d = trunc(d);
                if (d <= (double)arg_count)
                    d = 0.0;
                else
                    d -= (double)arg_count; /* also converts -0 to +0 */
            }
            len_val = JS_NewFloat64(ctx, d);
        } else {
            JS_FreeValue(ctx, len_val);
            len_val = JS_NewInt32(ctx, 0);
        }
    }
    JS_DefinePropertyValue(ctx, func_obj, JS_ATOM_length,
                           len_val, JS_PROP_CONFIGURABLE);

    name1 = JS_GetProperty(ctx, this_val, JS_ATOM_name);
    if (JS_IsException(name1))
        goto exception;
    if (!JS_IsString(name1)) {
        JS_FreeValue(ctx, name1);
        name1 = JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    name1 = JS_ConcatString3(ctx, "bound ", name1, "");
    if (JS_IsException(name1))
        goto exception;
    JS_DefinePropertyValue(ctx, func_obj, JS_ATOM_name, name1,
                           JS_PROP_CONFIGURABLE);
    return func_obj;
 exception:
    JS_FreeValue(ctx, func_obj);
    return JS_EXCEPTION;
}

static JSValue js_function_toString(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSObject *p;
    JSFunctionKindEnum func_kind = JS_FUNC_NORMAL;

    if (check_function(ctx, this_val))
        return JS_EXCEPTION;

    p = JS_VALUE_GET_OBJ(this_val);
    if (js_class_has_bytecode(p->class_id)) {
        JSFunctionBytecode *b = p->u.func.function_bytecode;
        if (b->has_debug && b->debug.source) {
            return JS_NewStringLen(ctx, b->debug.source, b->debug.source_len);
        }
        func_kind = b->func_kind;
    }
    {
        JSValue name;
        const char *pref, *suff;

        switch(func_kind) {
        default:
        case JS_FUNC_NORMAL:
            pref = "function ";
            break;
        case JS_FUNC_GENERATOR:
            pref = "function *";
            break;
        case JS_FUNC_ASYNC:
            pref = "async function ";
            break;
        case JS_FUNC_ASYNC_GENERATOR:
            pref = "async function *";
            break;
        }
        suff = "() {\n    [native code]\n}";
        name = JS_GetProperty(ctx, this_val, JS_ATOM_name);
        if (JS_IsUndefined(name))
            name = JS_AtomToString(ctx, JS_ATOM_empty_string);
        return JS_ConcatString3(ctx, pref, name, suff);
    }
}

static JSValue js_function_hasInstance(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    int ret;
    ret = JS_OrdinaryIsInstanceOf(ctx, argv[0], this_val);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static const JSCFunctionListEntry js_function_proto_funcs[] = {
    JS_CFUNC_DEF("call", 1, js_function_call ),
    JS_CFUNC_MAGIC_DEF("apply", 2, js_function_apply, 0 ),
    JS_CFUNC_DEF("bind", 1, js_function_bind ),
    JS_CFUNC_DEF("toString", 0, js_function_toString ),
    JS_CFUNC_DEF("[Symbol.hasInstance]", 1, js_function_hasInstance ),
    JS_CGETSET_DEF("fileName", js_function_proto_fileName, NULL ),
    JS_CGETSET_MAGIC_DEF("lineNumber", js_function_proto_lineNumber, NULL, 0 ),
    JS_CGETSET_MAGIC_DEF("columnNumber", js_function_proto_lineNumber, NULL, 1 ),
};

/* Error class */

static JSValue iterator_to_array(JSContext *ctx, JSValueConst items)
{
    JSValue iter, next_method = JS_UNDEFINED;
    JSValue v, r = JS_UNDEFINED;
    int64_t k;
    BOOL done;

    iter = JS_GetIterator(ctx, items, FALSE);
    if (JS_IsException(iter))
        goto exception;
    next_method = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next_method))
        goto exception;
    r = JS_NewArray(ctx);
    if (JS_IsException(r))
        goto exception;
    for (k = 0;; k++) {
        v = JS_IteratorNext(ctx, iter, next_method, 0, NULL, &done);
        if (JS_IsException(v))
            goto exception_close;
        if (done)
            break;
        if (JS_DefinePropertyValueInt64(ctx, r, k, v,
                                        JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            goto exception_close;
    }
 done:
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    return r;
 exception_close:
    JS_IteratorClose(ctx, iter, TRUE);
 exception:
    JS_FreeValue(ctx, r);
    r = JS_EXCEPTION;
    goto done;
}

static JSValue js_error_constructor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue obj, msg, proto;
    JSValueConst message, options;
    int arg_index;

    if (JS_IsUndefined(new_target))
        new_target = JS_GetActiveFunction(ctx);
    proto = JS_GetProperty(ctx, new_target, JS_ATOM_prototype);
    if (JS_IsException(proto))
        return proto;
    if (!JS_IsObject(proto)) {
        JSContext *realm;
        JSValueConst proto1;

        JS_FreeValue(ctx, proto);
        realm = JS_GetFunctionRealm(ctx, new_target);
        if (!realm)
            return JS_EXCEPTION;
        if (magic < 0) {
            proto1 = realm->class_proto[JS_CLASS_ERROR];
        } else {
            proto1 = realm->native_error_proto[magic];
        }
        proto = JS_DupValue(ctx, proto1);
    }
    obj = JS_NewObjectProtoClass(ctx, proto, JS_CLASS_ERROR);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    if (magic == JS_SUPPRESSED_ERROR)
        arg_index = 2; /* SuppressedError(error, suppressed, message) */
    else
        arg_index = (magic == JS_AGGREGATE_ERROR);

    message = argv[arg_index++];
    if (!JS_IsUndefined(message)) {
        msg = JS_ToString(ctx, message);
        if (unlikely(JS_IsException(msg)))
            goto exception;
        JS_DefinePropertyValue(ctx, obj, JS_ATOM_message, msg,
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }

    /* SuppressedError has no options argument; its own "error" and
       "suppressed" data properties are defined after "message". */
    if (magic == JS_SUPPRESSED_ERROR) {
        JS_DefinePropertyValue(ctx, obj, JS_ATOM_error,
                               JS_DupValue(ctx, argv[0]),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_DefinePropertyValue(ctx, obj, JS_ATOM_suppressed,
                               JS_DupValue(ctx, argv[1]),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    } else {
        if (arg_index < argc) {
            options = argv[arg_index];
            if (JS_IsObject(options)) {
                int present = JS_HasProperty(ctx, options, JS_ATOM_cause);
                if (present < 0)
                    goto exception;
                if (present) {
                    JSValue cause = JS_GetProperty(ctx, options, JS_ATOM_cause);
                    if (JS_IsException(cause))
                        goto exception;
                    JS_DefinePropertyValue(ctx, obj, JS_ATOM_cause, cause,
                                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
                }
            }
        }

        if (magic == JS_AGGREGATE_ERROR) {
            JSValue error_list = iterator_to_array(ctx, argv[0]);
            if (JS_IsException(error_list))
                goto exception;
            JS_DefinePropertyValue(ctx, obj, JS_ATOM_errors, error_list,
                                   JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        }
    }

    /* skip the Error() function in the backtrace */
    build_backtrace(ctx, obj, NULL, 0, 0, JS_BACKTRACE_FLAG_SKIP_FIRST_LEVEL);
    return obj;
 exception:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_error_toString(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue name, msg;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    name = JS_GetProperty(ctx, this_val, JS_ATOM_name);
    if (JS_IsUndefined(name))
        name = JS_AtomToString(ctx, JS_ATOM_Error);
    else
        name = JS_ToStringFree(ctx, name);
    if (JS_IsException(name))
        return JS_EXCEPTION;

    msg = JS_GetProperty(ctx, this_val, JS_ATOM_message);
    if (JS_IsUndefined(msg))
        msg = JS_AtomToString(ctx, JS_ATOM_empty_string);
    else
        msg = JS_ToStringFree(ctx, msg);
    if (JS_IsException(msg)) {
        JS_FreeValue(ctx, name);
        return JS_EXCEPTION;
    }
    if (!JS_IsEmptyString(name) && !JS_IsEmptyString(msg))
        name = JS_ConcatString3(ctx, "", name, ": ");
    return JS_ConcatString(ctx, name, msg);
}

static const JSCFunctionListEntry js_error_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_error_toString ),
    JS_PROP_STRING_DEF("name", "Error", JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
    JS_PROP_STRING_DEF("message", "", JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

/* 2 entries for each native error class */
/* Note: we use an atom to avoid the autoinit definition which does
   not work in get_prop_string() */
static const JSCFunctionListEntry js_native_error_proto_funcs[] = {
#define DEF(name) \
    JS_PROP_ATOM_DEF("name", name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),\
    JS_PROP_STRING_DEF("message", "", JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
    
    DEF(JS_ATOM_EvalError)
    DEF(JS_ATOM_RangeError)
    DEF(JS_ATOM_ReferenceError)
    DEF(JS_ATOM_SyntaxError)
    DEF(JS_ATOM_TypeError)
    DEF(JS_ATOM_URIError)
    DEF(JS_ATOM_InternalError)
    DEF(JS_ATOM_AggregateError)
    DEF(JS_ATOM_SuppressedError)
#undef DEF
};

static JSValue js_error_isError(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    return JS_NewBool(ctx, JS_IsError(ctx, argv[0]));
}

static const JSCFunctionListEntry js_error_funcs[] = {
    JS_CFUNC_DEF("isError", 1, js_error_isError),
};

/* AggregateError */

/* used by C code. */
static JSValue js_aggregate_error_constructor(JSContext *ctx,
                                              JSValueConst errors)
{
    JSValue obj;

    obj = JS_NewObjectProtoClass(ctx,
                                 ctx->native_error_proto[JS_AGGREGATE_ERROR],
                                 JS_CLASS_ERROR);
    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_errors, JS_DupValue(ctx, errors),
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return obj;
}

/* Array */

static int JS_CopySubArray(JSContext *ctx,
                           JSValueConst obj, int64_t to_pos,
                           int64_t from_pos, int64_t count, int dir)
{
    JSObject *p;
    int64_t i, from, to, len;
    JSValue val;
    int fromPresent;

    p = NULL;
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        p = JS_VALUE_GET_OBJ(obj);
        if (p->class_id != JS_CLASS_ARRAY || !p->fast_array) {
            p = NULL;
        }
    }

    for (i = 0; i < count; ) {
        if (dir < 0) {
            from = from_pos + count - i - 1;
            to = to_pos + count - i - 1;
        } else {
            from = from_pos + i;
            to = to_pos + i;
        }
        if (p && p->fast_array &&
            from >= 0 && from < (len = p->u.array.count)  &&
            to >= 0 && to < len) {
            int64_t l, j;
            /* Fast path for fast arrays. Since we don't look at the
               prototype chain, we can optimize only the cases where
               all the elements are present in the array. */
            l = count - i;
            if (dir < 0) {
                l = min_int64(l, from + 1);
                l = min_int64(l, to + 1);
                for(j = 0; j < l; j++) {
                    set_value(ctx, &p->u.array.u.values[to - j],
                              JS_DupValue(ctx, p->u.array.u.values[from - j]));
                }
            } else {
                l = min_int64(l, len - from);
                l = min_int64(l, len - to);
                for(j = 0; j < l; j++) {
                    set_value(ctx, &p->u.array.u.values[to + j],
                              JS_DupValue(ctx, p->u.array.u.values[from + j]));
                }
            }
            i += l;
        } else {
            fromPresent = JS_TryGetPropertyInt64(ctx, obj, from, &val);
            if (fromPresent < 0)
                goto exception;

            if (fromPresent) {
                if (JS_SetPropertyInt64(ctx, obj, to, val) < 0)
                    goto exception;
            } else {
                if (JS_DeletePropertyInt64(ctx, obj, to, JS_PROP_THROW) < 0)
                    goto exception;
            }
            i++;
        }
    }
    return 0;

 exception:
    return -1;
}

static JSValue js_array_constructor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv)
{
    JSValue obj;
    int i;

    obj = js_create_from_ctor(ctx, new_target, JS_CLASS_ARRAY);
    if (JS_IsException(obj))
        return obj;
    if (argc == 1 && JS_IsNumber(argv[0])) {
        uint32_t len;
        if (JS_ToArrayLengthFree(ctx, &len, JS_DupValue(ctx, argv[0]), TRUE))
            goto fail;
        if (JS_SetProperty(ctx, obj, JS_ATOM_length, JS_NewUint32(ctx, len)) < 0)
            goto fail;
    } else {
        for(i = 0; i < argc; i++) {
            if (JS_SetPropertyUint32(ctx, obj, i, JS_DupValue(ctx, argv[i])) < 0)
                goto fail;
        }
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_from(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    // from(items, mapfn = void 0, this_arg = void 0)
    JSValueConst items = argv[0], mapfn, this_arg;
    JSValueConst args[2];
    JSValue iter, r, v, v2, arrayLike, next_method, enum_obj;
    int64_t k, len;
    int done, mapping;

    mapping = FALSE;
    mapfn = JS_UNDEFINED;
    this_arg = JS_UNDEFINED;
    r = JS_UNDEFINED;
    arrayLike = JS_UNDEFINED;
    iter = JS_UNDEFINED;
    enum_obj = JS_UNDEFINED;
    next_method = JS_UNDEFINED;

    if (argc > 1) {
        mapfn = argv[1];
        if (!JS_IsUndefined(mapfn)) {
            if (check_function(ctx, mapfn))
                goto exception;
            mapping = 1;
            if (argc > 2)
                this_arg = argv[2];
        }
    }
    iter = JS_GetProperty(ctx, items, JS_ATOM_Symbol_iterator);
    if (JS_IsException(iter))
        goto exception;
    if (!JS_IsUndefined(iter) && !JS_IsNull(iter)) {
        if (!JS_IsFunction(ctx, iter)) {
            JS_ThrowTypeError(ctx, "value is not iterable");
            goto exception;
        }
        if (JS_IsConstructor(ctx, this_val))
            r = JS_CallConstructor(ctx, this_val, 0, NULL);
        else
            r = JS_NewArray(ctx);
        if (JS_IsException(r))
            goto exception;
        enum_obj = JS_GetIterator2(ctx, items, iter);
        if (JS_IsException(enum_obj))
            goto exception;
        next_method = JS_GetProperty(ctx, enum_obj, JS_ATOM_next);
        if (JS_IsException(next_method))
            goto exception;
        for (k = 0;; k++) {
            v = JS_IteratorNext(ctx, enum_obj, next_method, 0, NULL, &done);
            if (JS_IsException(v))
                goto exception;
            if (done)
                break;
            if (mapping) {
                args[0] = v;
                args[1] = JS_NewInt32(ctx, k);
                v2 = JS_Call(ctx, mapfn, this_arg, 2, args);
                JS_FreeValue(ctx, v);
                v = v2;
                if (JS_IsException(v))
                    goto exception_close;
            }
            if (JS_DefinePropertyValueInt64(ctx, r, k, v,
                                            JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                goto exception_close;
        }
    } else {
        arrayLike = JS_ToObject(ctx, items);
        if (JS_IsException(arrayLike))
            goto exception;
        if (js_get_length64(ctx, &len, arrayLike) < 0)
            goto exception;
        v = JS_NewInt64(ctx, len);
        args[0] = v;
        if (JS_IsConstructor(ctx, this_val)) {
            r = JS_CallConstructor(ctx, this_val, 1, args);
        } else {
            r = js_array_constructor(ctx, JS_UNDEFINED, 1, args);
        }
        JS_FreeValue(ctx, v);
        if (JS_IsException(r))
            goto exception;
        for(k = 0; k < len; k++) {
            v = JS_GetPropertyInt64(ctx, arrayLike, k);
            if (JS_IsException(v))
                goto exception;
            if (mapping) {
                args[0] = v;
                args[1] = JS_NewInt32(ctx, k);
                v2 = JS_Call(ctx, mapfn, this_arg, 2, args);
                JS_FreeValue(ctx, v);
                v = v2;
                if (JS_IsException(v))
                    goto exception;
            }
            if (JS_DefinePropertyValueInt64(ctx, r, k, v,
                                            JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                goto exception;
        }
    }
    if (JS_SetProperty(ctx, r, JS_ATOM_length, JS_NewUint32(ctx, k)) < 0)
        goto exception;
    goto done;

 exception_close:
    JS_IteratorClose(ctx, enum_obj, TRUE);
 exception:
    JS_FreeValue(ctx, r);
    r = JS_EXCEPTION;
 done:
    JS_FreeValue(ctx, arrayLike);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, enum_obj);
    JS_FreeValue(ctx, next_method);
    return r;
}

/* Array.fromAsync: the spec async closure is implemented as a chain of
   promise reactions, using the same throwaway-capability idiom as the
   'await' implementation of async functions. */

/* phases in the low bits of the reaction magic */
enum {
    FROMASYNC_PHASE_NEXT = 0,       /* argv[0] = awaited result of next() */
    FROMASYNC_PHASE_MAPPED,         /* argv[0] = awaited mapfn result (iterator) */
    FROMASYNC_PHASE_ELEMENT,        /* argv[0] = awaited element (array-like) */
    FROMASYNC_PHASE_MAPPED_ELEMENT, /* argv[0] = awaited mapfn result (array-like) */
    FROMASYNC_PHASE_CLOSE,          /* return() settled: reject stored error */
};
#define FROMASYNC_REJECTED 8

/* func_data slots of the driver reactions (FROMASYNC_PHASE_CLOSE uses only
   [0] = reject and [1] = error) */
enum {
    FROMASYNC_DATA_RESOLVE = 0,
    FROMASYNC_DATA_REJECT,
    FROMASYNC_DATA_ARRAY,
    FROMASYNC_DATA_ITER,    /* iterator, or the array-like object */
    FROMASYNC_DATA_NEXT,    /* next method, or the array-like length */
    FROMASYNC_DATA_K,
    FROMASYNC_DATA_MAPFN,   /* undefined if no mapping */
    FROMASYNC_DATA_THIS_ARG,
    FROMASYNC_DATA_COUNT,
};

static JSValue js_array_fromAsync_resume(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv,
                                         int magic, JSValue *func_data);

/* Await(value): resolve it and schedule the 'phase' continuation.
   Consumes 'value'. Return -1 if an exception is pending. */
static int js_array_fromAsync_await(JSContext *ctx, JSValue value, int phase,
                                    JSValueConst *func_data, int data_len)
{
    JSValue promise, on_settled[2];
    JSValueConst throwaway[2] = { JS_UNDEFINED, JS_UNDEFINED };
    int i, res;

    promise = js_promise_resolve(ctx, ctx->promise_ctor, 1,
                                 (JSValueConst *)&value, 0);
    JS_FreeValue(ctx, value);
    if (JS_IsException(promise))
        return -1;
    for(i = 0; i < 2; i++) {
        on_settled[i] = JS_NewCFunctionData(ctx, js_array_fromAsync_resume, 1,
                                            phase | (i ? FROMASYNC_REJECTED : 0),
                                            data_len, func_data);
        if (JS_IsException(on_settled[i])) {
            if (i)
                JS_FreeValue(ctx, on_settled[0]);
            JS_FreeValue(ctx, promise);
            return -1;
        }
    }
    res = perform_promise_then(ctx, promise, (JSValueConst *)on_settled,
                               throwaway);
    JS_FreeValue(ctx, on_settled[0]);
    JS_FreeValue(ctx, on_settled[1]);
    JS_FreeValue(ctx, promise);
    return res;
}

/* call 'reject' with the pending exception */
static void js_array_fromAsync_reject(JSContext *ctx, JSValueConst reject)
{
    JSValue error, res;

    error = JS_GetException(ctx);
    res = JS_Call(ctx, reject, JS_UNDEFINED, 1, (JSValueConst *)&error);
    if (JS_IsException(res))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, error);
}

/* AsyncIteratorClose for an abrupt completion: call iter.return(), await
   its result if any, then reject with the pending exception (the original
   error always wins over errors from return()). */
static void js_array_fromAsync_close_reject(JSContext *ctx, JSValueConst iter,
                                            JSValueConst reject)
{
    JSValue error, method, res;
    JSValueConst data[2];

    error = JS_GetException(ctx);
    method = JS_GetProperty(ctx, iter, JS_ATOM_return);
    if (JS_IsException(method)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        goto reject_now;
    }
    if (JS_IsUndefined(method) || JS_IsNull(method)) {
        JS_FreeValue(ctx, method);
        goto reject_now;
    }
    res = JS_Call(ctx, method, iter, 0, NULL);
    JS_FreeValue(ctx, method);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        goto reject_now;
    }
    data[0] = reject;
    data[1] = (JSValueConst)error;
    if (js_array_fromAsync_await(ctx, res, FROMASYNC_PHASE_CLOSE, data, 2)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        goto reject_now;
    }
    JS_FreeValue(ctx, error);
    return;
 reject_now:
    res = JS_Call(ctx, reject, JS_UNDEFINED, 1, (JSValueConst *)&error);
    if (JS_IsException(res))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, error);
}

/* one iterator-path step: guard k, call next() and schedule the
   continuation; all failures settle the capability */
static void js_array_fromAsync_step(JSContext *ctx, JSValueConst *func_data)
{
    JSValue next_result;
    int64_t k;

    JS_ToInt64(ctx, &k, func_data[FROMASYNC_DATA_K]);
    if (k >= MAX_SAFE_INTEGER) {
        JS_ThrowTypeError(ctx, "too many elements");
        js_array_fromAsync_close_reject(ctx, func_data[FROMASYNC_DATA_ITER],
                                        func_data[FROMASYNC_DATA_REJECT]);
        return;
    }
    next_result = JS_Call(ctx, func_data[FROMASYNC_DATA_NEXT],
                          func_data[FROMASYNC_DATA_ITER], 0, NULL);
    if (JS_IsException(next_result))
        goto reject;
    if (js_array_fromAsync_await(ctx, next_result, FROMASYNC_PHASE_NEXT,
                                 func_data, FROMASYNC_DATA_COUNT))
        goto reject;
    return;
 reject:
    js_array_fromAsync_reject(ctx, func_data[FROMASYNC_DATA_REJECT]);
}

/* one array-like-path step: fetch element k, or finish when k == len */
static void js_array_fromAsync_step_arraylike(JSContext *ctx,
                                              JSValueConst *func_data)
{
    JSValue v, res;
    int64_t k, len;

    JS_ToInt64(ctx, &k, func_data[FROMASYNC_DATA_K]);
    JS_ToInt64(ctx, &len, func_data[FROMASYNC_DATA_NEXT]);
    if (k >= len) {
        if (JS_SetProperty(ctx, func_data[FROMASYNC_DATA_ARRAY],
                           JS_ATOM_length, JS_NewInt64(ctx, len)) < 0)
            goto reject;
        res = JS_Call(ctx, func_data[FROMASYNC_DATA_RESOLVE], JS_UNDEFINED,
                      1, &func_data[FROMASYNC_DATA_ARRAY]);
        if (JS_IsException(res))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, res);
        return;
    }
    v = JS_GetPropertyInt64(ctx, func_data[FROMASYNC_DATA_ITER], k);
    if (JS_IsException(v))
        goto reject;
    if (js_array_fromAsync_await(ctx, v, FROMASYNC_PHASE_ELEMENT,
                                 func_data, FROMASYNC_DATA_COUNT))
        goto reject;
    return;
 reject:
    js_array_fromAsync_reject(ctx, func_data[FROMASYNC_DATA_REJECT]);
}

/* define A[k] = value (consumed), advance k and run the next step */
static void js_array_fromAsync_define(JSContext *ctx, JSValue value,
                                      JSValueConst *func_data, BOOL arraylike)
{
    JSValueConst data[FROMASYNC_DATA_COUNT];
    int64_t k;
    int i;

    JS_ToInt64(ctx, &k, func_data[FROMASYNC_DATA_K]);
    if (JS_DefinePropertyValueInt64(ctx, func_data[FROMASYNC_DATA_ARRAY], k,
                                    value, JS_PROP_C_W_E | JS_PROP_THROW) < 0) {
        if (arraylike)
            js_array_fromAsync_reject(ctx, func_data[FROMASYNC_DATA_REJECT]);
        else
            js_array_fromAsync_close_reject(ctx,
                                            func_data[FROMASYNC_DATA_ITER],
                                            func_data[FROMASYNC_DATA_REJECT]);
        return;
    }
    for(i = 0; i < FROMASYNC_DATA_COUNT; i++)
        data[i] = func_data[i];
    data[FROMASYNC_DATA_K] = (JSValueConst)JS_NewInt64(ctx, k + 1);
    if (arraylike)
        js_array_fromAsync_step_arraylike(ctx, data);
    else
        js_array_fromAsync_step(ctx, data);
}

static JSValue js_array_fromAsync_resume(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv,
                                         int magic, JSValue *func_data)
{
    JSValueConst v = argv[0];
    JSValue value, mapped, res, done_val;
    int phase = magic & 7;
    int done;

    if (phase == FROMASYNC_PHASE_CLOSE) {
        /* func_data: [0] = reject, [1] = original error */
        res = JS_Call(ctx, func_data[0], JS_UNDEFINED, 1,
                      (JSValueConst *)&func_data[1]);
        if (JS_IsException(res))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, res);
        return JS_UNDEFINED;
    }

    if (magic & FROMASYNC_REJECTED) {
        if (phase == FROMASYNC_PHASE_MAPPED) {
            /* IfAbruptCloseAsyncIterator on Await(mappedValue) */
            JS_Throw(ctx, JS_DupValue(ctx, v));
            js_array_fromAsync_close_reject(ctx,
                                            func_data[FROMASYNC_DATA_ITER],
                                            func_data[FROMASYNC_DATA_REJECT]);
        } else {
            res = JS_Call(ctx, func_data[FROMASYNC_DATA_REJECT], JS_UNDEFINED,
                          1, &v);
            if (JS_IsException(res))
                JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, res);
        }
        return JS_UNDEFINED;
    }

    switch(phase) {
    case FROMASYNC_PHASE_NEXT:
        if (!JS_IsObject(v)) {
            JS_ThrowTypeError(ctx, "iterator must return an object");
            goto reject;
        }
        done_val = JS_GetProperty(ctx, v, JS_ATOM_done);
        if (JS_IsException(done_val))
            goto reject;
        done = JS_ToBoolFree(ctx, done_val);
        if (done) {
            int64_t k;
            JS_ToInt64(ctx, &k, func_data[FROMASYNC_DATA_K]);
            if (JS_SetProperty(ctx, func_data[FROMASYNC_DATA_ARRAY],
                               JS_ATOM_length, JS_NewInt64(ctx, k)) < 0)
                goto reject;
            res = JS_Call(ctx, func_data[FROMASYNC_DATA_RESOLVE], JS_UNDEFINED,
                          1, (JSValueConst *)&func_data[FROMASYNC_DATA_ARRAY]);
            if (JS_IsException(res))
                JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, res);
            break;
        }
        value = JS_GetProperty(ctx, v, JS_ATOM_value);
        if (JS_IsException(value))
            goto reject;
        if (!JS_IsUndefined(func_data[FROMASYNC_DATA_MAPFN])) {
            JSValueConst args[2];
            args[0] = (JSValueConst)value;
            args[1] = func_data[FROMASYNC_DATA_K];
            mapped = JS_Call(ctx, func_data[FROMASYNC_DATA_MAPFN],
                             func_data[FROMASYNC_DATA_THIS_ARG], 2, args);
            JS_FreeValue(ctx, value);
            if (JS_IsException(mapped))
                goto reject_close;
            if (js_array_fromAsync_await(ctx, mapped, FROMASYNC_PHASE_MAPPED,
                                         (JSValueConst *)func_data,
                                         FROMASYNC_DATA_COUNT))
                goto reject_close;
        } else {
            js_array_fromAsync_define(ctx, value, (JSValueConst *)func_data,
                                      FALSE);
        }
        break;
    case FROMASYNC_PHASE_MAPPED:
        js_array_fromAsync_define(ctx, JS_DupValue(ctx, v),
                                  (JSValueConst *)func_data, FALSE);
        break;
    case FROMASYNC_PHASE_ELEMENT:
        if (!JS_IsUndefined(func_data[FROMASYNC_DATA_MAPFN])) {
            JSValueConst args[2];
            args[0] = v;
            args[1] = func_data[FROMASYNC_DATA_K];
            mapped = JS_Call(ctx, func_data[FROMASYNC_DATA_MAPFN],
                             func_data[FROMASYNC_DATA_THIS_ARG], 2, args);
            if (JS_IsException(mapped))
                goto reject;
            if (js_array_fromAsync_await(ctx, mapped,
                                         FROMASYNC_PHASE_MAPPED_ELEMENT,
                                         (JSValueConst *)func_data,
                                         FROMASYNC_DATA_COUNT))
                goto reject;
        } else {
            js_array_fromAsync_define(ctx, JS_DupValue(ctx, v),
                                      (JSValueConst *)func_data, TRUE);
        }
        break;
    case FROMASYNC_PHASE_MAPPED_ELEMENT:
        js_array_fromAsync_define(ctx, JS_DupValue(ctx, v),
                                  (JSValueConst *)func_data, TRUE);
        break;
    }
    return JS_UNDEFINED;
 reject_close:
    js_array_fromAsync_close_reject(ctx, func_data[FROMASYNC_DATA_ITER],
                                    func_data[FROMASYNC_DATA_REJECT]);
    return JS_UNDEFINED;
 reject:
    js_array_fromAsync_reject(ctx, func_data[FROMASYNC_DATA_REJECT]);
    return JS_UNDEFINED;
}

static JSValue js_array_fromAsync(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    // fromAsync(asyncItems, mapfn = void 0, thisArg = void 0)
    JSValueConst items = argv[0], mapfn, this_arg;
    JSValueConst data[FROMASYNC_DATA_COUNT];
    JSValue promise, resolving_funcs[2], method, iter, next_method, r, v;

    mapfn = JS_UNDEFINED;
    this_arg = JS_UNDEFINED;
    if (argc > 1) {
        mapfn = argv[1];
        if (argc > 2)
            this_arg = argv[2];
    }
    method = JS_UNDEFINED;
    iter = JS_UNDEFINED;
    next_method = JS_UNDEFINED;
    r = JS_UNDEFINED;

    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return promise;
    /* from here on, errors reject the promise instead of throwing */
    if (!JS_IsUndefined(mapfn) && !JS_IsFunction(ctx, mapfn)) {
        JS_ThrowTypeError(ctx, "not a function");
        goto reject;
    }
    method = JS_GetProperty(ctx, items, JS_ATOM_Symbol_asyncIterator);
    if (JS_IsException(method))
        goto reject;
    if (JS_IsUndefined(method) || JS_IsNull(method)) {
        JS_FreeValue(ctx, method);
        method = JS_GetProperty(ctx, items, JS_ATOM_Symbol_iterator);
        if (JS_IsException(method))
            goto reject;
        if (JS_IsUndefined(method) || JS_IsNull(method)) {
            /* array-like path */
            int64_t len;

            JS_FreeValue(ctx, method);
            method = JS_UNDEFINED;
            iter = JS_ToObject(ctx, items);
            if (JS_IsException(iter))
                goto reject;
            if (js_get_length64(ctx, &len, iter))
                goto reject;
            v = JS_NewInt64(ctx, len);
            if (JS_IsConstructor(ctx, this_val))
                r = JS_CallConstructor(ctx, this_val, 1, (JSValueConst *)&v);
            else
                r = js_array_constructor(ctx, JS_UNDEFINED, 1,
                                         (JSValueConst *)&v);
            JS_FreeValue(ctx, v);
            if (JS_IsException(r))
                goto reject;
            data[FROMASYNC_DATA_RESOLVE] = (JSValueConst)resolving_funcs[0];
            data[FROMASYNC_DATA_REJECT] = (JSValueConst)resolving_funcs[1];
            data[FROMASYNC_DATA_ARRAY] = (JSValueConst)r;
            data[FROMASYNC_DATA_ITER] = (JSValueConst)iter;
            data[FROMASYNC_DATA_NEXT] = (JSValueConst)JS_NewInt64(ctx, len);
            data[FROMASYNC_DATA_K] = (JSValueConst)JS_NewInt32(ctx, 0);
            data[FROMASYNC_DATA_MAPFN] = mapfn;
            data[FROMASYNC_DATA_THIS_ARG] = this_arg;
            js_array_fromAsync_step_arraylike(ctx, data);
            goto done;
        }
        if (!JS_IsFunction(ctx, method)) {
            JS_ThrowTypeError(ctx, "value is not iterable");
            goto reject;
        }
        v = JS_GetIterator2(ctx, items, method);
        if (JS_IsException(v))
            goto reject;
        iter = JS_CreateAsyncFromSyncIterator(ctx, v);
        JS_FreeValue(ctx, v);
        if (JS_IsException(iter))
            goto reject;
    } else {
        if (!JS_IsFunction(ctx, method)) {
            JS_ThrowTypeError(ctx, "value is not async iterable");
            goto reject;
        }
        iter = JS_GetIterator2(ctx, items, method);
        if (JS_IsException(iter))
            goto reject;
    }
    JS_FreeValue(ctx, method);
    method = JS_UNDEFINED;
    next_method = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next_method))
        goto reject;
    if (JS_IsConstructor(ctx, this_val))
        r = JS_CallConstructor(ctx, this_val, 0, NULL);
    else
        r = JS_NewArray(ctx);
    /* per spec 'Let A be ? Construct(C)' is a plain ReturnIfAbrupt, not
       IfAbruptCloseAsyncIterator: reject without closing the iterator */
    if (JS_IsException(r))
        goto reject;
    data[FROMASYNC_DATA_RESOLVE] = (JSValueConst)resolving_funcs[0];
    data[FROMASYNC_DATA_REJECT] = (JSValueConst)resolving_funcs[1];
    data[FROMASYNC_DATA_ARRAY] = (JSValueConst)r;
    data[FROMASYNC_DATA_ITER] = (JSValueConst)iter;
    data[FROMASYNC_DATA_NEXT] = (JSValueConst)next_method;
    data[FROMASYNC_DATA_K] = (JSValueConst)JS_NewInt32(ctx, 0);
    data[FROMASYNC_DATA_MAPFN] = mapfn;
    data[FROMASYNC_DATA_THIS_ARG] = this_arg;
    js_array_fromAsync_step(ctx, data);
    goto done;
 reject:
    js_array_fromAsync_reject(ctx, resolving_funcs[1]);
 done:
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static JSValue js_array_of(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    JSValue obj, args[1];
    int i;

    if (JS_IsConstructor(ctx, this_val)) {
        args[0] = JS_NewInt32(ctx, argc);
        obj = JS_CallConstructor(ctx, this_val, 1, (JSValueConst *)args);
    } else {
        obj = JS_NewArray(ctx);
    }
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    for(i = 0; i < argc; i++) {
        if (JS_CreateDataPropertyUint32(ctx, obj, i, JS_DupValue(ctx, argv[i]),
                                        JS_PROP_THROW) < 0) {
            goto fail;
        }
    }
    if (JS_SetProperty(ctx, obj, JS_ATOM_length, JS_NewUint32(ctx, argc)) < 0) {
    fail:
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue js_array_isArray(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    int ret;
    ret = JS_IsArray(ctx, argv[0]);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_get_this(JSContext *ctx,
                           JSValueConst this_val)
{
    return JS_DupValue(ctx, this_val);
}

/* XXX: optimize */
static JSValue JS_ArraySpeciesGetCtor(JSContext *ctx, JSValueConst obj)
{
    JSValue ctor, species;
    int res;
    JSContext *realm;

    res = JS_IsArray(ctx, obj);
    if (res < 0)
        return JS_EXCEPTION;
    if (!res)
        return JS_UNDEFINED;
    ctor = JS_GetProperty(ctx, obj, JS_ATOM_constructor);
    if (JS_IsException(ctor))
        return ctor;
    if (JS_IsConstructor(ctx, ctor)) {
        /* legacy web compatibility */
        realm = JS_GetFunctionRealm(ctx, ctor);
        if (!realm) {
            JS_FreeValue(ctx, ctor);
            return JS_EXCEPTION;
        }
        if (realm != ctx &&
            js_same_value(ctx, ctor, realm->array_ctor)) {
            JS_FreeValue(ctx, ctor);
            ctor = JS_UNDEFINED;
        }
    }
    if (JS_IsObject(ctor)) {
        species = JS_GetProperty(ctx, ctor, JS_ATOM_Symbol_species);
        JS_FreeValue(ctx, ctor);
        if (JS_IsException(species))
            return species;
        ctor = species;
        if (JS_IsNull(ctor))
            ctor = JS_UNDEFINED;
    }
    if (!JS_IsUndefined(ctor) &&
        js_same_value(ctx, ctor, ctx->array_ctor)) {
        JS_FreeValue(ctx, ctor);
        ctor = JS_UNDEFINED;
    }
    return ctor;
}

static JSValue JS_ArrayCreateFromCtor(JSContext *ctx, JSValueConst ctor, int64_t len)
{
    JSValue len_val, ret;

    len_val = JS_NewInt64(ctx, len);
    if (JS_IsUndefined(ctor)) {
        ret = js_array_constructor(ctx, JS_UNDEFINED, 1, (JSValueConst *)&len_val);
    } else {
        ret = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&len_val);
    }
    JS_FreeValue(ctx, len_val);
    return ret;
}

/* len must be >= 0 */
static JSValue JS_ArraySpeciesCreate(JSContext *ctx, JSValueConst obj, int64_t len)
{
    JSValue ctor, ret;

    ctor = JS_ArraySpeciesGetCtor(ctx, obj);
    if (JS_IsException(ctor))
        return ctor;
    ret = JS_ArrayCreateFromCtor(ctx, ctor, len);
    JS_FreeValue(ctx, ctor);
    return ret;
}

static const JSCFunctionListEntry js_array_funcs[] = {
    JS_CFUNC_DEF("isArray", 1, js_array_isArray ),
    JS_CFUNC_DEF("from", 1, js_array_from ),
    JS_CFUNC_DEF("fromAsync", 1, js_array_fromAsync ),
    JS_CFUNC_DEF("of", 0, js_array_of ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static int JS_isConcatSpreadable(JSContext *ctx, JSValueConst obj)
{
    JSValue val;

    if (!JS_IsObject(obj))
        return FALSE;
    val = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_isConcatSpreadable);
    if (JS_IsException(val))
        return -1;
    if (!JS_IsUndefined(val))
        return JS_ToBoolFree(ctx, val);
    return JS_IsArray(ctx, obj);
}

static JSValue js_array_at(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    JSValue obj, ret;
    int64_t len, idx;
    JSValue *arrp;
    uint32_t count;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    if (JS_ToInt64Sat(ctx, &idx, argv[0]))
        goto exception;

    if (idx < 0)
        idx = len + idx;
    if (idx < 0 || idx >= len) {
        ret = JS_UNDEFINED;
    } else if (js_get_fast_array(ctx, obj, &arrp, &count) && idx < count) {
        ret = JS_DupValue(ctx, arrp[idx]);
    } else {
        int present = JS_TryGetPropertyInt64(ctx, obj, idx, &ret);
        if (present < 0)
            goto exception;
        if (!present)
            ret = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, obj);
    return ret;
 exception:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_with(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValue arr, obj, ret, *arrp, *pval;
    JSObject *p;
    int64_t i, len, idx;
    uint32_t count32;

    ret = JS_EXCEPTION;
    arr = JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    if (JS_ToInt64Sat(ctx, &idx, argv[0]))
        goto exception;

    if (idx < 0)
        idx = len + idx;

    if (idx < 0 || idx >= len) {
        JS_ThrowRangeError(ctx, "invalid array index: %" PRId64, idx);
        goto exception;
    }

    arr = js_allocate_fast_array(ctx, len);
    if (JS_IsException(arr))
        goto exception;

    p = JS_VALUE_GET_OBJ(arr);
    i = 0;
    pval = p->u.array.u.values;
    if (js_get_fast_array(ctx, obj, &arrp, &count32) && count32 == len) {
        for (; i < idx; i++, pval++)
            *pval = JS_DupValue(ctx, arrp[i]);
        *pval = JS_DupValue(ctx, argv[1]);
        for (i++, pval++; i < len; i++, pval++)
            *pval = JS_DupValue(ctx, arrp[i]);
    } else {
        for (; i < idx; i++, pval++)
            if (-1 == JS_TryGetPropertyInt64(ctx, obj, i, pval))
                goto exception;
        *pval = JS_DupValue(ctx, argv[1]);
        for (i++, pval++; i < len; i++, pval++) {
            if (-1 == JS_TryGetPropertyInt64(ctx, obj, i, pval))
                goto exception;
        }
    }

    ret = arr;
    arr = JS_UNDEFINED;

exception:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, obj);
    return ret;
}

static JSValue js_array_concat(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JSValue obj, arr, val;
    JSValueConst e;
    int64_t len, k, n;
    int i, res;

    arr = JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (JS_IsException(obj))
        goto exception;

    arr = JS_ArraySpeciesCreate(ctx, obj, 0);
    if (JS_IsException(arr))
        goto exception;
    n = 0;
    for (i = -1; i < argc; i++) {
        if (i < 0)
            e = obj;
        else
            e = argv[i];

        res = JS_isConcatSpreadable(ctx, e);
        if (res < 0)
            goto exception;
        if (res) {
            if (js_get_length64(ctx, &len, e))
                goto exception;
            if (n + len > MAX_SAFE_INTEGER) {
                JS_ThrowTypeError(ctx, "Array loo long");
                goto exception;
            }
            for (k = 0; k < len; k++, n++) {
                res = JS_TryGetPropertyInt64(ctx, e, k, &val);
                if (res < 0)
                    goto exception;
                if (res) {
                    if (JS_DefinePropertyValueInt64(ctx, arr, n, val,
                                                    JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                        goto exception;
                }
            }
        } else {
            if (n >= MAX_SAFE_INTEGER) {
                JS_ThrowTypeError(ctx, "Array loo long");
                goto exception;
            }
            if (JS_DefinePropertyValueInt64(ctx, arr, n, JS_DupValue(ctx, e),
                                            JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                goto exception;
            n++;
        }
    }
    if (JS_SetProperty(ctx, arr, JS_ATOM_length, JS_NewInt64(ctx, n)) < 0)
        goto exception;

    JS_FreeValue(ctx, obj);
    return arr;

exception:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

#define special_every    0
#define special_some     1
#define special_forEach  2
#define special_map      3
#define special_filter   4
#define special_TA       8

static JSValue js_typed_array___speciesCreate(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv);

static JSValue js_array_every(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int special)
{
    JSValue obj, val, index_val, res, ret;
    JSValueConst args[3];
    JSValueConst func, this_arg;
    int64_t len, k, n;
    int present;

    ret = JS_UNDEFINED;
    val = JS_UNDEFINED;
    if (special & special_TA) {
        obj = JS_DupValue(ctx, this_val);
        len = js_typed_array_get_length_unsafe(ctx, obj);
        if (len < 0)
            goto exception;
    } else {
        obj = JS_ToObject(ctx, this_val);
        if (js_get_length64(ctx, &len, obj))
            goto exception;
    }
    func = argv[0];
    this_arg = JS_UNDEFINED;
    if (argc > 1)
        this_arg = argv[1];

    if (check_function(ctx, func))
        goto exception;

    switch (special) {
    case special_every:
    case special_every | special_TA:
        ret = JS_TRUE;
        break;
    case special_some:
    case special_some | special_TA:
        ret = JS_FALSE;
        break;
    case special_map:
        ret = JS_ArraySpeciesCreate(ctx, obj, len);
        if (JS_IsException(ret))
            goto exception;
        break;
    case special_filter:
        ret = JS_ArraySpeciesCreate(ctx, obj, 0);
        if (JS_IsException(ret))
            goto exception;
        break;
    case special_map | special_TA:
        args[0] = obj;
        args[1] = JS_NewInt32(ctx, len);
        ret = js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 2, args);
        if (JS_IsException(ret))
            goto exception;
        break;
    case special_filter | special_TA:
        ret = JS_NewArray(ctx);
        if (JS_IsException(ret))
            goto exception;
        break;
    }
    n = 0;

    for(k = 0; k < len; k++) {
        if (special & special_TA) {
            val = JS_GetPropertyInt64(ctx, obj, k);
            if (JS_IsException(val))
                goto exception;
            present = TRUE;
        } else {
            present = JS_TryGetPropertyInt64(ctx, obj, k, &val);
            if (present < 0)
                goto exception;
        }
        if (present) {
            index_val = JS_NewInt64(ctx, k);
            if (JS_IsException(index_val))
                goto exception;
            args[0] = val;
            args[1] = index_val;
            args[2] = obj;
            res = JS_Call(ctx, func, this_arg, 3, args);
            JS_FreeValue(ctx, index_val);
            if (JS_IsException(res))
                goto exception;
            switch (special) {
            case special_every:
            case special_every | special_TA:
                if (!JS_ToBoolFree(ctx, res)) {
                    ret = JS_FALSE;
                    goto done;
                }
                break;
            case special_some:
            case special_some | special_TA:
                if (JS_ToBoolFree(ctx, res)) {
                    ret = JS_TRUE;
                    goto done;
                }
                break;
            case special_map:
                if (JS_DefinePropertyValueInt64(ctx, ret, k, res,
                                                JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                    goto exception;
                break;
            case special_map | special_TA:
                if (JS_SetPropertyValue(ctx, ret, JS_NewInt32(ctx, k), res, JS_PROP_THROW) < 0)
                    goto exception;
                break;
            case special_filter:
            case special_filter | special_TA:
                if (JS_ToBoolFree(ctx, res)) {
                    if (JS_DefinePropertyValueInt64(ctx, ret, n++, JS_DupValue(ctx, val),
                                                    JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                        goto exception;
                }
                break;
            default:
                JS_FreeValue(ctx, res);
                break;
            }
            JS_FreeValue(ctx, val);
            val = JS_UNDEFINED;
        }
    }
done:
    if (special == (special_filter | special_TA)) {
        JSValue arr;
        args[0] = obj;
        args[1] = JS_NewInt32(ctx, n);
        arr = js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 2, args);
        if (JS_IsException(arr))
            goto exception;
        args[0] = ret;
        res = JS_Invoke(ctx, arr, JS_ATOM_set, 1, args);
        if (check_exception_free(ctx, res)) {
            JS_FreeValue(ctx, arr);
            goto exception;
        }
        JS_FreeValue(ctx, ret);
        ret = arr;
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, obj);
    return ret;

exception:
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

#define special_reduce       0
#define special_reduceRight  1

static JSValue js_array_reduce(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int special)
{
    JSValue obj, val, index_val, acc, acc1;
    JSValueConst args[4];
    JSValueConst func;
    int64_t len, k, k1;
    int present;

    acc = JS_UNDEFINED;
    val = JS_UNDEFINED;
    if (special & special_TA) {
        obj = JS_DupValue(ctx, this_val);
        len = js_typed_array_get_length_unsafe(ctx, obj);
        if (len < 0)
            goto exception;
    } else {
        obj = JS_ToObject(ctx, this_val);
        if (js_get_length64(ctx, &len, obj))
            goto exception;
    }
    func = argv[0];

    if (check_function(ctx, func))
        goto exception;

    k = 0;
    if (argc > 1) {
        acc = JS_DupValue(ctx, argv[1]);
    } else {
        for(;;) {
            if (k >= len) {
                JS_ThrowTypeError(ctx, "empty array");
                goto exception;
            }
            k1 = (special & special_reduceRight) ? len - k - 1 : k;
            k++;
            if (special & special_TA) {
                acc = JS_GetPropertyInt64(ctx, obj, k1);
                if (JS_IsException(acc))
                    goto exception;
                break;
            } else {
                present = JS_TryGetPropertyInt64(ctx, obj, k1, &acc);
                if (present < 0)
                    goto exception;
                if (present)
                    break;
            }
        }
    }
    for (; k < len; k++) {
        k1 = (special & special_reduceRight) ? len - k - 1 : k;
        if (special & special_TA) {
            val = JS_GetPropertyInt64(ctx, obj, k1);
            if (JS_IsException(val))
                goto exception;
            present = TRUE;
        } else {
            present = JS_TryGetPropertyInt64(ctx, obj, k1, &val);
            if (present < 0)
                goto exception;
        }
        if (present) {
            index_val = JS_NewInt64(ctx, k1);
            if (JS_IsException(index_val))
                goto exception;
            args[0] = acc;
            args[1] = val;
            args[2] = index_val;
            args[3] = obj;
            acc1 = JS_Call(ctx, func, JS_UNDEFINED, 4, args);
            JS_FreeValue(ctx, index_val);
            JS_FreeValue(ctx, val);
            val = JS_UNDEFINED;
            if (JS_IsException(acc1))
                goto exception;
            JS_FreeValue(ctx, acc);
            acc = acc1;
        }
    }
    JS_FreeValue(ctx, obj);
    return acc;

exception:
    JS_FreeValue(ctx, acc);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_fill(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValue obj;
    int64_t len, start, end;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    start = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64Clamp(ctx, &start, argv[1], 0, len, len))
            goto exception;
    }

    end = len;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt64Clamp(ctx, &end, argv[2], 0, len, len))
            goto exception;
    }

    /* XXX: should special case fast arrays */
    while (start < end) {
        if (JS_SetPropertyInt64(ctx, obj, start,
                                JS_DupValue(ctx, argv[0])) < 0)
            goto exception;
        start++;
    }
    return obj;

 exception:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_includes(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, val;
    int64_t len, n;
    JSValue *arrp;
    uint32_t count;
    int res;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    res = FALSE;
    if (len > 0) {
        n = 0;
        if (argc > 1) {
            if (JS_ToInt64Clamp(ctx, &n, argv[1], 0, len, len))
                goto exception;
        }
        if (js_get_fast_array(ctx, obj, &arrp, &count)) {
            for (; n < count; n++) {
                if (js_strict_eq2(ctx, argv[0], arrp[n],
                                  JS_EQ_SAME_VALUE_ZERO)) {
                    res = TRUE;
                    goto done;
                }
            }
        }
        for (; n < len; n++) {
            val = JS_GetPropertyInt64(ctx, obj, n);
            if (JS_IsException(val))
                goto exception;
            if (js_strict_eq2(ctx, argv[0], val,
                              JS_EQ_SAME_VALUE_ZERO)) {
                JS_FreeValue(ctx, val);
                res = TRUE;
                break;
            }
            JS_FreeValue(ctx, val);
        }
    }
 done:
    JS_FreeValue(ctx, obj);
    return JS_NewBool(ctx, res);

 exception:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_indexOf(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue obj, val;
    int64_t len, n, res;
    JSValue *arrp;
    uint32_t count;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    res = -1;
    if (len > 0) {
        n = 0;
        if (argc > 1) {
            if (JS_ToInt64Clamp(ctx, &n, argv[1], 0, len, len))
                goto exception;
        }
        if (js_get_fast_array(ctx, obj, &arrp, &count)) {
            for (; n < count; n++) {
                if (js_strict_eq2(ctx, argv[0], arrp[n], JS_EQ_STRICT)) {
                    res = n;
                    goto done;
                }
            }
        }
        for (; n < len; n++) {
            int present = JS_TryGetPropertyInt64(ctx, obj, n, &val);
            if (present < 0)
                goto exception;
            if (present) {
                if (js_strict_eq2(ctx, argv[0], val, JS_EQ_STRICT)) {
                    JS_FreeValue(ctx, val);
                    res = n;
                    break;
                }
                JS_FreeValue(ctx, val);
            }
        }
    }
 done:
    JS_FreeValue(ctx, obj);
    return JS_NewInt64(ctx, res);

 exception:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_lastIndexOf(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, val;
    int64_t len, n, res;
    int present;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    res = -1;
    if (len > 0) {
        n = len - 1;
        if (argc > 1) {
            if (JS_ToInt64Clamp(ctx, &n, argv[1], -1, len - 1, len))
                goto exception;
        }
        /* XXX: should special case fast arrays */
        for (; n >= 0; n--) {
            present = JS_TryGetPropertyInt64(ctx, obj, n, &val);
            if (present < 0)
                goto exception;
            if (present) {
                if (js_strict_eq2(ctx, argv[0], val, JS_EQ_STRICT)) {
                    JS_FreeValue(ctx, val);
                    res = n;
                    break;
                }
                JS_FreeValue(ctx, val);
            }
        }
    }
    JS_FreeValue(ctx, obj);
    return JS_NewInt64(ctx, res);

 exception:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

enum {
    ArrayFind,
    ArrayFindIndex,
    ArrayFindLast,
    ArrayFindLastIndex,
};

static JSValue js_array_find(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int mode)
{
    JSValueConst func, this_arg;
    JSValueConst args[3];
    JSValue obj, val, index_val, res;
    int64_t len, k, end;
    int dir;

    index_val = JS_UNDEFINED;
    val = JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    func = argv[0];
    if (check_function(ctx, func))
        goto exception;

    this_arg = JS_UNDEFINED;
    if (argc > 1)
        this_arg = argv[1];

    k = 0;
    dir = 1;
    end = len;
    if (mode == ArrayFindLast || mode == ArrayFindLastIndex) {
        k = len - 1;
        dir = -1;
        end = -1;
    }

    // TODO(bnoordhuis) add fast path for fast arrays
    for(; k != end; k += dir) {
        index_val = JS_NewInt64(ctx, k);
        if (JS_IsException(index_val))
            goto exception;
        val = JS_GetPropertyValue(ctx, obj, index_val);
        if (JS_IsException(val))
            goto exception;
        args[0] = val;
        args[1] = index_val;
        args[2] = this_val;
        res = JS_Call(ctx, func, this_arg, 3, args);
        if (JS_IsException(res))
            goto exception;
        if (JS_ToBoolFree(ctx, res)) {
            if (mode == ArrayFindIndex || mode == ArrayFindLastIndex) {
                JS_FreeValue(ctx, val);
                JS_FreeValue(ctx, obj);
                return index_val;
            } else {
                JS_FreeValue(ctx, index_val);
                JS_FreeValue(ctx, obj);
                return val;
            }
        }
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, index_val);
    }
    JS_FreeValue(ctx, obj);
    if (mode == ArrayFindIndex || mode == ArrayFindLastIndex)
        return JS_NewInt32(ctx, -1);
    else
        return JS_UNDEFINED;

exception:
    JS_FreeValue(ctx, index_val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_toString(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, method, ret;

    obj = JS_ToObject(ctx, this_val);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    method = JS_GetProperty(ctx, obj, JS_ATOM_join);
    if (JS_IsException(method)) {
        ret = JS_EXCEPTION;
    } else
    if (!JS_IsFunction(ctx, method)) {
        /* Use intrinsic Object.prototype.toString */
        JS_FreeValue(ctx, method);
        ret = js_object_toString(ctx, obj, 0, NULL);
    } else {
        ret = JS_CallFree(ctx, method, obj, 0, NULL);
    }
    JS_FreeValue(ctx, obj);
    return ret;
}

static JSValue js_array_join(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int toLocaleString)
{
    JSValue obj, sep = JS_UNDEFINED, el;
    StringBuffer b_s, *b = &b_s;
    JSString *p = NULL;
    int64_t i, n;
    int c;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &n, obj))
        goto exception;

    c = ',';    /* default separator */
    if (!toLocaleString && argc > 0 && !JS_IsUndefined(argv[0])) {
        sep = JS_ToString(ctx, argv[0]);
        if (JS_IsException(sep))
            goto exception;
        p = JS_VALUE_GET_STRING(sep);
        if (p->len == 1 && !p->is_wide_char)
            c = p->u.str8[0];
        else
            c = -1;
    }
    string_buffer_init(ctx, b, 0);

    for(i = 0; i < n; i++) {
        if (i > 0) {
            if (c >= 0) {
                string_buffer_putc8(b, c);
            } else {
                string_buffer_concat(b, p, 0, p->len);
            }
        }
        el = JS_GetPropertyUint32(ctx, obj, i);
        if (JS_IsException(el))
            goto fail;
        if (!JS_IsNull(el) && !JS_IsUndefined(el)) {
            if (toLocaleString) {
                el = JS_ToLocaleStringFree(ctx, el);
            }
            if (string_buffer_concat_value_free(b, el))
                goto fail;
        }
    }
    JS_FreeValue(ctx, sep);
    JS_FreeValue(ctx, obj);
    return string_buffer_end(b);

fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, sep);
exception:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_pop(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int shift)
{
    JSValue obj, res = JS_UNDEFINED;
    int64_t len, newLen;
    JSValue *arrp;
    uint32_t count32;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;
    newLen = 0;
    if (len > 0) {
        newLen = len - 1;
        /* Special case fast arrays */
        if (js_get_fast_array(ctx, obj, &arrp, &count32) && count32 == len) {
            JSObject *p = JS_VALUE_GET_OBJ(obj);
            if (shift) {
                res = arrp[0];
                memmove(arrp, arrp + 1, (count32 - 1) * sizeof(*arrp));
                p->u.array.count--;
            } else {
                res = arrp[count32 - 1];
                p->u.array.count--;
            }
        } else {
            if (shift) {
                res = JS_GetPropertyInt64(ctx, obj, 0);
                if (JS_IsException(res))
                    goto exception;
                if (JS_CopySubArray(ctx, obj, 0, 1, len - 1, +1))
                    goto exception;
            } else {
                res = JS_GetPropertyInt64(ctx, obj, newLen);
                if (JS_IsException(res))
                    goto exception;
            }
            if (JS_DeletePropertyInt64(ctx, obj, newLen, JS_PROP_THROW) < 0)
                goto exception;
        }
    }
    if (JS_SetProperty(ctx, obj, JS_ATOM_length, JS_NewInt64(ctx, newLen)) < 0)
        goto exception;

    JS_FreeValue(ctx, obj);
    return res;

 exception:
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_push(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int unshift)
{
    JSValue obj;
    int i;
    int64_t len, from, newLen;

    if (likely(JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT && !unshift)) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (likely(p->class_id == JS_CLASS_ARRAY && p->fast_array &&
                   can_extend_fast_array(p) &&
                   JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT &&
                   JS_VALUE_GET_INT(p->prop[0].u.value) == p->u.array.count &&
                   (get_shape_prop(p->shape)->flags & JS_PROP_WRITABLE) != 0)) {
            /* fast case */
            uint32_t new_len;
            new_len = p->u.array.count + argc;
            if (likely(new_len <= INT32_MAX)) {
                if (unlikely(new_len > p->u.array.u1.size)) {
                    if (expand_fast_array(ctx, p, new_len))
                        return JS_EXCEPTION;
                }
                for(i = 0; i < argc; i++)
                    p->u.array.u.values[p->u.array.count + i] = JS_DupValue(ctx, argv[i]);
                p->prop[0].u.value = JS_NewInt32(ctx, new_len);
                p->u.array.count = new_len;
                return JS_NewInt32(ctx, new_len);
            }
        }
    }
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;
    newLen = len + argc;
    if (newLen > MAX_SAFE_INTEGER) {
        JS_ThrowTypeError(ctx, "Array loo long");
        goto exception;
    }
    from = len;
    if (unshift && argc > 0) {
        if (JS_CopySubArray(ctx, obj, argc, 0, len, -1))
            goto exception;
        from = 0;
    }
    for(i = 0; i < argc; i++) {
        if (JS_SetPropertyInt64(ctx, obj, from + i,
                                JS_DupValue(ctx, argv[i])) < 0)
            goto exception;
    }
    if (JS_SetProperty(ctx, obj, JS_ATOM_length, JS_NewInt64(ctx, newLen)) < 0)
        goto exception;

    JS_FreeValue(ctx, obj);
    return JS_NewInt64(ctx, newLen);

 exception:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_array_reverse(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue obj, lval, hval;
    JSValue *arrp;
    int64_t len, l, h;
    int l_present, h_present;
    uint32_t count32;

    lval = JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    /* Special case fast arrays */
    if (js_get_fast_array(ctx, obj, &arrp, &count32) && count32 == len) {
        uint32_t ll, hh;

        if (count32 > 1) {
            for (ll = 0, hh = count32 - 1; ll < hh; ll++, hh--) {
                lval = arrp[ll];
                arrp[ll] = arrp[hh];
                arrp[hh] = lval;
            }
        }
        return obj;
    }

    for (l = 0, h = len - 1; l < h; l++, h--) {
        l_present = JS_TryGetPropertyInt64(ctx, obj, l, &lval);
        if (l_present < 0)
            goto exception;
        h_present = JS_TryGetPropertyInt64(ctx, obj, h, &hval);
        if (h_present < 0)
            goto exception;
        if (h_present) {
            if (JS_SetPropertyInt64(ctx, obj, l, hval) < 0)
                goto exception;

            if (l_present) {
                if (JS_SetPropertyInt64(ctx, obj, h, lval) < 0) {
                    lval = JS_UNDEFINED;
                    goto exception;
                }
                lval = JS_UNDEFINED;
            } else {
                if (JS_DeletePropertyInt64(ctx, obj, h, JS_PROP_THROW) < 0)
                    goto exception;
            }
        } else {
            if (l_present) {
                if (JS_DeletePropertyInt64(ctx, obj, l, JS_PROP_THROW) < 0)
                    goto exception;
                if (JS_SetPropertyInt64(ctx, obj, h, lval) < 0) {
                    lval = JS_UNDEFINED;
                    goto exception;
                }
                lval = JS_UNDEFINED;
            }
        }
    }
    return obj;

 exception:
    JS_FreeValue(ctx, lval);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

// Note: a.toReversed() is a.slice().reverse() with the twist that a.slice()
// leaves holes in sparse arrays intact whereas a.toReversed() replaces them
// with undefined, thus in effect creating a dense array.
// Does not use Array[@@species], always returns a base Array.
static JSValue js_array_toReversed(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue arr, obj, ret, *arrp, *pval;
    JSObject *p;
    int64_t i, len;
    uint32_t count32;

    ret = JS_EXCEPTION;
    arr = JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    arr = js_allocate_fast_array(ctx, len);
    if (JS_IsException(arr))
        goto exception;

    if (len > 0) {
        p = JS_VALUE_GET_OBJ(arr);

        i = len - 1;
        pval = p->u.array.u.values;
        if (js_get_fast_array(ctx, obj, &arrp, &count32) && count32 == len) {
            for (; i >= 0; i--, pval++)
                *pval = JS_DupValue(ctx, arrp[i]);
        } else {
            // Query order is observable; test262 expects descending order.
            for (; i >= 0; i--, pval++) {
                if (-1 == JS_TryGetPropertyInt64(ctx, obj, i, pval))
                    goto exception;
            }
        }
    }

    ret = arr;
    arr = JS_UNDEFINED;

exception:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, obj);
    return ret;
}

static JSValue js_array_slice(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue obj, arr, val, ctor;
    int64_t len, start, k, final, n, count;
    int kPresent;
    JSValue *arrp;
    uint32_t count32;

    arr = JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    if (JS_ToInt64Clamp(ctx, &start, argv[0], 0, len, len))
        goto exception;

    final = len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt64Clamp(ctx, &final, argv[1], 0, len, len))
            goto exception;
    }
    count = max_int64(final - start, 0);

    ctor = JS_ArraySpeciesGetCtor(ctx, obj);
    if (JS_IsException(ctor))
        goto exception;

    final = start + count;
    if (JS_IsUndefined(ctor) &&
        js_get_fast_array(ctx, obj, &arrp, &count32) &&
        final <= count32) {
        /* fast case */
        arr = js_create_array(ctx, count, (JSValueConst *)arrp + start);
    } else {
        arr = JS_ArrayCreateFromCtor(ctx, ctor, count);
        JS_FreeValue(ctx, ctor);
        if (JS_IsException(arr))
            goto exception;

        n = 0;
        for (k = start; k < final; k++, n++) {
            kPresent = JS_TryGetPropertyInt64(ctx, obj, k, &val);
            if (kPresent < 0)
                goto exception;
            if (kPresent) {
                if (JS_CreateDataPropertyUint32(ctx, arr, n, val, JS_PROP_THROW) < 0)
                    goto exception;
            }
        }
        if (JS_SetProperty(ctx, arr, JS_ATOM_length, JS_NewInt64(ctx, n)) < 0)
            goto exception;
    }
    JS_FreeValue(ctx, obj);
    return arr;

 exception:
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue js_array_splice(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue obj, arr, val, ctor;
    int64_t len, start, k, final, n, del_count, new_len;
    int kPresent;
    uint32_t i, item_count;
    JSObject *p;
    
    arr = JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    if (JS_ToInt64Clamp(ctx, &start, argv[0], 0, len, len))
        goto exception;

    if (argc == 0) {
        item_count = 0;
        del_count = 0;
    } else if (argc == 1) {
        item_count = 0;
        del_count = len - start;
    } else {
        item_count = argc - 2;
        if (JS_ToInt64Clamp(ctx, &del_count, argv[1], 0, len - start, 0))
            goto exception;
    }
    if (len + item_count - del_count > MAX_SAFE_INTEGER) {
        JS_ThrowTypeError(ctx, "Array loo long");
        goto exception;
    }
    final = start + del_count;
    /* warning: 'len' may be different from the actual array length
       because it may have been modified */
    new_len = len + item_count - del_count;
    
    ctor = JS_ArraySpeciesGetCtor(ctx, obj);
    if (JS_IsException(ctor))
        goto exception;

    p = JS_VALUE_GET_PTR(obj);
    if (JS_IsUndefined(ctor) &&
        p->class_id == JS_CLASS_ARRAY &&
        p->fast_array &&
        final <= p->u.array.count && 
        (get_shape_prop(p->shape)->flags & JS_PROP_WRITABLE) && /* writable array length */
        can_extend_fast_array(p)) {
        uint32_t count32 = p->u.array.count;
        JSValue *arrp = p->u.array.u.values;

        /* fast case */
        arr = js_create_array(ctx, del_count, (JSValueConst *)arrp + start);
        if (JS_IsException(arr))
            goto exception;
        
        if (item_count != del_count) {
            /* resize */
            uint32_t new_count32;
            new_count32 = count32 + item_count - del_count;
            if (del_count > item_count) {
                for(i = 0; i < del_count - item_count; i++)
                    JS_FreeValue(ctx, arrp[start + item_count + i]);
                memmove(arrp + start + item_count, arrp + final,
                        (count32 - final) * sizeof(arrp[0]));
            } else {
                if (unlikely(new_count32 > p->u.array.u1.size)) {
                    if (expand_fast_array(ctx, p, new_count32))
                        goto exception;
                    arrp = p->u.array.u.values;
                }
                memmove(arrp + start + item_count, arrp + final,
                        (count32 - final) * sizeof(arrp[0]));
                for(i = 0; i < item_count - del_count; i++)
                    arrp[start + del_count + i] = JS_UNDEFINED;
            }
            p->u.array.count = new_count32;
        }
        for(i = 0; i < item_count; i++)
            set_value(ctx, &arrp[start + i], JS_DupValue(ctx, argv[i + 2]));
    } else {
        arr = JS_ArrayCreateFromCtor(ctx, ctor, del_count);
        JS_FreeValue(ctx, ctor);
        if (JS_IsException(arr))
            goto exception;
        
        n = 0;
        for (k = start; k < final; k++, n++) {
            kPresent = JS_TryGetPropertyInt64(ctx, obj, k, &val);
            if (kPresent < 0)
                goto exception;
            if (kPresent) {
                if (JS_CreateDataPropertyUint32(ctx, arr, n, val, JS_PROP_THROW) < 0)
                    goto exception;
            }
        }
        if (JS_SetProperty(ctx, arr, JS_ATOM_length, JS_NewInt64(ctx, n)) < 0)
            goto exception;
        
        if (item_count != del_count) {
            if (JS_CopySubArray(ctx, obj, start + item_count,
                                start + del_count, len - (start + del_count),
                                item_count <= del_count ? +1 : -1) < 0)
                goto exception;

            for (k = len; k-- > new_len; ) {
                if (JS_DeletePropertyInt64(ctx, obj, k, JS_PROP_THROW) < 0)
                    goto exception;
            }
        }
        for (i = 0; i < item_count; i++) {
            if (JS_SetPropertyInt64(ctx, obj, start + i, JS_DupValue(ctx, argv[i + 2])) < 0)
                goto exception;
        }
    }
    if (JS_SetProperty(ctx, obj, JS_ATOM_length, JS_NewInt64(ctx, new_len)) < 0)
        goto exception;
    JS_FreeValue(ctx, obj);
    return arr;

 exception:
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue js_array_toSpliced(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue arr, obj, ret, *arrp, *pval, *last;
    JSObject *p;
    int64_t i, j, len, newlen, start, add, del;
    uint32_t count32;

    pval = NULL;
    last = NULL;
    ret = JS_EXCEPTION;
    arr = JS_UNDEFINED;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    start = 0;
    if (argc > 0)
        if (JS_ToInt64Clamp(ctx, &start, argv[0], 0, len, len))
            goto exception;

    del = 0;
    if (argc > 0)
        del = len - start;
    if (argc > 1)
        if (JS_ToInt64Clamp(ctx, &del, argv[1], 0, del, 0))
            goto exception;

    add = 0;
    if (argc > 2)
        add = argc - 2;

    newlen = len + add - del;
    if (newlen > MAX_SAFE_INTEGER) {
        JS_ThrowTypeError(ctx, "invalid array length");
        goto exception;
    }

    arr = js_allocate_fast_array(ctx, newlen);
    if (JS_IsException(arr))
        goto exception;

    if (newlen <= 0)
        goto done;

    p = JS_VALUE_GET_OBJ(arr);
    pval = &p->u.array.u.values[0];
    last = &p->u.array.u.values[newlen];

    if (js_get_fast_array(ctx, obj, &arrp, &count32) && count32 == len) {
        for (i = 0; i < start; i++, pval++)
            *pval = JS_DupValue(ctx, arrp[i]);
        for (j = 0; j < add; j++, pval++)
            *pval = JS_DupValue(ctx, argv[2 + j]);
        for (i += del; i < len; i++, pval++)
            *pval = JS_DupValue(ctx, arrp[i]);
    } else {
        for (i = 0; i < start; i++, pval++)
            if (-1 == JS_TryGetPropertyInt64(ctx, obj, i, pval))
                goto exception;
        for (j = 0; j < add; j++, pval++)
            *pval = JS_DupValue(ctx, argv[2 + j]);
        for (i += del; i < len; i++, pval++)
            if (-1 == JS_TryGetPropertyInt64(ctx, obj, i, pval))
                goto exception;
    }

    assert(pval == last);

done:
    ret = arr;
    arr = JS_UNDEFINED;

exception:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, obj);
    return ret;
}

static JSValue js_array_copyWithin(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj;
    int64_t len, from, to, final, count;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    if (JS_ToInt64Clamp(ctx, &to, argv[0], 0, len, len))
        goto exception;

    if (JS_ToInt64Clamp(ctx, &from, argv[1], 0, len, len))
        goto exception;

    final = len;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt64Clamp(ctx, &final, argv[2], 0, len, len))
            goto exception;
    }

    count = min_int64(final - from, len - to);

    if (JS_CopySubArray(ctx, obj, to, from, count,
                        (from < to && to < from + count) ? -1 : +1))
        goto exception;

    return obj;

 exception:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static int64_t JS_FlattenIntoArray(JSContext *ctx, JSValueConst target,
                                   JSValueConst source, int64_t sourceLen,
                                   int64_t targetIndex, int depth,
                                   JSValueConst mapperFunction,
                                   JSValueConst thisArg)
{
    JSValue element;
    int64_t sourceIndex, elementLen;
    int present, is_array;

    if (js_check_stack_overflow(ctx->rt, 0)) {
        JS_ThrowStackOverflow(ctx);
        return -1;
    }

    for (sourceIndex = 0; sourceIndex < sourceLen; sourceIndex++) {
        present = JS_TryGetPropertyInt64(ctx, source, sourceIndex, &element);
        if (present < 0)
            return -1;
        if (!present)
            continue;
        if (!JS_IsUndefined(mapperFunction)) {
            JSValueConst args[3] = { element, JS_NewInt64(ctx, sourceIndex), source };
            element = JS_Call(ctx, mapperFunction, thisArg, 3, args);
            JS_FreeValue(ctx, (JSValue)args[0]);
            JS_FreeValue(ctx, (JSValue)args[1]);
            if (JS_IsException(element))
                return -1;
        }
        if (depth > 0) {
            is_array = JS_IsArray(ctx, element);
            if (is_array < 0)
                goto fail;
            if (is_array) {
                if (js_get_length64(ctx, &elementLen, element) < 0)
                    goto fail;
                targetIndex = JS_FlattenIntoArray(ctx, target, element,
                                                  elementLen, targetIndex,
                                                  depth - 1,
                                                  JS_UNDEFINED, JS_UNDEFINED);
                if (targetIndex < 0)
                    goto fail;
                JS_FreeValue(ctx, element);
                continue;
            }
        }
        if (targetIndex >= MAX_SAFE_INTEGER) {
            JS_ThrowTypeError(ctx, "Array too long");
            goto fail;
        }
        if (JS_DefinePropertyValueInt64(ctx, target, targetIndex, element,
                                        JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            return -1;
        targetIndex++;
    }
    return targetIndex;

fail:
    JS_FreeValue(ctx, element);
    return -1;
}

static JSValue js_array_flatten(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int map)
{
    JSValue obj, arr;
    JSValueConst mapperFunction, thisArg;
    int64_t sourceLen;
    int depthNum;

    arr = JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &sourceLen, obj))
        goto exception;

    depthNum = 1;
    mapperFunction = JS_UNDEFINED;
    thisArg = JS_UNDEFINED;
    if (map) {
        mapperFunction = argv[0];
        if (argc > 1) {
            thisArg = argv[1];
        }
        if (check_function(ctx, mapperFunction))
            goto exception;
    } else {
        if (argc > 0 && !JS_IsUndefined(argv[0])) {
            if (JS_ToInt32Sat(ctx, &depthNum, argv[0]) < 0)
                goto exception;
        }
    }
    arr = JS_ArraySpeciesCreate(ctx, obj, 0);
    if (JS_IsException(arr))
        goto exception;
    if (JS_FlattenIntoArray(ctx, arr, obj, sourceLen, 0, depthNum,
                            mapperFunction, thisArg) < 0)
        goto exception;
    JS_FreeValue(ctx, obj);
    return arr;

exception:
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

/* Array sort */

typedef struct ValueSlot {
    JSValue val;
    JSString *str;
    int64_t pos;
} ValueSlot;

struct array_sort_context {
    JSContext *ctx;
    int exception;
    int has_method;
    JSValueConst method;
};

static int js_array_cmp_generic(const void *a, const void *b, void *opaque) {
    struct array_sort_context *psc = opaque;
    JSContext *ctx = psc->ctx;
    JSValueConst argv[2];
    JSValue res;
    ValueSlot *ap = (ValueSlot *)(void *)a;
    ValueSlot *bp = (ValueSlot *)(void *)b;
    int cmp;

    if (psc->exception)
        return 0;

    if (psc->has_method) {
        /* custom sort function is specified as returning 0 for identical
         * objects: avoid method call overhead.
         */
        if (!memcmp(&ap->val, &bp->val, sizeof(ap->val)))
            goto cmp_same;
        argv[0] = ap->val;
        argv[1] = bp->val;
        res = JS_Call(ctx, psc->method, JS_UNDEFINED, 2, argv);
        if (JS_IsException(res))
            goto exception;
        if (JS_VALUE_GET_TAG(res) == JS_TAG_INT) {
            int val = JS_VALUE_GET_INT(res);
            cmp = (val > 0) - (val < 0);
        } else {
            double val;
            if (JS_ToFloat64Free(ctx, &val, res) < 0)
                goto exception;
            cmp = (val > 0) - (val < 0);
        }
    } else {
        /* Not supposed to bypass ToString even for identical objects as
         * tested in test262/test/built-ins/Array/prototype/sort/bug_596_1.js
         */
        if (!ap->str) {
            JSValue str = JS_ToString(ctx, ap->val);
            if (JS_IsException(str))
                goto exception;
            ap->str = JS_VALUE_GET_STRING(str);
        }
        if (!bp->str) {
            JSValue str = JS_ToString(ctx, bp->val);
            if (JS_IsException(str))
                goto exception;
            bp->str = JS_VALUE_GET_STRING(str);
        }
        cmp = js_string_compare(ctx, ap->str, bp->str);
    }
    if (cmp != 0)
        return cmp;
cmp_same:
    /* make sort stable: compare array offsets */
    return (ap->pos > bp->pos) - (ap->pos < bp->pos);

exception:
    psc->exception = 1;
    return 0;
}

static JSValue js_array_sort(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    struct array_sort_context asc = { ctx, 0, 0, argv[0] };
    JSValue obj = JS_UNDEFINED;
    ValueSlot *array = NULL;
    size_t array_size = 0, pos = 0, n = 0;
    int64_t i, len, undefined_count = 0;
    int present;

    if (!JS_IsUndefined(asc.method)) {
        if (check_function(ctx, asc.method))
            goto exception;
        asc.has_method = 1;
    }
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    /* XXX: should special case fast arrays */
    for (i = 0; i < len; i++) {
        if (pos >= array_size) {
            size_t new_size, slack;
            ValueSlot *new_array;
            new_size = (array_size + (array_size >> 1) + 31) & ~15;
            new_array = js_realloc2(ctx, array, new_size * sizeof(*array), &slack);
            if (new_array == NULL)
                goto exception;
            new_size += slack / sizeof(*new_array);
            array = new_array;
            array_size = new_size;
        }
        present = JS_TryGetPropertyInt64(ctx, obj, i, &array[pos].val);
        if (present < 0)
            goto exception;
        if (present == 0)
            continue;
        if (JS_IsUndefined(array[pos].val)) {
            undefined_count++;
            continue;
        }
        array[pos].str = NULL;
        array[pos].pos = i;
        pos++;
    }
    rqsort(array, pos, sizeof(*array), js_array_cmp_generic, &asc);
    if (asc.exception)
        goto exception;

    /* XXX: should special case fast arrays */
    while (n < pos) {
        if (array[n].str)
            JS_FreeValue(ctx, JS_MKPTR(JS_TAG_STRING, array[n].str));
        if (array[n].pos == n) {
            JS_FreeValue(ctx, array[n].val);
        } else {
            if (JS_SetPropertyInt64(ctx, obj, n, array[n].val) < 0) {
                n++;
                goto exception;
            }
        }
        n++;
    }
    js_free(ctx, array);
    for (i = n; undefined_count-- > 0; i++) {
        if (JS_SetPropertyInt64(ctx, obj, i, JS_UNDEFINED) < 0)
            goto fail;
    }
    for (; i < len; i++) {
        if (JS_DeletePropertyInt64(ctx, obj, i, JS_PROP_THROW) < 0)
            goto fail;
    }
    return obj;

exception:
    for (; n < pos; n++) {
        JS_FreeValue(ctx, array[n].val);
        if (array[n].str)
            JS_FreeValue(ctx, JS_MKPTR(JS_TAG_STRING, array[n].str));
    }
    js_free(ctx, array);
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

// Note: a.toSorted() is a.slice().sort() with the twist that a.slice()
// leaves holes in sparse arrays intact whereas a.toSorted() replaces them
// with undefined, thus in effect creating a dense array.
// Does not use Array[@@species], always returns a base Array.
static JSValue js_array_toSorted(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue arr, obj, ret, *arrp, *pval;
    JSObject *p;
    int64_t i, len;
    uint32_t count32;
    int ok;

    ok = JS_IsUndefined(argv[0]) || JS_IsFunction(ctx, argv[0]);
    if (!ok)
        return JS_ThrowTypeError(ctx, "not a function");

    ret = JS_EXCEPTION;
    arr = JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto exception;

    arr = js_allocate_fast_array(ctx, len);
    if (JS_IsException(arr))
        goto exception;

    if (len > 0) {
        p = JS_VALUE_GET_OBJ(arr);
        i = 0;
        pval = p->u.array.u.values;
        if (js_get_fast_array(ctx, obj, &arrp, &count32) && count32 == len) {
            for (; i < len; i++, pval++)
                *pval = JS_DupValue(ctx, arrp[i]);
        } else {
            for (; i < len; i++, pval++) {
                if (-1 == JS_TryGetPropertyInt64(ctx, obj, i, pval))
                    goto exception;
            }
        }
    }

    ret = js_array_sort(ctx, arr, argc, argv);
    if (JS_IsException(ret))
        goto exception;
    JS_FreeValue(ctx, ret);

    ret = arr;
    arr = JS_UNDEFINED;

exception:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, obj);
    return ret;
}

typedef struct JSArrayIteratorData {
    JSValue obj;
    JSIteratorKindEnum kind;
    uint32_t idx;
} JSArrayIteratorData;

static void js_array_iterator_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSArrayIteratorData *it = p->u.array_iterator_data;
    if (it) {
        JS_FreeValueRT(rt, it->obj);
        js_free_rt(rt, it);
    }
}

static void js_array_iterator_mark(JSRuntime *rt, JSValueConst val,
                                   JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSArrayIteratorData *it = p->u.array_iterator_data;
    if (it) {
        JS_MarkValue(rt, it->obj, mark_func);
    }
}

static JSValue js_create_array_iterator(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int magic)
{
    JSValue enum_obj, arr;
    JSArrayIteratorData *it;
    JSIteratorKindEnum kind;
    int class_id;

    kind = magic & 3;
    if (magic & 4) {
        /* string iterator case */
        arr = JS_ToStringCheckObject(ctx, this_val);
        class_id = JS_CLASS_STRING_ITERATOR;
    } else {
        arr = JS_ToObject(ctx, this_val);
        class_id = JS_CLASS_ARRAY_ITERATOR;
    }
    if (JS_IsException(arr))
        goto fail;
    enum_obj = JS_NewObjectClass(ctx, class_id);
    if (JS_IsException(enum_obj))
        goto fail;
    it = js_malloc(ctx, sizeof(*it));
    if (!it)
        goto fail1;
    it->obj = arr;
    it->kind = kind;
    it->idx = 0;
    JS_SetOpaque(enum_obj, it);
    return enum_obj;
 fail1:
    JS_FreeValue(ctx, enum_obj);
 fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue js_array_iterator_next(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv,
                                      BOOL *pdone, int magic)
{
    JSArrayIteratorData *it;
    uint32_t len, idx;
    JSValue val, obj;
    JSObject *p;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ARRAY_ITERATOR);
    if (!it)
        goto fail1;
    if (JS_IsUndefined(it->obj))
        goto done;
    p = JS_VALUE_GET_OBJ(it->obj);
    if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
        p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
        if (typed_array_is_oob(p)) {
            JS_ThrowTypeErrorArrayBufferOOB(ctx);
            goto fail1;
        }
        len = p->u.array.count;
    } else {
        if (js_get_length32(ctx, &len, it->obj)) {
        fail1:
            *pdone = FALSE;
            return JS_EXCEPTION;
        }
    }
    idx = it->idx;
    if (idx >= len) {
        JS_FreeValue(ctx, it->obj);
        it->obj = JS_UNDEFINED;
    done:
        *pdone = TRUE;
        return JS_UNDEFINED;
    }
    it->idx = idx + 1;
    *pdone = FALSE;
    if (it->kind == JS_ITERATOR_KIND_KEY) {
        return JS_NewUint32(ctx, idx);
    } else {
        val = JS_GetPropertyUint32(ctx, it->obj, idx);
        if (JS_IsException(val))
            return JS_EXCEPTION;
        if (it->kind == JS_ITERATOR_KIND_VALUE) {
            return val;
        } else {
            JSValueConst args[2];
            JSValue num;
            num = JS_NewUint32(ctx, idx);
            args[0] = num;
            args[1] = val;
            obj = js_create_array(ctx, 2, args);
            JS_FreeValue(ctx, val);
            JS_FreeValue(ctx, num);
            return obj;
        }
    }
}

/* Iterator Wrap */

typedef struct JSIteratorWrapData {
    JSValue wrapped_iter;
    JSValue wrapped_next;
} JSIteratorWrapData;

static void js_iterator_wrap_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorWrapData *it = p->u.iterator_wrap_data;
    if (it) {
        JS_FreeValueRT(rt, it->wrapped_iter);
        JS_FreeValueRT(rt, it->wrapped_next);
        js_free_rt(rt, it);
    }
}

static void js_iterator_wrap_mark(JSRuntime *rt, JSValueConst val,
                                  JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorWrapData *it = p->u.iterator_wrap_data;
    if (it) {
        JS_MarkValue(rt, it->wrapped_iter, mark_func);
        JS_MarkValue(rt, it->wrapped_next, mark_func);
    }
}

static JSValue js_iterator_wrap_next(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv,
                                     int *pdone, int magic)
{
    JSIteratorWrapData *it;
    JSValue method, ret;
    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_WRAP);
    if (!it)
        return JS_EXCEPTION;
    if (magic == GEN_MAGIC_NEXT) {
        return JS_IteratorNext(ctx, it->wrapped_iter, it->wrapped_next, 0, NULL, pdone);
    } else {
        method = JS_GetProperty(ctx, it->wrapped_iter, JS_ATOM_return);
        if (JS_IsException(method))
            return JS_EXCEPTION;
        if (JS_IsNull(method) || JS_IsUndefined(method)) {
            *pdone = TRUE;
            return JS_UNDEFINED;
        }
        ret = JS_IteratorNext2(ctx, it->wrapped_iter, method, 0, NULL, pdone);
        JS_FreeValue(ctx, method);
        return ret;
    }
}

static const JSCFunctionListEntry js_iterator_wrap_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_iterator_wrap_next, GEN_MAGIC_NEXT ),
    JS_ITERATOR_NEXT_DEF("return", 0, js_iterator_wrap_next, GEN_MAGIC_RETURN ),
};

