/*
 * dyna:bits -- a faithful port of Go's `math/bits` package. Self-contained,
 * in-repo (no external deps). Pure functions (no `this`, no native resource,
 * nothing to close).
 *
 *   import { LeadingZeros32, OnesCount64, RotateLeft8, Mul64, Div64, UintSize }
 *       from "dyna:bits";
 *   LeadingZeros32(1);        // -> 31
 *   OnesCount64(14n);         // -> 3
 *   RotateLeft8(15, 2);       // -> 60   (0x0F rotated left 2 -> 0x3C)
 *   RotateLeft8(15, -2);      // -> 195  (negative k rotates right -> 0xC3)
 *   Mul64(0xffffffffffffffffn, 0xffffffffffffffffn); // -> [hi, lo] BigInts
 *   Div64(0n, 6n, 3n);        // -> [2n, 0n]
 *   UintSize;                 // -> 64
 *
 * TYPE RULE (JS Numbers cannot hold 64 bits exactly, so the width-64 variants
 * speak BigInt):
 *   - 8/16/32-bit VALUE arguments are JS Numbers, coerced with ECMAScript
 *     ToUint32 (JS_ToUint32) and masked to their width. A BigInt passed to one
 *     of these throws a TypeError (ToNumber(BigInt) throws) -- pass a Number.
 *   - 64-bit VALUE arguments are BigInts, read with JS_ToBigInt64 (which throws
 *     a TypeError for a Number argument, and truncates modulo 2^64 for a
 *     magnitude wider than 64 bits -- the same convention every native module
 *     here uses, since dynajs.h exposes no arbitrary-precision BigInt reader).
 *     The low 64 bits are reinterpreted unsigned (two's complement), so a
 *     caller passing a value in [0, 2^64) round-trips exactly.
 *   - The rotate COUNT k of RotateLeft8/16/32/64 is always a JS Number (Go's
 *     `k int`), even for the 64-bit variant -- only the value x is a BigInt.
 *
 * RETURN TYPES (mirroring Go's return types exactly):
 *   - LeadingZeros/TrailingZeros/OnesCount/Len return an `int` in Go -> a JS
 *     Number for EVERY width (0..width), including the 64-bit variants whose
 *     ARGUMENT is a BigInt but whose RESULT is a small Number.
 *   - Reverse/ReverseBytes/RotateLeft return the same width they take: a JS
 *     Number for 8/16/32, a BigInt for 64.
 *   - Add/Sub/Mul/Div return a two-element array; Rem returns a scalar. The
 *     element type follows the width (Number for 32, BigInt for 64):
 *       Add32/Add64(a,b,carry) -> [sum, carryOut]
 *       Sub32/Sub64(a,b,borrow) -> [diff, borrowOut]
 *       Mul32/Mul64(a,b)        -> [hi, lo]
 *       Div32/Div64(hi,lo,y)    -> [quo, rem]
 *       Rem32/Rem64(hi,lo,y)    -> rem
 *
 * EDGE CASES (Go-defined, and the ones C makes UB):
 *   - LeadingZeros(0) == width, TrailingZeros(0) == width, Len(0) == 0.
 *     __builtin_clz(0)/__builtin_ctz(0) are UB, so zero is special-cased.
 *   - RotateLeft reduces k modulo width as Go does: s = uint(k) & (width-1),
 *     so a negative k rotates right and any k wraps. A rotate by s==0 is
 *     returned as the identity to avoid the C UB of `x >> width`.
 *   - Div32/Div64 THROW a RangeError (Go panics) when y == 0 (divide error) or
 *     when y <= hi (the quotient would not fit -- overflow). Rem32/Rem64 throw
 *     only for y == 0; a would-be-overflowing quotient is fine for a remainder.
 *
 * Memory discipline: each function coerces ALL of its JS arguments to C locals
 * FIRST (there is no native resource a reentrant valueOf could invalidate --
 * these are plain functions -- but a value read must precede any use). A
 * two-element result is built via JS_NewArray + JS_DefinePropertyValueUint32,
 * freeing the partial array on any failure.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BITS)

#include <stdint.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ================================================================ *
 *  Argument readers: every value is coerced to a width-masked uint64
 *  (8/16/32 from a Number, 64 from a BigInt) so the shared scalar
 *  kernels below can operate on one representation.
 * ================================================================ */

