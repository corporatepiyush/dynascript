/*
 * dynajs:text -- SIMD-accelerated byte/text utilities over the shared kernel
 * engine (dynajs-simd-kernels.h). Self-contained, in-repo (no external deps).
 *
 *   import { count, indexOfAny, isValidUtf8, base64Encode, base64Decode }
 *       from "dynajs:text";
 *
 *   count("a,b,c,d", ",");                 // -> 3   (byte count; SIMD count_u8)
 *   indexOfAny("key: value", ":,\n");      // -> 3   (first delimiter; find_first_of)
 *   isValidUtf8(bytesArrayBuffer);          // -> true/false (SIMD-ASCII + DFA)
 *   base64Encode("hi");                     // -> "aGk="
 *   base64Decode("aGk=");                   // -> ArrayBuffer([0x68,0x69])
 *
 * Byte semantics: string arguments are materialised as UTF-8 bytes via
 * JS_ToCStringLen (offsets/counts are byte-based, matching dynajs:search).
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
#include <string.h>

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
        /* not an ArrayBuffer: clear the TypeError it raised before trying a
         * TypedArray view (else a stale exception leaks). */
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        /* a TypedArray/DataView (e.g. Uint8Array) view: take its raw bytes
         * through the backing buffer. The caller's argv still references the
         * view, so the buffer data stays live for the call after we drop our
         * local buffer ref. */
        size_t off = 0, blen = 0, bpe = 0;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
        if (!JS_IsException(buf)) {
            size_t absize = 0;
            uint8_t *ab = JS_GetArrayBuffer(ctx, &absize, buf);
            JS_FreeValue(ctx, buf);
            if (ab) {
                *data = ab + off;
                *len = blen;
                return 0;
            }
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
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

/* Build a fresh Uint8Array copying `len` bytes from `data` (never aliases a
 * native pointer into JS). `data` may be NULL only when len==0. */
static JSValue dyn_text_new_u8array(JSContext *ctx, const uint8_t *data,
                                    size_t len)
{
    static const uint8_t zero_stub = 0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (len == 0)
        data = &zero_stub; /* never pass NULL into JS_NewArrayBufferCopy */
    ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
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

/* hexEncode(data) -> lowercase hex string of the input bytes. */
static JSValue dyn_text_hex_encode(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "hexEncode(data)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (char *)malloc(len ? len * 2 : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    simd.hex_encode(data, len, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, len * 2);
    free(out);
    return result;
}

/* hexDecode(str) -> Uint8Array of the decoded bytes; throws on odd length or a
 * non-hex character. `str` is coerced to a string first (its UTF-8 bytes). */
static JSValue dyn_text_hex_decode(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *src;
    size_t n, dec_len;
    uint8_t *out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "hexDecode(str)");
    src = JS_ToCStringLen(ctx, &n, argv[0]); /* coerce first */
    if (!src)
        return JS_EXCEPTION;
    out = (uint8_t *)malloc(n / 2 ? n / 2 : 1);
    if (!out) {
        JS_FreeCString(ctx, src);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec_len = simd.hex_decode(src, n, out);
    JS_FreeCString(ctx, src);
    if (dec_len == SIZE_MAX) {
        free(out);
        return JS_ThrowTypeError(ctx, "hexDecode: invalid hex input");
    }
    result = dyn_text_new_u8array(ctx, out, dec_len);
    free(out);
    return result;
}

/* latin1ToUtf8(bytes) -> Uint8Array; each byte is a latin1 code point re-encoded
 * as UTF-8 (bytes <0x80 copy, 0x80..0xFF expand to two bytes). */
static JSValue dyn_text_latin1_to_utf8(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len, out_len;
    const char *owned;
    uint8_t *out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "latin1ToUtf8(bytes)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint8_t *)malloc(len ? len * 2 : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    out_len = simd.latin1_to_utf8(data, len, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = dyn_text_new_u8array(ctx, out, out_len);
    free(out);
    return result;
}

/* utf8ToLatin1(bytes) -> Uint8Array; throws RangeError if the input is invalid
 * UTF-8 or contains any code point > 0xFF (not representable in latin1). */
static JSValue dyn_text_utf8_to_latin1(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len, out_len = 0;
    const char *owned;
    uint8_t *out;
    JSValue result;
    int rc;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf8ToLatin1(bytes)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint8_t *)malloc(len ? len : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf8_to_latin1(data, len, out, &out_len);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (rc != 0) {
        free(out);
        return JS_ThrowRangeError(
            ctx, "utf8ToLatin1: invalid UTF-8 or code point > 0xFF");
    }
    result = dyn_text_new_u8array(ctx, out, out_len);
    free(out);
    return result;
}

/* countUtf8(data) -> number of UTF-8 code points (assumes valid UTF-8). */
static JSValue dyn_text_count_utf8(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "countUtf8(data)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    {
        size_t c = simd.count_utf8(data, len);
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_NewInt64(ctx, (int64_t)c);
    }
}

/* Borrow input as UTF-16 code units: read raw bytes via dyn_text_bytes, then
 * copy into a fresh 2-byte-aligned uint16_t buffer (a TypedArray view may start
 * at an odd byte offset, and the scalar kernels do aligned u16 reads). On
 * success returns the malloc'd buffer (release with free()), sets *units to
 * byte_len/2 and *odd to the trailing-odd-byte flag. Returns NULL with a
 * pending exception on error/OOM. A 0-unit input returns a valid non-NULL stub.
 * On this engine's little-endian targets the input bytes ARE UTF-16LE, so the
 * host-endian uint16_t copy is a straight reinterpretation. */
static uint16_t *dyn_text_u16_copy(JSContext *ctx, JSValueConst v, size_t *units,
                                   int *odd)
{
    const uint8_t *data;
    size_t len;
    const char *owned;
    uint16_t *u16;

    if (dyn_text_bytes(ctx, v, &data, &len, &owned))
        return NULL;
    *odd = (int)(len & 1);
    *units = len >> 1;
    u16 = (uint16_t *)malloc(*units ? *units * 2 : 2);
    if (!u16) {
        if (owned)
            JS_FreeCString(ctx, owned);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memcpy(u16, data, *units * 2); /* whole units only; a trailing odd byte drops */
    if (owned)
        JS_FreeCString(ctx, owned);
    return u16;
}

/* utf8ToUtf16(bytesOrString) -> Uint8Array of UTF-16LE bytes. Strict/lossless:
 * throws RangeError on malformed UTF-8 (matching simdutf's convert path). */
static JSValue dyn_text_utf8_to_utf16(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t len, out_units = 0;
    const char *owned;
    uint16_t *out;
    JSValue result;
    int rc;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf8ToUtf16(bytesOrString)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint16_t *)malloc(len ? len * 2 : 2); /* <= len units => 2*len bytes */
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf8_to_utf16le(data, len, out, &out_units);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (rc != 0) {
        free(out);
        return JS_ThrowRangeError(ctx, "utf8ToUtf16: invalid UTF-8");
    }
    result = dyn_text_new_u8array(ctx, (const uint8_t *)out, out_units * 2);
    free(out);
    return result;
}

/* utf16ToUtf8(u16bytes) -> Uint8Array of UTF-8 bytes. Strict/lossless: throws
 * RangeError on an odd byte length or an ill-formed surrogate. */
static JSValue dyn_text_utf16_to_utf8(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    size_t units = 0, out_len = 0;
    int odd, rc;
    uint16_t *u16;
    uint8_t *out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf16ToUtf8(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    if (odd) {
        free(u16);
        return JS_ThrowRangeError(ctx, "utf16ToUtf8: byte length must be even");
    }
    out = (uint8_t *)malloc(units ? units * 3 : 1); /* <= 3 UTF-8 bytes per unit */
    if (!out) {
        free(u16);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf16le_to_utf8(u16, units, out, &out_len);
    free(u16);
    if (rc != 0) {
        free(out);
        return JS_ThrowRangeError(ctx,
                                  "utf16ToUtf8: ill-formed UTF-16 surrogate");
    }
    result = dyn_text_new_u8array(ctx, out, out_len);
    free(out);
    return result;
}

/* isValidUtf16(u16bytes) -> true if the bytes are well-formed UTF-16LE (even
 * length, every high surrogate paired with a following low surrogate). */
static JSValue dyn_text_is_valid_utf16(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    size_t units = 0;
    int odd, ok;
    uint16_t *u16;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "isValidUtf16(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    ok = !odd && simd.validate_utf16le(u16, units); /* odd byte length is ill-formed */
    free(u16);
    return JS_NewBool(ctx, ok);
}

/* countUtf16(u16bytes) -> number of code points (surrogate pairs count once).
 * Does not validate; a trailing odd byte is ignored. */
static JSValue dyn_text_count_utf16(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    size_t units = 0, c;
    int odd;
    uint16_t *u16;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "countUtf16(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    c = simd.count_utf16(u16, units);
    free(u16);
    return JS_NewInt64(ctx, (int64_t)c);
}

static const JSCFunctionListEntry dyn_text_funcs[] = {
    JS_CFUNC_DEF("count", 2, dyn_text_count),
    JS_CFUNC_DEF("indexOfAny", 2, dyn_text_index_of_any),
    JS_CFUNC_DEF("isValidUtf8", 1, dyn_text_is_valid_utf8),
    JS_CFUNC_DEF("base64Encode", 1, dyn_text_base64_encode),
    JS_CFUNC_DEF("base64Decode", 1, dyn_text_base64_decode),
    JS_CFUNC_DEF("hexEncode", 1, dyn_text_hex_encode),
    JS_CFUNC_DEF("hexDecode", 1, dyn_text_hex_decode),
    JS_CFUNC_DEF("latin1ToUtf8", 1, dyn_text_latin1_to_utf8),
    JS_CFUNC_DEF("utf8ToLatin1", 1, dyn_text_utf8_to_latin1),
    JS_CFUNC_DEF("countUtf8", 1, dyn_text_count_utf8),
    JS_CFUNC_DEF("utf8ToUtf16", 1, dyn_text_utf8_to_utf16),
    JS_CFUNC_DEF("utf16ToUtf8", 1, dyn_text_utf16_to_utf8),
    JS_CFUNC_DEF("isValidUtf16", 1, dyn_text_is_valid_utf16),
    JS_CFUNC_DEF("countUtf16", 1, dyn_text_count_utf16),
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
    m = JS_NewCModule(ctx, "dynajs:text", dyn_text_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_text_funcs,
                                  (int)countof(dyn_text_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_TEXT */
