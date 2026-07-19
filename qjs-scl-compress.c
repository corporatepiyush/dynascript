/*
 * scl:compress -- native gzip (RFC 1952) compress/decompress backed by
 * secure-c-libs.
 *
 *   import { gzip, gunzip } from "scl:compress";
 *   const packed = gzip("hello world".repeat(100)); // str|Uint8Array|ArrayBuffer -> Uint8Array
 *   const bytes  = gunzip(packed);                   // -> Uint8Array
 *   const text   = gunzip(packed, { asString: true });// -> string (UTF-8 decode)
 *
 * These are TRANSIENT operations: a fresh SCL arena backs each call. Input bytes
 * are copied into arena memory FIRST (fully decoupled from the JS heap), the
 * codec compresses/decompresses into the same arena, then the result is COPIED
 * into an independent JS value (a Uint8Array over its own ArrayBuffer, or a
 * string). The arena is destroyed before returning on EVERY path -- success and
 * error -- so no native pointer ever escapes and RSS stays flat across calls.
 */
#include "qjs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_gzip.h"
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Default gzip effort. The SCL compressor treats level 0 as stored blocks and
 * any level > 1 as full hash-chain search (chain depth 64); 6 is the customary
 * zlib default and maps onto that best-effort path. */
#define JS_COMPRESS_DEFAULT_LEVEL 6

/* ---------- input: JS string / Uint8Array / ArrayBuffer -> arena bytes -------
 *
 * Copies the argument's bytes into freshly allocated arena memory and releases
 * every JS-side handle (the C string, the buffer reference) before returning, so
 * the pointer handed back is decoupled from the JS heap and stays valid until the
 * arena is destroyed. The copy also gives scl_gzip_* a non-NULL `src` even for
 * empty input (both codecs reject a NULL source). Returns 0 with *pdata (never
 * NULL) / *plen set, or -1 with a pending JS exception. */