static int bits_read8(JSContext *ctx, JSValueConst v, uint64_t *out)
{
    uint32_t u;
    if (JS_ToUint32(ctx, &u, v))
        return -1;
    *out = u & 0xFFu;
    return 0;
}

static int bits_read16(JSContext *ctx, JSValueConst v, uint64_t *out)
{
    uint32_t u;
    if (JS_ToUint32(ctx, &u, v))
        return -1;
    *out = u & 0xFFFFu;
    return 0;
}

static int bits_read32(JSContext *ctx, JSValueConst v, uint64_t *out)
{
    uint32_t u;
    if (JS_ToUint32(ctx, &u, v))
        return -1;
    *out = u;
    return 0;
}

/* JS_ToBigInt64 throws a TypeError if v is a Number and truncates modulo 2^64
 * for a wider magnitude; the low 64 bits reinterpret unsigned bit-for-bit. */
static int bits_read64(JSContext *ctx, JSValueConst v, uint64_t *out)
{
    int64_t s;
    if (JS_ToBigInt64(ctx, &s, v))
        return -1;
    *out = (uint64_t)s;
    return 0;
}

/* ================================================================ *
 *  Scalar bit kernels (x is pre-masked to its width).
 * ================================================================ */

/* Len: minimum bits to represent x (0 for x==0), width-independent because
 * clzll acts on the true highest set-bit position of the masked value. */
static int bits_len_u64(uint64_t x)
{
    return x ? 64 - __builtin_clzll(x) : 0;
}

/* Full-width bit reversal: reverse bits within each byte, then reverse the
 * byte order. Smaller widths reverse the whole 64 bits then shift the result
 * (which lands in the high end) back down. */
static uint64_t bits_rev64(uint64_t x)
{
    x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
    x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    return __builtin_bswap64(x);
}

/* ================================================================ *
 *  Bit-count family (LeadingZeros/TrailingZeros/OnesCount/Len). Every
 *  variant returns a JS Number (0..width), including the 64-bit ones
 *  whose argument is a BigInt.
 * ================================================================ */
#define BITS_COUNT_GROUP(W) \
    static JSValue bits_leading_zeros##W(JSContext *ctx, JSValueConst this_val, \
                                         int argc, JSValueConst *argv) { \
        uint64_t x; (void)this_val; (void)argc; \
        if (bits_read##W(ctx, argv[0], &x)) return JS_EXCEPTION; \
        return JS_NewInt32(ctx, (W) - bits_len_u64(x)); } \
    static JSValue bits_trailing_zeros##W(JSContext *ctx, JSValueConst this_val, \
                                          int argc, JSValueConst *argv) { \
        uint64_t x; (void)this_val; (void)argc; \
        if (bits_read##W(ctx, argv[0], &x)) return JS_EXCEPTION; \
        return JS_NewInt32(ctx, x ? __builtin_ctzll(x) : (W)); } \
    static JSValue bits_ones_count##W(JSContext *ctx, JSValueConst this_val, \
                                      int argc, JSValueConst *argv) { \
        uint64_t x; (void)this_val; (void)argc; \
        if (bits_read##W(ctx, argv[0], &x)) return JS_EXCEPTION; \
        return JS_NewInt32(ctx, __builtin_popcountll(x)); } \
    static JSValue bits_len##W(JSContext *ctx, JSValueConst this_val, \
                               int argc, JSValueConst *argv) { \
        uint64_t x; (void)this_val; (void)argc; \
        if (bits_read##W(ctx, argv[0], &x)) return JS_EXCEPTION; \
        return JS_NewInt32(ctx, bits_len_u64(x)); }

BITS_COUNT_GROUP(8)
BITS_COUNT_GROUP(16)
BITS_COUNT_GROUP(32)
BITS_COUNT_GROUP(64)

/* ================================================================ *
 *  Reverse (bit order). 8/16/32 return a Number; 64 returns a BigInt.
 * ================================================================ */
#define BITS_REVERSE_SMALL(W, SHIFT) \
    static JSValue bits_reverse##W(JSContext *ctx, JSValueConst this_val, \
                                   int argc, JSValueConst *argv) { \
        uint64_t x; (void)this_val; (void)argc; \
        if (bits_read##W(ctx, argv[0], &x)) return JS_EXCEPTION; \
        return JS_NewUint32(ctx, (uint32_t)(bits_rev64(x) >> (SHIFT))); }

BITS_REVERSE_SMALL(8, 56)
BITS_REVERSE_SMALL(16, 48)
BITS_REVERSE_SMALL(32, 32)

static JSValue bits_reverse64(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    uint64_t x;
    (void)this_val; (void)argc;
    if (bits_read64(ctx, argv[0], &x))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, bits_rev64(x));
}

