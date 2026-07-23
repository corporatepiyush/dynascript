/* test_strings.js -- dynajs:strings (Go strings package + JS String helpers).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_strings.js
 * Prints "test_strings: all tests passed" on success; throws on failure. */

import {
    split, splitN, fields, join,
    trim, trimStart, trimEnd, trimPrefix, trimSuffix, trimChars,
    toUpper, toLower, title, repeat, padStart, padEnd,
    contains, containsAny, hasPrefix, hasSuffix,
    index, lastIndex, indexAny, count,
    replace, replaceAll, equalFold, compare,
} from "dynajs:strings";

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
function throws(fn) {
    try { fn(); return false; } catch (e) { return true; }
}

/* A handful of strings spanning ASCII, accented Latin, Greek (incl. the
 * final-sigma case), Cyrillic, CJK and astral (emoji) code points, plus
 * assorted Unicode whitespace -- used to cross-check our code-point-aware
 * functions against this engine's OWN (already-correct) String.prototype
 * implementations, rather than hand-verifying Unicode tables ourselves. */
const DIVERSE = [
    "", "hello", "Hello, World!", "  padded  ", "\t\nmixed\r\n",
    "café", "naïve", "Zürich", "ΟΔΥΣΣΕΥΣ", "ΣΣ", "Привет мир",
    "你好世界", "hi\u{1F600}bye", " nbsp ", " emspace ",
    "﻿bom﻿", "MiXeD CaSe 123",
];

/* ================= split / splitN ================= */
{
    assert(eqArr(split("a,b,c", ","), ["a", "b", "c"]), "split basic");
    assert(eqArr(split(",a,", ","), ["", "a", ""]), "split leading/trailing sep");
    assert(eqArr(split("", ","), [""]), "split empty haystack -> ['']");
    assert(eqArr(split("abc", ""), ["a", "b", "c"]), "split empty sep -> runes");
    assert(eqArr(split("", ""), []), "split empty haystack, empty sep -> []");
    assert(eqArr(split("a,b,c", ";"), ["a,b,c"]), "split sep not found -> [whole]");
    assert(eqArr(split("a,,b", ","), ["a", "", "b"]), "split adjacent seps -> empty piece");
    assert(eqArr(split("aXXbXXc", "XX"), ["a", "b", "c"]), "split multi-byte sep");
    assert(eqArr(split("héllo", ""), ["h", "é", "l", "l", "o"]), "split runes: multibyte code point is one piece");
    assert(eqArr(split("a,é,c", ","), ["a", "é", "c"]), "split ascii sep around multibyte piece");
}
{
    assert(eqArr(splitN("a,b,c", ",", 2), ["a", "b,c"]), "splitN n=2");
    assert(eqArr(splitN("a,b,c", ",", 1), ["a,b,c"]), "splitN n=1 -> whole string");
    assert(eqArr(splitN("a,b,c", ",", 0), []), "splitN n=0 -> []");
    assert(eqArr(splitN("a,b,c", ",", -1), ["a", "b", "c"]), "splitN n=-1 -> unbounded");
    assert(eqArr(splitN("a,b,c", ",", -5), ["a", "b", "c"]), "splitN any negative -> unbounded");
    assert(eqArr(splitN("a,b,c", ",", 100), ["a", "b", "c"]), "splitN n larger than pieces");
    assert(eqArr(splitN("abc", "", 2), ["a", "bc"]), "splitN empty sep, bounded");
    assert(eqArr(splitN("abc", "", 1), ["abc"]), "splitN empty sep, n=1");
    assert(eqArr(splitN("abc", "", 0), []), "splitN empty sep, n=0");
    assert(eqArr(splitN("", ",", 5), [""]), "splitN empty haystack");
}

/* ================= fields ================= */
{
    assert(eqArr(fields("  foo  bar  baz   "), ["foo", "bar", "baz"]), "fields basic");
    assert(eqArr(fields(""), []), "fields empty -> []");
    assert(eqArr(fields("   "), []), "fields all-space -> []");
    assert(eqArr(fields("foo"), ["foo"]), "fields single word");
    assert(eqArr(fields("\tfoo\nbar\r\n"), ["foo", "bar"]), "fields mixed whitespace");
    assert(eqArr(fields("a b"), ["a", "b"]), "fields single spaces");
}

