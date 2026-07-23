/*
 * dynajs:compress -- gzip (RFC 1952) compress/decompress. Self-contained, in-repo,
 * no external deps.
 *
 *   import { gzip, gunzip } from "dynajs:compress";
 *   const packed = gzip("hello world".repeat(100)); // str|Uint8Array|ArrayBuffer -> Uint8Array
 *   const bytes  = gunzip(packed);                   // -> Uint8Array
 *   const text   = gunzip(packed, { asString: true });// -> string (UTF-8 decode)
 *
 * Transient plain functions -- no resource, no dispose. The input bytes are
 * copied into a private libc buffer FIRST (fully decoupled from the JS heap),
 * the codec runs entirely in C, then the result is COPIED into a fresh,
 * independent JS value; every C buffer is freed before returning on EVERY path.
 * Nothing native escapes into the JS heap.
 *
 * Codec:
 *   - gzip():   RFC 1952 framing (magic 1f 8b, method 8, mtime=0, OS=ff) around a
 *               DEFLATE stream of *stored* (uncompressed) blocks -- always valid
 *               DEFLATE that any standard decoder accepts -- plus the CRC-32 +
 *               ISIZE trailer.
 *   - gunzip(): a full RFC 1951 inflate (stored / fixed-Huffman / dynamic-Huffman
 *               blocks) so it decodes the output of the system gzip/zlib, with
 *               RFC 1952 header parsing and CRC-32 + ISIZE trailer validation.
 *
 * Every bit/byte read from untrusted input is bounds-checked against the input
 * length; the output size is capped to reject decompression bombs.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Hard cap on decompressed size -- rejects a malicious "zip bomb" before it can
 * exhaust memory. 1 GiB is far beyond any realistic script payload. */
#define DYN_MAX_OUTPUT ((size_t)1 << 30)

#define DYN_STORED_MAX  65535u  /* max LEN of a single DEFLATE stored block */

/* ---------- CRC-32 (IEEE 802.3, gzip trailer) -----------------------------
 * Table-free (bit-serial) so there is no lazily-initialised static state to
 * race across worker threads; the payloads here are small. */
static uint32_t dyn_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---------- growable output buffer (libc-owned) --------------------------- */

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} dyn_outbuf_t;

static int dyn_ob_ensure(dyn_outbuf_t *o, size_t extra)
{
    size_t need, ncap;
    uint8_t *nb;

    if (extra > SIZE_MAX - o->len)
        return -1;
    need = o->len + extra;
    if (need > DYN_MAX_OUTPUT)
        return -1;
    if (need <= o->cap)
        return 0;
    ncap = o->cap ? o->cap : 256;
    while (ncap < need)
        ncap <<= 1; /* bounded: need <= DYN_MAX_OUTPUT, so ncap <= 2*that */
    nb = (uint8_t *)realloc(o->buf, ncap);
    if (!nb)
        return -1;
    o->buf = nb;
    o->cap = ncap;
    return 0;
}

/* ---------- bit reader over an untrusted buffer --------------------------- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;    /* next unread byte */
    uint32_t bitbuf;
    int bitcnt;
    int error;     /* set once a read runs past the end */
} dyn_bitreader_t;

/* Read `need` (0..15) bits LSB-first. On underflow sets error and returns 0.
 * Never reads past `len` -- the only place that touches input memory. */
