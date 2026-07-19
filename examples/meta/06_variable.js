/*
 * meta@ directives — VARIABLE-based (dynascript additions)
 *
 * These name a specific variable as their first argument and assert a fact the
 * optimiser could exploit (a value range, non-null, a type, aligned buffer, …).
 * All but `volatile` are UNSAFE assertions and need the file-level opt-in.
 *
 * Form: meta@<dir>(<var> [, <args>...])
 */

// meta@enable(unsafe)

function pixel(i, buf) {
    // meta@range(i, 0, 255)
    // meta@nonnull(buf)
    // meta@frozen(buf)
    return buf[i & 0xff];
}

function safeDiv(num, den) {
    // meta@nonzero(den)
    return num / den;
}

function typed(x) {
    // meta@type(x, i32)
    return (x | 0) + 1;
}

function withConst() {
    // meta@const(k)
    const k = 42;
    let s = 0;
    for (let i = 0; i < 10; i++) s += k;
    return s;
}

function simd(buf, n) {
    // meta@align(buf, 16)
    // meta@length(buf, 4)
    let s = 0;
    for (let i = 0; i < n; i++) s += buf[i];
    return s;
}

function initialised() {
    // meta@init(v)
    let v = 7;
    return v * 2;
}

function counter() {
    // meta@volatile(ticks)
    let ticks = 0;
    for (let i = 0; i < 5; i++) ticks++;
    return ticks;
}

const buf = new Uint8Array([10, 20, 30, 40]);
console.log("variable level ok: pixel=" + pixel(2, buf) + " div=" + safeDiv(10, 2) +
            " typed=" + typed(41) + " const=" + withConst() +
            " simd=" + simd(buf, 4) + " init=" + initialised() +
            " counter=" + counter());
