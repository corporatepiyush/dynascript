/* test_bits.js -- dynajs:bits (in-repo faithful port of Go's math/bits).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_bits.js
 *
 * Every function is cross-checked against an INDEPENDENT reference computed
 * here: naive bit-by-bit loops for the count/leading/trailing/len/reverse
 * family, and BigInt schoolbook arithmetic for Add/Sub/Mul/Div/Rem. Inputs are
 * a large deterministic-random set (xorshift32, so failures reproduce) plus
 * hand vectors for the corners (0, all-ones, every single bit, width
 * boundaries, Mul64 max*max, Div overflow/zero throws). Expected values come
 * from the references, NOT from Go's doc examples (whose hex annotations are
 * unreliable -- e.g. Reverse8(19) is 0b11001000 == 0xC8, not the doc's 0xD8).
 *
 * Prints "test_bits: all tests passed (N assertions)"; throws on any failure. */

import * as bits from "dynajs:bits";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
/* compare an integer (count) result: both are plain Numbers */
function eqInt(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("FAIL " + msg + ": got " + got + " want " + want);
}
/* compare a width-w value result (Number for w<64, BigInt for 64) against a
 * BigInt reference, coercing both to BigInt masked to the width. */
function eqBig(got, want, w, msg) {
    n++;
    const g = BigInt(got);
    const m = (want & maskOf(w));
    if (g !== m)
        throw new Error("FAIL " + msg + ": got " + g + " want " + m);
}
function throws(fn) {
    try { fn(); return false; } catch (e) { return true; }
}

function maskOf(w) { return (1n << BigInt(w)) - 1n; }
function toBig(v, w) { return BigInt(v) & maskOf(w); } /* masked BigInt of an input */

/* ---- deterministic PRNG (xorshift32; only 32-bit shifts/xor, exact in JS) ---- */
let _s = 0x9e3779b9 | 0;
function rndU32() {
    let x = _s | 0;
    x ^= x << 13; x |= 0;
    x ^= x >>> 17;
    x ^= x << 5; x |= 0;
    _s = x | 0;
    return x >>> 0;
}
/* a random value of width w in the JS type the module expects (Number<=32, BigInt=64) */
function randVal(w) {
    if (w === 64) return (BigInt(rndU32()) << 32n) | BigInt(rndU32());
    if (w === 32) return rndU32();
    return rndU32() & ((1 << w) - 1); /* w in {8,16} */
}
/* a random signed rotate count (well inside int32 range) */
function randK() { return (rndU32() % 4001) - 2000; }

/* ================= references (independent of the C module) ================= */
function refLen(bx)        { let c = 0; while (bx > 0n) { c++; bx >>= 1n; } return c; }
function refLeadingZeros(bx, w)  { return w - refLen(bx); }
function refTrailingZeros(bx, w) { if (bx === 0n) return w; let c = 0; while ((bx & 1n) === 0n) { c++; bx >>= 1n; } return c; }
function refOnesCount(bx)  { let c = 0; while (bx > 0n) { c += Number(bx & 1n); bx >>= 1n; } return c; }
function refReverse(bx, w) { let r = 0n; for (let i = 0; i < w; i++) { r = (r << 1n) | (bx & 1n); bx >>= 1n; } return r; }
function refReverseBytes(bx, w) { const nb = w / 8; let r = 0n; for (let i = 0; i < nb; i++) { r = (r << 8n) | (bx & 0xFFn); bx >>= 8n; } return r; }
function refRotateLeft(bx, k, w) {
    const W = BigInt(w);
    const s = BigInt(((k % w) + w) % w);          /* == Go's uint(k)&(w-1) for power-of-2 w */
    return ((bx << s) | (bx >> (W - s))) & maskOf(w);
}
function refAdd(a, b, c, w) { const mod = 1n << BigInt(w); const s = a + b + c; return [s % mod, s / mod]; }
function refSub(a, b, c, w) { const mod = 1n << BigInt(w); let d = a - b - c; const bo = d < 0n ? 1n : 0n; d = ((d % mod) + mod) % mod; return [d, bo]; }
function refMul(a, b, w)    { const mod = 1n << BigInt(w); const p = a * b; return [p / mod, p % mod]; }
function refDiv(hi, lo, y, w) { const mod = 1n << BigInt(w); const d = hi * mod + lo; return [d / y, d % y]; }
function refRem(hi, lo, y, w) { const mod = 1n << BigInt(w); const d = hi * mod + lo; return d % y; }

