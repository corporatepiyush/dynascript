/*
 * dynajs:bytes -- byte-buffer utilities over Uint8Array/ArrayBuffer, mirroring
 * Go's `bytes` package (compare/equal/indexOf/lastIndexOf/contains/count/copy/
 * concat/fill) plus Node's Buffer fixed-width read/write helpers and hex/
 * base64/utf8 codecs. Self-contained, in-repo (no external deps).
 *
 *   import { compare, indexOf, readUint32LE, writeUint32LE, toHex } from "dynajs:bytes";
 *   compare(new Uint8Array([1,2]), new Uint8Array([1,3]));  // -1
 *   indexOf(buf, 0x0a);                 // first newline byte, or -1
 *   indexOf(buf, new Uint8Array([13,10])); // first "\r\n", or -1
 *   const off = writeUint32LE(buf, 0, 0xdeadbeef);  // -> 4 (next offset)
 *   readUint32LE(buf, 0);               // -> 0xdeadbeef
 *   toHex(new Uint8Array([0xde, 0xad])); // "dead"
 *
 * Byte semantics: every buffer argument is a Uint8Array/Int8Array/
 * Uint8ClampedArray view (any 1-byte-per-element TypedArray) or a plain
 * ArrayBuffer -- resolved zero-copy onto the backing store, no per-call
 * allocation for reads. `needle` in indexOf/lastIndexOf/contains/count is
 * either a byte value (Number, 0..255) or another byte view. Multi-byte
 * search reuses the shared SIMD substring kernel (dynajs-simd-kernels.h,
 * the same engine dynajs:search/dynajs:text run on).
 *
 * Endianness: every fixed-width accessor assembles/disassembles bytes
 * explicitly (a plain shift loop keyed by an LE/BE flag), independent of the
 * host's native byte order -- no bswap/is_be() needed. 64-bit reads/writes
 * always use BigInt (lossless; a JS Number cannot hold the full 64-bit
 * range), matching DataView.getBigInt64/getBigUint64. Out-of-range VALUES
 * wrap modulo 2^width (matching DataView.setUint8/TypedArray element
 * assignment, not Node Buffer's throw-on-out-of-range); out-of-range
 * OFFSETS always throw RangeError.
 *
 * count()/indexOf() empty-needle convention: this module has no notion of
 * "characters" (it is byte-oriented, not text-oriented), so unlike Go's
 * bytes.Count (which special-cases an empty separator as 1 + a UTF-8 rune
 * count) count(buf, emptyNeedle) is defined here as buf.length + 1 -- the
 * number of insertion points among the raw bytes. indexOf/lastIndexOf with
 * an empty needle return 0 / buf.length respectively (matching Go's
 * bytes.Index/LastIndex).
 *
 * Reentrancy discipline (CLAUDE.md): every method coerces ALL scalar
 * arguments (offsets, values, lengths) to C locals FIRST -- via
 * JS_ToIndex/JS_ToInt32/JS_ToUint32/JS_ToFloat64/JS_ToBigInt64, any of which
 * may run arbitrary JS through valueOf/@@toPrimitive on a non-primitive
 * argument -- and only THEN resolves a buffer argument's backing pointer
 * (dyn_bytes_view, a pure structural query that never invokes JS). No
 * JS-invoking call ever runs between a resolve and its use. A `needle` that
 * is itself a byte view is likewise resolved only after any scalar
 * coercion; resolving a second/third buffer argument is safe in any order
 * since none of those resolutions can themselves run JS. Every returned
 * Uint8Array/string is a fresh copy -- no native pointer escapes into a JS
 * value.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BYTES)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dynajs-simd-kernels.h" /* strfind / base64 kernels, shared with search + text */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------- buffer boundary: Uint8Array/Int8Array/Uint8ClampedArray view or
 * a plain ArrayBuffer -> raw byte pointer + length ---------- */

