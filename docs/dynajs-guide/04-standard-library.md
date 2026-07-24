# Chapter 4 — The Standard Library, Module by Module

This is the fun part — the heart of DynaJS. Everything here is native (C11), compiled into the
binary with `CONFIG_NATIVE_MODULES=y`, imported under `dyna:`, and completely dependency-free. You
don't need to read it front to back; skim the headings, find the capability you want, and steal the
example. Every snippet below was run against the real modules while writing this book, so what you
see is what you get.

## 4.0 How modules are shaped

Two shapes recur:

- **Plain-function modules** (`path`, `strings`, `bytes`, `encoding`, `crypto` one-shots, `time`,
  `mathx`, `bits`, `uuid`, `netip`, `simd`, `text`, `sort`, `search`, `compress`, `docparse`).
  Import functions, call them, get JavaScript values back. No lifecycle.
- **Resource-class modules** (`file`, `http`, `container`, `structures`, `ml`, `crypto`'s streaming
  `Hasher`, `random`'s `Random`). You `new` a class that owns a native resource, use it, then
  `close()` it (or `[Symbol.dispose]()`, or a `DisposableStack`). Disposal is **deterministic** —
  the native memory returns immediately (Chapter 3 §3.2, Chapter 5 §5.2).

A rule the whole library obeys: **native results are copied into fresh JavaScript values at the
call boundary.** No native pointer escapes into the JS heap; nothing you hold can dangle.

---

## 4.1 Text & bytes

### `dyna:strings` — Go-style string utilities

Fills the gaps around `String.prototype`, with native (some SIMD-accelerated) implementations.

```js
import { title, fields, split, splitN, contains, count, index, lastIndex,
         replace, replaceAll, equalFold, compare, trimChars, repeat, join } from "dyna:strings";

print(title("hello world"));              // "Hello World"
print(fields("  a  b   c ").join("|"));   // "a|b|c"   (split on whitespace runs, trimmed)
print(equalFold("Go", "GO"));             // true      (case-insensitive equality)
print(count("banana", "a"));              // 3
print(splitN("a,b,c,d", ",", 2).join("|"));// "a|b,c,d" (at most N pieces)
print(trimChars("xxhixx", "x"));          // "hi"
print(compare("apple", "banana"));        // -1        (lexicographic ordering)
```

`equalFold`, `fields`, `title`, and `splitN` are the genuine gaps — JavaScript has none of them
built in, and `contains`/`count` run over long strings using the SIMD substring engine (§4.8, §5.1).

### `dyna:bytes` — the byte buffer JavaScript never shipped

`DataView` is clumsy; `dyna:bytes` is the ergonomic, fast byte toolkit — comparison, search,
concatenation, and hex/base64/utf8 conversion, all native.

```js
import { toHex, fromHex, toBase64, fromBase64, toUtf8, fromUtf8,
         compare, equal, indexOf, count, concat, fill } from "dyna:bytes";

print(toHex(new Uint8Array([0xde, 0xad, 0xbe, 0xef])));   // "deadbeef"
print(toBase64(fromUtf8("hi")));                          // "aGk="
print(toUtf8(fromBase64("aGVsbG8=")));                    // "hello"

const a = fromUtf8("the quick brown fox");
print(indexOf(a, fromUtf8("quick")));                     // 4
print(count(a, fromUtf8("o")));                           // 2
print(toHex(concat(fromHex("dead"), fromHex("beef"))));   // "deadbeef"
```

Contrast: in Node you would reach for `Buffer` (a Node-specific global) or a `DataView` dance; in
Go this is `bytes` + `encoding/hex` + `encoding/base64`. DynaJS gives you one coherent native module
over standard `Uint8Array`.

### `dyna:encoding` — codecs beyond `atob`

Hex, standard and URL-safe base64, Ascii85/base85, and Go-style LEB128 var-ints — the encodings a
data-plane program actually needs.

```js
import { hexEncode, hexDecode, base64Encode, base64UrlEncode,
         base85Encode, base85Decode, putUvarint, uvarint } from "dyna:encoding";

print(hexEncode(new Uint8Array([1, 255])));       // "01ff"
print(base64UrlEncode(new Uint8Array([251,255,191]))); // "-_-_"  (URL-safe alphabet)

// Ascii85 — compact binary-to-text, round-trips exactly:
const a85 = base85Encode(hexDecode("deadbeef"));
print(a85, "→", hexEncode(base85Decode(a85)));    // "hQ=N\\" → "deadbeef"

// LEB128 var-ints (the wire format Protobuf/DWARF use):
const encoded = putUvarint(300);                  // → Uint8Array [0xac, 0x02]
const [value, read] = uvarint(encoded);           // → [300, 2]  (value, bytes read)
print(value, "in", read, "bytes");                // 300 in 2 bytes
```

`atob`/`btoa` cover only standard base64 of Latin-1 strings; this module covers the rest, over
bytes, natively.

### `dyna:text` — SIMD text kernels

Lower-level, throughput-oriented text operations built directly on the SIMD engine: UTF-8
validation and code-point counting, hex transcoding, Latin-1↔UTF-8, and multi-byte search — the
primitives a parser or protocol codec leans on.

```js
import { isValidUtf8, countUtf8, hexEncode, latin1ToUtf8, indexOfAny } from "dyna:text";

print(isValidUtf8(new Uint8Array([0xc3, 0xa9])));        // true  (é)
print(countUtf8(new Uint8Array([0xc3, 0xa9, 0x61])));    // 2     (code points, not bytes)
print(hexEncode(new Uint8Array([0xde, 0xad])));          // "dead"  (SIMD PSHUFB path on x86)
print(Array.from(latin1ToUtf8(new Uint8Array([0xe9]))).map(b=>b.toString(16)).join(" "));
//   "c3 a9"   (Latin-1 é → UTF-8, vectorized)
```

These accept `Uint8Array` views, `ArrayBuffer`, or strings. On long inputs they run at multiple
GiB/s (Chapter 5). This is the module the engine itself reuses internally for HTTP header scanning.

`text` also does **UTF-8 ↔ UTF-16 transcoding** (vectorized), for interop with UTF-16 systems:

```js
import { utf8ToUtf16, utf16ToUtf8, isValidUtf16, countUtf16 } from "dyna:text";

const u16 = utf8ToUtf16("hello 世界 😀");        // → UTF-16LE bytes (Uint8Array)
print(countUtf16(u16));                            // 10  (code points)
print(isValidUtf16(u16));                          // true — round-trips exactly
print(isValidUtf16(new Uint8Array([0x00, 0xD8]))); // false (lone high surrogate)
```

The policy is strict/lossless (simdutf `convert` semantics): a lone or misordered surrogate is an
**error** (the transcoders throw; `isValidUtf16` returns false), never silently replaced with U+FFFD.

---

## 4.2 Cryptographic hashing & identity

### `dyna:crypto` — hashes, HMAC, CRC

The full classic hash suite, verified against the published standard test vectors (FIPS 180-4,
RFC 1321/2104/4231, IEEE 802.3). One-shot helpers *and* a streaming `Hasher`.

```js
import { sha256Hex, sha512Hex, md5Hex, hmacHex, crc32, crc32c, Hasher } from "dyna:crypto";

print(sha256Hex("hello world"));
//   b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
print(hmacHex("sha256", "key", "message"));   // keyed MAC, hex
print(crc32(new Uint8Array([1, 2, 3])) >>> 0);// IEEE CRC-32 as unsigned

// Streaming: hash data you never fully hold in memory.
const h = new Hasher("sha256");
try {
  h.update("part one ");
  h.update("part two");
  print(h.digestHex());                        // same as sha256Hex("part one part two")
} finally {
  h.close();                                   // deterministic release
}
```

Every one-shot has a `*Hex` variant (`sha256`/`sha256Hex`) so you rarely convert manually.
Algorithms: MD5, SHA-1, SHA-224/256/384/512, HMAC over any of them, CRC-32 and CRC-32C. Contrast:
Node's `crypto` is a large module (and browser `SubtleCrypto` is async-only); DynaJS gives you the
common hashing surface synchronously, natively, with no import ceremony.

### `dyna:uuid` — RFC 9562 UUIDs

The modern UUID spec, including the time-ordered **v7** that has become the default choice for
database keys.

```js
import { v4, v7, v5, parse, validate, version, NAMESPACE_DNS } from "dyna:uuid";

print(v4());                              // random:       a random UUIDv4
print(v7());                              // time-ordered: sorts by creation time
print(v5(NAMESPACE_DNS, "www.example.com"));
//   2ed6657d-e927-568b-95e1-2665a8aea6a2   (deterministic, name-based)
print(version(v7()));                     // 7
print(validate("not-a-uuid"));            // false
```

Why v7 matters: v4 UUIDs are random, so as primary keys they scatter B-tree inserts. v7 embeds a
millisecond timestamp in the high bits, so freshly-minted ids are monotonically increasing — index
locality without a central sequence. JavaScript has no built-in UUID generator at all; Node needs
`crypto.randomUUID` (v4 only). DynaJS ships v4, v7, and the name-based v3/v5.

### `dyna:random` — seedable PRNG

A fast, **seedable** generator — reproducible streams for tests, simulations, and sampling
(`Math.random` is neither seedable nor reproducible).

```js
import { Random } from "dyna:random";

const rng = new Random(42);               // seed with a Number or BigInt
print(rng.nextFloat().toFixed(4));        // 0.0839  (deterministic for seed 42)
print(rng.nextBounded(6) + 1);            // a fair die roll in [1,6]
print(rng.nextU64());                     // a full 64-bit BigInt

const buf = new Uint8Array(16);
rng.fill(buf);                            // fill a buffer with random bytes

// Same seed ⇒ identical stream (Number and BigInt seeds agree):
print(new Random(7).nextU64() === new Random(7n).nextU64());   // true
```

---

## 4.3 Numbers & bits

### `dyna:mathx` — the math `Math` is missing

Special functions, exact integer helpers, and constants that `Math` never included.

```js
import { gamma, erf, hypot, gcd, lcm, factorial, isPrime, cbrt, Phi } from "dyna:mathx";

print(gamma(5));                  // 24        (Γ(5) = 4!)
print(erf(1).toFixed(6));         // 0.842701  (the error function)
print(hypot(3, 4));               // 5         (overflow-safe)
print(cbrt(27));                  // 3
print(gcd(462n, 1071n));          // 21n
print(lcm(4n, 6n));               // 12n
print(factorial(25));             // 15511210043330985984000000n  (exact BigInt)
print(isPrime(1000000007n));      // true
print(Phi);                       // 1.618033988749895  (golden ratio)
```

`gcd`/`lcm`/`factorial` return `BigInt` because their results routinely exceed `Number` range —
`factorial(25)` is a 26-digit integer, computed exactly. `isPrime` is a deterministic Miller-Rabin
test valid across the full 64-bit range. Contrast: this is Go's `math` + `math/big` primitives, in
one small module.

### `dyna:bits` — Go's `math/bits`, exactly

Bit-twiddling primitives at fixed widths (8/16/32/64), plus full-width add/sub/mul/div with carry.

```js
import { LeadingZeros32, OnesCount64, RotateLeft8, ReverseBytes32,
         Len32, Mul64, Add64, Div64 } from "dyna:bits";

print(LeadingZeros32(1));          // 31
print(OnesCount64(0xffffn));       // 16        (popcount, 64-bit)
print(RotateLeft8(1, -1));         // 128       (negative k rotates right)
print(ReverseBytes32(0x01020304)); // 67305985  (= 0x04030201, byte swap)
print(Len32(1000));                // 10        (bits needed to represent)

// Full 128-bit multiply and carry-aware add — no precision loss:
const [hi, lo] = Mul64(0xffffffffffffffffn, 0xffffffffffffffffn);
print(hi, lo);                     // 18446744073709551614n 1n
const [sum, carry] = Add64(0xffffffffffffffffn, 1n, 0n);
print(sum, carry);                 // 0n 1n
```

64-bit variants take and return `BigInt` (exact); the 8/16/32 variants use `Number`. `Div64`/`Rem64`
throw on a zero divisor or quotient overflow, matching Go. This is the toolkit for hashing,
bit-set data structures, codec framing, and low-level protocol work — none of which JavaScript
supports beyond `Math.clz32`.

---

## 4.4 Collections

### `dyna:container` — heap, doubly-linked list, ring (Go's `container/*`)

Native data structures JavaScript lacks. Each owns native memory and is `close()`d when done.

```js
import { Heap, List, Ring } from "dyna:container";

// A binary heap with a user comparator — a real priority queue.
const pq = new Heap((a, b) => a - b);           // min-heap
[5, 1, 4, 2, 8].forEach(v => pq.push(v));
const sorted = [];
while (pq.size) sorted.push(pq.pop());
print(sorted.join(","));                        // "1,2,4,5,8"
pq.close();

// A doubly-linked list — O(1) push/pop at both ends (a deque).
const dq = new List();
dq.pushBack(2); dq.pushFront(1); dq.pushBack(3);
print(dq.toArray().join(","));                  // "1,2,3"
print(dq.popFront(), dq.popBack());             // 1 3
dq.close();

// A fixed-capacity ring buffer — overwrites oldest on overflow.
const ring = new Ring(3);
[1, 2, 3, 4, 5].forEach(v => ring.push(v));
print(ring.toArray().join(","));                // "3,4,5"  (last 3 kept)
ring.close();
```

The `Heap` is the standout: JavaScript has no priority queue, so people hand-roll one or pull an
npm package on every project. Here it is a native, comparator-driven class — and its comparator
handling is hardened against re-entrancy (a comparator that closes the heap mid-operation cannot
corrupt memory).

### `dyna:structures` — growable vector & hash map

```js
import { Vector, HashMap } from "dyna:structures";

const v = new Vector();
v.push(10); v.push(20); v.push(30);
print(v.length, v.get(1));                      // 3 20
v.close();

const m = new HashMap();
m.set("a", 1); m.set("b", 2);
print(m.has("a"), m.get("b"), m.size);          // true 2 2
m.delete("a");
print(m.has("a"));                              // false
m.close();
```

`Map` and `Array` cover most needs; `structures` exists for cases where a native, contiguous
backing store or specific memory behavior matters. (By the curation bar of Chapter 7 these overlap
built-ins the most — they are here for the native-memory story, not to replace `Map`/`Array`.)

---

## 4.5 Files & paths

### `dyna:path` — path manipulation (no built-in exists)

```js
import { join, resolve, normalize, dirname, basename, extname, relative, isAbsolute } from "dyna:path";

print(join("a", "b", "..", "c"));         // "a/c"
print(normalize("./a//b/../c"));          // "a/c"
print(dirname("/usr/local/bin/dynajs"));  // "/usr/local/bin"
print(basename("/x/report.csv"));         // "report.csv"
print(extname("/x/report.csv"));          // ".csv"
print(relative("/a/b/c", "/a/x/y"));      // "../../x/y"
print(isAbsolute("/etc"), isAbsolute("etc")); // true false
```

Pure path-string logic (POSIX semantics). JavaScript has nothing here; Node's `path` is the model.

### `dyna:file` — the filesystem module

Buffered `FileReader`/`FileWriter` plus one-shot `readFile`/`writeFile`, **and all filesystem
operations** — `stat`/`lstat`/`exists`, `readDir`/`makeDir`/`remove`/`removeAll`/`rename`,
`symlink`/`readLink`/`realPath`/`chmod`, `glob`, and `tempDir`/`makeTempDir`/`makeTempFile` (these
moved here from `dyna:sys`, which now holds only process/environment). Under the hood the content I/O
uses the platform's best primitives — `F_RDAHEAD`/`F_PREALLOCATE`/`F_FULLFSYNC` on macOS, `fadvise`/
`fallocate`/io_uring on Linux — behind one identical API.

```js
import { readFile, writeFile, FileReader, FileWriter,
         stat, readDir, makeDir, glob } from "dyna:file";

// One-shot:
writeFile("/tmp/demo.txt", "line one\nline two\n");
print(readFile("/tmp/demo.txt").length);        // 18

// Buffered writer with preallocation (fewer syscalls, no fragmentation):
const w = new FileWriter("/tmp/big.txt", { bufferSize: 1 << 16, preallocate: 1 << 20 });
for (let i = 0; i < 100000; i++) w.write(`row ${i}\n`);
w.sync();                                        // durable flush (F_FULLFSYNC on macOS)
w.close();

// Buffered reader, line by line across buffer refills:
const r = new FileReader("/tmp/big.txt", { bufferSize: 1 << 16 });
let lines = 0, line;
while ((line = r.readLine()) !== null) lines++;
r.close();
print("read", lines, "lines");                  // read 100000 lines
```

`writeFile` returns the byte count; `readLine()` returns `null` at EOF; `sync()` forces a durable
flush. The buffering and preallocation matter: a naïve write-per-line loop is syscall-bound, while
this batches into large buffers and preallocates the file extent.

### `dyna:uring` — io_uring bulk file read (Linux)

On Linux, a high-queue-depth whole-file reader and checksummer built on **io_uring** — true async
disk I/O for throughput-bound bulk reads.

```js
// Linux only (built with io_uring support):
import { readFile, checksum } from "dyna:uring";

const data = readFile("/var/log/big.log");      // async io_uring under the hood
print(data.length, "bytes");
print("crc:", checksum("/var/log/big.log"));    // streamed checksum, high QD
```

This is the "cheap read-heavy win" path: io_uring submits many read requests without a syscall per
block, so large sequential reads saturate the device. On non-Linux builds, use `dyna:file`.

---

## 4.6 Networking

### `dyna:http` — a client, an application server, and a static reactor

`dyna:http` gives you three tools that share one foundation. There's a synchronous **`HttpClient`**,
a full application server (**`App`**) where *you* write the handlers, and a lower-level static-only
reactor (`HttpServerAsync`). All three sit on the same single-thread event-loop reactor — kqueue on
macOS, epoll or io_uring on Linux — so they scale to thousands of connections on one thread
(Chapter 5 §5.4).

**The client** is the simplest piece — a blocking request/response object:

```js
import { HttpClient } from "dyna:http";

const client = new HttpClient();
try {
  const r = client.get("http://example.com/");
  print(r.status);                       // 200
  print(r.headers["Content-Type"]);      // e.g. "text/html; charset=UTF-8"
  print(r.body.length, "bytes");
} finally {
  client.close();
}
```

A response is a plain `{ status, headers, body }` object. `client.post(url, body, headers?)` and the
general `client.request(url, { method, headers, body })` round it out.

**`App` is the server you actually build on.** You never touch raw HTTP — you register a handful of
*typed routes* and DynaJS takes care of the wire protocol, parsing, and connection lifecycle. There
are four kinds of route, each doing one job well:

- **`app.rpc(path, methods)`** — a strict **JSON-RPC 2.0** endpoint. This is where your business
  logic goes. Each method is just `(params) => result`; DynaJS parses the request, calls the right
  one, and serializes the reply (including proper JSON-RPC error objects). No REST guesswork.
- **`app.static(prefix, dir, { maxFileSize, allow })`** — serve files from a directory over a
  zero-copy `sendfile` path. `allow` is a whitelist of `.ext`s or MIME types.
- **`app.upload(path, { dir, maxFileSize, allow }, handler)`** — stream an upload straight to disk,
  then call `handler(savedPath, meta)` with where it landed (`meta` has `size` and `contentType`).
- **`app.ws(path, { open, message, close })`** — a full **RFC 6455 WebSocket** endpoint.

```js
import { App } from "dyna:http";

const app = new App({ port: 8080 });

// Business logic — a JSON-RPC 2.0 service:
app.rpc("/rpc", {
  add:   ([a, b]) => a + b,
  greet: ({ name }) => `hello ${name}`,
});

// Static files, extension-whitelisted:
app.static("/assets", "/var/www/assets", { maxFileSize: 8 << 20, allow: [".css", ".js", ".png"] });

// Uploads streamed to disk, then handed to you:
app.upload("/upload", { dir: "/var/uploads", maxFileSize: 32 << 20, allow: ["image/png"] },
  (savedPath, meta) => print("saved", meta.size, "bytes to", savedPath));

// A WebSocket echo endpoint:
app.ws("/ws", {
  open:    (ws) => ws.send("welcome"),
  message: (ws, data, isBinary) => ws.send(isBinary ? data : "echo: " + data),
  close:   (ws) => {},
});

app.start();
print("serving on", app.port);
```

Because the server runs on *this program's* event loop, you drive it from another process. From a
shell (this is a real, verified round-trip):

```sh
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"add","params":[2,3]}' \
  http://127.0.0.1:8080/rpc
#   {"jsonrpc":"2.0","result":5,"id":1}
```

A few things worth knowing:

- **Handlers run on the JS thread**, right on the reactor loop — so a handler shares the same heap
  as the rest of your program (no cross-thread copying), and a request arrives as a zero-copy view.
  The catch: **don't block the loop.** A CPU-heavy handler stalls every other connection — push that
  work to an `os.Worker` (Chapter 3 §3.4). For the same reason, a *same-thread* blocking `HttpClient`
  can't call your own `App` (one thread can't both wait and serve) — drive it from outside.
