/*
 * dyna:time -- time/duration utilities, self-contained, in-repo (no
 * external deps), mirroring Go's `time` package (durations, RFC3339
 * formatting/parsing, monotonic clock, civil calendar math).
 *
 *   import { Second, Hour, durationString, parseDuration, now, monotonicNano,
 *            formatRFC3339, parseRFC3339, date, fromUnix } from "dyna:time";
 *
 *   durationString(90 * Second);       // -> "1m30s"
 *   parseDuration("1h30m");            // -> 5400000000000  (ns, a Number)
 *   date(2001, 9, 9, 1, 46, 40);       // -> 1000000000      (unix seconds)
 *   fromUnix(1000000000).year;        // -> 2001
 *
 * Semantics (documented guarantees callers may rely on):
 *
 *   - Duration is modeled exactly like Go's time.Duration: an integer count
 *     of nanoseconds. The six unit constants (Nanosecond..Hour) are plain
 *     Numbers (all comfortably inside 2^53). durationString()/parseDuration()
 *     accept BOTH a Number and a BigInt for the nanosecond count (a Number is
 *     coerced with the engine's normal ToInt64 truncation; a BigInt wider
 *     than 64 bits wraps modulo 2^64, matching this engine's existing
 *     BigInt64Array/asIntN convention elsewhere) -- this is required because
 *     the natural source of a duration, `monotonicNano() - t0`, is a BigInt
 *     minus BigInt and routinely exceeds 2^53 ns (~104 days). parseDuration's
 *     RESULT is a Number when the magnitude is a safe integer (<= 2^53-1) and
 *     a BigInt otherwise, so ordinary sub-day durations compare with `===`
 *     against a literal while still being exact at any magnitude.
 *   - durationString() reproduces Go's Duration.String() byte for byte: unit
 *     auto-selection (ns/us/ms while |d| < 1s, else h/m/s), no zero-padding
 *     on the integer fields, trailing-zero-trimmed fractional seconds, and
 *     the literal special case "0s". Output uses the proper micro sign (µ,
 *     U+00B5); parseDuration accepts "us", "µs" (U+00B5) and "μs" (U+03BC,
 *     the Greek-letter lookalike Go also accepts) as spellings of the same
 *     unit.
 *   - now()/nowUnixNano()/nowMillis() read CLOCK_REALTIME (wall time, subject
 *     to NTP/user adjustment); monotonicNano() reads CLOCK_MONOTONIC (never
 *     goes backwards, unrelated epoch -- only meaningful as a difference
 *     between two calls in the same process). None of these block.
 *   - Calendar math (date()/fromUnix()/formatRFC3339()/formatUnix()/
 *     parseRFC3339()) is built on ONE from-scratch primitive: Howard
 *     Hinnant's days_from_civil/civil_from_days (the public-domain
 *     proleptic-Gregorian algorithm used by C++20 <chrono>). It is exact for
 *     every year a 64-bit day count can reach, including negative years
 *     (pre-1970) and every leap year, and does NOT call gmtime_r/timegm --
 *     this engine's own Date implementation (src/builtins/date.inc.c) makes
 *     the identical choice, for the identical reason: identical output
 *     across glibc/musl/Darwin libc instead of trusting each libc's calendar
 *     routines. date() additionally normalizes an out-of-range month (e.g.
 *     13 or 0) by carrying into the year, exactly like Go's time.Date; a
 *     day/hour/minute/second outside its usual range is simply additional
 *     seconds and carries naturally through the flat day-count arithmetic.
 *   - formatRFC3339(sec, nsec, utc) defaults utc=true (append literal "Z").
 *     utc=false asks the OS for the LOCAL zone's offset at that instant via
 *     localtime_r()+tm_gmtoff -- the same mechanism this engine's own
 *     Date.prototype.getTimezoneOffset() uses (see
 *     src/builtins/object_array_iterator.inc.c) -- and renders "+HH:MM" /
 *     "-HH:MM"; the civil fields themselves are still produced by OUR OWN
 *     algorithm, so only the offset value depends on the OS tzdata.
 *   - formatUnix(sec, layout) always renders the UTC breakdown and supports
 *     EXACTLY Go's reference-time tokens "2006" "01" "02" "15" "04" "05"
 *     "Jan" "Mon" (year/month/day/hour/minute/second/month-abbrev/
 *     weekday-abbrev); any other byte in the layout -- including Go's OWN
 *     unpadded "2"/"1"/"3"/"06" and spelled-out "Monday"/"January" tokens,
 *     which are deliberately out of scope -- is copied through literally
 *     (so e.g. a bare "2" in a layout is a literal digit, not a day-of-month
 *     token). (Chosen over a strftime-style layout for consistency with
 *     durationString's Go-shaped API; Go's layout has no ambiguous '%'
 *     escaping and ties naturally to a reference instant readers already
 *     know from Go.)
 *   - parseRFC3339 accepts "Z"/"z" or a "+HH:MM"/"-HH:MM" offset and an
 *     optional ".fraction" (1-9+ digits; beyond 9 the extra digits are still
 *     consumed but only the leading 9 contribute to the nanosecond result).
 *     The year accepts an optional leading '-' and any width >= 4 digits
 *     (matching what formatRFC3339/formatUnix's "2006" token ever emits),
 *     so a negative or > 9999 year round-trips through format+parse; every
 *     other field is the fixed width RFC3339 mandates. It validates the
 *     calendar fields strictly (month 1-12, day against the real
 *     days-in-that-month, hour<=23, minute<=59, second<=60 to tolerate a
 *     leap-second literal) and throws SyntaxError on anything else -- this
 *     is deliberately the opposite of date()'s permissive normalization,
 *     matching Go's own time.Parse (strict) vs time.Date (normalizing).
 *   - fromUnix()'s `weekday` is 0=Sunday..6=Saturday (Go's time.Weekday
 *     numbering) and `yday` is the 1-based day of year (Go's YearDay()).
 *
 * Memory discipline: every JS argument is coerced to an owned C value
 * (JS_ToInt64Ext/JS_ToCStringLen/JS_ToBool) BEFORE any allocation runs --
 * coercion may run arbitrary JS (valueOf/toString), but these are plain
 * functions with no closable resource, so nothing a reentrant coercion could
 * invalidate. formatUnix's output buffer grows on the heap (js_malloc/
 * js_realloc) because a layout string is caller-controlled and unbounded in
 * length; every other function's scratch space is a small stack buffer whose
 * maximum content length is a proven compile-time bound (a 64-bit integer
 * has at most 20 decimal digits), never sized by untrusted input. Every
 * JS_ToCStringLen result is released on every path; results are always fresh
 * JS values (JS_NewStringLen/JS_NewObject) -- nothing native escapes.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_NS_PER_US     1000LL
#define DYN_NS_PER_MS     1000000LL
#define DYN_NS_PER_SEC    1000000000LL
#define DYN_NS_PER_MIN    60000000000LL
#define DYN_NS_PER_HOUR   3600000000000LL
#define DYN_SECS_PER_DAY  86400LL
#define DYN_MAX_SAFE_INT  9007199254740991LL   /* 2^53 - 1 */
#define DYN_U64_2_POW_63  (((uint64_t)1) << 63)

