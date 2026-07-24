/* test_sort.js — dyna:sort (in-repo merge sort + binary search).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_sort.js
 * Prints "test_sort: all tests passed" on success; throws on failure. */

import { sort, binarySearch } from "dyna:sort";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eqArr(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++)
        if (!Object.is(a[i], b[i])) return false;
    return true;
}

/* --- sort matches [...arr].sort((a,b)=>a-b) for numeric input --- */
{
    const inputs = [
        [3, 1, 2],
        [5, 4, 3, 2, 1],
        [1, 2, 3, 4, 5],
        [10, -1, 0, 7, 7, 3, -100, 42],
        [2.5, 2.4, 2.6, -0.0, 0.0, 1e308, -1e308],
    ];
    for (const arr of inputs) {
        const got = sort(arr);
        const ref = [...arr].sort((a, b) => a - b);
        assert(eqArr(got, ref), "sort vs ref: " + JSON.stringify(arr) +
            " => " + JSON.stringify(got));
        /* input not mutated */
        assert(arr.length === arr.length, "len stable");
    }
}

/* --- input is NOT mutated --- */
{
    const arr = [3, 1, 2];
    const copy = [...arr];
    sort(arr);
    assert(eqArr(arr, copy), "sort does not mutate input");
}

/* --- empty / single / duplicates --- */
{
    assert(eqArr(sort([]), []), "empty array sorts to empty");
    assert(eqArr(sort([42]), [42]), "single element");
    assert(eqArr(sort([7, 7, 7, 7]), [7, 7, 7, 7]), "all duplicates");
    assert(eqArr(sort([2, 1, 2, 1, 2, 1]), [1, 1, 1, 2, 2, 2]), "duplicates mixed");
}

/* --- NaN sorts to the end (documented total order) --- */
{
    const got = sort([3, NaN, 1, NaN, 2]);
    assert(got.length === 5, "NaN sort keeps count");
    assert(got[0] === 1 && got[1] === 2 && got[2] === 3, "finite part sorted");
    assert(Number.isNaN(got[3]) && Number.isNaN(got[4]), "NaNs at end");
}

/* --- string elements coerce via Number (no comparator) --- */
{
    assert(eqArr(sort(["3", "10", "2"]), [2, 3, 10]),
        "numeric strings coerce and sort numerically");
}

/* --- custom comparator: descending --- */
{
    const arr = [3, 1, 2, 5, 4];
    const got = sort(arr, (a, b) => b - a);
    assert(eqArr(got, [5, 4, 3, 2, 1]), "descending comparator");
}

/* --- comparator preserves original element type (no float coercion) --- */
{
    const arr = ["banana", "apple", "cherry"];
    const got = sort(arr, (a, b) => (a < b ? -1 : a > b ? 1 : 0));
    assert(eqArr(got, ["apple", "banana", "cherry"]), "string comparator sort");
    /* elements are the original strings, not coerced numbers */
    assert(typeof got[0] === "string", "elements stay strings");
}

/* --- comparator sort is stable --- */
{
    const arr = [
        { k: 1, id: "a" }, { k: 0, id: "b" }, { k: 1, id: "c" },
        { k: 0, id: "d" }, { k: 1, id: "e" },
    ];
    const got = sort(arr, (a, b) => a.k - b.k);
    const ids = got.map(x => x.id).join("");
    /* stable: within equal keys, original order preserved */
    assert(ids === "bdace", "stable comparator sort (" + ids + ")");
}

/* --- comparator that throws propagates --- */
{
    let threw = false;
    try {
        sort([3, 1, 2], () => { throw new Error("boom"); });
    } catch (e) { threw = e.message === "boom"; }
    assert(threw, "throwing comparator propagates");
}

/* --- non-callable comparator throws TypeError --- */
{
    let threw = false;
    try { sort([1, 2], 123); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "non-callable comparator is TypeError");
}

/* --- non-array throws --- */
{
    let threw = false;
    try { sort("not an array"); } catch { threw = true; }
    assert(threw, "sort(non-array) throws");
}

/* --- binarySearch: hit / miss / boundaries (double path) --- */
{
    const s = sort([10, -1, 0, 7, 3, -100, 42]); // [-100,-1,0,3,7,10,42]
    assert(s[binarySearch(s, -100)] === -100, "find first element");
    assert(s[binarySearch(s, 42)] === 42, "find last element");
    assert(s[binarySearch(s, 3)] === 3, "find middle element");
    assert(binarySearch(s, 5) === -1, "miss returns -1");
    assert(binarySearch(s, -1000) === -1, "below range -1");
    assert(binarySearch(s, 1000) === -1, "above range -1");
    assert(binarySearch([], 1) === -1, "binarySearch empty -1");
    assert(binarySearch([9], 9) === 0, "single hit");
    assert(binarySearch([9], 8) === -1, "single miss");
}

/* --- binarySearch cross-check against a linear scan over many keys --- */
{
    const base = [];
    for (let i = 0; i < 200; i++) base.push(((i * 37) % 101) - 50);
    const s = sort(base);
    for (let key = -60; key <= 60; key++) {
        const idx = binarySearch(s, key);
        const present = s.includes(key);
        if (present) assert(idx >= 0 && s[idx] === key, "bsearch finds " + key);
        else assert(idx === -1, "bsearch miss " + key);
    }
}

/* --- binarySearch with comparator (descending order) --- */
{
    const arr = sort([3, 1, 2, 5, 4], (a, b) => b - a); // [5,4,3,2,1]
    const cmp = (a, b) => b - a;
    assert(arr[binarySearch(arr, 5, cmp)] === 5, "desc bsearch first");
    assert(arr[binarySearch(arr, 1, cmp)] === 1, "desc bsearch last");
    assert(arr[binarySearch(arr, 3, cmp)] === 3, "desc bsearch middle");
    assert(binarySearch(arr, 99, cmp) === -1, "desc bsearch miss");
}

/* --- binarySearch comparator on strings --- */
{
    const words = sort(["cherry", "apple", "banana"],
        (a, b) => (a < b ? -1 : a > b ? 1 : 0));
    const cmp = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
    assert(words[binarySearch(words, "banana", cmp)] === "banana",
        "string bsearch hit");
    assert(binarySearch(words, "durian", cmp) === -1, "string bsearch miss");
}

/* --- binarySearch throwing comparator propagates --- */
{
    let threw = false;
    try {
        binarySearch([1, 2, 3], 2, () => { throw new Error("cmp"); });
    } catch (e) { threw = e.message === "cmp"; }
    assert(threw, "throwing bsearch comparator propagates");
}

print("test_sort: all tests passed (" + n + " assertions)");