- **Give `App` an explicit port.** Unlike `HttpServerAsync`, `app.port` reports the port you asked
  for, so `port: 0` is not resolved to an OS-assigned one — pick a real port.
- An RPC method may return a value synchronously *or* a `Promise` (for single requests; a *batched*
  JSON-RPC call needs synchronous methods).

**`HttpServerAsync` is the bare static reactor underneath.** It maps paths to fixed responses and
serves them without ever entering the JavaScript world — great for static content and for showing
off the reactor's raw concurrency, but it **cannot run your code.** For anything with logic, use
`App` above.

```js
import { HttpServerAsync, HttpClient } from "dyna:http";

const server = new HttpServerAsync({ port: 0, routes: {   // port 0 → a free OS port
  "/":     "hello world\n",
  "/json": { status: 200, contentType: "application/json", body: '{"a":1}' },
}});
server.start();

const client = new HttpClient();
print(client.get(`http://127.0.0.1:${server.port}/json`).status);   // 200
client.close();
server.stop();
```

(`HttpServer` is an older thread-pool variant with the same `start`/`stop`/`port` shape; prefer the
reactor.)

### `dyna:netip` — IP addresses & CIDR (Go's `net/netip`)

Parsing and reasoning about IPv4/IPv6 addresses and CIDR prefixes — a capability JavaScript lacks
entirely.

```js
import { parseAddr, contains, masked, canonical, isValid, isLoopback } from "dyna:netip";