static int dyn_bits(dyn_bitreader_t *s, int need)
{
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->len) {
            s->error = 1;
            return 0;
        }
        val |= (long)s->data[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (uint32_t)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

/* ---------- Huffman decoding (canonical, RFC 1951 §3.2) -------------------- */

#define DYN_MAXBITS   15
#define DYN_MAXLCODES 286
#define DYN_MAXDCODES 30
#define DYN_FIXLCODES 288
#define DYN_MAXCODES  (DYN_MAXLCODES + DYN_MAXDCODES) /* 316 */

/* Build canonical count[]/symbol[] tables from per-symbol code lengths. Returns
 * 0 for a complete code, >0 for an incomplete code, <0 for an over-subscribed
 * (invalid) code. `length[i]` is trusted to be in [0, DYN_MAXBITS]. */
static int dyn_huff_build(short *count, short *symbol, const short *length, int n)
{
    short offs[DYN_MAXBITS + 1];
    int sym, len, left;

    for (len = 0; len <= DYN_MAXBITS; len++)
        count[len] = 0;
    for (sym = 0; sym < n; sym++)
        count[length[sym]]++;
    if (count[0] == n)
        return 0; /* no codes */
    left = 1;
    for (len = 1; len <= DYN_MAXBITS; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0)
            return left; /* over-subscribed */
    }
    offs[1] = 0;
    for (len = 1; len < DYN_MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + count[len]);
    for (sym = 0; sym < n; sym++)
        if (length[sym] != 0)
            symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

/* Decode one symbol. Returns the symbol, or -1 on an invalid code / underflow.
 * The `code - count < first` walk keeps the returned index inside symbol[] even
 * for incomplete codes (an invalid code falls off the end and returns -1). */
static int dyn_huff_decode(dyn_bitreader_t *s, const short *count,
                           const short *symbol)
{
    int code = 0, first = 0, index = 0, len, cnt;
    for (len = 1; len <= DYN_MAXBITS; len++) {
        code |= dyn_bits(s, 1);
        if (s->error)
            return -1;
        cnt = count[len];
        if (code - cnt < first)
            return symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* ---------- block decoders ------------------------------------------------ */

/* Length/distance base values + extra bits (RFC 1951 §3.2.5). */
static const short dyn_lens[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67,
    83, 99, 115, 131, 163, 195, 227, 258
};
static const short dyn_lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
    5, 5, 5, 0
};
static const short dyn_dists[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const short dyn_dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13
};

/* Decode literal/length symbols until the end-of-block (256). */
static int dyn_inflate_codes(dyn_bitreader_t *s, dyn_outbuf_t *o,
                             const short *lcount, const short *lsym,
                             const short *dcount, const short *dsym)
{
    for (;;) {
        int sym = dyn_huff_decode(s, lcount, lsym);
        if (sym < 0)
            return -1;
        if (sym == 256)
            return 0; /* end of block */
        if (sym < 256) {
            if (dyn_ob_ensure(o, 1))
                return -1;
            o->buf[o->len++] = (uint8_t)sym;
        } else {
            int isym = sym - 257;
            int length, dsv;
            size_t dist, from;
            int k;
            if (isym >= 29)
                return -1; /* 285 is the last valid length code */
            length = dyn_lens[isym] + dyn_bits(s, dyn_lext[isym]);
            if (s->error)
                return -1;
            dsv = dyn_huff_decode(s, dcount, dsym);
            if (dsv < 0 || dsv >= 30)
                return -1;
            dist = (size_t)dyn_dists[dsv] + (size_t)dyn_bits(s, dyn_dext[dsv]);
            if (s->error)
                return -1;
            if (dist > o->len)
                return -1; /* reference before the start of output */
            if (dyn_ob_ensure(o, (size_t)length))
                return -1; /* reserve all of it: buf will not move mid-copy */
            from = o->len - dist;
            for (k = 0; k < length; k++)
                o->buf[o->len++] = o->buf[from++]; /* handles overlap */
        }
    }
}

/* Copy a stored (uncompressed) block. */
static int dyn_inflate_stored(dyn_bitreader_t *s, dyn_outbuf_t *o)
{
    unsigned len, nlen;

    s->bitbuf = 0; /* discard bits back to a byte boundary */
    s->bitcnt = 0;
    if (s->pos + 4 > s->len) {
        s->error = 1;
        return -1;
    }
    len = (unsigned)s->data[s->pos] | ((unsigned)s->data[s->pos + 1] << 8);
    nlen = (unsigned)s->data[s->pos + 2] | ((unsigned)s->data[s->pos + 3] << 8);
    s->pos += 4;
    if ((len ^ 0xffffu) != nlen)
        return -1; /* LEN/NLEN must be one's complements */
    if (len > s->len - s->pos) {
        s->error = 1;
        return -1;
    }
    if (dyn_ob_ensure(o, len))
        return -1;
    if (len) {
        memcpy(o->buf + o->len, s->data + s->pos, len);
        o->len += len;
        s->pos += len;
    }
    return 0;
}

/* Build the fixed Huffman tables and decode a fixed block. */
static int dyn_inflate_fixed(dyn_bitreader_t *s, dyn_outbuf_t *o)
{
    short lengths[DYN_FIXLCODES];
    short lcount[DYN_MAXBITS + 1], lsym[DYN_FIXLCODES];
    short dcount[DYN_MAXBITS + 1], dsym[DYN_MAXDCODES];
    int i;

    for (i = 0; i < 144; i++)
        lengths[i] = 8;
    for (; i < 256; i++)
        lengths[i] = 9;
    for (; i < 280; i++)
        lengths[i] = 7;
    for (; i < 288; i++)
        lengths[i] = 8;
    dyn_huff_build(lcount, lsym, lengths, DYN_FIXLCODES);
    for (i = 0; i < 30; i++)
        lengths[i] = 5;
    dyn_huff_build(dcount, dsym, lengths, 30);
    return dyn_inflate_codes(s, o, lcount, lsym, dcount, dsym);
}

/* Read the dynamic Huffman header, build the tables, and decode the block. */
static int dyn_inflate_dynamic(dyn_bitreader_t *s, dyn_outbuf_t *o)
{
    static const short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    short lengths[DYN_MAXCODES];
    short clcount[DYN_MAXBITS + 1], clsym[19];
    short lcount[DYN_MAXBITS + 1], lsym[DYN_MAXLCODES];
    short dcount[DYN_MAXBITS + 1], dsym[DYN_MAXDCODES];
    int hlit, hdist, hclen, idx, err, total, sym;

    hlit = dyn_bits(s, 5) + 257;
    hdist = dyn_bits(s, 5) + 1;
    hclen = dyn_bits(s, 4) + 4;
    if (s->error)
        return -1;
    if (hlit > DYN_MAXLCODES || hdist > DYN_MAXDCODES)
        return -1;

    for (idx = 0; idx < 19; idx++)
        lengths[idx] = 0;
    for (idx = 0; idx < hclen; idx++)
        lengths[order[idx]] = (short)dyn_bits(s, 3);
    if (s->error)
        return -1;
    if (dyn_huff_build(clcount, clsym, lengths, 19) != 0)
        return -1; /* code-length code must be complete */

    total = hlit + hdist;
    idx = 0;
    while (idx < total) {
        sym = dyn_huff_decode(s, clcount, clsym);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[idx++] = (short)sym;
        } else {
            int rep;
            short val = 0;
            if (sym == 16) {
                if (idx == 0)
                    return -1; /* nothing to repeat */
                val = lengths[idx - 1];
                rep = 3 + dyn_bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + dyn_bits(s, 3);
            } else {
                rep = 11 + dyn_bits(s, 7);
            }
            if (s->error)
                return -1;
            if (idx + rep > total)
                return -1; /* would overrun the length list */
            while (rep--)
                lengths[idx++] = val;
        }
    }
    if (lengths[256] == 0)
        return -1; /* no end-of-block code */

    err = dyn_huff_build(lcount, lsym, lengths, hlit);
    if (err && (err < 0 || hlit != lcount[0] + lcount[1]))
        return -1; /* incomplete ok only for a single one-bit code */
    err = dyn_huff_build(dcount, dsym, lengths + hlit, hdist);
    if (err && (err < 0 || hdist != dcount[0] + dcount[1]))
        return -1;
    return dyn_inflate_codes(s, o, lcount, lsym, dcount, dsym);
}

/* Inflate a raw DEFLATE stream (RFC 1951) into `o`. */
static int dyn_inflate(const uint8_t *src, size_t src_len, dyn_outbuf_t *o)
{
    dyn_bitreader_t s;
    int last, type, rc;

    s.data = src;
    s.len = src_len;
    s.pos = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;
    s.error = 0;

    do {
        last = dyn_bits(&s, 1);
        type = dyn_bits(&s, 2);
        if (s.error)
            return -1;
        if (type == 0)
            rc = dyn_inflate_stored(&s, o);
        else if (type == 1)
            rc = dyn_inflate_fixed(&s, o);
        else if (type == 2)
            rc = dyn_inflate_dynamic(&s, o);
        else
            return -1; /* reserved block type */
        if (rc)
            return -1;
    } while (!last);
    return 0;
}

/* ---------- gzip framing --------------------------------------------------- */

/* Build a gzip member around `src` using DEFLATE stored blocks. Returns a fresh
 * libc buffer via pout / pout_len (caller frees), or -1 (OOM). */
static int dyn_gzip_build(const uint8_t *src, size_t src_len,
                          uint8_t **pout, size_t *pout_len)
{
    size_t nblocks, total, off;
    uint32_t crc, isize;
    uint8_t *out, *p;

    nblocks = src_len ? (src_len + DYN_STORED_MAX - 1) / DYN_STORED_MAX : 1;
    /* 10 header + (5 per stored block header) + payload + 8 trailer. */
    if (src_len > SIZE_MAX - 18 - nblocks * 5)
        return -1;
    total = 10 + nblocks * 5 + src_len + 8;
    out = (uint8_t *)malloc(total);
    if (!out)
        return -1;
    p = out;

    /* RFC 1952 header: magic, CM=deflate, no FLG, MTIME=0, XFL=0, OS=255. */
    *p++ = 0x1f;
    *p++ = 0x8b;
    *p++ = 0x08;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0xff;

    /* DEFLATE stored blocks. */
    if (src_len == 0) {
        *p++ = 0x01; /* BFINAL=1, BTYPE=00 */
        *p++ = 0x00;
        *p++ = 0x00; /* LEN = 0 */
        *p++ = 0xff;
        *p++ = 0xff; /* NLEN = ~0 */
    } else {
        off = 0;
        while (off < src_len) {
            size_t chunk = src_len - off;
            unsigned len, nlen;
            if (chunk > DYN_STORED_MAX)
                chunk = DYN_STORED_MAX;
            len = (unsigned)chunk;
            nlen = (~len) & 0xffffu;
            *p++ = (off + chunk == src_len) ? 0x01 : 0x00; /* BFINAL on last */
            *p++ = (uint8_t)(len & 0xff);
            *p++ = (uint8_t)((len >> 8) & 0xff);
            *p++ = (uint8_t)(nlen & 0xff);
            *p++ = (uint8_t)((nlen >> 8) & 0xff);
            memcpy(p, src + off, chunk);
            p += chunk;
            off += chunk;
        }
    }

    /* RFC 1952 trailer: CRC-32 then ISIZE (mod 2^32), both little-endian. */
    crc = dyn_crc32(src, src_len);
    isize = (uint32_t)(src_len & 0xffffffffu);
    *p++ = (uint8_t)(crc & 0xff);
    *p++ = (uint8_t)((crc >> 8) & 0xff);
    *p++ = (uint8_t)((crc >> 16) & 0xff);
    *p++ = (uint8_t)((crc >> 24) & 0xff);
    *p++ = (uint8_t)(isize & 0xff);
    *p++ = (uint8_t)((isize >> 8) & 0xff);
    *p++ = (uint8_t)((isize >> 16) & 0xff);
    *p++ = (uint8_t)((isize >> 24) & 0xff);

    *pout = out;
    *pout_len = (size_t)(p - out);
    return 0;
}

/* Parse a gzip member (RFC 1952), inflate it, and validate the trailer.
 * Returns 0 with `o` filled, or -1 on any malformed/corrupt/truncated input. */
static int dyn_gunzip_decode(const uint8_t *src, size_t len, dyn_outbuf_t *o)
{
    size_t pos = 10;
    uint8_t flg;
    uint32_t crc, isize;

    if (len < 18) /* 10-byte header + 8-byte trailer minimum */
        return -1;
    if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 0x08)
        return -1;
    flg = src[3];
    if (flg & 0xe0) /* reserved FLG bits must be zero */
        return -1;

    if (flg & 0x04) { /* FEXTRA */
        size_t xlen;
        if (pos + 2 > len)
            return -1;
        xlen = (size_t)src[pos] | ((size_t)src[pos + 1] << 8);
        pos += 2;
        if (xlen > len - pos)
            return -1;
        pos += xlen;
    }
    if (flg & 0x08) { /* FNAME */
        while (pos < len && src[pos] != 0)
            pos++;
        if (pos >= len)
            return -1;
        pos++;
    }
    if (flg & 0x10) { /* FCOMMENT */
        while (pos < len && src[pos] != 0)
            pos++;
        if (pos >= len)
            return -1;
        pos++;
    }
    if (flg & 0x02) { /* FHCRC */
        if (pos + 2 > len)
            return -1;
        pos += 2;
    }

    if (pos + 8 > len) /* need room for the trailer after the header/fields */
        return -1;
    if (dyn_inflate(src + pos, len - 8 - pos, o) < 0)
        return -1;

    crc = (uint32_t)src[len - 8] | ((uint32_t)src[len - 7] << 8) |
          ((uint32_t)src[len - 6] << 16) | ((uint32_t)src[len - 5] << 24);
    isize = (uint32_t)src[len - 4] | ((uint32_t)src[len - 3] << 8) |
            ((uint32_t)src[len - 2] << 16) | ((uint32_t)src[len - 1] << 24);
    if (dyn_crc32(o->buf, o->len) != crc)
        return -1;
    if ((uint32_t)(o->len & 0xffffffffu) != isize)
        return -1;
    return 0;
}

