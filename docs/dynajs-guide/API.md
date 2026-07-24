# DynaJS API Reference

Complete, per-function documentation for every `dyna:*` standard-library module — the page to keep
open in a tab while you code. If you'd rather learn by example first, [Chapter 4](04-standard-library.md)
is the tutorial-style tour; this is the look-up-the-exact-signature companion.

The final part — [**Built-in prototype extensions (SugarJS + RamdaJS)**](#built-in-prototype-extensions-sugarjs--ramdajs) —
documents the always-on native methods installed on `Array`/`String`/`Number`/`Object`/`Function`/
`Date` and the `Lens` type. These need **no import** (they are part of every build, not `dyna:*`
modules), and the ⚡ marker flags the ones backed by SIMD kernels.

## Conventions used in this reference

**Importing.** Every module is imported by its `dyna:` specifier, either statically in a module
(`import { fn } from "dyna:crypto"`) or dynamically anywhere (`const m = await import("dyna:crypto")`).
The native standard library is present only in a binary built with `CONFIG_NATIVE_MODULES=y`.

**Type notation.** This reference uses TypeScript-style types for clarity (DynaJS is plain
JavaScript; the types are documentation, not enforced syntax):

| Notation | Meaning |
|---|---|
| `number` | a JavaScript IEEE‑754 double. |
| `BigInt` | an arbitrary-precision integer literal, e.g. `42n`. Used wherever a value may exceed 2⁵³ or must be bit-exact. |
| `bytes` | a **bytes-like** value: a `Uint8Array`, an `ArrayBuffer`, or a `string`. A string is interpreted as its UTF‑8 bytes (Latin‑1 where noted). A `Uint8Array` view is read through its backing buffer at the correct offset. |
| `T[]` | a JavaScript `Array` of `T`. |
| `A \| B` | either type. `T?` after a parameter means it is optional. |

**Error model.** On invalid input a function throws a JavaScript `Error` (or a subclass:
`TypeError` for wrong argument types, `RangeError` for out-of-range values/offsets, `SyntaxError`
for malformed text input). The specific subclass and condition are listed per function under
**Throws**. Functions documented as "no throw" return a sentinel (`false`, `null`, `undefined`,
`-1`) instead.

**Return values are copies.** Every value a module returns is a fresh JavaScript value; it never
aliases native memory, so a returned `Uint8Array`/string/object can be held indefinitely.

**Resource classes.** Some modules export classes that own native memory (noted per class). They
must be released explicitly with `.close()`, or `[Symbol.dispose]()`, or a `DisposableStack`.
Release is deterministic (memory returns immediately). After release, every method throws
`TypeError: use of a closed native resource`; the `.closed` getter reports the state. The class
finalizer is a safety net for a leaked object, not the intended release path.

**Argument coercion order.** Every method coerces its JavaScript arguments to native values
*before* it touches its native resource, so passing an adversarial argument (a `valueOf` that
closes the object) fails cleanly rather than corrupting memory.

---

# path

`import { join, resolve, normalize, dirname, basename, extname, isAbsolute, relative, clean, sep, delimiter } from "dyna:path";`

Pure POSIX path-string manipulation. No function touches the filesystem. Separator is `/`.

### `join(...segments)`

Concatenates the given segments with `/` and normalizes the result (collapses `.`, `..`, and
repeated slashes).

| Parameter | Type | Description |
|---|---|---|
| `...segments` | `string` | Zero or more path fragments. Empty strings are ignored. |

**Returns** `string` — the joined, normalized path; `"."` if the joined result is empty.

```js
join("a", "b", "..", "c");   // "a/c"
join("/x/", "y", "z");       // "/x/y/z"
join();                      // "."
```

### `resolve(...segments)`

Resolves the segments into an **absolute** path, processing them right-to-left and prepending the
current working directory until an absolute path is produced.

| Parameter | Type | Description |
|---|---|---|
| `...segments` | `string` | Path fragments, joined from the right. A leading `/` in a segment makes it the anchor. |

**Returns** `string` — an absolute, normalized path.

```js
resolve("/a/b", "../c");     // "/a/c"
resolve("x", "y");           // "<cwd>/x/y"
```

### `normalize(p)` · `clean(p)`

Normalizes a path by collapsing `.` and `..` components and duplicate separators. `clean` is an
alias of `normalize`.

| Parameter | Type | Description |
|---|---|---|
| `p` | `string` | The path to normalize. |

**Returns** `string` — the cleaned path (`"."` for an empty result; a trailing `/` is removed except for the root).

```js
normalize("./a//b/../c");    // "a/c"
normalize("/a/b/../../..");  // "/"
```

### `dirname(p)`

Returns the directory portion of `p` (everything up to, but not including, the final component).

| Parameter | Type | Description |
|---|---|---|
| `p` | `string` | A path. |

**Returns** `string` — the parent directory; `"."` if `p` has no directory part, `"/"` for a root child.

```js
dirname("/usr/local/bin/dynajs");   // "/usr/local/bin"
dirname("file.txt");                // "."
```

### `basename(p, suffix?)`

Returns the final component of `p`, optionally removing a trailing `suffix`.

| Parameter | Type | Description |
|---|---|---|
| `p` | `string` | A path. |
| `suffix` | `string?` | If given and it is a suffix of the base name, it is stripped. |

**Returns** `string` — the last path component.

```js
basename("/x/report.csv");          // "report.csv"
basename("/x/report.csv", ".csv");  // "report"
```

### `extname(p)`

Returns the file extension of the final component, including the leading dot.

| Parameter | Type | Description |
|---|---|---|
| `p` | `string` | A path. |

**Returns** `string` — the extension (e.g. `".csv"`), or `""` if there is none or the name is a dotfile with no other dot.

```js
extname("archive.tar.gz");   // ".gz"
extname(".bashrc");          // ""
```

### `isAbsolute(p)`

| Parameter | Type | Description |
|---|---|---|
| `p` | `string` | A path. |

**Returns** `boolean` — `true` if `p` begins with `/`.

### `relative(from, to)`

Computes the relative path that, resolved against `from`, yields `to`.

| Parameter | Type | Description |
|---|---|---|
| `from` | `string` | The base path. |
| `to` | `string` | The target path. |

**Returns** `string` — a relative path (using `..` as needed); `""` if `from` and `to` are equal.

```js
relative("/a/b/c", "/a/x/y");   // "../../x/y"
```

### Constants

- `sep: string` — the path separator, `"/"`.
- `delimiter: string` — the `PATH`-list delimiter, `":"`.

---

# strings

`import { split, splitN, fields, join, trim, trimStart, trimEnd, trimPrefix, trimSuffix, trimChars, toUpper, toLower, title, repeat, padStart, padEnd, contains, containsAny, hasPrefix, hasSuffix, index, lastIndex, indexAny, count, replace, replaceAll, equalFold, compare } from "dyna:strings";`

Go-flavored string utilities that complement `String.prototype`. Offsets and counts are **byte**
offsets into the UTF‑8 encoding. `contains`/`index`/`count` use the SIMD substring engine on long
inputs.

### `split(s, sep)` · `splitN(s, sep, n)`

Splits `s` around each occurrence of `sep`.

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The string to split. |
| `sep` | `string` | The separator. If `""`, `s` is split into its code points. |
| `n` | `number` | (`splitN` only) the maximum number of pieces; the last piece holds the unsplit remainder. `n < 0` means unlimited. |

**Returns** `string[]` — the pieces.

```js
split("a,b,c", ",");         // ["a","b","c"]
splitN("a,b,c,d", ",", 2);   // ["a","b,c,d"]
```

### `fields(s)`

Splits `s` around runs of whitespace, discarding leading/trailing whitespace.

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The string to split. |

**Returns** `string[]` — the non-empty whitespace-separated fields; `[]` for an all-whitespace or empty string.

```js
fields("  a  b   c ");   // ["a","b","c"]
```

### `join(parts, sep)`

| Parameter | Type | Description |
|---|---|---|
| `parts` | `string[]` | The strings to join. |
| `sep` | `string` | Inserted between each pair. |

**Returns** `string`.

### `trim(s)` · `trimStart(s)` · `trimEnd(s)`

Removes leading and/or trailing whitespace.

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The input. |

**Returns** `string`.

### `trimPrefix(s, prefix)` · `trimSuffix(s, suffix)`

Removes `prefix`/`suffix` from `s` **only if present** (otherwise returns `s` unchanged).

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The input. |
| `prefix` / `suffix` | `string` | The affix to remove. |

**Returns** `string`.

### `trimChars(s, chars)`

Removes any leading and trailing characters that appear in the set `chars`.

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The input. |
| `chars` | `string` | The set of cutset characters. |

**Returns** `string`.

```js
trimChars("xxhixx", "x");   // "hi"
```

### `toUpper(s)` · `toLower(s)` · `title(s)`

Case mapping. `title` upper-cases the first letter of each whitespace-delimited word.

**Returns** `string`.

```js
title("hello world");   // "Hello World"
```

### `repeat(s, count)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The string to repeat. |
| `count` | `number` | Number of repetitions (≥ 0). |

**Returns** `string`. **Throws** `RangeError` if `count` is negative.

### `padStart(s, len, pad)` · `padEnd(s, len, pad)`

Pads `s` to at least `len` characters by repeating `pad` on the start/end.

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The input. |
| `len` | `number` | Target minimum length. |
| `pad` | `string` | The padding unit. |

**Returns** `string` — `s` unchanged if it is already ≥ `len`.

### `contains(s, sub)` · `containsAny(s, chars)`

**Returns** `boolean` — `contains`: whether `sub` occurs in `s` (SIMD). `containsAny`: whether any
character of `chars` occurs in `s`.

### `hasPrefix(s, prefix)` · `hasSuffix(s, suffix)`

**Returns** `boolean`.

### `index(s, sub)` · `lastIndex(s, sub)` · `indexAny(s, chars)`

Finds a byte offset.

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The haystack. |
| `sub` / `chars` | `string` | The needle substring, or (for `indexAny`) the set of characters. |

**Returns** `number` — the first (`index`, `indexAny`) or last (`lastIndex`) matching byte offset, or `-1` if not found. `index` uses SIMD.

### `count(s, sub)`

Counts non-overlapping occurrences of `sub` in `s`.

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The haystack. |
| `sub` | `string` | The needle. If `""`, returns the code-point count of `s` plus 1. |

**Returns** `number`.

### `replace(s, old, new, n)` · `replaceAll(s, old, new)`

Replaces occurrences of `old` with `new`.

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | The input. |
| `old` | `string` | The substring to replace. `""` inserts `new` at every code-point boundary. |
| `new` | `string` | The replacement. |
| `n` | `number` | (`replace` only) replace at most the first `n` occurrences; `n < 0` = all. |

**Returns** `string`.

### `equalFold(a, b)`

**Returns** `boolean` — `true` if `a` and `b` are equal under Unicode simple case-folding.

```js
equalFold("Go", "GO");   // true
```

### `compare(a, b)`

**Returns** `-1 | 0 | 1` — lexicographic comparison of the UTF‑8 byte sequences.

---

# bytes

`import { compare, equal, indexOf, lastIndexOf, contains, count, concat, copy, fill, /* read*, write* */ toHex, fromHex, toBase64, fromBase64, toUtf8, fromUtf8 } from "dyna:bytes";`

Byte-buffer operations over `Uint8Array`, plus fixed-width integer/float accessors in both byte
orders. `indexOf`/`contains`/`count` use the SIMD substring engine.

### `compare(a, b)` · `equal(a, b)`

| Parameter | Type | Description |
|---|---|---|
| `a`, `b` | `bytes` | The buffers to compare. |

**Returns** `compare`: `-1 | 0 | 1` (lexicographic by byte value, shorter-is-less on a common prefix). `equal`: `boolean`.

### `indexOf(buf, sub)` · `lastIndexOf(buf, sub)` · `contains(buf, sub)` · `count(buf, sub)`

Substring search within a byte buffer.

| Parameter | Type | Description |
|---|---|---|
| `buf` | `bytes` | The haystack. |
| `sub` | `bytes` | The needle. |

**Returns** `indexOf`/`lastIndexOf`: `number` (first/last start offset, or `-1`; `indexOf` is SIMD). `contains`: `boolean`. `count`: `number` (non-overlapping occurrences).

### `concat(buffers)`

| Parameter | Type | Description |
|---|---|---|
| `buffers` | `bytes[]` | An array of buffers to join, in order. |

**Returns** `Uint8Array` — a new buffer containing every input byte concatenated.

### `copy(dst, src, dstStart?, srcStart?, srcEnd?)`

Copies a range of `src` into `dst`.

| Parameter | Type | Description |
|---|---|---|
| `dst` | `Uint8Array` | The destination (written in place). |
| `src` | `bytes` | The source. |
| `dstStart` | `number?` | Destination offset. Default `0`. |
| `srcStart` | `number?` | Source start. Default `0`. |
| `srcEnd` | `number?` | Source end (exclusive). Default `src.length`. |

**Returns** `number` — the number of bytes copied (bounded by the space in `dst`).

```js
const d = new Uint8Array(6);
copy(d, new Uint8Array([1,2,3,4]), 1, 0, 3);   // returns 3; d = [0,1,2,3,0,0]
```

### `fill(buf, value, start?, end?)`

Fills a range of `buf` with a byte.

| Parameter | Type | Description |
|---|---|---|
| `buf` | `Uint8Array` | The buffer (written in place). |
| `value` | `number` | The byte value (low 8 bits). |
| `start` | `number?` | Start offset. Default `0`. |
| `end` | `number?` | End offset (exclusive). Default `buf.length`. |

**Returns** `Uint8Array` — `buf`.

### Fixed-width accessors — `read*` / `write*`

Read or write an integer/float at a byte offset in a specified width and byte order. The 8‑bit
forms have no byte order.

**Read** `readTYPE(buf, offset)` → the value. **Write** `writeTYPE(buf, offset, value)` → `number`
(the offset just past the written bytes).

| Family | `TYPE` values | Value type |
|---|---|---|
| 8‑bit | `Uint8`, `Int8` | `number` |
| 16‑bit | `Uint16LE/BE`, `Int16LE/BE` | `number` |
| 32‑bit | `Uint32LE/BE`, `Int32LE/BE` | `number` |
| 64‑bit | `BigUint64LE/BE`, `BigInt64LE/BE` | `BigInt` |
| float | `FloatLE/BE` (32‑bit), `DoubleLE/BE` (64‑bit) | `number` |

| Parameter | Type | Description |
|---|---|---|
| `buf` | `Uint8Array` | The buffer. `write*` mutates it in place. |
| `offset` | `number` | The byte offset. |
| `value` | `number \| BigInt` | (`write*`) the value; `BigInt` for the 64‑bit integer forms. |

**Throws** `RangeError` if `[offset, offset+width)` is out of bounds.

```js
const b = new Uint8Array(8);
writeUint32LE(b, 0, 0xdeadbeef);   // returns 4
readUint32LE(b, 0).toString(16);   // "deadbeef"
```

### Text conversions

- `toHex(buf) → string` / `fromHex(hex: string) → Uint8Array` — lowercase hex. `fromHex` **throws** `SyntaxError` on an odd length or non-hex digit.
- `toBase64(buf) → string` / `fromBase64(s: string) → Uint8Array` — standard base64.
- `toUtf8(buf) → string` / `fromUtf8(s: string) → Uint8Array` — decode/encode UTF‑8.

```js
toHex(new Uint8Array([0xde,0xad]));   // "dead"
toUtf8(fromBase64("aGk="));           // "hi"
```

---

# encoding

`import { hexEncode, hexDecode, base64Encode, base64Decode, base64UrlEncode, base64UrlDecode, base32Encode, base32Decode, base32HexEncode, base32HexDecode, base85Encode, base85Decode, putUvarint, uvarint, putVarint, varint } from "dyna:encoding";`

Binary↔text codecs and LEB128 variable-length integers. Hex and base64 run on the SIMD kernels.

### Hex — `hexEncode(data)` / `hexDecode(s)`

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | (encode) the input bytes. |
| `s` | `string` | (decode) a hex string. |

**Returns** `hexEncode`: `string` (lowercase). `hexDecode`: `Uint8Array`.
**Throws** (`hexDecode`) `SyntaxError` on an odd-length string or a non-hex character.

### Base64 family

`base64Encode/base64Decode` (standard `+/`, padded) and `base64UrlEncode/base64UrlDecode`
(URL-safe `-_`, unpadded).

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | (encode) the input. |
| `s` | `string` | (decode) the encoded text. |

**Returns** encode: `string`; decode: `Uint8Array`. **Throws** `SyntaxError` on invalid input (decode).

### Base32 family

`base32Encode/base32Decode` (RFC 4648 standard alphabet) and `base32HexEncode/base32HexDecode`
(extended-hex alphabet). Same parameter/return/throw shape as the base64 family.

### Ascii85 — `base85Encode(data)` / `base85Decode(s)`

Same shape: `base85Encode(data: bytes) → string`, `base85Decode(s: string) → Uint8Array`.

```js
base85Encode(hexDecode("deadbeef"));   // "hQ=N\\"
```

### Variable-length integers — `putUvarint` / `uvarint` / `putVarint` / `varint`

LEB128 encoding. The `u`-forms are unsigned; the signed forms use zig-zag.

### `putUvarint(value)` · `putVarint(value)`

| Parameter | Type | Description |
|---|---|---|
| `value` | `number \| BigInt` | The integer to encode. `putUvarint` requires a non-negative value. |

**Returns** `Uint8Array` — the encoded bytes (1–10). **Throws** `RangeError` if `putUvarint` is given a negative value, or the value is not a safe integer/BigInt.

### `uvarint(buf)` · `varint(buf)`

| Parameter | Type | Description |
|---|---|---|
| `buf` | `bytes` | A buffer whose prefix holds a var-int. |

**Returns** `[value: number | BigInt, bytesRead: number]`. **Throws** on truncated input or overflow past 64 bits.

```js
const enc = putUvarint(300);   // Uint8Array [0xac, 0x02]
uvarint(enc);                  // [300, 2]
```

---

# text

`import { count, indexOfAny, isValidUtf8, countUtf8, base64Encode, base64Decode, hexEncode, hexDecode, latin1ToUtf8, utf8ToLatin1, utf8ToUtf16, utf16ToUtf8, isValidUtf16, countUtf16 } from "dyna:text";`

Throughput-oriented SIMD text kernels. Inputs are `bytes`; multi-byte results are fresh
`Uint8Array`s.

### `count(data, ch)`

Counts occurrences of a single byte.

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | The buffer. |
| `ch` | `string \| number` | The byte to count: a string's first byte, or a number (low 8 bits). |

**Returns** `number`.

### `indexOfAny(data, chars)`

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | The haystack. |
| `chars` | `bytes` | The set of bytes to look for. |

**Returns** `number` — the first index at which any byte of `chars` occurs, or `-1`.

### `isValidUtf8(data)` · `countUtf8(data)`

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | The buffer to inspect. |

**Returns** `isValidUtf8`: `boolean`. `countUtf8`: `number` (Unicode code-point count of well-formed UTF‑8).

### `base64Encode/Decode`, `hexEncode/Decode`

As in `dyna:encoding`/`dyna:bytes` (SIMD implementations). `base64Decode` returns an
`ArrayBuffer`; `hexDecode` returns a `Uint8Array` and throws `TypeError` on a non-hex character.

### `latin1ToUtf8(data)` · `utf8ToLatin1(data)`

Transcode between Latin‑1 (ISO‑8859‑1) and UTF‑8.

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | Latin‑1 bytes (`latin1ToUtf8`) or UTF‑8 bytes (`utf8ToLatin1`). |

**Returns** `Uint8Array`. **Throws** (`utf8ToLatin1`) `RangeError` if a code point exceeds U+00FF.

### `utf8ToUtf16(data)` · `utf16ToUtf8(u16bytes)`

Lossless transcode between UTF‑8 and UTF‑16LE. **Strict**: malformed input is rejected, never
replaced with U+FFFD.

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | (`utf8ToUtf16`) UTF‑8 input. |
| `u16bytes` | `bytes` | (`utf16ToUtf8`) UTF‑16LE bytes (little-endian `uint16` units). |

**Returns** `Uint8Array` — UTF‑16LE bytes, or UTF‑8 bytes, respectively.
**Throws** `RangeError` — `utf8ToUtf16` on malformed UTF‑8; `utf16ToUtf8` on a lone/misordered surrogate, a high surrogate at end-of-buffer, or an odd byte length.

```js
const u16 = utf8ToUtf16("😀");     // 4 bytes (a surrogate pair)
utf16ToUtf8(u16);                  // the original UTF-8 bytes
```

### `isValidUtf16(u16bytes)` · `countUtf16(u16bytes)`

| Parameter | Type | Description |
|---|---|---|
| `u16bytes` | `bytes` | UTF‑16LE bytes. |

**Returns** `isValidUtf16`: `boolean` (well-formed, all surrogates paired). `countUtf16`: `number` (code-point count).

---

# crypto

`import { md5, sha1, sha224, sha256, sha384, sha512, /* + *Hex */ hmac, hmacHex, crc32, crc32c, Hasher } from "dyna:crypto";`

Cryptographic hashes, HMAC, and CRC. Verified against FIPS 180‑4, RFC 1321/2104/4231, and IEEE
802.3 test vectors. One-shot functions plus a streaming `Hasher` class.

### One-shot digests — `md5` / `sha1` / `sha224` / `sha256` / `sha384` / `sha512`

Each has a bytes form and a `*Hex` form (`sha256` → `sha256Hex`, etc.).

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | The message to hash. |

**Returns** `sha256(...)` → `Uint8Array` (the raw digest); `sha256Hex(...)` → `string` (lowercase hex).
**Throws** `TypeError` if `data` is not bytes-like.

```js
sha256Hex("hello world");
// "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"
```

### `hmac(algo, key, data)` · `hmacHex(algo, key, data)`

Keyed-hash message authentication code.

| Parameter | Type | Description |
|---|---|---|
| `algo` | `string` | The underlying hash: `"md5"`, `"sha1"`, `"sha224"`, `"sha256"`, `"sha384"`, or `"sha512"`. |
| `key` | `bytes` | The secret key. |
| `data` | `bytes` | The message. |

**Returns** `hmac`: `Uint8Array`; `hmacHex`: `string`. **Throws** `TypeError` on an unknown `algo`.

### `crc32(data)` · `crc32c(data)`

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | The input. |

**Returns** `number` — the 32‑bit CRC (IEEE for `crc32`, Castagnoli for `crc32c`). It is signed as a JS number; apply `>>> 0` for the unsigned value.

### `class Hasher`

Incremental hashing for data you do not hold all at once. A resource — call `.close()` when done.

**`new Hasher(algo)`**

| Parameter | Type | Description |
|---|---|---|
| `algo` | `string` | One of the hash names above. |

**Throws** `TypeError` on an unknown algorithm.

**Methods & properties**

| Member | Signature | Description |
|---|---|---|
| `.update(data)` | `(bytes) → this` | Feed more input. Chainable. |
| `.digest()` | `() → Uint8Array` | Finalize and return the raw digest. |
| `.digestHex()` | `() → string` | Finalize and return the hex digest. |
| `.reset()` | `() → this` | Reset to the initial state for reuse. |
| `.algorithm` | `string` (getter) | The configured algorithm name. |
| `.digestSize` | `number` (getter) | The digest length in bytes. |
| `.close()` | `() → void` | Release. Also `[Symbol.dispose]()`. |

```js
const h = new Hasher("sha256");
try { h.update("part one "); h.update("part two"); h.digestHex(); }
finally { h.close(); }
```

---

# uuid

`import { v4, v7, v3, v5, parse, validate, version, variant, bytes, fromBytes, NIL, MAX, NAMESPACE_DNS, NAMESPACE_URL, NAMESPACE_OID, NAMESPACE_X500 } from "dyna:uuid";`

RFC 9562 universally-unique identifiers. Returned UUIDs are canonical lowercase `8-4-4-4-12`.

### `v4()`

**Returns** `string` — a random (version 4) UUID from the OS CSPRNG.

### `v7()`

**Returns** `string` — a time-ordered (version 7) UUID: a 48‑bit big-endian Unix‑millisecond
timestamp in the high bits, the rest random. Successive calls are non-decreasing, so v7 ids sort by
creation time — a good database primary key.

### `v3(namespace, name)` · `v5(namespace, name)`

Deterministic name-based UUIDs (`v3` = MD5, `v5` = SHA‑1).

| Parameter | Type | Description |
|---|---|---|
| `namespace` | `string` | A UUID string — typically one of the `NAMESPACE_*` constants. |
| `name` | `string` | The name within the namespace. |

**Returns** `string` — the derived UUID (identical inputs always yield the same UUID).

```js
v5(NAMESPACE_DNS, "www.example.com");
// "2ed6657d-e927-568b-95e1-2665a8aea6a2"
```

### `parse(s)` · `validate(s)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | A candidate UUID (canonical, `urn:uuid:` prefixed, braced, or 32 raw hex; case-insensitive). |

**Returns** `parse`: `string` — the canonical lowercase form; **throws** `SyntaxError` on a malformed value. `validate`: `boolean` — never throws (non-string → `false`).

### `version(s)` · `variant(s)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | A UUID string. |

**Returns** `number` — the version (1–8) or variant field.

### `bytes(s)` · `fromBytes(u8)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | (`bytes`) a UUID string. |
| `u8` | `Uint8Array` | (`fromBytes`) exactly 16 bytes. |

**Returns** `bytes`: `Uint8Array` (16 bytes). `fromBytes`: `string` (canonical). **Throws** `RangeError` if `u8` is not 16 bytes.

### Constants

`NIL` (all-zero UUID), `MAX` (all-ones), and the four RFC namespace UUIDs `NAMESPACE_DNS`,
`NAMESPACE_URL`, `NAMESPACE_OID`, `NAMESPACE_X500` — all `string`.

---

# random

`import { Random, uuid } from "dyna:random";`

A fast, **seedable** pseudo-random generator (reproducible, unlike `Math.random`).

### `class Random`

**`new Random(seed)`**

| Parameter | Type | Description |
|---|---|---|
| `seed` | `number \| BigInt` | The seed. Equal seeds (as `number` or `BigInt`) produce identical streams. |

**Methods**

| Member | Signature | Description |
|---|---|---|
| `.nextU64()` | `() → BigInt` | The next 64‑bit unsigned integer. |
| `.nextU53()` | `() → number` | The next 53‑bit integer (exactly representable). |
| `.nextFloat()` | `() → number` | A double in `[0, 1)`. |
| `.nextBounded(n)` | `(number) → number` | A uniform integer in `[0, n)` (unbiased). |
| `.fill(buf)` | `(Uint8Array) → Uint8Array` | Fill `buf` with random bytes; returns `buf`. |

`Random` holds only a small integer state; it does not need `.close()`.

```js
const rng = new Random(42);
rng.nextFloat();       // 0.0839...  (deterministic for seed 42)
rng.nextBounded(6);    // a fair die index 0..5
```

### `uuid()`

**Returns** `string` — a random UUID drawn from this module's generator.

---

# mathx

`import { /* real */ gamma, lgamma, erf, erfc, cbrt, hypot, copysign, nextafter, expm1, log1p, log2, logb, scalbn, ilogb, modf, frexp, ldexp, remainder, fmod, isInf, isNaN, signbit, trunc, round, roundToEven, /* int */ gcd, lcm, factorial, isPrime, abs, bitLen, popcount, /* + constants */ } from "dyna:mathx";`

The mathematics `Math` omits: special functions, IEEE‑754 helpers, and exact integer routines. All
real-valued functions take and return `number` unless a tuple is noted.

### Special & elementary functions

| Function | Signature | Description |
|---|---|---|
| `gamma(x)` | `(number) → number` | The gamma function Γ(x). |
| `lgamma(x)` | `(number) → [value, sign]` | log\|Γ(x)\| and the sign of Γ(x) (`±1`). |
| `erf(x)` / `erfc(x)` | `(number) → number` | Error function and its complement. |
| `cbrt(x)` | `(number) → number` | Real cube root. |
| `hypot(x, y)` | `(number, number) → number` | √(x²+y²), computed without overflow. |
| `expm1(x)` / `log1p(x)` | `(number) → number` | eˣ−1 and ln(1+x), accurate near 0. |
| `log2(x)` / `logb(x)` | `(number) → number` | Base‑2 log; the unbiased exponent. |

### IEEE‑754 manipulation

| Function | Signature | Description |
|---|---|---|
| `copysign(x, y)` | `(number, number) → number` | \|x\| with the sign of y. |
| `nextafter(x, y)` | `(number, number) → number` | The next representable double after x toward y. |
| `scalbn(x, n)` / `ldexp(x, n)` | `(number, number) → number` | x·2ⁿ. |
| `ilogb(x)` | `(number) → number` | The exponent as an integer. |
| `modf(x)` | `(number) → [intPart, fracPart]` | Split into integer and fractional parts. |
| `frexp(x)` | `(number) → [frac, exp]` | x = frac·2^exp with frac in [0.5, 1). |
| `remainder(x, y)` / `fmod(x, y)` | `(number, number) → number` | IEEE remainder; C `fmod`. |
| `trunc(x)` / `round(x)` / `roundToEven(x)` | `(number) → number` | Toward zero; away-from-zero (ties); banker's rounding. |
| `isInf(x, sign?)` / `isNaN(x)` / `signbit(x)` | `→ boolean` | Classification. `isInf`'s optional `sign` (`>0`, `<0`) tests a specific infinity. |

### Exact integer routines

| Function | Signature | Description |
|---|---|---|
| `gcd(a, b)` | `(number\|BigInt, number\|BigInt) → BigInt` | Greatest common divisor. |
| `lcm(a, b)` | `(number\|BigInt, number\|BigInt) → BigInt` | Least common multiple. |
| `factorial(n)` | `(number) → BigInt` | n! computed exactly (arbitrary precision). |
| `isPrime(n)` | `(number\|BigInt) → boolean` | Deterministic Miller–Rabin, exact for all uint64. |
| `abs(n)` | `(BigInt) → BigInt` | Absolute value. |
| `bitLen(n)` | `(BigInt) → number` | Number of bits in \|n\|. |
| `popcount(n)` | `(BigInt) → number` | Number of set bits. |

```js
factorial(25);          // 15511210043330985984000000n
isPrime(1000000007n);   // true
gcd(462n, 1071n);       // 21n
```

### Constants

`E`, `Pi`, `Phi`, `Sqrt2`, `SqrtE`, `SqrtPi`, `Ln2`, `Log2E`, `Ln10`, `Log10E` (numbers);
`MaxInt32`, `MinInt32`, `MaxSafeInteger` (numbers); `MaxInt64` (BigInt).

---

# bits

`import { LeadingZeros32, TrailingZeros32, OnesCount32, Len32, Reverse32, ReverseBytes32, RotateLeft32, Add32, Add64, Sub32, Sub64, Mul32, Mul64, Div32, Div64, Rem32, Rem64, UintSize /* + 8/16/64 variants */ } from "dyna:bits";`

Go `math/bits`, at fixed widths of 8, 16, 32, and 64 bits. **The 8/16/32 forms use `number`; the
64‑bit forms take and return `BigInt`.** The counting functions (`LeadingZeros`, `TrailingZeros`,
`OnesCount`, `Len`) always return a `number` (a bit count) even in their 64‑bit form.

### Bit-counting — `LeadingZeros{8,16,32,64}` · `TrailingZeros{8,16,32,64}` · `OnesCount{8,16,32,64}` · `Len{8,16,32,64}`

| Parameter | Type | Description |
|---|---|---|
| `x` | `number` (8/16/32) or `BigInt` (64) | The value, interpreted at the given width. |

**Returns** `number`. `LeadingZeros(0)` = `TrailingZeros(0)` = the width; `Len(0)` = 0.

```js
LeadingZeros32(1);   // 31
OnesCount64(0xffffn);// 16
Len32(1000);         // 10
```

### Bit-manipulation — `Reverse{8,16,32,64}` · `ReverseBytes{16,32,64}` · `RotateLeft{8,16,32,64}`

| Parameter | Type | Description |
|---|---|---|
| `x` | `number` / `BigInt` | The value at the given width. |
| `k` | `number` | (`RotateLeft` only) rotate distance; negative rotates right. |

**Returns** the transformed value (`number` for 8/16/32, `BigInt` for 64). `Reverse` reverses the
bit order; `ReverseBytes` swaps byte order.

```js
Reverse8(19);         // 200   (0b00010011 → 0b11001000)
ReverseBytes32(0x01020304); // 67305985 (0x04030201)
RotateLeft8(1, -1);   // 128
```

### Multi-precision arithmetic — `Add{32,64}` · `Sub{32,64}` · `Mul{32,64}` · `Div{32,64}` · `Rem{32,64}`

Full-width integer arithmetic that surfaces the carry/borrow and the double-width product.

| Function | Signature | Description |
|---|---|---|
| `Add32/Add64(a, b, carry)` | `→ [sum, carryOut]` | Sum with carry-in and carry-out. |
| `Sub32/Sub64(a, b, borrow)` | `→ [diff, borrowOut]` | Difference with borrow. |
| `Mul32/Mul64(a, b)` | `→ [hi, lo]` | Full double-width product. |
| `Div32/Div64(hi, lo, y)` | `→ [quo, rem]` | Divide the double-width `[hi,lo]` by `y`. |
| `Rem32/Rem64(hi, lo, y)` | `→ rem` | The remainder only. |

Operands/results are `number` for the 32‑bit forms and `BigInt` for the 64‑bit forms.
**Throws** `RangeError` from `Div`/`Rem` when `y == 0`, and from `Div` when the quotient overflows
the width.

```js
Mul64(0xffffffffffffffffn, 0xffffffffffffffffn); // [18446744073709551614n, 1n]
Add64(0xffffffffffffffffn, 1n, 0n);              // [0n, 1n]  (sum, carry)
```

### `UintSize`

A `number` constant, `64`.

---

# container

`import { Heap, List, Ring } from "dyna:container";`

Native data structures JavaScript lacks. All three are resource classes — call `.close()` when
done. Element values may be any JavaScript value.

### `class Heap`

A binary heap / priority queue ordered by a user comparator.

**`new Heap(compare)`**

| Parameter | Type | Description |
|---|---|---|
| `compare` | `(a, b) => number` | **Required.** Returns `<0` if `a` should come out before `b`. `(a,b)=>a-b` is a min-heap; `(a,b)=>b-a` a max-heap. |

**Throws** `TypeError` if `compare` is not a function.

| Member | Signature | Description |
|---|---|---|
| `.push(value)` | `(any) → void` | Insert a value. |
| `.pop()` | `() → any` | Remove and return the highest-priority element (`undefined` if empty). |
| `.peek()` | `() → any` | Return, without removing, the highest-priority element. |
| `.size` | `number` (getter) | The element count. |
| `.close()` | `() → void` | Release. Also `[Symbol.dispose]()`. |

The comparator is invoked during `push`/`pop`; it may run arbitrary JavaScript safely (a comparator
that closes the heap or mutates it cannot corrupt native memory).

```js
const pq = new Heap((a, b) => a - b);
[5,1,4,2,8].forEach(v => pq.push(v));
const out = []; while (pq.size) out.push(pq.pop());   // [1,2,4,5,8]
pq.close();
```

### `class List`

A doubly-linked list, i.e. a deque with O(1) operations at both ends.

**`new List()`**

| Member | Signature | Description |
|---|---|---|
| `.pushFront(value)` / `.pushBack(value)` | `(any) → void` | Insert at the front/back. |
| `.popFront()` / `.popBack()` | `() → any` | Remove and return from the front/back (`undefined` if empty). |
| `.front()` / `.back()` | `() → any` | Peek at the front/back. |
| `.toArray()` | `() → any[]` | A front-to-back snapshot array. |
| `[Symbol.iterator]()` | | Iterate front to back (`for...of`). |
| `.length` | `number` (getter) | Element count. |
| `.close()` | `() → void` | Release. |

### `class Ring`

A fixed-capacity circular buffer; pushing into a full ring overwrites the oldest element.

**`new Ring(capacity)`**

| Parameter | Type | Description |
|---|---|---|
| `capacity` | `number` | Maximum retained elements (≥ 1). |

| Member | Signature | Description |
|---|---|---|
| `.push(value)` | `(any) → void` | Append; evicts the oldest if full. |
| `.get(i)` | `(number) → any` | The i‑th element (0 = oldest retained). |
| `.toArray()` | `() → any[]` | Oldest-to-newest snapshot. |
| `.length` | `number` (getter) | Current element count (≤ capacity). |
| `.capacity` | `number` (getter) | The fixed capacity. |
| `.close()` | `() → void` | Release. |

```js
const r = new Ring(3);
[1,2,3,4,5].forEach(v => r.push(v));
r.toArray();   // [3,4,5]  (oldest two evicted)
r.close();
```

---

# structures

`import { Vector, HashMap } from "dyna:structures";`

A growable array and a hash map with native contiguous backing. Resource classes.

### `class Vector`

| Member | Signature | Description |
|---|---|---|
| `new Vector()` | | Create an empty vector. |
| `.push(value)` | `(any) → number` | Append; returns the new length. |
| `.get(i)` | `(number) → any` | Element at `i`, or `undefined` if out of range. |
| `.set(i, value)` | `(number, any) → void` | Overwrite element `i`. |
| `.pop()` | `() → any` | Remove and return the last element. |
| `.length` | `number` (getter) | Element count. |
| `.close()` | `() → void` | Release. |

### `class HashMap`

Keys are JavaScript values compared by identity/value as appropriate.

| Member | Signature | Description |
|---|---|---|
| `new HashMap()` | | Create an empty map. |
| `.set(key, value)` | `(any, any) → void` | Insert or update. |
| `.get(key)` | `(any) → any` | The value, or `undefined`. |
| `.has(key)` | `(any) → boolean` | Membership test. |
| `.delete(key)` | `(any) → boolean` | Remove; returns whether a key was removed. |
| `.size` | `number` (getter) | Entry count. |
| `.close()` | `() → void` | Release. |

---

# sys

`import { env, getEnv, setEnv, args, cwd, chDir, platform, pid, hostName, homeDir } from "dyna:sys";`

Process and environment access. **The filesystem surface — metadata, directories, links, globbing
and temp files — moved to [`dyna:file`](#file)**, which now owns all filesystem operations alongside
buffered content I/O; path-string logic is in [`dyna:path`](#path). On failure, functions throw an
`Error` whose `.code` (e.g. `"ENOENT"`) and `.errno` identify the OS error.

### Process & environment

| Function | Signature | Description |
|---|---|---|
| `env()` | `() → object` | All environment variables as an object. |
| `getEnv(name)` | `(string) → string \| undefined` | One variable. |
| `setEnv(name, value)` | `(string, string) → void` | Set a variable. |
| `args()` | `() → string[]` | The process argument vector. |
| `cwd()` / `chDir(path)` | | Get / set the working directory. |
| `platform()` | `() → string` | `"darwin"`, `"linux"`, or `"unknown"`. |
| `pid()` | `() → number` | The process id. |
| `hostName()` | `() → string` | The host name. |
| `homeDir()` | `() → string` | The current user's home directory. |

---

# file

`import { readFile, writeFile, FileReader, FileWriter, stat, lstat, exists, readDir, makeDir, remove, removeAll, rename, symlink, readLink, realPath, chmod, glob, tempDir, makeTempDir, makeTempFile } from "dyna:file";`

The filesystem module: buffered file content I/O (with per-OS fast paths — macOS
`F_RDAHEAD`/`F_PREALLOCATE`/`F_FULLFSYNC`; Linux `fadvise`/`fallocate`/io_uring) **plus all
filesystem operations** (metadata, directories, links, globbing, temp files — moved here from
`dyna:sys`). Path-string logic (join/normalize/dirname/…) is in [`dyna:path`](#path); process and
environment access is in [`dyna:sys`](#sys). Filesystem functions throw an `Error` whose `.code`
(e.g. `"ENOENT"`) and `.errno` identify the OS error.

### `readFile(path)` · `writeFile(path, data)`

One-shot whole-file helpers.

| Parameter | Type | Description |
|---|---|---|
| `path` | `string` | The file. |
| `data` | `string \| bytes` | (`writeFile`) the content to write. |

**Returns** `readFile`: `string` (the file content). `writeFile`: `number` (bytes written). **Throws** on I/O error.

### `class FileReader`

Buffered sequential reader.

**`new FileReader(path, options?)`**

| Parameter | Type | Description |
|---|---|---|
| `path` | `string` | The file to open for reading. |
| `options` | `{ bufferSize? }?` | Read-buffer size in bytes. |

| Member | Signature | Description |
|---|---|---|
| `.read()` | `() → string \| null` | The next buffered chunk, or `null` at EOF. |
| `.readLine()` | `() → string \| null` | The next line **without** its newline, or `null` at EOF. Handles buffer refills. |
| `.readAll()` | `() → string` | The rest of the file. |
| `.close()` | `() → void` | Release. |

### `class FileWriter`

Buffered sequential writer.

**`new FileWriter(path, options?)`**

| Parameter | Type | Description |
|---|---|---|
| `path` | `string` | The file to open/create for writing. |
| `options` | `{ bufferSize?, preallocate? }?` | Write-buffer size; `preallocate` reserves a file extent (fewer syscalls, less fragmentation). |

| Member | Signature | Description |
|---|---|---|
| `.write(data)` | `(string \| bytes) → number` | Buffer bytes for writing; returns the count. |
| `.flush()` | `() → void` | Flush the buffer to the OS. |
| `.sync()` | `() → void` | Durable flush to disk (`F_FULLFSYNC` on macOS). |
| `.close()` | `() → void` | Flush and release. |

```js
const w = new FileWriter("/tmp/out", { bufferSize: 1 << 16, preallocate: 1 << 20 });
try { for (let i = 0; i < 100000; i++) w.write(`row ${i}\n`); w.sync(); }
finally { w.close(); }
```

### `stat(path)` · `lstat(path)`

Return file metadata. `lstat` does not follow a final symlink.

| Parameter | Type | Description |
|---|---|---|
| `path` | `string` | The target path. |

**Returns** an object: `{ size, mode, isDir, isFile, isSymlink, mtimeMs, atimeMs, ctimeMs, uid, gid, ino, nlink }`. `mode` is the full Unix `st_mode`; times are milliseconds (float). **Throws** on a missing path.

### `exists(path)`

**Returns** `boolean` — whether the path exists (uses `lstat`, so `true` even for a dangling symlink). Does **not** throw.

### `readDir(path)`

| Parameter | Type | Description |
|---|---|---|
| `path` | `string` | A directory. |

**Returns** `{ name, isDir, isFile, isSymlink }[]` — entries sorted by name, excluding `.` and `..`. **Throws** if `path` is not a readable directory.

### `makeDir(path, options?)` · `remove(path)` · `removeAll(path)` · `rename(from, to)`

| Function | Parameters | Behavior |
|---|---|---|
| `makeDir(path, { recursive?, mode? })` | `string`, options | Create a directory. `recursive: true` creates parents. `mode` sets permissions. |
| `remove(path)` | `string` | Remove a file or empty directory. Throws if a directory is non-empty. |
| `removeAll(path)` | `string` | Recursively remove `path`. Symlink-safe (never deletes through a symlink out of the tree). A missing `path` is a **no-op** (no throw). |
| `rename(from, to)` | `string, string` | Rename/move. |

### `symlink(target, linkPath)` · `readLink(path)` · `realPath(path)` · `chmod(path, mode)`

| Function | Signature | Description |
|---|---|---|
| `symlink(target, linkPath)` | `(string, string) → void` | Create `linkPath` pointing at `target`. |
| `readLink(path)` | `(string) → string` | The target of a symlink. |
| `realPath(path)` | `(string) → string` | The canonicalized absolute path (resolving symlinks). |
| `chmod(path, mode)` | `(string, number) → void` | Set permission bits (e.g. `0o755`). |

### `glob(pattern, options?)`

Expand a shell-style glob against the filesystem.

| Parameter | Type | Description |
|---|---|---|
| `pattern` | `string` | Supports `*` (not crossing `/`), `**` (crossing directories), `?`, and `[...]` character classes (ranges and `[!...]` negation). |
| `options` | `{ cwd? }?` | `cwd` sets the base for a relative pattern. |

**Returns** `string[]` — matching paths, sorted and de-duplicated. Symlink cycles terminate (never infinite-loop).

```js
glob("src/**/*.js");     // every .js under src, at any depth
glob("data/file-[0-9].csv");
```

### Temp paths — `tempDir()` · `makeTempDir(prefix)` · `makeTempFile(prefix)`

| Function | Signature | Description |
|---|---|---|
| `tempDir()` | `() → string` | The OS temporary directory. |
| `makeTempDir(prefix)` | `(string) → string` | Atomically create and return a unique temp directory. |
| `makeTempFile(prefix)` | `(string) → string` | Create and return a unique temp file path. |

---

# uring

`import { readFile, readFileSync, checksum } from "dyna:uring";`

High-queue-depth bulk file reads via Linux **io_uring**. Available only on Linux builds with
io_uring support; on other platforms use `dyna:file`.

| Function | Signature | Description |
|---|---|---|
| `readFile(path)` | `(string) → Uint8Array` | Whole-file read submitted through io_uring (many outstanding reads, minimal syscalls). |
| `readFileSync(path)` | `(string) → Uint8Array` | Synchronous whole-file read. |
| `checksum(path)` | `(string) → number` | Streamed CRC over the file at high queue depth. |

**Throws** on I/O error, or if the ring cannot be created (e.g. seccomp-restricted environment).

---

# http

`import { App, HttpClient, HttpServerAsync, HttpServer } from "dyna:http";`

An HTTP/1.1 client (`HttpClient`), an application server (`App`), and two low-level static server
implementations (`HttpServerAsync`, `HttpServer`). **`App` is the server you build on** — it runs
your JavaScript handlers on a single-thread event-loop reactor (kqueue on macOS; epoll or io_uring
on Linux). `HttpServerAsync` is the bare reactor: it serves fixed responses only and never enters
the JavaScript world.

### `class HttpClient`

**`new HttpClient()`**

| Member | Signature | Description |
|---|---|---|
| `.get(url)` | `(string) → Response` | Perform a GET. |
| `.post(url, body, headers?)` | `(string, string\|bytes, object?) → Response` | POST with a body and optional headers. |
| `.request(url, options)` | `(string, object) → Response` | A general request (`{ method, headers, body }`). |
| `.setTimeout(ms)` | `(number) → void` | Set the request timeout. |
| `.disconnect()` | `() → void` | Drop the underlying connection. |
| `.close()` | `() → void` | Release the client. |

**`Response`** = `{ status: number, headers: object, body: string }`.

### `class App`

The application server. You register **typed routes** — there is deliberately no raw request
handler — and `App` runs your handlers on the single-thread event-loop reactor. A resource: call
`.close()` when done.

**`new App(config)`**

| Field | Type | Description |
|---|---|---|
| `config.port` | `number` | The listen port. **Required in practice** — unlike `HttpServerAsync`, `.port` reports this value, so `port: 0` is not resolved to an OS-assigned port. |

| Member | Signature | Description |
|---|---|---|
| `.rpc(path, methods)` | `(string, object) → void` | Register a strict **JSON-RPC 2.0** endpoint. `methods` maps a method name to `(params) => result`. See below. |
| `.static(prefix, dir, opts?)` | `(string, string, object?) → void` | Serve files under `prefix` from `dir` via zero-copy `sendfile`. `opts`: `{ maxFileSize?: number, allow?: string[] }` where `allow` is a whitelist of `.ext`s or MIME types. |
| `.upload(path, opts, handler)` | `(string, object, function) → void` | Stream an upload to disk, then call `handler(savedPath, meta)`. `opts`: `{ dir (required), maxFileSize?, allow? }`; `meta` is `{ size, contentType }`. |
| `.ws(path, handlers)` | `(string, object) → void` | Register an **RFC 6455** WebSocket endpoint. `handlers`: `{ open(ws), message(ws, data, isBinary), close(ws, code, reason) }`. |
| `.start()` | `() → void` | Bind, listen, and fold the reactor into this thread's event loop. |
| `.port` | `number` (getter) | The configured port. |
| `.close()` | `() → void` | Stop and release. Also `[Symbol.dispose]()`. |

**RPC contract.** Each method receives the request's JSON-RPC `params` as its single argument and
returns the `result`. Throwing produces a JSON-RPC error object (code `-32000`); an unknown method
yields `-32601`; a malformed request yields `-32600`/`-32700`. A method may return a value
synchronously *or* a `Promise` for a **single** request; a **batch** request requires synchronous
methods (an async handler in a batch is rejected).

**WebSocket connection (`ws`).** The object passed to the handlers has `.send(data)` — a `string`
is sent as a text frame, an `ArrayBuffer` as a binary frame — and `.close()`, which sends a close
frame and tears the connection down.

**Threading.** Handlers run on the JS thread, so they share your program's heap with no
cross-thread copy — but a blocking handler stalls the whole reactor (offload heavy CPU work to an
`os.Worker`), and a same-thread blocking `HttpClient` cannot call the same process's `App`. Drive an
`App` from another process.

```js
const app = new App({ port: 8080 });
app.rpc("/rpc", { add: ([a, b]) => a + b });
app.static("/assets", "/var/www", { allow: [".css", ".png"] });
app.start();
// curl -sd '{"jsonrpc":"2.0","id":1,"method":"add","params":[2,3]}' :8080/rpc
//   → {"jsonrpc":"2.0","result":5,"id":1}
```

### `class HttpServerAsync`

A low-level reactor that maps paths to **fixed responses** — it never runs JavaScript, so it is not
a substitute for `App`. Useful for static content and for exercising the reactor's raw concurrency.

**`new HttpServerAsync(config)`**

| Field | Type | Description |
|---|---|---|
| `config.port` | `number` | The listen port; `0` picks a free port (read it back from `.port`). |
| `config.routes` | `object` | Maps a path string to either a body `string` or `{ status, contentType, body }`. Unmatched paths return 404. |

| Member | Signature | Description |
|---|---|---|
| `.start()` | `() → void` | Begin listening/serving. |
| `.stop()` | `() → void` | Stop and release. |
| `.port` | `number` (getter) | The bound port. |

Routes are fixed at construction; there are no callbacks to run, so nothing user-supplied executes
per request. (For dynamic behavior, use `App`.)

```js
const s = new HttpServerAsync({ port: 0, routes: { "/": "hello\n" } });
s.start();
const c = new HttpClient();
c.get(`http://127.0.0.1:${s.port}/`).status;   // 200
s.stop();
```

### `class HttpServer`

A thread-pool server variant with the same `.start()`, `.stop()`, and `.port` surface. Prefer
`HttpServerAsync` for high connection concurrency.

---

# netip

`import { parseAddr, parsePrefix, contains, masked, canonical, isValid, compareAddr, isLoopback, isPrivate, isMulticast, isUnspecified, isLinkLocalUnicast, isGlobalUnicast, isLinkLocalMulticast } from "dyna:netip";`

IPv4/IPv6 address and CIDR-prefix parsing and reasoning (modeled on Go `net/netip`).

### `parseAddr(s)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | An IPv4 dotted-quad, an IPv6 address (compressed or full), or an IPv4-mapped IPv6 address. |

**Returns** `{ is4: boolean, is6: boolean, bytes: Uint8Array, string: string }` — `bytes` is 4 or 16 bytes; `string` is the canonical form. **Throws** `SyntaxError` on an invalid address. Note: an IPv4-mapped IPv6 address (`"::ffff:1.2.3.4"`) parses as an **IPv6** value (`is4: false`, 16 bytes); the classifiers below unmap it first.

### `parsePrefix(s)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | A CIDR prefix, e.g. `"10.0.0.0/8"` or `"2001:db8::/32"`. |

**Returns** `{ addr, bits: number }`. **Throws** on an invalid prefix or out-of-range length.

### `contains(prefix, addr)`

| Parameter | Type | Description |
|---|---|---|
| `prefix` | `string` | A CIDR prefix. |
| `addr` | `string` | An address. |

**Returns** `boolean` — whether `addr` falls within `prefix` (comparing only the masked bits).

### `masked(prefix)` · `canonical(addr)` · `isValid(s)` · `compareAddr(a, b)`

| Function | Signature | Description |
|---|---|---|
| `masked(prefix)` | `(string) → string` | The network address with host bits zeroed. |
| `canonical(addr)` | `(string) → string` | The RFC 5952 canonical form (lowercase, `::`-compressed). |
| `isValid(s)` | `(string) → boolean` | Validity test (no throw). |
| `compareAddr(a, b)` | `(string, string) → -1\|0\|1` | Ordering of two addresses. |

### Classifiers

Each takes an address string and returns a `boolean`: `isLoopback`, `isPrivate`, `isMulticast`,
`isUnspecified`, `isLinkLocalUnicast`, `isGlobalUnicast`, `isLinkLocalMulticast`. All except
`isUnspecified` first unmap an IPv4-in-IPv6 address (so `isLoopback("::ffff:127.0.0.1")` is `true`).

```js
contains("10.0.0.0/8", "10.1.2.3");   // true
canonical("2001:0db8:0000:0000:0000:0000:0000:0001"); // "2001:db8::1"
isPrivate("192.168.1.1");             // true
```

---

# time

`import { Nanosecond, Microsecond, Millisecond, Second, Minute, Hour, durationString, parseDuration, now, nowUnixNano, nowMillis, monotonicNano, formatRFC3339, parseRFC3339, formatUnix, date, fromUnix } from "dyna:time";`

Nanosecond-precision durations, a monotonic clock, and RFC 3339 formatting (modeled on Go `time`).

### Duration constants

`Nanosecond`, `Microsecond`, `Millisecond`, `Second`, `Minute`, `Hour` — each the number of
nanoseconds in that unit (used to build durations, e.g. `90 * Number(Minute)`).

### `durationString(ns)` · `parseDuration(s)`

| Function | Signature | Description |
|---|---|---|
| `durationString(ns)` | `(number\|BigInt) → string` | Format a nanosecond duration, e.g. `"1h30m0s"`. |
| `parseDuration(s)` | `(string) → number\|BigInt` | Parse a duration string (`"1h30m"`, `"500ms"`, `"1.5s"`) to nanoseconds. |

### Clocks

| Function | Signature | Description |
|---|---|---|
| `now()` | `() → number` | Current Unix time in milliseconds. |
| `nowUnixNano()` | `() → BigInt` | Current Unix time in nanoseconds. |
| `nowMillis()` | `() → number` | Current Unix time in milliseconds. |
| `monotonicNano()` | `() → BigInt` | A monotonic clock reading (nanoseconds); never decreases — use it for elapsed-time and timeouts. |

### Formatting & construction

| Function | Signature | Description |
|---|---|---|
| `formatRFC3339(t)` | `→ string` | RFC 3339 / ISO‑8601 timestamp. |
| `parseRFC3339(s)` | `(string) → …` | Parse an RFC 3339 timestamp. |
| `formatUnix(sec, layout)` | `(number, string) → string` | Format a Unix-second time using Go layout tokens. |
| `date(year, month, day)` | | Construct a time value from calendar fields. |
| `fromUnix(sec, nsec?)` | | Construct a time value from Unix seconds (+ optional nanoseconds). |

```js
durationString(90 * Number(Minute));    // "1h30m0s"
parseDuration("1h30m");                  // 5400000000000
formatRFC3339(fromUnix(0, 0));           // "1970-01-01T00:00:00Z"

const t0 = monotonicNano();
/* work */
const ms = Number(monotonicNano() - t0) / 1e6;
```

---

# semver

`import { parse, isValid, clean, compare, gt, gte, lt, lte, eq, neq, sort, major, minor, patch, prerelease, inc, satisfies, maxSatisfying, minSatisfying, coerce } from "dyna:semver";`

Semantic Versioning 2.0.0 with npm-style ranges.

### `parse(v)` · `isValid(v)` · `clean(v)`

| Parameter | Type | Description |
|---|---|---|
| `v` | `string` | A version string. |

**Returns** `parse`: `{ major, minor, patch, prerelease: (string|number)[], build: string[], version: string }`; **throws** `SyntaxError` on an invalid version. `isValid`: `boolean` (no throw). `clean`: `string | null` — a normalized version (strips a leading `v`/whitespace), or `null` if not coercible to strict form.

### Comparison

| Function | Signature | Description |
|---|---|---|
| `compare(a, b)` | `(string, string) → -1\|0\|1` | SemVer precedence. A version with a prerelease is **lower** than the release; build metadata is ignored. |
| `gt` `gte` `lt` `lte` `eq` `neq` | `(string, string) → boolean` | The obvious comparisons. |
| `sort(versions)` | `(string[]) → string[]` | A new array sorted ascending by precedence. |

Precedence follows the spec §11 chain, e.g.
`1.0.0-alpha < 1.0.0-alpha.1 < 1.0.0-alpha.beta < 1.0.0-beta < 1.0.0-beta.2 < 1.0.0-beta.11 < 1.0.0-rc.1 < 1.0.0`.

### Field accessors & increment

| Function | Signature | Description |
|---|---|---|
| `major(v)` / `minor(v)` / `patch(v)` | `(string) → number` | The numeric fields. |
| `prerelease(v)` | `(string) → (string\|number)[]` | The prerelease identifiers. |
| `inc(v, release)` | `(string, string) → string` | Increment. `release` ∈ `"major" \| "minor" \| "patch" \| "premajor" \| "preminor" \| "prepatch" \| "prerelease"`. |

### Ranges

| Function | Signature | Description |
|---|---|---|
| `satisfies(version, range)` | `(string, string) → boolean` | Whether `version` satisfies the npm `range`. |
| `maxSatisfying(versions, range)` | `(string[], string) → string \| null` | The highest satisfying version, or `null`. |
| `minSatisfying(versions, range)` | `(string[], string) → string \| null` | The lowest satisfying version. |
| `coerce(s)` | `(string) → string \| null` | Best-effort extract a valid version from loose text. |

`range` supports the full npm grammar: exact versions; comparators `>` `>=` `<` `<=` `=`; caret
`^1.2.3` (with the special 0.x rules); tilde `~1.2.3`; hyphen `1.2.3 - 2.3.4`; x-ranges `1.x` /
`1.2.*` / `*`; conjunction (space) and disjunction (`||`). A prerelease version only satisfies a
range whose comparator set carries a prerelease at the same `major.minor.patch`.

```js
satisfies("1.2.9", "^1.2.3");   // true
satisfies("0.3.0", "^0.2.3");   // false  (^0.2.3 := >=0.2.3 <0.3.0)
inc("1.2.3", "minor");          // "1.3.0"
maxSatisfying(["1.0.0","1.2.0","1.9.0","2.0.0"], "^1.2.0");   // "1.9.0"
```

---

# simd

`import { /* many */ } from "dyna:simd";`

Multi-ISA vector math dispatched at runtime to the best instruction set available (scalar / NEON /
SSE4.2 / AVX2 / AVX‑512 / SVE). Operands are typed arrays. **In-place** operations mutate and return
their first array argument; **output** operations write into a caller-provided destination and
return it. Reductions/products return a scalar.

> AVX2/AVX‑512 variants of the f64/i32/UTF‑16 kernels are currently gated off pending
> AVX-hardware verification; those machines transparently run the verified SSE4.2/AVX2 kernels.

### f32 reductions — `(x: Float32Array) → number`

`sum`, `max`, `min`, `normL1` (Σ\|xᵢ\|), `normL2` (√Σxᵢ²), `argmax`, `argmin` (index of the extreme).

### f32 pairwise — `(a: Float32Array, b: Float32Array) → number`

`dot` (Σ aᵢbᵢ), `distL1`, `distL2`, `distCos` (cosine **distance** = 1 − cosine similarity),
`distCheb` (Chebyshev / L∞).

### f32 elementwise

| Function | Signature | Effect |
|---|---|---|
| `add`/`sub`/`mul`/`div` | `(z, a, b) → z` | `z = a ⊙ b`, elementwise, into `z`. |
| `fma` | `(z, a, b) → z` | `z = z + a·b`. |
| `abs` | `(out, in) → out` | `out = \|in\|`. |
| `scale` | `(a, s: number) → a` | `a *= s`, in place. |
| `addScalar` | `(a, s: number) → a` | `a += s`, in place. |
| `affine` | `(a, s: number, b: number) → a` | `a = a·s + b`, in place. |
| `clamp` | `(a, lo: number, hi: number) → a` | Clamp each element into `[lo, hi]`. |
| `threshold` | `(a, t: number) → a` | Zero elements below `t`. |

### f32 BLAS

| Function | Signature | Effect |
|---|---|---|
| `axpy(y, alpha, x)` | `(Float32Array, number, Float32Array) → y` | `y += alpha·x`. |
| `gemv(y, A, x, m, n, beta)` | `→ y` | `y = A·x + beta·y`; `A` is m×n row-major. |
| `gemvT(y, A, x, m, n, beta)` | `→ y` | As `gemv` but with Aᵀ. |
| `gemm(C, A, B, m, n, k, alpha, beta)` | `→ C` | `C = alpha·A·B + beta·C`; `A` is m×k, `B` is k×n, `C` is m×n (all row-major). |

### f32 activations / transcendentals (in place, `(x: Float32Array) → x`)

`sigmoid`, `relu`, `relu6`, `tanhFast`, `gelu`, `silu`, `softmax`, `logSoftmax`, `vexp`, `vlog`,
`vsqrt`, `vrsqrt`, `vinv`. Two take a parameter: `leakyRelu(x, slope)`, `elu(x, alpha)`. And
`topkIndices(x, k) → number[]` returns the indices of the `k` largest elements.

### f64 — `(Float64Array)`

`f64Sum(x) → number`, `f64Dot(a, b) → number`, `f64Max(x) → number`, `f64Min(x) → number`,
`f64Scale(x, s) → x` (in place), `f64Axpy(y, a, x) → y` (`y += a·x`).

### i32 — `(Int32Array)`

`i32Sum(x) → number` (widened to int64, exact for \|Σ\| ≤ 2⁵³), `i32Min(x)`/`i32Max(x) → number`,
`i32Dot(a, b) → number`, `i32Add(z, a, b)`/`i32Mul(z, a, b) → z` (two's-complement wrap, like
`Math.imul`), `i32Scale(x, s) → x`.

### Prefix scans

`cumsum(x) → x` (inclusive prefix sum) and `cummax(x) → x` (running maximum), each accepting an
`Int32Array` or `Float32Array`, in place.

```js
dot(new Float32Array([1,2,3,4]), new Float32Array([5,6,7,8]));   // 70
const g = new Float32Array([2,1,0.1]); softmax(g);               // → probabilities
const c = new Int32Array([1,2,3,4]); cumsum(c);                  // c = [1,3,6,10]
```

---

# ml

`import { LinearRegression, LogisticRegression, KMeans } from "dyna:ml";`

Classic models implemented natively on the SIMD engine. All are resource classes; `.fit()` returns
`this` for chaining; call `.close()` when done.

### `class LinearRegression`

| Member | Signature | Description |
|---|---|---|
| `new LinearRegression()` | | Create an unfitted model. |
| `.fit(X, y)` | `(number[][], number[]) → this` | Fit to feature rows `X` and targets `y`. |
| `.predict(X)` | `(number[][]) → number[]` | Predict for each feature row. |
| `.close()` | `() → void` | Release. |

### `class LogisticRegression`

| Member | Signature | Description |
|---|---|---|
| `new LogisticRegression()` | | Create an unfitted model. |
| `.fit(X, y)` | `(number[][], number[]) → this` | Fit to features and binary labels. |
| `.predict(X)` | `(number[][]) → number[]` | Predicted class labels. |
| `.predictProba(X)` | `(number[][]) → number[]` | Predicted probabilities. |
| `.close()` | `() → void` | Release. |

### `class KMeans`

| Member | Signature | Description |
|---|---|---|
| `new KMeans(k, seed?)` | `(number, number?)` | `k` cluster count; optional RNG `seed` for reproducibility. |
| `.fit(points)` | `(number[][]) → this` | Cluster the points. |
| `.predict(points)` | `(number[][]) → number[]` | The nearest cluster index for each point. |
| `.inertia` | `number` (getter) | The within-cluster sum of squared distances after `fit`. |
| `.close()` | `() → void` | Release. |

```js
const lr = new LinearRegression();
try { lr.fit([[0],[1],[2]], [1,3,5]); lr.predict([[10]]); }   // ≈ [21]  (y = 2x+1)
finally { lr.close(); }
```

---

# compress

`import { gzip, gunzip } from "dyna:compress";`

DEFLATE-based compression (a real implementation, gzip framing).

| Function | Signature | Description |
|---|---|---|
| `gzip(data)` | `(bytes) → Uint8Array` | Compress. |
| `gunzip(data)` | `(bytes) → Uint8Array` | Decompress. **Throws** on corrupt input. |

```js
const z = gzip("the quick brown fox ".repeat(100));
gunzip(z).length;   // the original length
```

---

# docparse

`import { parseJson, parseCsv } from "dyna:docparse";`

Native parsers for the two common data interchange formats.

| Function | Signature | Description |
|---|---|---|
| `parseJson(s)` | `(string) → any` | Parse a JSON document to a JavaScript value. **Throws** `SyntaxError` on invalid JSON. |
| `parseCsv(s)` | `(string) → string[][]` | Parse RFC 4180 CSV (quoted fields, embedded commas/newlines/quotes) into an array of rows, each an array of string cells. |

```js
parseCsv("a,b\n1,2\n3,4");   // [["a","b"],["1","2"],["3","4"]]
parseJson('{"x":[1,2,3]}').x[2];   // 3
```

---

# csv

`import { CSVFile } from "dyna:csv";`

File-oriented CSV create/read/update/delete, RFC 4180 (quoted fields, embedded commas/newlines/quotes,
`""` escaping). The module exports one class, **`CSVFile`**, whose constructor binds a file path; every
operation is a method on that instance and takes a single **options object** (the `path` is passed
once, to the constructor — never per call). Mutations are load-modify-store and write **atomically**
(temp file + fsync + rename); reads mmap the file; the structural scan is SIMD-accelerated. **Row
indices are 0-based over data rows** — row `0` is the first row after the header. Errors throw
`Error`/`TypeError`/`RangeError`.

### `new CSVFile(path)`

Binds `path`; does **not** touch the disk. The instance is stateless apart from the path — there is no
open file handle and no explicit save, so a single instance can be reused across operations (each
method re-reads the file). Release it with `.close()` / `[Symbol.dispose]` (optional; a `CSVFile`
holds no OS handle, only the path string). Each method copies the path before coercing arguments, so a
re-entrant `{ valueOf() { f.close(); } }` argument cannot use-after-free.

### `create(options)`

| Option | Type | Description |
|---|---|---|
| `headers` | `string[]` | Column names (≥ 1). |
| `rows` | `string[][]?` | Initial rows; each must have exactly `headers.length` values. |
| `overwrite` | `boolean?` | If `false` (default), **throws** when the file exists. |

Creates the file at the instance path (parent directories are created automatically). **Returns** `{ path, rows }` (`rows` = data rows written). **Throws** if the file exists without `overwrite`, `headers` is empty, or a row's width ≠ `headers.length`.

### `read(options?)`

| Option | Type | Description |
|---|---|---|
| `offset` | `number?` | Data rows to skip (default `0`). |
| `limit` | `number?` | Max rows returned (default: all). |
| `columns` | `string[]?` | Column names to include, in order (default: all). |

Called with no argument, reads the whole file. **Returns** `{ headers: string[], rows: string[][], totalRows: number }` — `totalRows` is the full count regardless of pagination. **Throws** on a missing file or unknown column.

### `addRow(options)`

| Option | Type | Description |
|---|---|---|
| `rows` | `(string[] \| object)[]` | Each row is a positional `string[]` (column order) **or** an object `{ column: value }` (missing columns → `""`, extra keys ignored). |

**Returns** `{ added: number, totalRows: number }`.

### `updateCell(options)`

| Option | Type | Description |
|---|---|---|
| `row` | `number` | 0-based data-row index. |
| `column` | `string?` | Column by name (mutually exclusive with `columnIndex`). |
| `columnIndex` | `number?` | Column by 0-based index. |
| `value` | `string` | New value (`""` clears the cell). |

**Returns** `{ row, column, value }`. **Throws** `RangeError` on an out-of-range row/index; `TypeError` on an unknown column or if no column selector is given.

### `removeRow(options)`

| Option | Type | Description |
|---|---|---|
| `row` | `number` | 0-based data-row index. Remaining rows shift up. |

**Returns** `{ removed: number, totalRows: number }`. **Throws** `RangeError` if out of range.

### `addColumn(options)`

| Option | Type | Description |
|---|---|---|
| `column` | `string` | New column name (must not exist). |
| `defaultValue` | `string?` | Fill for existing rows (default `""`). |

**Returns** `{ column, totalColumns }`. **Throws** if the column already exists.

### `removeColumn(options)`

| Option | Type | Description |
|---|---|---|
| `column` | `string?` | By name (mutually exclusive with `columnIndex`). |
| `columnIndex` | `number?` | By 0-based index. |

**Returns** `{ removedIndex, totalColumns }`.

### `renameColumn(options)`

| Option | Type | Description |
|---|---|---|
| `oldName` | `string` | Existing column (must exist). |
| `newName` | `string` | New name (must not exist unless equal to `oldName` → no-op). |

**Returns** `{ oldName, newName }`.

### `readColumnValuesRange(options)`

| Option | Type | Description |
|---|---|---|
| `column` | `string` | Column name. |
| `start` | `number?` | First data row (inclusive, default `0`). |
| `end` | `number?` | End row (exclusive, default: all). Max **requested** window (`end - start`) is **1000**. |

**Returns** `string[]`.

### `readRowRange(options?)`

| Option | Type | Description |
|---|---|---|
| `start` | `number?` | First data row (inclusive, default `0`). |
| `end` | `number?` | End row (exclusive, default `start + 1`). Max window **100**. |

**Returns** `{ headers: string[], rows: string[][] }`.

### `selectColumnRange(options)`

Project specific columns over a range (like `SELECT col…` with no `WHERE`).

| Option | Type | Description |
|---|---|---|
| `columns` | `string[]` | Column names, in output order (non-empty; all must exist). |
| `start` | `number?` | First data row (inclusive, default `0`). |
| `end` | `number?` | End row (exclusive, default: all). Max window **100**. |

**Returns** `{ columns: string[], rows: string[][] }`.

```js
const users = new CSVFile("/tmp/u.csv");
users.create({ headers: ["Name","Age"], rows: [["Alice","30"]], overwrite: true });
users.addRow({ rows: [{ Name: "Bob", Age: "25" }] });
users.updateCell({ row: 0, column: "Age", value: "31" });
users.read();
// { headers:["Name","Age"], rows:[["Alice","31"],["Bob","25"]], totalRows:2 }
```

---

# sort

`import { sort, binarySearch } from "dyna:sort";`

### `sort(arr)`

| Parameter | Type | Description |
|---|---|---|
| `arr` | `number[]` | A JavaScript array of numbers. (TypedArrays are rejected — pass a plain `Array`.) |

**Returns** `number[]` — a **new** ascending-sorted array. The input is not mutated. **Throws** `TypeError` if `arr` is not a plain array.

### `binarySearch(sortedArr, target)`

| Parameter | Type | Description |
|---|---|---|
| `sortedArr` | `number[]` | An ascending-sorted array. |
| `target` | `number` | The value to find. |

**Returns** `number` — the index of `target`, or `-1` if absent.

```js
const s = sort([3,1,2,5,4]);   // [1,2,3,4,5]  (original untouched)
binarySearch(s, 4);            // 3
```

---

# search

`import { indexOf, indexOfAll } from "dyna:search";`

SIMD substring search over strings (byte offsets into UTF‑8).

| Function | Signature | Description |
|---|---|---|
| `indexOf(haystack, needle)` | `(string, string) → number` | The first match offset, or `-1`. |
| `indexOfAll(haystack, needle)` | `(string, string) → number[]` | Every match start offset, **including overlapping** matches. |

```js
indexOf("the quick brown fox", "quick");   // 4
indexOfAll("abababa", "aba");              // [0, 2, 4]
```

---

# Built-in prototype extensions (SugarJS + RamdaJS)

Native methods matching **SugarJS 2.0** and **RamdaJS 0.32** where they are *not* already ECMAScript,
installed **non-enumerable** on the built-in prototypes/constructors via the engine's own method
tables (the exact mechanism as `map`/`filter`). **No import** — present in every build.

**Conventions.**
- **Naming** — plain names; an ES-standard method is never shadowed (`map filter reduce find sort
  slice concat join …` stay ES). Ramda `is` is not exposed (collides with ES `Object.is`).
- **Immutability** — value-producing `Array`/`Object`/`Date` methods return a **new** value. Exceptions:
  `Object.set` (deep-mutates), `Number.times/upto/downto` and `Number.range` (build fresh arrays).
- **Matchers** — many `Array` methods accept **value | predicate `fn` | `RegExp`** (kind resolved once
  per call). Set-ops use SameValueZero; `Object.equals` is deep (SameValue leaf).
- **Date** — English/ISO, **local-time**, immutable. `Date.create(string)` (NL parser) and per-locale
  masks are not implemented.
- **Security** — iteration builders cap element count at `1e8` and throw `RangeError` *before*
  allocating; `pad`/`hex` cap width at 65536; `Function.until` caps at `1e7` iterations;
  `String.removeTags` is single-pass O(n).
- **⚡ SIMD** — the marker flags methods that dispatch to `simd.*` byte/vector kernels (scalar →
  NEON/SSE4.2/AVX2/AVX‑512). SIMD pays only on long spans (64-byte gate); short inputs run a scalar
  oracle. The complete SIMD-accelerated set is: `%TypedArray%` `sum min max mean average dot`; `String`
  `compact count escapeHTML unescapeHTML stripTags lines encodeBase64 decodeBase64`. Everything else is
  scalar (the generic `Array` cannot SIMD strided tagged values).

## Array.prototype

**Aggregate & query** — a `match` is a value, a predicate `fn`, or a `RegExp`; a `map` is a mapper `fn`,
a property-key string, or omitted (identity).

| Method | Signature | Description |
|---|---|---|
| `sum` / `average` / `mean` | `() → number` | Σ / arithmetic mean (mean of empty = `NaN`). |
| `median` / `product` | `() → number` | Middle value (coerced to a C buffer first) / Π. |
| `min` / `max` | `(map?) → element` | Extremum by optional mapper. |
| `count` / `none` / `any` / `all` | `(match) → number \| boolean` | Count / quantifiers over the matcher. |
| `countBy` / `indexBy` | `(fn) → object` | `{key: count}` / `{key: lastElement}` grouped by `fn(el)`. |
| `scan` | `(fn, acc) → array` | Running reduce (all intermediate accumulators). |

**Access & window**

| Method | Signature | Description |
|---|---|---|
| `first` / `last` / `head` | `() → element` | Ends (`head` = `first`). |
| `nth` | `(i) → element` | `i`-th element (negative from the end). |
| `init` / `tail` | `() → array` | All but last / all but first. |
| `take` / `drop` / `takeLast` / `dropLast` | `(n) → array` | Fixed-count slices. |
| `takeWhile` / `dropWhile` / `takeLastWhile` / `dropLastWhile` | `(match) → array` | Matcher-bounded slices. |

**Transform & structure**

| Method | Signature | Description |
|---|---|---|
| `unique` / `uniq` / `uniqBy` | `(map?) → array` | Dedup by SameValueZero (or by `fn(el)`). |
| `compact` | `() → array` | Drop falsey (`null`/`undefined`/`NaN`/`false`/empty) elements. |
| `flatten` / `transpose` | `() → array` | Deep flatten / matrix transpose. |
| `intersperse` | `(sep) → array` | Insert `sep` between elements. |
| `aperture` | `(n) → array` | Sliding windows of width `n`. |
| `splitEvery` / `splitAt` / `splitWhen` | `(n \| i \| match) → array` | Chunk / split at index / split at first match. |
| `shuffle` / `sample` | `() → array \| element` | Fisher–Yates / random element. |
| `sortBy` / `groupBy` / `partition` | `(map \| fn \| match) → …` | Stable sort / `{key:[…]}` / `[pass, fail]`. |
| `pluck` | `(key) → array` | `el[key]` for each. |
| `adjust` / `update` | `(i, fn) / (i, v) → array` | Non-mutating element edit. |
| `move` / `swap` | `(from, to) / (i, j) → array` | Non-mutating reorder. |
| `zip` / `zipWith` / `zipObj` / `fromPairs` | `(b) / (fn,b) / (vals) / () → …` | Pair, combine, or build objects. |
| `xprod` / `innerJoin` | `(b) / (pred, b) → array` | Cartesian product / relational join. |

**Build & combine**

| Method | Signature | Description |
|---|---|---|
| `append` / `prepend` | `(x) → array` | Add one element at an end. |
| `insert` / `insertAll` | `(i, x) / (i, xs) → array` | Splice-insert (out-of-range appends). |
| `removeAt` / `removeRange` | `(i) / (start, count) → array` | Non-mutating removal. |
| `remove` / `reject` | `(match) → array` | Drop matching elements (aliases). |
| `union` / `intersect` / `intersection` / `difference` / `without` | `(b) → array` | Set ops (SameValueZero). |

```js
[1,2,3,4].sum();                 // 10
[5,3,1,4,2].sortBy();            // [1,2,3,4,5]
["a","b","c"].intersperse("-");  // ["a","-","b","-","c"]
[{n:1},{n:2}].pluck("n");        // [1, 2]
[1,2,3].union([3,4,5]);          // [1,2,3,4,5]
```

## %TypedArray%.prototype  (SIMD reductions)

| Method | Signature | Description |
|---|---|---|
| `sum` ⚡ | `() → number` | SIMD Σ (f64/f32/i32 kernel by array type). |
| `min` / `max` ⚡ | `() → number` | SIMD horizontal min/max. |
| `mean` / `average` ⚡ | `() → number` | SIMD sum ÷ length. |
| `dot` ⚡ | `(other) → number` | SIMD dot product with another same-type TypedArray. |

```js
new Float64Array([1,2,3,4]).sum();                          // 10
new Float32Array([1,2,3]).dot(new Float32Array([4,5,6]));   // 32
```

## String.prototype

**Predicates & slice**

| Method | Signature | Description |
|---|---|---|
| `isEmpty` / `isBlank` | `() → boolean` | Length 0 / empty-or-whitespace. |
| `first` / `last` | `(n=1) → string` | Leading / trailing `n` chars. |
| `from` / `to` | `(i) → string` | Substring from / up to index (negative from end). |

**Split & scan**

| Method | Signature | Description |
|---|---|---|
| `chars` / `codes` | `() → array` | Code-point strings / char codes (pre-sized fast array). |
| `words` | `() → string[]` | Whitespace-delimited words. |
| `lines` ⚡ | `() → string[]` | Split on `\n` (`simd.count_u8` presize + `simd.find_u8`). |
| `count` ⚡ | `(sub) → number` | Non-overlapping occurrences (`simd.count_u8` for a 1-char needle). |
| `forEach` | `(fn) → string[]` | `fn(char, i)` per code point; returns the char array. |

**Transform**

| Method | Signature | Description |
|---|---|---|
| `reverse` | `() → string` | Reversed (direct alloc + tight copy; auto-vectorized). |
| `compact` ⚡ | `() → string` | Collapse whitespace runs → single space, trim (`simd.find_first_of`). |
| `insert` | `(str, i=end) → string` | Insert at a code-unit index. |
| `remove` / `removeAll` | `(m) → string` | Delete first / all matches (string or `RegExp`). |
| `shift` | `(n) → string` | Caesar-shift each char code by `n`. |
| `truncate` / `truncateOnWord` | `(len, from='right', ellipsis='…') → string` | Clip with ellipsis. |
| `pad` | `(n, char=' ') → string` | Pad both sides to width `n`. |

**Case & inflection**

| Method | Signature | Description |
|---|---|---|
| `capitalize` | `(all=false, downcaseRest=false) → string` | Capitalize first (or each) word. |
| `camelize` / `underscore` / `dasherize` / `spacify` | `(upperFirst=true?) → string` | camelCase / snake / kebab / spaced. |
| `titleize` | `() → string` | Title Case with a lowercase stop-word list. |
| `humanize` | `() → string` | `user_name_id` → `"User name"`. |
| `parameterize` | `() → string` | Lowercase URL slug (`-`; ASCII, no accent transliteration). |
| `pluralize` / `singularize` | `() → string` | English rules + small irregular/uncountable table. |

**HTML / URL / Base64**

| Method | Signature | Description |
|---|---|---|
| `escapeHTML` ⚡ | `() → string` | Escape `& < >` (`simd.find_first_of`). |
| `unescapeHTML` ⚡ | `() → string` | Decode named + numeric entities (`simd.find_u8`). |
| `stripTags` ⚡ | `() → string` | Remove `<…>` tags, keep text (`simd.find_u8`). |
| `removeTags` | `(tagName?) → string` | Remove element(s) **and** content; single-pass **O(n)**. |
| `escapeURL` / `unescapeURL` | `(all=false? / partial=false?) → string` | Percent encode / decode. |
| `encodeBase64` ⚡ | `() → string` | `simd.base64_encode`. |
| `decodeBase64` ⚡ | `() → string` | `simd.base64_decode`. |

**Convert**

| Method | Signature | Description |
|---|---|---|
| `toNumber` | `(base=10) → number` | Lenient parse (`strtod` / `strtoll`); `NaN` on failure. |
| `format` | `(...args) → string` | `{0}`/`{name}` template (`{{`/`}}` literal braces). |

```js
"  many   spaces  ".compact();       // "many spaces"
"banana".count("a");                 // 3
"<b>hi</b>".stripTags();             // "hi"
"hello_world".titleize();            // "Hello World"
"person".pluralize();                // "people"
"{0} + {1}".format(2, 3);            // "2 + 3"
```

## Number.prototype

**Ramda arithmetic / relational**

| Method | Signature | Description |
|---|---|---|
| `negate` / `inc` / `dec` | `() → number` | −x / x+1 / x−1. |
| `abs` `sqrt` `exp` `sin` `cos` `tan` `asin` `acos` `atan` | `() → number` | libm delegation. |
| `add` `subtract` `multiply` `divide` `modulo` `pow` | `(n) → number` | `modulo` == `fmod` == JS `%`. |
| `gt` / `gte` / `lt` / `lte` | `(n) → boolean` | Relational (NaN → false). |

**Predicates & math**

| Method | Signature | Description |
|---|---|---|
| `isInteger` / `isOdd` / `isEven` | `() → boolean` | Integer tests. |
| `isMultipleOf` | `(n) → boolean` | `this % n === 0`. |
| `mathMod` | `(n) → number` | Non-negative modulus; `NaN` unless both integer and `n ≥ 1`. |
| `clamp` | `(min, max) → number` | Clamp into range. |
| `log` | `(base=e) → number` | Change-of-base logarithm. |
| `round` / `ceil` / `floor` | `(places=0) → number` | Precision rounding (negative places → tens/hundreds). |
| `chr` | `() → string` | The char for this char code. |

**Formatting**

| Method | Signature | Description |
|---|---|---|
| `pad` | `(place, sign=false, base=10) → string` | Zero-pad the integer part. |
| `hex` | `(place=1) → string` | Hex, zero-padded. |
| `format` | `(place=0, thousands=',', decimal='.') → string` | Grouped thousands. |
| `abbr` / `metric` / `bytes` | `(precision=0) → string` | `2k` (÷1000 k/m/b/t) / SI / byte size (÷1024). |
| `ordinalize` | `() → string` | `1`→`"1st"`, `11`→`"11th"`. |
| `duration` | `() → string` | Treat `this` as ms → `"2 hours"`. |

**Iteration** — build arrays; element count capped at `1e8` (throws `RangeError` before allocating).

| Method | Signature | Description |
|---|---|---|
| `times` | `(fn?) → array` | `[fn(0)…fn(n-1)]` (or `[0…n-1]`). |
| `upto` / `downto` | `(end, step=1, fn?) → array` | Inclusive numeric range. |
| `Number.range` *(static)* | `(start, end, step=1) → array` | Ramda end-exclusive range. |

```js
(3.14159).round(2);       // 3.14
(1536).bytes(1);          // "1.5KB"
(1234567).format();       // "1,234,567"
(3).times(i => i*i);      // [0, 1, 4]
Number.range(0, 5);       // [0, 1, 2, 3, 4]
```

## Object  (static on the `Object` constructor)

**Type guards & nil** — `(v) → boolean` unless noted (class-id based; wrapper objects report the
primitive tag).

| Method | Description |
|---|---|
| `isObject isArray isBoolean isNumber isString isFunction isDate isRegExp isError isSet isMap isArguments` | Type tests. |
| `isNil` / `isNotNil` | `== null` / not. |
| `type(v) → string` | `"Number"`, `"Array"`, `"Null"`, … (class-id based). |
| `defaultTo(d, v) → any` | `v` unless `null`/`undefined`/`NaN`, then `d`. |
| `propIs(Ctor, name, obj) → boolean` | `obj[name]` is an instance of `Ctor` (incl. primitives). |

**Query** — `path` is a dotted string or an array.

| Method | Signature | Description |
|---|---|---|
| `size` / `isEmpty` | `(o) → number \| boolean` | Count of own enumerable string keys. |
| `keysIn` / `valuesIn` | `(o) → array` | Enumerable keys/values **including inherited**. |
| `toPairs` / `fromPairs` | `(o) / (pairs) → …` | `[[k,v]]` ⇄ object. |
| `has` / `hasIn` / `hasPath` | `(k,o) / (k,o) / (path,o) → boolean` | Own / `in` / deep-own presence. |
| `prop` / `propOr` / `props` | `(k,o) / (d,k,o) / (keys,o) → …` | Read one / with default / many. |
| `path` / `pathOr` / `paths` | `(path,o) / (d,path,o) / (list,o) → …` | Deep read. |
| `get` | `(o, path, default?) → any` | Sugar deep read (object first). |

**Predicates**

| Method | Signature | Description |
|---|---|---|
| `equals` / `identical` | `(a, b) → boolean` | Deep structural (SameValue leaf; cycles throw) / SameValue. |
| `propEq` / `pathEq` / `eqProps` | `(val,k,o) / (val,path,o) / (k,a,b) → boolean` | Focused equality. |
| `propSatisfies` / `pathSatisfies` | `(pred, k\|path, o) → boolean` | Focused predicate. |
| `where` / `whereEq` / `whereAny` | `(spec, o) → boolean` | All preds / deep-eq per key / any pred. |

**Build & transform** (immutable unless noted)

| Method | Signature | Description |
|---|---|---|
| `clone` | `(o) → any` | Deep clone (arrays, plain objects, Date, RegExp; other exotics by ref). |
| `pick` / `pickAll` / `omit` / `pickBy` | `(keys\|pred, o) → object` | Select / with missing / drop / by predicate. |
| `project` | `(keys, arr) → array` | `arr.map(pick(keys))`. |
| `assoc` / `dissoc` | `(k, v, o) / (k, o) → object` | Shallow set / delete. |
| `assocPath` / `dissocPath` | `(path, v, o) / (path, o) → object` | Immutable deep set / delete. |
| `set` | `(o, path, v) → object` | **Mutates** `o` (deep, creates intermediates); returns `o`. |
| `modify` / `modifyPath` | `(k\|path, fn, o) → object` | Apply `fn` to a focus. |
| `evolve` | `(transforms, o) → object` | Per-key transform (fn or nested transforms). |
| `mapObjIndexed` / `forEachObjIndexed` | `(fn, o) → object` | `{k: fn(v,k,o)}` / side effect returns `o`. |
| `mapKeys` / `renameKeys` | `(fn, o) / (map, o) → object` | Transform / rename keys. |
| `invert` / `invertObj` / `objOf` | `(o) / (o) / (k,v) → object` | Swap keys↔values / `{[k]:v}`. |
| `tap` | `(fn, x) → x` | Run `fn(x)` for effect, return `x`. |
| `defaults` | `(o, source) → object` | `o` wins, `source` fills gaps. |
| `merge` / `mergeRight` / `mergeLeft` | `(a, b) → object` | Shallow (left key order; right/left wins). |
| `mergeDeepRight` / `mergeDeepLeft` | `(a, b) → object` | Recursive merge. |
| `mergeWith` / `mergeWithKey` | `(fn, a, b) → object` | Resolve conflicts with `fn`. |

```js
Object.pick(["a","c"], {a:1,b:2,c:3});          // {a:1, c:3}
Object.path("a.b.c", {a:{b:{c:42}}});           // 42
Object.mergeDeepRight({a:{x:1}}, {a:{y:2}});     // {a:{x:1, y:2}}
Object.evolve({n: x=>x*2}, {n:5, s:"k"});        // {n:10, s:"k"}
Object.equals({a:[1,2]}, {a:[1,2]});             // true
```

## Function.prototype  (combinators)

Each returns a **new function** capturing the receiver + args; `this` is the receiving function.
No `R.__` placeholders. Composing beyond ~253 functions throws `RangeError`.

| Method | Signature | Description |
|---|---|---|
| `pipe` / `compose` / `flow` | `(...fns) → fn` | L→R / R→L composition (`flow` = `pipe`). |
| `o` / `on` | `(g) → fn` | `x=>f(g(x))` / `(a,b)=>f(g(a),g(b))`. |
| `both` / `allPass` | `(...preds) → fn` | Logical AND (short-circuit). |
| `either` / `anyPass` / `complement` | `(...preds) / () → fn` | OR / NOT. |
| `juxt` | `(...fns) → fn` | `x => [f(x), g(x), …]`. |
| `converge` | `(...branches) → fn` | `(...a) => f(b0(...a), b1(...a), …)`. |
| `useWith` | `(...ts) → fn` | `(a,b,…) => f(t0(a), t1(b), …)`. |
| `flip` / `unary` / `binary` / `nAry` | `() / () / () / (n) → fn` | Swap first two args / fix arity. |
| `once` / `thunkify` | `() → fn` | Memoize first result / `(...a)()` ⇒ `f(...a)`. |
| `partial` / `partialRight` | `(...a) → fn` | Pre-bind leading / trailing args. |
| `curry` / `curryN` | `() / (n) → fn` | Auto-curry (reusable, no arg bleed). |
| `ifElse` / `when` / `unless` | `(t,f) / (fn) / (fn) → fn` | Conditional (`this` is the predicate). |
| `until` | `(fn) → fn` | `x => apply fn until this(x)` (≤ 1e7 iterations). |
| `tryCatch` | `(handler) → fn` | `(...a) => try f(...a) catch (e) handler(e, ...a)`. |
| `unapply` / `comparator` | `() → fn` | `(...a)=>f(a)` / predicate → sort comparator. |

```js
const f = (x=>x+1).pipe(x=>x*2, x=>-x);   f(3);     // -8
const add3 = ((a,b,c)=>a+b+c).curry();    add3(1)(2)(3);   // 6
const safe = (x=>{ if(x<0) throw Error("neg"); return x }).tryCatch(()=>0);   safe(-1);  // 0
```

## Date.prototype  (English/ISO, local-time, immutable)

**Predicates** — `() → boolean`

| Group | Methods |
|---|---|
| State | `isValid isToday isYesterday isTomorrow isFuture isPast isWeekday isWeekend isLeapYear` |
| Day-of-week | `isSunday isMonday isTuesday isWednesday isThursday isFriday isSaturday` |
| Month | `isJanuary … isDecember` |

**Query & compare**

| Method | Signature | Description |
|---|---|---|
| `getWeekday` | `() → 0–6` | Day of week (Sun–Sat). |
| `getISOWeek` | `() → 1–53` | ISO-8601 week number. |
| `daysInMonth` | `() → number` | Days in this month. |
| `isBefore` / `isAfter` | `(d) → boolean` | Ordering. |
| `isBetween` | `(a, b) → boolean` | Inclusive; bounds auto-ordered. |

**Diffs** — whole units (`trunc`); invalid date → `NaN`. `<unit> ∈ {milliseconds, seconds, minutes,
hours, days, weeks, months, years}` (months/years use calendar math).

| Method | Signature | Description |
|---|---|---|
| `<unit>Since` / `<unit>Until` | `(d) → number` | From `d` to this / this to `d`. |
| `<unit>Ago` / `<unit>FromNow` | `() → number` | From this to now / now to this. |

**Produce** — `→ Date` (immutable; JS field-overflow / MakeDay)

| Method | Signature | Description |
|---|---|---|
| `addMilliseconds … addYears` | `(n) → Date` | Add a signed amount of a unit. |
| `beginningOfDay` / `endOfDay` | `() → Date` | 00:00 / 23:59:59.999. |
| `beginningOfWeek` / `endOfWeek` | `() → Date` | Sunday 00:00 / Saturday 23:59:59.999. |
| `beginningOfMonth` / `endOfMonth` | `() → Date` | First / last day. |
| `beginningOfYear` / `endOfYear` | `() → Date` | Jan 1 / Dec 31. |
| `advance` / `rewind` | `(spec) → Date` | Apply/subtract `{years,months,weeks,days,hours,minutes,seconds,milliseconds}`. |
| `clone` | `() → Date` | Copy. |

**Format** — `→ string`

| Method | Signature | Description |
|---|---|---|
| `iso` | `() → string` | `toISOString` alias. |
| `format` | `(mask?) → string` | `{token}` substitution; no mask → `"yyyy-MM-dd HH:mm:ss"`. |
| `relative` | `() → string` | `"2 days ago"` / `"in 3 days"` / `"just now"`. |

`format` tokens: `yyyy yy MM M dd d HH H hh h mm m ss s SSS Mon Month dow Weekday tt TT`.

```js
const d = new Date(2024, 1, 29, 15, 30);
d.isLeapYear();                              // true
d.addDays(1).getMonth();                     // 2  (Mar 1)
d.endOfMonth().getDate();                    // 29
d.format("{Weekday}, {Month} {d}, {yyyy}");  // "Thursday, February 29, 2024"
new Date(2024,0,1).daysUntil(new Date(2024,0,11));   // 10
```

## Lens  (Ramda lenses — the `Lens` global)

A lens is an ordinary object (proto `Lens.prototype`, non-enumerable config); no new intrinsic class.
`set`/`over` are immutable and preserve container type (arrays stay arrays).

| Method | Signature | Description |
|---|---|---|
| `Lens.prop` / `Lens.index` | `(k) / (i) → Lens` | Focus a property / array index. |
| `Lens.path` | `(p) → Lens` | Focus a deep path (dotted string or array). |
| `Lens.lens` | `(getter, setter) → Lens` | Custom; `getter(obj)`, `setter(newVal, obj)`. |
| `Lens.view` / `lens.view` | `(lens, o) / (o) → any` | Read the focus. |
| `Lens.set` / `lens.set` | `(lens, v, o) / (v, o) → any` | Immutable set (new container). |
| `Lens.over` / `lens.over` | `(lens, fn, o) / (fn, o) → any` | Immutable `fn`-modify. |

Obeys the lens laws: `view(set(v,s)) ≡ v`, `set(view(s),s) ≡ s`, `set(v, set(_,s)) ≡ set(v,s)`.

```js
const nameL = Lens.prop("name");
nameL.view({name:"ada"});                 // "ada"
nameL.set("bob", {name:"ada", age:1});    // {name:"bob", age:1}   (original unchanged)
Lens.over(Lens.index(1), x=>x*10, [1,2,3]);   // [1, 20, 3]
```

## Not implemented (deferred)

- `Date.create(string)` — the natural-language date parser + per-locale format masks.
- Function timers — `throttle debounce delay memoize after lazy` (need event-loop + cancellation).
- Array transducers — `into sequence traverse transduce mapAccum`.
- `RegExp.prototype` flag helpers — blocked by `test262 staging/sm/RegExp/prototype.js` (pins
  `Reflect.ownKeys(RegExp.prototype)`).
- Remaining Array (`startsWith endsWith unnest dropRepeats sortWith *With set-ops symmetricDifference
  repeat reduceBy`, Sugar FromIndex) and Function statics (`always identity cond of not negate applyTo
  uncurryN lift ap`).
