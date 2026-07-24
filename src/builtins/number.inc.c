/* Number.prototype (standard + Sugar/Ramda extensions) and Boolean.prototype.
 *
 * Unity-build fragment: #included into src/dynajs.c, never compiled alone.
 * Split out of the former object_array_iterator.inc.c (byte-identical token
 * stream preserved; see MODULARIZATION.md). */
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

/* --- SugarJS 2.0 + Ramda 0.32 non-ECMAScript Number.prototype methods ---
 * (SUGAR_RAMDA_NATIVE.md, phase 3). Installed non-enumerable on Number.prototype.
 * Every method coerces `this` to a double first (js_thisNumberValue) so it also
 * works on Number wrapper objects; numeric results normalize via JS_NewFloat64.
 * No native handle is held across argument coercion, so there is no reentrancy
 * hazard from a {valueOf}-armed argument. */

static double js_math_round(double a);   /* defined later in this TU (Math.round) */

enum {   /* js_number_ext_unary (0 args) */
    NUM_EXT_ABS, NUM_EXT_SQRT, NUM_EXT_EXP, NUM_EXT_SIN, NUM_EXT_COS,
    NUM_EXT_TAN, NUM_EXT_ASIN, NUM_EXT_ACOS, NUM_EXT_ATAN,
    NUM_EXT_NEGATE, NUM_EXT_INC, NUM_EXT_DEC,
};

static JSValue js_number_ext_unary(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    JSValue val;
    double d;
    (void)argc; (void)argv;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToFloat64Free(ctx, &d, val)) return JS_EXCEPTION;
    switch (magic) {
    case NUM_EXT_ABS:    d = fabs(d); break;
    case NUM_EXT_SQRT:   d = sqrt(d); break;
    case NUM_EXT_EXP:    d = exp(d); break;
    case NUM_EXT_SIN:    d = sin(d); break;
    case NUM_EXT_COS:    d = cos(d); break;
    case NUM_EXT_TAN:    d = tan(d); break;
    case NUM_EXT_ASIN:   d = asin(d); break;
    case NUM_EXT_ACOS:   d = acos(d); break;
    case NUM_EXT_ATAN:   d = atan(d); break;
    case NUM_EXT_NEGATE: d = -d; break;
    case NUM_EXT_INC:    d = d + 1; break;
    case NUM_EXT_DEC:    d = d - 1; break;
    }
    return JS_NewFloat64(ctx, d);
}

enum {   /* js_number_ext_binary (1 arg, coerced to double) */
    NUM_EXT_ADD, NUM_EXT_SUB, NUM_EXT_MUL, NUM_EXT_DIV, NUM_EXT_MOD, NUM_EXT_POW,
};

static JSValue js_number_ext_binary(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue val;
    double d, a;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToFloat64Free(ctx, &d, val)) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &a, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    switch (magic) {
    case NUM_EXT_ADD: d = d + a; break;
    case NUM_EXT_SUB: d = d - a; break;
    case NUM_EXT_MUL: d = d * a; break;
    case NUM_EXT_DIV: d = d / a; break;
    case NUM_EXT_MOD: d = fmod(d, a); break;   /* JS `%` on doubles == fmod */
    case NUM_EXT_POW: d = js_pow(d, a); break;
    }
    return JS_NewFloat64(ctx, d);
}

enum {   /* js_number_ext_compare (1 arg) -> boolean */
    NUM_EXT_GT, NUM_EXT_GTE, NUM_EXT_LT, NUM_EXT_LTE,
};

static JSValue js_number_ext_compare(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    JSValue val;
    double d, a;
    BOOL r = FALSE;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToFloat64Free(ctx, &d, val)) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &a, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    switch (magic) {   /* NaN comparisons are false by construction */
    case NUM_EXT_GT:  r = d >  a; break;
    case NUM_EXT_GTE: r = d >= a; break;
    case NUM_EXT_LT:  r = d <  a; break;
    case NUM_EXT_LTE: r = d <= a; break;
    }
    return JS_NewBool(ctx, r);
}

enum {   /* js_number_ext_predicate (0 args) -> boolean */
    NUM_EXT_IS_INTEGER, NUM_EXT_IS_ODD, NUM_EXT_IS_EVEN,
};