const WIDTHS = [8, 16, 32, 64];
const RUNS = 400;

/* hand vectors per width: 0, 1, all-ones, every single-bit, and two patterns */
function handCases(w) {
    const big = w === 64;
    const arr = [];
    arr.push(big ? 0n : 0, big ? 1n : 1, big ? maskOf(64) : (2 ** w - 1));
    for (let i = 0; i < w; i++) arr.push(big ? (1n << BigInt(i)) : Math.pow(2, i));
    if (big) { arr.push(0xDEADBEEFCAFEBABEn, 0x8000000000000000n, 0x0F0F0F0F0F0F0F0Fn); }
    else     { arr.push(w >= 16 ? 0xCAFE & (2 ** w - 1) : 0xCA, Math.floor((2 ** w - 1) / 2), 1 << (w - 1)); }
    return arr;
}
function caseSet(w) {
    const arr = handCases(w);
    for (let i = 0; i < RUNS; i++) arr.push(randVal(w));
    return arr;
}

/* ===================== bit-count family (result: Number) ===================== */
for (const w of WIDTHS) {
    for (const x of caseSet(w)) {
        const bx = toBig(x, w);
        eqInt(bits["LeadingZeros" + w](x), refLeadingZeros(bx, w), "LeadingZeros" + w + "(" + x + ")");
        eqInt(bits["TrailingZeros" + w](x), refTrailingZeros(bx, w), "TrailingZeros" + w + "(" + x + ")");
        eqInt(bits["OnesCount" + w](x), refOnesCount(bx), "OnesCount" + w + "(" + x + ")");
        eqInt(bits["Len" + w](x), refLen(bx), "Len" + w + "(" + x + ")");
    }
}

/* ===================== Reverse (result: same width) ===================== */
for (const w of WIDTHS)
    for (const x of caseSet(w))
        eqBig(bits["Reverse" + w](x), refReverse(toBig(x, w), w), w, "Reverse" + w + "(" + x + ")");

/* ===================== ReverseBytes (16/32/64) ===================== */
for (const w of [16, 32, 64])
    for (const x of caseSet(w))
        eqBig(bits["ReverseBytes" + w](x), refReverseBytes(toBig(x, w), w), w, "ReverseBytes" + w + "(" + x + ")");

/* ===================== RotateLeft (k any sign) ===================== */
for (const w of WIDTHS) {
    const ks = [0, 1, -1, w, -w, w + 1, -(w + 1), 2 * w, 3, -3, w - 1, 1 - w];
    for (let i = 0; i < 8; i++) ks.push(randK());
    for (const x of caseSet(w))
        for (const k of ks)
            eqBig(bits["RotateLeft" + w](x, k), refRotateLeft(toBig(x, w), k, w), w,
                  "RotateLeft" + w + "(" + x + ", " + k + ")");
}

/* ===================== Add / Sub (carry/borrow in {0,1}) ===================== */
for (const w of [32, 64]) {
    const carries = w === 64 ? [0n, 1n] : [0, 1];
    for (let i = 0; i < RUNS * 3; i++) {
        const a = randVal(w), b = randVal(w);
        const ba = toBig(a, w), bb = toBig(b, w);
        for (const c of carries) {
            const bc = toBig(c, w);
            const [s, co] = bits["Add" + w](a, b, c);
            const [rs, rco] = refAdd(ba, bb, bc, w);
            eqBig(s, rs, w, "Add" + w + " sum(" + a + "," + b + "," + c + ")");
            eqBig(co, rco, w, "Add" + w + " carry(" + a + "," + b + "," + c + ")");
            const [d, bo] = bits["Sub" + w](a, b, c);
            const [rd, rbo] = refSub(ba, bb, bc, w);
            eqBig(d, rd, w, "Sub" + w + " diff(" + a + "," + b + "," + c + ")");
            eqBig(bo, rbo, w, "Sub" + w + " borrow(" + a + "," + b + "," + c + ")");
        }
    }
}

/* ===================== Mul (full-width product) ===================== */
for (const w of [32, 64]) {
    for (let i = 0; i < RUNS * 3; i++) {
        const a = randVal(w), b = randVal(w);
        const [hi, lo] = bits["Mul" + w](a, b);
        const [rhi, rlo] = refMul(toBig(a, w), toBig(b, w), w);
        eqBig(hi, rhi, w, "Mul" + w + " hi(" + a + "," + b + ")");
        eqBig(lo, rlo, w, "Mul" + w + " lo(" + a + "," + b + ")");
    }
}