/* ---------- JS input/output boundary --------------------------------------- */

/* Copy the argument's bytes into a fresh libc buffer and release every JS-side
 * handle before returning, so the pointer handed back is decoupled from the JS
 * heap. Returns 0 with *pout (never NULL) / *plen set, or -1 with a pending JS
 * exception. Accepts a string, a typed array, or an ArrayBuffer. */
static int dyn_read_input(JSContext *ctx, JSValueConst val, uint8_t **pout,
                          size_t *plen)
{
    uint8_t *base, *copy;
    size_t off = 0, tlen = 0, ab = 0;
    JSValue buf;

    if (JS_IsString(val)) {
        size_t slen;
        const char *str = JS_ToCStringLen(ctx, &slen, val);
        if (!str)
            return -1;
        copy = (uint8_t *)malloc(slen ? slen : 1);
        if (!copy) {
            JS_FreeCString(ctx, str);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (slen)
            memcpy(copy, str, slen);
        JS_FreeCString(ctx, str);
        *pout = copy;
        *plen = slen;
        return 0;
    }

    buf = JS_GetTypedArrayBuffer(ctx, val, &off, &tlen, NULL);
    if (!JS_IsException(buf)) {
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        if (!base) {
            JS_FreeValue(ctx, buf);
            return -1;
        }
        if (off > ab)
            off = ab;
        if (tlen > ab - off)
            tlen = ab - off;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx)); /* not a typed array */
        base = JS_GetArrayBuffer(ctx, &ab, val);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_ThrowTypeError(ctx, "dynajs:compress: input must be a string, "
                                   "Uint8Array, or ArrayBuffer");
            return -1;
        }
        buf = JS_UNDEFINED; /* bare ArrayBuffer: no reference to release */
        off = 0;
        tlen = ab;
    }

    copy = (uint8_t *)malloc(tlen ? tlen : 1);
    if (!copy) {
        JS_FreeValue(ctx, buf);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (tlen)
        memcpy(copy, base + off, tlen);
    JS_FreeValue(ctx, buf);
    *pout = copy;
    *plen = tlen;
    return 0;
}

