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

/* ---- batch 2: _count / _none / _any / _all (matcher variants) ---- */
/* _count variants: no-arg → length; value (SameValueZero); predicate fn */
eq([1, 2, 2, 3, 2]._count(), 5, "_count() → length");
eq([1, 2, 2, 3, 2]._count(2), 3, "_count(value) counts equal");
eq([1, 2, 3, 4, 5]._count(x => x % 2 === 0), 2, "_count(fn) counts predicate");
eq([NaN, 1, NaN]._count(NaN), 2, "_count(NaN) uses SameValueZero");
eq([]._count(x => true), 0, "_count of empty → 0");
/* _none / _any / _all — value variant */
assert([1, 2, 3]._none(4) === true, "_none(value) true when absent");
assert([1, 2, 3]._none(2) === false, "_none(value) false when present");
assert([1, 2, 3]._any(2) === true, "_any(value) true when present");
assert([1, 2, 3]._any(9) === false, "_any(value) false when absent");
/* _none / _any / _all — predicate variant */
assert([2, 4, 6]._all(x => x % 2 === 0) === true, "_all(fn) true");
assert([2, 4, 5]._all(x => x % 2 === 0) === false, "_all(fn) false");
assert([1, 3, 5]._none(x => x % 2 === 0) === true, "_none(fn) true");
assert([1, 3, 4]._any(x => x % 2 === 0) === true, "_any(fn) true");
/* empty-array quantifiers (vacuous truth) */
assert([]._all(x => false) === true, "_all of empty → true (vacuous)");
assert([]._any(x => true) === false, "_any of empty → false");
assert([]._none(x => true) === true, "_none of empty → true");

/* ---- batch 2: _min / _max (mapper variants) ---- */
eq([3, 1, 4, 1, 5]._min(), 1, "_min() numeric");
eq([3, 1, 4, 1, 5]._max(), 5, "_max() numeric");
eq([]._min(), undefined, "_min() of empty → undefined");
eq([]._max(), undefined, "_max() of empty → undefined");
eq([{ a: 3 }, { a: 1 }, { a: 2 }]._min("a"), { a: 1 }, "_min(prop) returns the element");
eq([{ a: 3 }, { a: 1 }, { a: 2 }]._max(p => p.a), { a: 3 }, "_max(fn) returns the element");
eq([{ a: 1, id: "x" }, { a: 1, id: "y" }]._min("a"), { a: 1, id: "x" }, "_min ties → first");

/* ---- batch 2: _take / _drop / _takeLast / _dropLast ---- */
const b = [1, 2, 3, 4, 5];
eq(b._take(2), [1, 2], "_take(n)");
eq(b._take(0), [], "_take(0) → []");
eq(b._take(99), [1, 2, 3, 4, 5], "_take(n>len) clamps");
eq(b._take(-1), [], "_take(-1) → []");
eq(b._drop(2), [3, 4, 5], "_drop(n)");
eq(b._drop(99), [], "_drop(n>len) → []");
eq(b._takeLast(2), [4, 5], "_takeLast(n)");
eq(b._takeLast(99), [1, 2, 3, 4, 5], "_takeLast(n>len) clamps");
eq(b._dropLast(2), [1, 2, 3], "_dropLast(n)");
eq(b._dropLast(99), [], "_dropLast(n>len) → []");
eq([]._take(3), [], "_take of empty → []");
/* _take/_drop do not mutate the receiver */
b._drop(2); eq(b, [1, 2, 3, 4, 5], "_drop does not mutate");

/* ---- batch 3: _sortBy (mapper + desc + stability variants) ---- */
eq([3, 1, 2, 10]._sortBy(), [1, 2, 3, 10], "_sortBy() numeric (not lexical)");
eq(["banana", "apple", "cherry"]._sortBy(), ["apple", "banana", "cherry"], "_sortBy() string");
eq([{ a: 3 }, { a: 1 }, { a: 2 }]._sortBy("a").map(x => x.a), [1, 2, 3], "_sortBy(prop)");
eq([{ a: 3 }, { a: 1 }]._sortBy(x => x.a).map(x => x.a), [1, 3], "_sortBy(fn)");
eq([1, 2, 3]._sortBy(undefined, true), [3, 2, 1], "_sortBy(_, desc)");
/* stability: equal keys keep original order, asc AND desc */
const st = [{ k: 1, id: "a" }, { k: 0, id: "b" }, { k: 1, id: "c" }, { k: 0, id: "d" }];
eq(st._sortBy("k").map(x => x.id), ["b", "d", "a", "c"], "_sortBy stable asc");
eq(st._sortBy("k", true).map(x => x.id), ["a", "c", "b", "d"], "_sortBy stable desc");
eq([]._sortBy(), [], "_sortBy of empty → []");
const so = [3, 1, 2]; so._sortBy(); eq(so, [3, 1, 2], "_sortBy does not mutate");

/* ---- batch 3: _groupBy (mapper variants) ---- */
eq([1, 2, 3, 4, 5, 6]._groupBy(x => x % 2 ? "odd" : "even"), { odd: [1, 3, 5], even: [2, 4, 6] }, "_groupBy(fn)");
eq([{ t: "x", v: 1 }, { t: "y", v: 2 }, { t: "x", v: 3 }]._groupBy("t"),
   { x: [{ t: "x", v: 1 }, { t: "x", v: 3 }], y: [{ t: "y", v: 2 }] }, "_groupBy(prop) preserves order");
eq([]._groupBy(x => x), {}, "_groupBy of empty → {}");

/* ---- batch 3: _shuffle (invariants — randomised) ---- */
const src = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
const shuf = src._shuffle();
assert(shuf.length === src.length, "_shuffle preserves length");
eq([...shuf].sort((a, b) => a - b), src, "_shuffle preserves the multiset");
eq(src, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10], "_shuffle does not mutate the original");
eq([]._shuffle(), [], "_shuffle of empty → []");
eq([7]._shuffle(), [7], "_shuffle of one → same");

/* ---- batch 3: _sample (single + n variants) ---- */
assert(src.includes(src._sample()), "_sample() → an element of the array");
assert([]._sample() === undefined, "_sample() of empty → undefined");
const s3 = src._sample(3);
assert(s3.length === 3 && new Set(s3).size === 3, "_sample(n) → n distinct elements");
assert(s3.every(x => src.includes(x)), "_sample(n) elements come from the array");
assert(src._sample(99).length === src.length, "_sample(n>len) → all");
eq(src._sample(0), [], "_sample(0) → []");
eq(src, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10], "_sample does not mutate the original");

print("test_array_ext: all tests passed (" + n + " assertions)");
