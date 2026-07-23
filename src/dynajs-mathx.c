/*
 * dynajs:mathx -- extended math utilities, self-contained, in-repo (no
 * external deps): the functions Go's `math` package has that JS's `Math`
 * lacks, plus a small set of integer/BigInt bit helpers. Pure functions
 * (no `this`, no native resource, nothing to close).
 *
 *   import { gamma, erf, cbrt, hypot, gcd, lcm, factorial, isPrime,
 *            Pi, E, MaxSafeInteger } from "dynajs:mathx";
 *   gamma(5);            // -> 24            (Gamma(n) = (n-1)! for integer n)
 *   erf(0);              // -> 0
 *   hypot(3, 4);          // -> 5
 *   gcd(12, 18);          // -> 6n
 *   factorial(20);        // -> 2432902008176640000n
 *   isPrime(2305843009213693951n); // -> true  (the Mersenne prime 2^61-1)
 *
 * Semantics (documented guarantees callers may rely on):
 *
 *   - DOUBLE FUNCTIONS (gamma/lgamma/erf/erfc/cbrt/hypot/copysign/nextafter/
 *     expm1/log1p/log2/logb/scalbn/ilogb/modf/frexp/ldexp/remainder/fmod/
 *     isInf/isNaN/signbit/trunc/round/roundToEven) coerce every argument via
 *     JS_ToFloat64 (so a BigInt argument throws a TypeError, matching plain
 *     ToNumber semantics -- this group never takes BigInt) and are thin
 *     wrappers over the platform libm, which on every target this repo
 *     builds for (Darwin libc, glibc, musl) already implements Go's exact
 *     documented special-case behavior for gamma/lgamma/erf/erfc/cbrt/
 *     hypot/copysign/nextafter/expm1/log1p/log2/logb/remainder/fmod/round
 *     (round is C99 round(): away-from-zero on a tie, UNLIKE Math.round,
 *     which rounds a negative tie toward +Infinity -- round(-2.5) === -3
 *     here, Math.round(-2.5) === -2). Two of the group do NOT match Go by
 *     direct passthrough and are explicitly special-cased instead:
 *       * modf(x) -- for infinite x, C99 modf() defines the fractional part
 *         as +-0, but Go's Modf(+-Inf) is documented as (+-Inf, NaN) (the
 *         fractional part of an infinity is undefined, not zero) -- modf
 *         special-cases +-Inf and NaN itself rather than trusting libm.
 *       * ilogb(x) -- C99 leaves ilogb(0) and ilogb(NaN) as two DIFFERENT
 *         implementation-defined sentinels (FP_ILOGB0 / FP_ILOGBNAN), and at
 *         least one target libm in this repo's matrix returns the SAME
 *         sentinel (INT32_MIN) for both -- but Go's Ilogb(0) = MinInt32 and
 *         Ilogb(NaN) = MaxInt32 are DIFFERENT values, so ilogb classifies
 *         0/Inf/NaN itself before ever calling libm ilogb().
 *     frexp(x) additionally special-cases 0/Inf/NaN explicitly (even though
 *     this repo's libms already agree with Go there) purely so the result
 *     does not silently start depending on libc agreement if that ever
 *     changes -- the same stance src/dynajs-time.c documents for calendar
 *     math (control the edge case ourselves, don't trust three different
 *     libcs to keep agreeing).
 *   - modf(x) -> [intPart, fracPart]; frexp(x) -> [frac, exp] -- note the
 *     ORDER differs between the two (modf's integer part leads; frexp's
 *     fraction leads), matching Go's Modf/Frexp return-tuple order exactly.
 *   - lgamma(x) -> [value, sign] via the reentrant lgamma_r (never the
 *     non-reentrant lgamma(), which reports its sign through the global
 *     mutable `signgam` -- a data race across JS worker threads sharing no
 *     JS state but the same C process).
 *   - isInf(x, sign=0) -- sign>0 tests +Infinity only, sign<0 tests
 *     -Infinity only, sign===0 (or omitted) tests either, mirroring Go's
 *     math.IsInf(f, sign).
 *   - roundToEven(x) is Go's bit-exact round-half-to-even, implemented via
 *     trunc/fmod/copysign rather than rint()/nearbyint() -- the latter pair
 *     honor the CURRENT floating-point rounding mode (FLT_ROUNDS), which a
 *     future caller of this same process could change via fesetround();
 *     Go's RoundToEven has no such dependency, so neither does ours.
 *   - scalbn(x, n) and ldexp(frac, exp) are exposed as two distinct
 *     functions (mirroring their two distinct C99 libm names) even though
 *     they compute the identical x * 2**n on every FLT_RADIX==2 target this
 *     repo builds for.
 *
 *   - INTEGER/BIGINT FUNCTIONS (gcd/lcm/factorial/isPrime accept EITHER a
 *     Number or a BigInt; abs/bitLen/popcount require an ACTUAL BigInt and
 *     throw a TypeError otherwise -- matching the task's own annotation of
 *     exactly those three as "(BigInt)"). A Number argument must be a
 *     finite integer that fits exactly in an int64_t, or a RangeError is
 *     thrown -- UNLIKE JS_ToInt64Ext's silent wrap-on-truncate (a fine
 *     default for a duration/calendar field elsewhere in this repo), a
 *     silently truncated/wrapped operand here would be a much more likely,
 *     much less visible bug for what is supposed to be EXACT integer
 *     arithmetic. A BigInt argument is read via JS_ToBigInt64, which
 *     truncates/wraps modulo 2^64 for a magnitude that does not fit in 64
 *     bits -- the SAME already-documented convention this repo uses
 *     everywhere else a native module accepts a BigInt (dynajs-time.c,
 *     dynajs-random.c, dynajs-encoding.c): there is no arbitrary-precision
 *     BigInt constructor in dynajs.h, so a >64-bit input has nowhere exact
 *     to go on the way in.
 *   - gcd/lcm/factorial/abs always return a BigInt (never a Number), for one
 *     predictable contract across the whole group.
 *   - A RESULT, unlike an input, can need more than 64 bits (21! already
 *     does; so can lcm(a, b) of two ordinary 64-bit-range, nearly-coprime
 *     a/b). Since dynajs.h exposes no arbitrary-precision BigInt
 *     constructor either (JS_NewBigInt64/JS_NewBigUint64 are both fixed
 *     64-bit), the only way to hand back a wider BigInt through the public
 *     API is to render the decimal digits (computed by this module's own
 *     bounded arithmetic: a base-10^9 accumulator for factorial, one exact
 *     `unsigned __int128` multiply for lcm) as a BigInt-literal source
 *     string and JS_Eval it -- deferring the actual arbitrary-precision
 *     VALUE representation to the engine's own already-tested BigInt
 *     literal parser. That source string is built ENTIRELY from digits
 *     this module computed -- never from caller-controlled text -- so this
 *     is not an eval-injection surface. The common case (factorial(n) for
 *     n <= 20, any lcm whose true result fits in 64 bits) never reaches
 *     this path at all: it is produced directly via JS_NewBigUint64.
 *   - factorial(n) is capped at DYN_MATHX_FACTORIAL_MAX purely to bound the
 *     compute/memory of one call (10000! already has ~35660 decimal
 *     digits); the cap is far beyond any n a caller would want the fully
 *     expanded decimal integer for.
 *   - isPrime(n) treats n < 2 (including every negative n) as not prime and
 *     otherwise runs a DETERMINISTIC 64-bit Miller-Rabin test (the 12-witness
 *     set {2,3,5,7,11,13,17,19,23,29,31,37}, proven sufficient for every
 *     n < 3.3e24, far above UINT64_MAX) -- not trial division, which would
 *     need up to ~2^32 iterations for a worst-case 64-bit prime.
 *   - gcd(0, 0) = 0n; lcm(a, 0) = lcm(0, b) = 0n (the standard conventions).
 *   - abs/bitLen/popcount operate on the BigInt's ABSOLUTE VALUE (bitLen(0n)
 *     = 0; popcount treats a negative BigInt's magnitude, since two's-
 *     complement popcount of a negative value is infinite and meaningless
 *     here).
 *
 * Memory discipline: every function coerces ALL of its arguments to C
 * locals FIRST (JS_ToFloat64/JS_ToInt32/JS_ToBigInt64), before doing any
 * work -- there is no native resource a reentrant coercion could invalidate
 * (these are plain functions, no `this`), but validating every argument
 * before using any of them avoids paying for partial work on a call that is
 * going to throw anyway. A 2-element result ([value,sign]/[intPart,
 * fracPart]/[frac,exp]) is built via JS_NewArray + JS_DefinePropertyValue-
 * Uint32, freeing the partial array on any failure. The factorial/lcm
 * bignum-decimal scratch buffer is heap-allocated via js_malloc/js_realloc
 * only when it does not fit a small stack buffer, and is always freed (or
 * never allocated) on every path, success or failure.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

#include <stdint.h>
#include <stdio.h>
/* Darwin's <math.h> only declares the reentrant lgamma_r/lgammaf_r/
 * lgammal_r variants when _REENTRANT (or __swift__) is defined before the
 * header is processed; glibc/musl expose it unconditionally under the
 * already-global -D_GNU_SOURCE. Defining it here (before this TU's own
 * first #include <math.h>) is a no-op on the platforms that don't gate it. */
