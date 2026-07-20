/* test_search.js — scl:search (in-repo Boyer-Moore-Horspool substring search).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_search.js
 * Prints "test_search: all tests passed" on success; throws on failure. */

import { indexOf, indexOfAll } from "scl:search";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eqArr(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

/* --- indexOf found / not found (ASCII: byte offset == JS index) --- */
{
    assert(indexOf("the quick brown fox", "quick") === 4, "found in middle");
    assert(indexOf("the quick brown fox", "the") === 0, "found at start");
    assert(indexOf("the quick brown fox", "fox") === 16, "found at end");
    assert(indexOf("hello world", "xyz") === -1, "not found");
    assert(indexOf("abc", "abcd") === -1, "needle longer than haystack");
    assert(indexOf("", "a") === -1, "empty haystack, non-empty needle");
    assert(indexOf("abcabc", "bc") === 1, "first of several");
}

/* --- indexOf cross-checked against String.prototype.indexOf (ASCII) --- */
{
    const hay = "mississippi river is missing";
    const needles = ["is", "ss", "i", "riv", "z", "missing", "m", "g"];
    for (const nd of needles)
        assert(indexOf(hay, nd) === hay.indexOf(nd),
            "indexOf matches String.indexOf for '" + nd + "'");
}

/* --- empty needle -> 0 (mirrors String.prototype.indexOf("")) --- */
{
    assert(indexOf("hello", "") === 0, "empty needle -> 0");
    assert(indexOf("", "") === 0, "empty needle, empty haystack -> 0");
    assert("hello".indexOf("") === 0, "sanity: JS empty needle is 0");
}

/* --- indexOfAll: non-overlapping and overlapping --- */
{
    assert(eqArr(indexOfAll("abababab", "ab"), [0, 2, 4, 6]), "ab*4");
    assert(eqArr(indexOfAll("aaaa", "aa"), [0, 1, 2]), "overlapping aa in aaaa");
    assert(eqArr(indexOfAll("aaaaa", "aa"), [0, 1, 2, 3]), "overlapping aa in aaaaa");
    assert(eqArr(indexOfAll("hello world", "o"), [4, 7]), "single char matches");
    assert(eqArr(indexOfAll("abcabcabc", "abc"), [0, 3, 6]), "abc*3");
}

/* --- indexOfAll: not found / empty needle / long needle --- */
{
    assert(eqArr(indexOfAll("hello", "z"), []), "no match -> []");
    assert(eqArr(indexOfAll("hello", ""), []), "empty needle -> []");
    assert(eqArr(indexOfAll("", "a"), []), "empty haystack -> []");
    assert(eqArr(indexOfAll("ab", "abc"), []), "needle longer -> []");
}

/* --- indexOfAll offsets are ascending and each is a real occurrence --- */
{
    const hay = "the cat sat on the mat with the hat";
    const res = indexOfAll(hay, "at");
    for (let i = 1; i < res.length; i++)
        assert(res[i] > res[i - 1], "ascending offsets");
    for (const p of res)
        assert(hay.substr(p, 2) === "at", "offset " + p + " is a real match");
    /* count matches a manual scan */
    let count = 0, from = hay.indexOf("at");
    while (from !== -1) { count++; from = hay.indexOf("at", from + 1); }
    assert(res.length === count, "indexOfAll count matches manual scan");
}

/* --- UTF-8 byte offsets (documented: not UTF-16 code-unit indices) --- */
{
    /* "é" is 2 UTF-8 bytes; "x" after it lands at byte offset 2, JS index 1. */
    const s = "éx";
    assert(indexOf(s, "x") === 2, "byte offset after 2-byte char is 2");
    assert(s.indexOf("x") === 1, "sanity: JS code-unit index is 1");
}

/* --- coercion: non-string arguments are stringified --- */
{
    assert(indexOf(12345, "34") === 2, "number haystack coerced to string");
    assert(indexOf("a1b1c", 1) === 1, "number needle coerced to string");
}

/* --- self-match and full-string needle --- */
{
    assert(indexOf("hello", "hello") === 0, "needle equals haystack");
    assert(eqArr(indexOfAll("hello", "hello"), [0]), "full-string single match");
}

print("test_search: all tests passed (" + n + " assertions)");