/* Copy `len` bytes into a fresh, independent JS Uint8Array over its own buffer. */
static JSValue dyn_bytes_to_uint8(JSContext *ctx, const uint8_t *data, size_t len)
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
    u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab); /* u8 keeps its own reference to the buffer */
    return u8;
}

/* ---------- gzip / gunzip -------------------------------------------------- */

static JSValue dyn_gzip(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    JSValue result;

    (void)this_val;

    /* Accept an optional level for signature compatibility; the stored-block
     * encoder ignores it. Coerce it FIRST (a throwing valueOf strands nothing). */
    if (argc > 1 && JS_IsNumber(argv[1])) {
        int32_t lv;
        if (JS_ToInt32(ctx, &lv, argv[1]))
            return JS_EXCEPTION;
        (void)lv;
    }

    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_gzip_build(src, src_len, &out, &out_len) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_gunzip(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    uint8_t *src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;

    (void)this_val;

    /* Optional { asString: true } -> UTF-8 decode to a JS string. Read the
     * (possibly getter-backed) property before touching native buffers. */
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }

    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_gunzip_decode(src, src_len, &o) < 0) {
        free(src);
        free(o.buf);
        return JS_ThrowTypeError(ctx, "dynajs:compress gunzip: invalid gzip data");
    }
    free(src);

    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char *)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

/* ---------- module registration -------------------------------------------- */

static const JSCFunctionListEntry dyn_compress_funcs[] = {
    JS_CFUNC_DEF("gzip", 1, dyn_gzip),
    JS_CFUNC_DEF("gunzip", 1, dyn_gunzip),
};

static int dyn_compress_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_compress_funcs,
                                  countof(dyn_compress_funcs));
}

int js_nat_init_compress(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dynajs:compress", dyn_compress_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_compress_funcs,
                                  countof(dyn_compress_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_COMPRESS */
