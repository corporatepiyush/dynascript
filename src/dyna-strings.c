/*
 * dyna:strings -- string utility library mirroring Go's `strings` package
 * plus a few JS String helpers, in dynajs style. Self-contained, in-repo (no
 * external deps beyond the engine's own SIMD kernel table and Unicode tables,
 * both already linked into every build).
 *
 *   import { split, join, trim, toUpper, hasPrefix, replaceAll }
 *       from "dyna:strings";
 *
 *   split("a,b,,c", ",");            // -> ["a","b","","c"]
 *   join(["a","b","c"], "-");        // -> "a-b-c"
 *   trim("  hi  ");                  // -> "hi"
 *   hasPrefix("golang", "go");       // -> true
 *   replaceAll("a.b.c", ".", "/");   // -> "a/b/c"
 *
 * Semantics (documented guarantees callers may rely on):
 *
 *   - Byte-oriented functions (split/index/lastIndex/count/replace/contains/
 *     hasPrefix/hasSuffix/trimPrefix/trimSuffix/equalFold/compare) operate on
 *     UTF-8 BYTES and report/accept BYTE offsets and lengths, exactly like
 *     Go's strings package operates on byte strings (see dyna:search's
 *     docs for the same ASCII-vs-non-ASCII point re: JS UTF-16 indices).
 *   - Code-point-aware functions (trim/trimStart/trimEnd/trimChars/fields/
 *     toUpper/toLower/title, and the split(s,"") rune-split case) decode
 *     UTF-8 and operate on Unicode code points using the engine's own tables
 *     (libunicode.h: lre_is_space/lre_case_conv/...), so they agree with
 *     this engine's native String.prototype.trim/toUpperCase/toLowerCase.
 *   - padStart/padEnd measure `len` in Unicode CODE POINTS (this module's
 *     strings don't carry a UTF-16 length; code points are the natural
 *     analogue for a byte-string library, and coincide with JS's UTF-16
 *     length for all non-astral text).
 *   - title() mirrors Go's (deprecated, quirky-by-design) strings.Title
 *     algorithm exactly for ASCII, including the well-known "it's" ->
 *     "It'S" behavior (a word boundary is "previous rune is a separator",
 *     not "this rune is a letter"). For non-ASCII it approximates Go's
 *     unicode.IsPunct/IsSymbol word-boundary test with "not an identifier
 *     character" (no such category table is exposed by this engine), and
 *     approximates true Unicode titlecase with uppercase (the two differ
 *     only for a handful of digraph/ligature code points).
 *   - equalFold is documented ASCII-only case folding (NOT Go's full
 *     Unicode simple case folding), per this module's contract.
 *   - count/replace/replaceAll use Go's NON-overlapping match semantics
 *     (contrast with dyna:search's indexOfAll, which is overlapping).
 *
 * Memory discipline: every JS argument is fully coerced to an owned C value
 * (JS_ToCStringLen / JS_ToInt32) BEFORE any allocation runs -- coercion may
 * run arbitrary JS (toString/valueOf), but these are plain functions with no
 * closable resource, so there is nothing a reentrant coercion could
 * invalidate. Every owned C string / scratch buffer is freed on every path,
 * including every error path. Results are always fresh JS values
 * (JS_NewStringLen / JS_NewArray) -- nothing native escapes.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRINGS)

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dyna-simd-kernels.h" /* the shared multi-ISA SIMD engine */

/* libunicode.h's lre_js_is_ident_next() references TRUE without defining it
 * itself -- normally supplied transitively via cutils.h, which native
 * modules avoid including (its container_of/likely can clash with the old
 * scl binding framework's headers elsewhere in the tree). */
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#include "libunicode.h" /* lre_is_space / lre_case_conv / ... */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* magic values shared by a handful of prefix/suffix/direction-style pairs of
 * exported functions that differ only in which end/case they act on. */
#define DYN_TRIM_START   1
#define DYN_TRIM_END     2
#define DYN_TRIM_BOTH    (DYN_TRIM_START | DYN_TRIM_END)
#define DYN_AFFIX_PREFIX 0
#define DYN_AFFIX_SUFFIX 1
#define DYN_PAD_AT_END   0
#define DYN_PAD_AT_START 1
#define DYN_ANY_INDEX    0
#define DYN_ANY_CONTAINS 1

/* ================================================================ *
 *  UTF-8 codec (byte offsets <-> Unicode code points)
 * ================================================================ */

/* Decode one UTF-8 code point at s[*pos] (precondition: *pos < len) and
 * advance *pos past it. Lenient: a malformed/truncated sequence (never
 * produced by JS_ToCStringLen, but handled defensively) decodes as its raw
 * byte value and advances by exactly 1, so this never infinite-loops. */
static uint32_t dyn_utf8_next(const uint8_t *s, size_t len, size_t *pos)
{
    uint8_t c0 = s[*pos];
    uint32_t cp;
    int extra, i;

    if (c0 < 0x80) {
        (*pos)++;
        return c0;
    }
    if ((c0 & 0xe0) == 0xc0) { cp = c0 & 0x1f; extra = 1; }
    else if ((c0 & 0xf0) == 0xe0) { cp = c0 & 0x0f; extra = 2; }
    else if ((c0 & 0xf8) == 0xf0) { cp = c0 & 0x07; extra = 3; }
    else {
        (*pos)++;
        return c0;
    }
    if (*pos + (size_t)extra >= len) {
        (*pos)++;
        return c0;
    }
    for (i = 1; i <= extra; i++) {
        uint8_t cc = s[*pos + (size_t)i];
        if ((cc & 0xc0) != 0x80) {
            (*pos)++;
            return c0;
        }
        cp = (cp << 6) | (uint32_t)(cc & 0x3f);
    }
    *pos += (size_t)extra + 1;
    return cp;
}