/* ================================================================ *
 *  ReverseBytes (byte order / endianness swap).
 * ================================================================ */
static JSValue bits_reverse_bytes16(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    uint64_t x;
    (void)this_val; (void)argc;
    if (bits_read16(ctx, argv[0], &x))
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, __builtin_bswap16((uint16_t)x));
}

static JSValue bits_reverse_bytes32(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    uint64_t x;
    (void)this_val; (void)argc;
    if (bits_read32(ctx, argv[0], &x))
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, __builtin_bswap32((uint32_t)x));
}

static JSValue bits_reverse_bytes64(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    uint64_t x;
    (void)this_val; (void)argc;
    if (bits_read64(ctx, argv[0], &x))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, __builtin_bswap64(x));
}

/* ================================================================ *
 *  RotateLeft (k<0 rotates right). k is a Number for every width; only
 *  the 64-bit value x is a BigInt. s==0 short-circuits the C `x>>width` UB.
 * ================================================================ */
#define BITS_ROTATE_SMALL(W, MASK) \
    static JSValue bits_rotate_left##W(JSContext *ctx, JSValueConst this_val, \
                                       int argc, JSValueConst *argv) { \
        uint64_t x; int32_t k; unsigned s; (void)this_val; (void)argc; \
        if (bits_read##W(ctx, argv[0], &x)) return JS_EXCEPTION; \
        if (JS_ToInt32(ctx, &k, argv[1])) return JS_EXCEPTION; \
        s = (unsigned)k & (MASK); \
        if (s) \
            x = ((x << s) | (x >> ((W) - s))) & ((((uint64_t)1) << (W)) - 1); \
        return JS_NewUint32(ctx, (uint32_t)x); }

BITS_ROTATE_SMALL(8, 7u)
BITS_ROTATE_SMALL(16, 15u)
BITS_ROTATE_SMALL(32, 31u)

static JSValue bits_rotate_left64(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint64_t x;
    int32_t k;
    unsigned s;
    (void)this_val; (void)argc;
    if (bits_read64(ctx, argv[0], &x))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &k, argv[1]))
        return JS_EXCEPTION;
    s = (unsigned)k & 63u;
    if (s)
        x = (x << s) | (x >> (64 - s));
    return JS_NewBigUint64(ctx, x);
}

/* ================================================================ *
 *  Multi-precision arithmetic (Add/Sub/Mul/Div/Rem). 32-bit variants
 *  return Number pairs; 64-bit variants return BigInt pairs.
 * ================================================================ */

/* Build [a, b]; each JS_DefinePropertyValueUint32 consumes its value (even on
 * failure), and the values are created inline so a failed array creation
 * leaves nothing to free. Mirrors dyna-mathx.c's 2-tuple builders. */
static JSValue bits_pair_u32(JSContext *ctx, uint32_t a, uint32_t b)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewUint32(ctx, a),
                                     JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewUint32(ctx, b),
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static JSValue bits_pair_u64(JSContext *ctx, uint64_t a, uint64_t b)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewBigUint64(ctx, a),
                                     JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewBigUint64(ctx, b),
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

/* Add32(a,b,carry) -> [sum, carryOut]. */
static JSValue bits_add32(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint32_t a, b, c;
    uint64_t s;
    (void)this_val; (void)argc;
    if (JS_ToUint32(ctx, &a, argv[0]) || JS_ToUint32(ctx, &b, argv[1]) ||
        JS_ToUint32(ctx, &c, argv[2]))
        return JS_EXCEPTION;
    s = (uint64_t)a + b + c;
    return bits_pair_u32(ctx, (uint32_t)s, (uint32_t)(s >> 32));
}

/* Add64(a,b,carry) -> [sum, carryOut] (BigInts). */
static JSValue bits_add64(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint64_t a, b, c;
    unsigned __int128 s;
    (void)this_val; (void)argc;
    if (bits_read64(ctx, argv[0], &a) || bits_read64(ctx, argv[1], &b) ||
        bits_read64(ctx, argv[2], &c))
        return JS_EXCEPTION;
    s = (unsigned __int128)a + b + c;
    return bits_pair_u64(ctx, (uint64_t)s, (uint64_t)(s >> 64));
}