/* ===================== Div / Rem (valid path: y!=0, y>hi for Div) ===================== */
for (const w of [32, 64]) {
    const one = w === 64 ? 1n : 1, zero = w === 64 ? 0n : 0;
    for (let i = 0; i < RUNS * 3; i++) {
        /* nonzero y */
        let y = randVal(w); if (y === zero) y = one;
        const by = toBig(y, w);
        const lo = randVal(w);
        /* Div requires hi < y (no quotient overflow): hi = randomValue mod y */
        const hi = w === 64 ? (randVal(w) % y) : (randVal(w) % y);
        const [q, r] = bits["Div" + w](hi, lo, y);
        const [rq, rr] = refDiv(toBig(hi, w), toBig(lo, w), by, w);
        eqBig(q, rq, w, "Div" + w + " quo(" + hi + "," + lo + "," + y + ")");
        eqBig(r, rr, w, "Div" + w + " rem(" + hi + "," + lo + "," + y + ")");
        /* Rem accepts any hi (even hi >= y) */
        const hi2 = randVal(w);
        const rem = bits["Rem" + w](hi2, lo, y);
        eqBig(rem, refRem(toBig(hi2, w), toBig(lo, w), by, w), w,
              "Rem" + w + "(" + hi2 + "," + lo + "," + y + ")");
    }
}

/* ===================== hand vectors (specific known values) ===================== */
eqInt(bits.LeadingZeros8(1), 7, "LeadingZeros8(1)");
eqInt(bits.LeadingZeros8(0), 8, "LeadingZeros8(0)");
eqInt(bits.LeadingZeros16(0), 16, "LeadingZeros16(0)");
eqInt(bits.LeadingZeros32(1), 31, "LeadingZeros32(1)");
eqInt(bits.LeadingZeros64(0n), 64, "LeadingZeros64(0)");
eqInt(bits.LeadingZeros64(1n), 63, "LeadingZeros64(1)");
eqInt(bits.TrailingZeros8(0), 8, "TrailingZeros8(0)");
eqInt(bits.TrailingZeros8(14), 1, "TrailingZeros8(14)");
eqInt(bits.TrailingZeros64(0n), 64, "TrailingZeros64(0)");
eqInt(bits.TrailingZeros64(1n << 40n), 40, "TrailingZeros64(2^40)");
eqInt(bits.OnesCount8(14), 3, "OnesCount8(14)");
eqInt(bits.OnesCount64(0xFFFFFFFFFFFFFFFFn), 64, "OnesCount64(all ones)");
eqInt(bits.Len8(8), 4, "Len8(8)");
eqInt(bits.Len8(0), 0, "Len8(0)");
eqInt(bits.Len64(0n), 0, "Len64(0)");
eqInt(bits.Len64(0xFFFFFFFFFFFFFFFFn), 64, "Len64(all ones)");

eqBig(bits.Reverse8(1), 128n, 8, "Reverse8(1)");
eqBig(bits.Reverse8(19), 0xC8n, 8, "Reverse8(19)");          /* 00010011 -> 11001000 */
eqBig(bits.Reverse32(1), 0x80000000n, 32, "Reverse32(1)");
eqBig(bits.Reverse64(1n), 0x8000000000000000n, 64, "Reverse64(1)");
eqBig(bits.ReverseBytes16(15), 0x0F00n, 16, "ReverseBytes16(15)");
eqBig(bits.ReverseBytes32(0xFF), 0xFF000000n, 32, "ReverseBytes32(0xFF)");
eqBig(bits.ReverseBytes64(0x1122334455667788n), 0x8877665544332211n, 64, "ReverseBytes64");

eqBig(bits.RotateLeft8(15, 2), 0x3Cn, 8, "RotateLeft8(15,2)");
eqBig(bits.RotateLeft8(15, -2), 0xC3n, 8, "RotateLeft8(15,-2)");   /* rotate right by 2 */
eqBig(bits.RotateLeft8(0xFF, 100), 0xFFn, 8, "RotateLeft8(all ones)");
eqBig(bits.RotateLeft64(1n, 64), 1n, 64, "RotateLeft64(1,64) identity");
eqBig(bits.RotateLeft64(1n, 0), 1n, 64, "RotateLeft64(1,0) identity");

