# Chapter 5 — Performance Architecture

Let's talk speed — honestly. DynaJS's performance story is *not* "our interpreter beats V8's JIT on a
hot loop." It does not, and this book will not pretend otherwise. The story is architectural: **fast startup, flat memory,
native SIMD where compute matters, and a reactor I/O model that scales connections cheaply.** This
chapter explains each, with the numbers that were actually measured in the project.

## 5.1 The multi-ISA SIMD engine

At the core is a shared SIMD kernel table. Each kernel (dot product, sum, min/max, distance,
activation, GEMM, substring search, byte transcode) is implemented once per instruction set —
**scalar, ARM NEON, x86 SSE4.2, AVX2, AVX-512, ARM SVE** — and the right implementation is bound at
startup based on the CPU's actual capabilities (`cpu_features()` + a one-time dispatch install).

Two properties make this valuable rather than merely present:

1. **It is exposed to JavaScript** (`dyna:simd`, `dyna:text`, `dyna:ml`). You get vectorized
   math with no native-addon build.
2. **It is dual-use.** The *same* kernels the language exposes are used *inside* the engine. The
   clearest example: the HTTP server's header scanning was moved from a scalar byte-at-a-time loop
   to the SIMD substring kernel (`simd.strfind`) and got **~4.75× faster** on that path (measured,
   and checked clean under both AddressSanitizer and ThreadSanitizer). On long inputs the substring
   kernel runs in the multiple-GiB/s range on NEON.

Why a JIT doesn't just do this for you: auto-vectorization requires simple counted loops, unit
stride, no early exit, and no aliasing — conditions real JavaScript rarely satisfies, and which a
tracing JIT is conservative about. A hand-written kernel sidesteps all of it. That is the trade:
DynaJS gives you the kernel instead of hoping the compiler finds one.

**Verification discipline (why you can trust the kernels).** x86 kernels never run on the ARM
development host, so "the tests pass on my machine" proves nothing about AVX2. Every per-ISA kernel
is differentially tested against a scalar reference under emulation, plus a guard-page test that
catches out-of-bounds reads on short inputs. This discipline has caught real x86-only bugs
(reductions that reduced with the wrong horizontal operation; full-width loads before a length
check). Where a path cannot be exercised on available hardware/emulation — some AVX-512 kernels —
it is **gated off and falls back to the verified AVX2 path** rather than shipping unproven. DynaJS
ships the proven path.

## 5.2 The deterministic memory model

This is the property that most differentiates DynaJS from tracing-GC runtimes in production.

**The language heap** uses reference counting with a cycle collector. An object is freed the instant
its last reference drops; a periodic trial-deletion pass reclaims reference cycles. There is no
stop-the-world pause tied to a tracing collector's heuristics.

**Native modules go further.** Every `dyna:*` resource object owns its native memory directly and
frees it **deterministically** on `close()` / `[Symbol.dispose]()`:

- The constructor allocates the native struct.
- Methods copy results into fresh JS values at the boundary — **no native pointer escapes** into the
  JS heap.
- Disposal runs the module's teardown immediately. The class finalizer is only a *safety net* for a
  leaked object, never the primary path.

The observable result: **flat peak RSS**. In the project's leak methodology (run a create/use/close
workload at N = 20k, 100k, 500k and watch peak RSS), a correctly-disposed module holds a flat
plateau — memory returns as you close resources, it does not accumulate waiting for a GC. That is
why DynaJS suits long-running services with strict memory budgets.

There is also a safety dimension. Because coercing a JavaScript argument can run arbitrary user code
(a `valueOf`, a Proxy trap) that might `close()` the very resource you are about to use, every
module method follows a strict rule: **coerce all arguments to C values first, resolve the native
handle last.** This closes an entire class of use-after-free bugs by construction, and each method
is tested against a hostile `{ valueOf() { obj.close(); return 1 } }` argument.

## 5.3 The interpreter

DynaJS inherits QuickJS's **token-threaded (computed-goto) interpreter**: bytecode dispatch jumps
through a label table rather than a `switch`, which the CPU's branch predictor handles far better.
Combined with:

- **rope strings** — concatenation builds a tree node instead of copying, so building large strings
  by `+=` is not quadratic;
- a **slab allocator** with the refcount stored in the malloc-block header — small-object allocation
  is cheap and cache-friendly;
- **inline caches / shapes** — objects with the same "shape" (property layout) share a hidden class,
  and property access is cached against it, so repeated `obj.field` is fast;
- **short-BigInt** — small big integers avoid heap allocation;

…you get an interpreter that is genuinely quick to *start* and steady in *throughput*, without a JIT
warmup curve. For a CLI tool, an edge function, or a script that runs for a few hundred milliseconds,
this is often faster end-to-end than a JIT runtime that spends the first chunk of its life compiling.

