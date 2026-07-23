# Chapter 7 — Roadmap & Philosophy

## 7.1 Node.js compatibility will not be implemented

State it plainly: **DynaJS will not implement Node.js compatibility. Ever. It is not on the
roadmap, and it is not a gap awaiting resources — it is a rejected design.**

There will be no `require`, no CommonJS resolver, no `node:` built-in modules, no `process`/`Buffer`/
`__dirname` globals, no attempt to run npm packages as-is, no compatibility shim layer. If you need
to run the npm ecosystem, DynaJS is the wrong runtime and that is by design.

The reasoning is the whole thesis of this book (Chapter 1). Node compatibility and DynaJS's goals
pull in opposite directions:

- Node's model is "a thin runtime plus a vast third-party ecosystem." That model brings transitive
  dependency trees, supply-chain risk, `node_modules` bloat, and semver churn as *intrinsic*
  properties, not accidents.
- DynaJS's model is "a small correct engine plus a large, curated, native, first-party standard
  library." A compatibility layer would drag the entire Node object model — and, transitively, the
  ecosystem's assumptions — back into a runtime whose entire value proposition is *not* having them.

So the answer to "how do I do X that an npm package does?" is never "we'll add Node compat." It is
"X should become a first-party DynaJS module." Which leads to the actual plan.

## 7.2 The plan: the top ~50 npm packages, as first-party native modules

Instead of running npm, DynaJS will **absorb the *functionality* of the most-used packages into its
own standard library** — rewritten in the DynaJS style, native where it pays, dependency-free,
audited, and shipped in the binary.

Concretely, the roadmap is to work down the list of the **~50 most-depended-upon distinct npm
packages** and, for each one that represents a genuine capability, provide an equivalent `dynajs:*`
module (or fold it into an existing one). And deliberately **not a clone**: the package names and
their arrangement are re-thought for DynaJS — functionality is regrouped by domain under DynaJS-native
names, not copied one-package-to-one-module with the upstream name. It is an *implementation of what
the package does*, done properly and re-arranged:

- A date/time library becomes coverage in `dynajs:time`.
- A UUID library becomes `dynajs:uuid` (already shipped: v4/v7/v3/v5).
- A hashing/crypto library becomes `dynajs:crypto` (already shipped).
- A compression library becomes `dynajs:compress` (already shipped: real DEFLATE).
- A CSV/JSON parser becomes `dynajs:docparse` (already shipped).
- An HTTP client/server becomes `dynajs:http` (already shipped: reactor server + client).
- The filesystem/glob packages (`fs-extra`, `glob`, `rimraf`, `mkdirp`) become `dynajs:sys` (already
  shipped: unified filesystem + process + glob, re-arranged into one module).
- The `semver` package becomes `dynajs:semver` (already shipped: SemVer 2.0.0 + npm range grammar).
- A validation, templating, or data-structure package becomes a native module or a documented
  built-in recipe.

The end state is a runtime where the things people install a dependency for are simply *there*,
under `dynajs:`, with no install step, no lockfile, no `node_modules`, and — because they are native
and SIMD-aware — often faster than the JavaScript package they replace.

This is a large, multi-release effort. It is the central roadmap item, and it is why the standard
library (Chapter 4) grows every release.

## 7.3 The curation bar — why not *everything* gets a module

The standard library is curated, not comprehensive-for-its-own-sake. A candidate module must clear a
bar, and "the npm package is popular" is not sufficient by itself. The bar is:

> **A native module must fill a capability JavaScript genuinely lacks, OR deliver real performance on
> a path that matters. It must not merely re-skin an existing JavaScript built-in or an existing
> DynaJS module.**

This bar is enforced, and it has teeth. Examples of what it *rejects*:

- A `strconv` module would duplicate `parseInt` / `parseFloat` / `Number.prototype.toString(base)` /
  `JSON.stringify`. **Rejected** — the language already does this.