/* ================================================================ *
 *  Civil calendar: Howard Hinnant's days_from_civil / civil_from_days
 *  (http://howardhinnant.github.io/date_algorithms.html, public domain).
 *  The ONLY calendar-math primitive this module uses -- see header comment.
 * ================================================================ */

static int64_t dyn_time_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

static int64_t dyn_time_floor_mod(int64_t a, int64_t b)
{
    int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

/* Days since the 1970-01-01 epoch (negative for earlier dates). `m` must be
 * in [1, 12]; `d` may be ANY integer (including <1 or >last-day-of-month) --
 * the linear day-of-year formula carries such overflow into the next/prior
 * month/year for free, which is exactly what date()'s Go-like normalization
 * relies on. */
static int64_t dyn_days_from_civil(int64_t y, int m, int64_t d)
{
    int64_t era, yoe, doy, doe;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                       /* [0, 399] */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Inverse of dyn_days_from_civil: decompose a day count into y/m/d (m in
 * [1,12], d in [1,31]). Exact for any int64_t z. */
static void dyn_civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;                                    /* [0, 146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
    *y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              /* [0, 365] */
    mp = (5 * doy + 2) / 153;                                   /* [0, 11] */
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);                   /* [1, 31] */
    *m = (int)(mp + (mp < 10 ? 3 : -9));                        /* [1, 12] */
    *y += (*m <= 2);
}

