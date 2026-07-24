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

/* ---- batch 2: reverse / insert / remove / removeAll / compact ---- */
eq("hello".reverse(), "olleh", "reverse");
eq("".reverse(), "", "reverse of empty");
eq("a".reverse(), "a", "reverse single");
eq("café".reverse(), "éfac", "reverse BMP wide string (accents fine)");
eq("abcabc".reverse(), "cbacba", "reverse repeats");

eq("hello".insert("!"), "hello!", "insert() appends by default");
eq("hello".insert(" world", 5), "hello world", "insert(str, index)");
eq("hello".insert("X", 0), "Xhello", "insert at front");
eq("hello".insert("X", -1), "hellXo", "insert negative index from end");
eq("hello".insert("X", 99), "helloX", "insert index>len clamps to end");
eq("hi".insert(5), "hi5", "insert coerces a non-string arg");

eq("hello".remove("l"), "helo", "remove first occurrence");
eq("hello world".remove("o"), "hell world", "remove first o");
eq("hello".remove("z"), "hello", "remove no match → unchanged");
eq("hello".remove(""), "hello", "remove empty needle → unchanged");
eq("aXbXc".removeAll("X"), "abc", "removeAll");
eq("aaa".removeAll("a"), "", "removeAll everything");
eq("ababab".removeAll("ab"), "", "removeAll multi-char");
eq("aaaa".removeAll("aa"), "", "removeAll non-overlapping (2 matches)");
eq("banana".remove("na"), "bana", "remove multi-char first");

eq("  hello   world  ".compact(), "hello world", "compact trims + collapses");
eq("a\t\n b".compact(), "a b", "compact assorted whitespace → single space");
eq("   ".compact(), "", "compact all-whitespace → ''");
eq("nospace".compact(), "nospace", "compact no whitespace unchanged");
eq("".compact(), "", "compact empty");

/* wide + rope */
{
    let r = ""; for (let i = 0; i < 40; i++) r += "ab ";
    eq(r.compact().length, 119, "compact on a rope-built string");   /* "ab "*40 → "ab ...ab" */
    eq("café crème".remove("è"), "café crme", "remove on a wide string");
    eq("café".insert("!", 2), "ca!fé", "insert into a wide string");
}
/* reentrant valueOf/toString args must not corrupt the result */
eq("hello".insert({ toString() { return "!" } }, { valueOf() { return 2 } }), "he!llo", "insert with object args");

/* compact differential oracle: native (SIMD path >= 64 narrow chars, scalar
 * below) must byte-match `s.trim().replace(/\s+/g,' ')` — an exact reference
 * because trim() and \s share this engine's lre_is_space whitespace set. Cross
 * the 64-byte threshold and include NBSP (a Latin1 whitespace byte). */
{
    const ref = s => s.trim().replace(/\s+/g, " ");
    const parts = ["word", "  ", "\t", " ", "x", "  \n ", "aVeryLongTokenIndeed", " "];
    let rng = 123456789;
    const rand = () => (rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
    for (let t = 0; t < 4000; t++) {
        let s = "";
        const par8 = 1 + ((rng >> 3) % 40);          /* vary length across the 64 threshold */
        for (let k = 0; k < par8; k++) s += parts[(rand() * parts.length) | 0];
        eq(s.compact(), ref(s), "compact differential t=" + t + " len=" + s.length);
    }
    /* explicit long narrow with NBSP exercises the SIMD path's derived ws set */
    const nb = ("alpha\u00A0\u00A0beta   gamma\t").repeat(8);  /* > 64, narrow, NBSP */
    eq(nb.compact(), ref(nb), "compact SIMD path collapses NBSP runs");
    assert(nb.length >= 64 && ![...nb].some(c => c.charCodeAt(0) > 255), "NBSP test string is long + narrow");
}

print("test_string_ext: all tests passed (" + n + " assertions)");