#define _REENTRANT
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ================================================================ *
 *  Double in/out: unary and binary libm passthroughs.
 * ================================================================ */

/* Go's RoundToEven, independent of the process's current FP rounding mode
 * (see header comment). trunc()/fmod() of values within +-1 of each other
 * are exact per Sterbenz's lemma, so no precision is lost picking apart
 * the fractional part this way. */
static double dyn_mathx_rte(double x)
{
    double t, diff;
    if (!isfinite(x))
        return x;
    t = trunc(x);
    diff = fabs(x - t);
    if (diff < 0.5)
        return t;
    if (diff > 0.5)
        return t + copysign(1.0, x);
    return (fmod(t, 2.0) == 0.0) ? t : t + copysign(1.0, x);
}

/* length=1 for every unary function below: each reads only argv[0], and
 * ALWAYS unconditionally, so the JS_CFUNC_DEF length MUST be >= 1 (see the
 * rule spelled out in dynajs-bytes.c's registration-table comment: length
 * is the highest argv[] index read unconditionally, not the "conceptual"
 * arg count). */
#define DYN_MATHX_UNARY(name, cexpr) \
    static JSValue dyn_mathx_##name(JSContext *ctx, JSValueConst this_val, \
                                    int argc, JSValueConst *argv) \
    { \
        double x; \
        (void)this_val; (void)argc; \
        if (JS_ToFloat64(ctx, &x, argv[0])) \
            return JS_EXCEPTION; \
        return JS_NewFloat64(ctx, (cexpr)); \
    }