/* Byte offset of the code point boundary immediately before `pos` (pos must
 * be > 0 and itself a valid boundary). Walks back over UTF-8 continuation
 * bytes (10xxxxxx); never underflows past 0. */
static size_t dyn_utf8_prev_pos(const uint8_t *s, size_t pos)
{
    size_t start = pos - 1;
    while (start > 0 && (s[start] & 0xc0) == 0x80)
        start--;
    return start;
}

/* Encode code point cp as UTF-8 into buf (needs up to 4 bytes of room);
 * returns the byte count written (1-4). */
static int dyn_utf8_put(uint8_t *buf, uint32_t cp)
{
    if (cp < 0x80) {
        buf[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (uint8_t)(0xc0 | (cp >> 6));
        buf[1] = (uint8_t)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (uint8_t)(0xe0 | (cp >> 12));
        buf[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
        buf[2] = (uint8_t)(0x80 | (cp & 0x3f));
        return 3;
    }
    buf[0] = (uint8_t)(0xf0 | (cp >> 18));
    buf[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
    buf[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
    buf[3] = (uint8_t)(0x80 | (cp & 0x3f));
    return 4;
}

/* ================================================================ *
 *  Argument coercion (mirrors dyna-search.c's dyn_search_coerce)
 * ================================================================ */

static int dyn_str_coerce2(JSContext *ctx, JSValueConst a_val, JSValueConst b_val,
                           const char **a, size_t *alen,
                           const char **b, size_t *blen)
{
    *a = JS_ToCStringLen(ctx, alen, a_val);
    if (!*a) {
        *b = NULL;
        return -1;
    }
    *b = JS_ToCStringLen(ctx, blen, b_val);
    if (!*b) {
        JS_FreeCString(ctx, *a);
        *a = NULL;
        return -1;
    }
    return 0;
}

static int dyn_str_coerce3(JSContext *ctx, JSValueConst a_val, JSValueConst b_val,
                           JSValueConst c_val, const char **a, size_t *alen,
                           const char **b, size_t *blen,
                           const char **c, size_t *clen)
{
    if (dyn_str_coerce2(ctx, a_val, b_val, a, alen, b, blen)) {
        *c = NULL;
        return -1;
    }
    *c = JS_ToCStringLen(ctx, clen, c_val);
    if (!*c) {
        JS_FreeCString(ctx, *a);
        JS_FreeCString(ctx, *b);
        *a = NULL;
        *b = NULL;
        return -1;
    }
    return 0;
}

static void dyn_str_free2(JSContext *ctx, const char *a, const char *b)
{
    JS_FreeCString(ctx, a);
    JS_FreeCString(ctx, b);
}

static void dyn_str_free3(JSContext *ctx, const char *a, const char *b, const char *c)
{
    JS_FreeCString(ctx, a);
    JS_FreeCString(ctx, b);
    JS_FreeCString(ctx, c);
}

/* ================================================================ *
 *  Substring search (byte-level; forward via the shared SIMD kernel,
 *  backward via a direct scan -- no reverse SIMD kernel exists).
 * ================================================================ */

/* First occurrence of pat[0..plen) in text[start..tlen), or SIZE_MAX.
 * Requires 1 <= plen <= tlen - start (checked here; callers may pass an
 * out-of-range start/plen freely). */
static size_t dyn_str_find(const uint8_t *text, size_t tlen, const uint8_t *pat,
                           size_t plen, size_t start)
{
    size_t rel;
    if (start >= tlen || plen > tlen - start)
        return (size_t)-1;
    rel = simd.strfind(text + start, tlen - start, pat, plen);
    return rel == SIZE_MAX ? (size_t)-1 : start + rel;
}

/* Last occurrence of pat[0..plen) in text[0..tlen), or SIZE_MAX. Requires
 * plen >= 1. A plain backward memcmp scan -- fine for a utility function,
 * not a hot loop, and there is no reverse SIMD kernel (yet) to call. */
static size_t dyn_str_find_last(const uint8_t *text, size_t tlen,
                                const uint8_t *pat, size_t plen)
{
    size_t pos;
    if (plen > tlen)
        return (size_t)-1;
    pos = tlen - plen;
    for (;;) {
        if (memcmp(text + pos, pat, plen) == 0)
            return pos;
        if (pos == 0)
            break;
        pos--;
    }
    return (size_t)-1;
}

static int dyn_str_all_ascii(const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (b[i] >= 0x80)
            return 0;
    return 1;
}

/* ================================================================ *
 *  Trim: bidirectional code-point walk parameterised by a predicate.
 *  Used by trim/trimStart/trimEnd (predicate = lre_is_space) and by
 *  trimChars (predicate = membership in a decoded rune set).
 * ================================================================ */

typedef int (*DynCpPred)(void *opaque, uint32_t cp);

static int dyn_pred_is_space(void *opaque, uint32_t cp)
{
    (void)opaque;
    return lre_is_space(cp);
}

typedef struct {
    const uint32_t *set;
    size_t n;
} DynCpSet;

static int dyn_pred_in_set(void *opaque, uint32_t cp)
{
    const DynCpSet *set = opaque;
    size_t i;
    for (i = 0; i < set->n; i++)
        if (set->set[i] == cp)
            return 1;
    return 0;
}

/* [*pstart,*pend) is s[0,slen) with leading (mode&DYN_TRIM_START) and/or
 * trailing (mode&DYN_TRIM_END) code points satisfying `pred` removed. */
static void dyn_str_trim_span(const uint8_t *s, size_t slen, int mode,
                              DynCpPred pred, void *opaque,
                              size_t *pstart, size_t *pend)
{
    size_t start = 0, end = slen;

    if (mode & DYN_TRIM_START) {
        while (start < end) {
            size_t save = start;
            uint32_t cp = dyn_utf8_next(s, slen, &start);
            if (!pred(opaque, cp)) {
                start = save;
                break;
            }
        }
    }
    if (mode & DYN_TRIM_END) {
        while (end > start) {
            size_t prev = dyn_utf8_prev_pos(s, end);
            size_t tmp = prev;
            uint32_t cp = dyn_utf8_next(s, slen, &tmp);
            if (!pred(opaque, cp))
                break;
            end = prev;
        }
    }
    *pstart = start;
    *pend = end;
}

/* ================================================================ *
 *  split / splitN (Go's genSplit + explode, expressed with an explicit
 *  "force the remainder to be the last piece" flag instead of a literal
 *  port of Go's recursive counting).
 * ================================================================ */

/* sep == "": split after each UTF-8 code point, at most `limit` pieces
 * (limit<0 = unbounded); the LAST piece absorbs any remainder. */
static JSValue dyn_str_split_runes(JSContext *ctx, const uint8_t *s, size_t slen,
                                   int64_t limit)
{
    JSValue arr;
    size_t pos = 0;
    uint32_t idx = 0;

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || slen == 0)
        return arr;
    while (pos < slen) {
        size_t start = pos;
        JSValue piece;

        if (limit >= 0 && (int64_t)idx == limit - 1)
            pos = slen;
        else
            dyn_utf8_next(s, slen, &pos);
        piece = JS_NewStringLen(ctx, (const char *)s + start, pos - start);
        if (JS_DefinePropertyValueUint32(ctx, arr, idx, piece, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        idx++;
    }
    return arr;
}

/* sep != "": split on non-overlapping occurrences of sep, at most `limit`
 * pieces (limit<0 = unbounded); the LAST piece is the unsplit remainder. */
static JSValue dyn_str_split_sep(JSContext *ctx, const uint8_t *s, size_t slen,
                                 const uint8_t *sep, size_t seplen, int64_t limit)
{
    JSValue arr;
    size_t pos = 0;
    uint32_t idx = 0;

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (;;) {
        size_t m;
        JSValue piece;

        if (limit >= 0 && (int64_t)idx == limit - 1)
            m = (size_t)-1;
        else
            m = dyn_str_find(s, slen, sep, seplen, pos);
        if (m == (size_t)-1) {
            piece = JS_NewStringLen(ctx, (const char *)s + pos, slen - pos);
            if (JS_DefinePropertyValueUint32(ctx, arr, idx, piece, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            return arr;
        }
        piece = JS_NewStringLen(ctx, (const char *)s + pos, m - pos);
        if (JS_DefinePropertyValueUint32(ctx, arr, idx, piece, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        idx++;
        pos = m + seplen;
    }
}

static JSValue dyn_str_split_core(JSContext *ctx, const uint8_t *s, size_t slen,
                                  const uint8_t *sep, size_t seplen, int64_t limit)
{
    if (limit == 0)
        return JS_NewArray(ctx);
    if (seplen == 0)
        return dyn_str_split_runes(ctx, s, slen, limit);
    return dyn_str_split_sep(ctx, s, slen, sep, seplen, limit);
}

/* ================================================================ *
 *  fields: split on runs of whitespace code points (Go's strings.Fields,
 *  using this engine's own lre_is_space so it agrees with trim()).
 * ================================================================ */

static JSValue dyn_str_fields(JSContext *ctx, const uint8_t *s, size_t slen)
{
    JSValue arr;
    size_t pos = 0;
    uint32_t idx = 0;

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (;;) {
        size_t start, mark;
        uint32_t cp;

        while (pos < slen) {
            mark = pos;
            cp = dyn_utf8_next(s, slen, &pos);
            if (!lre_is_space(cp)) {
                pos = mark;
                break;
            }
        }
        if (pos >= slen)
            break;
        start = pos;
        while (pos < slen) {
            mark = pos;
            cp = dyn_utf8_next(s, slen, &pos);
            if (lre_is_space(cp)) {
                pos = mark;
                break;
            }
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, idx++,
                JS_NewStringLen(ctx, (const char *)s + start, pos - start),
                JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

/* ================================================================ *
 *  toUpper / toLower: decode -> per-code-point lre_case_conv -> re-encode.
 *  Mirrors the engine's own js_string_toLowerCase, including the Greek
 *  final-sigma special case for lowercasing.
 * ================================================================ */

/* True if cps[pos] (a capital sigma, U+03A3) is preceded -- skipping
 * case-ignorable code points -- by a cased letter, and NOT followed
 * (likewise skipping case-ignorable ones) by another cased letter. */
static int dyn_str_is_final_sigma(const uint32_t *cps, size_t n, size_t pos)
{
    size_t k;
    int seen_cased = 0;

    k = pos;
    while (k > 0) {
        k--;
        if (!lre_is_case_ignorable(cps[k])) {
            seen_cased = lre_is_cased(cps[k]);
            break;
        }
    }
    if (!seen_cased)
        return 0;
    for (k = pos + 1; k < n; k++) {
        if (!lre_is_case_ignorable(cps[k]))
            return !lre_is_cased(cps[k]);
    }
    return 1;
}

static JSValue dyn_str_case_convert(JSContext *ctx, const uint8_t *s, size_t slen,
                                    int to_lower)
{
    uint32_t *cps;
    uint8_t *out;
    size_t n = 0, pos, i, out_cap, out_len = 0;
    JSValue result;

    if (slen == 0)
        return JS_NewStringLen(ctx, "", 0);

    cps = js_malloc(ctx, sizeof(*cps) * slen); /* <= 1 code point per byte */
    if (!cps)
        return JS_EXCEPTION;
    pos = 0;
    while (pos < slen)
        cps[n++] = dyn_utf8_next(s, slen, &pos);

    /* lre_case_conv can return up to LRE_CC_RES_LEN_MAX code points, each up
     * to 4 UTF-8 bytes. */
    out_cap = n * (size_t)LRE_CC_RES_LEN_MAX * 4 + 1;
    out = js_malloc(ctx, out_cap);
    if (!out) {
        js_free(ctx, cps);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        uint32_t res[LRE_CC_RES_LEN_MAX];
        int l, j;

        if (to_lower && cps[i] == 0x3a3 && dyn_str_is_final_sigma(cps, n, i)) {
            res[0] = 0x3c2; /* final sigma */
            l = 1;
        } else {
            l = lre_case_conv(res, cps[i], to_lower);
        }
        for (j = 0; j < l; j++)
            out_len += (size_t)dyn_utf8_put(out + out_len, res[j]);
    }
    js_free(ctx, cps);
    result = JS_NewStringLen(ctx, (const char *)out, out_len);
    js_free(ctx, out);
    return result;
}

/* ================================================================ *
 *  title: Go's strings.Title algorithm (deprecated upstream, kept here for
 *  parity -- word boundary is "the PREVIOUS rune was a separator", which is
 *  what produces the well-known "it's" -> "It'S" behavior).
 * ================================================================ */

/* Go's isSeparator: ASCII digit/letter/underscore are never separators,
 * every other ASCII byte is; non-ASCII separators are whitespace or
 * punctuation/symbols. This engine doesn't expose an IsPunct/IsSymbol
 * table, so non-ASCII approximates "not an identifier character" (letters,
 * digits, marks, connector punctuation) minus whitespace. */
static int dyn_str_title_is_sep(uint32_t r)
{
    if (r < 128) {
        if ((r >= '0' && r <= '9') || (r >= 'a' && r <= 'z') ||
            (r >= 'A' && r <= 'Z') || r == '_')
            return 0;
        return 1;
    }
    if (lre_is_space(r))
        return 1;
    return !(lre_is_id_start(r) || lre_is_id_continue(r));
}

static JSValue dyn_str_title(JSContext *ctx, const uint8_t *s, size_t slen)
{
    uint8_t *out;
    size_t out_cap, out_len = 0, pos = 0;
    uint32_t prev = ' '; /* Go's Title starts as if preceded by a separator */
    JSValue result;

    if (slen == 0)
        return JS_NewStringLen(ctx, "", 0);
    out_cap = slen * (size_t)LRE_CC_RES_LEN_MAX * 4 + 1;
    out = js_malloc(ctx, out_cap);
    if (!out)
        return JS_EXCEPTION;
    while (pos < slen) {
        uint32_t cp = dyn_utf8_next(s, slen, &pos);

        if (dyn_str_title_is_sep(prev)) {
            uint32_t res[LRE_CC_RES_LEN_MAX];
            int l = lre_case_conv(res, cp, 0 /* uppercase approximates titlecase */);
            int j;
            for (j = 0; j < l; j++)
                out_len += (size_t)dyn_utf8_put(out + out_len, res[j]);
        } else {
            out_len += (size_t)dyn_utf8_put(out + out_len, cp);
        }
        prev = cp;
    }
    result = JS_NewStringLen(ctx, (const char *)out, out_len);
    js_free(ctx, out);
    return result;
}

/* ================================================================ *
 *  repeat
 * ================================================================ */

static JSValue dyn_str_repeat(JSContext *ctx, const uint8_t *s, size_t slen,
                              int32_t count)
{
    uint8_t *out;
    size_t total, i;
    JSValue result;

    if (count < 0)
        return JS_ThrowRangeError(ctx, "repeat: count must be non-negative");
    if (count == 0 || slen == 0)
        return JS_NewStringLen(ctx, "", 0);
    if ((size_t)count > SIZE_MAX / slen)
        return JS_ThrowRangeError(ctx, "repeat: result too large");
    total = slen * (size_t)count;
    out = js_malloc(ctx, total);
    if (!out)
        return JS_EXCEPTION;
    for (i = 0; i < (size_t)count; i++)
        memcpy(out + i * slen, s, slen);
    result = JS_NewStringLen(ctx, (const char *)out, total);
    js_free(ctx, out);
    return result;
}

/* ================================================================ *
 *  padStart / padEnd -- `target` is measured in Unicode code points.
 * ================================================================ */

static JSValue dyn_str_pad(JSContext *ctx, const uint8_t *s, size_t slen,
                           int32_t target, const uint8_t *pad, size_t padlen,
                           int at_start)
{
    size_t have = 0, pos, need, i, pad_n = 0;
    size_t *pad_offs;
    uint8_t *out;
    size_t out_len;
    JSValue result;

    /* Count s's code points, stopping early once `have` already reaches
     * target (at that point s needs no padding regardless of its true
     * length, so the exact count beyond target is never needed). */
    if (target > 0) {
        pos = 0;
        while (pos < slen) {
            dyn_utf8_next(s, slen, &pos);
            have++;
            if (have >= (size_t)target)
                break;
        }
    }
    if (target <= 0 || (size_t)target <= have || padlen == 0)
        return JS_NewStringLen(ctx, (const char *)s, slen);
    need = (size_t)target - have;

    /* offsets of each code point in `pad` (pad_offs[pad_n] == padlen), so
     * cycling through them for `need` copies can memcpy the ORIGINAL bytes
     * verbatim instead of re-encoding. */
    pad_offs = js_malloc(ctx, sizeof(*pad_offs) * (padlen + 1));
    if (!pad_offs)
        return JS_EXCEPTION;
    pos = 0;
    while (pos < padlen) {
        pad_offs[pad_n++] = pos;
        dyn_utf8_next(pad, padlen, &pos);
    }
    pad_offs[pad_n] = padlen;

    out = js_malloc(ctx, slen + need * 4 /* max UTF-8 bytes per code point */);
    if (!out) {
        js_free(ctx, pad_offs);
        return JS_EXCEPTION;
    }

    out_len = 0;
    if (!at_start) {
        memcpy(out, s, slen);
        out_len = slen;
    }
    for (i = 0; i < need; i++) {
        size_t j = i % pad_n;
        size_t clen = pad_offs[j + 1] - pad_offs[j];
        memcpy(out + out_len, pad + pad_offs[j], clen);
        out_len += clen;
    }
    if (at_start) {
        memcpy(out + out_len, s, slen);
        out_len += slen;
    }

    js_free(ctx, pad_offs);
    result = JS_NewStringLen(ctx, (const char *)out, out_len);
    js_free(ctx, out);
    return result;
}

/* ================================================================ *
 *  indexAny / containsAny: first code point in s matching any code point
 *  in `chars`. Pure-ASCII needles delegate to the SIMD byte-set kernel
 *  (exact, since single-byte code points can't collide with continuation
 *  bytes); a multi-byte needle set decodes once and scans s rune-by-rune,
 *  since treating multi-byte code points as an independent byte pool would
 *  false-positive on shared continuation bytes.
 * ================================================================ */

static size_t dyn_str_index_any(JSContext *ctx, const uint8_t *s, size_t slen,
                                const uint8_t *chars, size_t charslen, int *err)
{
    uint32_t *set;
    size_t set_n, pos;

    *err = 0;
    if (slen == 0 || charslen == 0)
        return SIZE_MAX;
    if (dyn_str_all_ascii(chars, charslen))
        return simd.find_first_of(s, slen, chars, charslen);

    set = js_malloc(ctx, sizeof(*set) * charslen);
    if (!set) {
        *err = 1;
        return SIZE_MAX;
    }
    set_n = 0;
    pos = 0;
    while (pos < charslen)
        set[set_n++] = dyn_utf8_next(chars, charslen, &pos);

    pos = 0;
    while (pos < slen) {
        size_t start = pos, i;
        uint32_t cp = dyn_utf8_next(s, slen, &pos);
        for (i = 0; i < set_n; i++) {
            if (set[i] == cp) {
                js_free(ctx, set);
                return start;
            }
        }
    }
    js_free(ctx, set);
    return SIZE_MAX;
}

/* ================================================================ *
 *  count: non-overlapping occurrences (Go semantics; contrast with
 *  dyna:search's overlapping indexOfAll).
 * ================================================================ */

static int64_t dyn_str_count(const uint8_t *s, size_t slen, const uint8_t *sub,
                             size_t sublen)
{
    int64_t n = 0;
    size_t pos = 0;

    if (sublen == 0) {
        while (pos < slen) {
            dyn_utf8_next(s, slen, &pos);
            n++;
        }
        return n + 1;
    }
    for (;;) {
        size_t m = dyn_str_find(s, slen, sub, sublen, pos);
        if (m == (size_t)-1)
            break;
        n++;
        pos = m + sublen;
    }
    return n;
}

/* ================================================================ *
 *  replace / replaceAll (shared core). old == "" inserts `repl` at every
 *  code-point boundary (k+1 insertion points for a k-code-point string),
 *  matching Go's Replace/ReplaceAll exactly -- including that only the
 *  insertions count against `limit`; whatever code points remain once the
 *  budget is spent are copied through untouched as the final segment.
 * ================================================================ */

static JSValue dyn_str_replace(JSContext *ctx, const uint8_t *s, size_t slen,
                               const uint8_t *old, size_t oldlen,
                               const uint8_t *repl, size_t repllen, int64_t limit)
{
    uint8_t *out;
    size_t out_cap, out_len = 0, pos = 0;
    JSValue result;

    if (limit == 0)
        return JS_NewStringLen(ctx, (const char *)s, slen);

    if (oldlen == 0) {
        size_t n_cp = 0, p = 0;
        int64_t max_ins, actual, i;

        while (p < slen) {
            dyn_utf8_next(s, slen, &p);
            n_cp++;
        }
        max_ins = (int64_t)n_cp + 1;
        actual = (limit < 0 || limit > max_ins) ? max_ins : limit;
        out_cap = slen + (size_t)actual * repllen + 1;
        out = js_malloc(ctx, out_cap);
        if (!out)
            return JS_EXCEPTION;
        pos = 0;
        for (i = 0; i < actual; i++) {
            if (i > 0) {
                size_t start = pos;
                dyn_utf8_next(s, slen, &pos);
                memcpy(out + out_len, s + start, pos - start);
                out_len += pos - start;
            }
            memcpy(out + out_len, repl, repllen);
            out_len += repllen;
        }
        memcpy(out + out_len, s + pos, slen - pos);
        out_len += slen - pos;
        result = JS_NewStringLen(ctx, (const char *)out, out_len);
        js_free(ctx, out);
        return result;
    }

    {
        int64_t m = dyn_str_count(s, slen, old, oldlen);
        int64_t actual = (limit < 0 || limit > m) ? m : limit;
        if (repllen >= oldlen)
            out_cap = slen + (size_t)actual * (repllen - oldlen) + 1;
        else
            out_cap = slen + 1;
    }
    out = js_malloc(ctx, out_cap);
    if (!out)
        return JS_EXCEPTION;
    {
        int64_t done = 0;
        for (;;) {
            size_t m;

            if (limit >= 0 && done == limit)
                m = (size_t)-1;
            else
                m = dyn_str_find(s, slen, old, oldlen, pos);
            if (m == (size_t)-1) {
                memcpy(out + out_len, s + pos, slen - pos);
                out_len += slen - pos;
                break;
            }
            memcpy(out + out_len, s + pos, m - pos);
            out_len += m - pos;
            memcpy(out + out_len, repl, repllen);
            out_len += repllen;
            pos = m + oldlen;
            done++;
        }
    }
    result = JS_NewStringLen(ctx, (const char *)out, out_len);
    js_free(ctx, out);
    return result;
}

/* ================================================================ *
 *  JS-facing exports
 * ================================================================ */

static JSValue dyn_strings_split(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *s, *sep;
    size_t slen, seplen;
    JSValue result;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &sep, &seplen))
        return JS_EXCEPTION;
    result = dyn_str_split_core(ctx, (const uint8_t *)s, slen,
                                (const uint8_t *)sep, seplen, -1);
    dyn_str_free2(ctx, s, sep);
    return result;
}

static JSValue dyn_strings_split_n(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *s, *sep;
    size_t slen, seplen;
    int32_t n;
    JSValue result;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &sep, &seplen))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &n, argv[2])) {
        dyn_str_free2(ctx, s, sep);
        return JS_EXCEPTION;
    }
    result = dyn_str_split_core(ctx, (const uint8_t *)s, slen,
                                (const uint8_t *)sep, seplen, n);
    dyn_str_free2(ctx, s, sep);
    return result;
}

static JSValue dyn_strings_fields(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *s;
    size_t slen;
    JSValue result;

    (void)this_val; (void)argc;
    s = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    result = dyn_str_fields(ctx, (const uint8_t *)s, slen);
    JS_FreeCString(ctx, s);
    return result;
}

static JSValue dyn_strings_join(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *sep;
    size_t seplen;
    JSValue len_val, result = JS_EXCEPTION;
    uint32_t len, i;
    const char **parts = NULL;
    size_t *part_lens = NULL;
    size_t total, out_len;
    uint8_t *out;

    (void)this_val; (void)argc;
    sep = JS_ToCStringLen(ctx, &seplen, argv[1]);
    if (!sep)
        return JS_EXCEPTION;

    len_val = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(len_val)) {
        JS_FreeCString(ctx, sep);
        return JS_EXCEPTION;
    }
    if (JS_ToUint32(ctx, &len, len_val)) {
        JS_FreeValue(ctx, len_val);
        JS_FreeCString(ctx, sep);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len_val);

    if (len == 0) {
        JS_FreeCString(ctx, sep);
        return JS_NewStringLen(ctx, "", 0);
    }

    parts = js_mallocz(ctx, sizeof(*parts) * len);
    part_lens = js_malloc(ctx, sizeof(*part_lens) * len);
    if (!parts || !part_lens)
        goto done; /* js_malloc/js_mallocz already threw */

    total = seplen * (size_t)(len - 1);
    for (i = 0; i < len; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(el))
            goto done;
        if (JS_IsNull(el) || JS_IsUndefined(el)) {
            JS_FreeValue(ctx, el); /* parts[i] stays NULL (mallocz) */
            continue;
        }
        parts[i] = JS_ToCStringLen(ctx, &part_lens[i], el);
        JS_FreeValue(ctx, el);
        if (!parts[i])
            goto done;
        total += part_lens[i];
    }

    out = js_malloc(ctx, total ? total : 1);
    if (!out)
        goto done;
    out_len = 0;
    for (i = 0; i < len; i++) {
        if (i > 0) {
            memcpy(out + out_len, sep, seplen);
            out_len += seplen;
        }
        if (parts[i]) {
            memcpy(out + out_len, parts[i], part_lens[i]);
            out_len += part_lens[i];
        }
    }
    result = JS_NewStringLen(ctx, (const char *)out, out_len);
    js_free(ctx, out);

 done:
    if (parts)
        for (i = 0; i < len; i++)
            if (parts[i])
                JS_FreeCString(ctx, parts[i]);
    js_free(ctx, parts);
    js_free(ctx, part_lens);
    JS_FreeCString(ctx, sep);
    return result;
}

static JSValue dyn_strings_trim(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    const char *s;
    size_t slen, start, end;
    JSValue result;

    (void)this_val; (void)argc;
    s = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    dyn_str_trim_span((const uint8_t *)s, slen, magic, dyn_pred_is_space, NULL,
                      &start, &end);
    result = JS_NewStringLen(ctx, s + start, end - start);
    JS_FreeCString(ctx, s);
    return result;
}

static JSValue dyn_strings_trim_affix(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic)
{
    const char *s, *p;
    size_t slen, plen;
    int matched;
    JSValue result;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &p, &plen))
        return JS_EXCEPTION;
    matched = plen <= slen &&
             memcmp(magic == DYN_AFFIX_SUFFIX ? s + slen - plen : s, p, plen) == 0;
    if (!matched)
        result = JS_NewStringLen(ctx, s, slen);
    else if (magic == DYN_AFFIX_SUFFIX)
        result = JS_NewStringLen(ctx, s, slen - plen);
    else
        result = JS_NewStringLen(ctx, s + plen, slen - plen);
    dyn_str_free2(ctx, s, p);
    return result;
}

static JSValue dyn_strings_trim_chars(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *s, *chars;
    size_t slen, charslen, start, end;
    uint32_t *set;
    size_t set_n = 0, pos;
    DynCpSet cpset;
    JSValue result;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &chars, &charslen))
        return JS_EXCEPTION;
    set = js_malloc(ctx, sizeof(*set) * (charslen ? charslen : 1));
    if (!set) {
        dyn_str_free2(ctx, s, chars);
        return JS_EXCEPTION;
    }
    pos = 0;
    while (pos < charslen)
        set[set_n++] = dyn_utf8_next((const uint8_t *)chars, charslen, &pos);
    cpset.set = set;
    cpset.n = set_n;
    dyn_str_trim_span((const uint8_t *)s, slen, DYN_TRIM_BOTH, dyn_pred_in_set,
                      &cpset, &start, &end);
    result = JS_NewStringLen(ctx, s + start, end - start);
    js_free(ctx, set);
    dyn_str_free2(ctx, s, chars);
    return result;
}

