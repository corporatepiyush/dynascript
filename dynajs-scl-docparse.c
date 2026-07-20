/*
 * scl:docparse -- native document parsers backed by secure-c-libs.
 *
 *   import { parseJson, parseCsv } from "scl:docparse";
 *   const obj  = parseJson('{"a":1,"b":[2,3]}');  // -> plain JS value
 *   const rows = parseCsv("a,b\n1,2\n3,4");        // -> [["a","b"],["1","2"],["3","4"]]
 *   const objs = parseCsv("a,b\n1,2", { header: true }); // -> [{a:"1",b:"2"}]
 *
 * These are TRANSIENT operations: a fresh SCL arena backs the parse, the whole
 * native parse tree is deep-copied into independent JS values (JS_New*), then
 * the arena is destroyed before returning. No arena pointer escapes into the JS
 * heap, so the result survives the free and these need not be resource classes.
 */
#include "dynajs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_parse_json.h"
#include "scl_parse_csv.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Recursion cap for the native-tree -> JS conversion. The SCL JSON parser is
 * itself iterative and rejects nesting past SCL_JSON_MAX_DEPTH (256), so a tree
 * handed to the converter can never be this deep; the guard is defence in depth
 * against a C-stack overflow regardless of where the tree came from. */
#define JS_DOCPARSE_MAX_DEPTH 1000

/* ---------- JSON: native tree -> independent JS values ---------- */

/* Deep-copy one native JSON node into a fresh JS value. Returns JS_EXCEPTION on
 * OOM or excessive depth, having freed any partial JS values it created. */
static JSValue js_docparse_json_to_js(JSContext *ctx,
                                      const scl_parse_json_value_t *node,
                                      int depth)
{
    if (depth > JS_DOCPARSE_MAX_DEPTH)
        return JS_ThrowRangeError(ctx, "scl:docparse: JSON nesting too deep");
    if (!node)
        return JS_NULL;

    switch (scl_parse_json_get_type(node)) {
    case SCL_JSON_NULL:
        return JS_NULL;
    case SCL_JSON_BOOL:
        return JS_NewBool(ctx, scl_parse_json_get_bool(node));
    case SCL_JSON_INT64:
        return JS_NewInt64(ctx, scl_parse_json_get_int(node));
    case SCL_JSON_DOUBLE:
        return JS_NewFloat64(ctx, scl_parse_json_get_double(node));
    case SCL_JSON_STRING: {
        const char *s = scl_parse_json_get_string(node);
        return JS_NewString(ctx, s ? s : "");
    }
    case SCL_JSON_ARRAY: {
        size_t i, n = scl_parse_json_array_len(node);
        JSValue arr = JS_NewArray(ctx);
        if (JS_IsException(arr))
            return arr;
        for (i = 0; i < n; i++) {
            JSValue child = js_docparse_json_to_js(
                ctx, scl_parse_json_array_get(node, i), depth + 1);
            if (JS_IsException(child)) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            /* consumes `child` on every path */
            if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, child,
                                             JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        }
        return arr;
    }
    case SCL_JSON_OBJECT: {
        size_t i, n = scl_parse_json_object_len(node);
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj))
            return obj;
        for (i = 0; i < n; i++) {
            const char *key = node->keys ? node->keys[i] : NULL;
            JSValue child =
                js_docparse_json_to_js(ctx, node->children[i], depth + 1);
            if (JS_IsException(child)) {
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            /* consumes `child` on every path */
            if (JS_DefinePropertyValueStr(ctx, obj, key ? key : "", child,
                                          JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
        }
        return obj;
    }
    default:
        return JS_ThrowInternalError(ctx, "scl:docparse: unknown JSON node");
    }
}

static JSValue js_scl_docparse_parse_json(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    const char *input;
    size_t input_len;
    scl_allocator_t *arena;
    scl_parse_json_value_t *root = NULL;
    scl_error_t err;
    JSValue result;

    (void)this_val;
    (void)argc;
    input = JS_ToCStringLen(ctx, &input_len, argv[0]);
    if (!input)
        return JS_EXCEPTION;

    arena = js_scl_arena_new(ctx);
    if (!arena) {
        JS_FreeCString(ctx, input);
        return JS_EXCEPTION; /* OOM already thrown */
    }

    /* Parse the entire document into the arena-backed native tree. */
    err = scl_parse_json_parse_with_len(arena, input, input_len, &root);
    JS_FreeCString(ctx, input); /* tree owns its own arena copy now */
    if (err != SCL_OK || !root) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowSyntaxError(ctx, "scl:docparse parseJson: %s",
                                   scl_error_string(err));
    }

    /* Deep-copy the whole tree into JS values, then reclaim the arena. The
     * returned value is fully independent of the (now freed) native memory. */
    result = js_docparse_json_to_js(ctx, root, 0);
    scl_alloc_arena_destroy(arena);
    return result;
}

/* ---------- CSV: streamed rows -> array of arrays / array of objects ------ */

/* Array-of-array form: each record becomes an array of string fields. Empty
 * records (blank lines) are skipped. */
