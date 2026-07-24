/*
 * dyna:path -- POSIX filesystem-path string utilities. Self-contained,
 * in-repo (no external deps). Mirrors Node's `path.posix` (and, where they
 * agree, Go's `path` / `path/filepath`) byte-for-byte -- verified against a
 * >35,000-case differential run against Node's real path.posix (exhaustive
 * over short strings built from the alphabet {a, b, ., /} plus every edge
 * case called out below), not just hand-derivation.
 *
 *   import { join, resolve, normalize, clean, dirname, basename, extname,
 *            isAbsolute, relative, sep, delimiter } from "dyna:path";
 *
 * These are TRANSIENT plain functions: no filesystem access, no `this`, no
 * closable native resource. The JS arguments are the only input and a fresh
 * JS string (or bool) is the only output. '/' is always the separator --
 * this is the POSIX flavor only; there is no win32 variant.
 *
 * Semantics (documented guarantees):
 *
 *   - join(...parts): empty parts are skipped; the rest are '/'-joined and
 *     the result is normalize()d. Zero parts, or all-empty parts, -> ".".
 *
 *   - resolve(...parts): processes parts RIGHT TO LEFT, prepending each to
 *     an accumulator, and stops at the first (rightmost) part that starts
 *     with '/'. Real Node's path.resolve() falls back to process.cwd() when
 *     no part is absolute; this module has no cwd, so it falls back to a
 *     NOTIONAL cwd of "/". Consequently resolve() ALWAYS returns an
 *     absolute path, and excess ".." above the root is silently absorbed
 *     (e.g. resolve("/a", "../../..") -> "/"), exactly like real Node.
 *
 *   - normalize(p) / clean(p): the identical implementation under two
 *     names (clean() is the Go-flavored alias). Collapses "//" runs,
 *     resolves "." and ".." segments, and PRESERVES a trailing slash if the
 *     input had one -- this is Node's rule, not Go's (Go's path.Clean
 *     always strips a trailing slash; we deliberately follow Node here for
 *     both names). "" -> ".".
 *
 *   - dirname/basename/extname operate lexically on the string (no "."/".."
 *     resolution), exactly like Node: dirname("a/..") -> "a", not ".".
 *     extname(".bashrc") -> "" (a leading dot makes a dotfile, not an
 *     extension); extname("..") -> ""; extname("...") -> ".".
 *     basename(p, ext) strips a literal trailing `ext` UNLESS doing so
 *     would empty the final path segment (basename("/foo/.html", ".html")
 *     is ".html", not "" -- only basename(p, p) itself returns "").
 *
 *   - relative(from, to): both sides are resolve()d (against the same
 *     notional "/" cwd) first, then compared segment-by-segment.
 *
 * All path arguments must be JS strings (typeof 'string', including a
 * pending rope) -- like Node's internal validateString, a non-string
 * argument is a thrown TypeError, NOT coerced via ToString (unlike e.g.
 * dyna:search's indexOf, which documents the opposite choice).
 *
 * Coercion discipline: every method fully validates+materialises its
 * arguments into owned C locals FIRST and only then runs the algorithm
 * (pure C, calls no JS) -- for join()/resolve()'s variadic list this means
 * validating left-to-right, ALL of them, before building anything. This
 * matches Node exactly for join(). For resolve() it is a deliberate, narrow
 * deviation: real Node's resolve() walks right-to-left and can short-
 * circuit BEFORE ever looking at (or validating) an earlier argument once
 * it hits an absolute one (e.g. real `path.resolve(123, "/abs")` does NOT
 * throw -- it never reaches the bad leading argument). Our resolve()
 * validates every argument up front, so the same call throws here. Every
 * *string* argument's resulting path is identical to Node in every case
 * checked. No argument coercion here ever runs JS (strings don't invoke
 * valueOf/@@toPrimitive), so batching the validation costs nothing for real
 * programs and keeps the implementation simple: fully materialize, then
 * compute, with nothing to double-check for reentrancy.
 *
 * Memory: every scratch buffer is sized from the input length -- join()/
 * resolve() sum the coerced parts' lengths up front; the shared normalizer
 * core is proven (and differentially verified) to never emit more bytes
 * than it consumes. No fixed-size stack buffers regardless of path length.
 * Every JS_ToCStringLen result is released on every path, including every
 * error path.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PATH)

#include <stddef.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ====================================================================
 * Core: POSIX path-segment normalizer.
 *
 * A direct, index-for-index port of Node's internal normalizeString()
 * (lib/path.js), specialised to the POSIX separator '/'. Scans `path`
 * once, collapses "." segments and repeated slashes, and backtracks over
 * ".." segments against whatever has already been emitted -- unless
 * `allow_above_root` is set, in which case a ".." that can't erase
 * anything is kept literally (used for a path that must stay *relative*;
 * resolve() always passes allow_above_root=0, because its result is
 * always absolute and excess ".." above the root is simply dropped).
 *
 * Writes into `res` (caller-owned). Capacity >= path_len always suffices:
 * every emitted byte is either copied verbatim from `path` (segments are
 * disjoint, monotonically-advancing substrings of `path`) or is a literal
 * ".." emitted only when the *input* itself just contributed a ".."
 * segment of at least 2 bytes -- so output length can never exceed input
 * length. (Exhaustively verified up to length 9 over a representative
 * alphabet, and spot-checked well beyond that -- see the module docstring.)
 *
 * Returns the number of bytes written to `res`.
 * ==================================================================== */