static JSValue dyn_strings_case(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    const char *s;
    size_t slen;
    JSValue result;

    (void)this_val; (void)argc;
    s = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    result = dyn_str_case_convert(ctx, (const uint8_t *)s, slen, magic);
    JS_FreeCString(ctx, s);
    return result;
}

static JSValue dyn_strings_title(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *s;
    size_t slen;
    JSValue result;

    (void)this_val; (void)argc;
    s = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    result = dyn_str_title(ctx, (const uint8_t *)s, slen);
    JS_FreeCString(ctx, s);
    return result;
}

static JSValue dyn_strings_repeat(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *s;
    size_t slen;
    int32_t n;
    JSValue result;

    (void)this_val; (void)argc;
    s = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &n, argv[1])) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    result = dyn_str_repeat(ctx, (const uint8_t *)s, slen, n);
    JS_FreeCString(ctx, s);
    return result;
}

static JSValue dyn_strings_pad(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    const char *s, *pad;
    size_t slen, padlen;
    int32_t target;
    int default_pad = 0;
    JSValue result;

    (void)this_val; (void)argc;
    s = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &target, argv[1])) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(argv[2])) {
        pad = " ";
        padlen = 1;
        default_pad = 1;
    } else {
        pad = JS_ToCStringLen(ctx, &padlen, argv[2]);
        if (!pad) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
    }
    result = dyn_str_pad(ctx, (const uint8_t *)s, slen, target,
                         (const uint8_t *)pad, padlen, magic);
    if (!default_pad)
        JS_FreeCString(ctx, pad);
    JS_FreeCString(ctx, s);
    return result;
}