/* ================= join ================= */
{
    assert(join(["a", "b", "c"], "-") === "a-b-c", "join basic");
    assert(join([], "-") === "", "join empty array");
    assert(join(["a"], "-") === "a", "join single element");
    assert(join(["a", null, "b"], "-") === "a--b", "join null -> empty string");
    assert(join(["a", undefined, "b"], "-") === "a--b", "join undefined -> empty string");
    assert(join([1, 2, 3], "-") === "1-2-3", "join coerces numbers");
    assert(join(["a", "b"], "") === "ab", "join empty separator");
}

/* ================= trim / trimStart / trimEnd ================= */
{
    assert(trim("  hi  ") === "hi", "trim both sides");
    assert(trimStart("  hi  ") === "hi  ", "trimStart only");
    assert(trimEnd("  hi  ") === "  hi", "trimEnd only");
    assert(trim("\t\nhi\r\n") === "hi", "trim mixed whitespace");
    assert(trim("") === "", "trim empty");
    assert(trim("   ") === "", "trim all-space -> empty");
    assert(trim("hi") === "hi", "trim no-op");
    assert(trim("   a   b   ") === "a   b", "trim preserves interior spaces");

    for (const s of DIVERSE) {
        assert(trim(s) === s.trim(), "trim matches String.trim for: " + JSON.stringify(s));
        assert(trimStart(s) === s.trimStart(), "trimStart matches for: " + JSON.stringify(s));
        assert(trimEnd(s) === s.trimEnd(), "trimEnd matches for: " + JSON.stringify(s));
    }
}

/* ================= trimPrefix / trimSuffix ================= */
{
    assert(trimPrefix("golang", "go") === "lang", "trimPrefix match");
    assert(trimPrefix("golang", "xyz") === "golang", "trimPrefix no match -> unchanged");
    assert(trimSuffix("golang", "lang") === "go", "trimSuffix match");
    assert(trimSuffix("golang", "xyz") === "golang", "trimSuffix no match -> unchanged");
    assert(trimPrefix("", "") === "", "trimPrefix empty/empty");
    assert(trimPrefix("abc", "") === "abc", "trimPrefix empty prefix -> unchanged");
    assert(trimSuffix("abc", "") === "abc", "trimSuffix empty suffix -> unchanged");
    assert(trimPrefix("abc", "abcd") === "abc", "trimPrefix longer-than-s -> unchanged");
    assert(trimPrefix("héllo", "h") === "éllo", "trimPrefix byte-level ascii prefix before multibyte");
}

/* ================= trimChars ================= */
{
    assert(trimChars("¡¡¡Hello, Gophers!!!", "!¡") === "Hello, Gophers",
        "trimChars canonical Go example (multibyte cutset)");
    assert(trimChars("xxhixx", "x") === "hi", "trimChars simple");
    assert(trimChars("hi", "xyz") === "hi", "trimChars no matching chars -> unchanged");
    assert(trimChars("", "abc") === "", "trimChars empty s");
    assert(trimChars("abc", "") === "abc", "trimChars empty cutset -> unchanged");
    assert(trimChars("aaa", "a") === "", "trimChars strips everything");
}

/* ================= toUpper / toLower ================= */
{
    assert(toUpper("hello") === "HELLO", "toUpper ascii");
    assert(toLower("HELLO") === "hello", "toLower ascii");
    assert(toUpper("Hello, World!") === "HELLO, WORLD!", "toUpper punctuation passthrough");
    assert(toLower("Hello, World!") === "hello, world!", "toLower punctuation passthrough");
    assert(toUpper("café") === "CAFÉ", "toUpper accented Latin");
    assert(toLower("CAFÉ") === "café", "toLower accented Latin");

    /* Greek final sigma: capital sigma maps to final-form ς only when it
     * ends a word, matching this engine's own String.prototype.toLowerCase
     * (SpecialCasing.txt), not simple per-rune lowercasing. */
    assert(toLower("ΟΔΥΣΣΕΥΣ") === "ΟΔΥΣΣΕΥΣ".toLowerCase(), "toLower final-sigma word");
    assert(toLower("ΣΣ") === "ΣΣ".toLowerCase(), "toLower final-sigma minimal case");
    /* A LONE capital sigma has no PRECEDING cased letter, so the Final_Sigma
     * context rule doesn't apply (it requires "preceded by a cased letter");
     * it lowercases to plain sigma, not the word-final form -- verified
     * against this engine's own String.prototype.toLowerCase(). */
    assert(toLower("Σ") === "σ", "toLower lone sigma has no preceding context -> plain form");

    for (const s of DIVERSE) {
        assert(toUpper(s) === s.toUpperCase(), "toUpper matches String.toUpperCase for: " + JSON.stringify(s));
        assert(toLower(s) === s.toLowerCase(), "toLower matches String.toLowerCase for: " + JSON.stringify(s));
    }
}