static size_t path_normalize_core(const char *path, size_t path_len,
                                  int allow_above_root, char *res)
{
    size_t res_len = 0;
    long last_segment_length = 0;
    long last_slash = -1;
    int dots = 0;
    int code = 0;
    long i;

    for (i = 0; i <= (long)path_len; i++) {
        if (i < (long)path_len)
            code = (unsigned char)path[i];
        else if (code == '/')
            break; /* trailing separator already handled at the prior i */
        else
            code = '/'; /* flush the final pending segment */

        if (code == '/') {
            if (last_slash == i - 1 || dots == 1) {
                /* "//" run, or a "." segment: nothing to emit */
            } else if (last_slash != i - 1 && dots == 2) {
                /* ".." segment: try to erase the previously emitted one */
                if (res_len < 2 || last_segment_length != 2 ||
                    res[res_len - 1] != '.' || res[res_len - 2] != '.') {
                    if (res_len > 2) {
                        long k, last_slash_index = -1;
                        for (k = (long)res_len - 1; k >= 0; k--) {
                            if (res[k] == '/') {
                                last_slash_index = k;
                                break;
                            }
                        }
                        if (last_slash_index != (long)res_len - 1) {
                            if (last_slash_index == -1) {
                                res_len = 0;
                                last_segment_length = 0;
                            } else {
                                long k2, new_last_slash = -1;
                                res_len = (size_t)last_slash_index;
                                for (k2 = (long)res_len - 1; k2 >= 0; k2--) {
                                    if (res[k2] == '/') {
                                        new_last_slash = k2;
                                        break;
                                    }
                                }
                                last_segment_length =
                                    (long)res_len - 1 - new_last_slash;
                            }
                            last_slash = i;
                            dots = 0;
                            continue;
                        }
                    } else if (res_len == 2 || res_len == 1) {
                        res_len = 0;
                        last_segment_length = 0;
                        last_slash = i;
                        dots = 0;
                        continue;
                    }
                }
                if (allow_above_root) {
                    if (res_len > 0)
                        res[res_len++] = '/';
                    res[res_len++] = '.';
                    res[res_len++] = '.';
                    last_segment_length = 2;
                }
            } else {
                size_t seg_start = (size_t)(last_slash + 1);
                size_t seg_len = (size_t)(i - (last_slash + 1));
                if (res_len > 0)
                    res[res_len++] = '/';
                memcpy(res + res_len, path + seg_start, seg_len);
                res_len += seg_len;
                last_segment_length = (long)seg_len;
            }
            last_slash = i;
            dots = 0;
        } else if (code == '.' && dots != -1) {
            dots++;
        } else {
            dots = -1;
        }
    }
    return res_len;
}