/* Resolve `v` to its backing byte pointer and length. Accepts a plain
 * ArrayBuffer (whole buffer) or any 1-byte-per-element TypedArray view;
 * rejects a wider-element view (Uint16Array etc) as a type error -- this
 * module is byte-oriented only. On failure a pending exception is set and
 * -1 is returned. The returned pointer is valid only for the synchronous
 * remainder of the call (see the module-level reentrancy note above). */
static int dyn_bytes_view(JSContext *ctx, JSValueConst v, uint8_t **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        *pp = base;
        *pn = ab;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx)); /* not an ArrayBuffer: clear, retry as a view */

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a Uint8Array or ArrayBuffer");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1; /* detached mid-resolve; JS_GetArrayBuffer already threw */
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

/* Build a fresh Uint8Array copying `len` bytes from `data` (never aliases a
 * native pointer into JS). `data` may be NULL only when len==0. */
static JSValue dyn_bytes_new_u8array(JSContext *ctx, const uint8_t *data, size_t len)
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

/* ---------- compare / equal ---------- */

static JSValue dyn_bytes_compare(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint8_t *a, *b;
    size_t an, bn, minlen;
    int cmp;
    (void)this_val; (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &a, &an))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &b, &bn))
        return JS_EXCEPTION;

    minlen = an < bn ? an : bn;
    cmp = minlen ? memcmp(a, b, minlen) : 0;
    if (cmp != 0)
        cmp = cmp < 0 ? -1 : 1;
    else
        cmp = (an > bn) - (an < bn);
    return JS_NewInt32(ctx, cmp);
}

static JSValue dyn_bytes_equal(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    uint8_t *a, *b;
    size_t an, bn;
    (void)this_val; (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &a, &an))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &b, &bn))
        return JS_EXCEPTION;

    return JS_NewBool(ctx, an == bn && (an == 0 || memcmp(a, b, an) == 0));
}

/* ---------- needle resolution shared by indexOf/lastIndexOf/contains/count ---------- */

/* Resolve a search needle: either a byte value (a JS Number, 0..255 via
 * ToInt32 truncation) or a byte view (Uint8Array/ArrayBuffer). A numeric
 * needle is always a primitive (JS_IsNumber is a pure tag check), so its
 * ToInt32 can never invoke user JS -- neither path here risks running JS
 * between resolving the needle and resolving `buf` (done by the caller
 * right after this returns). `byte_out` is 1 byte of caller-owned storage
 * used as the pattern buffer for the numeric case. */
static int dyn_bytes_needle(JSContext *ctx, JSValueConst v, uint8_t *byte_out,
                            const uint8_t **pat, size_t *plen)
{
    if (JS_IsNumber(v)) {
        int32_t b;
        if (JS_ToInt32(ctx, &b, v))
            return -1;
        *byte_out = (uint8_t)b;
        *pat = byte_out;
        *plen = 1;
        return 0;
    }
    {
        uint8_t *p;
        size_t n;
        if (dyn_bytes_view(ctx, v, &p, &n))
            return -1;
        *pat = p;
        *plen = n;
        return 0;
    }
}

/* Last occurrence of pat[0..plen) in text[0..tlen), or (size_t)-1. Plain
 * backward memcmp scan -- the shared SIMD engine has no reverse-search
 * kernel, and this is the standard textbook algorithm. Requires
 * 1 <= plen <= tlen (checked by every caller before this runs). */
static size_t dyn_bytes_last_find(const uint8_t *text, size_t tlen,
                                  const uint8_t *pat, size_t plen)
{
    size_t i = tlen - plen + 1;
    while (i > 0) {
        i--;
        if (memcmp(text + i, pat, plen) == 0)
            return i;
    }
    return (size_t)-1;
}

