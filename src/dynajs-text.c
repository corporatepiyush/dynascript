/*
 * scl:text -- SIMD-accelerated byte/text utilities over the shared kernel
 * engine (dynajs-simd-kernels.h). Self-contained, in-repo (no external deps).
 *
 *   import { count, indexOfAny, isValidUtf8, base64Encode, base64Decode }
 *       from "scl:text";
 *
 *   count("a,b,c,d", ",");                 // -> 3   (byte count; SIMD count_u8)
 *   indexOfAny("key: value", ":,\n");      // -> 3   (first delimiter; find_first_of)
 *   isValidUtf8(bytesArrayBuffer);          // -> true/false (SIMD-ASCII + DFA)
 *   base64Encode("hi");                     // -> "aGk="
 *   base64Decode("aGk=");                   // -> ArrayBuffer([0x68,0x69])
 *
 * Byte semantics: string arguments are materialised as UTF-8 bytes via
 * JS_ToCStringLen (offsets/counts are byte-based, matching scl:search).
 * ArrayBuffer arguments are used as raw bytes. Every JS argument is coerced to
 * a C view BEFORE any kernel runs (coercion can run arbitrary JS); these are
 * plain functions with no closable resource, and every owned C string is freed
 * on every path.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TEXT)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "dynajs-simd-kernels.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Borrow raw bytes from `v`: an ArrayBuffer (zero-copy) or a string (UTF-8 via
 * JS_ToCStringLen). On success sets data and len; `owned` is the cstring to
 * free (NULL for an ArrayBuffer). Returns 0, or -1 with a pending exception. */
static int dyn_text_bytes(JSContext *ctx, JSValueConst v, const uint8_t **data,
                          size_t *len, const char **owned)
{
    *owned = NULL;
    if (JS_IsString(v)) {
        const char *s = JS_ToCStringLen(ctx, len, v);
        if (!s)
            return -1;
        *owned = s;
        *data = (const uint8_t *)s;
        return 0;
    }
    {
        uint8_t *ab = JS_GetArrayBuffer(ctx, len, v);
        if (ab) {
            *data = ab;
            return 0;
        }
    }
    /* fall back to string coercion for other types (numbers, etc.) */
    {
        const char *s = JS_ToCStringLen(ctx, len, v);
        if (!s)
            return -1;
        *owned = s;
        *data = (const uint8_t *)s;
        return 0;
    }
}

/* count(text, ch) -> occurrences of byte `ch` in text. `ch` is a string (its
 * first byte) or a number (low 8 bits). */
static JSValue dyn_text_count(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    int32_t ch = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "count(text, ch)");
    if (JS_IsString(argv[1])) {
        size_t cl;
        const char *cs = JS_ToCStringLen(ctx, &cl, argv[1]);
        if (!cs)
            return JS_EXCEPTION;
        ch = cl > 0 ? (uint8_t)cs[0] : 0;
        JS_FreeCString(ctx, cs);
    } else if (JS_ToInt32(ctx, &ch, argv[1])) {
        return JS_EXCEPTION;
    }
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    {
        size_t c = simd.count_u8(data, (uint8_t)ch, len);
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_NewInt64(ctx, (int64_t)c);
    }
}

/* indexOfAny(text, chars) -> first byte index of any char in `chars`, or -1. */
static JSValue dyn_text_index_of_any(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const uint8_t *data, *set;
    size_t len, setlen, pos;
    const char *owned_data, *owned_set;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "indexOfAny(text, chars)");
    if (dyn_text_bytes(ctx, argv[1], &set, &setlen, &owned_set))
        return JS_EXCEPTION;
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned_data)) {
        if (owned_set)
            JS_FreeCString(ctx, owned_set);
        return JS_EXCEPTION;
    }
    pos = simd.find_first_of(data, len, set, setlen);
    if (owned_data)
        JS_FreeCString(ctx, owned_data);
    if (owned_set)
        JS_FreeCString(ctx, owned_set);
    return pos == SIZE_MAX ? JS_NewInt32(ctx, -1)
                           : JS_NewInt64(ctx, (int64_t)pos);
}

/* isValidUtf8(data) -> true if `data`'s bytes are well-formed UTF-8. */
static JSValue dyn_text_is_valid_utf8(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    int ok;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "isValidUtf8(data)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    ok = (simd.validate_utf8(data, len) == len);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewBool(ctx, ok);
}

/* base64Encode(data) -> standard base64 string ('+/' alphabet, '=' padded). */
static JSValue dyn_text_base64_encode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len, enc_cap, enc_len;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "base64Encode(data)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    enc_cap = 4 * ((len + 2) / 3);
    out = (char *)malloc(enc_cap ? enc_cap : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    enc_len = simd.base64_encode(data, len, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, enc_len);
    free(out);
    return result;
}

/* base64Decode(str) -> ArrayBuffer of the decoded bytes; throws on invalid. */
static JSValue dyn_text_base64_decode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *src;
    size_t n, dec_cap, dec_len;
    uint8_t *out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "base64Decode(str)");
    src = JS_ToCStringLen(ctx, &n, argv[0]); /* coerce first */
    if (!src)
        return JS_EXCEPTION;
    dec_cap = 3 * (n / 4);
    out = (uint8_t *)malloc(dec_cap ? dec_cap : 1);
    if (!out) {
        JS_FreeCString(ctx, src);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec_len = simd.base64_decode(src, n, out);
    JS_FreeCString(ctx, src);
    if (dec_len == SIZE_MAX) {
        free(out);
        return JS_ThrowTypeError(ctx, "base64Decode: invalid base64 input");
    }
    result = JS_NewArrayBufferCopy(ctx, out, dec_len);
    free(out);
    return result;
}

static const JSCFunctionListEntry dyn_text_funcs[] = {
    JS_CFUNC_DEF("count", 2, dyn_text_count),
    JS_CFUNC_DEF("indexOfAny", 2, dyn_text_index_of_any),
    JS_CFUNC_DEF("isValidUtf8", 1, dyn_text_is_valid_utf8),
    JS_CFUNC_DEF("base64Encode", 1, dyn_text_base64_encode),
    JS_CFUNC_DEF("base64Decode", 1, dyn_text_base64_decode),
};

static int dyn_text_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_text_funcs,
                                  (int)countof(dyn_text_funcs));
}

int js_nat_init_text(JSContext *ctx)
{
    JSModuleDef *m;
    simd_init(); /* idempotent: select best count/find_first_of/utf8 kernels */
    m = JS_NewCModule(ctx, "scl:text", dyn_text_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_text_funcs,
                                  (int)countof(dyn_text_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_TEXT */