/* Sub32(a,b,borrow) -> [diff, borrowOut]. */
static JSValue bits_sub32(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint32_t a, b, c;
    uint64_t sub;
    (void)this_val; (void)argc;
    if (JS_ToUint32(ctx, &a, argv[0]) || JS_ToUint32(ctx, &b, argv[1]) ||
        JS_ToUint32(ctx, &c, argv[2]))
        return JS_EXCEPTION;
    sub = (uint64_t)b + c;
    return bits_pair_u32(ctx, (uint32_t)((uint64_t)a - sub),
                         (uint64_t)a < sub ? 1u : 0u);
}

/* Sub64(a,b,borrow) -> [diff, borrowOut] (BigInts). */
static JSValue bits_sub64(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint64_t a, b, c;
    unsigned __int128 sub;
    (void)this_val; (void)argc;
    if (bits_read64(ctx, argv[0], &a) || bits_read64(ctx, argv[1], &b) ||
        bits_read64(ctx, argv[2], &c))
        return JS_EXCEPTION;
    sub = (unsigned __int128)b + c;
    return bits_pair_u64(ctx, (uint64_t)((unsigned __int128)a - sub),
                         (unsigned __int128)a < sub ? 1u : 0u);
}

/* Mul32(a,b) -> [hi, lo]. */
static JSValue bits_mul32(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint32_t a, b;
    uint64_t p;
    (void)this_val; (void)argc;
    if (JS_ToUint32(ctx, &a, argv[0]) || JS_ToUint32(ctx, &b, argv[1]))
        return JS_EXCEPTION;
    p = (uint64_t)a * b;
    return bits_pair_u32(ctx, (uint32_t)(p >> 32), (uint32_t)p);
}

/* Mul64(a,b) -> [hi, lo] (BigInts). */
static JSValue bits_mul64(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint64_t a, b;
    unsigned __int128 p;
    (void)this_val; (void)argc;
    if (bits_read64(ctx, argv[0], &a) || bits_read64(ctx, argv[1], &b))
        return JS_EXCEPTION;
    p = (unsigned __int128)a * b;
    return bits_pair_u64(ctx, (uint64_t)(p >> 64), (uint64_t)p);
}

/* Div32(hi,lo,y) -> [quo, rem]; throws on y==0 or y<=hi (quotient overflow). */
static JSValue bits_div32(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint32_t hi, lo, y;
    uint64_t d;
    (void)this_val; (void)argc;
    if (JS_ToUint32(ctx, &hi, argv[0]) || JS_ToUint32(ctx, &lo, argv[1]) ||
        JS_ToUint32(ctx, &y, argv[2]))
        return JS_EXCEPTION;
    if (y == 0)
        return JS_ThrowRangeError(ctx, "dyna:bits: Div32 division by zero");
    if (y <= hi)
        return JS_ThrowRangeError(ctx, "dyna:bits: Div32 quotient overflow");
    d = ((uint64_t)hi << 32) | lo;
    return bits_pair_u32(ctx, (uint32_t)(d / y), (uint32_t)(d % y));
}

/* Div64(hi,lo,y) -> [quo, rem] (BigInts); throws on y==0 or y<=hi. */
static JSValue bits_div64(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint64_t hi, lo, y;
    unsigned __int128 d;
    (void)this_val; (void)argc;
    if (bits_read64(ctx, argv[0], &hi) || bits_read64(ctx, argv[1], &lo) ||
        bits_read64(ctx, argv[2], &y))
        return JS_EXCEPTION;
    if (y == 0)
        return JS_ThrowRangeError(ctx, "dyna:bits: Div64 division by zero");
    if (y <= hi)
        return JS_ThrowRangeError(ctx, "dyna:bits: Div64 quotient overflow");
    d = ((unsigned __int128)hi << 64) | lo;
    return bits_pair_u64(ctx, (uint64_t)(d / y), (uint64_t)(d % y));
}

/* Rem32(hi,lo,y) -> rem; throws only on y==0 (no quotient-overflow panic). */
static JSValue bits_rem32(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint32_t hi, lo, y;
    uint64_t d;
    (void)this_val; (void)argc;
    if (JS_ToUint32(ctx, &hi, argv[0]) || JS_ToUint32(ctx, &lo, argv[1]) ||
        JS_ToUint32(ctx, &y, argv[2]))
        return JS_EXCEPTION;
    if (y == 0)
        return JS_ThrowRangeError(ctx, "dyna:bits: Rem32 division by zero");
    d = ((uint64_t)hi << 32) | lo;
    return JS_NewUint32(ctx, (uint32_t)(d % y));
}