const a = parseAddr("::ffff:127.0.0.1");
print(a.is4, a.is6);                     // false true   (a mapped address is an IPv6 value)
print(isLoopback("::ffff:127.0.0.1"));   // true         (classifiers unmap the IPv4-in-IPv6 first)
print(canonical("2001:0db8:0000:0000:0000:0000:0000:0001")); // "2001:db8::1" (RFC 5952)
print(contains("10.0.0.0/8", "10.1.2.3"));                   // true
print(contains("192.168.0.0/16", "10.0.0.1"));               // false
print(masked("192.168.5.130/24"));                           // "192.168.5.0"
print(isValid("999.1.1.1"));                                 // false
```

A small but important detail: `parseAddr("::ffff:127.0.0.1")` is an **IPv6** value (`is4` is
`false`) — the raw parse keeps the 16-byte form. The *classifiers* (`isLoopback`, `isPrivate`, …)
unmap an IPv4-in-IPv6 address first, so `isLoopback("::ffff:127.0.0.1")` is `true`. `canonical`
produces the RFC 5952 form (longest zero-run compressed, lowercase); `contains` does the
masked-prefix comparison at any bit boundary. This is the module for allow-lists, subnet routing,
and address classification.

---

## 4.7 Time

### `dyna:time` — durations, monotonic clock, RFC 3339 (Go's `time`)

`Date` handles wall-clock timestamps; `dyna:time` adds what Go's `time` package gives you —
typed durations, a monotonic clock, and precise formatting/parsing.

```js
import { Second, Minute, Hour, durationString, parseDuration,
         nowUnixNano, monotonicNano, formatRFC3339, parseRFC3339, fromUnix } from "dyna:time";

