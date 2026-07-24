/* test_typedarray_ext.js — SIMD-accelerated `_`-prefixed TypedArray reductions.
 * Float64/Float32/Int32 use the multi-ISA SIMD kernels; other numeric element
 * types use a scalar loop. Run: dynajs tests/test_typedarray_ext.js
 * (Core builtins — present in every build; verify on macOS AND Linux amd64/arm64.) */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(Object.is(a, b) || a === b, m + " (got " + a + ", want " + b + ")"); }
/* SIMD sum reorders additions -> not bit-identical; compare with a rel tolerance */
function near(a, b, m, tol = 1e-9) { assert(Math.abs(a - b) <= tol * Math.max(1, Math.abs(b)), m + " (got " + a + ", want ≈" + b + ")"); }

/* ---- Float64Array (SIMD f64_sum/f64_min/f64_max) ---- */
{
    const a = new Float64Array([3.5, -1.5, 4.5, 1.0, 5.5]);
    near(a._sum(), 13.0, "f64 _sum");
    eq(a._min(), -1.5, "f64 _min (bit-exact)");
    eq(a._max(), 5.5, "f64 _max (bit-exact)");
    near(a._mean(), 2.6, "f64 _mean");
    near(a._average(), 2.6, "f64 _average alias");
}
/* ---- Int32Array (SIMD i32_sum widening / i32_min / i32_max) ---- */
{
    const a = new Int32Array([3, 1, 4, 1, 5, 9, 2, 6, -7]);
    eq(a._sum(), 24, "i32 _sum exact (int64 widening)");
    eq(a._min(), -7, "i32 _min exact");
    eq(a._max(), 9, "i32 _max exact");
    near(a._mean(), 24 / 9, "i32 _mean");
}
/* ---- Float32Array (SIMD f32 sum/min/max) ---- */
{
    const a = new Float32Array([1, 2, 3, 4, 5]);
    near(a._sum(), 15, "f32 _sum", 1e-5);
    eq(a._max(), 5, "f32 _max");
    eq(a._min(), 1, "f32 _min");
}
/* ---- scalar element types: Uint8/Int8/Int16/Uint16/Uint32/Float16 ---- */
eq(new Uint8Array([10, 20, 30, 250])._sum(), 310, "u8 _sum");
eq(new Uint8Array([10, 20, 30])._max(), 30, "u8 _max");
eq(new Int8Array([-5, 5, -10, 10])._min(), -10, "i8 _min");
eq(new Int16Array([100, -200, 300])._sum(), 200, "i16 _sum");
eq(new Uint16Array([1000, 2000])._max(), 2000, "u16 _max");
eq(new Uint32Array([1, 2, 3000000000])._sum(), 3000000003, "u32 _sum");
near(new Float16Array([1.5, 2.5, 3.0])._sum(), 7.0, "f16 _sum", 1e-2);

/* ---- large-array SIMD correctness (exercises the vector tail + remainder) ---- */
{
    const N = 10000, a = new Float64Array(N);
    let expect = 0;
    for (let i = 0; i < N; i++) { a[i] = (i % 7) - 3 + i * 0.001; expect += a[i]; }
    near(a._sum(), expect, "f64 _sum large (SIMD reorder within tolerance)", 1e-9);
    let mn = Infinity, mx = -Infinity;
    for (let i = 0; i < N; i++) { if (a[i] < mn) mn = a[i]; if (a[i] > mx) mx = a[i]; }
    eq(a._min(), mn, "f64 _min large (bit-exact)");
    eq(a._max(), mx, "f64 _max large (bit-exact)");
    const b = new Int32Array(N);
    let is = 0n; for (let i = 0; i < N; i++) { b[i] = (i * 131071) | 0; is += BigInt(b[i]); }
    eq(BigInt(b._sum()), is, "i32 _sum large exact");
}

/* ---- edge cases ---- */
eq(new Float64Array([])._sum(), 0, "empty _sum → 0");
eq(new Float64Array([])._min(), undefined, "empty _min → undefined");
eq(new Float64Array([])._max(), undefined, "empty _max → undefined");
assert(Number.isNaN(new Float64Array([])._mean()), "empty _mean → NaN");
{ let t = false; try { new BigInt64Array([1n, 2n])._sum(); } catch { t = true; } assert(t, "BigInt64Array _sum throws"); }

/* ---- demarcation: non-enumerable, on %TypedArray%.prototype ---- */
assert(!Object.keys(Float64Array.prototype).includes("_sum"), "_sum non-enumerable");
const TAproto = Object.getPrototypeOf(Float64Array.prototype);
assert(Object.getOwnPropertyNames(TAproto).includes("_sum"), "_sum lives on %TypedArray%.prototype (shared)");
assert(new Int8Array([1])._sum !== undefined, "inherited by every typed array");

print("test_typedarray_ext: all tests passed (" + n + " assertions)");
