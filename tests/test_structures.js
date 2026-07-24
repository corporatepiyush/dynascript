/* test_structures.js — dyna:structures (in-repo Vector + HashMap).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_structures.js
 * Prints "test_structures: all tests passed" on success; throws on failure. */

import { Vector, HashMap } from "dyna:structures";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, msg) {
    let threw = false;
    try { fn(); } catch { threw = true; }
    assert(threw, msg);
}

/* ---------------- Vector: push / get / length / iteration ---------------- */
{
    const v = new Vector();
    try {
        assert(v.length === 0, "new Vector is empty");
        assert(v.push(10) === 1, "push returns new length 1");
        assert(v.push(20) === 2, "push returns new length 2");
        assert(v.push(30) === 3, "push returns new length 3");
        assert(v.length === 3, "length is 3 after three pushes");
        assert(v.get(0) === 10 && v.get(1) === 20 && v.get(2) === 30,
               "get returns pushed values in order");
        /* iteration via length + get */
        let sum = 0;
        for (let i = 0; i < v.length; i++) sum += v.get(i);
        assert(sum === 60, "iteration sums to 60");
        assert(v.get(3) === undefined, "get past end is undefined");
        assert(v.get(999) === undefined, "get far past end is undefined");
    } finally { v.close(); }
}

/* ---------------- Vector: set / pop / bounds errors ---------------- */
{
    const v = new Vector();
    try {
        v.push(1); v.push(2); v.push(3);
        assert(v.set(1, 99) === undefined, "set returns undefined");
        assert(v.get(1) === 99, "set overwrote index 1");
        throws(() => v.set(3, 0), "set out of range throws RangeError");
        throws(() => v.set(100, 0), "set far out of range throws");
        assert(v.pop() === 3, "pop returns last element");
        assert(v.length === 2, "length drops after pop");
        assert(v.pop() === 99, "pop returns updated middle element");
        assert(v.pop() === 1, "pop returns first element");
        assert(v.length === 0, "empty after popping all");
        assert(v.pop() === undefined, "pop on empty is undefined");
    } finally { v.close(); }
}

/* ---------------- Vector: growth (force several reallocs) ---------------- */
{
    const v = new Vector();
    try {
        const N = 5000;
        for (let i = 0; i < N; i++) assert(v.push(i) === i + 1, "push grows @" + i);
        assert(v.length === N, "length after bulk push");
        let ok = true;
        for (let i = 0; i < N; i++) if (v.get(i) !== i) ok = false;
        assert(ok, "all values survive growth");
    } finally { v.close(); }
}

/* ---------------- Vector: stores objects / strings / numbers ---------------- */
{
    const v = new Vector();
    try {
        const obj = { tag: "hi" };
        v.push(obj); v.push("str"); v.push(3.5);
        assert(v.get(0) === obj, "stored object identity preserved");
        assert(v.get(0).tag === "hi", "stored object readable");
        assert(v.get(1) === "str", "stored string");
        assert(v.get(2) === 3.5, "stored number");
        const popped = v.pop();
        assert(popped === 3.5, "pop returns number");
    } finally { v.close(); }
}

/* ---------------- HashMap: set / get / has / size / overwrite ---------------- */
{
    const m = new HashMap();
    try {
        assert(m.size === 0, "new HashMap is empty");
        assert(m.set("a", 1) === m, "set returns the map (chainable)");
        m.set("b", 2).set("c", 3);
        assert(m.size === 3, "size is 3");
        assert(m.get("a") === 1 && m.get("b") === 2 && m.get("c") === 3,
               "get returns stored values");
        assert(m.has("a") === true, "has existing key");
        assert(m.has("z") === false, "has missing key is false");
        assert(m.get("z") === undefined, "get missing key is undefined");
        m.set("a", 111); /* overwrite */
        assert(m.get("a") === 111, "overwrite updates value");
        assert(m.size === 3, "overwrite does not change size");
    } finally { m.close(); }
}

