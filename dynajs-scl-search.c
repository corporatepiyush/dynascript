/*
 * scl:search -- native substring search backed by secure-c-libs (KMP).
 *
 *   import { indexOf, indexOfAll } from "scl:search";
 *   indexOf("the quick brown fox", "quick");   // -> 4   (first match, or -1)
 *   indexOfAll("abababab", "ab");               // -> [0, 2, 4, 6]
 *
 * Semantics (documented; these are the guarantees callers may rely on):
 *
 *   - OFFSETS ARE UTF-8 BYTE OFFSETS, not JS UTF-16 code-unit indices.
 *     JS_ToCStringLen materialises each string as UTF-8 bytes and the scl matcher
 *     reports byte positions. For ASCII input the two coincide, so indexOf equals
 *     String.prototype.indexOf; for non-ASCII they differ (e.g. for "hexllo" with
 *     a 2-byte 'x' the byte offset of a later match is one greater than the JS
 *     code-unit index). Explicit lengths are passed, so embedded NUL (U+0000) is
 *     searched correctly rather than truncating the string.
 *
 *   - indexOfAll enumerates OVERLAPPING matches (scl KMP resumes at lps[j-1]):
 *     indexOfAll("aaaa", "aa") -> [0, 1, 2], not [0, 2]. Offsets are ascending.
 *
 *   - Empty needle: indexOf("", ...)/indexOf(x, "") -> 0, mirroring
 *     String.prototype.indexOf(""); indexOfAll(x, "") -> [] (an empty needle has
 *     no substring occurrence to enumerate -- documented, and it also avoids an
 *     O(n) array of zero-width matches). The scl matcher rejects plen==0, so both
 *     are answered directly before it runs.
 *
 *   - Needle longer than the haystack, or no occurrence, or an empty haystack:
 *     indexOf -> -1, indexOfAll -> [].
 *
 * Memory discipline (see dynajs-scl.h): both string arguments are fully materialised
 * to C locals via JS_ToCStringLen BEFORE any search runs -- coercion may run
 * arbitrary JS (toString/valueOf), but each returned C string is owned by us and
 * stays valid until JS_FreeCString regardless of any JS that runs afterwards.
 * These are plain functions (no `this`), so there is no closable native resource
 * a re-entrant valueOf could free. The KMP scratch (prefix table) is allocated
 * from a per-call SCL arena that is destroyed before return; results are copied
 * out as plain JS numbers, so no arena pointer ever escapes. Every JS_ToCStringLen
 * result and the per-call arena are released on all paths.
 *
 * Boyer-Moore (scl_search_boyer_moore / _all, identical signatures) is a drop-in
 * alternative here -- a real implementation seam. KMP is used for both entry
 * points so indexOf and indexOfAll share one matcher with identical overlapping
 * semantics and KMP's guaranteed O(N+M) worst case (no adversarial-input blowup).
 */
#include "dynajs-scl.h"

#ifdef CONFIG_SCL_MODULES

#include "scl_kmp.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Coerce both JS string arguments to C (UTF-8 bytes, explicit lengths). On
 * failure the pending exception is left set, any partial result is freed, and
 * -1 is returned; on success both out strings must be released via JS_FreeCString.
 * Coercion runs arbitrary JS but there is no native resource to invalidate, and
 * the first string stays valid while the second is coerced. */
static int scl_search_coerce(JSContext *ctx, JSValueConst text_val,
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
static JSValue js_scl_search_index_of(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *text, *pat;
    size_t tlen, plen, pos = 0;
    scl_allocator_t *arena;
    scl_error_t err;
    JSValue result;

    (void)this_val;
    (void)argc;

    if (scl_search_coerce(ctx, argv[0], argv[1], &text, &tlen, &pat, &plen))
        return JS_EXCEPTION;

    if (plen == 0) {
        /* String.prototype.indexOf("") === 0 for any haystack. */
        result = JS_NewInt32(ctx, 0);
        goto done;
    }

    arena = js_scl_arena_new(ctx);
    if (!arena) {
        result = JS_EXCEPTION; /* OOM already thrown */
        goto done;
    }

    err = scl_search_kmp(arena, text, tlen, pat, plen, &pos);
    scl_alloc_arena_destroy(arena); /* KMP scratch reclaimed; pos is a copy */

    switch (err) {
    case SCL_OK:
        result = JS_NewInt64(ctx, (int64_t)pos);
        break;
    case SCL_ERR_NOT_FOUND: /* incl. needle longer than haystack */
    case SCL_ERR_EMPTY:     /* empty haystack, non-empty needle */
        result = JS_NewInt32(ctx, -1);
        break;
    case SCL_ERR_OUT_OF_MEMORY:
        result = JS_ThrowOutOfMemory(ctx);
        break;
    default:
        result = JS_ThrowInternalError(ctx, "scl:search indexOf: %s",
                                       scl_error_string(err));
        break;
    }

done:
    JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, pat);
    return result;
}

