/* test_docparse.js — dyna:docparse (in-repo CSV state machine + JSON reader).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_docparse.js
 * Prints "test_docparse: all tests passed" on success; throws on failure. */

import { parseJson, parseCsv } from "dyna:docparse";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

/* Structural deep-equality for the plain values these parsers return. */
function deepEqual(a, b) {
    if (a === b) return true;
    if (typeof a !== typeof b) return false;
    if (Array.isArray(a) || Array.isArray(b)) {
        if (!Array.isArray(a) || !Array.isArray(b)) return false;
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++)
            if (!deepEqual(a[i], b[i])) return false;
        return true;
    }
    if (a && b && typeof a === "object") {
        const ka = Object.keys(a), kb = Object.keys(b);
        if (ka.length !== kb.length) return false;
        for (const k of ka) {
            if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
            if (!deepEqual(a[k], b[k])) return false;
        }
        return true;
    }
    return false;
}

function eqCsv(input, opts, expected, label) {
    const got = opts ? parseCsv(input, opts) : parseCsv(input);
    assert(deepEqual(got, expected),
        label + " -> " + JSON.stringify(got) + " != " + JSON.stringify(expected));
}

/* ------------------------- parseCsv: array-of-arrays ------------------------ */

eqCsv("a,b,c\n1,2,3", null,
    [["a", "b", "c"], ["1", "2", "3"]], "simple rows");

eqCsv("1,,3", null, [["1", "", "3"]], "empty middle field");

eqCsv("only", null, [["only"]], "single field, no terminator");

/* quoted fields: embedded comma, embedded newline, escaped quotes */
eqCsv('x,"a,b",y', null, [["x", "a,b", "y"]], "quoted embedded comma");

eqCsv('"line1\nline2",b', null,
    [["line1\nline2", "b"]], "quoted embedded newline");

eqCsv('"she said ""hi""",z', null,
    [['she said "hi"', "z"]], "escaped double quotes");

eqCsv('""', null, [[""]], "empty quoted field");

eqCsv('"unterminated', null, [["unterminated"]], "unterminated quote");

/* line terminators */
eqCsv("a,b\r\n1,2", null, [["a", "b"], ["1", "2"]], "CRLF terminator");
eqCsv("a,b\r1,2", null, [["a", "b"], ["1", "2"]], "bare CR terminator");
eqCsv("a,b\n1,2\n", null, [["a", "b"], ["1", "2"]], "trailing newline");
eqCsv("a,b\r\n1,2\r\n", null, [["a", "b"], ["1", "2"]], "trailing CRLF");

/* blank lines are skipped */
eqCsv("a\n\nb", null, [["a"], ["b"]], "blank line skipped");
eqCsv("\n\n", null, [], "only blank lines");

/* empty input */
eqCsv("", null, [], "empty input");

/* ragged rows (varying widths) */
eqCsv("a,b,c\n1,2", null, [["a", "b", "c"], ["1", "2"]], "ragged rows");

/* --------------------------- parseCsv: array-of-objects --------------------- */

eqCsv("a,b\n1,2\n3,4", { header: true },
    [{ a: "1", b: "2" }, { a: "3", b: "4" }], "header objects");

eqCsv("a,b\n1,2,3\n4", { header: true },
    [{ a: "1", b: "2" }, { a: "4" }], "header ragged (surplus + missing)");

eqCsv("a,b", { header: true }, [], "header only, no data rows");

eqCsv('name,note\nx,"a,b"\n', { header: true },
    [{ name: "x", note: "a,b" }], "header with quoted field");

/* header:false option is equivalent to no option */
eqCsv("a,b\n1,2", { header: false },
    [["a", "b"], ["1", "2"]], "header:false is arrays");

/* ------------------------------- parseJson --------------------------------- */

function eqJson(input, expected, label) {
    const got = parseJson(input);
    assert(deepEqual(got, expected),
        label + " -> " + JSON.stringify(got) + " != " + JSON.stringify(expected));
}

eqJson('{"a":1,"b":[2,3]}', { a: 1, b: [2, 3] }, "object with array");
eqJson('[1,2,[3,[4,5]]]', [1, 2, [3, [4, 5]]], "nested arrays");
eqJson('{"n":null,"t":true,"f":false}',
    { n: null, t: true, f: false }, "literals");
eqJson('[0, -2.5, 1e3, 42]', [0, -2.5, 1000, 42], "numbers");
eqJson('{"outer":{"inner":{"deep":"v"}}}',
    { outer: { inner: { deep: "v" } } }, "deep object nesting");
eqJson('{"greeting":"héllo","cjk":"中文","emoji":"😀"}',
    { greeting: "héllo", cjk: "中文", emoji: "😀" }, "unicode");
eqJson('"\\u00e9\\u4e2d"', "é中", "unicode escapes");
eqJson('  [ 1 , 2 ]  ', [1, 2], "surrounding whitespace");
eqJson('"just a string"', "just a string", "top-level string");
eqJson('123', 123, "top-level number");

/* malformed input must throw */
function throwsJson(input, label) {
    let threw = false;
    try { parseJson(input); } catch { threw = true; }
    assert(threw, "malformed JSON throws: " + label);
}

throwsJson('', "empty input");
throwsJson('{bad}', "bare word in object");
throwsJson('[1,2', "unterminated array");
throwsJson('{"a":}', "missing value");
throwsJson('{"a":1,}', "trailing comma");
throwsJson("{'a':1}", "single-quoted key");
throwsJson('nul', "not a literal");
throwsJson('[1 2]', "missing comma");

/* --------------------------- adversarial / fuzz-ish ------------------------- */

/* These must not crash — just parse (or throw) cleanly. */
parseCsv('"');
parseCsv('""""');
parseCsv(',,,\n,,,');
parseCsv('\r\n\r\n');
parseCsv('"a""b"c,d');
parseCsv("a".repeat(5000) + ',' + "b".repeat(5000));
assert(true, "adversarial CSV inputs survive");

print("test_docparse: all tests passed (" + n + " assertions)");