static int js_compress_read_input(JSContext *ctx, JSValueConst val,
                                  scl_allocator_t *arena,
                                  const uint8_t **pdata, size_t *plen)
{
    const uint8_t *src;
    size_t len;
    JSValue bufval;
    uint8_t *base, *copy;
    size_t byte_off = 0, byte_len = 0, ab_size = 0, bytes_per_elem = 0;

    if (JS_IsString(val)) {
        size_t slen;
        const char *s = JS_ToCStringLen(ctx, &slen, val);
        if (!s)
            return -1;
        copy = (uint8_t *)scl_alloc(arena, slen ? slen : 1, JS_SCL_ARENA_ALIGN);
        if (!copy) {
            JS_FreeCString(ctx, s);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (slen)
            memcpy(copy, s, slen);
        JS_FreeCString(ctx, s);
        *pdata = copy;
        *plen = slen;
        return 0;
    }

    /* Try a typed array (Uint8Array & friends): borrow its backing buffer. */
    bufval = JS_GetTypedArrayBuffer(ctx, val, &byte_off, &byte_len,
                                    &bytes_per_elem);
    if (!JS_IsException(bufval)) {
        base = JS_GetArrayBuffer(ctx, &ab_size, bufval);
        if (!base) {
            JS_FreeValue(ctx, bufval); /* exception already set */
            return -1;
        }
        byte_len = scl_clamp_len(ab_size, byte_off, byte_len); /* defensive */
        src = base + byte_off;
        len = byte_len;
    } else {
        /* Not a typed array; clear that error and try a bare ArrayBuffer. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab_size, val);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_ThrowTypeError(ctx, "scl:compress: input must be a string, "
                                   "Uint8Array, or ArrayBuffer");
            return -1;
        }
        bufval = JS_UNDEFINED; /* no buffer reference to release */
        src = base;
        len = ab_size;
    }

    /* Copy into the arena, THEN drop the JS buffer reference. No native call
     * runs JS between fetching `src` and this memcpy, so the pointer is live. */
    copy = (uint8_t *)scl_alloc(arena, len ? len : 1, JS_SCL_ARENA_ALIGN);
    if (!copy) {
        JS_FreeValue(ctx, bufval);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (len)
        memcpy(copy, src, len);
    JS_FreeValue(ctx, bufval);
    *pdata = copy;
    *plen = len;
    return 0;
}

/* ---------- output: arena bytes -> independent JS Uint8Array ---------------- */

/* Copy `len` bytes into a fresh JS Uint8Array over its own ArrayBuffer. The
 * result is fully independent of the arena (JS_NewArrayBufferCopy duplicates the
 * bytes into JS-managed memory), so the caller may destroy the arena afterward. */
static JSValue js_compress_bytes_to_uint8(JSContext *ctx, const uint8_t *data,
                                          size_t len)
{
    static const uint8_t empty = 0;
    JSValue ab, u8;
    JSValueConst args[3];

    ab = JS_NewArrayBufferCopy(ctx, len ? data : &empty, len);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8); /* view over `ab` */
    JS_FreeValue(ctx, ab); /* u8 keeps its own reference to the buffer */
    return u8;
}

/* ---------- gzip / gunzip --------------------------------------------------- */

static JSValue js_scl_compress_gzip(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    const uint8_t *src = NULL;
    size_t src_len = 0;
    void *out = NULL;
    size_t out_len = 0;
    scl_error_t err;
    int level = JS_COMPRESS_DEFAULT_LEVEL;
    JSValue result;

    (void)this_val;

    /* Optional level (number, clamped 0..9). Coerce before the arena exists so a
     * throwing valueOf cannot strand it. */
    if (argc > 1 && JS_IsNumber(argv[1])) {
        int32_t lv;
        if (JS_ToInt32(ctx, &lv, argv[1]))
            return JS_EXCEPTION;
        level = lv < 0 ? 0 : (lv > 9 ? 9 : lv);
    }

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    if (js_compress_read_input(ctx, argv[0], arena, &src, &src_len) < 0) {
        scl_alloc_arena_destroy(arena);
        return JS_EXCEPTION;
    }

    err = scl_gzip_compress(arena, src, src_len, &out, &out_len, level);
    if (err == SCL_ERR_SIZE_OVERFLOW && level != 0) {
        /* libscl sizing quirk: the deflate buffer reserves the 10-byte gzip
         * header but not the 8-byte trailer, so fixed-Huffman output that lands
         * in that final 8-byte gap fits the writer yet trips the trailer check
         * -- and scl_gzip_compress reports it instead of falling back to stored.
         * Stored blocks (level 0) budget the trailer and always fit, so retry
         * there. Output stays valid, round-trippable gzip. */
        err = scl_gzip_compress(arena, src, src_len, &out, &out_len, 0);
    }
    if (err != SCL_OK) {
        scl_alloc_arena_destroy(arena);
        if (err == SCL_ERR_OUT_OF_MEMORY)
            return JS_ThrowOutOfMemory(ctx);
        return JS_ThrowInternalError(ctx, "scl:compress gzip: %s",
                                     scl_error_string(err));
    }

    /* Copy out BEFORE reclaiming the arena; free the native buffer for intent
     * (arena_free is a no-op, the destroy below reclaims everything). */
    result = js_compress_bytes_to_uint8(ctx, (const uint8_t *)out, out_len);
    scl_gzip_free_result(arena, &out, &out_len);
    scl_alloc_arena_destroy(arena);
    return result;
}

static JSValue js_scl_compress_gunzip(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    const uint8_t *src = NULL;
    size_t src_len = 0;
    void *out = NULL;
    size_t out_len = 0;
    scl_error_t err;
    int as_string = 0;
    JSValue result;

    (void)this_val;

    /* Optional { asString: true } -> UTF-8 decode to a JS string. Read the
     * (possibly getter-backed) property before the arena exists. */
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }

    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    if (js_compress_read_input(ctx, argv[0], arena, &src, &src_len) < 0) {
        scl_alloc_arena_destroy(arena);
        return JS_EXCEPTION;
    }

    err = scl_gzip_decompress(arena, src, src_len, &out, &out_len);
    if (err != SCL_OK) {
        /* Malformed/short/corrupt gzip: the codec freed its own scratch and
         * left *out NULL. Throw a clean Error; nothing leaks. */
        scl_alloc_arena_destroy(arena);
        return JS_ThrowTypeError(ctx, "scl:compress gunzip: invalid gzip data (%s)",
                                 scl_error_string(err));
    }

    if (as_string)
        result = JS_NewStringLen(ctx, out_len ? (const char *)out : "", out_len);
    else
        result = js_compress_bytes_to_uint8(ctx, (const uint8_t *)out, out_len);
    scl_gzip_free_result(arena, &out, &out_len);
    scl_alloc_arena_destroy(arena);
    return result;
}

/* ---------- module registration -------------------------------------------- */

static const JSCFunctionListEntry js_scl_compress_funcs[] = {
    JS_CFUNC_DEF("gzip", 1, js_scl_compress_gzip),
    JS_CFUNC_DEF("gunzip", 1, js_scl_compress_gunzip),
};

static int js_scl_compress_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, js_scl_compress_funcs,
                                  countof(js_scl_compress_funcs));
}

int js_scl_init_compress(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:compress",
                                   js_scl_compress_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, js_scl_compress_funcs,
                                  countof(js_scl_compress_funcs));
}

#endif /* CONFIG_SCL_MODULES */