static JSValue dyn_bytes_index_of(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint8_t needle_byte;
    const uint8_t *pat;
    size_t plen;
    uint8_t *buf;
    size_t n, pos;
    (void)this_val; (void)argc;

    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (plen == 0)
        return JS_NewInt32(ctx, 0); /* bytes.Index(s, []) == 0 */
    if (plen > n)
        return JS_NewInt32(ctx, -1);
    pos = simd.strfind(buf, n, pat, plen);
    return pos == SIZE_MAX ? JS_NewInt32(ctx, -1) : JS_NewInt64(ctx, (int64_t)pos);
}

static JSValue dyn_bytes_last_index_of(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    uint8_t needle_byte;
    const uint8_t *pat;
    size_t plen;
    uint8_t *buf;
    size_t n, pos;
    (void)this_val; (void)argc;

    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)n); /* bytes.LastIndex(s, []) == len(s) */
    if (plen > n)
        return JS_NewInt32(ctx, -1);
    pos = dyn_bytes_last_find(buf, n, pat, plen);
    return pos == (size_t)-1 ? JS_NewInt32(ctx, -1) : JS_NewInt64(ctx, (int64_t)pos);
}

static JSValue dyn_bytes_contains(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint8_t needle_byte;
    const uint8_t *pat;
    size_t plen;
    uint8_t *buf;
    size_t n;
    (void)this_val; (void)argc;

    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (plen == 0)
        return JS_NewBool(ctx, 1);
    if (plen > n)
        return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, simd.strfind(buf, n, pat, plen) != SIZE_MAX);
}

/* count(buf, needle): NON-overlapping occurrences (Go's bytes.Count
 * semantics), unlike dynajs:search's indexOfAll (which is deliberately
 * overlapping). See the module doc comment for the empty-needle
 * convention. */
static JSValue dyn_bytes_count(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    uint8_t needle_byte;
    const uint8_t *pat;
    size_t plen;
    uint8_t *buf;
    size_t n, scan, cnt;
    (void)this_val; (void)argc;

    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)n + 1);
    if (plen > n)
        return JS_NewInt32(ctx, 0);

    cnt = 0;
    scan = 0;
    while (scan <= n - plen) {
        size_t pos = simd.strfind(buf + scan, n - scan, pat, plen);
        if (pos == SIZE_MAX)
            break;
        cnt++;
        scan += pos + plen; /* non-overlapping: skip past this match */
    }
    return JS_NewInt64(ctx, (int64_t)cnt);
}

/* ---------- concat / copy / fill ---------- */

/* concat(arrayOfByteViews) -> Uint8Array. Pass 1 validates every element and
 * sums lengths (each element is fetched, resolved, and immediately
 * released -- no pointer is held across the next element's fetch, which may
 * run arbitrary JS via a getter/Proxy trap). Pass 2 re-resolves each
 * element fresh right before copying it; the copy length is clamped to
 * whatever remains of the pass-1-sized destination, so a length change from
 * JS run during element access (getter/Proxy) can never overflow this
 * module's own allocation -- it can only yield fewer/more source bytes than
 * pass 1 counted. */
static JSValue dyn_bytes_concat(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue len_val, elem, result = JS_EXCEPTION;
    uint32_t n, i;
    uint8_t *out;
    size_t total, used;
    (void)this_val; (void)argc;

    len_val = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(len_val))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &n, len_val)) {
        JS_FreeValue(ctx, len_val);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len_val);

    total = 0;
    for (i = 0; i < n; i++) {
        uint8_t *p;
        size_t plen;
        elem = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(elem))
            return JS_EXCEPTION;
        if (dyn_bytes_view(ctx, elem, &p, &plen)) {
            JS_FreeValue(ctx, elem);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, elem);
        total += plen;
    }

    out = (uint8_t *)malloc(total ? total : 1);
    if (!out)
        return JS_ThrowOutOfMemory(ctx);

    used = 0;
    for (i = 0; i < n; i++) {
        uint8_t *p;
        size_t plen, remaining;
        elem = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(elem))
            goto done;
        if (dyn_bytes_view(ctx, elem, &p, &plen)) {
            JS_FreeValue(ctx, elem);
            goto done;
        }
        JS_FreeValue(ctx, elem);
        remaining = total - used;
        if (plen > remaining)
            plen = remaining;
        if (plen)
            memcpy(out + used, p, plen);
        used += plen;
    }
    result = dyn_bytes_new_u8array(ctx, out, used);

 done:
    free(out);
    return result;
}