/* Accumulator for indexOfAll: appends each match offset to a JS array as it is
 * reported. `oom` records a JS allocation failure so the scan can stop early. */
typedef struct {
    JSContext *ctx;
    JSValue arr;
    uint32_t count;
    int oom;
} scl_search_collect_t;

/* Per-match callback. Stores no native data (offset copied as a JS number) and
 * runs no user JS (plain array index store), so it cannot re-enter and free
 * anything. Returns false to stop the scan on JS OOM. */
static bool scl_search_collect_cb(size_t pos, void *user)
{
    scl_search_collect_t *c = (scl_search_collect_t *)user;
    if (JS_DefinePropertyValueUint32(c->ctx, c->arr, c->count,
                                     JS_NewInt64(c->ctx, (int64_t)pos),
                                     JS_PROP_C_W_E) < 0) {
        c->oom = 1;
        return false;
    }
    c->count++;
    return true;
}

/* indexOfAll(haystack, needle) -> ascending array of byte offsets of every
 * (overlapping) match, or []. */
static JSValue js_scl_search_index_of_all(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    const char *text, *pat;
    size_t tlen, plen;
    scl_allocator_t *arena;
    scl_error_t err;
    scl_search_collect_t collect;
    JSValue result;

    (void)this_val;
    (void)argc;

    if (scl_search_coerce(ctx, argv[0], argv[1], &text, &tlen, &pat, &plen))
        return JS_EXCEPTION;

    collect.ctx = ctx;
    collect.count = 0;
    collect.oom = 0;
    collect.arr = JS_NewArray(ctx);
    if (JS_IsException(collect.arr)) {
        result = JS_EXCEPTION;
        goto done;
    }

    if (plen == 0) {
        /* Empty needle -> [] (documented; the matcher rejects plen==0 anyway). */
        result = collect.arr;
        goto done;
    }

    arena = js_scl_arena_new(ctx);
    if (!arena) {
        JS_FreeValue(ctx, collect.arr);
        result = JS_EXCEPTION; /* OOM already thrown */
        goto done;
    }

    err = scl_search_kmp_all(arena, text, tlen, pat, plen,
                             scl_search_collect_cb, &collect, NULL);
    scl_alloc_arena_destroy(arena); /* KMP scratch reclaimed; offsets copied */

    if (collect.oom) {
        JS_FreeValue(ctx, collect.arr);
        result = JS_ThrowOutOfMemory(ctx);
    } else if (err == SCL_OK || err == SCL_ERR_NOT_FOUND ||
               err == SCL_ERR_EMPTY) {
        result = collect.arr; /* possibly empty (no match / empty haystack) */
    } else {
        JS_FreeValue(ctx, collect.arr);
        result = (err == SCL_ERR_OUT_OF_MEMORY)
                     ? JS_ThrowOutOfMemory(ctx)
                     : JS_ThrowInternalError(ctx, "scl:search indexOfAll: %s",
                                             scl_error_string(err));
    }

done:
    JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, pat);
    return result;
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry js_scl_search_funcs[] = {
    JS_CFUNC_DEF("indexOf", 2, js_scl_search_index_of),
    JS_CFUNC_DEF("indexOfAll", 2, js_scl_search_index_of_all),
};

static int js_scl_search_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, js_scl_search_funcs,
                                  countof(js_scl_search_funcs));
}

int js_scl_init_search(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:search",
                                   js_scl_search_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, js_scl_search_funcs,
                                  countof(js_scl_search_funcs));
}

#endif /* CONFIG_SCL_MODULES */
