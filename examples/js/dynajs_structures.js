/*
 * dynajs:structures -- native data structures from secure-c-libs, with
 * DETERMINISTIC memory management (no GC reliance).
 *
 * Requires the SCL-modules build:
 *     make CONFIG_SCL_MODULES=y
 *     ./dynajs examples/js/dynajs_structures.js
 *
 * Each native object owns a private arena; .close() (aliased .dispose())
 * frees it immediately -- O(1), no GC. The finalizer is only a safety net,
 * so production code should always close explicitly (try/finally or the
 * withResource helper below).
 */
import { Vector, HashMap } from "dynajs:structures";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }

/* Deterministic-dispose helper: runs `fn(resource)` and always closes the
 * resource, even on throw. This is the idiomatic "explicit free" pattern
 * until `using` declarations land. */
function withResource(resource, fn) {
    try { return fn(resource); }
    finally { resource.close(); }
}

/* ---- Vector: a growable array of numbers backed by a native growable array ---- */
function demo_vector() {
    return withResource(new Vector(), (v) => {
        for (let i = 0; i < 10; i++) v.push(i * i);
        assert(v.length === 10, "length");
        assert(v.get(3) === 9, "get");
        v.set(0, -1);
        assert(v.get(0) === -1, "set");
        assert(v.pop() === 81, "pop");
        // sum via get
        let sum = 0;
        for (let i = 0; i < v.length; i++) sum += v.get(i);
        return sum;
    });
}

/* ---- HashMap: int32 -> double, backed by a native hash map ---- */
function demo_hashmap() {
    return withResource(new HashMap(), (m) => {
        for (let i = 1; i <= 100; i++) m.set(i, 1 / i);
        assert(m.size === 100, "size");
        assert(Math.abs(m.get(4) - 0.25) < 1e-12, "get");
        assert(m.has(50) && !m.has(1000), "has");
        m.delete(50);
        assert(!m.has(50) && m.size === 99, "delete");
        return m.size;
    });
}

/* ---- The point: deterministic release means flat memory ---- */
function demo_deterministic_free() {
    // create + close 200k native objects; because each close() frees its
    // arena immediately, this runs in constant memory with zero GC pressure.
    for (let i = 0; i < 200000; i++) {
        const v = new Vector();
        v.push(i);
        v.close();               // explicit, immediate free
    }
    return 200000;
}

const vsum = demo_vector();
// squares 0..9, with [0] set to -1 and 81 popped: -1+1+4+9+16+25+36+49+64
assert(vsum === 203, "vector sum " + vsum);
print("Vector demo: sum =", vsum);

const msize = demo_hashmap();
print("HashMap demo: final size =", msize);

const n = demo_deterministic_free();
print("Deterministic-free demo:", n, "objects created+closed in constant memory");

/* ---- DisposableStack: standard-protocol deterministic cleanup ---- */
/* native resource objects expose [Symbol.dispose], so DisposableStack.use() (and, once
 * `using` syntax lands, `using v = new Vector()`) auto-close them in reverse
 * order at scope exit -- even on throw. */
function demo_disposable_stack() {
    const stack = new DisposableStack();
    const v = stack.use(new Vector());
    const m = stack.use(new HashMap());
    v.push(42);
    m.set(1, 42);
    const ok = v.get(0) === 42 && m.get(1) === 42;
    stack.dispose();                 // closes m then v (reverse order)
    assert(v.closed && m.closed, "DisposableStack closed both");
    return ok;
}
assert(demo_disposable_stack(), "disposable stack");
print("DisposableStack demo: both resources auto-disposed at scope exit");

// closed resources reject further use (fail fast, not silent corruption)
const dead = new Vector();
dead.close();
let threw = false;
try { dead.get(0); } catch (e) { threw = true; }
assert(threw, "use-after-close must throw");
assert(dead.closed === true, "closed flag");

print("PASS");