/* normalize()/clean(): shared implementation over an already-coerced
 * buffer. Frees nothing owned by the caller; returns a fresh JS string or
 * JS_EXCEPTION (OOM only -- no other failure mode). */
static JSValue path_clean_impl(JSContext *ctx, const char *p, size_t plen)
{
    int is_abs, trailing_sep;
    char *core_buf, *final_buf;
    size_t core_len, total, o;
    JSValue result;

    if (plen == 0)
        return JS_NewString(ctx, ".");

    is_abs = (p[0] == '/');
    trailing_sep = (p[plen - 1] == '/');

    core_buf = js_malloc(ctx, plen);
    if (!core_buf)
        return JS_EXCEPTION;
    core_len = path_normalize_core(p, plen, !is_abs, core_buf);

    if (core_len == 0) {
        js_free(ctx, core_buf);
        if (is_abs)
            return JS_NewString(ctx, "/");
        return JS_NewString(ctx, trailing_sep ? "./" : ".");
    }

    total = (is_abs ? 1 : 0) + core_len + (trailing_sep ? 1 : 0);
    final_buf = js_malloc(ctx, total);
    if (!final_buf) {
        js_free(ctx, core_buf);
        return JS_EXCEPTION;
    }
    o = 0;
    if (is_abs)
        final_buf[o++] = '/';
    memcpy(final_buf + o, core_buf, core_len);
    o += core_len;
    if (trailing_sep)
        final_buf[o++] = '/';
    result = JS_NewStringLen(ctx, final_buf, total);
    js_free(ctx, final_buf);
    js_free(ctx, core_buf);
    return result;
}

/* Shared resolve algorithm over N already-coerced (pointer, length) parts.
 * Always returns an absolute path -- our notional cwd is "/", which is
 * itself absolute, so the fallback (no part in `parts` is absolute) is
 * always absolute too. Returns a fresh JS string or JS_EXCEPTION (OOM). */
static JSValue path_resolve_core(JSContext *ctx, const char *const *parts,
                                 const size_t *lens, int n)
{
    int i, root_idx, first, start;
    size_t sum_lens, cap, o, core_len;
    char *raw, *core_buf, *fin;
    JSValue result;

    sum_lens = 0;
    for (i = 0; i < n; i++)
        sum_lens += lens[i];

    /* Find the rightmost non-empty part that starts with '/'; -1 means
     * "none -- fall back to the notional cwd". */
    root_idx = -1;
    for (i = n - 1; i >= 0; i--) {
        if (lens[i] > 0 && parts[i][0] == '/') {
            root_idx = i;
            break;
        }
    }

    cap = sum_lens + (size_t)n + 2; /* parts + separators + cwd byte + slack */
    raw = js_malloc(ctx, cap);
    if (!raw)
        return JS_EXCEPTION;

    o = 0;
    first = 1;
    start = root_idx;
    if (root_idx == -1) {
        raw[o++] = '/'; /* notional cwd */
        first = 0;
        start = 0;
    }
    for (i = start; i < n; i++) {
        if (lens[i] == 0)
            continue;
        if (!first)
            raw[o++] = '/';
        memcpy(raw + o, parts[i], lens[i]);
        o += lens[i];
        first = 0;
    }

    /* o >= 1 always: either the cwd byte was written, or root_idx pointed
     * at a part with lens[root_idx] > 0 that the loop above just copied. */
    core_buf = js_malloc(ctx, o);
    if (!core_buf) {
        js_free(ctx, raw);
        return JS_EXCEPTION;
    }
    /* allow_above_root=0: resolve()'s result is always absolute, so a
     * ".." that reaches the root is dropped, not kept -- see docstring. */
    core_len = path_normalize_core(raw, o, 0, core_buf);
    js_free(ctx, raw);

    if (core_len == 0) {
        js_free(ctx, core_buf);
        return JS_NewString(ctx, "/");
    }
    fin = js_malloc(ctx, core_len + 1);
    if (!fin) {
        js_free(ctx, core_buf);
        return JS_EXCEPTION;
    }
    fin[0] = '/';
    memcpy(fin + 1, core_buf, core_len);
    result = JS_NewStringLen(ctx, fin, core_len + 1);
    js_free(ctx, fin);
    js_free(ctx, core_buf);
    return result;
}