print(durationString(90 * Number(Minute)));   // "1h30m0s"
print(parseDuration("1h30m"));                // 5400000000000  (nanoseconds)
print(formatRFC3339(fromUnix(0, 0)));         // "1970-01-01T00:00:00Z"

// Monotonic clock for measuring elapsed time (immune to wall-clock jumps):
const t0 = monotonicNano();
for (let i = 0; i < 1e6; i++) { /* work */ }
const elapsedMs = Number(monotonicNano() - t0) / 1e6;
print(`took ${elapsedMs.toFixed(2)} ms`);
```

Durations are nanosecond-precise; `monotonicNano()` is the right clock for benchmarks and timeouts
(unlike `Date.now()`, it never goes backwards). `formatRFC3339`/`parseRFC3339` handle the
interchange format APIs actually use.

---

## 4.8 Compute: SIMD & ML

### `dyna:simd` — a multi-ISA vector-math engine, from JavaScript

This is DynaJS's signature capability. A native SIMD kernel set — dispatched at runtime to the best
instruction set your CPU has (scalar / NEON / SSE4.2 / AVX2 / AVX-512 / SVE) — exposed directly to
JavaScript over typed arrays. No native addon, no build step.

```js
import { dot, sum, normL2, distL2, distCos, axpy, scale,
         relu, sigmoid, softmax, gelu, gemv, gemm, argmax,
         f64Sum, f64Dot } from "dyna:simd";