static JSValue js_number_ext_predicate(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int magic)
{
    JSValue val;
    double d;
    BOOL is_int, r = FALSE;
    (void)argc; (void)argv;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToFloat64Free(ctx, &d, val)) return JS_EXCEPTION;
    is_int = isfinite(d) && floor(d) == d;
    switch (magic) {
    case NUM_EXT_IS_INTEGER: r = is_int; break;
    case NUM_EXT_IS_ODD:     r = is_int && fmod(d, 2.0) != 0.0; break;
    case NUM_EXT_IS_EVEN:    r = is_int && fmod(d, 2.0) == 0.0; break;
    }
    return JS_NewBool(ctx, r);
}

/* isMultipleOf(n) -> this % n === 0 (Sugar). n==0 or NaN -> false via fmod. */
static JSValue js_number_ext_isMultipleOf(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    JSValue val;
    double d, a;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToFloat64Free(ctx, &d, val)) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &a, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    return JS_NewBool(ctx, a != 0.0 && fmod(d, a) == 0.0);
}

/* mathMod(n) -> Ramda: NaN unless both are integers and n >= 1, else a
 * non-negative modulus ((m % n) + n) % n. */
static JSValue js_number_ext_mathMod(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue val;
    double d, a, r;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToFloat64Free(ctx, &d, val)) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &a, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    if (!(isfinite(d) && floor(d) == d) ||
        !(isfinite(a) && floor(a) == a) || a < 1.0)
        return JS_NewFloat64(ctx, NAN);
    r = fmod(fmod(d, a) + a, a);
    return JS_NewFloat64(ctx, r);
}

/* clamp(min, max) -> Ramda clamp with `this` as the value. */
static JSValue js_number_ext_clamp(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue val;
    double d, lo, hi;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToFloat64Free(ctx, &d, val)) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &lo, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &hi, argc > 1 ? argv[1] : JS_UNDEFINED)) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, d < lo ? lo : (d > hi ? hi : d));
}

/* log(base=e) -> Sugar: change-of-base logarithm. */
static JSValue js_number_ext_log(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue val;
    double d, base;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToFloat64Free(ctx, &d, val)) return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &base, argv[0])) return JS_EXCEPTION;
        return JS_NewFloat64(ctx, log(d) / log(base));
    }
    return JS_NewFloat64(ctx, log(d));
}

enum {   /* js_number_ext_roundp (1 optional arg: decimal places) */
    NUM_EXT_ROUND, NUM_EXT_CEIL, NUM_EXT_FLOOR,
};

/* round/ceil/floor(precision=0) -> Sugar: round to `precision` decimal places
 * (negative rounds to tens/hundreds/...). */
static JSValue js_number_ext_roundp(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue val;
    double d, factor, scaled;
    int prec = 0;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToFloat64Free(ctx, &d, val)) return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt32Sat(ctx, &prec, argv[0])) return JS_EXCEPTION;
    }
    factor = pow(10.0, (double)prec);
    scaled = d * factor;
    switch (magic) {
    case NUM_EXT_ROUND: scaled = js_math_round(scaled); break;
    case NUM_EXT_CEIL:  scaled = ceil(scaled); break;
    case NUM_EXT_FLOOR: scaled = floor(scaled); break;
    }
    return JS_NewFloat64(ctx, scaled / factor);
}

/* chr() -> the single character for this code (Sugar, fromCharCode semantics). */
static JSValue js_number_ext_chr(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue val;
    StringBuffer b_s, *b = &b_s;
    int32_t c;
    (void)argc; (void)argv;
    val = js_thisNumberValue(ctx, this_val);
    if (JS_IsException(val)) return val;
    if (JS_ToInt32(ctx, &c, val)) { JS_FreeValue(ctx, val); return JS_EXCEPTION; }
    JS_FreeValue(ctx, val);
    if (string_buffer_init(ctx, b, 1)) return JS_EXCEPTION;
    if (string_buffer_putc16(b, c & 0xffff)) { string_buffer_free(b); return JS_EXCEPTION; }
    return string_buffer_end(b);
}