/* ================= title ================= */
{
    assert(title("her royal highness") === "Her Royal Highness", "title basic");
    assert(title("loud noises") === "Loud Noises", "title basic 2");
    assert(title("  this is a test") === "  This Is A Test", "title preserves leading spaces");
    assert(title("it's") === "It'S", "title Go's documented apostrophe quirk");
    assert(title("a1b") === "A1b", "title digit does not reset word boundary");
    assert(title("") === "", "title empty");
    assert(title("café bar") === "Café Bar", "title multibyte word-start letter");
    assert(title("hello world") === "Hello World", "title two words");
}

/* ================= repeat ================= */
{
    assert(repeat("ab", 3) === "ababab", "repeat basic");
    assert(repeat("ab", 0) === "", "repeat zero");
    assert(repeat("", 5) === "", "repeat empty string");
    assert(repeat("x", 1) === "x", "repeat once");
    assert(repeat("ab", 1000).length === 2000, "repeat large count length");
    assert(repeat("ab", 1000) === "ab".repeat(1000), "repeat large count matches String.repeat");
    assert(throws(() => repeat("ab", -1)), "repeat negative count throws");
    try { repeat("ab", -1); assert(false, "unreachable"); }
    catch (e) { assert(e instanceof RangeError, "repeat negative throws RangeError"); }
    assert(repeat("x", 5) === "x".repeat(5), "repeat matches String.repeat ascii");
}

/* ================= padStart / padEnd ================= */
{
    assert(padStart("5", 3, "0") === "005", "padStart basic");
    assert(padEnd("5", 3, "0") === "500", "padEnd basic");
    assert(padStart("abc", 3) === "abc", "padStart already at target");
    assert(padStart("abc", 2) === "abc", "padStart target < length -> unchanged");
    assert(padStart("1", 5, "ab") === "abab1", "padStart cycles pad string");
    assert(padEnd("1", 5, "ab") === "1abab", "padEnd cycles pad string");
    assert(padStart("x", 4, "abc") === "abcx", "padStart pad cycle non-multiple");
    assert(padStart("x", 3) === "  x", "padStart default pad is space");
    assert(padStart("x", 3, "") === "x", "padStart empty pad -> unchanged");
    assert(padEnd("x", 3, "") === "x", "padEnd empty pad -> unchanged");
    assert(padStart("x", -5) === "x", "padStart negative target -> unchanged");
    assert(padStart("", 3, "z") === "zzz", "padStart empty source");

    const padCases = [["5", 3, "0"], ["ab", 5, "xy"], ["hello", 3, "-"], ["", 4, "ab"]];
    for (const [s, len, pad] of padCases) {
        assert(padStart(s, len, pad) === s.padStart(len, pad),
            "padStart matches String.padStart for " + JSON.stringify([s, len, pad]));
        assert(padEnd(s, len, pad) === s.padEnd(len, pad),
            "padEnd matches String.padEnd for " + JSON.stringify([s, len, pad]));
    }
}

/* ================= contains / containsAny ================= */
{
    assert(contains("seafood", "foo") === true, "contains match");
    assert(contains("seafood", "bar") === false, "contains no match");
    assert(contains("seafood", "") === true, "contains empty sub -> true");
    assert(contains("", "") === true, "contains empty/empty -> true");
    assert(contains("", "x") === false, "contains empty haystack -> false");

    assert(containsAny("failure", "ui") === true, "containsAny Go doc example");
    assert(containsAny("hello", "xyz") === false, "containsAny no match");
    assert(containsAny("hello", "") === false, "containsAny empty chars -> false");
    assert(containsAny("", "abc") === false, "containsAny empty haystack -> false");
    assert(containsAny("hi\u{1F600}bye", "\u{1F600}xyz") === true,
        "containsAny multibyte chars set (non-ASCII path)");
    assert(containsAny("hello world", "\u{1F600}") === false,
        "containsAny multibyte needle absent");
}