const a = new Float32Array([1, 2, 3, 4]);
const b = new Float32Array([5, 6, 7, 8]);

print(dot(a, b));                 // 70          (vectorized dot product)
print(sum(a));                    // 10
print(normL2(new Float32Array([3, 4])));         // 5   (Euclidean norm)
print(distCos(a, b).toFixed(4));  // cosine *distance* between vectors
print(argmax(a));                 // 3           (index of the max)

// BLAS-style: y = alpha·x + y  (in place, returns y)
const y = new Float32Array([1, 1, 1, 1]);
axpy(y, 10, a);                   // y becomes [11, 21, 31, 41]

// Neural-net activations, elementwise, vectorized:
const logits = new Float32Array([2.0, 1.0, 0.1]);
softmax(logits);                  // in place → a probability distribution
print(Array.from(logits).map(x => x.toFixed(3)).join(",")); // "0.647,0.252,0.102"

// GEMM: C = alpha·A·B + beta·C.  A is m×k, B is k×n, C is m×n.
const A = new Float32Array([1, 2, 3, 4]);    // 2×2
const I = new Float32Array([1, 0, 0, 1]);    // 2×2 identity
const C = new Float32Array(4);
gemm(C, A, I, 2, 2, 2, 1, 0);     // C = A·I
print(Array.from(C).join(","));   // "1,2,3,4"

