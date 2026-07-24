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

/* ---- batch 4: _unique / _uniq / _uniqBy (SameValueZero + mapper variants) ---- */
eq([1, 2, 2, 3, 1, 4, 3]._unique(), [1, 2, 3, 4], "_unique dedup, first-occurrence order");
eq(["a", "b", "a", "c", "b"]._unique(), ["a", "b", "c"], "_unique strings by content");
eq([1, 1.0, 2]._unique(), [1, 2], "_unique: 1 and 1.0 are SameValueZero-equal");
{ const u = [0, -0, 1]._unique(); assert(u.length === 2 && Object.is(u[0], 0), "_unique: -0 and +0 dedup to +0"); }
{ const u = [NaN, NaN, 1]._unique(); assert(u.length === 2 && Number.isNaN(u[0]), "_unique: NaN dedups (SameValueZero)"); }
{ const a = {}, b = {}; eq([a, b, a]._unique().length, 2, "_unique objects by identity"); }
eq([1, 2, 3]._uniq(), [1, 2, 3], "_uniq alias");
eq([{ id: 1 }, { id: 2 }, { id: 1 }]._uniqBy(x => x.id).map(o => o.id), [1, 2], "_uniqBy(fn) keeps first");
eq([{ t: "a" }, { t: "b" }, { t: "a" }]._unique("t").map(o => o.t), ["a", "b"], "_unique(prop)");
eq([]._unique(), [], "_unique of empty → []");

/* ---- batch 4: _intersect / _intersection / _difference / _without / _union ---- */
eq([1, 2, 3, 4]._intersect([2, 4, 6]), [2, 4], "_intersect");
eq([1, 2, 3]._intersection([2, 3, 9]), [2, 3], "_intersection alias");
eq([2, 2, 3]._intersect([2, 3]), [2, 3], "_intersect dedups the result");
eq([1, 2, 3]._intersect([]), [], "_intersect with empty → []");
eq([1, 2, 3, 4]._difference([2, 4]), [1, 3], "_difference");
eq([1, 1, 2, 3]._difference([3]), [1, 2], "_difference dedups the result");
eq([1, 2, 3]._difference([]), [1, 2, 3], "_difference with empty → this");
eq([1, 2, 2, 3, 2]._without([2]), [1, 3], "_without removes all occurrences");
eq([1, 1, 2]._without([2]), [1, 1], "_without keeps this's own duplicates");
eq([1, 2, 3]._union([3, 4, 5]), [1, 2, 3, 4, 5], "_union dedup, this-then-other order");
eq([1, 1, 2]._union([2, 3]), [1, 2, 3], "_union dedups within and across");
eq([]._union([1, 1, 2]), [1, 2], "_union from empty");
/* set-ops preserve SameValueZero and don't mutate */
const setSrc = [1, 2, 3, 4]; setSrc._difference([2]); eq(setSrc, [1, 2, 3, 4], "_difference does not mutate");

/* ---- batch 4: large-array hash-set correctness (O(n), not O(n²)) ---- */
{
    const big = [];
    for (let i = 0; i < 20000; i++) big.push(i % 5000);   /* each value appears 4x */
    const uq = big._unique();
    assert(uq.length === 5000, "_unique large: 5000 distinct");
    eq(uq.slice(0, 5), [0, 1, 2, 3, 4], "_unique large: order preserved");
    const other = []; for (let i = 0; i < 5000; i++) other.push(i * 2);
    assert(big._intersect(other).length === 2500, "_intersect large correct");
}

/* ---- batch 5: _partition / _pluck ---- */
eq([1, 2, 3, 4, 5, 6]._partition(x => x % 2 === 0), [[2, 4, 6], [1, 3, 5]], "_partition(fn)");
eq([1, 2, 1, 3, 1]._partition(1), [[1, 1, 1], [2, 3]], "_partition(value)");
eq([]._partition(x => true), [[], []], "_partition of empty → [[],[]]");
eq([1, 2, 3]._partition(x => true), [[1, 2, 3], []], "_partition all-match");
eq([{ n: "a", v: 1 }, { n: "b", v: 2 }, { n: "c", v: 3 }]._pluck("n"), ["a", "b", "c"], "_pluck(key)");
eq([{ v: 1 }, { v: 2 }]._pluck("missing"), [undefined, undefined], "_pluck missing key → undefined");
eq([]._pluck("x"), [], "_pluck of empty → []");
eq([[10, 20], [30, 40]]._pluck(1), [20, 40], "_pluck numeric index");
const pSrc = [1, 2, 3]; pSrc._partition(x => x > 1); eq(pSrc, [1, 2, 3], "_partition does not mutate");

/* ---- batch 6: _zip / _zipWith / _intersperse / _flatten / _transpose ---- */
eq([1, 2, 3]._zip(["a", "b"]), [[1, "a"], [2, "b"]], "_zip truncates to shorter");
eq([1, 2]._zip([]), [], "_zip with empty → []");
eq([1, 2, 3]._zipWith((a, b) => a + b, [10, 20, 30]), [11, 22, 33], "_zipWith(fn, other)");
eq([1, 2, 3]._zipWith((a, b) => a * b, [10, 20]), [10, 40], "_zipWith truncates");
eq([1, 2, 3]._intersperse(0), [1, 0, 2, 0, 3], "_intersperse");
eq([1]._intersperse(0), [1], "_intersperse single → no sep");
eq([]._intersperse(0), [], "_intersperse empty → []");
eq([1, [2, [3, [4]]]]._flatten(), [1, 2, 3, 4], "_flatten deep (default)");
eq([1, [2, [3]]]._flatten(1), [1, 2, [3]], "_flatten(1) one level");
eq([1, [2, [3]]]._flatten(0), [1, [2, [3]]], "_flatten(0) → unchanged copy");
eq([1, 2, 3]._flatten(), [1, 2, 3], "_flatten of a flat array");
eq([[1, 2, 3], [4, 5, 6]]._transpose(), [[1, 4], [2, 5], [3, 6]], "_transpose square");
eq([[1, 2], [3], [4, 5, 6]]._transpose(), [[1, 3, 4], [2, 5], [6]], "_transpose ragged (skips missing)");
eq([]._transpose(), [], "_transpose of empty → []");
/* non-mutation */
const zSrc = [1, 2, 3]; zSrc._flatten(); zSrc._intersperse(0); eq(zSrc, [1, 2, 3], "structural methods do not mutate");

print("test_array_ext: all tests passed (" + n + " assertions)");
