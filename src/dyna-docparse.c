/*
 * dyna:docparse -- native document parsers. Self-contained, in-repo (no deps).
 *
 *   import { parseJson, parseCsv } from "dyna:docparse";
 *   const obj  = parseJson('{"a":1,"b":[2,3]}');  // -> plain JS value
 *   const rows = parseCsv("a,b\n1,2\n3,4");        // -> [["a","b"],["1","2"],["3","4"]]
 *   const objs = parseCsv("a,b\n1,2", { header: true }); // -> [{a:"1",b:"2"}]
 *
 * parseJson delegates to the engine's own JSON reader (JS_ParseJSON), which
 * throws a SyntaxError on malformed input -- no reason to reimplement a JSON
 * parser from scratch. parseCsv is a small RFC-4180-ish state machine: quoted
 * fields, embedded commas/newlines, "" escaping, and CR/LF/CRLF terminators.
 * Both are TRANSIENT: the parse works over a private copy of the input and every
 * result byte is copied into fresh JS values (JS_New*), so nothing native
 * escapes into the JS heap. Blank records are skipped; a trailing terminator
 * does not create an empty final row.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCPARSE)

#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------- JSON: delegate to the engine's own reader ---------- */

static JSValue js_docparse_parse_json(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *input;
    size_t input_len;
    JSValue result;

    (void)this_val;
    (void)argc;
    input = JS_ToCStringLen(ctx, &input_len, argv[0]);
    if (!input)
        return JS_EXCEPTION;
    /* The engine's JSON reader throws a SyntaxError on malformed input and
     * returns an independent JS value; the C string can be freed straight
     * after. */
    result = JS_ParseJSON(ctx, input, input_len, "<input>");
    JS_FreeCString(ctx, input);
    return result;
}

/* ---------- CSV: RFC-4180-ish state machine over a private copy ---------- */

typedef struct {
    char *buf;        /* mutable working copy of the whole input (owned) */
    size_t len;       /* bytes in buf (embedded NULs are preserved) */
    size_t pos;       /* cursor */
    int row_started;  /* 1 once the current record has emitted a field */
} DynCsv;

/* Parse one field starting at p->pos. Quoted fields are unescaped in place
 * ("" -> a single "). Leaves pos at the terminator (',', CR, LF or EOF) without
 * consuming it. The output always lies within [start, pos), so it can never
 * point past the populated buffer. Mirrors the reference parser exactly. */
static void csv_parse_field(DynCsv *p, const char **out, size_t *out_len)
{
    char *buf = p->buf;
    size_t len = p->len;
    size_t pos = p->pos;

    if (pos < len && buf[pos] == '"') {
        size_t content0, w, run;
        int closed = 0;
        pos++; /* skip opening quote */
        content0 = pos;
        w = pos;
        run = pos;
        while (pos < len) {
            if (buf[pos] == '"') {
                if (pos + 1 < len && buf[pos + 1] == '"') {
                    if (pos > run) {
                        memmove(buf + w, buf + run, pos - run);
                        w += pos - run;
                    }
                    buf[w++] = '"'; /* "" collapses to one quote */
                    pos += 2;
                    run = pos;
                } else {
                    if (pos > run) {
                        memmove(buf + w, buf + run, pos - run);
                        w += pos - run;
                    }
                    pos++; /* consume closing quote */
                    closed = 1;
                    break;
                }
            } else {
                pos++;
            }
        }
        if (!closed && pos > run) {
            memmove(buf + w, buf + run, pos - run);
            w += pos - run;
        }
        /* Skip any stray bytes between a closing quote and the terminator. */
        if (closed)
            while (pos < len && buf[pos] != ',' && buf[pos] != '\r' &&
                   buf[pos] != '\n')
                pos++;
        *out = buf + content0;
        *out_len = w - content0;
        p->pos = pos;
        return;
    }

    {
        size_t start = pos;
        while (pos < len && buf[pos] != ',' && buf[pos] != '\r' &&
               buf[pos] != '\n')
            pos++;
        *out = buf + start;
        *out_len = pos - start;
        p->pos = pos;
    }
}

