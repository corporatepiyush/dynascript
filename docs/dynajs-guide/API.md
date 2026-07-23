# DynaJS API Reference

Complete signatures and behavior for every `dynajs:*` standard-library module. This is the precise
reference; [Chapter 4](04-standard-library.md) is the worked-example tour.

**Conventions.** `data` accepts a `string` (its UTF‑8/Latin‑1 bytes), an `ArrayBuffer`, or a
`TypedArray` view (`Uint8Array`) unless noted. 64‑bit integers are `BigInt` (written `42n`). All
functions throw a JavaScript `Error` (subclass noted) on invalid input. Resource classes own native
memory and must be released with `.close()` / `[Symbol.dispose]()`; after release every method
throws `TypeError: use of a closed native resource`. Nothing returned aliases native memory.

Modules: [path](#path) · [strings](#strings) · [bytes](#bytes) · [encoding](#encoding) ·
[text](#text) · [crypto](#crypto) · [uuid](#uuid) · [random](#random) · [mathx](#mathx) ·
[bits](#bits) · [container](#container) · [structures](#structures) · [sys](#sys) · [path](#path) ·
[file](#file) · [uring](#uring) · [http](#http) · [netip](#netip) · [time](#time) · [semver](#semver) ·
[simd](#simd) · [ml](#ml) · [compress](#compress) · [docparse](#docparse) · [sort](#sort) ·
[search](#search)

---

## path

POSIX path-string logic (pure, no filesystem access).

- `join(...segments: string) → string` — join with `/`, then normalize. `join("a","b","..","c") → "a/c"`.
- `resolve(...segments: string) → string` — resolve to an absolute path, right-to-left, against `cwd`.
- `normalize(p: string) → string` / `clean(p: string) → string` — collapse `.`/`..`/duplicate slashes.
- `dirname(p: string) → string` — directory portion. `dirname("/a/b/c") → "/a/b"`.
- `basename(p: string, suffix?: string) → string` — final component; strips `suffix` if given.
- `extname(p: string) → string` — extension incl. the dot (`".csv"`), or `""`.
- `isAbsolute(p: string) → boolean`.
- `relative(from: string, to: string) → string` — relative path from `from` to `to`.
- `sep: string` (`"/"`), `delimiter: string` (`":"`) — constants.

## strings

Go-style string utilities (some SIMD-accelerated). Indices/counts are byte-based (UTF‑8).

- `split(s, sep) → string[]` — split on every `sep`. `splitN(s, sep, n) → string[]` — at most `n` pieces.
- `fields(s) → string[]` — split on runs of whitespace, trimmed (empty → `[]`).
- `join(parts: string[], sep) → string`.
- `trim(s)` / `trimStart(s)` / `trimEnd(s) → string` — strip whitespace.
- `trimPrefix(s, prefix)` / `trimSuffix(s, suffix) → string` — remove if present.
- `trimChars(s, chars) → string` — strip any leading/trailing char in `chars`.
- `toUpper(s)` / `toLower(s)` / `title(s) → string` — case mapping; `title` upper-cases each word start.
- `repeat(s, count) → string`. `padStart(s, len, pad)` / `padEnd(s, len, pad) → string`.
- `contains(s, sub) → boolean` (SIMD). `containsAny(s, chars) → boolean`.
- `hasPrefix(s, prefix)` / `hasSuffix(s, suffix) → boolean`.
- `index(s, sub) → number` — first byte index or `-1` (SIMD). `lastIndex(s, sub) → number`.
- `indexAny(s, chars) → number` — first index of any char in `chars`, or `-1`.
- `count(s, sub) → number` — non-overlapping occurrences (`count(s,"") →` code-point count + 1).
- `replace(s, old, new, n) → string` — first `n` (n<0 = all). `replaceAll(s, old, new) → string`.
- `equalFold(a, b) → boolean` — case-insensitive (Unicode) equality.
- `compare(a, b) → -1 | 0 | 1` — lexicographic byte order.

## bytes

Byte-buffer utilities over `Uint8Array`. Read/write helpers cover every width in LE and BE.

- `compare(a, b) → -1 | 0 | 1`. `equal(a, b) → boolean`.
- `indexOf(buf, sub) → number` (SIMD, `-1` if absent). `lastIndexOf(buf, sub) → number`.
- `contains(buf, sub) → boolean`. `count(buf, sub) → number` — non-overlapping occurrences.
- `concat(buffers: Uint8Array[]) → Uint8Array` — concatenate.
- `copy(dst, src, dstStart=0, srcStart=0, srcEnd=src.length) → number` — copy a range into `dst`, returns bytes copied.
- `fill(buf, value, start=0, end=buf.length) → Uint8Array` — fill a range with byte `value`.
- **Read** `readUint8/readInt8(buf, offset) → number`; `readUint16LE/BE`, `readInt16LE/BE`,
  `readUint32LE/BE`, `readInt32LE/BE`, `readFloatLE/BE`, `readDoubleLE/BE` `(buf, offset) → number`;
  `readBigUint64LE/BE`, `readBigInt64LE/BE` `(buf, offset) → BigInt`. Throw `RangeError` on OOB offset.
- **Write** `writeUint8/writeInt8(buf, offset, value)`; the 16/32/Float/Double LE/BE variants
  `(buf, offset, value) → number` (next offset); the 64-bit variants take a `BigInt` value.
- `toHex(buf) → string` / `fromHex(hex) → Uint8Array`. `toBase64(buf) → string` / `fromBase64(s) → Uint8Array`.
- `toUtf8(buf) → string` / `fromUtf8(s) → Uint8Array`.

## encoding

Binary↔text codecs (hex/base64/base32/base85 + LEB128 var-ints). Hex/base64 are SIMD-accelerated.

- `hexEncode(data) → string` / `hexDecode(s) → Uint8Array` — throws `SyntaxError` on odd length or non-hex.
- `base64Encode(data) → string` / `base64Decode(s) → Uint8Array` — standard `+/`, padded.
- `base64UrlEncode(data) → string` / `base64UrlDecode(s) → Uint8Array` — URL-safe `-_`, no padding.
- `base32Encode/base32Decode`, `base32HexEncode/base32HexDecode` — RFC 4648 base32 (standard + extended-hex).
- `base85Encode(data) → string` / `base85Decode(s) → Uint8Array` — Ascii85.
- `putUvarint(value: number|BigInt) → Uint8Array` — LEB128 encode an unsigned int.
- `uvarint(buf) → [value: number|BigInt, bytesRead: number]` — decode; throws on truncation/overflow.
- `putVarint(value) → Uint8Array` / `varint(buf) → [value, bytesRead]` — zig-zag signed LEB128.

## text

SIMD text kernels (throughput-oriented). Accept `Uint8Array`/`ArrayBuffer`/`string`.

- `count(data, ch) → number` — occurrences of byte `ch` (string's first byte, or a number).
- `indexOfAny(data, chars) → number` — first index of any byte in `chars`, or `-1`.
- `isValidUtf8(data) → boolean`. `countUtf8(data) → number` — code-point count.
- `base64Encode(data) → string` / `base64Decode(s) → ArrayBuffer`.
- `hexEncode(data) → string` / `hexDecode(s) → Uint8Array` (throws `TypeError` on bad input).
- `latin1ToUtf8(data) → Uint8Array` / `utf8ToLatin1(data) → Uint8Array`.
- `utf8ToUtf16(data) → Uint8Array` — UTF‑16LE bytes; throws `RangeError` on malformed UTF‑8.
- `utf16ToUtf8(u16bytes) → Uint8Array` — throws on a lone/misordered surrogate or odd length.
- `isValidUtf16(u16bytes) → boolean`. `countUtf16(u16bytes) → number` — code-point count.

## crypto

Hashes, HMAC, CRC. One-shot functions + a streaming `Hasher`. Verified against standard vectors.

- `md5/sha1/sha224/sha256/sha384/sha512(data) → Uint8Array` — digest bytes.
- `md5Hex/sha1Hex/…/sha512Hex(data) → string` — lowercase hex digest.
- `hmac(algo: string, key, data) → Uint8Array` / `hmacHex(algo, key, data) → string` — `algo` ∈ the hash names.
- `crc32(data) → number` / `crc32c(data) → number` — IEEE / Castagnoli CRC‑32 (use `>>> 0` for unsigned).
- `class Hasher` — streaming:
  - `new Hasher(algo: string)` — `algo` ∈ `"md5"|"sha1"|"sha224"|"sha256"|"sha384"|"sha512"`.
  - `.update(data) → this`. `.digest() → Uint8Array`. `.digestHex() → string`. `.reset() → this`.
  - `.algorithm: string` (getter). `.digestSize: number` (getter). `.close()`.

## uuid

RFC 9562 UUIDs.

- `v4() → string` — random UUIDv4.
- `v7() → string` — time-ordered (48-bit ms timestamp high bits); monotonic-ish, sorts by creation.
- `v3(namespace: string, name: string) → string` — MD5 name-based. `v5(namespace, name) → string` — SHA‑1 name-based.
- `parse(s: string) → string` — validate + canonical lowercase; throws on malformed. `validate(s) → boolean`.
- `version(s) → number`. `variant(s) → number`.
- `bytes(s) → Uint8Array` (16 bytes). `fromBytes(u8: Uint8Array) → string`.
- Constants: `NIL`, `MAX`, `NAMESPACE_DNS`, `NAMESPACE_URL`, `NAMESPACE_OID`, `NAMESPACE_X500` (strings).

## random

Seedable PRNG (`class Random`) + a convenience `uuid`.

- `new Random(seed: number | BigInt)` — deterministic stream per seed.
- `.nextU64() → BigInt` (full 64-bit). `.nextU53() → number` (53-bit safe int). `.nextFloat() → number` (`[0,1)`).
- `.nextBounded(n: number) → number` — uniform in `[0, n)`. `.fill(buf: Uint8Array) → Uint8Array` — random bytes.
- `uuid() → string` — a random UUID from this module's generator.

## mathx

Math beyond `Math`: special functions, exact integer helpers, constants.

- Real→real (number): `gamma erf erfc cbrt expm1 log1p log2 logb trunc round roundToEven(x)`;
  `hypot copysign nextafter scalbn ldexp remainder fmod(x,y)`; `lgamma(x) → [value, sign]`;
  `modf(x) → [intPart, fracPart]`; `frexp(x) → [frac, exp]`; `ilogb(x) → number`.
- Predicates: `isInf(x, sign?) → boolean`, `isNaN(x) → boolean`, `signbit(x) → boolean`.
- Integer/BigInt: `gcd(a, b) → BigInt`, `lcm(a, b) → BigInt` (accept number|BigInt); `factorial(n) → BigInt`
  (exact); `isPrime(n: BigInt|number) → boolean` (deterministic Miller–Rabin over uint64);
  `abs(n: BigInt) → BigInt`, `bitLen(n: BigInt) → number`, `popcount(n: BigInt) → number`.
- Constants: `E Pi Phi Sqrt2 SqrtE SqrtPi Ln2 Log2E Ln10 Log10E MaxInt32 MinInt32 MaxSafeInteger` (+`MaxInt64` BigInt).

## bits

Go `math/bits`. 8/16/32-bit use `number`; **64-bit variants take/return `BigInt`**; the count family always returns `number`.

- `LeadingZeros{8,16,32,64}(x)`, `TrailingZeros{8,16,32,64}(x)`, `OnesCount{8,16,32,64}(x)`, `Len{8,16,32,64}(x) → number`.
- `Reverse{8,16,32,64}(x)` (reverse bits), `ReverseBytes{16,32,64}(x)` (byte-swap), `RotateLeft{8,16,32,64}(x, k)` (k<0 = right).
- `Add32/Add64(a, b, carry) → [sum, carryOut]`. `Sub32/Sub64(a, b, borrow) → [diff, borrowOut]`.
- `Mul32/Mul64(a, b) → [hi, lo]`. `Div32/Div64(hi, lo, y) → [quo, rem]` (throws `RangeError` on `y==0` or overflow). `Rem32/Rem64(hi, lo, y)`.
- `UintSize: number` (`64`).

## container

Native data structures. All are resource classes — `.close()` when done.

- `class Heap` — binary heap / priority queue.
  - `new Heap(compare: (a, b) => number)` — `compare` defines the order (min-heap for `a-b`). **Required.**
  - `.push(v) → void`. `.pop() → any` (removes+returns the top, `undefined` if empty). `.peek() → any`.
  - `.size: number` (getter). `.close()`.
- `class List` — doubly-linked list (deque).
  - `new List()`. `.pushFront(v)` / `.pushBack(v)`. `.popFront() → any` / `.popBack() → any`. `.front() → any` / `.back() → any`.
  - `.toArray() → any[]`. `[Symbol.iterator]()`. `.length: number` (getter). `.close()`.
- `class Ring` — fixed-capacity circular buffer (overwrites oldest).
  - `new Ring(capacity: number)`. `.push(v)`. `.get(i) → any`. `.toArray() → any[]`.
  - `.length: number`, `.capacity: number` (getters). `.close()`.

## structures

Growable vector + hash map (native backing). Resource classes.

- `class Vector` — `new Vector()`; `.push(v) → number` (new length); `.get(i) → any` (`undefined` OOB);
  `.set(i, v)`; `.pop() → any`; `.length: number` (getter); `.close()`.
- `class HashMap` — `new HashMap()`; `.set(k, v)`; `.get(k) → any`; `.has(k) → boolean`;
  `.delete(k) → boolean`; `.size: number` (getter); `.close()`.

## sys

Unified system interface — filesystem metadata, directories, glob, process/env. Synchronous. Errors throw an `Error` carrying `.code` (e.g. `"ENOENT"`) and `.errno`.

- `stat(path) → StatInfo` / `lstat(path) → StatInfo` (no symlink follow). `StatInfo` = `{ size, mode, isDir, isFile, isSymlink, mtimeMs, atimeMs, ctimeMs, uid, gid, ino, nlink }`.
- `exists(path) → boolean` (no throw).
- `readDir(path) → { name, isDir, isFile, isSymlink }[]` — sorted by name, excludes `.`/`..`.
- `makeDir(path, { recursive?, mode? }) → void`. `remove(path) → void`. `removeAll(path) → void` (recursive, symlink-safe, missing = no-op). `rename(from, to) → void`.
- `symlink(target, linkPath) → void`. `readLink(path) → string`. `realPath(path) → string`. `chmod(path, mode: number) → void`.
- `glob(pattern, { cwd? }) → string[]` — `*` `**` `?` `[...]` (ranges, `[!..]` negation); symlink-cycle-safe.
- `tempDir() → string`. `makeTempDir(prefix) → string` (creates it). `makeTempFile(prefix) → string`.
- `env() → object`. `getEnv(name) → string | undefined`. `setEnv(name, value) → void`.
- `args() → string[]`. `cwd() → string`. `chDir(path) → void`. `platform() → "darwin"|"linux"|"unknown"`.
- `pid() → number`. `hostName() → string`. `homeDir() → string`.

## file

Buffered file I/O with OS fast paths (macOS F_RDAHEAD/F_PREALLOCATE/F_FULLFSYNC; Linux fadvise/fallocate/io_uring).

- `readFile(path) → string` — whole file. `writeFile(path, data) → number` — bytes written.
- `class FileReader` — `new FileReader(path, { bufferSize? })`; `.read() → string|null` (a chunk);
  `.readLine() → string|null` (`null` at EOF, without the newline); `.readAll() → string`; `.close()`.
- `class FileWriter` — `new FileWriter(path, { bufferSize?, preallocate? })`; `.write(data) → number`;
  `.flush() → void`; `.sync() → void` (durable, F_FULLFSYNC on macOS); `.close()`.

## uring

Linux io_uring high-queue-depth bulk read (Linux builds only).

- `readFile(path) → Uint8Array` — async io_uring whole-file read. `readFileSync(path) → Uint8Array`.
- `checksum(path) → number` — streamed CRC over the file.

## http

HTTP client + servers. `HttpServerAsync` is a single-thread kqueue/epoll/io_uring reactor.

- `class HttpClient` — `new HttpClient()`; `.get(url) → Response`; `.post(url, body, headers?) → Response`;
  `.request(url, options) → Response`; `.setTimeout(ms) → void`; `.disconnect() → void`; `.close()`.
  `Response` = `{ status: number, headers: object, body: string }`.
- `class HttpServerAsync` — `new HttpServerAsync({ port, routes })`; `routes` maps a path to a string
  body or `{ status, contentType, body }`. `.start() → void`; `.stop() → void`; `.port: number` (getter). `port: 0` picks a free port.
- `class HttpServer` — a threaded variant with the same `.start()`/`.stop()`/`.port` surface.

## netip

IP address + CIDR reasoning (Go `net/netip`).

- `parseAddr(s) → { is4, is6, bytes: Uint8Array, string }` — throws on invalid.
- `parsePrefix(s) → { addr, bits }` — parse CIDR (`10.0.0.0/8`).
- `contains(prefix: string, addr: string) → boolean` — masked-prefix membership.
- `masked(prefix: string) → string` — network address (host bits zeroed).
- `canonical(addr: string) → string` — RFC 5952 canonical form. `isValid(s) → boolean` (no throw).
- `compareAddr(a, b) → -1 | 0 | 1`.
- Classifiers `(addr: string) → boolean`: `isLoopback isPrivate isMulticast isUnspecified isLinkLocalUnicast isGlobalUnicast isLinkLocalMulticast` (all unmap IPv4-in-IPv6 first except `isUnspecified`).

## time

Durations (nanoseconds), monotonic clock, RFC 3339.

- Constants (ns): `Nanosecond Microsecond Millisecond Second Minute Hour` (number/BigInt).
- `durationString(ns) → string` (e.g. `"1h30m0s"`). `parseDuration(s) → number|BigInt` (ns).
- `now() → number` (Unix ms). `nowUnixNano() → BigInt`. `nowMillis() → number`. `monotonicNano() → BigInt` (never decreases).
- `formatRFC3339(t) → string`. `parseRFC3339(s) → …`. `formatUnix(sec, format) → string` (Go layout tokens).
- `date(year, month, day) → …`. `fromUnix(sec, nsec?) → …`.

## semver

SemVer 2.0.0 + npm-style ranges.

- `parse(v) → { major, minor, patch, prerelease: (string|number)[], build: string[], version }` — throws on invalid. `isValid(v) → boolean`. `clean(v) → string | null`.
- `compare(a, b) → -1 | 0 | 1` (SemVer precedence; prerelease < release; build ignored). `gt gte lt lte eq neq(a, b) → boolean`.
- `sort(versions: string[]) → string[]` — ascending, new array.
- `major(v) minor(v) patch(v) → number`. `prerelease(v) → (string|number)[]`.
- `inc(v, release: "major"|"minor"|"patch"|"premajor"|"preminor"|"prepatch"|"prerelease") → string`.
- `satisfies(version, range) → boolean` — full npm grammar (`^ ~ - x-ranges || ` and comparators).
- `maxSatisfying(versions: string[], range) → string | null`. `minSatisfying(versions, range) → string | null`.
- `coerce(s) → string | null` — best-effort extract a version.

## simd

Multi-ISA vector math over typed arrays. In-place ops mutate and return their first array argument.

- **f32 reductions** `(x: Float32Array)`: `sum → number`, `max`, `min`, `normL1`, `normL2 → number`, `argmax`, `argmin → number`.
- **f32 pairwise** `(a, b: Float32Array)`: `dot → number`, `distL1`, `distL2`, `distCos`, `distCheb → number`.
- **f32 elementwise** (into `z`): `add(z,a,b) sub mul div fma(z,a,b)`; `abs(out,in)`; `scale(a, s)`, `addScalar(a, s)`, `affine(a, s, b)` (in place); `clamp(a, lo, hi)`, `threshold(a, t)`.
- **f32 BLAS**: `axpy(y, alpha, x)` (y += αx); `gemv(y, A, x, m, n, beta)`, `gemvT(y, A, x, m, n, beta)`; `gemm(C, A, B, m, n, k, alpha, beta)` (C = αA·B + βC; A is m×k, B is k×n, C is m×n). All write+return the output array.
- **f32 activations/transcendentals** (in place): `sigmoid relu relu6 tanhFast gelu silu softmax logSoftmax vexp vlog vsqrt vrsqrt vinv(x)`; `leakyRelu(x, slope)`, `elu(x, alpha)`. `topkIndices(x, k) → number[]`.
- **f64** `(Float64Array)`: `f64Sum(x) → number`, `f64Dot(a, b) → number`, `f64Max(x)`, `f64Min(x)`, `f64Scale(x, s)`, `f64Axpy(y, a, x)`.
- **i32** `(Int32Array)`: `i32Sum(x) → number` (exact via int64), `i32Min/i32Max(x) → number`, `i32Dot(a, b) → number`, `i32Add/i32Mul(z, a, b)` (two's-complement wrap), `i32Scale(x, s)`.
- **prefix scans**: `cumsum(x)`, `cummax(x)` — accept `Int32Array` or `Float32Array`, inclusive, in place.

*Note:* AVX2/AVX‑512 kernels for f64/i32/utf16 are gated off pending AVX-hardware verification; AVX machines transparently run the verified SSE4.2/AVX2 path.

## ml

Classic models (native, SIMD-backed). Resource classes: `.fit()` returns `this`; `.close()` when done.

- `class LinearRegression` — `new LinearRegression()`; `.fit(X: number[][], y: number[]) → this`; `.predict(X: number[][]) → number[]`; `.close()`.
- `class LogisticRegression` — `.fit(X, y) → this`; `.predict(X) → number[]`; `.predictProba(X) → number[]`; `.close()`.
- `class KMeans` — `new KMeans(k: number, seed?: number)`; `.fit(points: number[][]) → this`; `.predict(points) → number[]`; `.inertia: number` (getter); `.close()`.

## compress

- `gzip(data) → Uint8Array` — real DEFLATE. `gunzip(data) → Uint8Array`.

## docparse

- `parseJson(s: string) → any` — parse JSON. `parseCsv(s: string) → string[][]` — RFC 4180 (quoting/embedded newlines), array of rows.

## sort

- `sort(arr: number[]) → number[]` — ascending, **returns a new array** (input untouched); rejects TypedArrays.
- `binarySearch(sortedArr: number[], target) → number` — index, or `-1` if absent.

## search

- `indexOf(haystack: string, needle: string) → number` — SIMD substring search; `-1` if absent.
- `indexOfAll(haystack, needle) → number[]` — all (overlapping) match start indices.