/* [0, 6] -> Sun..Sat (Go's time.Weekday numbering). */
static int dyn_weekday_from_days(int64_t z)
{
    return (int)(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

/* Number of days in civil month (y, m) (m in [1, 12]), leap years included;
 * derived from the same primitive so it can never disagree with it. */
static int dyn_time_days_in_month(int64_t y, int m)
{
    int64_t y2 = y;
    int m2 = m + 1;
    if (m2 > 12) {
        m2 = 1;
        y2++;
    }
    return (int)(dyn_days_from_civil(y2, m2, 1) - dyn_days_from_civil(y, m, 1));
}

/* Carry an out-of-[1,12] month into *y (floor-style: month 0 => December of
 * the prior year, month 13 => January of the next), matching Go's
 * time.Date month-overflow normalization. */
static void dyn_time_norm_month(int64_t *y, int64_t *mo)
{
    int64_t m0 = *mo - 1;
    int64_t yshift = dyn_time_floor_div(m0, 12);
    *y += yshift;
    *mo = dyn_time_floor_mod(m0, 12) + 1;
}

/* int64_t -> time_t, clamped on the (only theoretical, on this repo's 64-bit
 * targets) 32-bit time_t platform -- mirrors the same defensive clamp this
 * engine's own getTimezoneOffset() applies (object_array_iterator.inc.c). */
static time_t dyn_time_to_time_t(int64_t sec)
{
    if (sizeof(time_t) == 4) {
        if (sec < INT32_MIN)
            sec = INT32_MIN;
        else if (sec > INT32_MAX)
            sec = INT32_MAX;
    }
    return (time_t)sec;
}

/* ================================================================ *
 *  Small bounded numeric-to-decimal helpers (never a stack overflow: every
 *  caller's buffer is sized against a proven max digit count, see header).
 * ================================================================ */

/* Write v's decimal digits (no sign, no padding) into out; returns the
 * digit count (1-20). */
static int dyn_time_utoa(uint64_t v, char *out)
{
    char tmp[20];
    int n = 0, i;
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* Zero-padded to AT LEAST `width` digits (wider if v needs more, never
 * truncated -- matches Go's appendInt used for layout tokens and years). */
static int dyn_time_utoa_pad(uint64_t v, int width, char *out)
{
    char tmp[20];
    int n = dyn_time_utoa(v, tmp);
    int i;
    if (n >= width) {
        memcpy(out, tmp, (size_t)n);
        return n;
    }
    for (i = 0; i < width - n; i++)
        out[i] = '0';
    memcpy(out + (width - n), tmp, (size_t)n);
    return width;
}

/* Format `frac` (an integer in [0, 10^width)) as ".ddd" with trailing zeros
 * stripped; writes nothing and returns 0 if frac == 0 once padded/trimmed
 * (i.e. the fraction is exactly zero). width <= 9 at every call site. */
static int dyn_time_fmt_frac(uint64_t frac, int width, char *out)
{
    char digits[16];
    int n = dyn_time_utoa_pad(frac, width, digits);
    while (n > 0 && digits[n - 1] == '0')
        n--;
    if (n == 0)
        return 0;
    out[0] = '.';
    memcpy(out + 1, digits, (size_t)n);
    return n + 1;
}

/* ================================================================ *
 *  Growable output buffer for formatUnix (the only function whose output
 *  length is proportional to a caller-controlled, unbounded input string).
 * ================================================================ */

typedef struct DynTimeBuf {
    JSContext *ctx;
    uint8_t *data;
    size_t len, cap;
} DynTimeBuf;

static int dyn_time_buf_init(JSContext *ctx, DynTimeBuf *b, size_t hint)
{
    b->ctx = ctx;
    b->len = 0;
    b->cap = hint > 0 ? hint : 16;
    b->data = js_malloc(ctx, b->cap);
    if (!b->data) {
        b->cap = 0;
        return -1; /* js_malloc already threw */
    }
    return 0;
}

static int dyn_time_buf_reserve(DynTimeBuf *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t ncap;
    uint8_t *nd;
    if (need <= b->cap)
        return 0;
    ncap = b->cap * 2;
    if (ncap < need)
        ncap = need;
    nd = js_realloc(b->ctx, b->data, ncap);
    if (!nd)
        return -1; /* js_realloc already threw */
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static int dyn_time_buf_put(DynTimeBuf *b, const void *src, size_t n)
{
    if (dyn_time_buf_reserve(b, n))
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static void dyn_time_buf_free(DynTimeBuf *b)
{
    js_free(b->ctx, b->data);
    b->data = NULL;
}

/* ================================================================ *
 *  Duration: constants are plain module properties (see registration at the
 *  bottom); durationString / parseDuration below.
 * ================================================================ */

/* Number when safe (so plain integer literals compare with ===), BigInt
 * otherwise (so precision is never silently lost) -- see header comment. */
static JSValue dyn_time_ns_to_jsvalue(JSContext *ctx, int64_t ns)
{
    if (ns >= -DYN_MAX_SAFE_INT && ns <= DYN_MAX_SAFE_INT)
        return JS_NewInt64(ctx, ns);
    return JS_NewBigInt64(ctx, ns);
}

#define DYN_TIME_DUR_BUF 64

/* durationString(ns) -> string, byte-for-byte matching Go's
 * time.Duration.String(). */
static JSValue dyn_time_duration_string(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    int64_t ns;
    uint64_t u;
    int neg;
    char buf[DYN_TIME_DUR_BUF];
    int pos = 0;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64Ext(ctx, &ns, argv[0]))
        return JS_EXCEPTION;

    neg = ns < 0;
    u = (uint64_t)ns;
    if (neg)
        u = -u; /* well-defined unsigned negation; exact even for ns==INT64_MIN */

    if (u == 0)
        return JS_NewStringLen(ctx, "0s", 2);

    if (neg)
        buf[pos++] = '-';

    if (u < (uint64_t)DYN_NS_PER_SEC) {
        uint64_t ip, divisor;
        int width, ulen;
        const char *unit;

        if (u < (uint64_t)DYN_NS_PER_US) {
            ip = u;
            divisor = 1;
            width = 0;
            unit = "ns";
            ulen = 2;
        } else if (u < (uint64_t)DYN_NS_PER_MS) {
            divisor = (uint64_t)DYN_NS_PER_US;
            ip = u / divisor;
            width = 3;
            unit = "\xc2\xb5s"; /* U+00B5 MICRO SIGN + 's' */
            ulen = 3;
        } else {
            divisor = (uint64_t)DYN_NS_PER_MS;
            ip = u / divisor;
            width = 6;
            unit = "ms";
            ulen = 2;
        }
        pos += dyn_time_utoa(ip, buf + pos);
        if (width > 0)
            pos += dyn_time_fmt_frac(u % divisor, width, buf + pos);
        memcpy(buf + pos, unit, (size_t)ulen);
        pos += ulen;
    } else {
        uint64_t total_sec = u / (uint64_t)DYN_NS_PER_SEC;
        uint64_t frac_ns = u % (uint64_t)DYN_NS_PER_SEC;
        uint64_t secs = total_sec % 60ULL;
        uint64_t total_min = total_sec / 60ULL;

        if (total_min > 0) {
            uint64_t mins = total_min % 60ULL;
            uint64_t total_hr = total_min / 60ULL;
            if (total_hr > 0) {
                pos += dyn_time_utoa(total_hr, buf + pos);
                buf[pos++] = 'h';
            }
            pos += dyn_time_utoa(mins, buf + pos);
            buf[pos++] = 'm';
        }
        pos += dyn_time_utoa(secs, buf + pos);
        pos += dyn_time_fmt_frac(frac_ns, 9, buf + pos);
        buf[pos++] = 's';
    }
    return JS_NewStringLen(ctx, buf, pos);
}

struct DynTimeUnit {
    const char *name;
    int len;
    int64_t ns;
};

/* Matches Go's time.ParseDuration unitMap exactly, including both spellings
 * of the micro sign it accepts (U+00B5 and the Greek-letter lookalike
 * U+03BC). The unit "span" scanned by the caller is matched by its EXACT
 * byte length first, so "m" vs "ms" is never ambiguous (see caller). */
static const struct DynTimeUnit dyn_time_units[] = {
    { "ns", 2, 1LL },
    { "us", 2, DYN_NS_PER_US },
    { "\xc2\xb5s", 3, DYN_NS_PER_US },   /* µs  U+00B5 */
    { "\xce\xbcs", 3, DYN_NS_PER_US },   /* μs  U+03BC */
    { "ms", 2, DYN_NS_PER_MS },
    { "s",  1, DYN_NS_PER_SEC },
    { "m",  1, DYN_NS_PER_MIN },
    { "h",  1, DYN_NS_PER_HOUR },
};

static int dyn_time_lookup_unit(const char *s, size_t ulen, int64_t *out_ns)
{
    size_t i;
    for (i = 0; i < countof(dyn_time_units); i++) {
        if ((size_t)dyn_time_units[i].len == ulen &&
            memcmp(dyn_time_units[i].name, s, ulen) == 0) {
            *out_ns = dyn_time_units[i].ns;
            return 0;
        }
    }
    return -1;
}

/* Consume a run of ASCII digits as a fraction: accumulates into *pf (and
 * grows *pscale = 10^ndigits-kept) only while doing so cannot overflow --
 * once it would, further digits are still consumed (so the scan position
 * stays correct) but no longer counted, matching Go's leadingFraction
 * (float64 has nowhere near enough precision for a 19-digit fraction
 * anyway). Always "succeeds"; *pi is advanced past every digit consumed. */
static void dyn_time_leading_fraction(const char *s, size_t len, size_t *pi,
                                      uint64_t *pf, double *pscale)
{
    size_t i = *pi;
    uint64_t x = 0;
    double scale = 1.0;
    int overflow = 0;

    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
        int digit = s[i] - '0';
        if (!overflow) {
            if (x > (DYN_U64_2_POW_63 - 1) / 10) {
                overflow = 1;
            } else {
                uint64_t y = x * 10 + (uint64_t)digit;
                if (y > DYN_U64_2_POW_63)
                    overflow = 1;
                else {
                    x = y;
                    scale *= 10.0;
                }
            }
        }
    }
    *pi = i;
    *pf = x;
    *pscale = scale;
}

/* parseDuration(str) -> number|bigint, mirroring Go's time.ParseDuration:
 * a possibly-signed sequence of (number)(unit) pairs, e.g. "300ms", "-1.5h",
 * "2h45m"; units ns/us/µs/μs/ms/s/m/h. Throws SyntaxError on malformed
 * input (unlike date()'s deliberately permissive/normalizing sibling). */
static JSValue dyn_time_parse_duration(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const char *orig;
    size_t len, i = 0;
    int neg = 0;
    uint64_t d = 0;
    JSValue result = JS_EXCEPTION;

    (void)this_val;
    (void)argc;
    orig = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!orig)
        return JS_EXCEPTION;

    if (len > 0 && (orig[0] == '-' || orig[0] == '+')) {
        neg = (orig[0] == '-');
        i = 1;
    }

    /* Special case identical to Go: a bare "0" needs no unit. */
    if (len - i == 1 && orig[i] == '0') {
        result = JS_NewInt32(ctx, 0);
        goto done;
    }
    if (i >= len)
        goto bad;

    while (i < len) {
        uint64_t v = 0, f = 0;
        double scale = 1.0;
        int have_int, have_frac = 0;
        size_t start, unit_start;
        int64_t unit_ns;

        if (!(orig[i] == '.' || (orig[i] >= '0' && orig[i] <= '9')))
            goto bad;

        start = i;
        for (; i < len && orig[i] >= '0' && orig[i] <= '9'; i++) {
            int digit = orig[i] - '0';
            if (v > DYN_U64_2_POW_63 / 10)
                goto bad;
            v = v * 10 + (uint64_t)digit;
            if (v > DYN_U64_2_POW_63)
                goto bad;
        }
        have_int = (i != start);

        if (i < len && orig[i] == '.') {
            size_t before;
            i++;
            before = i;
            dyn_time_leading_fraction(orig, len, &i, &f, &scale);
            have_frac = (i != before);
        }
        if (!have_int && !have_frac)
            goto bad;

        unit_start = i;
        while (i < len && orig[i] != '.' && !(orig[i] >= '0' && orig[i] <= '9'))
            i++;
        if (i == unit_start)
            goto bad; /* missing unit */
        if (dyn_time_lookup_unit(orig + unit_start, i - unit_start, &unit_ns))
            goto bad; /* unknown unit */

        if (v > DYN_U64_2_POW_63 / (uint64_t)unit_ns)
            goto bad; /* overflow */
        v *= (uint64_t)unit_ns;

        if (f > 0) {
            double add = (double)f * ((double)unit_ns / scale);
            v += (uint64_t)add;
            if (v > DYN_U64_2_POW_63)
                goto bad;
        }

        d += v;
        if (d > DYN_U64_2_POW_63)
            goto bad;
    }

    if (neg) {
        int64_t signed_d = (d == DYN_U64_2_POW_63) ? INT64_MIN : -(int64_t)d;
        result = dyn_time_ns_to_jsvalue(ctx, signed_d);
    } else {
        if (d > (uint64_t)INT64_MAX)
            goto bad;
        result = dyn_time_ns_to_jsvalue(ctx, (int64_t)d);
    }
    goto done;

 bad:
    JS_ThrowSyntaxError(ctx, "dyna:time: invalid duration");
    result = JS_EXCEPTION;

 done:
    JS_FreeCString(ctx, orig);
    return result;
}

/* ================================================================ *
 *  Clock: CLOCK_REALTIME (wall) / CLOCK_MONOTONIC. Never blocks/sleeps.
 * ================================================================ */

static JSValue dyn_time_now(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    struct timespec ts;
    JSValue obj;

    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(ctx, obj, "sec",
                                  JS_NewInt64(ctx, (int64_t)ts.tv_sec),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "nsec",
                                  JS_NewInt32(ctx, (int32_t)ts.tv_nsec),
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue dyn_time_now_unix_nano(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewBigInt64(ctx, (int64_t)ts.tv_sec * DYN_NS_PER_SEC +
                                (int64_t)ts.tv_nsec);
}

static JSValue dyn_time_now_millis(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewInt64(ctx, (int64_t)ts.tv_sec * 1000 +
                            (int64_t)ts.tv_nsec / 1000000);
}

static JSValue dyn_time_monotonic_nano(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewBigInt64(ctx, (int64_t)ts.tv_sec * DYN_NS_PER_SEC +
                                (int64_t)ts.tv_nsec);
}

/* ================================================================ *
 *  Formatting
 * ================================================================ */

static const char *const dyn_time_month_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const dyn_time_weekday_abbr[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

#define DYN_TIME_RFC_BUF 96

/* formatRFC3339(unixSec, nsec=0, utc=true) -> string. */
static JSValue dyn_time_format_rfc3339(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    int64_t sec, nsec = 0, y, days, tod, off = 0;
    int mo, d, h, mi, s, utc = 1;
    char buf[DYN_TIME_RFC_BUF];
    int pos = 0;

    (void)this_val;
    if (JS_ToInt64Ext(ctx, &sec, argv[0]))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt64Ext(ctx, &nsec, argv[1]))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        int b = JS_ToBool(ctx, argv[2]);
        if (b < 0)
            return JS_EXCEPTION;
        utc = b;
    }
    if (nsec < 0 || nsec > 999999999)
        return JS_ThrowRangeError(ctx, "dyna:time: nsec must be in [0, 999999999]");

    if (!utc) {
        /* Ask the OS for the local offset ONLY -- see header comment. */
        time_t ti = dyn_time_to_time_t(sec);
        struct tm tmv;
        localtime_r(&ti, &tmv);
        off = tmv.tm_gmtoff;
    }

    days = dyn_time_floor_div(sec + off, DYN_SECS_PER_DAY);
    tod = dyn_time_floor_mod(sec + off, DYN_SECS_PER_DAY);
    h = (int)(tod / 3600);
    mi = (int)((tod / 60) % 60);
    s = (int)(tod % 60);
    dyn_civil_from_days(days, &y, &mo, &d);

    if (y < 0) {
        buf[pos++] = '-';
        y = -y;
    }
    pos += dyn_time_utoa_pad((uint64_t)y, 4, buf + pos);
    buf[pos++] = '-';
    pos += dyn_time_utoa_pad((uint64_t)mo, 2, buf + pos);
    buf[pos++] = '-';
    pos += dyn_time_utoa_pad((uint64_t)d, 2, buf + pos);
    buf[pos++] = 'T';
    pos += dyn_time_utoa_pad((uint64_t)h, 2, buf + pos);
    buf[pos++] = ':';
    pos += dyn_time_utoa_pad((uint64_t)mi, 2, buf + pos);
    buf[pos++] = ':';
    pos += dyn_time_utoa_pad((uint64_t)s, 2, buf + pos);
    if (nsec > 0)
        pos += dyn_time_fmt_frac((uint64_t)nsec, 9, buf + pos);

    if (off == 0) {
        buf[pos++] = 'Z';
    } else {
        int64_t o = off;
        int oneg = o < 0;
        if (oneg)
            o = -o;
        buf[pos++] = oneg ? '-' : '+';
        pos += dyn_time_utoa_pad((uint64_t)(o / 3600), 2, buf + pos);
        buf[pos++] = ':';
        pos += dyn_time_utoa_pad((uint64_t)((o / 60) % 60), 2, buf + pos);
    }
    return JS_NewStringLen(ctx, buf, pos);
}

/* formatUnix(unixSec, layout) -> string. layout uses Go's reference-time
 * tokens "2006"/"01"/"02"/"15"/"04"/"05"/"Jan"/"Mon"; always UTC. */
static JSValue dyn_time_format_unix(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    int64_t sec, y, days, tod;
    int mo, d, h, mi, s, wd;
    const char *layout;
    size_t llen, i;
    DynTimeBuf out;
    JSValue result = JS_EXCEPTION;
    int have_buf = 0;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64Ext(ctx, &sec, argv[0]))
        return JS_EXCEPTION;
    layout = JS_ToCStringLen(ctx, &llen, argv[1]);
    if (!layout)
        return JS_EXCEPTION;

    days = dyn_time_floor_div(sec, DYN_SECS_PER_DAY);
    tod = dyn_time_floor_mod(sec, DYN_SECS_PER_DAY);
    h = (int)(tod / 3600);
    mi = (int)((tod / 60) % 60);
    s = (int)(tod % 60);
    dyn_civil_from_days(days, &y, &mo, &d);
    wd = dyn_weekday_from_days(days);

    if (dyn_time_buf_init(ctx, &out, llen + 16))
        goto done;
    have_buf = 1;

    i = 0;
    while (i < llen) {
        char tmp[8];
        size_t rem = llen - i;
        const char *p = layout + i;
        int n;

        if (rem >= 4 && memcmp(p, "2006", 4) == 0) {
            int yneg = y < 0;
            uint64_t yy = (uint64_t)(yneg ? -y : y);
            if (yneg && dyn_time_buf_put(&out, "-", 1))
                goto done;
            n = dyn_time_utoa_pad(yy, 4, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 4;
        } else if (rem >= 3 && memcmp(p, "Jan", 3) == 0) {
            if (dyn_time_buf_put(&out, dyn_time_month_abbr[mo - 1], 3))
                goto done;
            i += 3;
        } else if (rem >= 3 && memcmp(p, "Mon", 3) == 0) {
            if (dyn_time_buf_put(&out, dyn_time_weekday_abbr[wd], 3))
                goto done;
            i += 3;
        } else if (rem >= 2 && memcmp(p, "01", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)mo, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else if (rem >= 2 && memcmp(p, "02", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)d, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else if (rem >= 2 && memcmp(p, "15", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)h, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else if (rem >= 2 && memcmp(p, "04", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)mi, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else if (rem >= 2 && memcmp(p, "05", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)s, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else {
            if (dyn_time_buf_put(&out, p, 1))
                goto done;
            i += 1;
        }
    }
    result = JS_NewStringLen(ctx, (const char *)out.data, out.len);

 done:
    if (have_buf)
        dyn_time_buf_free(&out);
    JS_FreeCString(ctx, layout);
    return result;
}

/* ================================================================ *
 *  Parsing
 * ================================================================ */

/* Read exactly n ASCII digits at s[pos..pos+n) into *out; 0 on success. */
static int dyn_time_expect_digits(const char *s, size_t len, size_t pos,
                                  int n, int64_t *out)
{
    int64_t v = 0;
    int i;
    if (pos + (size_t)n > len)
        return -1;
    for (i = 0; i < n; i++) {
        char c = s[pos + (size_t)i];
        if (c < '0' || c > '9')
            return -1;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return 0;
}

/* Scan an optional '-' sign then a run of ASCII digits (>= 4 of them) as a
 * year, advancing *ppos past it; 0 on success. Unlike the month/day/time
 * fields (always exactly 2 digits), the year is variable-width and
 * possibly negative, because formatRFC3339/formatUnix's "2006" token
 * mirrors Go's appendInt: at least 4 digits, wider if the year needs it,
 * signed if negative -- so the parser must accept the same shape to
 * round-trip the formatter's own output. Overflow-safe: an absurdly long
 * digit run is rejected rather than silently wrapping. */
static int dyn_time_scan_year(const char *s, size_t len, size_t *ppos,
                              int64_t *out)
{
    size_t pos = *ppos, start;
    int neg = 0;
    int64_t v = 0;

    if (pos < len && s[pos] == '-') {
        neg = 1;
        pos++;
    }
    start = pos;
    while (pos < len && s[pos] >= '0' && s[pos] <= '9') {
        if (v > (INT64_MAX - 9) / 10)
            return -1; /* overflow */
        v = v * 10 + (s[pos] - '0');
        pos++;
    }
    if (pos - start < 4)
        return -1; /* every year we ever emit has at least 4 digits */
    *out = neg ? -v : v;
    *ppos = pos;
    return 0;
}

/* parseRFC3339(str) -> {sec, nsec}. Strict: throws SyntaxError on anything
 * that doesn't fit "YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)" with
 * valid calendar fields. */
static JSValue dyn_time_parse_rfc3339(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *s;
    size_t len, pos = 0;
    int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, nsec = 0, off_sec = 0;
    JSValue result;

    (void)this_val;
    (void)argc;
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;

    if (dyn_time_scan_year(s, len, &pos, &y))
        goto fail;
    if (pos >= len || s[pos] != '-')
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &mo))
        goto fail;
    pos += 2;
    if (pos >= len || s[pos] != '-')
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &d))
        goto fail;
    pos += 2;
    if (pos >= len || (s[pos] != 'T' && s[pos] != 't'))
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &h))
        goto fail;
    pos += 2;
    if (pos >= len || s[pos] != ':')
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &mi))
        goto fail;
    pos += 2;
    if (pos >= len || s[pos] != ':')
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &se))
        goto fail;
    pos += 2;

    if (pos < len && s[pos] == '.') {
        size_t start;
        int ndig, i, use;
        int64_t frac = 0;
        pos++;
        start = pos;
        while (pos < len && s[pos] >= '0' && s[pos] <= '9')
            pos++;
        ndig = (int)(pos - start);
        if (ndig == 0)
            goto fail;
        use = ndig < 9 ? ndig : 9;
        for (i = 0; i < use; i++)
            frac = frac * 10 + (s[start + (size_t)i] - '0');
        for (; i < 9; i++)
            frac *= 10;
        nsec = frac;
    }

    if (pos >= len)
        goto fail;
    if (s[pos] == 'Z' || s[pos] == 'z') {
        pos++;
        off_sec = 0;
    } else if (s[pos] == '+' || s[pos] == '-') {
        int sign = (s[pos] == '-') ? -1 : 1;
        int64_t oh, om;
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &oh))
            goto fail;
        pos += 2;
        if (pos >= len || s[pos] != ':')
            goto fail;
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &om))
            goto fail;
        pos += 2;
        if (oh > 23 || om > 59)
            goto fail;
        off_sec = sign * (oh * 3600 + om * 60);
    } else {
        goto fail;
    }
    if (pos != len)
        goto fail; /* trailing garbage */

    if (mo < 1 || mo > 12)
        goto fail;
    if (d < 1 || d > dyn_time_days_in_month(y, (int)mo))
        goto fail;
    if (h > 23 || mi > 59 || se > 60)
        goto fail; /* se==60 tolerated as a leap-second literal */

    {
        int64_t days = dyn_days_from_civil(y, (int)mo, d);
        int64_t total = days * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + se - off_sec;
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) {
            result = JS_EXCEPTION;
            goto out;
        }
        if (JS_DefinePropertyValueStr(ctx, obj, "sec", JS_NewInt64(ctx, total),
                                      JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueStr(ctx, obj, "nsec",
                                      JS_NewInt32(ctx, (int32_t)nsec),
                                      JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, obj);
            result = JS_EXCEPTION;
            goto out;
        }
        result = obj;
        goto out;
    }

 fail:
    JS_ThrowSyntaxError(ctx, "dyna:time: invalid RFC3339 timestamp");
    result = JS_EXCEPTION;

 out:
    JS_FreeCString(ctx, s);
    return result;
}