/* copy(dst, src, dstOff=0, srcOff=0, len=min(dst.length-dstOff, src.length-srcOff))
 * -> number of bytes copied. Overlap-safe (memmove): mirrors Go's copy()
 * and Node's Buffer.prototype.copy. All three optional scalars are coerced
 * before either buffer is resolved. */
static JSValue dyn_bytes_copy(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    uint64_t dst_off = 0, src_off = 0, len = UINT64_MAX; /* MAX = "not given" */
    uint8_t *dst, *src;
    size_t dn, sn;
    (void)this_val; (void)argc;

    if (!JS_IsUndefined(argv[2]) && JS_ToIndex(ctx, &dst_off, argv[2]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[3]) && JS_ToIndex(ctx, &src_off, argv[3]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[4]) && JS_ToIndex(ctx, &len, argv[4]))
        return JS_EXCEPTION;

    if (dyn_bytes_view(ctx, argv[0], &dst, &dn))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &src, &sn))
        return JS_EXCEPTION;

    if (dst_off > (uint64_t)dn)
        return JS_ThrowRangeError(ctx, "copy: dstOffset out of bounds");
    if (src_off > (uint64_t)sn)
        return JS_ThrowRangeError(ctx, "copy: srcOffset out of bounds");

    {
        uint64_t davail = (uint64_t)dn - dst_off, savail = (uint64_t)sn - src_off;
        uint64_t maxlen = davail < savail ? davail : savail;
        if (len == UINT64_MAX)
            len = maxlen;
        else if (len > maxlen)
            return JS_ThrowRangeError(ctx, "copy: length out of bounds");
    }

    if (len)
        memmove(dst + (size_t)dst_off, src + (size_t)src_off, (size_t)len);
    return JS_NewInt64(ctx, (int64_t)len);
}

/* fill(buf, val, start=0, end=buf.length): sets buf[start..end) to the low 8
 * bits of val; returns buf. start/end/val are all coerced before buf is
 * resolved. */