The honest boundary: for a numeric kernel that runs for minutes, a tracing JIT will eventually
out-throughput the interpreter. DynaJS's answer there is §5.1 — move the kernel to native SIMD — not
to out-JIT V8.

## 5.4 The async I/O reactor

DynaJS's networking is built on **one single-threaded event-loop reactor** over the platform's
readiness API — **kqueue** on macOS, **epoll** or **io_uring** on Linux. One thread multiplexes
thousands of connections via non-blocking sockets and a readiness queue, and the **`App`** server
(Chapter 4 §4.6) runs your JavaScript route handlers directly on that loop — no per-request thread,
no cross-thread copy of the request.

Why a reactor beats a thread-per-connection design is the classic C10k story:

- **Thread-per-connection / thread pool**: each connection costs a thread's stack and scheduler
  attention; at high concurrency, context-switching and memory dominate.
- **Reactor**: one thread, non-blocking sockets, O(ready) work per loop turn. Per-connection
  overhead is a file descriptor and a small state struct, not a thread — so the advantage *grows*
  with concurrency.

To size that gap, the project benchmarked the reactor against a thread-pool server on **fixed static
responses** (the reactor serving without entering the JS world): at 500 concurrent connections the
reactor did **~172× the throughput** of the thread pool. Read that number for what it is — a
demonstration of the reactor *model's* concurrency headroom, not a benchmark of your JavaScript
handlers. The `App` server inherits that same reactor; end-to-end throughput of an `App` also depends
on what your handlers do (JSON-RPC parse, call, serialize).

The I/O layer also uses the cheap, correct OS levers where they pay: `TCP_NODELAY` is on for latency;
file I/O uses `F_RDAHEAD`/`F_PREALLOCATE`/`F_FULLFSYNC` on macOS and `fadvise`/`fallocate`/io_uring on
Linux (`dyna:file`, `dyna:uring`).

> A concurrency caveat the project is explicit about: the reactor is single-threaded, so an `App`
> handler must not block it. CPU-heavy per-request work belongs in a worker (Chapter 3 §3.4), not
> inline in the reactor thread.

## 5.5 The optimizer program (`meta@`) — making plain code fast automatically

DynaJS treats the optimizer as a standing program, not a fixed feature set. The guiding principle:
**the engine should make ordinary code fast automatically**, and only ask the programmer for a hint
when the precondition is genuinely impossible for the compiler to prove.

- **Implicit (the default).** If a property is provable by bounded static analysis inside the
  compile budget, the engine applies the optimization with no annotation — constant folding, dead-
  code elimination, fused compare-and-branch, and a growing set of codegen passes. You write plain
  code; you get the speed.
- **Explicit `meta@` directives.** For the rare case where a precondition is a runtime promise the
  compiler cannot verify (or would be too expensive to), DynaJS has a directive front-end
  (`meta@...`). Crucially, this front-end is **codegen-neutral**: code with no directives compiles
  byte-for-byte identically to before the feature existed (proven by a pristine-vs-patched corpus
  diff), so it cannot regress conformance or performance by its mere presence.

The important design stance: **as new engine capabilities land (inline caches, SIMD kernels, escape
analysis, and eventually perhaps a JIT), optimizations move from "explicit hint" to "automatic."**
The directive surface shrinks as the engine gets smarter. This is a feedback loop, not a frozen API.

## 5.6 Putting the performance picture together

| You care about | DynaJS mechanism | The honest read |
|---|---|---|
| Startup latency | interpreter, no JIT warmup, tiny baseline | **wins** vs V8/JSC |
| Idle / peak memory | refcount GC + deterministic native disposal | **wins**, flat RSS |
| Vector/number crunch | native multi-ISA SIMD kernels | **wins** vs scalar JS; competitive with addons |
| Connection concurrency | single-thread reactor (kqueue/epoll/io_uring), `App` handlers on the loop | **wins** vs thread-per-conn (~172× @500, static microbench) |
| Long hot scalar loop | token-threaded interpreter | **loses** to a tracing JIT — use SIMD instead |
| Bulk disk read (Linux) | io_uring high-QD reads | **wins** vs blocking read loop |

The theme: DynaJS is fast where architecture beats raw compilation — startup, memory, I/O
concurrency, and native kernels — and it is candid about the one place a JIT wins, offering native
SIMD as the counter.

---

*Next: [Chapter 6 — For AI Agents](06-for-ai-agents.md). (A runtime-vs-runtime comparison lives in
the [README](README.md#how-dynajs-compares-in-brief); the QuickJS lineage is summarized there too.)*