// Double precision (f64) — because JS Number *is* f64:
print(f64Sum(new Float64Array([0.1, 0.2, 0.3])).toFixed(1)); // 0.6
```

The full surface includes reductions (`sum`/`max`/`min`/`argmax`/`argmin`), elementwise arithmetic
(`add`/`sub`/`mul`/`div`/`abs`/`fma`/`scale`/`axpy`), norms and distances (`normL1`/`normL2`,
`distL1`/`distL2`/`distCos`/`distCheb`), activations (`relu`/`relu6`/`leakyRelu`/`elu`/`gelu`/
`silu`/`sigmoid`/`tanhFast`/`softmax`/`logSoftmax`), transcendentals (`vexp`/`vlog`/`vsqrt`/
`vrsqrt`/`vinv`), BLAS-2/3 (`gemv`/`gemvT`/`gemm`), and utilities (`clamp`/`threshold`/`topkIndices`),
in both f32 and (for the core reductions/elementwise) f64. **Integer** (`Int32Array`) reductions and
elementwise ops — `i32Sum` (exact via int64), `i32Min`/`i32Max`/`i32Dot`, `i32Add`/`i32Mul`/`i32Scale`
(two's-complement wrap, like `Math.imul`) — and **prefix scans** `cumsum`/`cummax` (over `Int32Array`
or `Float32Array`) round out the array toolkit:

```js
import { i32Sum, i32Dot, cumsum, cummax } from "dyna:simd";

const a = new Int32Array([5, 2, 8, 1, 9]);
print(i32Sum(a));                 // 25       (widened, exact)
print(i32Dot(a, a));              // 175      (Σ aᵢ²)
const c = new Int32Array([1, 2, 3, 4]); cumsum(c);
print(Array.from(c).join(","));   // "1,3,6,10"   (inclusive prefix sum, in place)
const m = new Float32Array([3, 1, 4, 1, 5]); cummax(m);
print(Array.from(m).join(","));   // "3,3,4,4,5"  (running maximum)
```

Why this is a differentiator: in Node or Bun, vector math from JS means either a slow scalar loop
(and hoping the JIT vectorizes it — it often can't, due to aliasing and early-exit rules) or
shipping a per-platform native addon. DynaJS puts a verified, cross-ISA SIMD library *in the
runtime*. It is **dual-use**: the same kernels accelerate the engine internally (e.g. HTTP header
scanning uses the SIMD substring search) and are available to your code.

> A note on honesty: the SIMD kernels are differentially verified (each ISA against a scalar
> reference) on the hardware/emulation available. Certain AVX-512 paths are conservatively gated
> and fall back to the verified AVX2 path until they can be exercised on real AVX-512 silicon —
> DynaJS ships the proven path rather than an unverified one.

### `dyna:ml` — classic ML models

Native `LinearRegression`, `LogisticRegression`, and `KMeans`, built on the same SIMD engine.

```js
import { LinearRegression, KMeans } from "dyna:ml";

// Fit y = 2x + 1 and predict.
const lr = new LinearRegression();
try {
  const X = [], y = [];
  for (let x = 0; x < 20; x++) { X.push([x]); y.push(2 * x + 1); }
  lr.fit(X, y);
  print(lr.predict([[100], [0]]));          // ≈ [201, 1]  (recovered y = 2x + 1)
} finally {
  lr.close();
}