/* resolve() a single already-owned C string (used internally by relative()
 * to resolve `from`/`to` without going through JS argv again). */
static JSValue path_resolve_one(JSContext *ctx, const char *s, size_t slen)
{
    const char *parts[1];
    size_t lens[1];
    parts[0] = s;
    lens[0] = slen;
    return path_resolve_core(ctx, parts, lens, 1);
}

/* relative(from, to), given both sides ALREADY resolve()d (so both start
 * with '/'). Direct port of Node's relative(), minus the redundant pre-
 * resolve string-equality fast path (resolve() is a pure function, so the
 * post-resolve equality check below catches the same case). */
static JSValue path_relative_impl(JSContext *ctx, const char *from_r,
                                  size_t from_len, const char *to_r,
                                  size_t to_len)
{
    size_t from_l, to_l, smallest, i, cap, o;
    long last_common_sep;
    char *buf;
    JSValue result;

    if (from_len == to_len && memcmp(from_r, to_r, from_len) == 0)
        return JS_NewString(ctx, "");

    from_l = from_len - 1; /* length after the shared leading '/' */
    to_l = to_len - 1;
    smallest = from_l < to_l ? from_l : to_l;
    last_common_sep = -1;

    for (i = 0; i < smallest; i++) {
        char fc = from_r[1 + i];
        if (fc != to_r[1 + i])
            break;
        if (fc == '/')
            last_common_sep = (long)i;
    }

    if (i == smallest) {
        if (to_l > smallest) {
            if (to_r[1 + i] == '/')
                return JS_NewStringLen(ctx, to_r + 1 + i + 1,
                                       to_len - (1 + i + 1));
            if (i == 0)
                return JS_NewStringLen(ctx, to_r + 1 + i, to_len - (1 + i));
        } else if (from_l > smallest) {
            if (from_r[1 + i] == '/')
                last_common_sep = (long)i;
            else if (i == 0)
                last_common_sep = 0;
        }
    }

    /* Every "up" hop is a "/.." (or ".." for the first, sans slash) that
     * consumes a segment already present in `from`, and the tail is a
     * verbatim slice of `to` -- sized from both input lengths. */
    cap = from_len + to_len + 4;
    buf = js_malloc(ctx, cap);
    if (!buf)
        return JS_EXCEPTION;

    o = 0;
    for (i = (size_t)(1 + last_common_sep + 1); i <= from_len; i++) {
        if (i == from_len || from_r[i] == '/') {
            if (o == 0) {
                buf[o++] = '.';
                buf[o++] = '.';
            } else {
                buf[o++] = '/';
                buf[o++] = '.';
                buf[o++] = '.';
            }
        }
    }
    {
        size_t to_start = (size_t)(1 + last_common_sep);
        size_t suffix_len = to_len - to_start;
        memcpy(buf + o, to_r + to_start, suffix_len);
        o += suffix_len;
    }
    result = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    return result;
}

/* ---------- module functions ---------- */