/* Return the next field of the current record, or -1 once it is exhausted. */
static int csv_next_field(DynCsv *p, const char **out, size_t *out_len)
{
    char c;
    if (p->pos >= p->len)
        return -1;
    c = p->buf[p->pos];

    if (!p->row_started) {
        /* Start of a record: a bare terminator is an empty line (no fields). */
        if (c == '\r' || c == '\n')
            return -1;
        p->row_started = 1;
        csv_parse_field(p, out, out_len);
        return 0;
    }

    /* Mid record: a terminator ends it; a delimiter introduces the next field
     * (which may be empty). */
    if (c == '\r' || c == '\n')
        return -1;
    if (c == ',')
        p->pos++;
    csv_parse_field(p, out, out_len);
    return 0;
}

/* Consume the current record's terminator. Returns 0 if another record
 * follows, -1 at end of input. */
static int csv_next_row(DynCsv *p)
{
    char *buf = p->buf;
    size_t len = p->len;

    /* The caller drains fields first, so pos is normally already here. */
    while (p->pos < len && buf[p->pos] != '\r' && buf[p->pos] != '\n')
        p->pos++;
    if (p->pos >= len)
        return -1; /* no terminator: end of input */

    if (buf[p->pos] == '\r') {
        p->pos++;
        if (p->pos < len && buf[p->pos] == '\n')
            p->pos++; /* CRLF */
    } else {
        p->pos++; /* LF */
    }
    p->row_started = 0;
    /* A terminator at the very end of input is not a new record. */
    return p->pos < len ? 0 : -1;
}

/* Array-of-array form: each record becomes an array of string fields. Empty
 * records (blank lines) are skipped. */
static JSValue js_docparse_csv_to_arrays(JSContext *ctx, DynCsv *p)
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
        while (csv_next_field(p, &field, &flen) == 0) {
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
    } while (csv_next_row(p) == 0);

    return result;
}

/* Array-of-object form: the first record supplies column keys; each later
 * record becomes an object keyed by column. Surplus fields (beyond the header
 * width) are ignored; missing trailing fields are simply absent. */
static JSValue js_docparse_csv_to_objects(JSContext *ctx, DynCsv *p)
{
    JSValue result = JS_UNDEFINED;
    JSAtom *headers = NULL;
    uint32_t n_headers = 0, cap = 0, i, row_index = 0;
    const char *field;
    size_t flen;

    /* 1) Collect the header record as reusable atoms. */
    while (csv_next_field(p, &field, &flen) == 0) {
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
    while (csv_next_row(p) == 0) {
        JSValue obj = JS_NewObject(ctx);
        uint32_t j = 0;
        int any = 0;
        if (JS_IsException(obj))
            goto fail_result;
        while (csv_next_field(p, &field, &flen) == 0) {
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

static JSValue js_docparse_parse_csv(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const char *input;
    size_t input_len;
    DynCsv parser;
    JSValue result;
    int header = 0;

    (void)this_val;
    /* Coerce every JS argument to C locals FIRST. Read the option object before
     * touching argv[0] so a throwing/reentrant coercion leaks nothing. */
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

    /* Private mutable copy: quoted fields are unescaped in place and field
     * pointers alias it, so it must outlive the field-to-JS-string copies. */
    parser.buf = (char *)malloc(input_len + 1);
    if (!parser.buf) {
        JS_FreeCString(ctx, input);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(parser.buf, input, input_len);
    parser.buf[input_len] = '\0';
    parser.len = input_len;
    parser.pos = 0;
    parser.row_started = 0;
    JS_FreeCString(ctx, input);

    result = header ? js_docparse_csv_to_objects(ctx, &parser)
                    : js_docparse_csv_to_arrays(ctx, &parser);

    free(parser.buf); /* result is an independent deep copy */
    return result;
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry js_docparse_funcs[] = {
    JS_CFUNC_DEF("parseJson", 1, js_docparse_parse_json),
    JS_CFUNC_DEF("parseCsv", 1, js_docparse_parse_csv),
};

static int js_docparse_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, js_docparse_funcs,
                                  countof(js_docparse_funcs));
}

int js_nat_init_docparse(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:docparse", js_docparse_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, js_docparse_funcs,
                                  countof(js_docparse_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_DOCPARSE */