DYN_MATHX_UNARY(gamma, tgamma(x))
DYN_MATHX_UNARY(erf, erf(x))
DYN_MATHX_UNARY(erfc, erfc(x))
DYN_MATHX_UNARY(cbrt, cbrt(x))
DYN_MATHX_UNARY(expm1, expm1(x))
DYN_MATHX_UNARY(log1p, log1p(x))
DYN_MATHX_UNARY(log2, log2(x))
DYN_MATHX_UNARY(logb, logb(x))
DYN_MATHX_UNARY(trunc, trunc(x))
DYN_MATHX_UNARY(round, round(x))
DYN_MATHX_UNARY(roundToEven, dyn_mathx_rte(x))

/* length=2 for every binary function below: both argv[0] and argv[1] are
 * read unconditionally. */
#define DYN_MATHX_BINARY(name, cexpr) \
    static JSValue dyn_mathx_##name(JSContext *ctx, JSValueConst this_val, \
                                    int argc, JSValueConst *argv) \
    { \
        double x, y; \
        (void)this_val; (void)argc; \
        if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1])) \
            return JS_EXCEPTION; \
        return JS_NewFloat64(ctx, (cexpr)); \
    }

DYN_MATHX_BINARY(hypot, hypot(x, y))
DYN_MATHX_BINARY(copysign, copysign(x, y))
DYN_MATHX_BINARY(nextafter, nextafter(x, y))
DYN_MATHX_BINARY(remainder, remainder(x, y))
DYN_MATHX_BINARY(fmod, fmod(x, y))

/* lgamma(x) -> [value, sign]. lgamma_r, not the non-reentrant lgamma()
 * (see header comment). */
