/* test_simd.js — scl:simd float32 vector kernels.
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_simd.js */
import { dot, sum, scale, axpy, add } from "scl:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
const near = (a, b, eps) => Math.abs(a - b) <= (eps || Math.max(1, Math.abs(b)) * 1e-4);

/* correctness vs JS reference over sizes that exercise the SIMD body + scalar tail */
for (const len of [0, 1, 3, 4, 7, 8, 15, 16, 100, 1000]) {
    const a = Float32Array.from({ length: len }, (_, i) => Math.fround((i % 17) - 8.5));
    const b = Float32Array.from({ length: len }, (_, i) => Math.fround((i % 13) - 6.25));
    let d = 0, s = 0;
    for (let i = 0; i < len; i++) { d += a[i] * b[i]; s += a[i]; }
    assert(near(dot(a, b), d), "dot len=" + len);
    assert(near(sum(a), s), "sum len=" + len);

    const y = Float32Array.from(a), x = Float32Array.from(b);
    const r = axpy(y, 2.5, x);
    assert(r === y, "axpy returns y");
    for (let i = 0; i < len; i++) assert(near(y[i], Math.fround(a[i] + 2.5 * b[i])), "axpy[" + i + "]");

    const c = Float32Array.from(a); scale(c, -3);
    for (let i = 0; i < len; i++) assert(near(c[i], Math.fround(a[i] * -3)), "scale[" + i + "]");

    const o = new Float32Array(len); add(o, a, b);
    for (let i = 0; i < len; i++) assert(near(o[i], Math.fround(a[i] + b[i])), "add[" + i + "]");
}

/* type + shape errors */
{
    let t = false; try { dot(new Float32Array(3), new Float32Array(4)); } catch { t = true; }
    assert(t, "length mismatch throws");
    t = false; try { dot(new Uint8Array(4), new Uint8Array(4)); } catch { t = true; }
    assert(t, "non-Float32Array throws");
    t = false; try { sum([1, 2, 3]); } catch { t = true; }
    assert(t, "plain array throws");
}

/* reentrancy: a scalar arg whose valueOf mutates must not corrupt (coerced first) */
{
    const a = new Float32Array([1, 2, 3, 4]);
    let calls = 0;
    scale(a, { valueOf() { calls++; return 2; } });
    assert(calls === 1 && a[0] === 2 && a[3] === 8, "scale coerces scalar first");
}

print("test_simd: all tests passed (" + n + " assertions)");