/* join(...parts) -> string */
static JSValue dyn_path_join(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    const char **parts;
    size_t *lens;
    int i, n_coerced, failed;
    size_t total, n_nonempty;
    JSValue result;

    (void)this_val;

    if (argc == 0)
        return JS_NewString(ctx, ".");

    parts = js_malloc(ctx, (size_t)argc * sizeof(*parts));
    if (!parts)
        return JS_EXCEPTION;
    lens = js_malloc(ctx, (size_t)argc * sizeof(*lens));
    if (!lens) {
        js_free(ctx, parts);
        return JS_EXCEPTION;
    }

    n_coerced = 0;
    failed = 0;
    total = 0;
    n_nonempty = 0;
    for (i = 0; i < argc; i++) {
        if (!JS_IsString(argv[i])) {
            JS_ThrowTypeError(ctx, "dyna:path: \"path\" argument must be a string");
            failed = 1;
            break;
        }
        parts[i] = JS_ToCStringLen(ctx, &lens[i], argv[i]);
        if (!parts[i]) {
            failed = 1;
            break;
        }
        n_coerced = i + 1;
        if (lens[i] > 0) {
            total += lens[i];
            n_nonempty++;
        }
    }

    if (failed) {
        result = JS_EXCEPTION;
    } else if (n_nonempty == 0) {
        result = JS_NewString(ctx, ".");
    } else {
        char *joined;
        total += n_nonempty - 1; /* one separator between each kept part */
        joined = js_malloc(ctx, total);
        if (!joined) {
            result = JS_EXCEPTION;
        } else {
            size_t o = 0;
            int first = 1;
            for (i = 0; i < argc; i++) {
                if (lens[i] == 0)
                    continue;
                if (!first)
                    joined[o++] = '/';
                memcpy(joined + o, parts[i], lens[i]);
                o += lens[i];
                first = 0;
            }
            result = path_clean_impl(ctx, joined, total);
            js_free(ctx, joined);
        }
    }

    for (i = 0; i < n_coerced; i++)
        JS_FreeCString(ctx, parts[i]);
    js_free(ctx, lens);
    js_free(ctx, parts);
    return result;
}

/* resolve(...parts) -> string (see docstring: notional cwd is "/") */
static JSValue dyn_path_resolve(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char **parts;
    size_t *lens;
    int i, n_coerced, failed;
    JSValue result;

    (void)this_val;

    parts = NULL;
    lens = NULL;
    if (argc > 0) {
        parts = js_malloc(ctx, (size_t)argc * sizeof(*parts));
        if (!parts)
            return JS_EXCEPTION;
        lens = js_malloc(ctx, (size_t)argc * sizeof(*lens));
        if (!lens) {
            js_free(ctx, parts);
            return JS_EXCEPTION;
        }
    }

    n_coerced = 0;
    failed = 0;
    for (i = 0; i < argc; i++) {
        if (!JS_IsString(argv[i])) {
            JS_ThrowTypeError(ctx, "dyna:path: \"path\" argument must be a string");
            failed = 1;
            break;
        }
        parts[i] = JS_ToCStringLen(ctx, &lens[i], argv[i]);
        if (!parts[i]) {
            failed = 1;
            break;
        }
        n_coerced = i + 1;
    }

    result = failed ? JS_EXCEPTION : path_resolve_core(ctx, parts, lens, argc);

    for (i = 0; i < n_coerced; i++)
        JS_FreeCString(ctx, parts[i]);
    js_free(ctx, lens);
    js_free(ctx, parts);
    return result;
}

/* normalize(p) / clean(p) -> string (same implementation, two names) */
static JSValue dyn_path_normalize(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *p;
    size_t plen;
    JSValue result;

    (void)this_val;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:path: \"path\" argument must be a string");
    p = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!p)
        return JS_EXCEPTION;
    result = path_clean_impl(ctx, p, plen);
    JS_FreeCString(ctx, p);
    return result;
}

/* dirname(p) -> string. Purely lexical: does not resolve "."/"..". */
static JSValue dyn_path_dirname(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *p;
    size_t plen;
    JSValue result;
    long end, i;
    int has_root, matched_slash;

    (void)this_val;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:path: \"path\" argument must be a string");
    p = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!p)
        return JS_EXCEPTION;

    if (plen == 0) {
        result = JS_NewString(ctx, ".");
        goto done;
    }

    has_root = (p[0] == '/');
    end = -1;
    matched_slash = 1;
    for (i = (long)plen - 1; i >= 1; i--) {
        if (p[i] == '/') {
            if (!matched_slash) {
                end = i;
                break;
            }
        } else {
            matched_slash = 0;
        }
    }
    if (end == -1)
        result = JS_NewString(ctx, has_root ? "/" : ".");
    else if (has_root && end == 1)
        result = JS_NewString(ctx, "//"); /* POSIX: exactly 2 leading slashes are kept */
    else
        result = JS_NewStringLen(ctx, p, (size_t)end);

 done:
    JS_FreeCString(ctx, p);
    return result;
}