/* Mul / Add corners */
{
    const [hi, lo] = bits.Mul32(0x80000000, 2);
    eqBig(hi, 1n, 32, "Mul32 overflow hi"); eqBig(lo, 0n, 32, "Mul32 overflow lo");
}
{
    const MAX = 0xFFFFFFFFFFFFFFFFn;
    const [hi, lo] = bits.Mul64(MAX, MAX);
    eqBig(hi, 0xFFFFFFFFFFFFFFFEn, 64, "Mul64 max*max hi");
    eqBig(lo, 1n, 64, "Mul64 max*max lo");
}
{
    const [s, c] = bits.Add64(0xFFFFFFFFFFFFFFFFn, 1n, 0n);
    eqBig(s, 0n, 64, "Add64 wrap sum"); eqBig(c, 1n, 64, "Add64 wrap carry");
}
{
    const [d, b] = bits.Sub32(0, 1, 0);
    eqBig(d, 0xFFFFFFFFn, 32, "Sub32 underflow diff"); eqBig(b, 1n, 32, "Sub32 underflow borrow");
}
{
    const [q, r] = bits.Div32(2, 0x80000000, 0x80000000); /* Go doc example -> (5,0) */
    eqBig(q, 5n, 32, "Div32 example quo"); eqBig(r, 0n, 32, "Div32 example rem");
}
{
    const [q, r] = bits.Div64(0n, 6n, 3n);
    eqBig(q, 2n, 64, "Div64(0,6,3) quo"); eqBig(r, 0n, 64, "Div64(0,6,3) rem");
}

/* Div/Rem throw semantics */
assert(throws(() => bits.Div32(0, 10, 0)), "Div32 y==0 throws");
assert(throws(() => bits.Div32(5, 0, 3)), "Div32 y<=hi throws (overflow)");
assert(throws(() => bits.Div32(3, 0, 3)), "Div32 y==hi throws (overflow)");
assert(!throws(() => bits.Div32(2, 0, 3)), "Div32 y>hi ok");
assert(throws(() => bits.Div64(0n, 10n, 0n)), "Div64 y==0 throws");
assert(throws(() => bits.Div64(5n, 0n, 3n)), "Div64 y<=hi throws (overflow)");
assert(!throws(() => bits.Div64(2n, 0n, 3n)), "Div64 y>hi ok");
assert(throws(() => bits.Rem32(5, 0, 0)), "Rem32 y==0 throws");
assert(!throws(() => bits.Rem32(5, 0, 3)), "Rem32 no overflow throw");
assert(throws(() => bits.Rem64(5n, 0n, 0n)), "Rem64 y==0 throws");
assert(!throws(() => bits.Rem64(5n, 0n, 3n)), "Rem64 no overflow throw");

/* type discipline: 64-bit variants speak BigInt; count family always Number */
assert(typeof bits.LeadingZeros64(1n) === "number", "LeadingZeros64 -> Number");
assert(typeof bits.OnesCount64(1n) === "number", "OnesCount64 -> Number");
assert(typeof bits.Reverse64(1n) === "bigint", "Reverse64 -> BigInt");
assert(typeof bits.Reverse32(1) === "number", "Reverse32 -> Number");
assert(typeof bits.RotateLeft64(1n, 1) === "bigint", "RotateLeft64 -> BigInt");
assert(typeof bits.Mul64(1n, 1n)[0] === "bigint", "Mul64 elem -> BigInt");
assert(typeof bits.Mul32(1, 1)[0] === "number", "Mul32 elem -> Number");
assert(typeof bits.Add64(1n, 1n, 0n)[1] === "bigint", "Add64 carry -> BigInt");
assert(typeof bits.Rem64(1n, 1n, 3n) === "bigint", "Rem64 -> BigInt");
/* wrong-type arguments throw (Number->BigInt reader, BigInt->ToUint32) */
assert(throws(() => bits.LeadingZeros64(5)), "LeadingZeros64(Number) throws");
assert(throws(() => bits.Reverse64(5)), "Reverse64(Number) throws");
assert(throws(() => bits.LeadingZeros32(5n)), "LeadingZeros32(BigInt) throws");

/* UintSize constant */
eqInt(bits.UintSize, 64, "UintSize");

/* reentrant valueOf on a 32-bit function must not crash (no native resource,
 * but exercises the coerce-first path). */
{
    let hits = 0;
    const evil = { valueOf() { hits++; return 7; } };
    eqInt(bits.OnesCount8(evil), 3, "OnesCount8(valueOf->7)"); /* 7 = 0b111 */
    assert(hits === 1, "valueOf called exactly once");
}

print("test_bits: all tests passed (" + n + " assertions)");