static JSValue dyn_mathx_lgamma(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    double x, val;
    int sign = 1;
    JSValue arr;

    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    val = lgamma_r(x, &sign);

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewFloat64(ctx, val),
                                     JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewInt32(ctx, sign),
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

/* scalbn(x, n) -> x * 2**n. */
static JSValue dyn_mathx_scalbn(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    double x;
    int32_t n;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToInt32(ctx, &n, argv[1]))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, scalbn(x, n));
}

/* ldexp(frac, exp) -> frac * 2**exp (identical to scalbn -- see header). */
static JSValue dyn_mathx_ldexp(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double frac;
    int32_t exp;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &frac, argv[0]) || JS_ToInt32(ctx, &exp, argv[1]))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, ldexp(frac, exp));
}

/* ilogb(x) -> int, classifying 0/Inf/NaN itself -- see header comment for
 * why the raw libm sentinels are not trustworthy here. */
static JSValue dyn_mathx_ilogb(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double x;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (isnan(x) || isinf(x))
        return JS_NewInt32(ctx, INT32_MAX); /* Go: Ilogb(+-Inf)=Ilogb(NaN)=MaxInt32 */
    if (x == 0.0)
        return JS_NewInt32(ctx, INT32_MIN); /* Go: Ilogb(0) = MinInt32 */
    return JS_NewInt32(ctx, (int32_t)ilogb(x));
}

/* modf(x) -> [intPart, fracPart], special-casing Inf/NaN to match Go's
 * Modf (see header comment: C99 modf() disagrees for infinite x). */
static JSValue dyn_mathx_modf(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    double x, ip, fp;
    JSValue arr;

    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (isnan(x)) {
        ip = x;
        fp = x;
    } else if (isinf(x)) {
        ip = x;
        fp = copysign(NAN, x);
    } else {
        fp = modf(x, &ip);
    }

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewFloat64(ctx, ip),
                                     JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewFloat64(ctx, fp),
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

/* frexp(x) -> [frac, exp]. Explicitly classifies 0/Inf/NaN rather than
 * trusting libm's frexp to already agree with Go there on every target
 * libc (see header comment); every target this repo builds for already
 * agrees, so this is a portability hedge, not a fix for an observed bug. */
static JSValue dyn_mathx_frexp(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double x, frac;
    int exp;
    JSValue arr;

    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (isnan(x) || isinf(x) || x == 0.0) {
        frac = x;
        exp = 0;
    } else {
        frac = frexp(x, &exp);
    }

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewFloat64(ctx, frac),
                                     JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewInt32(ctx, exp),
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

/* isInf(x, sign=0) -> bool (see header comment for sign semantics). length
 * is 1: argv[1] is read only when argc>1 (the established convention for
 * an optional trailing argument -- see e.g. dynajs-time.c's
 * formatRFC3339). */
static JSValue dyn_mathx_is_inf(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    double x;
    int32_t sign = 0;
    int r;

    (void)this_val;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt32(ctx, &sign, argv[1]))
            return JS_EXCEPTION;
    }
    if (sign > 0)
        r = isinf(x) && x > 0;
    else if (sign < 0)
        r = isinf(x) && x < 0;
    else
        r = isinf(x) != 0;
    return JS_NewBool(ctx, r);
}

static JSValue dyn_mathx_is_nan(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    double x;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isnan(x) != 0);
}

static JSValue dyn_mathx_signbit(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    double x;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, signbit(x) != 0);
}

/* ================================================================ *
 *  Integer/BigInt helpers (see header comment for the full contract).
 * ================================================================ */

/* Coerce a Number or BigInt argument to an exact int64_t. See header
 * comment for why this is stricter than JS_ToInt64Ext's silent wrap. */
