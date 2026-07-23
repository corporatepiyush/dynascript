/*
 * dynajs:encoding -- binary/text encodings, mirroring Go's encoding/hex,
 * encoding/base32, encoding/base64, encoding/binary (varint), and
 * encoding/ascii85. Self-contained, in-repo (no external deps).
 *
 *   import { hexEncode, hexDecode, base64Encode, base64Decode,
 *            base64UrlEncode, base64UrlDecode, base32Encode, base32Decode,
 *            base32HexEncode, base32HexDecode,
 *            putUvarint, uvarint, putVarint, varint,
 *            base85Encode, base85Decode } from "dynajs:encoding";
 *
 *   hexEncode("abc");                 // -> "616263"
 *   hexDecode("616263");              // -> Uint8Array [0x61,0x62,0x63]
 *   base64Encode("foo");              // -> "Zm9v"
 *   base64UrlEncode(new Uint8Array([0xfb, 0xff])); // -> "-_8" (no padding)
 *   base32Encode("foobar");           // -> "MZXW6YTBOI======"
 *   putUvarint(300);                  // -> Uint8Array [0xAC, 0x02]
 *   uvarint(new Uint8Array([0xAC, 0x02])); // -> [300, 2]
 *   putVarint(-1);                    // -> Uint8Array [0x01]     (zigzag)
 *   base85Encode("foo");              // -> "AoDS"
 *
 * Byte semantics: every *Encode function accepts a string (encoded as UTF-8
 * via JS_ToCStringLen), a plain ArrayBuffer, or any 1-byte-per-element
 * TypedArray view (dyn_enc_bytes) -- the same "TypedArray boundary" resolved
 * by dynajs:bytes, extended with dynajs:text's string fallback since the
 * hex/base64/base32 RFC vectors are conventionally exercised on text. Every
 * *Decode function takes a JS string (coerced via JS_ToCStringLen -- a
 * non-string is stringified, matching every other codec in this codebase).
 * putUvarint/putVarint/uvarint/varint operate on raw bytes only (a byte view,
 * no string fallback), mirroring Go's []byte-only encoding/binary API.
 *
 * base64/base32 reuse the shared multi-ISA SIMD kernel table's
 * base64_encode/base64_decode (dynajs-simd-kernels.h, the same engine
 * dynajs:search/dynajs:text/dynajs:bytes run on) for the standard alphabet;
 * base64Url derives from it (translate '+/' <-> '-_' and add/strip '='
 * padding) since the kernel itself is alphabet-fixed. base32 has no shared
 * kernel (scalar-only; RFC 4648's 5-byte/8-char blocking is cheap either
 * way) and is implemented directly, parameterized over the standard and
 * extended-hex alphabets (JS_CFUNC_MAGIC_DEF, matching dynajs:bytes' width/
 * sign/endian read/write dispatch).
 *
 * varint: putUvarint(v)/putVarint(v) accept EITHER a Number that must be an
 * EXACT (non-negative, respectively any-sign) safe integer, |v| <= 2^53-1
 * (a fractional or out-of-range Number throws RangeError rather than
 * silently truncating/wrapping, since this encodes a value, not an array
 * index or a fixed-width field) OR a BigInt (the full 64-bit range; wraps
 * modulo 2^64, matching dynajs:bytes' writeBigUint64LE/writeBigInt64LE
 * convention).
 * uvarint(bytes)/varint(bytes) invert this automatically: a decoded
 * magnitude that fits in a safe integer comes back as a Number, otherwise as
 * a BigInt -- so "BigInt for values beyond 2^53" is a property of the
 * VALUE, not a caller-chosen mode. Both decoders mirror Go's
 * encoding/binary.Uvarint/Varint return-code convention exactly (rather than
 * throwing) since it is the precise, well-specified way to report a
 * truncated buffer vs a >64-bit overflow: bytesRead > 0 is success, 0 means
 * `bytes` ran out before a terminating byte (truncated), and a negative
 * -(i+1) means the value would need more than 64 bits at byte i (overflow).
 *
 * Reentrancy discipline (CLAUDE.md): every method coerces ALL scalar/BigInt
 * arguments to C locals FIRST (JS_ToFloat64/JS_ToBigInt64/JS_ToCStringLen,
 * any of which may run arbitrary JS via valueOf/
 * @@toPrimitive) and only THEN resolves a buffer argument's backing pointer
 * (dyn_enc_view, a pure structural query that never invokes JS). These are
 * plain functions with no `this` and no closable resource, so there is no
 * native handle a re-entrant valueOf could free out from under the call.
 * Every returned Uint8Array/string is a fresh copy -- no native pointer
 * escapes into a JS value -- and every owned C string (JS_ToCStringLen) and
 * malloc'd scratch buffer is freed on every path, including every error
 * path.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dynajs-simd-kernels.h" /* base64_encode/base64_decode, shared with text/bytes */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Number.MAX_SAFE_INTEGER == 2^53-1: the largest magnitude a JS double
 * represents exactly. Values beyond this must travel as BigInt. */
