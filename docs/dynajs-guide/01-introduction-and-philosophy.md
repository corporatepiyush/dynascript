# Chapter 1 — Introduction & Philosophy

## 1.1 What DynaJS is, in one paragraph

DynaJS is a JavaScript runtime built on a fork of Fabrice Bellard's **QuickJS** engine (the
`2026-06-04` release). QuickJS is famous for being tiny, correct, and embeddable: a complete
ES2023+ engine in a handful of C files, with a bytecode compiler, a small register/stack virtual
machine, reference-counting garbage collection, and near-instant startup. DynaJS keeps that core
and adds what a *runtime* needs to be useful on its own: a curated, native, **SIMD-accelerated
standard library** exposed to JavaScript under the `dyna:` module namespace, an async I/O
reactor, buffered file I/O, an HTTP client and server, cryptographic hashes, compression, a
multi-ISA vector-math engine, and a growing catalogue of the utilities you normally reach to npm or
the Go standard library for — all shipped inside the binary, with no package manager and no
`node_modules`.

If Node.js is "V8 plus libuv plus the npm ecosystem," and Bun is "JavaScriptCore plus Zig plus
npm-compatibility," then DynaJS is "**QuickJS plus a hand-built, dependency-free, performance-first
standard library**." It is a different bet.

## 1.2 The QuickJS lineage — why it matters

Understanding where DynaJS came from explains most of its character. QuickJS was engineered by
Bellard (of FFmpeg, QEMU, and the Bellard formula fame) around a few uncompromising ideas:

- **Small and complete.** The whole engine is a unity build — a master C file that `#include`s
  layered source fragments — compiling to a compact static library and a ~1 MB interpreter. There
  is no JIT, no multi-megabyte snapshot, no warmup.
- **Fast startup, low memory.** Because there is no JIT to warm and no heavyweight heap to
  initialize, a QuickJS process is ready in microseconds and idles in a few megabytes. This is the
  opposite end of the spectrum from V8, which trades startup and memory for peak throughput on
  long-running hot loops.
- **Correctness first.** QuickJS tracks the ECMAScript test-suite (`test262`) closely. DynaJS
  inherits and *guards* that: the project holds a fixed `test262` baseline and every engine change
  must keep it — modern language features are added without regressing conformance.
- **Reference-counting GC with cycle collection.** Objects are freed deterministically the moment
  their last reference drops; a separate trial-deletion pass reclaims cycles. Combined with the
  runtime's native-module memory model (Chapter 5), this gives DynaJS remarkably *flat* memory
  behavior — no stop-the-world pauses tied to a tracing collector's schedule.

Modern QuickJS (the 2026 vintage DynaJS forks) is not the old 2019 engine. It has **rope strings**
(concatenation without copying), a **slab allocator** that stores the refcount in the malloc block
header, a **short-BigInt** representation, and a **token-threaded (computed-goto) interpreter** for
fast dispatch. DynaJS builds on all of that.

## 1.3 The core thesis: a runtime is its standard library

A bare engine executes JavaScript. A *runtime* lets you build things: read files, hash bytes, open
sockets, parse data, do math at speed. Node.js answered "how do we build things?" with a large
C++ surface plus, crucially, **npm** — hundreds of thousands of third-party packages. That answer
made Node ubiquitous. It also made every Node application a distributed-systems problem in
miniature: transitive dependency trees thousands deep, supply-chain risk, `node_modules`
directories larger than the programs that use them, and semver churn.

DynaJS makes the opposite bet: **the runtime should ship the batteries itself**, curated and
native. Not "install `left-pad`," but `import { title } from "dyna:strings"`. Not "add a crypto
npm package," but `import { sha256 } from "dyna:crypto"`. Not "pull in a SIMD add-on compiled per
platform," but `import { dot } from "dyna:simd"`. The functionality lives in the binary, is
written in C, is SIMD-accelerated where it pays, and is verified in-tree.

This is why the standard library is the heart of this book (Chapter 4). It is also why DynaJS is
**deliberately not Node-compatible** — the two philosophies pull in opposite directions.

## 1.4 What DynaJS deliberately does *not* do

Being explicit about non-goals is part of the design.

- **No Node.js compatibility.** No `require()`, no CommonJS, no `node:fs`/`node:http`/`node:crypto`
  shims, no attempt to load npm packages. DynaJS modules live under `dyna:` and have their own,
  often nicer, APIs. (See §1.6 and Chapter 9.)
- **No JIT (today).** DynaJS is an interpreter. It wins on startup, memory, and predictability, not
  on multi-minute numeric hot loops where a tracing JIT eventually pulls ahead. Where raw compute
  matters, DynaJS gives you **native SIMD kernels** (`dyna:simd`, `dyna:ml`) instead of hoping a
  JIT vectorizes your loop.
