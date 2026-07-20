/*
 * scl:sort -- native sorting + binary search from secure-c-libs.
 *
 * Requires the SCL-modules build:
 *     make CONFIG_SCL_MODULES=y
 *     ./dynajs examples/js/scl_sort.js
 *
 * These are TRANSIENT functions, not resource objects: each call spins up a
 * private arena, copies the whole input into it, sorts/searches natively, copies
 * the result back into fresh JS values, and destroys the arena before returning.
 * Nothing to .close() -- peak memory stays flat across any number of calls.
 */
import { sort, binarySearch } from "scl:sort";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }

/* ---- sort(): ascending order, input left untouched ---- */
function demo_sort() {
    const input = [3, 1, 4, 1, 5, 9, 2, 6];
    const s = sort(input);
    assert(JSON.stringify(s) === "[1,1,2,3,4,5,6,9]", "ascending order");
    assert(s !== input, "sort returns a FRESH Array");
    assert(JSON.stringify(input) === "[3,1,4,1,5,9,2,6]", "input is NOT mutated");
    return s;
}

/* ---- byte-identical order vs JS's own numeric sort, for finite values ---- */
function demo_matches_js() {
    for (let t = 0; t < 1000; t++) {
        const a = Array.from({ length: (Math.random() * 20) | 0 },
                             () => Math.random() * 1e6 - 5e5);
        const mine = sort(a);
        const ref = a.slice().sort((x, y) => x - y);
        assert(JSON.stringify(mine) === JSON.stringify(ref), "differs from JS sort");
    }
    return 1000;
}

/* ---- binarySearch(): index of a match, or -1 ---- */
function demo_search() {
    const s = sort([50, 10, 40, 20, 30]);          // -> [10,20,30,40,50]
    assert(binarySearch(s, 30) === 2, "hit");
    assert(binarySearch(s, 35) === -1, "miss");
    assert(binarySearch(s, 10) === 0, "first");
    assert(binarySearch(s, 50) === 4, "last");
    assert(binarySearch([], 1) === -1, "empty");
    return s;
}

/* ---- NaN has a defined total order here: it always sorts LAST ---- */
function demo_nan() {
    const s = sort([3, NaN, 1, NaN, 2]);
    const finite = s.filter((x) => x === x);
    assert(JSON.stringify(finite) === "[1,2,3]", "finite part sorted");
    assert(s.slice(-2).every((x) => x !== x), "NaNs pushed to the end");
    // +/-Infinity are ordinary extremes (not NaN): they bound the finite values
    const s2 = sort([Infinity, 0, -Infinity, NaN]);
    assert(s2[0] === -Infinity && s2[2] === Infinity && s2[3] !== s2[3],
           "-Inf < finite < +Inf < NaN");
    return s;
}

print("sort demo:", JSON.stringify(demo_sort()));
print("matches JS numeric sort on", demo_matches_js(), "random arrays");
print("binarySearch demo:", JSON.stringify(demo_search()));
print("NaN sorts last:", JSON.stringify(demo_nan().map((x) => (x === x ? x : "NaN"))));

print("PASS");
