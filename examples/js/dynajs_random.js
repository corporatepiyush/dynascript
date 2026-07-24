/*
 * dyna:random -- native random number generation from secure-c-libs, with
 * DETERMINISTIC memory management (no GC reliance).
 *
 * Requires the SCL-modules build:
 *     make CONFIG_SCL_MODULES=y CONFIG_SCL_MODULE_RANDOM=y
 *     ./dynajs examples/js/dynajs_random.js
 *
 * Random is an arena-per-object PRNG (xoshiro256**): a private arena holds its
 * state and .close() (aliased .dispose()) frees it immediately -- O(1), no GC.
 * uuid() is a plain function (RFC 4122 v4). Seeded Randoms are deterministic;
 * an unseeded Random draws its seed from the system CSPRNG.
 */
import { Random, uuid } from "dyna:random";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }

/* Deterministic-dispose helper: always closes the resource, even on throw. */
function withResource(resource, fn) {
    try { return fn(resource); }
    finally { resource.close(); }
}

/* ---- API tour ---- */
withResource(new Random(0xC0FFEE), (r) => {
    const big = r.nextU64();        // BigInt in [0, 2^64)   (full 64-bit)
    const n53 = r.nextU53();        // Number in [0, 2^53)
    const f = r.nextFloat();        // Number in [0, 1)
    const die = r.nextBounded(6);   // Number in [0, 6)      (unbiased)
    const bytes = r.fill(new Uint8Array(16)); // random bytes in place
    assert(typeof big === "bigint", "nextU64 is a BigInt");
    assert(typeof n53 === "number" && n53 >= 0 && n53 < 2 ** 53, "nextU53 range");
    assert(f >= 0 && f < 1, "nextFloat range");
    assert(die >= 0 && die < 6, "nextBounded range");
    assert(bytes.length === 16, "fill returns the view");
    print("nextU64:", big.toString(), "| nextFloat:", f.toFixed(6), "| d6:", die);
});

/* ---- Determinism: same seed -> identical sequence ---- */
function firstN(seed, n) {
    return withResource(new Random(seed), (r) => {
        const out = [];
        for (let i = 0; i < n; i++) out.push(r.nextU64());
        return out;
    });
}
const seqA = firstN(42, 8), seqB = firstN(42, 8);
assert(seqA.every((v, i) => v === seqB[i]), "same seed -> same sequence");
assert(firstN(43, 8).some((v, i) => v !== seqA[i]), "distinct seed -> distinct");
print("Determinism: two Random(42) produced identical 8-draw sequences");

/* ---- uuid(): RFC 4122 v4 ---- */
const id = uuid();
assert(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(id),
       "uuid is canonical v4");
print("uuid:", id);

/* ---- The point: deterministic release means flat memory ---- */
for (let i = 0; i < 200000; i++) {
    const r = new Random(i);
    r.nextU64();
    r.close();                       // explicit, immediate arena free
}
print("Deterministic-free: 200000 Randoms created+closed in constant memory");

/* ---- DisposableStack / [Symbol.dispose] ---- */
{
    const stack = new DisposableStack();
    const r = stack.use(new Random(1));
    r.nextU64();
    stack.dispose();
    assert(r.closed, "DisposableStack disposed the Random");
}

/* ---- use-after-close fails fast ---- */
const dead = new Random(1);
dead.close();
let threw = false;
try { dead.nextU64(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "use-after-close must throw TypeError");
assert(dead.closed === true, "closed flag");

print("PASS");