static JSValue js_docparse_csv_to_arrays(JSContext *ctx, scl_parse_csv_t *parser)
{
    JSValue result;
    uint32_t row_index = 0;
    const char *field;
    size_t flen;

    result = JS_NewArray(ctx);
    if (JS_IsException(result))
        return result;

    do {
        JSValue row = JS_NewArray(ctx);
        uint32_t nf = 0;
        if (JS_IsException(row)) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        while (scl_parse_csv_next_field(parser, &field, &flen) == SCL_OK) {
            JSValue s = JS_NewStringLen(ctx, field, flen);
            if (JS_IsException(s) ||
                JS_DefinePropertyValueUint32(ctx, row, nf, s,
                                             JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, row);
                JS_FreeValue(ctx, result);
                return JS_EXCEPTION;
            }
            nf++;
        }
        if (nf == 0) {
            JS_FreeValue(ctx, row); /* skip blank line */
        } else if (JS_DefinePropertyValueUint32(ctx, result, row_index, row,
                                                JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        } else {
            row_index++;
        }
    } while (scl_parse_csv_next_row(parser) == SCL_OK);

    return result;
}

/* Array-of-object form: the first record supplies column keys; each later
 * record becomes an object keyed by column. Surplus fields (beyond the header
 * width) are ignored; missing trailing fields are simply absent. */
static JSValue js_docparse_csv_to_objects(JSContext *ctx, scl_parse_csv_t *parser)
{
    JSValue result = JS_UNDEFINED;
    JSAtom *headers = NULL;
    uint32_t n_headers = 0, cap = 0, i, row_index = 0;
    const char *field;
    size_t flen;

    /* 1) Collect the header record as reusable atoms. */
    while (scl_parse_csv_next_field(parser, &field, &flen) == SCL_OK) {
        JSAtom a;
        if (n_headers >= cap) {
            uint32_t ncap = cap ? cap * 2 : 8;
            JSAtom *nh = js_realloc(ctx, headers, ncap * sizeof(JSAtom));
            if (!nh)
                goto fail;
            headers = nh;
            cap = ncap;
        }
        a = JS_NewAtomLen(ctx, field, flen);
        if (a == JS_ATOM_NULL)
            goto fail;
        headers[n_headers++] = a;
    }

    result = JS_NewArray(ctx);
    if (JS_IsException(result))
        goto fail;

    /* 2) Each subsequent record -> object keyed by the header atoms. */
    while (scl_parse_csv_next_row(parser) == SCL_OK) {
        JSValue obj = JS_NewObject(ctx);
        uint32_t j = 0;
        int any = 0;
        if (JS_IsException(obj))
            goto fail_result;
        while (scl_parse_csv_next_field(parser, &field, &flen) == SCL_OK) {
            any = 1;
            if (j < n_headers) {
                JSValue s = JS_NewStringLen(ctx, field, flen);
                if (JS_IsException(s) ||
                    JS_DefinePropertyValue(ctx, obj, headers[j], s,
                                           JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, obj);
                    goto fail_result;
                }
            }
            j++;
        }
        if (!any) {
            JS_FreeValue(ctx, obj); /* skip blank line */
        } else if (JS_DefinePropertyValueUint32(ctx, result, row_index, obj,
                                                JS_PROP_C_W_E) < 0) {
            goto fail_result;
        } else {
            row_index++;
        }
    }

    for (i = 0; i < n_headers; i++)
        JS_FreeAtom(ctx, headers[i]);
    js_free(ctx, headers);
    return result;

fail_result:
    JS_FreeValue(ctx, result);
fail:
    for (i = 0; i < n_headers; i++)
        JS_FreeAtom(ctx, headers[i]);
    js_free(ctx, headers);
    return JS_EXCEPTION;
}

static JSValue js_scl_docparse_parse_csv(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    const char *input;
    size_t input_len;
    scl_allocator_t *arena;
    scl_parse_csv_t parser;
    scl_error_t err;
    JSValue result;
    int header = 0;

    (void)this_val;
    /* Optional { header: true } -> array of objects keyed by the first row. */
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue h = JS_GetPropertyStr(ctx, argv[1], "header");
        if (JS_IsException(h))
            return JS_EXCEPTION;
        header = JS_ToBool(ctx, h);
        JS_FreeValue(ctx, h);
        if (header < 0)
            return JS_EXCEPTION;
    }

    input = JS_ToCStringLen(ctx, &input_len, argv[0]);
    if (!input)
        return JS_EXCEPTION;

    arena = js_scl_arena_new(ctx);
    if (!arena) {
        JS_FreeCString(ctx, input);
        return JS_EXCEPTION;
    }

    if (scl_parse_csv_init(arena, &parser) != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        JS_FreeCString(ctx, input);
        return JS_ThrowOutOfMemory(ctx);
    }

    /* Feed the whole document at once; field pointers alias the arena buffer
     * and stay valid until destroy (we never feed again). */
    err = scl_parse_csv_feed(&parser, input, input_len);
    JS_FreeCString(ctx, input);
    if (err != SCL_OK) {
        scl_parse_csv_destroy(&parser);
        scl_alloc_arena_destroy(arena);
        return JS_ThrowSyntaxError(ctx, "scl:docparse parseCsv: %s",
                                   scl_error_string(err));
    }

    result = header ? js_docparse_csv_to_objects(ctx, &parser)
                    : js_docparse_csv_to_arrays(ctx, &parser);

    scl_parse_csv_destroy(&parser); /* no-op free on arena mem */
    scl_alloc_arena_destroy(arena); /* result is an independent deep copy */
    return result;
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry js_scl_docparse_funcs[] = {
    JS_CFUNC_DEF("parseJson", 1, js_scl_docparse_parse_json),
    JS_CFUNC_DEF("parseCsv", 1, js_scl_docparse_parse_csv),
};

static int js_scl_docparse_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, js_scl_docparse_funcs,
                                  countof(js_scl_docparse_funcs));
}

int js_scl_init_docparse(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:docparse",
                                   js_scl_docparse_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, js_scl_docparse_funcs,
                                  countof(js_scl_docparse_funcs));
}

#endif /* CONFIG_SCL_MODULES */
