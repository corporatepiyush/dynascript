/* test_array_ext.js — SugarJS/RamdaJS `_`-prefixed native Array methods.
 * Phase 1, batch 1. Run: dynajs tests/test_array_ext.js
 * These are core-engine builtins (present in every build). */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

/* _first / _last */
eq([1, 2, 3, 4, 5]._first(), 1, "_first() → first element");
eq([1, 2, 3, 4, 5]._first(2), [1, 2], "_first(n) → first n");
eq([1, 2, 3]._first(9), [1, 2, 3], "_first(n>len) clamps");
eq([1, 2, 3]._first(0), [], "_first(0) → []");
eq([]._first(), undefined, "_first() of empty → undefined");
eq([1, 2, 3, 4, 5]._last(), 5, "_last() → last element");
eq([1, 2, 3, 4, 5]._last(2), [4, 5], "_last(n) → last n in order");
eq([1, 2, 3]._last(9), [1, 2, 3], "_last(n>len) clamps");
eq([]._last(), undefined, "_last() of empty → undefined");

/* _sum / _average / _mean */
eq([1, 2, 3, 4, 5]._sum(), 15, "_sum");
eq([]._sum(), 0, "_sum of empty → 0");
eq([1, 2, 3, 4]._average(), 2.5, "_average");
eq([1, 2, 3, 4]._mean(), 2.5, "_mean alias");
eq([]._average(), 0, "_average of empty → 0");
eq(["1", "2", "3"]._sum(), 6, "_sum coerces numeric strings");

/* _compact */
eq([1, null, 2, undefined, 3]._compact(), [1, 2, 3], "_compact removes null/undefined");
eq([0, false, "", NaN]._compact(), [0, false, "", NaN], "_compact keeps other falsy");
eq([]._compact(), [], "_compact of empty → []");

/* _isEmpty */
assert([]._isEmpty() === true, "_isEmpty of []");
assert([1]._isEmpty() === false, "_isEmpty of non-empty");

/* demarcation: `_` methods are NON-ENUMERABLE (for..in / Object.keys unaffected) */
assert(!Object.keys(Array.prototype).includes("_sum"), "_sum is non-enumerable");
let seen = []; for (const k in [1, 2, 3]) seen.push(k);
eq(seen, ["0", "1", "2"], "for..in over an array sees only indices, not _methods");
assert(Object.getOwnPropertyNames(Array.prototype).includes("_sum"), "_sum is an own property (present)");
/* works on array-likes via `this` */
eq(Array.prototype._first.call({ 0: "a", 1: "b", length: 2 }, 2), ["a", "b"], "_first on an array-like");

/* a re-entrant valueOf arg must not corrupt the result */
eq([1, 2, 3]._first({ valueOf() { return 2; } }), [1, 2], "_first coerces an object arg");

print("test_array_ext: all tests passed (" + n + " assertions)");