static const JSCFunctionListEntry js_number_ext_funcs[] = {
    /* Sugar math wrappers + Ramda unary */
    JS_CFUNC_MAGIC_DEF("abs",    0, js_number_ext_unary, NUM_EXT_ABS ),
    JS_CFUNC_MAGIC_DEF("sqrt",   0, js_number_ext_unary, NUM_EXT_SQRT ),
    JS_CFUNC_MAGIC_DEF("exp",    0, js_number_ext_unary, NUM_EXT_EXP ),
    JS_CFUNC_MAGIC_DEF("sin",    0, js_number_ext_unary, NUM_EXT_SIN ),
    JS_CFUNC_MAGIC_DEF("cos",    0, js_number_ext_unary, NUM_EXT_COS ),
    JS_CFUNC_MAGIC_DEF("tan",    0, js_number_ext_unary, NUM_EXT_TAN ),
    JS_CFUNC_MAGIC_DEF("asin",   0, js_number_ext_unary, NUM_EXT_ASIN ),
    JS_CFUNC_MAGIC_DEF("acos",   0, js_number_ext_unary, NUM_EXT_ACOS ),
    JS_CFUNC_MAGIC_DEF("atan",   0, js_number_ext_unary, NUM_EXT_ATAN ),
    JS_CFUNC_MAGIC_DEF("negate", 0, js_number_ext_unary, NUM_EXT_NEGATE ),
    JS_CFUNC_MAGIC_DEF("inc",    0, js_number_ext_unary, NUM_EXT_INC ),
    JS_CFUNC_MAGIC_DEF("dec",    0, js_number_ext_unary, NUM_EXT_DEC ),
    /* Ramda Math (binary) */
    JS_CFUNC_MAGIC_DEF("add",      1, js_number_ext_binary, NUM_EXT_ADD ),
    JS_CFUNC_MAGIC_DEF("subtract", 1, js_number_ext_binary, NUM_EXT_SUB ),
    JS_CFUNC_MAGIC_DEF("multiply", 1, js_number_ext_binary, NUM_EXT_MUL ),
    JS_CFUNC_MAGIC_DEF("divide",   1, js_number_ext_binary, NUM_EXT_DIV ),
    JS_CFUNC_MAGIC_DEF("modulo",   1, js_number_ext_binary, NUM_EXT_MOD ),
    JS_CFUNC_MAGIC_DEF("pow",      1, js_number_ext_binary, NUM_EXT_POW ),
    /* Ramda relational */
    JS_CFUNC_MAGIC_DEF("gt",  1, js_number_ext_compare, NUM_EXT_GT ),
    JS_CFUNC_MAGIC_DEF("gte", 1, js_number_ext_compare, NUM_EXT_GTE ),
    JS_CFUNC_MAGIC_DEF("lt",  1, js_number_ext_compare, NUM_EXT_LT ),
    JS_CFUNC_MAGIC_DEF("lte", 1, js_number_ext_compare, NUM_EXT_LTE ),
    /* Sugar predicates */
    JS_CFUNC_MAGIC_DEF("isInteger", 0, js_number_ext_predicate, NUM_EXT_IS_INTEGER ),
    JS_CFUNC_MAGIC_DEF("isOdd",     0, js_number_ext_predicate, NUM_EXT_IS_ODD ),
    JS_CFUNC_MAGIC_DEF("isEven",    0, js_number_ext_predicate, NUM_EXT_IS_EVEN ),
    JS_CFUNC_DEF("isMultipleOf", 1, js_number_ext_isMultipleOf ),
    /* Ramda mathMod / clamp, Sugar log / precision round */
    JS_CFUNC_DEF("mathMod", 1, js_number_ext_mathMod ),
    JS_CFUNC_DEF("clamp",   2, js_number_ext_clamp ),
    JS_CFUNC_DEF("log",     1, js_number_ext_log ),
    JS_CFUNC_MAGIC_DEF("round", 1, js_number_ext_roundp, NUM_EXT_ROUND ),
    JS_CFUNC_MAGIC_DEF("ceil",  1, js_number_ext_roundp, NUM_EXT_CEIL ),
    JS_CFUNC_MAGIC_DEF("floor", 1, js_number_ext_roundp, NUM_EXT_FLOOR ),
    JS_CFUNC_DEF("chr", 0, js_number_ext_chr ),
};

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