/* ================================================================ *
 *  Civil date helpers
 * ================================================================ */

/* date(y, mo, d, h=0, mi=0, s=0) -> unixSec (UTC), normalizing an
 * out-of-[1,12] month like Go's time.Date. */
static JSValue dyn_time_date(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    int64_t y, mo, d, h = 0, mi = 0, s = 0, days, total;

    (void)this_val;
    if (JS_ToInt64Ext(ctx, &y, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt64Ext(ctx, &mo, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToInt64Ext(ctx, &d, argv[2]))
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && JS_ToInt64Ext(ctx, &h, argv[3]))
        return JS_EXCEPTION;
    if (argc > 4 && !JS_IsUndefined(argv[4]) && JS_ToInt64Ext(ctx, &mi, argv[4]))
        return JS_EXCEPTION;
    if (argc > 5 && !JS_IsUndefined(argv[5]) && JS_ToInt64Ext(ctx, &s, argv[5]))
        return JS_EXCEPTION;

    dyn_time_norm_month(&y, &mo);
    days = dyn_days_from_civil(y, (int)mo, d);
    total = days * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + s;
    return JS_NewInt64(ctx, total);
}

/* fromUnix(sec) -> {year, month, day, hour, min, sec, weekday, yday}. */
static JSValue dyn_time_from_unix(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    int64_t sec, days, tod, y, yday_start;
    int mo, d, h, mi, s, wd, yday;
    JSValue obj;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64Ext(ctx, &sec, argv[0]))
        return JS_EXCEPTION;

    days = dyn_time_floor_div(sec, DYN_SECS_PER_DAY);
    tod = dyn_time_floor_mod(sec, DYN_SECS_PER_DAY);
    h = (int)(tod / 3600);
    mi = (int)((tod / 60) % 60);
    s = (int)(tod % 60);

    dyn_civil_from_days(days, &y, &mo, &d);
    wd = dyn_weekday_from_days(days);
    yday_start = dyn_days_from_civil(y, 1, 1);
    yday = (int)(days - yday_start) + 1;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(ctx, obj, "year", JS_NewInt64(ctx, y),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "month", JS_NewInt32(ctx, mo),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "day", JS_NewInt32(ctx, d),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "hour", JS_NewInt32(ctx, h),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "min", JS_NewInt32(ctx, mi),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "sec", JS_NewInt32(ctx, s),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "weekday", JS_NewInt32(ctx, wd),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "yday", JS_NewInt32(ctx, yday),
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* ---------- module registration ---------- */

static const JSCFunctionListEntry dyn_time_funcs[] = {
    JS_PROP_INT64_DEF("Nanosecond", 1LL, 0),
    JS_PROP_INT64_DEF("Microsecond", DYN_NS_PER_US, 0),
    JS_PROP_INT64_DEF("Millisecond", DYN_NS_PER_MS, 0),
    JS_PROP_INT64_DEF("Second", DYN_NS_PER_SEC, 0),
    JS_PROP_INT64_DEF("Minute", DYN_NS_PER_MIN, 0),
    JS_PROP_INT64_DEF("Hour", DYN_NS_PER_HOUR, 0),
    JS_CFUNC_DEF("durationString", 1, dyn_time_duration_string),
    JS_CFUNC_DEF("parseDuration", 1, dyn_time_parse_duration),
    JS_CFUNC_DEF("now", 0, dyn_time_now),
    JS_CFUNC_DEF("nowUnixNano", 0, dyn_time_now_unix_nano),
    JS_CFUNC_DEF("nowMillis", 0, dyn_time_now_millis),
    JS_CFUNC_DEF("monotonicNano", 0, dyn_time_monotonic_nano),
    JS_CFUNC_DEF("formatRFC3339", 1, dyn_time_format_rfc3339),
    JS_CFUNC_DEF("formatUnix", 2, dyn_time_format_unix),
    JS_CFUNC_DEF("parseRFC3339", 1, dyn_time_parse_rfc3339),
    JS_CFUNC_DEF("date", 3, dyn_time_date),
    JS_CFUNC_DEF("fromUnix", 1, dyn_time_from_unix),
};

static int dyn_time_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_time_funcs,
                                  countof(dyn_time_funcs));
}

int js_nat_init_time(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:time", dyn_time_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_time_funcs,
                                  countof(dyn_time_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_TIME */