/* ================= hasPrefix / hasSuffix ================= */
{
    assert(hasPrefix("golang", "go") === true, "hasPrefix match");
    assert(hasPrefix("golang", "og") === false, "hasPrefix no match");
    assert(hasSuffix("golang", "lang") === true, "hasSuffix match");
    assert(hasSuffix("golang", "goo") === false, "hasSuffix no match");
    assert(hasPrefix("golang", "") === true, "hasPrefix empty -> true");
    assert(hasSuffix("golang", "") === true, "hasSuffix empty -> true");
    assert(hasPrefix("", "") === true, "hasPrefix empty/empty -> true");
    assert(hasPrefix("", "x") === false, "hasPrefix empty s, non-empty prefix -> false");
    assert(hasPrefix("abc", "abcd") === false, "hasPrefix longer than s -> false");
}

/* ================= index / lastIndex / indexAny / count ================= */
{
    assert(index("chicken", "ken") === 4, "index basic");
    assert(index("chicken", "dmx") === -1, "index not found");
    assert(index("", "") === 0, "index empty/empty -> 0");
    assert(index("x", "") === 0, "index empty sub -> 0");

    assert(lastIndex("go gopher", "go") === 3, "lastIndex basic");
    assert(lastIndex("go gopher", "rodent") === -1, "lastIndex not found");
    assert(lastIndex("abcabc", "") === 6, "lastIndex empty sub -> length");
    assert(lastIndex("", "") === 0, "lastIndex empty/empty -> 0");
    assert(lastIndex("aaaa", "aa") === 2, "lastIndex overlapping-capable needle -> rightmost start");

    assert(indexAny("chicken", "aeiouy") === 2, "indexAny Go doc example");
    assert(indexAny("crwth", "aeiouy") === -1, "indexAny no vowels");
    assert(indexAny("hello", "") === -1, "indexAny empty chars -> -1");
    assert(indexAny("hi\u{1F600}bye", "\u{1F600}") === 2, "indexAny multibyte byte offset");

    assert(count("cheese", "e") === 3, "count basic");
    assert(count("five", "") === 5, "count empty sub -> runes+1");
    assert(count("", "") === 1, "count empty/empty -> 1");
    assert(count("banana", "ana") === 1, "count is NON-overlapping (overlapping edge case)");
    assert(count("aaaa", "aa") === 2, "count non-overlapping aa in aaaa -> 2, not 3");
    assert(count("hello", "z") === 0, "count no match -> 0");
    assert(count("hello", "helloworld") === 0, "count sub longer than s -> 0");

    /* embedded NUL: explicit lengths must not truncate at \0 */
    const withNul = "a b";
    assert(withNul.length === 3, "sanity: embedded NUL string is 3 code units");
    assert(index(withNul, " ") === 1, "index finds embedded NUL");
    assert(count(withNul, " ") === 1, "count finds embedded NUL");
    assert(eqArr(split(withNul, " "), ["a", "b"]), "split on embedded NUL");
}

/* ================= replace / replaceAll ================= */
{
    assert(replace("oink oink oink", "oink", "moo", -1) === "moo moo moo", "replace unlimited");
    assert(replace("oink oink oink", "oink", "moo", 2) === "moo moo oink", "replace bounded");
    assert(replace("oink oink oink", "oink", "moo", 0) === "oink oink oink", "replace n=0 -> unchanged");
    assert(replace("abc", "", "-", -1) === "-a-b-c-", "replace empty old, unlimited: insert at every boundary");
    assert(replace("abc", "", "-", 2) === "-a-bc", "replace empty old, bounded: remainder untouched");
    assert(replace("abc", "", "-", 0) === "abc", "replace empty old, n=0 -> unchanged");
    assert(replace("abc", "", "-", 1) === "-abc", "replace empty old, n=1: only leading insert");
    assert(replace("abc", "b", "b", 5) === "abc", "replace old==new short-circuit");
    assert(replace("", "", "-", -1) === "-", "replace empty old on empty string -> single insert");

    assert(replaceAll("This is a cat", "a", "o") === "This is o cot", "replaceAll Go doc example");
    assert(replaceAll("oink oink oink", "oink", "moo") === "moo moo moo", "replaceAll basic");
    assert(replaceAll("hello", "z", "Z") === "hello", "replaceAll no match -> unchanged");
    assert(replaceAll("aaaa", "aa", "b") === "bb", "replaceAll non-overlapping consumption");
    assert(replaceAll("abc", "", "-") === "-a-b-c-", "replaceAll empty old");

    /* cross-check against this engine's own String.prototype.replace/replaceAll */
    const rCases = [["hello world", "o", "0"], ["banana", "an", "AN"], ["aaa", "a", "b"]];
    for (const [s, o, r] of rCases) {
        assert(replace(s, o, r, 1) === s.replace(o, r),
            "replace n=1 matches String.replace for " + JSON.stringify([s, o, r]));
        assert(replaceAll(s, o, r) === s.replaceAll(o, r),
            "replaceAll matches String.replaceAll for " + JSON.stringify([s, o, r]));
    }
    assert(replaceAll("abc", "", "-") === "abc".replaceAll("", "-"),
        "replaceAll empty old matches String.replaceAll");
}