// Cluster 2-D points into k groups.
const km = new KMeans(2);
try {
  km.fit([[0, 0], [0.1, 0], [10, 10], [10.1, 10]]);
  print(km.predict([[0.05, 0], [10, 10]]));  // [clusterA, clusterB]
  print("inertia:", km.inertia.toFixed(3));  // .inertia is a getter, not a method
} finally {
  km.close();
}
```

These are resource classes (`fit` returns `this` for chaining; `close()` when done). They exist
because doing this in plain JS is slow, and pulling a Python/Node ML stack is heavy — here it is a
native module using the in-binary SIMD kernels.

---

## 4.9 Data formats & algorithms

### `dyna:compress` — real DEFLATE (gzip)

```js
import { gzip, gunzip } from "dyna:compress";

const original = "the quick brown fox ".repeat(100);        // 2000 bytes
const packed = gzip(original);                               // real DEFLATE
print(original.length, "→", packed.length, "bytes");        // 2000 → 56 bytes
print(gunzip(packed).length === original.length);           // round-trips
```

A genuine DEFLATE implementation (not a stub) — the standard `gzip`/`gunzip` pair over bytes.

### `dyna:docparse` — fast JSON & CSV parsing

```js
import { parseJson, parseCsv } from "dyna:docparse";

print(JSON.stringify(parseCsv("a,b\n1,2\n3,4")));   // [["a","b"],["1","2"],["3","4"]]
const obj = parseJson('{"x": [1, 2, 3], "y": true}');
print(obj.x[2], obj.y);                             // 3 true
```

`parseCsv` handles RFC-4180 quoting/embedded newlines; both are native and fast — the module the
data-ingestion path uses.

### `dyna:csv` — CSV files as a mini database

Where `docparse.parseCsv` parses a CSV *string*, `dyna:csv` treats a CSV *file* as an editable
dataset — create it, page through it, mutate rows and columns — with RFC-4180 quoting, a
SIMD-accelerated parse, and **atomic writes** (a crash mid-write never corrupts the file). The module
exports a single class, **`CSVFile`**: construct it with a path, then call methods on it. Each method
takes an options object; row indices are 0-based over data rows.

```js
import { CSVFile } from "dyna:csv";

const people = new CSVFile("/tmp/people.csv");
people.create({ headers: ["Name", "Age", "City"],
                rows: [["Alice", "30", "NYC"]], overwrite: true });
people.addRow({ rows: [{ Name: "Bob", Age: "25", City: "LA" }] });
people.updateCell({ row: 0, column: "City", value: "Brooklyn" });
people.addColumn({ column: "Active", defaultValue: "yes" });

const page = people.read({ offset: 0, limit: 50, columns: ["Name", "City"] });
print(page.headers, page.rows, "of", page.totalRows);
// ["Name","City"] [["Alice","Brooklyn"],["Bob","LA"]] of 2
```

The eleven methods — `create`, `read`, `addRow`, `updateCell`, `removeRow`, `addColumn`,
`removeColumn`, `renameColumn`, `readColumnValuesRange`, `readRowRange`, `selectColumnRange` — each
take a single options object (easy to expose as an MCP tool). The path is bound once at construction,
so a single instance is reused across operations. Reads mmap the file; a 100k-row file creates and
reads back in a few milliseconds each. See the [API Reference](API.md#csv) for every option.

### `dyna:sort` — sorting & binary search

```js
import { sort, binarySearch } from "dyna:sort";

const sorted = sort([3, 1, 2, 5, 4]);        // returns a NEW sorted array
print(sorted.join(","));                     // "1,2,3,4,5"
print(binarySearch(sorted, 4));              // 3   (index of 4)
print(binarySearch(sorted, 99));             // -1  (not found)
```

Note the semantics: `sort` returns a **new** array (it does not mutate its input), matching a
functional style. `binarySearch` requires a sorted array and returns the index or `-1`.

### `dyna:search` — substring / subsequence search

```js
import { indexOf, indexOfAll } from "dyna:search";

print(indexOf("the quick brown fox", "quick"));   // 4
print(JSON.stringify(indexOfAll("abababa", "aba")));// [0, 2, 4]  (overlapping matches)
```

Backed by the SIMD substring engine — on long haystacks this runs at multiple GiB/s, and it is the
same kernel the HTTP server uses internally for header scanning (measured ~4.75× faster than the
scalar scan it replaced).

---

## 4.10 System: filesystem and process (`dyna:file` + `dyna:sys`)

The **filesystem** surface lives in `dyna:file` (alongside buffered content I/O, §4.5): metadata,
directories, globbing, links, and temp files. `dyna:sys` keeps only the **process/environment**
surface. (Path-*string* logic stays in `dyna:path` — no duplication.) Everything is synchronous.

```js
import { stat, readDir, makeDir, removeAll, glob, exists,
         makeTempDir, writeFile } from "dyna:file";
import { getEnv, platform } from "dyna:sys";

const root = makeTempDir("demo-");               // fresh unique temp dir
makeDir(root + "/a/b", { recursive: true });
writeFile(root + "/a/x.txt", "hi");
writeFile(root + "/a/b/y.js", "1");