static JSValue dyn_bytes_fill(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    int32_t val;
    uint64_t start = 0, end = UINT64_MAX; /* MAX = "not given" */
    uint8_t *buf;
    size_t n;
    (void)this_val; (void)argc;

    if (JS_ToInt32(ctx, &val, argv[1]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[2]) && JS_ToIndex(ctx, &start, argv[2]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[3]) && JS_ToIndex(ctx, &end, argv[3]))
        return JS_EXCEPTION;

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (end == UINT64_MAX)
        end = (uint64_t)n;
    if (start > (uint64_t)n || end > (uint64_t)n || start > end)
        return JS_ThrowRangeError(ctx, "fill: start/end out of bounds");

    memset(buf + start, (uint8_t)val, (size_t)(end - start));
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- fixed-width read/write, magic-dispatched over every width/sign/endian ---------- */

enum {
    DYN_U8, DYN_I8,
    DYN_U16LE, DYN_U16BE, DYN_I16LE, DYN_I16BE,
    DYN_U32LE, DYN_U32BE, DYN_I32LE, DYN_I32BE,
    DYN_U64LE, DYN_U64BE, DYN_I64LE, DYN_I64BE,
    DYN_F32LE, DYN_F32BE, DYN_F64LE, DYN_F64BE,
    DYN_FIELD_COUNT
};

typedef enum { DK_UINT, DK_INT, DK_FLOAT, DK_BIGUINT, DK_BIGINT } DynBytesKind;

typedef struct {
    uint8_t width; /* bytes: 1, 2, 4, or 8 */
    uint8_t be;    /* 1 = big-endian, 0 = little-endian (irrelevant when width==1) */
    uint8_t kind;  /* DynBytesKind */
} DynBytesField;

static const DynBytesField dyn_bytes_fields[DYN_FIELD_COUNT] = {
    [DYN_U8]    = { 1, 0, DK_UINT },
    [DYN_I8]    = { 1, 0, DK_INT },
    [DYN_U16LE] = { 2, 0, DK_UINT },
    [DYN_U16BE] = { 2, 1, DK_UINT },
    [DYN_I16LE] = { 2, 0, DK_INT },
    [DYN_I16BE] = { 2, 1, DK_INT },
    [DYN_U32LE] = { 4, 0, DK_UINT },
    [DYN_U32BE] = { 4, 1, DK_UINT },
    [DYN_I32LE] = { 4, 0, DK_INT },
    [DYN_I32BE] = { 4, 1, DK_INT },
    [DYN_U64LE] = { 8, 0, DK_BIGUINT },
    [DYN_U64BE] = { 8, 1, DK_BIGUINT },
    [DYN_I64LE] = { 8, 0, DK_BIGINT },
    [DYN_I64BE] = { 8, 1, DK_BIGINT },
    [DYN_F32LE] = { 4, 0, DK_FLOAT },
    [DYN_F32BE] = { 4, 1, DK_FLOAT },
    [DYN_F64LE] = { 8, 0, DK_FLOAT },
    [DYN_F64BE] = { 8, 1, DK_FLOAT },
};

/* Assemble `width` bytes at `p` (LE or BE per `be`) into a uint64_t (the raw
 * bit pattern, zero-extended). Host-endianness independent. */
static uint64_t dyn_bytes_load(const uint8_t *p, int width, int be)
{
    uint64_t v = 0;
    int i;
    if (be) {
        for (i = 0; i < width; i++)
            v = (v << 8) | p[i];
    } else {
        for (i = width - 1; i >= 0; i--)
            v = (v << 8) | p[i];
    }
    return v;
}

/* Inverse of dyn_bytes_load: write the low `width` bytes of `v` to `p`. */
static void dyn_bytes_store(uint8_t *p, uint64_t v, int width, int be)
{
    int i;
    if (be) {
        for (i = width - 1; i >= 0; i--) {
            p[i] = (uint8_t)v;
            v >>= 8;
        }
    } else {
        for (i = 0; i < width; i++) {
            p[i] = (uint8_t)v;
            v >>= 8;
        }
    }
}

static JSValue dyn_bytes_read(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    const DynBytesField *f = &dyn_bytes_fields[magic];
    uint64_t off;
    uint8_t *buf;
    size_t n;
    uint64_t v;
    (void)this_val; (void)argc;

    if (JS_ToIndex(ctx, &off, argv[1]))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (off + f->width > (uint64_t)n)
        return JS_ThrowRangeError(ctx, "read: offset out of bounds");

    v = dyn_bytes_load(buf + off, f->width, f->be);
    switch (f->kind) {
    case DK_UINT:
        if (f->width == 4)
            return JS_NewUint32(ctx, (uint32_t)v);
        return JS_NewInt32(ctx, (int32_t)v); /* width 1 or 2: always non-negative */
    case DK_INT:
        if (f->width == 1)
            return JS_NewInt32(ctx, (int8_t)v);
        if (f->width == 2)
            return JS_NewInt32(ctx, (int16_t)v);
        return JS_NewInt32(ctx, (int32_t)v); /* width 4 */
    case DK_BIGUINT:
        return JS_NewBigUint64(ctx, v);
    case DK_BIGINT:
        return JS_NewBigInt64(ctx, (int64_t)v);
    case DK_FLOAT:
    default:
        if (f->width == 4) {
            union { uint32_t i; float f; } u;
            u.i = (uint32_t)v;
            return JS_NewFloat64(ctx, (double)u.f);
        } else {
            union { uint64_t i; double f; } u;
            u.i = v;
            return JS_NewFloat64(ctx, u.f);
        }
    }
}

static JSValue dyn_bytes_write(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    const DynBytesField *f = &dyn_bytes_fields[magic];
    uint64_t off;
    uint32_t u32 = 0;
    int64_t i64 = 0;
    double d = 0;
    uint64_t v = 0;
    uint8_t *buf;
    size_t n;
    (void)this_val; (void)argc;

    if (JS_ToIndex(ctx, &off, argv[1]))
        return JS_EXCEPTION;

    switch (f->kind) {
    case DK_UINT:
    case DK_INT:
        if (JS_ToUint32(ctx, &u32, argv[2]))
            return JS_EXCEPTION;
        v = u32;
        break;
    case DK_BIGUINT:
    case DK_BIGINT:
        if (JS_ToBigInt64(ctx, &i64, argv[2]))
            return JS_EXCEPTION;
        v = (uint64_t)i64;
        break;
    case DK_FLOAT:
    default:
        if (JS_ToFloat64(ctx, &d, argv[2]))
            return JS_EXCEPTION;
        if (f->width == 4) {
            union { uint32_t i; float f; } u;
            u.f = (float)d;
            v = u.i;
        } else {
            union { uint64_t i; double f; } u;
            u.f = d;
            v = u.i;
        }
        break;
    }

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (off + f->width > (uint64_t)n)
        return JS_ThrowRangeError(ctx, "write: offset out of bounds");

    dyn_bytes_store(buf + off, v, f->width, f->be);
    return JS_NewInt64(ctx, (int64_t)(off + f->width));
}

/* ---------- hex / base64 / utf8 codecs ---------- */

static const char dyn_bytes_hex_digits[] = "0123456789abcdef";

static JSValue dyn_bytes_to_hex(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    uint8_t *buf;
    size_t n, i;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    out = (char *)malloc(n ? n * 2 : 1);
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) {
        out[i * 2]     = dyn_bytes_hex_digits[buf[i] >> 4];
        out[i * 2 + 1] = dyn_bytes_hex_digits[buf[i] & 0xF];
    }
    result = JS_NewStringLen(ctx, out, n * 2);
    free(out);
    return result;
}