/* ================= equalFold ================= */
{
    assert(equalFold("Go", "GO") === true, "equalFold basic");
    assert(equalFold("AB", "ab") === true, "equalFold full flip");
    assert(equalFold("Hello", "world") === false, "equalFold different content");
    assert(equalFold("Go", "Go!") === false, "equalFold different length -> false");
    assert(equalFold("", "") === true, "equalFold empty/empty");
    assert(equalFold("abc", "abc") === true, "equalFold identical");
    /* documented ASCII-only scope: non-ASCII bytes must match exactly */
    assert(equalFold("café", "café") === true, "equalFold identical non-ASCII");
    assert(equalFold("café", "CAFÉ") === false, "equalFold non-ASCII case NOT folded (documented scope)");
}

/* ================= compare ================= */
{
    assert(compare("a", "b") === -1, "compare less");
    assert(compare("b", "a") === 1, "compare greater");
    assert(compare("abc", "abc") === 0, "compare equal");
    assert(compare("Go", "GO") === 1, "compare case-sensitive byte order");
    assert(compare("", "") === 0, "compare empty/empty");
    assert(compare("a", "") === 1, "compare non-empty vs empty");
    assert(compare("", "a") === -1, "compare empty vs non-empty");
    assert(compare("ab", "abc") === -1, "compare prefix is less");

    /* cross-check sign against JS's own string relational operators
     * (restricted to non-astral strings, where UTF-8 byte order and UTF-16
     * code-unit order are guaranteed to coincide). */
    const cmpCases = ["apple", "banana", "Apple", "app", "applesauce", "café", "cafe"];
    for (const a of cmpCases) {
        for (const b of cmpCases) {
            const want = a < b ? -1 : a > b ? 1 : 0;
            assert(compare(a, b) === want, "compare sign matches JS operators for " + a + " vs " + b);
        }
    }
}

/* ================= adversarial: reentrancy / hostile coercion ================= */
{
    /* Coercion runs arbitrary JS (toString/valueOf); these are pure
     * functions with no closable resource, but a throwing or reentrant
     * coercion must still propagate/behave correctly, not crash. */
    assert(throws(() => split({ toString() { throw new Error("boom"); } }, ",")),
        "split propagates a throwing toString");
    assert(throws(() => join([{ toString() { throw new Error("boom"); } }], ",")),
        "join propagates a throwing element toString");
    assert(throws(() => replace({ toString() { throw new Error("boom"); } }, "a", "b", 1)),
        "replace propagates a throwing toString");

    const hostile = { toString() { return toUpper("x"); } };
    assert(split(hostile, ",")[0] === "X", "reentrant toString calling back into the module works");

    let calls = 0;
    const counting = { toString() { calls++; return "abc"; } };
    assert(index(counting, "b") === 1, "coercion side effect sanity");
    assert(calls === 1, "each argument is coerced exactly once (no double toString)");
}

/* ================= adversarial: large-ish sizes ================= */
{
    const big = "x".repeat(5000);
    assert(count(big, "x") === 5000, "count over a large string");
    assert(split(big, "x").length === 5001, "split over a large string");
    assert(replaceAll(big, "x", "yz").length === 10000, "replaceAll growing a large string");
    assert(trim("  " + big + "  ") === big, "trim over a large padded string");
    assert(padStart("x", 5000, "ab").length === 5000, "padStart to a large target");
}

print("test_strings: all tests passed (" + n + " assertions)");
