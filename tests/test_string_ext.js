/* test_string_ext.js — native SugarJS/RamdaJS String methods (unprefixed,
 * non-enumerable). Core-engine builtins present in every build.
 * Run: dynajs tests/test_string_ext.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

/* ---- batch 1: isEmpty / isBlank / first / last / from / to / chars / codes ---- */
assert("".isEmpty() === true, "isEmpty of empty");
assert(" ".isEmpty() === false, "isEmpty of a space is false");
assert("x".isEmpty() === false, "isEmpty of non-empty");

assert("".isBlank() === true, "isBlank of empty");
assert("   ".isBlank() === true, "isBlank of spaces");
assert("\t\n\r\f\v ".isBlank() === true, "isBlank of assorted whitespace");
assert("  x  ".isBlank() === false, "isBlank false with a non-space");
assert(" ".isBlank() === true, "isBlank of NBSP (matches trim)");

eq("hello".first(), "h", "first() → first char");
eq("hello".first(2), "he", "first(2)");
eq("hello".first(9), "hello", "first(n>len) clamps");
eq("hello".first(0), "", "first(0) → ''");
eq("hello".first(-1), "", "first(-1) → ''");
eq("".first(), "", "first() of empty → ''");
eq("hello".last(), "o", "last() → last char");
eq("hello".last(2), "lo", "last(2) in order");
eq("hello".last(9), "hello", "last(n>len) clamps");
eq("".last(), "", "last() of empty → ''");

eq("hello".from(1), "ello", "from(index)");
eq("hello".from(), "hello", "from() → whole");
eq("hello".from(-2), "lo", "from(-2) from end");
eq("hello".from(99), "", "from(n>len) → ''");
eq("hello".to(3), "hel", "to(index) exclusive");
eq("hello".to(), "hello", "to() → whole");
eq("hello".to(-1), "hell", "to(-1) from end");
eq("hello".to(0), "", "to(0) → ''");

eq("abc".chars(), ["a", "b", "c"], "chars()");
eq("".chars(), [], "chars() of empty → []");
eq("AB".codes(), [65, 66], "codes()");
eq("".codes(), [], "codes() of empty → []");

/* wide (UTF-16) string correctness: emoji is a surrogate pair (2 code units) */
{
    const s = "a\u{1F600}b";              /* 'a', high+low surrogate, 'b' = 4 code units */
    assert(s.length === 4, "astral char is 2 code units");
    eq(s.chars().length, 4, "chars() is code-unit based (astral = 2 entries)");
    eq(s.first(1), "a", "first on a wide string");
    eq(s.last(1), "b", "last on a wide string");
    eq("café".first(3), "caf", "first on an accented (wide) string");
    eq("café".last(1), "é", "last returns the accented char");
    assert("café".isBlank() === false, "isBlank on accented text");
}

/* rope input: a concatenation the engine may keep as a rope until linearized */
{
    let r = "";
    for (let i = 0; i < 50; i++) r += "ab";
    eq(r.first(2), "ab", "first on a (possibly rope) built string");
    eq(r.last(2), "ab", "last on a rope-built string");
    assert(r.chars().length === 100, "chars on a rope-built string");
}

/* non-enumerable + does not shadow ES methods */
assert(!Object.keys(String.prototype).includes("first"), "first is non-enumerable");
assert(Object.getOwnPropertyNames(String.prototype).includes("first"), "first is an own property");
assert(typeof "x".at === "function" && "abc".at(-1) === "c", "ES String.prototype.at untouched");
assert(typeof "x".slice === "function" && "abc".slice(1) === "bc", "ES slice untouched");

/* array-like/boxed receiver + reentrant valueOf arg must not corrupt the result */
eq("hello".first({ valueOf() { return 2; } }), "he", "first coerces an object arg");
eq(String.prototype.first.call("world", 3), "wor", "first via .call on a primitive");

print("test_string_ext: all tests passed (" + n + " assertions)");