static int dyn_mathx_to_int64(JSContext *ctx, JSValueConst v, int64_t *out)
{
    double d;

    if (JS_IsBigInt(ctx, v))
        return JS_ToBigInt64(ctx, out, v);

    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!isfinite(d) || d != trunc(d)) {
        JS_ThrowRangeError(ctx, "dynajs:mathx: expected an integer or a BigInt");
        return -1;
    }
    /* 2^63 is exactly representable as a double; still out of range (the
     * valid int64_t domain tops out at 2^63 - 1), and casting an
     * out-of-range double to int64_t is undefined behavior in C, so this
     * must be rejected before the cast below, not after. */
    if (d < -9223372036854775808.0 || d >= 9223372036854775808.0) {
        JS_ThrowRangeError(ctx, "dynajs:mathx: integer out of 64-bit range");
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

/* |v| as a uint64_t, including INT64_MIN (which has no positive int64_t
 * representation -- its magnitude, 2^63, only fits unsigned). */
static uint64_t dyn_mathx_u64_abs(int64_t v)
{
    if (v == INT64_MIN)
        return (uint64_t)INT64_MAX + 1ULL;
    return v < 0 ? (uint64_t)(-v) : (uint64_t)v;
}

static uint64_t dyn_mathx_gcd_u64(uint64_t a, uint64_t b)
{
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* ---- base-10^9 big-decimal accumulator (factorial's overflow tail) ---- */

#define DYN_MATHX_LIMB_BASE 1000000000u /* 10^9: each limb is exactly 9 decimal digits */

typedef struct {
    JSContext *ctx;
    uint32_t *limb; /* base-1e9, least-significant limb first */
    size_t len, cap;
} DynBigDec;

static int dyn_bigdec_init(JSContext *ctx, DynBigDec *b, uint64_t v)
{
    b->ctx = ctx;
    b->cap = 4;
    b->len = 0;
    b->limb = js_malloc(ctx, b->cap * sizeof(uint32_t));
    if (!b->limb)
        return -1; /* js_malloc already threw */
    do {
        b->limb[b->len++] = (uint32_t)(v % DYN_MATHX_LIMB_BASE);
        v /= DYN_MATHX_LIMB_BASE;
    } while (v > 0);
    return 0;
}

static void dyn_bigdec_free(DynBigDec *b)
{
    js_free(b->ctx, b->limb);
    b->limb = NULL;
}

static int dyn_bigdec_reserve(DynBigDec *b, size_t need)
{
    uint32_t *nl;
    size_t ncap;
    if (need <= b->cap)
        return 0;
    ncap = b->cap * 2;
    if (ncap < need)
        ncap = need;
    nl = js_realloc(b->ctx, b->limb, ncap * sizeof(uint32_t));
    if (!nl)
        return -1; /* js_realloc already threw */
    b->limb = nl;
    b->cap = ncap;
    return 0;
}

/* Multiply the accumulator in place by `factor`. Only ever called with a
 * factor bounded by DYN_MATHX_FACTORIAL_MAX (the factorial loop counter),
 * so limb*factor+carry (well under 1e9 * DYN_MATHX_FACTORIAL_MAX) never
 * comes close to overflowing the uint64_t carry. */
static int dyn_bigdec_mul_small(DynBigDec *b, uint32_t factor)
{
    uint64_t carry = 0;
    size_t i;
    for (i = 0; i < b->len; i++) {
        uint64_t cur = (uint64_t)b->limb[i] * factor + carry;
        b->limb[i] = (uint32_t)(cur % DYN_MATHX_LIMB_BASE);
        carry = cur / DYN_MATHX_LIMB_BASE;
    }
    while (carry > 0) {
        if (dyn_bigdec_reserve(b, b->len + 1))
            return -1;
        b->limb[b->len++] = (uint32_t)(carry % DYN_MATHX_LIMB_BASE);
        carry /= DYN_MATHX_LIMB_BASE;
    }
    return 0;
}

/* Render `limb` (base 1e9, least-significant first, n>=1) as a JS BigInt
 * literal and JS_Eval it -- see header comment: the only way to construct
 * a BigInt wider than 64 bits through the public engine API. The
 * evaluated source is built ENTIRELY from digits this function itself
 * computed, never from caller-controlled input. */
static JSValue dyn_mathx_limbs_to_bigint(JSContext *ctx, const uint32_t *limb,
                                         size_t n)
{
    char stackbuf[16 * 9 + 3];
    char *buf = stackbuf;
    size_t cap = sizeof(stackbuf);
    size_t pos, i;
    JSValue result;

    if (n > 16) {
        cap = n * 9 + 3;
        buf = js_malloc(ctx, cap);
        if (!buf)
            return JS_EXCEPTION;
    }

    pos = (size_t)snprintf(buf, cap, "%u", limb[n - 1]);
    for (i = n - 1; i > 0; i--)
        pos += (size_t)snprintf(buf + pos, cap - pos, "%09u", limb[i - 1]);
    buf[pos++] = 'n';
    buf[pos] = '\0'; /* JS_Eval requires input[input_len] == '\0' */

    result = JS_Eval(ctx, buf, pos, "<dynajs:mathx>", JS_EVAL_TYPE_GLOBAL);
    if (buf != stackbuf)
        js_free(ctx, buf);
    return result;
}

/* ---- deterministic 64-bit Miller-Rabin (isPrime) ---- */

static uint64_t dyn_mathx_mulmod64(uint64_t a, uint64_t b, uint64_t m)
{
    return (uint64_t)(((unsigned __int128)a * b) % m);
}

static uint64_t dyn_mathx_powmod64(uint64_t base, uint64_t exp, uint64_t m)
{
    uint64_t result = 1;
    base %= m;
    while (exp) {
        if (exp & 1)
            result = dyn_mathx_mulmod64(result, base, m);
        base = dyn_mathx_mulmod64(base, base, m);
        exp >>= 1;
    }
    return result;
}

/* {2,3,5,7,11,13,17,19,23,29,31,37} is a proven-sufficient deterministic
 * Miller-Rabin witness set for every n < 3.317e24 -- far above UINT64_MAX
 * (~1.8e19) -- so this has zero probabilistic error over the entire
 * uint64_t domain (see header comment). */
static int dyn_mathx_is_prime_u64(uint64_t n)
{
    static const uint64_t small_primes[] = {
        2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37
    };
    uint64_t d;
    int r, i;

    if (n < 2)
        return 0;
    for (i = 0; i < (int)countof(small_primes); i++) {
        if (n == small_primes[i])
            return 1;
        if (n % small_primes[i] == 0)
            return 0;
    }

    d = n - 1;
    r = 0;
    while ((d & 1) == 0) {
        d >>= 1;
        r++;
    }

    for (i = 0; i < (int)countof(small_primes); i++) {
        uint64_t a = small_primes[i];
        uint64_t x = dyn_mathx_powmod64(a, d, n);
        int j, is_composite;

        if (x == 1 || x == n - 1)
            continue;
        is_composite = 1;
        for (j = 0; j < r - 1; j++) {
            x = dyn_mathx_mulmod64(x, x, n);
            if (x == n - 1) {
                is_composite = 0;
                break;
            }
        }
        if (is_composite)
            return 0;
    }
    return 1;
}

/* ---- exported Integer/BigInt functions ---- */

static JSValue dyn_mathx_gcd(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    int64_t a, b;
    (void)this_val; (void)argc;
    if (dyn_mathx_to_int64(ctx, argv[0], &a) ||
        dyn_mathx_to_int64(ctx, argv[1], &b))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, dyn_mathx_gcd_u64(dyn_mathx_u64_abs(a),
                                                  dyn_mathx_u64_abs(b)));
}

static JSValue dyn_mathx_lcm(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    int64_t a, b;
    uint64_t ua, ub, g, reduced;
    unsigned __int128 prod;

    (void)this_val; (void)argc;
    if (dyn_mathx_to_int64(ctx, argv[0], &a) ||
        dyn_mathx_to_int64(ctx, argv[1], &b))
        return JS_EXCEPTION;

    ua = dyn_mathx_u64_abs(a);
    ub = dyn_mathx_u64_abs(b);
    if (ua == 0 || ub == 0)
        return JS_NewBigInt64(ctx, 0);

    g = dyn_mathx_gcd_u64(ua, ub);
    reduced = ua / g;
    prod = (unsigned __int128)reduced * ub;

    if (prod <= UINT64_MAX)
        return JS_NewBigUint64(ctx, (uint64_t)prod);
    {
        uint32_t limb[5]; /* 2^128 < 1e39 < (1e9)^5 */
        size_t n = 0;
        unsigned __int128 v = prod;
        while (v > 0) {
            limb[n++] = (uint32_t)(v % DYN_MATHX_LIMB_BASE);
            v /= DYN_MATHX_LIMB_BASE;
        }
        return dyn_mathx_limbs_to_bigint(ctx, limb, n);
    }
}

#define DYN_MATHX_FACTORIAL_MAX 10000

static JSValue dyn_mathx_factorial(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    int64_t nv;
    int32_t n, i;
    uint64_t acc = 1;
    int overflowed = 0;
    DynBigDec big;
    JSValue result;

    (void)this_val; (void)argc;
    if (dyn_mathx_to_int64(ctx, argv[0], &nv))
        return JS_EXCEPTION;
    if (nv < 0)
        return JS_ThrowRangeError(ctx,
            "dynajs:mathx: factorial requires a non-negative integer");
    if (nv > DYN_MATHX_FACTORIAL_MAX)
        return JS_ThrowRangeError(ctx,
            "dynajs:mathx: factorial argument too large (max %d)",
            DYN_MATHX_FACTORIAL_MAX);
    n = (int32_t)nv;

    for (i = 2; i <= n; i++) {
        if (!overflowed) {
            if (acc > UINT64_MAX / (uint64_t)i) {
                if (dyn_bigdec_init(ctx, &big, acc))
                    return JS_EXCEPTION;
                overflowed = 1;
                /* fall through: still need to multiply by the CURRENT i,
                 * which triggered the promotion, into the new accumulator */
            } else {
                acc *= (uint64_t)i;
                continue;
            }
        }
        if (dyn_bigdec_mul_small(&big, (uint32_t)i)) {
            dyn_bigdec_free(&big);
            return JS_EXCEPTION;
        }
    }

    if (!overflowed)
        return JS_NewBigUint64(ctx, acc);

    result = dyn_mathx_limbs_to_bigint(ctx, big.limb, big.len);
    dyn_bigdec_free(&big);
    return result;
}

static JSValue dyn_mathx_is_prime(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    int64_t v;
    (void)this_val; (void)argc;
    if (dyn_mathx_to_int64(ctx, argv[0], &v))
        return JS_EXCEPTION;
    if (v < 2)
        return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, dyn_mathx_is_prime_u64((uint64_t)v));
}

static JSValue dyn_mathx_abs(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    int64_t v;
    (void)this_val; (void)argc;
    if (!JS_IsBigInt(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dynajs:mathx: abs expects a BigInt");
    if (JS_ToBigInt64(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, dyn_mathx_u64_abs(v));
}

static JSValue dyn_mathx_bit_len(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    int64_t v;
    uint64_t mag;
    (void)this_val; (void)argc;
    if (!JS_IsBigInt(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dynajs:mathx: bitLen expects a BigInt");
    if (JS_ToBigInt64(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    mag = dyn_mathx_u64_abs(v);
    if (mag == 0)
        return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, 64 - __builtin_clzll(mag));
}

static JSValue dyn_mathx_popcount(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    int64_t v;
    (void)this_val; (void)argc;
    if (!JS_IsBigInt(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dynajs:mathx: popcount expects a BigInt");
    if (JS_ToBigInt64(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, __builtin_popcountll(dyn_mathx_u64_abs(v)));
}

/* ---------- module registration ---------- */

/* Constants mirror Go's math package's own high-precision source values
 * (src/math/const.go), each written out to ~30 significant digits so the
 * compiler performs exactly one correctly-rounded conversion to the
 * nearest double -- the same single-rounding path Go's own compiler takes
 * when it rounds its arbitrary-precision untyped constants to float64.
 * Flags are 0 (read-only, non-configurable, non-enumerable), matching
 * dynajs-time.c's duration constants. */
static const JSCFunctionListEntry dyn_mathx_funcs[] = {
    JS_PROP_DOUBLE_DEF("E", 2.71828182845904523536028747135266249775724709369995957496696763, 0),
    JS_PROP_DOUBLE_DEF("Pi", 3.14159265358979323846264338327950288419716939937510582097494459, 0),
    JS_PROP_DOUBLE_DEF("Phi", 1.61803398874989484820458683436563811772030917980576286213544862, 0),
    JS_PROP_DOUBLE_DEF("Sqrt2", 1.41421356237309504880168872420969807856967187537694807317667974, 0),
    JS_PROP_DOUBLE_DEF("SqrtE", 1.64872127070012814684865078781416357165377610071014801157507931, 0),
    JS_PROP_DOUBLE_DEF("SqrtPi", 1.77245385090551602729816748334114518279754945612238712821380779, 0),
    JS_PROP_DOUBLE_DEF("Ln2", 0.693147180559945309417232121458176568075500134360255254120680009, 0),
    JS_PROP_DOUBLE_DEF("Log2E", 1.44269504088896340735992468100189213742664595415298593413544940693, 0),
    JS_PROP_DOUBLE_DEF("Ln10", 2.30258509299404568401799145468436420760110148862877297603332790, 0),
    JS_PROP_DOUBLE_DEF("Log10E", 0.434294481903251827651128918916605082294397005803663526424946929, 0),
    JS_PROP_INT32_DEF("MaxInt32", INT32_MAX, 0),
    JS_PROP_INT32_DEF("MinInt32", INT32_MIN, 0),
    JS_PROP_INT64_DEF("MaxSafeInteger", 9007199254740991LL, 0),

    JS_CFUNC_DEF("gamma", 1, dyn_mathx_gamma),
    JS_CFUNC_DEF("lgamma", 1, dyn_mathx_lgamma),
    JS_CFUNC_DEF("erf", 1, dyn_mathx_erf),
    JS_CFUNC_DEF("erfc", 1, dyn_mathx_erfc),
    JS_CFUNC_DEF("cbrt", 1, dyn_mathx_cbrt),
    JS_CFUNC_DEF("hypot", 2, dyn_mathx_hypot),
    JS_CFUNC_DEF("copysign", 2, dyn_mathx_copysign),
    JS_CFUNC_DEF("nextafter", 2, dyn_mathx_nextafter),
    JS_CFUNC_DEF("expm1", 1, dyn_mathx_expm1),
    JS_CFUNC_DEF("log1p", 1, dyn_mathx_log1p),
    JS_CFUNC_DEF("log2", 1, dyn_mathx_log2),
    JS_CFUNC_DEF("logb", 1, dyn_mathx_logb),
    JS_CFUNC_DEF("scalbn", 2, dyn_mathx_scalbn),
    JS_CFUNC_DEF("ilogb", 1, dyn_mathx_ilogb),
    JS_CFUNC_DEF("modf", 1, dyn_mathx_modf),
    JS_CFUNC_DEF("frexp", 1, dyn_mathx_frexp),
    JS_CFUNC_DEF("ldexp", 2, dyn_mathx_ldexp),
    JS_CFUNC_DEF("remainder", 2, dyn_mathx_remainder),
    JS_CFUNC_DEF("fmod", 2, dyn_mathx_fmod),
    JS_CFUNC_DEF("isInf", 1, dyn_mathx_is_inf),
    JS_CFUNC_DEF("isNaN", 1, dyn_mathx_is_nan),
    JS_CFUNC_DEF("signbit", 1, dyn_mathx_signbit),
    JS_CFUNC_DEF("trunc", 1, dyn_mathx_trunc),
    JS_CFUNC_DEF("round", 1, dyn_mathx_round),
    JS_CFUNC_DEF("roundToEven", 1, dyn_mathx_roundToEven),

    JS_CFUNC_DEF("gcd", 2, dyn_mathx_gcd),
    JS_CFUNC_DEF("lcm", 2, dyn_mathx_lcm),
    JS_CFUNC_DEF("factorial", 1, dyn_mathx_factorial),
    JS_CFUNC_DEF("isPrime", 1, dyn_mathx_is_prime),
    JS_CFUNC_DEF("abs", 1, dyn_mathx_abs),
    JS_CFUNC_DEF("bitLen", 1, dyn_mathx_bit_len),
    JS_CFUNC_DEF("popcount", 1, dyn_mathx_popcount),
};

/* MaxInt64 is a BigInt constant; there is no JS_PROP_*_DEF macro for a
 * BigInt (the JSCFunctionListEntry union has no BigInt member), so unlike
 * every other entry above it is declared here (js_nat_init_mathx) and set
 * here (dyn_mathx_init_module) by name instead of through the table. */
static int dyn_mathx_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (JS_SetModuleExportList(ctx, m, dyn_mathx_funcs,
                               countof(dyn_mathx_funcs)) < 0)
        return -1;
    return JS_SetModuleExport(ctx, m, "MaxInt64", JS_NewBigInt64(ctx, INT64_MAX));
}

int js_nat_init_mathx(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dynajs:mathx", dyn_mathx_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_mathx_funcs,
                               countof(dyn_mathx_funcs)) < 0)
        return -1;
    return JS_AddModuleExport(ctx, m, "MaxInt64");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_MATHX */