- A `unicode` classification module would duplicate the regex `\p{L}` / `\p{N}` / `\p{White_Space}`
  property escapes and `String.prototype.toUpperCase`/`toLowerCase`. **Rejected.**
- An in-memory line/word `bufio.Scanner` would duplicate `String.prototype.split(/\r?\n/)`.
  **Rejected** as a standalone module (the streaming case, where it *would* add value, belongs inside
  `dynajs:file`).
- Generic `slices`/`maps` utility packs would duplicate `Array.prototype` and `Map`. **Rejected.**

And what it *accepts* — the shipped library — is exactly the set that passes: path logic, IP/CIDR,
UUIDs, hashes, special-function math, fixed-width bit ops, a priority-queue heap, byte LE/BE
manipulation, compression, buffered/async I/O, and native SIMD. Each is a real gap or a real speedup,
not a re-skin.

The discipline behind this: every candidate is **guilty until a benchmark or a genuine-gap argument
proves it innocent.** The runtime stays lean because the answer to "should this be a module?" is
usually a well-reasoned *no*.

## 7.4 The performance roadmap: growing the SIMD engine

The SIMD engine (Chapter 5 §5.1) is the runtime's performance flywheel, and it is deliberately
dual-use: every kernel accelerates both user code and the engine's own internals. The expansion
roadmap moves through categories:

- **Arrays** — integer (i8/i16/i32/i64) and f64 variants of the f32 math (f64 **shipped**; i32
  reductions/elementwise + `cumsum`/`cummax` prefix-scan **shipped**), plus vectorized sort,
  filter/compress, gather/scatter, and running statistics.
- **Bytes/strings** — UTF-8↔UTF-16↔Latin-1 transcoding at GiB/s (UTF-8↔UTF-16 + validate/count
  **shipped** in `dynajs:text`; Latin-1↔UTF-8 already shipped), multi-pattern search, and CRC via
  carry-less multiply (`simdutf`-class throughput).
- **Maps** — SwissTable-style group-probe hashing, to speed the engine's own atom/shape lookups as
  well as `dynajs:structures`.
- **Matrix** — transpose, convolution, int8 matmul (VNNI/dot-product instructions), and attention.

Verification is part of the roadmap, not an afterthought: every per-ISA kernel is differentially
tested against a scalar reference under emulation, and a path that cannot be exercised on available
hardware is gated off in favor of a verified fallback. DynaJS ships proven kernels or none.

## 7.5 The optimizer roadmap: implicit-first, always

The `meta@` optimizer program (Chapter 5 §5.5) has a standing direction: **as engine capabilities
land, optimizations migrate from explicit hints to automatic.** Inline caches, escape analysis, SIMD
codegen, and eventually perhaps a JIT each unlock a group of optimizations the compiler can then
apply with no annotation. The directive surface is meant to *shrink* over time. The one hard gate on
any such change never moves: it must preserve the `test262` conformance baseline, pass the
sanitizers, and be justified by a real measurement.

## 7.6 An honest picture of where DynaJS is today

This book does not oversell, so the closing note is candid:

- **Mature:** the QuickJS-lineage engine (correct, `test262`-tracking, fast-starting) and the core
  language.
- **Solid and shipping:** the standard-library modules documented in Chapter 4 — each verified
  against standard test vectors or reference implementations, sanitizer-clean, with test suites in
  the tree.
- **Young and growing:** the *breadth* of the standard library. The top-50-npm plan is early; new
  modules land regularly. Some SIMD paths (certain AVX-512 kernels) are conservatively gated pending
  hardware verification. The `using` declaration syntax is pending (the library is done).

DynaJS is a bet that a small, correct engine wrapped in a curated, native, performance-first standard
library is a better foundation than a thin runtime plus an unbounded dependency ecosystem — for the
workloads where startup, memory, predictability, and native speed matter more than ecosystem
breadth. If those are your constraints, the runtime in this book is built for you, and it is getting
more capable every release.

---

*End of the DynaJS Guide. Back to the [table of contents](README.md).*
