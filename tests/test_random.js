/* test_random.js — dynajs:random (in-repo xoshiro256** + uuid v4).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_random.js
 * Prints "test_random: all tests passed" on success; throws on failure. */

import { Random, uuid } from "dynajs:random";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

/* --- determinism: same seed => same stream (Number and BigInt seed alias) --- */
{
    const a = new Random(42);
    const b = new Random(42n);
    try {
        for (let i = 0; i < 100; i++)
            assert(a.nextU64() === b.nextU64(), "42 and 42n produce same stream @" + i);
    } finally { a.close(); b.close(); }
}

/* --- different seeds diverge --- */
{
    const a = new Random(1), b = new Random(2);
    try {
        let same = 0;
        for (let i = 0; i < 50; i++) if (a.nextU64() === b.nextU64()) same++;
        assert(same < 3, "distinct seeds should rarely collide (" + same + ")");
    } finally { a.close(); b.close(); }
}

/* --- types and ranges --- */
{
    const r = new Random(7);
    try {
        assert(typeof r.nextU64() === "bigint", "nextU64 is a BigInt");
        const u53 = r.nextU53();
        assert(typeof u53 === "number" && Number.isInteger(u53), "nextU53 integer");
        assert(u53 >= 0 && u53 < 2 ** 53, "nextU53 in [0,2^53)");
        for (let i = 0; i < 1000; i++) {
            const f = r.nextFloat();
            assert(f >= 0 && f < 1, "nextFloat in [0,1)");
        }
        for (let i = 0; i < 1000; i++) {
            const d = r.nextBounded(6);
            assert(typeof d === "number" && d >= 0 && d < 6, "d6 in [0,6)");
        }
        assert(typeof r.nextBounded(10n) === "bigint", "BigInt bound => BigInt");
    } finally { r.close(); }
}

/* --- bound validation --- */
{
    const r = new Random(1);
    try {
        let threw = false;
        try { r.nextBounded(0); } catch { threw = true; }
        assert(threw, "nextBounded(0) throws RangeError");
        threw = false;
        try { r.nextBounded(-5); } catch { threw = true; }
        assert(threw, "nextBounded(-5) throws");
    } finally { r.close(); }
}

/* --- fill() writes bytes into a JS-owned buffer, deterministically --- */
{
    const a = new Random(99), b = new Random(99);
    try {
        const x = new Uint8Array(32), y = new Uint8Array(32);
        a.fill(x); b.fill(y);
        let allZero = true, equal = true;
        for (let i = 0; i < 32; i++) {
            if (x[i] !== 0) allZero = false;
            if (x[i] !== y[i]) equal = false;
        }
        assert(!allZero, "fill wrote non-zero bytes");
        assert(equal, "fill is deterministic for the same seed");
    } finally { a.close(); b.close(); }
}

/* --- distribution smoke: nextBounded(2) is roughly balanced --- */
{
    const r = new Random(123);
    try {
        let ones = 0;
        for (let i = 0; i < 10000; i++) ones += r.nextBounded(2);
        assert(ones > 4500 && ones < 5500, "coin flips ~balanced (" + ones + "/10000)");
    } finally { r.close(); }
}

/* --- closed-resource semantics --- */
{
    const r = new Random(1);
    assert(r.closed === false, "open initially");
    r.close();
    assert(r.closed === true, "closed after close()");
    let threw = false;
    try { r.nextU64(); } catch { threw = true; }
    assert(threw, "use-after-close throws");
    r.close(); // idempotent
}

/* --- reentrant close attack: valueOf that closes must not crash/UAF --- */
{
    const r = new Random(1);
    let threw = false;
    try {
        r.nextBounded({ valueOf() { r.close(); return 6; } });
    } catch { threw = true; }
    assert(threw, "coerce-then-close is caught (no UAF)");
    r.close();
}

/* --- uuid(): shape + version/variant + uniqueness --- */
{
    const re = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
    const seen = new Set();
    for (let i = 0; i < 1000; i++) {
        const id = uuid();
        assert(re.test(id), "uuid v4 shape: " + id);
        assert(!seen.has(id), "uuid unique");
        seen.add(id);
    }
}

print("test_random: all tests passed (" + n + " assertions)");