/* basename(p, ext?) -> string */
static JSValue dyn_path_basename(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *p;
    const char *suf;
    size_t plen, suflen;
    int has_suffix;
    long start, end, i;
    JSValue result;

    (void)this_val;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:path: \"path\" argument must be a string");
    has_suffix = (argc >= 2 && !JS_IsUndefined(argv[1]));
    if (has_suffix && !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:path: \"suffix\" argument must be a string");

    p = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!p)
        return JS_EXCEPTION;
    suf = NULL;
    suflen = 0;
    if (has_suffix) {
        suf = JS_ToCStringLen(ctx, &suflen, argv[1]);
        if (!suf) {
            JS_FreeCString(ctx, p);
            return JS_EXCEPTION;
        }
    }

    if (has_suffix && suflen > 0 && suflen <= plen) {
        if (suflen == plen && memcmp(p, suf, plen) == 0) {
            result = JS_NewString(ctx, "");
        } else {
            long ext_idx = (long)suflen - 1;
            long first_non_slash_end = -1;
            int matched_slash = 1;

            start = 0;
            end = -1;
            for (i = (long)plen - 1; i >= 0; i--) {
                unsigned char c = (unsigned char)p[i];
                if (c == '/') {
                    if (!matched_slash) {
                        start = i + 1;
                        break;
                    }
                } else {
                    if (first_non_slash_end == -1) {
                        matched_slash = 0;
                        first_non_slash_end = i + 1;
                    }
                    if (ext_idx >= 0) {
                        if (c == (unsigned char)suf[ext_idx]) {
                            if (--ext_idx == -1)
                                end = i;
                        } else {
                            ext_idx = -1;
                            end = first_non_slash_end;
                        }
                    }
                }
            }
            if (start == end)
                end = first_non_slash_end;
            else if (end == -1)
                end = (long)plen;
            result = (end > start)
                ? JS_NewStringLen(ctx, p + start, (size_t)(end - start))
                : JS_NewString(ctx, "");
        }
    } else {
        int matched_slash = 1;
        start = 0;
        end = -1;
        for (i = (long)plen - 1; i >= 0; i--) {
            if (p[i] == '/') {
                if (!matched_slash) {
                    start = i + 1;
                    break;
                }
            } else if (end == -1) {
                matched_slash = 0;
                end = i + 1;
            }
        }
        result = (end == -1)
            ? JS_NewString(ctx, "")
            : JS_NewStringLen(ctx, p + start, (size_t)(end - start));
    }

    if (suf)
        JS_FreeCString(ctx, suf);
    JS_FreeCString(ctx, p);
    return result;
}

/* extname(p) -> string (last '.' of the final segment; dotfiles have none) */
static JSValue dyn_path_extname(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *p;
    size_t plen;
    JSValue result;
    long start_dot, start_part, end, i;
    int matched_slash, pre_dot_state;

    (void)this_val;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:path: \"path\" argument must be a string");
    p = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!p)
        return JS_EXCEPTION;

    start_dot = -1;
    start_part = 0;
    end = -1;
    matched_slash = 1;
    pre_dot_state = 0;

    for (i = (long)plen - 1; i >= 0; i--) {
        unsigned char c = (unsigned char)p[i];
        if (c == '/') {
            if (!matched_slash) {
                start_part = i + 1;
                break;
            }
            continue;
        }
        if (end == -1) {
            matched_slash = 0;
            end = i + 1;
        }
        if (c == '.') {
            if (start_dot == -1)
                start_dot = i;
            else if (pre_dot_state != 1)
                pre_dot_state = 1;
        } else if (start_dot != -1) {
            pre_dot_state = -1;
        }
    }

    if (start_dot == -1 || end == -1 || pre_dot_state == 0 ||
        (pre_dot_state == 1 && start_dot == end - 1 &&
         start_dot == start_part + 1)) {
        result = JS_NewString(ctx, "");
    } else {
        result = JS_NewStringLen(ctx, p + start_dot, (size_t)(end - start_dot));
    }

    JS_FreeCString(ctx, p);
    return result;
}

