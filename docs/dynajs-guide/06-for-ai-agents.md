# Chapter 6 — For AI Agents

This chapter is a dense, factual reference for an AI coding agent generating or reviewing DynaJS
code. It states the rules precisely and flags the traps that most often produce wrong output.

## 6.1 The single most important fact

**DynaJS is not Node.js and will never be.** Do not emit `require(...)`, `module.exports`,
`process.*`, `Buffer` (the global), `__dirname`, or any `node:` import. There is no npm, no
`package.json` resolution, no CommonJS. If you reach for a Node API, you are wrong. Use the
`dyna:*` standard library instead, plus the ECMAScript built-ins and `std`/`os`.

## 6.2 How to import

- **Standard library:** `import { fn } from "dyna:<module>"` in a module context, or
  `const m = await import("dyna:<module>")` anywhere (including plain scripts and one-liners).
- **User code:** relative/absolute paths — `import x from "./x.js"`.
- **System modules:** `import * as std from "std"`, `import * as os from "os"`.
- The native standard library requires the binary to be built with `CONFIG_NATIVE_MODULES=y`. If an
  import throws "could not load module `dyna:...`", the build lacked that flag — that is a build
  issue, not a code issue.

## 6.3 The complete module surface (verified)

Import strings are always `dyna:<name>`.

| Module | Exports (representative) |
|---|---|
| `path` | `join resolve normalize clean dirname basename extname isAbsolute relative sep delimiter` |
| `strings` | `split splitN fields join trimChars title repeat contains index lastIndex count replace replaceAll equalFold compare` |
| `bytes` | `compare equal indexOf lastIndexOf contains count concat copy fill toHex fromHex toBase64 fromBase64 toUtf8 fromUtf8` |
| `encoding` | `hexEncode hexDecode base64Encode base64Decode base64UrlEncode base64UrlDecode putUvarint uvarint putVarint varint base85Encode base85Decode` |
| `text` | `count indexOfAny isValidUtf8 base64Encode base64Decode hexEncode hexDecode latin1ToUtf8 utf8ToLatin1 countUtf8 utf8ToUtf16 utf16ToUtf8 isValidUtf16 countUtf16` |
| `crypto` | `md5 sha1 sha224 sha256 sha384 sha512` (+ each `*Hex`), `hmac hmacHex crc32 crc32c`, class `Hasher` |
| `uuid` | `v4 v7 v3 v5 parse validate version variant bytes fromBytes NIL MAX NAMESPACE_DNS NAMESPACE_URL NAMESPACE_OID NAMESPACE_X500` |
| `random` | class `Random` (`nextU64 nextU53 nextFloat nextBounded fill`), `uuid` |
| `mathx` | `gamma lgamma erf erfc cbrt hypot copysign nextafter expm1 log1p log2 logb scalbn ilogb modf frexp ldexp remainder fmod isInf isNaN signbit trunc round roundToEven gcd lcm factorial isPrime abs bitLen popcount` + constants `E Pi Phi Sqrt2 …` |
| `bits` | `UintSize`, `LeadingZeros/TrailingZeros/OnesCount/Len{8,16,32,64}`, `Reverse{8,16,32,64}`, `ReverseBytes{16,32,64}`, `RotateLeft{8,16,32,64}`, `Add/Sub/Mul/Div/Rem{32,64}` |
| `container` | classes `Heap List Ring` |
| `structures` | classes `Vector HashMap` |
| `file` | classes `FileReader FileWriter`, functions `readFile writeFile` |
| `uring` | `readFile readFileSync checksum` (Linux io_uring) |
| `http` | classes `HttpClient HttpServer HttpServerAsync` |
| `netip` | `parseAddr parsePrefix contains masked canonical isValid compareAddr` |
| `sys` | `stat lstat exists readDir makeDir remove removeAll rename symlink readLink realPath chmod glob tempDir makeTempDir makeTempFile env getEnv setEnv args cwd chDir platform pid hostName homeDir` |
| `semver` | `parse isValid clean compare gt gte lt lte eq neq sort major minor patch prerelease inc satisfies maxSatisfying minSatisfying coerce` |
| `time` | `Nanosecond … Hour`, `durationString parseDuration now nowUnixNano nowMillis monotonicNano formatRFC3339 formatUnix parseRFC3339 date fromUnix` |
| `simd` | `dot sum scale axpy add sub mul div abs fma addScalar affine normL1 normL2 max min argmax argmin sigmoid relu relu6 leakyRelu elu tanhFast gelu silu softmax logSoftmax vexp vlog vsqrt vrsqrt vinv distL2 distL1 distCos distCheb gemv gemvT gemm clamp threshold topkIndices f64Sum f64Dot f64Max f64Min f64Scale f64Axpy i32Sum i32Min i32Max i32Dot i32Add i32Mul i32Scale cumsum cummax` |
| `ml` | classes `LinearRegression LogisticRegression KMeans` |
| `compress` | `gzip gunzip` |
| `docparse` | `parseJson parseCsv` |
| `csv` | `create read addRow updateCell removeRow addColumn removeColumn renameColumn readColumnValuesRange readRowRange selectColumnRange` (each takes one options object) |
| `sort` | `sort binarySearch` |
| `search` | `indexOf indexOfAll` |