- **No package manager, no `node_modules`.** Dependencies you don't control are a cost. DynaJS's
  answer to "I need X" is "X should be a curated native module," not "add a dependency."
- **No re-skinning of JavaScript built-ins.** DynaJS does not ship a `strconv` module to duplicate
  `parseInt`, or a `unicode` module to duplicate `/\p{L}/u`, or an `os.readFile` that duplicates the
  file module. A native module has to earn its place by filling a *real* gap or delivering *real*
  performance. This curation bar is enforced (Chapter 9).

## 1.5 Where DynaJS shines (and where it doesn't) — the honest version

This book will not oversell. Here is the straight assessment.

**DynaJS is an excellent fit when you want:**

- **Instant startup and low memory.** CLI tools, serverless/edge functions with cold-start budgets,
  embedded scripting, short-lived workers. A QuickJS-lineage process is ready before V8 has finished
  parsing its snapshot.
- **Predictable, flat memory.** Long-running services that must not accumulate RSS. Reference
  counting frees promptly; native modules use an explicit, arena-free disposal model with `close()`
  / `[Symbol.dispose]()` so their memory returns *deterministically*, not "eventually, when the GC
  feels like it."
- **A dependency-free standard library.** No supply chain, no `npm audit`, no lockfile drift. What
  ships in the binary is what runs.
- **Native vector math from JavaScript.** `dyna:simd` exposes a multi-ISA (scalar / NEON / SSE4.2
  / AVX2 / AVX-512 / SVE) kernel set — dot products, norms, distances, activations, GEMM, f32 and
  f64 — with no native-addon build step.
- **Data-plane utilities at C speed.** Hashing, compression, substring search, byte manipulation,
  base64/hex/base32 — all native, several SIMD-accelerated.

**Reach for a different tool when you need:**

- **The npm ecosystem.** If your project's value is "it glues together 400 npm packages," DynaJS is
  the wrong runtime — by design.
- **Peak long-running numeric throughput in plain JS.** A tracing JIT (V8/JSC) will out-run an
  interpreter on a hot arithmetic loop that runs for minutes. DynaJS's counter is native kernels;
  if your workload can't use them, weigh the trade-off.
- **A large, mature third-party module catalogue *today*.** DynaJS's standard library is growing
  fast but is young. Some modules are recent; some SIMD paths (e.g. certain AVX-512 kernels) are
  gated pending hardware verification. The book flags these where relevant.

## 1.6 The positioning, in one table

| Dimension | **DynaJS** | **Node.js 26** | **Bun** | **Go** |
|---|---|---|---|---|
| Engine / language | QuickJS fork (C11 interpreter) | V8 (JIT) | JavaScriptCore (JIT) | native compiler |
| Startup | microseconds | tens of ms | milliseconds | native (instant) |
| Idle memory | a few MB | tens of MB | tens of MB | small |
| Concurrency model | single-thread reactor + workers | libuv event loop + workers | event loop + workers | goroutines |
| Standard library | native `dyna:*`, curated, SIMD | large, C++ + npm | large, npm-compatible | large, first-party |
| Third-party ecosystem | **none by design** (stdlib grows instead) | npm (huge) | npm (huge) | Go modules |
| Native SIMD from the language | **yes, built in** | via N-API addons | via addons/FFI | via assembly/intrinsics |
| Node compatibility | **no, deliberate** | n/a | high | n/a |
| Best at | startup, footprint, embedding, curated speed | ecosystem, long-run JIT throughput | ecosystem + speed | systems concurrency |

The rest of this book substantiates each row with runnable examples.

## 1.7 A first taste

Here is DynaJS doing a few things that would each be an npm install (or a native addon) elsewhere —
all from the standard library, no dependencies:

```js
import { sha256Hex } from "dyna:crypto";
import { v7 } from "dyna:uuid";
import { dot } from "dyna:simd";
import { parseAddr, contains } from "dyna:netip";

// A content hash — native, streaming-capable, standard test-vector verified.
print(sha256Hex("hello world"));
//   b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9

// A time-ordered UUID (RFC 9562 v7) — good as a database key.
print(v7());
//   019f8eab-c823-7ba5-94c2-6b8808a4cc7e   (sorts by creation time)

// A SIMD dot product over Float32Array — one native call, vectorized.
print(dot(new Float32Array([1,2,3,4]), new Float32Array([5,6,7,8])));
//   70

// IP/CIDR reasoning that JavaScript simply has no built-in for.
print(contains("10.0.0.0/8", "10.1.2.3"));   // true
print(parseAddr("::ffff:127.0.0.1").is4);     // true (IPv4-mapped IPv6)
```

Save that as `taste.js` and run `dynajs taste.js`. No install step, no `package.json`, no build.
The next chapter gets you to exactly that point.

---

*Next: [Chapter 2 — Installation & First Steps](02-installation-and-first-steps.md).*
