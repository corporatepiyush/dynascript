# The DynaJS Guide

*A complete book on the DynaJS runtime — for beginners, for experienced engineers, and for AI coding agents.*

DynaJS is a small, fast, batteries-included JavaScript runtime. It began as a fork of
Fabrice Bellard's **QuickJS** (release `2026-06-04`) and grew into a runtime with a native,
SIMD-accelerated standard library under the `dynajs:` namespace. It is written in C11 as a
unity build, starts in microseconds, uses a few megabytes of RAM, and ships a curated set of
native modules that fill capabilities JavaScript genuinely lacks — instead of chasing
compatibility with an existing ecosystem.

> **A deliberate stance, stated up front.**
> DynaJS **will not implement Node.js compatibility.** There is no `require`, no `node:` built-ins,
> no attempt to run npm packages as-is. That is a design decision, not a gap we are waiting to
> fill. Instead, the plan is to bring the *functionality* of the **top ~50 distinct npm packages**
> into DynaJS as **first-party native modules** — audited, curated, SIMD-accelerated, dependency-free,
> re-named and re-arranged for DynaJS (not one-to-one clones), and shipped inside the runtime's own
> standard library. See
> [Chapter 7: Roadmap & Philosophy](07-roadmap-and-philosophy.md).

---

## How to read this book

- **New to runtimes / coming from Node or Python?** Read Chapters 1 → 2 → 3 → 4 in order. You will
  have a working `dynajs` binary and be writing real programs within the first two chapters.
- **Experienced engineer evaluating DynaJS?** Skim Chapter 1 for positioning, then jump to
  Chapter 5 (Performance Architecture) and the *How DynaJS compares* notes below for the honest
  trade-offs, and Chapter 4 for the standard-library surface.
- **AI coding agent?** Chapter 6 is written for you: import rules, the exact module surface, the
  invariants you must respect, and how to verify your own output. Chapters 2 and 4 are your API
  reference.

Every code example in this book is real and runs against the actual modules — the APIs were
extracted from the source and the test suites, not invented.

---

## Table of contents

1. [Introduction & Philosophy](01-introduction-and-philosophy.md) — what DynaJS is, where it came
   from (the QuickJS lineage), and the positioning against Node 26, Bun, and Go.
2. [Installation & First Steps](02-installation-and-first-steps.md) — building the binary, the CLI,
   the REPL, your first script, ES modules, and the `dynajs:` namespace.
3. [The Language & the Runtime](03-language-and-runtime.md) — the ECMAScript baseline (ES2023–2026),
   deterministic resource disposal, `std`/`os`, workers, and BigInt.
4. [The Standard Library, Module by Module](04-standard-library.md) — every `dynajs:*` module with
   worked examples: `path`, `strings`, `bytes`, `crypto`, `encoding`, `time`, `container`, `mathx`,
   `uuid`, `bits`, `netip`, `simd`, `text`, `file`, `http`, `sort`, `search`, `random`, `compress`,
   `ml`, `docparse`, `structures`, `uring`.
5. [Performance Architecture](05-performance-architecture.md) — the multi-ISA SIMD engine, the
   deterministic memory model, the token-threaded interpreter, the async I/O reactor, and the
   `meta@` optimizer program.
6. [For AI Agents](06-for-ai-agents.md) — a precise, machine-oriented reference and working rules.
7. [Roadmap & Philosophy](07-roadmap-and-philosophy.md) — no Node compatibility, the top-50-npm
   standard-library plan, and the curation bar every module must clear.

---

## Install in one command

DynaJS ships an installer that clones, builds from source with the full native standard library,
and installs the `dynajs` binary — overwriting any previous installation. Running it again is how
you **upgrade** or **repair** an install.

```sh
# From a checkout:
./install.sh                       # installs to /usr/local (or ~/.local without sudo)
./install.sh --prefix "$HOME/.local"
./install.sh --uninstall           # remove it

# Bootstrapped (no checkout needed):
curl -fsSL <raw-url>/install.sh | bash
```

Supported OS: macOS, Linux, FreeBSD. It needs `git`, `make`, and a C compiler (`clang` preferred);
pass `--with-deps` to let it install those via your package manager. See
[Chapter 2](02-installation-and-first-steps.md) for building by hand.

---

## How DynaJS compares (in brief)

A full runtime-vs-runtime chapter would age badly; here is the durable summary. DynaJS is not
trying to beat V8 on a long numeric hot loop — a tracing JIT wins there, and DynaJS answers with
native SIMD kernels instead. Where DynaJS *does* win is **startup** (microseconds, no JIT warmup),
**memory** (a few MB idle, flat RSS via reference counting + deterministic native disposal),
**I/O concurrency** (a single-thread kqueue/epoll/io_uring reactor — ~172× a thread-pool server at
500 connections in the project's own benchmark), and **batteries** (a curated, dependency-free,
SIMD-accelerated standard library — no `node_modules`, no supply chain).

| | **DynaJS** | **Node 26** | **Bun** | **Go** |
|---|---|---|---|---|
| Engine | QuickJS fork (interpreter) | V8 (JIT) | JSC (JIT) | native compiler |
| Startup / idle RAM | µs / few MB | tens of ms / tens of MB | ms / tens of MB | instant / small |
| Stdlib model | native `dynajs:*`, curated | C++ + npm | npm-compatible | first-party |
| Third-party ecosystem | **none, by design** | npm (huge) | npm (huge) | Go modules |
| SIMD from the language | **built in** | via addons | via addons/FFI | via intrinsics |
| Node compatibility | **no, deliberate** | — | high | — |

Pick DynaJS for CLI tools, edge/serverless with cold-start budgets, embedding, memory-bound
services, and native vector math from JavaScript. Pick Node/Bun when your value is the npm
ecosystem; pick Go for systems concurrency.

## Lineage: from QuickJS to DynaJS (in brief)

DynaJS is a fork of Fabrice Bellard's **QuickJS** (`2026-06-04`). QuickJS contributed the DNA that
defines DynaJS: a tiny, correct, `test262`-tracking engine with a bytecode compiler, a small VM,
reference-counting GC with cycle collection, and near-instant startup — no JIT, no snapshot. The
2026-vintage engine DynaJS builds on already has **rope strings**, a **slab allocator** with the
refcount in the malloc-block header, **short-BigInt**, and a **token-threaded (computed-goto)
interpreter**. What DynaJS *adds* is the thing that turns an embeddable engine into a runtime: the
native `dynajs:*` standard library (SIMD, crypto, http, file/io_uring, compression, data
structures, math, networking, …), the async I/O reactor, the deterministic native-resource memory
model, and a standing optimizer program (`meta@`). The engine stayed small and correct; the runtime
grew around it. Chapter 5 covers the machinery; Chapter 7 covers where it is going.

---

*This guide is versioned with the engine. The runtime identifies itself as
`DynaJS version 2026-06-04`. Where a feature is landed vs. planned, the text says so explicitly —
DynaJS is an evolving project and this book does not oversell it.*