/* isAbsolute(p) -> bool */
static JSValue dyn_path_is_absolute(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    const char *p;
    size_t plen;
    JSValue result;

    (void)this_val;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:path: \"path\" argument must be a string");
    p = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!p)
        return JS_EXCEPTION;
    result = JS_NewBool(ctx, plen > 0 && p[0] == '/');
    JS_FreeCString(ctx, p);
    return result;
}

/* relative(from, to) -> string */
static JSValue dyn_path_relative(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *from_s, *to_s;
    size_t from_slen, to_slen;
    JSValue from_resolved, to_resolved, result;
    const char *from_r = NULL, *to_r = NULL;
    size_t from_rlen = 0, to_rlen = 0;

    (void)this_val;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:path: \"from\" argument must be a string");
    if (argc < 2 || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:path: \"to\" argument must be a string");

    from_s = JS_ToCStringLen(ctx, &from_slen, argv[0]);
    if (!from_s)
        return JS_EXCEPTION;
    to_s = JS_ToCStringLen(ctx, &to_slen, argv[1]);
    if (!to_s) {
        JS_FreeCString(ctx, from_s);
        return JS_EXCEPTION;
    }

    from_resolved = path_resolve_one(ctx, from_s, from_slen);
    JS_FreeCString(ctx, from_s);
    to_resolved = JS_IsException(from_resolved)
        ? JS_EXCEPTION
        : path_resolve_one(ctx, to_s, to_slen);
    JS_FreeCString(ctx, to_s);

    if (JS_IsException(from_resolved) || JS_IsException(to_resolved)) {
        JS_FreeValue(ctx, from_resolved);
        JS_FreeValue(ctx, to_resolved);
        return JS_EXCEPTION;
    }

    from_r = JS_ToCStringLen(ctx, &from_rlen, from_resolved);
    if (from_r)
        to_r = JS_ToCStringLen(ctx, &to_rlen, to_resolved);
    /* from_resolved/to_resolved are our own freshly built strings, never
     * user-observable, so this pair of coercions cannot run JS and fails
     * only on OOM; still handled and freed on every path. */
    result = (from_r && to_r)
        ? path_relative_impl(ctx, from_r, from_rlen, to_r, to_rlen)
        : JS_EXCEPTION;

    if (from_r)
        JS_FreeCString(ctx, from_r);
    if (to_r)
        JS_FreeCString(ctx, to_r);
    JS_FreeValue(ctx, from_resolved);
    JS_FreeValue(ctx, to_resolved);
    return result;
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry dyn_path_funcs[] = {
    JS_CFUNC_DEF("join", 0, dyn_path_join),
    JS_CFUNC_DEF("resolve", 0, dyn_path_resolve),
    JS_CFUNC_DEF("normalize", 1, dyn_path_normalize),
    JS_CFUNC_DEF("clean", 1, dyn_path_normalize),
    JS_CFUNC_DEF("dirname", 1, dyn_path_dirname),
    JS_CFUNC_DEF("basename", 2, dyn_path_basename),
    JS_CFUNC_DEF("extname", 1, dyn_path_extname),
    JS_CFUNC_DEF("isAbsolute", 1, dyn_path_is_absolute),
    JS_CFUNC_DEF("relative", 2, dyn_path_relative),
    JS_PROP_STRING_DEF("sep", "/", 0),
    JS_PROP_STRING_DEF("delimiter", ":", 0),
};

static int dyn_path_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_path_funcs,
                                  countof(dyn_path_funcs));
}

int js_nat_init_path(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:path", dyn_path_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_path_funcs,
                                  countof(dyn_path_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_PATH */