/* Rem64(hi,lo,y) -> rem (BigInt); throws only on y==0. */
static JSValue bits_rem64(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint64_t hi, lo, y;
    unsigned __int128 d;
    (void)this_val; (void)argc;
    if (bits_read64(ctx, argv[0], &hi) || bits_read64(ctx, argv[1], &lo) ||
        bits_read64(ctx, argv[2], &y))
        return JS_EXCEPTION;
    if (y == 0)
        return JS_ThrowRangeError(ctx, "dyna:bits: Rem64 division by zero");
    d = ((unsigned __int128)hi << 64) | lo;
    return JS_NewBigUint64(ctx, (uint64_t)(d % y));
}

/* ---------- module registration ---------- */

/* length = the highest argv[] index read unconditionally + 1: 1 for the unary
 * value functions, 2 for RotateLeft/Mul, 3 for Add/Sub/Div/Rem. */
static const JSCFunctionListEntry bits_funcs[] = {
    JS_PROP_INT32_DEF("UintSize", 64, 0),

    JS_CFUNC_DEF("LeadingZeros8", 1, bits_leading_zeros8),
    JS_CFUNC_DEF("LeadingZeros16", 1, bits_leading_zeros16),
    JS_CFUNC_DEF("LeadingZeros32", 1, bits_leading_zeros32),
    JS_CFUNC_DEF("LeadingZeros64", 1, bits_leading_zeros64),

    JS_CFUNC_DEF("TrailingZeros8", 1, bits_trailing_zeros8),
    JS_CFUNC_DEF("TrailingZeros16", 1, bits_trailing_zeros16),
    JS_CFUNC_DEF("TrailingZeros32", 1, bits_trailing_zeros32),
    JS_CFUNC_DEF("TrailingZeros64", 1, bits_trailing_zeros64),

    JS_CFUNC_DEF("OnesCount8", 1, bits_ones_count8),
    JS_CFUNC_DEF("OnesCount16", 1, bits_ones_count16),
    JS_CFUNC_DEF("OnesCount32", 1, bits_ones_count32),
    JS_CFUNC_DEF("OnesCount64", 1, bits_ones_count64),

    JS_CFUNC_DEF("Len8", 1, bits_len8),
    JS_CFUNC_DEF("Len16", 1, bits_len16),
    JS_CFUNC_DEF("Len32", 1, bits_len32),
    JS_CFUNC_DEF("Len64", 1, bits_len64),

    JS_CFUNC_DEF("Reverse8", 1, bits_reverse8),
    JS_CFUNC_DEF("Reverse16", 1, bits_reverse16),
    JS_CFUNC_DEF("Reverse32", 1, bits_reverse32),
    JS_CFUNC_DEF("Reverse64", 1, bits_reverse64),

    JS_CFUNC_DEF("ReverseBytes16", 1, bits_reverse_bytes16),
    JS_CFUNC_DEF("ReverseBytes32", 1, bits_reverse_bytes32),
    JS_CFUNC_DEF("ReverseBytes64", 1, bits_reverse_bytes64),

    JS_CFUNC_DEF("RotateLeft8", 2, bits_rotate_left8),
    JS_CFUNC_DEF("RotateLeft16", 2, bits_rotate_left16),
    JS_CFUNC_DEF("RotateLeft32", 2, bits_rotate_left32),
    JS_CFUNC_DEF("RotateLeft64", 2, bits_rotate_left64),

    JS_CFUNC_DEF("Add32", 3, bits_add32),
    JS_CFUNC_DEF("Add64", 3, bits_add64),
    JS_CFUNC_DEF("Sub32", 3, bits_sub32),
    JS_CFUNC_DEF("Sub64", 3, bits_sub64),
    JS_CFUNC_DEF("Mul32", 2, bits_mul32),
    JS_CFUNC_DEF("Mul64", 2, bits_mul64),
    JS_CFUNC_DEF("Div32", 3, bits_div32),
    JS_CFUNC_DEF("Div64", 3, bits_div64),
    JS_CFUNC_DEF("Rem32", 3, bits_rem32),
    JS_CFUNC_DEF("Rem64", 3, bits_rem64),
};

static int bits_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, bits_funcs, countof(bits_funcs));
}

int js_nat_init_bits(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:bits", bits_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, bits_funcs, countof(bits_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_BITS */