#define DYN_ENC_MAX_SAFE_INT (((int64_t)1 << 53) - 1)
/* ceil(64/7): the longest a LEB128 varint of a 64-bit value can be. */
#define DYN_ENC_MAX_VARINT_LEN 10

/* ---------- buffer boundary: Uint8Array/Int8Array/Uint8ClampedArray view or
 * a plain ArrayBuffer -> raw byte pointer + length (dynajs:bytes' dyn_bytes_view,
 * duplicated per this module's own conventions -- see dynajs-bytes.c). ---------- */
static int dyn_enc_view(JSContext *ctx, JSValueConst v, uint8_t **pp, size_t *pn)
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
    JS_FreeValue(ctx, JS_GetException(ctx)); /* clear: not a plain ArrayBuffer */

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1; /* not a TypedArray either; propagate that TypeError */
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

/* Resolve `v` to raw bytes for an *Encode function: a string (UTF-8 via
 * JS_ToCStringLen -> *owned, must be released via JS_FreeCString) or a byte
 * view (dyn_enc_view; *owned left NULL, zero-copy). */
static int dyn_enc_bytes(JSContext *ctx, JSValueConst v, const uint8_t **data,
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
        uint8_t *p;
        size_t n;
        if (dyn_enc_view(ctx, v, &p, &n))
            return -1;
        *data = p;
        *len = n;
        return 0;
    }
}

/* Build a fresh Uint8Array copying `len` bytes from `data` (never aliases a
 * native pointer into JS). `data` may be NULL only when len==0. */
static JSValue dyn_enc_new_u8array(JSContext *ctx, const uint8_t *data, size_t len)
{
    static const uint8_t zero_stub = 0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (len == 0)
        data = &zero_stub;
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

/* ══════════════════════════════ hex ══════════════════════════════ */

/* hex enc/dec run on the shared SIMD kernel (PSHUFB on x86, table-lookup on
 * NEON) — same one dynajs:text uses; several GB/s on long inputs. */
static JSValue dyn_enc_hex_encode(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;

    out = (char *)malloc(n ? n * 2 : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    simd.hex_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, n * 2);
    free(out);
    return result;
}

static JSValue dyn_enc_hex_decode(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, outlen, dec;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (slen & 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "hexDecode: odd-length hex string");
    }
    outlen = slen / 2;
    out = (uint8_t *)malloc(outlen ? outlen : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec = simd.hex_decode(str, slen, out); /* SIZE_MAX on any non-hex digit */
    JS_FreeCString(ctx, str);
    if (dec == SIZE_MAX) {
        free(out);
        return JS_ThrowSyntaxError(ctx, "hexDecode: invalid hex digit");
    }
    result = dyn_enc_new_u8array(ctx, out, dec);
    free(out);
    return result;
}

/* ══════════════════════════════ base64 (standard, via SIMD kernel) ══════ */

static JSValue dyn_enc_base64_encode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, cap, written;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = 4 * ((n + 2) / 3);
    out = (char *)malloc(cap ? cap : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    written = simd.base64_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, written);
    free(out);
    return result;
}