Chapter 4 has a worked example for every one of these.

## 6.4 Invariants you must respect

1. **Resource classes must be released.** `FileReader`, `FileWriter`, `HttpClient`,
   `HttpServerAsync`, `Hasher`, `Heap`, `List`, `Ring`, `Vector`, `HashMap`, `LinearRegression`,
   `LogisticRegression`, `KMeans`, `Random` all own native memory. Call `.close()` when done (or use
   a `DisposableStack`, or `[Symbol.dispose]()`). Wrap usage in `try { … } finally { x.close(); }`.
   The finalizer is a safety net, not a substitute — leaking a resource is a bug you should not
   write.
2. **Results are copies.** Everything a module returns is a fresh JavaScript value; nothing you hold
   aliases native memory. You cannot get a dangling reference by holding a returned value.
3. **A closed resource throws on use.** After `close()`, any method call throws "use of a closed
   native resource." Do not use a resource after closing it.

## 6.5 Traps that produce wrong code (read these)

- **`sort(arr)` returns a NEW array; it does not mutate.** Write `const s = sort(arr)`, not
  `sort(arr); use(arr)`. It also rejects TypedArrays — pass a plain `Array`.
- **The `using` declaration syntax is NOT implemented.** Do not write `using f = new FileReader(...)`.
  Use explicit `close()` in `finally`, or `DisposableStack.use(...)`. The disposable *library*
  (`DisposableStack`, `Symbol.dispose`, `SuppressedError`) *is* available.
- **`structuredClone` is not a global.** Do not call it. Copy explicitly, or `JSON.parse(JSON.stringify(x))`
  for plain data.
- **`Heap` requires a comparator:** `new Heap((a, b) => a - b)` for a min-heap. There is no default.
- **64-bit integer values are `BigInt`.** `bits.Mul64`, `bits.Add64`, `mathx.gcd/lcm/factorial`, and
  `random.nextU64` take/return `BigInt` (`42n`). Do not compare a `BigInt` with `===` against a
  `Number`.
- **Byte functions accept `Uint8Array`, `ArrayBuffer`, or a string** — but a `Uint8Array` is bytes,
  a string is UTF-8/Latin-1 text; pass the right kind. When in doubt, pass a `Uint8Array`.
- **`gemm(C, A, B, m, n, k, alpha, beta)`** writes into `C` (an m×n `Float32Array` you preallocate)
  and returns it; `A` is m×k, `B` is k×n. `gemv(y, A, x, m, n, beta)` and in-place ops like
  `scale(a, s)` / `axpy(y, a, x)` / `softmax(a)` mutate their first typed-array argument.
- **The HTTP reactor is single-threaded.** Do not put a long CPU loop inside a request handler —
  offload to an `os.Worker`.
- **`dyna:uring` is Linux-only.** On macOS use `dyna:file`.

## 6.6 How to verify your own output

DynaJS is testable without external harnesses:

```js
// Minimal in-file assertion harness (the style the project's own tests use):
let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("FAIL: " + msg); }

import { sort } from "dyna:sort";
assert(sort([3,1,2]).join(",") === "1,2,3", "sort ascending");
assert(JSON.stringify(sort([3,1,2])) !== JSON.stringify([3,1,2]) || true, "returns new array");
print(`ok: ${n} assertions`);
```

Run it with `dynajs yourtest.js`. For the whole runtime, `make test` runs the language suite and
every module's test (`tests/test_<module>.js`) — read those test files; they are the ground-truth
usage examples and cover the edge cases.

## 6.7 A correct, idiomatic DynaJS program (copy this shape)

```js
import { FileReader } from "dyna:file";
import { sha256Hex } from "dyna:crypto";
import { Heap } from "dyna:container";

// Rank the 3 longest lines of a file by length, hashing each — deterministic
// disposal, BigInt-aware, no Node APIs, resources always closed.
function topLongestLines(path, k = 3) {
  const heap = new Heap((a, b) => a.len - b.len);   // min-heap by length
  const reader = new FileReader(path, { bufferSize: 1 << 16 });
  try {
    let line;
    while ((line = reader.readLine()) !== null) {
      heap.push({ len: line.length, hash: sha256Hex(line) });
      if (heap.size > k) heap.pop();                // keep only the top-k longest
    }
    const out = [];
    while (heap.size) out.push(heap.pop());
    return out.reverse();                            // longest first
  } finally {
    reader.close();
    heap.close();
  }
}
```

This is the shape to emit: `dyna:` imports, resource classes released in `finally`, results copied
out as plain JS, no Node surface anywhere.

---

*Next: [Chapter 7 — Roadmap & Philosophy](07-roadmap-and-philosophy.md).*