static int dyn_bytes_hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static JSValue dyn_bytes_from_hex(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, i, outlen;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    if (slen & 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "fromHex: odd-length hex string");
    }
    outlen = slen / 2;
    out = (uint8_t *)malloc(outlen ? outlen : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < outlen; i++) {
        int hi = dyn_bytes_hex_val((unsigned char)str[i * 2]);
        int lo = dyn_bytes_hex_val((unsigned char)str[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            free(out);
            JS_FreeCString(ctx, str);
            return JS_ThrowSyntaxError(ctx, "fromHex: invalid hex digit");
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    JS_FreeCString(ctx, str);
    result = dyn_bytes_new_u8array(ctx, out, outlen);
    free(out);
    return result;
}

static JSValue dyn_bytes_to_base64(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    uint8_t *buf;
    size_t n, cap, written;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    cap = 4 * ((n + 2) / 3);
    out = (char *)malloc(cap ? cap : 1);
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    written = simd.base64_encode(buf, n, out);
    result = JS_NewStringLen(ctx, out, written);
    free(out);
    return result;
}

static JSValue dyn_bytes_from_base64(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, cap, declen;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    cap = 3 * (slen / 4);
    out = (uint8_t *)malloc(cap ? cap : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = simd.base64_decode(str, slen, out);
    JS_FreeCString(ctx, str);
    if (declen == SIZE_MAX) {
        free(out);
        return JS_ThrowSyntaxError(ctx, "fromBase64: invalid base64 string");
    }
    result = dyn_bytes_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

/* toUtf8(buf) decodes the raw bytes as UTF-8 (lone/invalid sequences become
 * U+FFFD, matching JS_NewStringLen's general string-construction contract --
 * this is NOT validated UTF-8; use dynajs:text's isValidUtf8 first if that
 * matters). fromUtf8(str) is the inverse (UTF-8 encode via JS_ToCStringLen,
 * the same encoder every other native module uses). */
static JSValue dyn_bytes_to_utf8(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint8_t *buf;
    size_t n;
    (void)this_val; (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, (const char *)buf, n);
}

static JSValue dyn_bytes_from_utf8(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    result = dyn_bytes_new_u8array(ctx, (const uint8_t *)str, len);
    JS_FreeCString(ctx, str);
    return result;
}

/* ---------- module registration ----------
 *
 * The `length` in every JS_C[FUNC_MAGIC]_DEF below MUST equal the highest
 * argv[] index the handler unconditionally reads, NOT the count of
 * "conceptually required" JS arguments -- js_call_c_function only pads
 * argv[] with JS_UNDEFINED up to the declared length when the CALLER passes
 * fewer arguments than that; if the caller passes as many or more, argv is
 * used as-is with only the caller's actual argc slots valid, so reading a
 * higher index is an out-of-bounds read of whatever happens to be on the VM
 * stack next. copy/fill take optional trailing args (dstOff/srcOff/len,
 * start/end) accessed unconditionally, so they declare 5/4, not 2. */
static const JSCFunctionListEntry dyn_bytes_funcs[] = {
    JS_CFUNC_DEF("compare", 2, dyn_bytes_compare),
    JS_CFUNC_DEF("equal", 2, dyn_bytes_equal),
    JS_CFUNC_DEF("indexOf", 2, dyn_bytes_index_of),
    JS_CFUNC_DEF("lastIndexOf", 2, dyn_bytes_last_index_of),
    JS_CFUNC_DEF("contains", 2, dyn_bytes_contains),
    JS_CFUNC_DEF("count", 2, dyn_bytes_count),
    JS_CFUNC_DEF("concat", 1, dyn_bytes_concat),
    JS_CFUNC_DEF("copy", 5, dyn_bytes_copy),
    JS_CFUNC_DEF("fill", 4, dyn_bytes_fill),

    JS_CFUNC_MAGIC_DEF("readUint8", 2, dyn_bytes_read, DYN_U8),
    JS_CFUNC_MAGIC_DEF("readInt8", 2, dyn_bytes_read, DYN_I8),
    JS_CFUNC_MAGIC_DEF("readUint16LE", 2, dyn_bytes_read, DYN_U16LE),
    JS_CFUNC_MAGIC_DEF("readUint16BE", 2, dyn_bytes_read, DYN_U16BE),
    JS_CFUNC_MAGIC_DEF("readInt16LE", 2, dyn_bytes_read, DYN_I16LE),
    JS_CFUNC_MAGIC_DEF("readInt16BE", 2, dyn_bytes_read, DYN_I16BE),
    JS_CFUNC_MAGIC_DEF("readUint32LE", 2, dyn_bytes_read, DYN_U32LE),
    JS_CFUNC_MAGIC_DEF("readUint32BE", 2, dyn_bytes_read, DYN_U32BE),
    JS_CFUNC_MAGIC_DEF("readInt32LE", 2, dyn_bytes_read, DYN_I32LE),
    JS_CFUNC_MAGIC_DEF("readInt32BE", 2, dyn_bytes_read, DYN_I32BE),
    JS_CFUNC_MAGIC_DEF("readBigUint64LE", 2, dyn_bytes_read, DYN_U64LE),
    JS_CFUNC_MAGIC_DEF("readBigUint64BE", 2, dyn_bytes_read, DYN_U64BE),
    JS_CFUNC_MAGIC_DEF("readBigInt64LE", 2, dyn_bytes_read, DYN_I64LE),
    JS_CFUNC_MAGIC_DEF("readBigInt64BE", 2, dyn_bytes_read, DYN_I64BE),
    JS_CFUNC_MAGIC_DEF("readFloatLE", 2, dyn_bytes_read, DYN_F32LE),
    JS_CFUNC_MAGIC_DEF("readFloatBE", 2, dyn_bytes_read, DYN_F32BE),
    JS_CFUNC_MAGIC_DEF("readDoubleLE", 2, dyn_bytes_read, DYN_F64LE),
    JS_CFUNC_MAGIC_DEF("readDoubleBE", 2, dyn_bytes_read, DYN_F64BE),

    JS_CFUNC_MAGIC_DEF("writeUint8", 3, dyn_bytes_write, DYN_U8),
    JS_CFUNC_MAGIC_DEF("writeInt8", 3, dyn_bytes_write, DYN_I8),
    JS_CFUNC_MAGIC_DEF("writeUint16LE", 3, dyn_bytes_write, DYN_U16LE),
    JS_CFUNC_MAGIC_DEF("writeUint16BE", 3, dyn_bytes_write, DYN_U16BE),
    JS_CFUNC_MAGIC_DEF("writeInt16LE", 3, dyn_bytes_write, DYN_I16LE),
    JS_CFUNC_MAGIC_DEF("writeInt16BE", 3, dyn_bytes_write, DYN_I16BE),
    JS_CFUNC_MAGIC_DEF("writeUint32LE", 3, dyn_bytes_write, DYN_U32LE),
    JS_CFUNC_MAGIC_DEF("writeUint32BE", 3, dyn_bytes_write, DYN_U32BE),
    JS_CFUNC_MAGIC_DEF("writeInt32LE", 3, dyn_bytes_write, DYN_I32LE),
    JS_CFUNC_MAGIC_DEF("writeInt32BE", 3, dyn_bytes_write, DYN_I32BE),
    JS_CFUNC_MAGIC_DEF("writeBigUint64LE", 3, dyn_bytes_write, DYN_U64LE),
    JS_CFUNC_MAGIC_DEF("writeBigUint64BE", 3, dyn_bytes_write, DYN_U64BE),
    JS_CFUNC_MAGIC_DEF("writeBigInt64LE", 3, dyn_bytes_write, DYN_I64LE),
    JS_CFUNC_MAGIC_DEF("writeBigInt64BE", 3, dyn_bytes_write, DYN_I64BE),
    JS_CFUNC_MAGIC_DEF("writeFloatLE", 3, dyn_bytes_write, DYN_F32LE),
    JS_CFUNC_MAGIC_DEF("writeFloatBE", 3, dyn_bytes_write, DYN_F32BE),
    JS_CFUNC_MAGIC_DEF("writeDoubleLE", 3, dyn_bytes_write, DYN_F64LE),
    JS_CFUNC_MAGIC_DEF("writeDoubleBE", 3, dyn_bytes_write, DYN_F64BE),

    JS_CFUNC_DEF("toHex", 1, dyn_bytes_to_hex),
    JS_CFUNC_DEF("fromHex", 1, dyn_bytes_from_hex),
    JS_CFUNC_DEF("toBase64", 1, dyn_bytes_to_base64),
    JS_CFUNC_DEF("fromBase64", 1, dyn_bytes_from_base64),
    JS_CFUNC_DEF("toUtf8", 1, dyn_bytes_to_utf8),
    JS_CFUNC_DEF("fromUtf8", 1, dyn_bytes_from_utf8),
};

static int dyn_bytes_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_bytes_funcs,
                                  countof(dyn_bytes_funcs));
}

int js_nat_init_bytes(JSContext *ctx)
{
    JSModuleDef *m;
    simd_init(); /* idempotent (pthread_once): select the best strfind/base64 kernels */
    m = JS_NewCModule(ctx, "dynajs:bytes", dyn_bytes_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_bytes_funcs,
                                  countof(dyn_bytes_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_BYTES */