static JSValue dyn_enc_base64_decode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const char *str;
    size_t n, cap, declen;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    cap = 3 * (n / 4);
    out = (uint8_t *)malloc(cap ? cap : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = simd.base64_decode(str, n, out);
    JS_FreeCString(ctx, str);
    if (declen == SIZE_MAX) {
        free(out);
        return JS_ThrowSyntaxError(ctx, "base64Decode: invalid base64 string");
    }
    result = dyn_enc_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

/* base64url (RFC 4648 sec.5): '-'/'_' instead of '+'/'/', no padding. Derived
 * from the standard kernel: encode then translate+strip padding; decode
 * translates back and re-pads to a multiple of 4 before handing off (the
 * kernel itself requires n%4==0). A stray '+'/'/' is rejected -- those bytes
 * are simply not part of the url-safe alphabet (matches Go's
 * base64.URLEncoding, whose reverse table has no entry for them either). */
static JSValue dyn_enc_base64url_encode(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, cap, written, i, o;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = 4 * ((n + 2) / 3);
    out = (char *)malloc(cap ? cap : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    written = simd.base64_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);

    for (i = 0, o = 0; i < written; i++) {
        char c = out[i];
        if (c == '=')
            break; /* padding is always a contiguous tail run: done */
        out[o++] = (c == '+') ? '-' : (c == '/') ? '_' : c;
    }
    result = JS_NewStringLen(ctx, out, o);
    free(out);
    return result;
}

static JSValue dyn_enc_base64url_decode(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, rem, padlen, buflen, cap, declen, i;
    char *buf;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    rem = slen % 4;
    if (rem == 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "base64UrlDecode: invalid length");
    }
    padlen = rem ? 4 - rem : 0;
    buflen = slen + padlen;
    buf = (char *)malloc(buflen ? buflen : 1);
    if (!buf) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < slen; i++) {
        char c = str[i];
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        } else if (c == '+' || c == '/') {
            free(buf);
            JS_FreeCString(ctx, str);
            return JS_ThrowSyntaxError(ctx, "base64UrlDecode: invalid character");
        }
        buf[i] = c;
    }
    JS_FreeCString(ctx, str);
    for (i = 0; i < padlen; i++)
        buf[slen + i] = '=';

    cap = 3 * (buflen / 4);
    out = (uint8_t *)malloc(cap ? cap : 1);
    if (!out) {
        free(buf);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = simd.base64_decode(buf, buflen, out);
    free(buf);
    if (declen == SIZE_MAX) {
        free(out);
        return JS_ThrowSyntaxError(ctx, "base64UrlDecode: invalid base64url string");
    }
    result = dyn_enc_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

/* ══════════════════════════════ base32 (RFC 4648) ══════════════════════ */

enum { DYN_B32_STD = 0, DYN_B32_HEX = 1 };

static const char dyn_b32_std_alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static const char dyn_b32_hex_alpha[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";

static const char *dyn_b32_alpha(int magic)
{
    return magic == DYN_B32_HEX ? dyn_b32_hex_alpha : dyn_b32_std_alpha;
}

/* Encode `len` bytes (5-byte -> 8-char blocks, '='-padded per RFC 4648
 * sec.6) using `alpha` (a 32-byte alphabet table). `out` needs
 * ((len+4)/5)*8 bytes. */
static size_t dyn_enc_b32_encode(const uint8_t *restrict data, size_t len,
                                 const char *restrict alpha, char *restrict out)
{
    /* nbytes-remaining (0..5) -> count of non-padding chars in the block. */
    static const uint8_t nchars_tab[6] = {0, 2, 4, 5, 7, 8};
    size_t i, o = 0;

    for (i = 0; i < len; i += 5) {
        uint8_t chunk[5] = {0, 0, 0, 0, 0};
        size_t nb = (len - i < 5) ? len - i : 5;
        size_t j, nc;
        uint64_t v;

        for (j = 0; j < nb; j++)
            chunk[j] = data[i + j];
        v = ((uint64_t)chunk[0] << 32) | ((uint64_t)chunk[1] << 24) |
            ((uint64_t)chunk[2] << 16) | ((uint64_t)chunk[3] << 8) | chunk[4];
        nc = nchars_tab[nb];
        for (j = 0; j < nc; j++)
            out[o++] = alpha[(v >> (35 - 5 * j)) & 0x1F];
        for (; j < 8; j++)
            out[o++] = '=';
    }
    return o;
}

/* Decode an 8-char-block '='-padded base32 string. Returns the byte count,
 * or SIZE_MAX on any malformed input: length not a multiple of 8, a
 * character outside `alpha`, padding anywhere but the final block, or a
 * padding count that isn't one of RFC 4648's five valid block shapes. `out`
 * needs (slen/8)*5 bytes. */
static size_t dyn_enc_b32_decode(const char *restrict str, size_t slen,
                                 const char *restrict alpha, uint8_t *restrict out)
{
    /* count of non-padding chars in a block (0..8) -> decoded byte count;
     * -1 marks a char-count RFC 4648 never produces (1,3,6, or an all-pad
     * block), which cannot be honest base32. */
    static const int8_t nbytes_tab[9] = { -1, -1, 1, -1, 2, 3, -1, 4, 5 };
    int8_t rev[256];
    size_t i, o = 0;
    int k;

    if (slen % 8 != 0)
        return SIZE_MAX;
    memset(rev, -1, sizeof(rev));
    for (k = 0; k < 32; k++)
        rev[(uint8_t)alpha[k]] = (int8_t)k;

    for (i = 0; i < slen; i += 8) {
        uint8_t c[8], bytes[5];
        int pad, nreal, nbytes, last_group, j;
        uint64_t v = 0;

        last_group = (i + 8 == slen);
        pad = 0;
        for (j = 7; j >= 0; j--) {
            if (str[i + j] != '=')
                break;
            pad++;
        }
        if (pad > 0 && !last_group)
            return SIZE_MAX; /* padding only valid in the string's last block */
        nreal = 8 - pad;
        nbytes = nbytes_tab[nreal];
        if (nbytes < 0)
            return SIZE_MAX;

        for (j = 0; j < nreal; j++) {
            int8_t val = rev[(uint8_t)str[i + j]];
            if (val < 0)
                return SIZE_MAX;
            c[j] = (uint8_t)val;
        }
        for (; j < 8; j++)
            c[j] = 0; /* padding slots contribute zero bits */

        for (j = 0; j < 8; j++)
            v = (v << 5) | c[j];
        bytes[0] = (uint8_t)(v >> 32);
        bytes[1] = (uint8_t)(v >> 24);
        bytes[2] = (uint8_t)(v >> 16);
        bytes[3] = (uint8_t)(v >> 8);
        bytes[4] = (uint8_t)v;
        for (j = 0; j < nbytes; j++)
            out[o++] = bytes[j];
    }
    return o;
}

static JSValue dyn_enc_base32_encode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    const uint8_t *data;
    size_t n, cap, written;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = ((n + 4) / 5) * 8;
    out = (char *)malloc(cap ? cap : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    written = dyn_enc_b32_encode(data, n, dyn_b32_alpha(magic), out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, written);
    free(out);
    return result;
}

static JSValue dyn_enc_base32_decode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    const char *str;
    size_t slen, cap, declen;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    cap = (slen / 8) * 5;
    out = (uint8_t *)malloc(cap ? cap : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = dyn_enc_b32_decode(str, slen, dyn_b32_alpha(magic), out);
    JS_FreeCString(ctx, str);
    if (declen == SIZE_MAX) {
        free(out);
        return JS_ThrowSyntaxError(ctx, magic == DYN_B32_HEX ?
            "base32HexDecode: invalid base32hex string" :
            "base32Decode: invalid base32 string");
    }
    result = dyn_enc_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

/* ══════════════════ varint: Go encoding/binary (LEB128 + zigzag) ═══════ */

/* PutUvarint: 1-10 bytes, little-endian base-128 groups, continuation bit
 * (0x80) set on every byte but the last. `out` needs >= 10 bytes. */
static size_t dyn_enc_put_uvarint(uint64_t x, uint8_t *out)
{
    size_t i = 0;
    while (x >= 0x80) {
        out[i++] = (uint8_t)(x | 0x80);
        x >>= 7;
    }
    out[i++] = (uint8_t)x;
    return i;
}

/* Uvarint: mirrors Go's encoding/binary.Uvarint exactly. Returns bytes
 * consumed (>0) on success; 0 if `buf` ran out before a terminating byte
 * (truncated); or -(i+1) if the value would need more than 64 bits
 * (overflow at byte i). */
static int dyn_enc_uvarint(const uint8_t *buf, size_t n, uint64_t *out)
{
    uint64_t x = 0;
    unsigned s = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        uint8_t b = buf[i];
        if (i == DYN_ENC_MAX_VARINT_LEN)
            return -(int)(i + 1);
        if (b < 0x80) {
            if (i == DYN_ENC_MAX_VARINT_LEN - 1 && b > 1)
                return -(int)(i + 1); /* final byte can only hold 1 more bit */
            *out = x | ((uint64_t)b << s);
            return (int)(i + 1);
        }
        x |= (uint64_t)(b & 0x7f) << s;
        s += 7;
    }
    *out = 0;
    return 0;
}

/* PutVarint/Varint: zigzag-encode the int64 (0,-1,1,-2,2,... -> 0,1,2,3,4,...)
 * then LEB128 it, exactly like Go's encoding/binary. */
static size_t dyn_enc_put_varint(int64_t x, uint8_t *out)
{
    uint64_t ux = (uint64_t)x << 1;
    if (x < 0)
        ux = ~ux;
    return dyn_enc_put_uvarint(ux, out);
}

static int dyn_enc_varint(const uint8_t *buf, size_t n, int64_t *out)
{
    uint64_t ux;
    int nb = dyn_enc_uvarint(buf, n, &ux);
    int64_t x;

    if (nb <= 0) {
        *out = 0;
        return nb;
    }
    x = (int64_t)(ux >> 1);
    if (ux & 1)
        x = ~x;
    *out = x;
    return nb;
}

/* Coerce a JS value to the uint64 magnitude putUvarint encodes: a BigInt (the
 * full 64-bit range; wraps mod 2^64, matching writeBigUint64LE) or a Number
 * that must be an exact non-negative safe integer (<= 2^53-1). A fractional
 * or out-of-range Number throws RangeError rather than silently truncating/
 * wrapping (unlike JS_ToIndex or the fixed-width dynajs:bytes accessors):
 * putUvarint encodes an exact value, not an array index or a fixed-width
 * field, so silently accepting e.g. 1.5 would hide a caller bug. Use a
 * BigInt for anything beyond 2^53-1. */
static int dyn_enc_to_u64(JSContext *ctx, JSValueConst v, uint64_t *out)
{
    if (JS_IsBigInt(ctx, v)) {
        int64_t raw;
        if (JS_ToBigInt64(ctx, &raw, v))
            return -1;
        *out = (uint64_t)raw;
        return 0;
    }
    {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= 0 && d <= (double)DYN_ENC_MAX_SAFE_INT && floor(d) == d)) {
            JS_ThrowRangeError(ctx, "putUvarint: value must be a non-negative safe integer or a BigInt");
            return -1;
        }
        *out = (uint64_t)d;
        return 0;
    }
}

