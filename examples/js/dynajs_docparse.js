// dynajs_docparse.js — the native dyna:docparse module (parseJson / parseCsv),
// backed by secure-c-libs. Each call parses into a private SCL arena, deep-
// copies the whole native tree into plain JS values, then destroys the arena;
// the returned value is fully independent (no native pointer escapes).
//
// Requires a build with the module linked in:
//   make CONFIG_SCL_MODULES=y CONFIG_SCL_MODULE_DOCPARSE=y dynajs
// then:  ./dynajs examples/js/dynajs_docparse.js

import { parseJson, parseCsv } from "dyna:docparse";
import { test, run, assert, assertEqual, assertThrows, deepEqual } from "./harness.js";

test("parseJson handles every JSON type", () => {
  const v = parseJson('{"a":1,"b":[2,3],"c":{"d":true,"e":null},"f":"hi","g":2.5}');
  assert(deepEqual(v, { a: 1, b: [2, 3], c: { d: true, e: null }, f: "hi", g: 2.5 }));
  assertEqual(typeof parseJson("42"), "number");
  assertEqual(parseJson("null"), null);
  assertEqual(parseJson('"txt"'), "txt");
});

test("parseJson matches the built-in JSON.parse", () => {
  for (const src of [
    '{"users":[{"id":1,"tags":["x","y"]},{"id":2,"tags":[]}],"total":2}',
    '[1,-2,3.5,true,false,null,"s",{"k":"v"}]',
    '{"unicode":"caf\\u00e9 \\ud83d\\ude00"}',
  ]) {
    assert(deepEqual(parseJson(src), JSON.parse(src)), src);
  }
});

test("parseJson rejects malformed input with a clean SyntaxError", () => {
  for (const bad of ["{", "[1,2", '{"a":}', "tru", "", '{"a":1,,}']) {
    assertThrows(() => parseJson(bad));
  }
  // Deeply nested input is rejected, not a C-stack overflow.
  assertThrows(() => parseJson("[".repeat(5000) + "]".repeat(5000)));
});

test("parseCsv returns an array of arrays", () => {
  assert(deepEqual(parseCsv("a,b,c\n1,2,3\n4,5,6"),
    [["a", "b", "c"], ["1", "2", "3"], ["4", "5", "6"]]));
  // RFC-4180: quoted fields, escaped quotes, embedded commas, CRLF.
  assert(deepEqual(parseCsv('name,note\r\n"a,b","he said ""hi"""\r\n'),
    [["name", "note"], ["a,b", 'he said "hi"']]));
});

test("parseCsv with { header: true } returns an array of objects", () => {
  assert(deepEqual(parseCsv("id,name\n1,ann\n2,bob", { header: true }),
    [{ id: "1", name: "ann" }, { id: "2", name: "bob" }]));
});

await run("dyna:docparse (native parseJson / parseCsv)");