static JSValue dyn_strings_contains(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    const char *s, *sub;
    size_t slen, sublen;
    int ok;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &sub, &sublen))
        return JS_EXCEPTION;
    if (sublen == 0)
        ok = 1;
    else if (sublen > slen)
        ok = 0;
    else
        ok = dyn_str_find((const uint8_t *)s, slen, (const uint8_t *)sub, sublen, 0)
             != (size_t)-1;
    dyn_str_free2(ctx, s, sub);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_strings_any(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    const char *s, *chars;
    size_t slen, charslen, pos;
    int err = 0;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &chars, &charslen))
        return JS_EXCEPTION;
    pos = dyn_str_index_any(ctx, (const uint8_t *)s, slen,
                            (const uint8_t *)chars, charslen, &err);
    dyn_str_free2(ctx, s, chars);
    if (err)
        return JS_EXCEPTION;
    if (magic == DYN_ANY_CONTAINS)
        return JS_NewBool(ctx, pos != SIZE_MAX);
    return pos == SIZE_MAX ? JS_NewInt32(ctx, -1) : JS_NewInt64(ctx, (int64_t)pos);
}

static JSValue dyn_strings_has_affix(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    const char *s, *p;
    size_t slen, plen;
    int ok;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &p, &plen))
        return JS_EXCEPTION;
    if (plen > slen)
        ok = 0;
    else if (magic == DYN_AFFIX_SUFFIX)
        ok = memcmp(s + slen - plen, p, plen) == 0;
    else
        ok = memcmp(s, p, plen) == 0;
    dyn_str_free2(ctx, s, p);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_strings_index(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *s, *sub;
    size_t slen, sublen;
    JSValue result;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &sub, &sublen))
        return JS_EXCEPTION;
    if (sublen == 0) {
        result = JS_NewInt32(ctx, 0);
    } else if (sublen > slen) {
        result = JS_NewInt32(ctx, -1);
    } else {
        size_t pos = dyn_str_find((const uint8_t *)s, slen, (const uint8_t *)sub,
                                  sublen, 0);
        result = pos == (size_t)-1 ? JS_NewInt32(ctx, -1)
                                   : JS_NewInt64(ctx, (int64_t)pos);
    }
    dyn_str_free2(ctx, s, sub);
    return result;
}