/* Same for putVarint: a BigInt (full int64 range, wraps mod 2^64) or a
 * Number that must be an exact safe integer of either sign. */
static int dyn_enc_to_i64(JSContext *ctx, JSValueConst v, int64_t *out)
{
    if (JS_IsBigInt(ctx, v))
        return JS_ToBigInt64(ctx, out, v);
    {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= (double)-DYN_ENC_MAX_SAFE_INT &&
              d <= (double)DYN_ENC_MAX_SAFE_INT && floor(d) == d)) {
            JS_ThrowRangeError(ctx, "putVarint: value must be a safe integer or a BigInt");
            return -1;
        }
        *out = (int64_t)d;
        return 0;
    }
}

/* Inverse: a decoded magnitude picks Number when it fits exactly (matching
 * Number.MAX_SAFE_INTEGER), else BigInt -- so callers never see silent
 * precision loss on either decoder. */
static JSValue dyn_enc_u64_to_js(JSContext *ctx, uint64_t v)
{
    if (v <= (uint64_t)DYN_ENC_MAX_SAFE_INT)
        return JS_NewFloat64(ctx, (double)v);
    return JS_NewBigUint64(ctx, v);
}

static JSValue dyn_enc_i64_to_js(JSContext *ctx, int64_t v)
{
    if (v >= -DYN_ENC_MAX_SAFE_INT && v <= DYN_ENC_MAX_SAFE_INT)
        return JS_NewFloat64(ctx, (double)v);
    return JS_NewBigInt64(ctx, v);
}

