/*
 * dynajs:search -- native substring search. Self-contained, in-repo (no external
 * deps), Boyer-Moore-Horspool matcher.
 *
 *   import { indexOf, indexOfAll } from "dynajs:search";
 *   indexOf("the quick brown fox", "quick");   // -> 4   (first match, or -1)
 *   indexOfAll("abababab", "ab");               // -> [0, 2, 4, 6]
 *
 * Semantics (documented guarantees callers may rely on):
 *
 *   - OFFSETS ARE UTF-8 BYTE OFFSETS, not JS UTF-16 code-unit indices.
 *     JS_ToCStringLen materialises each string as UTF-8 bytes and the matcher
 *     reports byte positions. For ASCII input the two coincide, so indexOf equals
 *     String.prototype.indexOf; for non-ASCII they differ. Explicit lengths are
 *     passed, so an embedded NUL (U+0000) is searched, not treated as a
 *     terminator.
 *
 *   - indexOfAll enumerates OVERLAPPING matches (each match advances the scan by
 *     one byte): indexOfAll("aaaa", "aa") -> [0, 1, 2], not [0, 2]. Ascending.
 *
 *   - Empty needle: indexOf(x, "") -> 0, mirroring String.prototype.indexOf("");
 *     indexOfAll(x, "") -> [] (an empty needle has no substring occurrence to
 *     enumerate -- documented, and it avoids an O(n) array of zero-width matches).
 *
 *   - Needle longer than the haystack, or no occurrence, or an empty haystack:
 *     indexOf -> -1, indexOfAll -> [].
 *
 * Memory discipline: both string arguments are fully materialised to owned C
 * strings via JS_ToCStringLen BEFORE any search runs -- coercion may run
 * arbitrary JS (toString/valueOf), but each returned C string is ours and stays
 * valid until JS_FreeCString regardless of any JS run afterwards. These are plain
 * functions (no `this`), so there is no closable native resource a re-entrant
 * valueOf could free. Results are copied out as plain JS numbers; every
 * JS_ToCStringLen result is released on every path.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SEARCH)

#include <stddef.h>
#include <stdint.h>

#include "dynajs-simd-kernels.h" /* the shared multi-ISA SIMD engine */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* First occurrence of pat[0..plen) in text[start..tlen) via the SIMD substring
 * kernel (first+last algorithm; scalar memchr fallback). Returns the absolute
 * byte offset, or (size_t)-1 if none. Requires 1 <= plen <= tlen. */
static size_t simd_substr_find(const uint8_t *text, size_t tlen,
                               const uint8_t *pat, size_t plen, size_t start)
{
    size_t rel;
    if (start >= tlen || plen > tlen - start)
        return (size_t)-1;
    rel = simd.strfind(text + start, tlen - start, pat, plen);
    return rel == SIZE_MAX ? (size_t)-1 : start + rel;
}

/* Coerce both JS string arguments to owned C strings (UTF-8 bytes, explicit
 * lengths). On failure leaves the pending exception, frees any partial result,
 * and returns -1; on success both must be released via JS_FreeCString. */
static int dyn_search_coerce(JSContext *ctx, JSValueConst text_val,
                             JSValueConst pat_val, const char **text,
                             size_t *tlen, const char **pat, size_t *plen)
{
    *text = JS_ToCStringLen(ctx, tlen, text_val);
    if (!*text) {
        *pat = NULL;
        return -1;
    }
    *pat = JS_ToCStringLen(ctx, plen, pat_val);
    if (!*pat) {
        JS_FreeCString(ctx, *text);
        *text = NULL;
        return -1;
    }
    return 0;
}

/* indexOf(haystack, needle) -> byte offset of first match, or -1. */
static JSValue dyn_search_index_of(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *text, *pat;
    size_t tlen, plen, pos;
    JSValue result;

    (void)this_val;
    (void)argc;

    if (dyn_search_coerce(ctx, argv[0], argv[1], &text, &tlen, &pat, &plen))
        return JS_EXCEPTION;

    if (plen == 0) {
        result = JS_NewInt32(ctx, 0); /* indexOf("") === 0 for any haystack */
    } else if (plen > tlen) {
        result = JS_NewInt32(ctx, -1);
    } else {
        pos = simd_substr_find((const uint8_t *)text, tlen,
                               (const uint8_t *)pat, plen, 0);
        result = (pos == (size_t)-1) ? JS_NewInt32(ctx, -1)
                                     : JS_NewInt64(ctx, (int64_t)pos);
    }

    JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, pat);
    return result;
}

/* indexOfAll(haystack, needle) -> ascending array of byte offsets of every
 * (overlapping) match, or []. */
static JSValue dyn_search_index_of_all(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const char *text, *pat;
    size_t tlen, plen, pos;
    JSValue arr, result;
    uint32_t count = 0;

    (void)this_val;
    (void)argc;

    if (dyn_search_coerce(ctx, argv[0], argv[1], &text, &tlen, &pat, &plen))
        return JS_EXCEPTION;

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        result = JS_EXCEPTION;
        goto done;
    }

    /* Empty needle -> []; needle longer than haystack -> []. Both skip the scan
     * and yield the empty array built above. */
    if (plen == 0 || plen > tlen) {
        result = arr;
        goto done;
    }

    pos = simd_substr_find((const uint8_t *)text, tlen, (const uint8_t *)pat,
                           plen, 0);
    while (pos != (size_t)-1) {
        if (JS_DefinePropertyValueUint32(ctx, arr, count,
                                         JS_NewInt64(ctx, (int64_t)pos),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            result = JS_EXCEPTION;
            goto done;
        }
        count++;
        /* Advance by one byte for overlapping matches. */
        pos = simd_substr_find((const uint8_t *)text, tlen,
                               (const uint8_t *)pat, plen, pos + 1);
    }
    result = arr;

 done:
    JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, pat);
    return result;
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry dyn_search_funcs[] = {
    JS_CFUNC_DEF("indexOf", 2, dyn_search_index_of),
    JS_CFUNC_DEF("indexOfAll", 2, dyn_search_index_of_all),
};

static int dyn_search_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_search_funcs,
                                  countof(dyn_search_funcs));
}

int js_nat_init_search(JSContext *ctx)
{
    JSModuleDef *m;
    simd_init(); /* idempotent (pthread_once): select the best strfind kernel */
    m = JS_NewCModule(ctx, "dynajs:search", dyn_search_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_search_funcs,
                                  countof(dyn_search_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SEARCH */