static JSValue dyn_strings_last_index(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *s, *sub;
    size_t slen, sublen;
    JSValue result;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &sub, &sublen))
        return JS_EXCEPTION;
    if (sublen == 0) {
        result = JS_NewInt64(ctx, (int64_t)slen);
    } else if (sublen > slen) {
        result = JS_NewInt32(ctx, -1);
    } else {
        size_t pos = dyn_str_find_last((const uint8_t *)s, slen,
                                       (const uint8_t *)sub, sublen);
        result = pos == (size_t)-1 ? JS_NewInt32(ctx, -1)
                                   : JS_NewInt64(ctx, (int64_t)pos);
    }
    dyn_str_free2(ctx, s, sub);
    return result;
}

static JSValue dyn_strings_count(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *s, *sub;
    size_t slen, sublen;
    int64_t n;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &sub, &sublen))
        return JS_EXCEPTION;
    n = dyn_str_count((const uint8_t *)s, slen, (const uint8_t *)sub, sublen);
    dyn_str_free2(ctx, s, sub);
    return JS_NewInt64(ctx, n);
}

static JSValue dyn_strings_replace(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *s, *old, *repl;
    size_t slen, oldlen, repllen;
    int32_t n;
    JSValue result;

    (void)this_val; (void)argc;
    if (dyn_str_coerce3(ctx, argv[0], argv[1], argv[2], &s, &slen, &old, &oldlen,
                        &repl, &repllen))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &n, argv[3])) {
        dyn_str_free3(ctx, s, old, repl);
        return JS_EXCEPTION;
    }
    if (oldlen == repllen && memcmp(old, repl, oldlen) == 0)
        result = JS_NewStringLen(ctx, s, slen); /* old==new: byte-identical no-op */
    else
        result = dyn_str_replace(ctx, (const uint8_t *)s, slen,
                                 (const uint8_t *)old, oldlen,
                                 (const uint8_t *)repl, repllen, n);
    dyn_str_free3(ctx, s, old, repl);
    return result;
}