/* Build the [value, bytesRead] result pair. Takes ownership of `value`
 * (consumed by JS_DefinePropertyValueUint32 on both success and failure). */
static JSValue dyn_enc_pair(JSContext *ctx, JSValue value, int32_t n)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, value);
        return arr;
    }
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, value, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewInt32(ctx, n), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static JSValue dyn_enc_put_uvarint_js(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    uint64_t v;
    uint8_t buf[DYN_ENC_MAX_VARINT_LEN];
    (void)this_val; (void)argc;

    if (dyn_enc_to_u64(ctx, argv[0], &v))
        return JS_EXCEPTION;
    return dyn_enc_new_u8array(ctx, buf, dyn_enc_put_uvarint(v, buf));
}

static JSValue dyn_enc_put_varint_js(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    int64_t v;
    uint8_t buf[DYN_ENC_MAX_VARINT_LEN];
    (void)this_val; (void)argc;

    if (dyn_enc_to_i64(ctx, argv[0], &v))
        return JS_EXCEPTION;
    return dyn_enc_new_u8array(ctx, buf, dyn_enc_put_varint(v, buf));
}

static JSValue dyn_enc_uvarint_js(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint8_t *buf;
    size_t n;
    uint64_t v = 0;
    int nb;
    (void)this_val; (void)argc;

    if (dyn_enc_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    nb = dyn_enc_uvarint(buf, n, &v);
    return dyn_enc_pair(ctx, nb > 0 ? dyn_enc_u64_to_js(ctx, v) : JS_NewInt32(ctx, 0), nb);
}

static JSValue dyn_enc_varint_js(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint8_t *buf;
    size_t n;
    int64_t v = 0;
    int nb;
    (void)this_val; (void)argc;

    if (dyn_enc_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    nb = dyn_enc_varint(buf, n, &v);
    return dyn_enc_pair(ctx, nb > 0 ? dyn_enc_i64_to_js(ctx, v) : JS_NewInt32(ctx, 0), nb);
}

/* ══════════════════════════════ base85 (ascii85) ══════════════════════ */
/*
 * Mirrors Go's encoding/ascii85 exactly: '!'..'u' (33-117) is a base-85 big-
 * endian digit alphabet, 4 input bytes -> 5 output chars, with the 'z'
 * shorthand for an all-zero 4-byte group (encode only; only recognized at a
 * decode group boundary -- 'z' elsewhere is corrupt input). A trailing
 * partial group of 1-3 input bytes encodes as (that count + 1) characters
 * (the high-order digits of the 5; Go discards the low-order ones). Decode's
 * inverse: a trailing group of 2-4 characters (1 is impossible/corrupt)
 * decodes by treating the missing digit(s) as maximal ('u'=84) before
 * extracting the high-order (count-1) bytes. Whitespace (space/tab/CR/LF/
 * VT/FF) is skipped, matching ascii85's use inside line-wrapped text formats
 * (PostScript, PDF). Algorithm verified byte-for-byte against Python's
 * base64.a85encode/a85decode(adobe=False) -- see tests/test_encoding.js.
 */

static int dyn_enc_is_ascii85_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* Encode `len` bytes into `out` (needs ((len+3)/4)*5 bytes); returns bytes
 * written. Empty input -> 0 bytes. */
static size_t dyn_enc_b85_encode(const uint8_t *restrict data, size_t len,
                                 char *restrict out)
{
    size_t i, o = 0;

    for (i = 0; i < len; i += 4) {
        size_t nb = (len - i < 4) ? len - i : 4;
        uint32_t v = 0;
        char c[5];
        size_t m, j;

        for (j = 0; j < nb; j++)
            v |= (uint32_t)data[i + j] << (24 - 8 * j);

        if (nb == 4 && v == 0) {
            out[o++] = 'z';
            continue;
        }
        for (j = 0; j < 5; j++) {
            c[4 - j] = (char)('!' + (v % 85));
            v /= 85;
        }
        m = (nb < 4) ? nb + 1 : 5; /* a partial final group emits nb+1 chars */
        for (j = 0; j < m; j++)
            out[o++] = c[j];
    }
    return o;
}

/* Decode an ascii85 string into `out` (needs slen*4 bytes -- a safe upper
 * bound: no valid input decodes more than 4 bytes per character, and a
 * lone 'z' hits exactly that ratio). Returns bytes written, or SIZE_MAX on
 * a corrupt input: a byte outside whitespace/'!'-'u'/'z', a 'z' off a group
 * boundary, or a trailing group of exactly 1 leftover character. */
static size_t dyn_enc_b85_decode(const char *restrict str, size_t slen,
                                 uint8_t *restrict out)
{
    uint32_t v = 0;
    unsigned nb = 0;
    size_t i, o = 0;

    for (i = 0; i < slen; i++) {
        unsigned char b = (unsigned char)str[i];
        if (dyn_enc_is_ascii85_space(b))
            continue;
        if (b == 'z' && nb == 0) {
            out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;
            continue;
        }
        if (b < '!' || b > 'u')
            return SIZE_MAX;
        v = v * 85 + (uint32_t)(b - '!');
        nb++;
        if (nb == 5) {
            out[o++] = (uint8_t)(v >> 24);
            out[o++] = (uint8_t)(v >> 16);
            out[o++] = (uint8_t)(v >> 8);
            out[o++] = (uint8_t)v;
            v = 0;
            nb = 0;
        }
    }
    if (nb == 1)
        return SIZE_MAX; /* a lone leftover digit cannot decode to a byte */
    if (nb > 1) {
        unsigned k;
        for (k = nb; k < 5; k++)
            v = v * 85 + 84; /* missing trailing digits treated as maximal ('u') */
        for (k = 0; k < nb - 1; k++)
            out[o++] = (uint8_t)(v >> (24 - 8 * k));
    }
    return o;
}

static JSValue dyn_enc_base85_encode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, cap, written;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = ((n + 3) / 4) * 5;
    out = (char *)malloc(cap ? cap : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    written = dyn_enc_b85_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, written);
    free(out);
    return result;
}

static JSValue dyn_enc_base85_decode(JSContext *ctx, JSValueConst this_val,
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
    cap = slen * 4;
    out = (uint8_t *)malloc(cap ? cap : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = dyn_enc_b85_decode(str, slen, out);
    JS_FreeCString(ctx, str);
    if (declen == SIZE_MAX) {
        free(out);
        return JS_ThrowSyntaxError(ctx, "base85Decode: invalid ascii85 string");
    }
    result = dyn_enc_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry dyn_enc_funcs[] = {
    JS_CFUNC_DEF("hexEncode", 1, dyn_enc_hex_encode),
    JS_CFUNC_DEF("hexDecode", 1, dyn_enc_hex_decode),

    JS_CFUNC_DEF("base64Encode", 1, dyn_enc_base64_encode),
    JS_CFUNC_DEF("base64Decode", 1, dyn_enc_base64_decode),
    JS_CFUNC_DEF("base64UrlEncode", 1, dyn_enc_base64url_encode),
    JS_CFUNC_DEF("base64UrlDecode", 1, dyn_enc_base64url_decode),

    JS_CFUNC_MAGIC_DEF("base32Encode", 1, dyn_enc_base32_encode, DYN_B32_STD),
    JS_CFUNC_MAGIC_DEF("base32Decode", 1, dyn_enc_base32_decode, DYN_B32_STD),
    JS_CFUNC_MAGIC_DEF("base32HexEncode", 1, dyn_enc_base32_encode, DYN_B32_HEX),
    JS_CFUNC_MAGIC_DEF("base32HexDecode", 1, dyn_enc_base32_decode, DYN_B32_HEX),

    JS_CFUNC_DEF("putUvarint", 1, dyn_enc_put_uvarint_js),
    JS_CFUNC_DEF("uvarint", 1, dyn_enc_uvarint_js),
    JS_CFUNC_DEF("putVarint", 1, dyn_enc_put_varint_js),
    JS_CFUNC_DEF("varint", 1, dyn_enc_varint_js),

    JS_CFUNC_DEF("base85Encode", 1, dyn_enc_base85_encode),
    JS_CFUNC_DEF("base85Decode", 1, dyn_enc_base85_decode),
};

static int dyn_enc_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_enc_funcs, countof(dyn_enc_funcs));
}

int js_nat_init_encoding(JSContext *ctx)
{
    JSModuleDef *m;
    simd_init(); /* idempotent (pthread_once): select the best base64 kernel */
    m = JS_NewCModule(ctx, "dynajs:encoding", dyn_enc_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_enc_funcs, countof(dyn_enc_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_ENCODING */