/* ---------------- HashMap: delete ---------------- */
{
    const m = new HashMap();
    try {
        m.set("x", 1); m.set("y", 2);
        assert(m.delete("x") === true, "delete existing returns true");
        assert(m.has("x") === false, "deleted key gone");
        assert(m.size === 1, "size drops after delete");
        assert(m.delete("x") === false, "delete missing returns false");
        assert(m.delete("nope") === false, "delete never-present returns false");
    } finally { m.close(); }
}

/* ---------------- HashMap: many keys + collisions, stores any value ------- */
{
    const m = new HashMap();
    try {
        const N = 3000;
        for (let i = 0; i < N; i++) m.set("key" + i, i);
        assert(m.size === N, "size after bulk insert (forces rehash)");
        let ok = true;
        for (let i = 0; i < N; i++) if (m.get("key" + i) !== i) ok = false;
        assert(ok, "all values survive rehash");
        /* object / string / number values */
        const obj = { v: 42 };
        m.set("obj", obj); m.set("s", "hello"); m.set("num", 9.5);
        assert(m.get("obj") === obj, "object identity preserved");
        assert(m.get("s") === "hello", "string value");
        assert(m.get("num") === 9.5, "number value");
        assert(m.set("obj", obj) === m, "re-set same object is fine");
    } finally { m.close(); }
}

/* ---------------- closed-resource + idempotent close ---------------- */
{
    const v = new Vector();
    assert(v.closed === false, "vector open initially");
    v.push(1);
    v.close();
    assert(v.closed === true, "vector closed after close()");
    throws(() => v.push(2), "use-after-close push throws");
    throws(() => v.get(0), "use-after-close get throws");
    throws(() => v.length, "use-after-close length throws");
    v.close(); /* idempotent */
    assert(v.closed === true, "still closed after second close()");

    const m = new HashMap();
    assert(m.closed === false, "map open initially");
    m.set("k", 1);
    m.close();
    assert(m.closed === true, "map closed after close()");
    throws(() => m.get("k"), "use-after-close get throws");
    throws(() => m.set("k", 2), "use-after-close set throws");
    throws(() => m.size, "use-after-close size throws");
    m.close(); /* idempotent */
}

/* ---------------- reentrant close attack: coerce-then-close must not UAF --- */
{
    /* Vector.get: the index argument's valueOf closes the vector mid-call. */
    const v = new Vector();
    v.push(7);
    throws(() => v.get({ valueOf() { v.close(); return 0; } }),
           "vector.get coerce-then-close is caught (no UAF)");
    v.close();

    /* Vector.set: same, on the index argument. */
    const v2 = new Vector();
    v2.push(7);
    throws(() => v2.set({ valueOf() { v2.close(); return 0; } }, 1),
           "vector.set coerce-then-close is caught (no UAF)");
    v2.close();

    /* HashMap.set: the key's toString closes the map mid-call. */
    const m = new HashMap();
    m.set("a", 1);
    throws(() => m.set({ toString() { m.close(); return "b"; } }, 2),
           "hashmap.set coerce-then-close is caught (no UAF)");
    m.close();

    /* HashMap.get / delete: the key's toString closes the map. */
    const m2 = new HashMap();
    m2.set("a", 1);
    throws(() => m2.get({ toString() { m2.close(); return "a"; } }),
           "hashmap.get coerce-then-close is caught (no UAF)");
    m2.close();

    const m3 = new HashMap();
    m3.set("a", 1);
    throws(() => m3.delete({ toString() { m3.close(); return "a"; } }),
           "hashmap.delete coerce-then-close is caught (no UAF)");
    m3.close();
}

/* ---------------- finalizer path: created and never closed ---------------- */
{
    /* Leave many containers holding dup'd values unreferenced; the class
     * finalizer must free the native storage AND release every stored value.
     * (ASan validates no leak / no UAF on this path.) */
    for (let i = 0; i < 200; i++) {
        const v = new Vector();
        v.push({ i }); v.push("s" + i); v.push(i);
        const m = new HashMap();
        m.set("a", { i }); m.set("b", "s" + i); m.set("c", i);
        /* intentionally not closed */
    }
}

print("test_structures: all tests passed (" + n + " assertions)");