static JSValue dyn_strings_replace_all(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const char *s, *old, *repl;
    size_t slen, oldlen, repllen;
    JSValue result;

    (void)this_val; (void)argc;
    if (dyn_str_coerce3(ctx, argv[0], argv[1], argv[2], &s, &slen, &old, &oldlen,
                        &repl, &repllen))
        return JS_EXCEPTION;
    if (oldlen == repllen && memcmp(old, repl, oldlen) == 0)
        result = JS_NewStringLen(ctx, s, slen);
    else
        result = dyn_str_replace(ctx, (const uint8_t *)s, slen,
                                 (const uint8_t *)old, oldlen,
                                 (const uint8_t *)repl, repllen, -1);
    dyn_str_free3(ctx, s, old, repl);
    return result;
}

static JSValue dyn_strings_equal_fold(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *s, *t;
    size_t slen, tlen, i;
    int ok;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &t, &tlen))
        return JS_EXCEPTION;
    ok = (slen == tlen);
    for (i = 0; ok && i < slen; i++) {
        uint8_t a = (uint8_t)s[i], b = (uint8_t)t[i];
        if (a >= 'A' && a <= 'Z')
            a = (uint8_t)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (uint8_t)(b + ('a' - 'A'));
        if (a != b)
            ok = 0;
    }
    dyn_str_free2(ctx, s, t);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_strings_compare(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *s, *t;
    size_t slen, tlen;
    int c, result;

    (void)this_val; (void)argc;
    if (dyn_str_coerce2(ctx, argv[0], argv[1], &s, &slen, &t, &tlen))
        return JS_EXCEPTION;
    c = memcmp(s, t, slen < tlen ? slen : tlen);
    if (c < 0)
        result = -1;
    else if (c > 0)
        result = 1;
    else if (slen < tlen)
        result = -1;
    else if (slen > tlen)
        result = 1;
    else
        result = 0;
    dyn_str_free2(ctx, s, t);
    return JS_NewInt32(ctx, result);
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry dyn_strings_funcs[] = {
    JS_CFUNC_DEF("split", 2, dyn_strings_split),
    JS_CFUNC_DEF("splitN", 3, dyn_strings_split_n),
    JS_CFUNC_DEF("fields", 1, dyn_strings_fields),
    JS_CFUNC_DEF("join", 2, dyn_strings_join),
    JS_CFUNC_MAGIC_DEF("trim", 1, dyn_strings_trim, DYN_TRIM_BOTH),
    JS_CFUNC_MAGIC_DEF("trimStart", 1, dyn_strings_trim, DYN_TRIM_START),
    JS_CFUNC_MAGIC_DEF("trimEnd", 1, dyn_strings_trim, DYN_TRIM_END),
    JS_CFUNC_MAGIC_DEF("trimPrefix", 2, dyn_strings_trim_affix, DYN_AFFIX_PREFIX),
    JS_CFUNC_MAGIC_DEF("trimSuffix", 2, dyn_strings_trim_affix, DYN_AFFIX_SUFFIX),
    JS_CFUNC_DEF("trimChars", 2, dyn_strings_trim_chars),
    JS_CFUNC_MAGIC_DEF("toUpper", 1, dyn_strings_case, 0),
    JS_CFUNC_MAGIC_DEF("toLower", 1, dyn_strings_case, 1),
    JS_CFUNC_DEF("title", 1, dyn_strings_title),
    JS_CFUNC_DEF("repeat", 2, dyn_strings_repeat),
    JS_CFUNC_MAGIC_DEF("padStart", 3, dyn_strings_pad, DYN_PAD_AT_START),
    JS_CFUNC_MAGIC_DEF("padEnd", 3, dyn_strings_pad, DYN_PAD_AT_END),
    JS_CFUNC_DEF("contains", 2, dyn_strings_contains),
    JS_CFUNC_MAGIC_DEF("containsAny", 2, dyn_strings_any, DYN_ANY_CONTAINS),
    JS_CFUNC_MAGIC_DEF("hasPrefix", 2, dyn_strings_has_affix, DYN_AFFIX_PREFIX),
    JS_CFUNC_MAGIC_DEF("hasSuffix", 2, dyn_strings_has_affix, DYN_AFFIX_SUFFIX),
    JS_CFUNC_DEF("index", 2, dyn_strings_index),
    JS_CFUNC_DEF("lastIndex", 2, dyn_strings_last_index),
    JS_CFUNC_MAGIC_DEF("indexAny", 2, dyn_strings_any, DYN_ANY_INDEX),
    JS_CFUNC_DEF("count", 2, dyn_strings_count),
    JS_CFUNC_DEF("replace", 4, dyn_strings_replace),
    JS_CFUNC_DEF("replaceAll", 3, dyn_strings_replace_all),
    JS_CFUNC_DEF("equalFold", 2, dyn_strings_equal_fold),
    JS_CFUNC_DEF("compare", 2, dyn_strings_compare),
};

static int dyn_strings_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_strings_funcs,
                                  countof(dyn_strings_funcs));
}

int js_nat_init_strings(JSContext *ctx)
{
    JSModuleDef *m;
    simd_init(); /* idempotent: select the best find_first_of/strfind kernel */
    m = JS_NewCModule(ctx, "dyna:strings", dyn_strings_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_strings_funcs,
                                  countof(dyn_strings_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_STRINGS */
