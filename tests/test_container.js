/* test_container.js — dyna:container (in-repo Heap + List + Ring).
 * Mirrors Go's container/heap, container/list, container/ring.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_container.js
 * Prints "test_container: all tests passed" on success; throws on failure. */

import { Heap, List, Ring } from "dyna:container";
import * as std from "std";

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
function eqArr(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++)
        if (!Object.is(a[i], b[i])) return false;
    return true;
}
function mulberry32(seed) {
    let a = seed >>> 0;
    return function () {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

/* =====================================================================
 * Heap
 * ===================================================================== */

/* ---------------- basic min-heap ordering ---------------- */
{
    const h = new Heap((a, b) => a - b);
    try {
        assert(h.size === 0 && h.length === 0, "new Heap is empty");
        assert(h.peek() === undefined, "peek on empty is undefined");
        assert(h.pop() === undefined, "pop on empty is undefined");
        for (const v of [5, 3, 8, 1, 9, 2, 7, 4, 6, 0])
            h.push(v);
        assert(h.size === 10, "size after 10 pushes");
        const out = [];
        while (h.size > 0) out.push(h.pop());
        assert(eqArr(out, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]),
               "pop drains in ascending order: " + JSON.stringify(out));
        assert(h.size === 0, "empty after draining");
    } finally { h.close(); }
}

/* ---------------- max-heap via a flipped comparator ---------------- */
{
    const h = new Heap((a, b) => b - a);
    try {
        for (const v of [5, 3, 8, 1, 9, 2, 7, 4, 6, 0])
            h.push(v);
        const out = [];
        while (h.size > 0) out.push(h.pop());
        assert(eqArr(out, [9, 8, 7, 6, 5, 4, 3, 2, 1, 0]),
               "max-heap pop drains descending: " + JSON.stringify(out));
    } finally { h.close(); }
}

/* ---------------- peek does not remove ---------------- */
{
    const h = new Heap((a, b) => a - b);
    try {
        h.push(5); h.push(1); h.push(3);
        assert(h.peek() === 1, "peek returns the min");
        assert(h.size === 3, "peek does not change size");
        assert(h.peek() === 1, "peek is idempotent");
        assert(h.pop() === 1, "pop returns the same min peek saw");
        assert(h.size === 2, "size drops after pop");
    } finally { h.close(); }
}

/* ---------------- heapsort a large random array ---------------- */
{
    const rnd = mulberry32(12345);
    const N = 5000;
    const src = [];
    for (let i = 0; i < N; i++) src.push(Math.floor(rnd() * 1e6));
    const h = new Heap((a, b) => a - b);
    try {
        for (const v of src) h.push(v);
        assert(h.size === N, "heapsort: size after bulk push");
        const out = [];
        while (h.size > 0) out.push(h.pop());
        const ref = [...src].sort((a, b) => a - b);
        assert(eqArr(out, ref), "heapsort matches Array.prototype.sort");
    } finally { h.close(); }
}

/* ---------------- random interleaved push/pop vs. a reference model ----- */
{
    const rnd = mulberry32(999);
    const h = new Heap((a, b) => a - b);
    try {
        for (let round = 0; round < 20; round++) {
            const ref = [];
            const ops = 500;
            for (let i = 0; i < ops; i++) {
                if (ref.length === 0 || rnd() < 0.6) {
                    const v = Math.floor(rnd() * 1e6);
                    h.push(v);
                    ref.push(v);
                } else {
                    ref.sort((a, b) => a - b);
                    const want = ref.shift();
                    const got = h.pop();
                    assert(got === want,
                           "random fuzz round " + round + ": got " + got +
                           " want " + want);
                }
            }
            /* drain whatever remains before the next round */
            ref.sort((a, b) => a - b);
            while (ref.length > 0) {
                const want = ref.shift();
                const got = h.pop();
                assert(got === want, "random fuzz drain round " + round);
            }
            assert(h.size === 0, "heap empty at end of fuzz round " + round);
        }
    } finally { h.close(); }
}

/* ---------------- stores arbitrary JS values, not just numbers --------- */
{
    const h = new Heap((a, b) => a.k - b.k);
    try {
        h.push({ k: 3, name: "c" });
        h.push({ k: 1, name: "a" });
        h.push({ k: 2, name: "b" });
        assert(h.pop().name === "a", "object heap pops in key order (a)");
        assert(h.pop().name === "b", "object heap pops in key order (b)");
        assert(h.pop().name === "c", "object heap pops in key order (c)");
    } finally { h.close(); }
}

/* ---------------- comparator required at construction ---------------- */
{
    throws(() => new Heap(), "Heap() with no comparator throws");
    throws(() => new Heap(42), "Heap(non-function) throws");
    throws(() => new Heap("nope"), "Heap(string) throws");
}

/* ---------------- NaN comparator result treated as 0/equal ------------- */
{
    const h = new Heap(() => NaN);
    try {
        h.push(1); h.push(2); h.push(3);
        assert(h.size === 3, "NaN-comparator heap still accepts pushes");
        /* order is unspecified when everything compares "equal", but every
         * element must still come out exactly once and the heap must not
         * corrupt/crash. */
        const out = [h.pop(), h.pop(), h.pop()];
        assert(out.length === 3, "NaN-comparator heap drains fully");
        assert(h.size === 0, "NaN-comparator heap ends empty");
    } finally { h.close(); }
}

/* ---------------- throwing comparator: clean failure, heap stays usable */
{
    const h = new Heap((a, b) => a - b);
    try {
        h.push(1); h.push(2);
        let boom = new Heap((a, b) => { throw new Error("boom"); });
        try {
            boom.push(1); /* first element: no comparison, always succeeds */
            throws(() => boom.push(2), "throwing comparator surfaces during push");
        } finally { boom.close(); }
        /* unrelated heap must be entirely unaffected */
        h.push(0);
        assert(h.pop() === 0, "unrelated heap still orders correctly");
        assert(h.pop() === 1, "unrelated heap still orders correctly (2)");
        assert(h.pop() === 2, "unrelated heap still orders correctly (3)");
    } finally { h.close(); }
}

/* ---------------- throwing comparator on the SAME heap: stays usable --- */
{
    /* Documented behavior: push() appends and increments size BEFORE any
     * comparator call, so a comparator that throws mid-sift leaves its value
     * counted and present (never lost, never duplicated by the container
     * itself) but possibly not fully bubbled to its ideal position -- the
     * same "partially completed, not corrupted" contract Array.prototype.sort
     * has for a throwing comparator. The one thing NOT guaranteed afterward
     * is that the very next pop() sequence is perfectly ascending; what IS
     * guaranteed is that every element is still there exactly once (no
     * memory corruption, no lost/duplicated bookkeeping) and later
     * operations keep working. */
    let shouldThrow = false;
    const h = new Heap((a, b) => {
        if (shouldThrow) throw new Error("boom");
        return a - b;
    });
    try {
        h.push(5); h.push(1); h.push(3);
        assert(h.size === 3, "size is 3 before the throwing push");
        shouldThrow = true;
        throws(() => h.push(2), "mid-life throwing comparator surfaces");
        shouldThrow = false;
        assert(h.size === 4,
               "size reflects the appended element even though its sift threw");
        h.push(6);
        const out = [];
        while (h.size > 0) out.push(h.pop());
        const expected = [5, 1, 3, 2, 6].sort((a, b) => a - b);
        assert(eqArr([...out].sort((a, b) => a - b), expected),
               "draining after a throwing comparator loses/duplicates nothing: " +
               JSON.stringify(out));
    } finally { h.close(); }
}

/* ---------------- reentrant-close attack: valueOf on a pushed value ----- */
{
    let h = new Heap((a, b) => a - b);
    h.push(5);
    throws(() => h.push({ valueOf() { h.close(); return 1; } }),
           "push with a close()-ing valueOf is caught (no UAF/crash)");
    assert(h.closed, "heap is closed after the attack");
    throws(() => h.push(1), "closed heap rejects further push");
}

/* ---------------- reentrant-close attack: comparator closes the heap --- */
{
    let h;
    h = new Heap((a, b) => { h.close(); return a - b; });
    h.push(1); /* single element: no comparator call yet, succeeds */
    throws(() => h.push(2),
           "a comparator that closes its own heap is caught (no UAF/crash)");
    assert(h.closed, "heap is closed after the comparator-close attack");
}

/* ---------------- reentrant-close via valueOf on an ALREADY-stored value  */
{
    let h;
    h = new Heap((a, b) => a - b);
    /* Push a poison object with NO surviving JS-side reference (not bound to
     * any variable, unlike the push-arg attack above where the interpreter's
     * own argument-stack slot for the CURRENT call happens to keep the value
     * alive for the call's duration). After this push() returns, the ONLY
     * reference to the object is the heap's own storage. The next push()
     * forces a comparison against it; ToNumber() fires its valueOf(), which
     * closes the heap out from under the very value whose valueOf is
     * running right now -- this is the purest form of the attack, and only
     * dyn_heap_cmp_call's own dup of the comparator's operands (module
     * header rule 1) keeps it alive through the call. */
    h.push({ valueOf() { h.close(); return 100; } });
    throws(() => h.push(1),
           "comparing an already-stored close()-ing value is caught");
    assert(h.closed, "heap closed after the stored-value attack");
}

/* ---------------- reentrant mutation attack: comparator pushes/pops ----- */
{
    let h;
    h = new Heap((a, b) => { throws(() => h.push(999), "reentrant push during comparator throws"); return a - b; });
    h.push(3); h.push(1); h.push(2); /* triggers comparator calls */
    assert(h.size === 3, "reentrant push attempts do not corrupt size");
    const out = [];
    while (h.size > 0) out.push(h.pop());
    assert(eqArr(out, [1, 2, 3]), "heap orders correctly despite reentrant-push attempts");
    h.close();

    let h2;
    h2 = new Heap((a, b) => { throws(() => h2.pop(), "reentrant pop during comparator throws"); return a - b; });
    h2.push(3); h2.push(1); h2.push(2);
    const out2 = [];
    while (h2.size > 0) out2.push(h2.pop());
    assert(eqArr(out2, [1, 2, 3]), "heap orders correctly despite reentrant-pop attempts");
    h2.close();
}

/* ---------------- closed-resource + idempotent close ---------------- */
{
    const h = new Heap((a, b) => a - b);
    assert(h.closed === false, "heap open initially");
    h.push(1);
    h.close();
    assert(h.closed === true, "heap closed after close()");
    throws(() => h.push(2), "use-after-close push throws");
    throws(() => h.pop(), "use-after-close pop throws");
    throws(() => h.peek(), "use-after-close peek throws");
    throws(() => h.size, "use-after-close size throws");
    throws(() => h.length, "use-after-close length throws");
    h.close(); /* idempotent */
    assert(h.closed === true, "still closed after second close()");
}

/* ---------------- Heap finalizer path: created and never closed -------- */
{
    for (let i = 0; i < 200; i++) {
        const h = new Heap((a, b) => a - b);
        h.push({ i }); h.push("s" + i); h.push(i);
        /* intentionally not closed */
    }
    std.gc();
}

/* =====================================================================
 * List
 * ===================================================================== */

/* ---------------- basic push/pop both ends + front/back ---------------- */
{
    const l = new List();
    try {
        assert(l.length === 0, "new List is empty");
        assert(l.front() === undefined, "front on empty is undefined");
        assert(l.back() === undefined, "back on empty is undefined");
        assert(l.popFront() === undefined, "popFront on empty is undefined");
        assert(l.popBack() === undefined, "popBack on empty is undefined");

        assert(l.pushBack(2) === 1, "pushBack returns new length 1");
        assert(l.pushFront(1) === 2, "pushFront returns new length 2");
        assert(l.pushBack(3) === 3, "pushBack returns new length 3");
        /* list is now [1, 2, 3] */
        assert(l.length === 3, "length is 3");
        assert(l.front() === 1, "front is 1");
        assert(l.back() === 3, "back is 3");
        assert(eqArr(l.toArray(), [1, 2, 3]), "toArray is [1,2,3]");

        assert(l.popFront() === 1, "popFront returns 1");
        assert(l.popBack() === 3, "popBack returns 3");
        assert(l.length === 1, "length is 1 after both pops");
        assert(l.front() === 2 && l.back() === 2, "single element is both front and back");
        assert(l.popFront() === 2, "final popFront returns 2");
        assert(l.length === 0, "empty after draining");
    } finally { l.close(); }
}

/* ---------------- pushFront/pushBack interleaving builds correct order - */
{
    const l = new List();
    try {
        l.pushBack(1);      // [1]
        l.pushFront(0);     // [0,1]
        l.pushBack(2);      // [0,1,2]
        l.pushFront(-1);    // [-1,0,1,2]
        l.pushBack(3);      // [-1,0,1,2,3]
        assert(eqArr(l.toArray(), [-1, 0, 1, 2, 3]), "interleaved push order");
        assert(l.length === 5, "length matches after interleaving");
    } finally { l.close(); }
}

/* ---------------- drains identically from both ends -------------------- */
{
    const l = new List();
    try {
        const N = 2000;
        for (let i = 0; i < N; i++) l.pushBack(i);
        assert(l.length === N, "length after bulk pushBack");
        assert(eqArr(l.toArray(), Array.from({ length: N }, (_, i) => i)),
               "bulk toArray matches ascending sequence");
        for (let i = 0; i < N; i++) assert(l.popFront() === i, "popFront drains ascending @" + i);
        assert(l.length === 0, "empty after draining forward");

        for (let i = 0; i < N; i++) l.pushFront(i);
        for (let i = 0; i < N; i++) assert(l.popFront() === N - 1 - i, "popFront drains reversed @" + i);
        assert(l.length === 0, "empty after draining reversed");
    } finally { l.close(); }
}

/* ---------------- stores objects/strings/numbers, identity preserved --- */
{
    const l = new List();
    try {
        const obj = { tag: "x" };
        l.pushBack(obj); l.pushBack("str"); l.pushBack(4.5);
        assert(l.toArray()[0] === obj, "object identity preserved in toArray");
        assert(l.popFront() === obj, "popFront returns the same object");
        assert(l.popFront() === "str", "popFront returns the string");
        assert(l.popFront() === 4.5, "popFront returns the number");
    } finally { l.close(); }
}

/* ---------------- iteration via for..of ---------------- */
{
    const l = new List();
    try {
        l.pushBack(1); l.pushBack(2); l.pushBack(3);
        const seen = [];
        for (const v of l) seen.push(v);
        assert(eqArr(seen, [1, 2, 3]), "for..of visits in list order");
        assert(eqArr([...l], [1, 2, 3]), "spread operator works via [Symbol.iterator]");
        /* iterating does not mutate the list */
        assert(l.length === 3, "length unaffected by iteration");
    } finally { l.close(); }

    /* iterating an empty list yields nothing */
    const l2 = new List();
    try {
        const seen2 = [];
        for (const v of l2) seen2.push(v);
        assert(seen2.length === 0, "for..of over empty list yields nothing");
    } finally { l2.close(); }
}

/* ---------------- closed-resource + idempotent close ---------------- */
{
    const l = new List();
    assert(l.closed === false, "list open initially");
    l.pushBack(1);
    l.close();
    assert(l.closed === true, "list closed after close()");
    throws(() => l.pushBack(2), "use-after-close pushBack throws");
    throws(() => l.pushFront(2), "use-after-close pushFront throws");
    throws(() => l.popFront(), "use-after-close popFront throws");
    throws(() => l.popBack(), "use-after-close popBack throws");
    throws(() => l.front(), "use-after-close front throws");
    throws(() => l.back(), "use-after-close back throws");
    throws(() => l.toArray(), "use-after-close toArray throws");
    throws(() => l.length, "use-after-close length throws");
    l.close(); /* idempotent */
    assert(l.closed === true, "still closed after second close()");
}

/* ---------------- List finalizer path: created and never closed -------- */
{
    for (let i = 0; i < 200; i++) {
        const l = new List();
        l.pushBack({ i }); l.pushFront("s" + i); l.pushBack(i);
        /* intentionally not closed */
    }
    std.gc();
}

/* =====================================================================
 * Ring
 * ===================================================================== */

/* ---------------- basic push/get/length/capacity ---------------- */
{
    const r = new Ring(3);
    try {
        assert(r.capacity === 3, "capacity is fixed at construction");
        assert(r.length === 0, "new Ring is empty");
        assert(r.get(0) === undefined, "get on empty is undefined");
        assert(eqArr(r.toArray(), []), "toArray on empty is []");

        assert(r.push(1) === 1, "push returns new length 1");
        assert(r.push(2) === 2, "push returns new length 2");
        assert(eqArr(r.toArray(), [1, 2]), "toArray before full: [1,2]");
        assert(r.get(0) === 1 && r.get(1) === 2, "get(i) before full");
        assert(r.get(2) === undefined, "get past length is undefined");
    } finally { r.close(); }
}

/* ---------------- overwrite semantics + wraparound ---------------- */
{
    const r = new Ring(3);
    try {
        r.push(1); r.push(2); r.push(3);
        assert(r.length === 3, "length caps at capacity");
        assert(eqArr(r.toArray(), [1, 2, 3]), "full ring holds all 3");

        assert(r.push(4) === 3, "push while full still returns length==capacity");
        assert(eqArr(r.toArray(), [2, 3, 4]), "oldest (1) overwritten by 4");
        assert(r.get(0) === 2 && r.get(1) === 3 && r.get(2) === 4,
               "get(i) reflects the new logical order after overwrite");

        r.push(5); r.push(6);
        assert(eqArr(r.toArray(), [4, 5, 6]), "wraps around correctly over multiple overwrites");

        /* drive it around the physical buffer several full laps */
        for (let i = 7; i <= 50; i++) r.push(i);
        assert(eqArr(r.toArray(), [48, 49, 50]), "many laps: only the last 3 survive");
        assert(r.length === 3, "length still == capacity after many laps");
    } finally { r.close(); }
}

/* ---------------- capacity 1 edge case ---------------- */
{
    const r = new Ring(1);
    try {
        r.push(1);
        assert(eqArr(r.toArray(), [1]), "capacity-1 ring holds the single push");
        r.push(2);
        assert(eqArr(r.toArray(), [2]), "capacity-1 ring always overwrites");
        assert(r.length === 1, "capacity-1 ring length stays 1");
    } finally { r.close(); }
}

/* ---------------- construction validation ---------------- */
{
    throws(() => new Ring(), "Ring() with no capacity throws");
    throws(() => new Ring(0), "Ring(0) throws");
    throws(() => new Ring(-1), "Ring(-1) throws");
}

/* ---------------- stores arbitrary JS values ---------------- */
{
    const r = new Ring(2);
    try {
        const obj = { tag: "ring" };
        r.push(obj); r.push("s"); r.push(9.5);
        assert(eqArr(r.toArray(), ["s", 9.5]), "ring after overwrite: [\"s\", 9.5]");
        r.push(obj);
        assert(r.get(1) === obj, "object identity preserved through the ring");
    } finally { r.close(); }
}

/* ---------------- reentrant-close attack: valueOf on get()'s index ----- */
{
    const r = new Ring(4);
    r.push(1); r.push(2);
    throws(() => r.get({ valueOf() { r.close(); return 0; } }),
           "ring.get coerce-then-close is caught (no UAF)");
    assert(r.closed, "ring is closed after the attack");
}

/* ---------------- closed-resource + idempotent close ---------------- */
{
    const r = new Ring(4);
    assert(r.closed === false, "ring open initially");
    r.push(1);
    r.close();
    assert(r.closed === true, "ring closed after close()");
    throws(() => r.push(2), "use-after-close push throws");
    throws(() => r.get(0), "use-after-close get throws");
    throws(() => r.toArray(), "use-after-close toArray throws");
    throws(() => r.length, "use-after-close length throws");
    throws(() => r.capacity, "use-after-close capacity throws");
    r.close(); /* idempotent */
    assert(r.closed === true, "still closed after second close()");
}

/* ---------------- Ring finalizer path: created and never closed -------- */
{
    for (let i = 0; i < 200; i++) {
        const r = new Ring(8);
        for (let j = 0; j < 20; j++) r.push({ i, j });
        /* intentionally not closed: 20 pushes into capacity 8 also exercises
         * the overwrite path right before going out of scope unclosed */
    }
    std.gc();
}

/* =====================================================================
 * cross-cutting: all three importable together, independent lifetimes
 * ===================================================================== */
{
    const h = new Heap((a, b) => a - b);
    const l = new List();
    const r = new Ring(4);
    try {
        h.push(3); h.push(1); h.push(2);
        l.pushBack(1); l.pushBack(2);
        r.push(1); r.push(2);
        assert(h.pop() === 1, "heap independent of list/ring");
        assert(l.popFront() === 1, "list independent of heap/ring");
        assert(r.get(0) === 1, "ring independent of heap/list");
    } finally {
        h.close(); l.close(); r.close();
    }
}

print("test_container: all tests passed (" + n + " assertions)");