print(readDir(root + "/a").map(e => e.name + (e.isDir ? "/" : ""))); // ["b/", "x.txt"]
print(glob(root + "/**/*.js"));                  // recursive: ** / * / ? / [a-c] / [!..]
print(stat(root + "/a/x.txt").size, stat(root + "/a").isDir);        // 2 true
print(platform(), getEnv("HOME") !== undefined);                     // "darwin" true

removeAll(root);                                 // recursive, symlink-safe, missing = no-op
print(exists(root));                             // false
```

Highlights: `glob` is a self-contained matcher (`*`, `**`, `?`, `[...]`, ranges, negation),
symlink-cycle-safe; `removeAll` uses `openat` + `O_NOFOLLOW` so it can never delete outside the tree
through a symlink; every error throws an `Error` carrying `.code` (`"ENOENT"`) and `.errno`; `readDir`
returns sorted `{ name, isDir, isFile, isSymlink }` entries. Filesystem surface (`dyna:file`):
`stat`/`lstat`/`exists`, `readDir`/`makeDir`/`remove`/`removeAll`/`rename`,
`symlink`/`readLink`/`realPath`/`chmod`, `glob`, `tempDir`/`makeTempDir`/`makeTempFile`. Process
surface (`dyna:sys`): `env`/`getEnv`/`setEnv`/`args`/`cwd`/`chDir`/`platform`/`pid`/`hostName`/`homeDir`.

## 4.11 Semantic versioning (`dyna:semver`)

SemVer 2.0.0 parsing, comparison, and npm-style range satisfaction — the capability behind the npm
`semver` package, delivered as a curated native module (our own API, not a port).

```js
import { parse, compare, satisfies, inc, maxSatisfying, coerce, sort } from "dyna:semver";

print(compare("1.0.0-alpha", "1.0.0"));  // -1   (a prerelease is lower than the release)
print(satisfies("1.2.9", "^1.2.3"));     // true
print(satisfies("0.3.0", "^0.2.3"));     // false (caret is special for 0.x: >=0.2.3 <0.3.0)
print(inc("1.2.3", "minor"));            // "1.3.0"
print(maxSatisfying(["1.0.0", "1.2.0", "1.9.0", "2.0.0"], "^1.2.0")); // "1.9.0"
print(coerce("v2.3.4-x"));               // "2.3.4"
print(sort(["2.0.0", "1.0.0-rc.1", "1.0.0"]).join(" < ")); // "1.0.0-rc.1 < 1.0.0 < 2.0.0"
```

The full npm range grammar is supported — exact, comparators (`>` `>=` `<` `<=` `=`), caret `^`
(including the 0.x rules), tilde `~`, hyphen `a - b`, x-ranges (`1.x`, `1.2.*`, `*`), AND (space) and
OR (`||`), plus npm's prerelease rule (a prerelease only satisfies a range whose comparator carries a
prerelease at the same `major.minor.patch`).

## 4.12 The module map at a glance

| Module | Fills the gap of | One-line why |
|---|---|---|
| `path` | Node `path` | path-string logic JS has none of |
| `strings` | Go `strings` | `title`/`fields`/`equalFold`/SIMD `contains` |
| `bytes` | Node `Buffer`, Go `bytes` | ergonomic byte ops + hex/base64/utf8 |
| `encoding` | Go `encoding/*` | hex/base64url/base85/varint |
| `text` | (SIMD primitives) | UTF-8 validate/count, transcode at GiB/s |
| `crypto` | Node `crypto` | hashes/HMAC/CRC, streaming, vector-verified |
| `uuid` | `crypto.randomUUID` | v4 **+ v7** + v3/v5 (RFC 9562) |
| `random` | (seedable PRNG) | reproducible streams, `Math.random` can't |
| `mathx` | Go `math`+`math/big` | gamma/erf/gcd/lcm/factorial/isPrime |
| `bits` | Go `math/bits` | fixed-width bit ops + 64-bit carry math |
| `container` | Go `container/*` | **heap**/list/ring JS lacks |
| `structures` | (native collections) | vector/hashmap with native backing |
| `file` | Node `fs` | buffered I/O + OS fast paths |
| `uring` | (Linux io_uring) | high-QD async bulk reads |
| `http` | Node `http` | `App` server (rpc/static/upload/ws) + client |
| `netip` | Go `net/netip` | IP/CIDR parsing & reasoning |
| `time` | Go `time` | durations/monotonic/RFC3339 |
| `sys` | Node `fs`+`os`, `glob`/`rimraf` | unified filesystem + process + glob |
| `semver` | npm `semver` | SemVer parse/compare/ranges |
| `simd` | (native addon) | multi-ISA vector math in the runtime |
| `ml` | (Python/Node ML) | native regression/kmeans |
| `compress` | Node `zlib` | real DEFLATE gzip |
| `docparse` | (fast parsers) | native JSON/CSV |
| `csv` | CSV editor libs | file CRUD, RFC 4180, mmap + atomic |
| `sort` | (sort libs) | sort + binary search |
| `search` | (search libs) | SIMD substring, overlapping matches |

---

*Next: [Chapter 5 — Performance Architecture](05-performance-architecture.md) — how these modules
are fast, and why the runtime holds flat memory.*
