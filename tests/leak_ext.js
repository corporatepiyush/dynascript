/* leak_ext.js — exercise EVERY `_`-prefixed extension method (Array + TypedArray)
 * across success, edge, and error/throw paths, in a loop. Run under Linux ASan
 * + LeakSanitizer (ASAN_OPTIONS=detect_leaks=1): any JSValue a method fails to
 * free on any path becomes an unreachable allocation LSan reports at exit.
 * A per-call leak of even one heap value is amplified by the loop. */

function boom() { throw new Error("boom"); }
const evil = { valueOf() { return 3; }, toString() { return "k"; } };

/* iteration count: scriptArgs[1] if given (for the RSS-plateau leak check), else 200 */
const ITERS = (typeof scriptArgs !== "undefined" && scriptArgs[1]) ? +scriptArgs[1] : 200;
for (let it = 0; it < ITERS; it++) {
    const a = [3, 1, 4, 1, 5, 9, 2, 6, "x" + it, { id: it % 3 }, null, undefined];
    const nums = [3, 1, 4, 1, 5, 9, 2, 6];
    const objs = [{ id: it % 3 }, { id: (it + 1) % 3 }, { id: 2 }]; /* clean, for prop mappers */

    /* batch 1/2: reductions, queries, slicers */
    a._first(); a._first(3); a._last(); a._last(3); a._isEmpty(); a._compact();
    nums._sum(); nums._average(); nums._mean(); nums._min(); nums._max();
    nums._min(x => x * 2); nums._max(evil.valueOf ? (x => -x) : 0);
    a._count(); a._count(1); a._count(x => typeof x === "number");
    a._none(1); a._any(1); a._all(x => x !== 7);
    nums._take(3); nums._drop(3); nums._takeLast(2); nums._dropLast(2);
    nums._take(999); nums._take(-1);

    /* batch 3: transforms (allocate keys, call mappers, RNG) */
    nums._sortBy(); nums._sortBy(x => -x); nums._sortBy(undefined, true);
    objs._sortBy("id"); a._groupBy(x => typeof x); objs._groupBy("id");
    nums._shuffle(); nums._sample(); nums._sample(3); nums._sample(999);
    ["banana", "apple", "cherry"]._sortBy();

    /* batch 4: dedup + set-ops (hash set, resize, key dups) */
    a._unique(); a._uniq(); [{ id: 1 }, { id: 1 }, { id: 2 }]._uniqBy(x => x.id);
    objs._unique("id"); nums._intersect([2, 4, 6]); nums._intersection([1, 9]);
    nums._difference([2, 4]); nums._without([1]); nums._union([9, 10, 11]);
    ["a", "b", "a"]._unique();

    /* TypedArray SIMD + scalar reductions */
    const f64 = new Float64Array([3.5, -1.5, 4.5, it]);
    f64._sum(); f64._min(); f64._max(); f64._mean(); f64._average();
    new Int32Array([3, 1, 4, 1, 5])._sum();
    new Uint8Array([10, 20, 30])._max();
    new Float16Array([1.5, 2.5])._sum();

    /* error / throw paths must not leak (mapper throws, symbol key, bigint TA) */
    try { nums._sortBy(boom); } catch {}
    try { nums._groupBy(boom); } catch {}
    try { [{}, {}]._sortBy(() => Symbol()); } catch {}
    try { nums._unique(boom); } catch {}
    try { nums._min(boom); } catch {}
    try { new BigInt64Array([1n])._sum(); } catch {}
    try { [1, 2]._intersect(boom); } catch {}
    /* prop mapper on a null element throws mid-decorate -> exercises cleanup */
    try { a._sortBy("id"); } catch {}
    try { a._groupBy("id"); } catch {}
    try { a._unique("id"); } catch {}
    /* TypedArray _dot error paths */
    try { new Float64Array([1, 2])._dot(new Float64Array([1])); } catch {}
    try { new Float64Array([1])._dot([1]); } catch {}
    new Float64Array([1, 2, 3])._dot(new Float64Array([4, 5, 6]));
    new Int32Array([1, 2])._dot(new Int32Array([3, 4]));
}

print("leak_ext: exercised all extension methods (LSan verdict follows at exit)");
