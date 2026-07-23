<h1 align="center">DynaJS</h1>

<p align="center">
  <b>A small, fast, batteries-included JavaScript runtime.</b><br>
  A QuickJS fork with a native, SIMD-accelerated standard library — no npm, no <code>node_modules</code>, no supply chain.
</p>

---

DynaJS starts in **microseconds**, idles in a **few megabytes**, and ships a curated set of native
modules — crypto, HTTP, files, compression, vector math, data structures, and more — compiled into
the binary and reachable with one `import`. It began as a fork of Fabrice Bellard's
[QuickJS](https://bellard.org/quickjs/) (release `2026-06-04`) and grew the standard library a real
runtime needs, without adopting a package ecosystem.

> **A deliberate stance.** DynaJS **does not and will not implement Node.js compatibility** — no
> `require`, no `node:` modules, no npm. Instead, the functionality of the most-used packages is
> being brought in as **first-party native modules** under the `dynajs:` namespace: audited,
> curated, SIMD-accelerated, dependency-free, and re-arranged for DynaJS (not one-to-one clones).

## Install

```sh
# Clone, build with the full native standard library, and install (macOS / Linux / FreeBSD).
# Re-run any time to upgrade or repair — it overrides the previous install.
./install.sh                          # → /usr/local/bin/dynajs (or ~/.local/bin without sudo)
./install.sh --prefix "$HOME/.local"
./install.sh --uninstall

# ...or bootstrap without a checkout:
curl -fsSL <raw-url>/install.sh | bash
```

Requires `git`, `make`, and a C compiler (`clang` preferred). Pass `--with-deps` to let the
installer fetch them via your package manager.

## Hello, DynaJS

Parse CSV natively, train a model on it, and serve a prediction over HTTP — three native modules
working together, no dependencies:

```js
import { parseCsv } from "dynajs:docparse";
import { LinearRegression } from "dynajs:ml";
import { HttpServerAsync, HttpClient } from "dynajs:http";

// 1. Native CSV parsing (RFC 4180).
const rows = parseCsv("x,y\n1,3\n2,5\n3,7\n4,9");     // [["x","y"],["1","3"], ...]
const X = rows.slice(1).map(r => [Number(r[0])]);
const y = rows.slice(1).map(r => Number(r[1]));

// 2. Fit a model natively (SIMD-backed) — recovers y = 2x + 1.
const model = new LinearRegression();
model.fit(X, y);
const yhat = Math.round(model.predict([[10]])[0]);   // 21

// 3. Serve it from a single-thread reactor server; call it with the built-in client.
const server = new HttpServerAsync({ port: 0, routes: {
  "/predict": { status: 200, contentType: "application/json", body: JSON.stringify({ y: yhat }) },
}});
server.start();
const res = new HttpClient().get(`http://127.0.0.1:${server.port}/predict`);
print("GET /predict →", res.status, res.body);       // GET /predict → 200 {"y":21}
server.stop();
model.close();
```

```sh
dynajs hello.js          # run a file        dynajs -e 'EXPR'   # eval        dynajs -i   # REPL
```

No `package.json`, no install step, no build — the capabilities are in the binary.

## The standard library (`dynajs:*`)

Build with `CONFIG_NATIVE_MODULES=y` (the installer does this) to get a **native standard library** —
highlights:

| | | |
|---|---|---|
| **Data & text** | `strings` `bytes` `encoding` `text` | Go-style strings, byte LE/BE, hex/base64/base32/base85/varint, SIMD UTF‑8↔UTF‑16 |
| **Crypto & IDs** | `crypto` `random` | SHA/MD5/HMAC/CRC, seedable PRNG |
| **Numbers** | `mathx` `bits` | gamma/erf/gcd/lcm/factorial/isPrime, Go `math/bits` |
| **Collections** | `container` `structures` | heap / list / ring, vector / hashmap |
| **Filesystem** | `sys` `path` `file` `uring` | fs metadata + glob + process, path logic, buffered I/O, io_uring |
| **Networking** | `http` | reactor server + client |
| **Time & versions** | `time` `semver` | durations/monotonic/RFC3339, SemVer + ranges |
| **Compute** | `simd` `ml` | multi-ISA f32/f64/i32 vector math, regression/kmeans |
| **Formats** | `compress` `docparse` `sort` `search` | gzip, JSON/CSV, sort + SIMD substring search |

Every example in the docs is real and runs. See the **[API Reference](docs/dynajs-guide/API.md)** for
complete signatures.

## Why DynaJS

- **Instant startup, low memory** — an interpreter with no JIT warmup and a tiny baseline. Ideal for
  CLI tools, edge/serverless cold starts, embedding, and short-lived workers.
- **Predictable, flat memory** — reference-counting GC frees promptly; native modules dispose
  deterministically via `close()` / `[Symbol.dispose]()`, so RSS stays flat under load.
- **Native SIMD from JavaScript** — a verified, cross-ISA (NEON/SSE4.2/AVX2/AVX‑512/SVE) kernel set,
  no native-addon build step. Dual-use: the same kernels accelerate the engine internally.
- **A dependency-free standard library** — what ships in the binary is what runs. No `npm audit`,
  no lockfile drift, no transitive dependency trees.

Honest boundary: for a numeric loop that runs for minutes, a tracing JIT (V8/JSC) out-throughputs an
interpreter — DynaJS's answer is native SIMD kernels, not out-JIT'ing V8. Full trade-offs are in the
guide.

## Documentation

The complete book lives in **[`docs/dynajs-guide/`](docs/dynajs-guide/README.md)**:

1. [Introduction & Philosophy](docs/dynajs-guide/01-introduction-and-philosophy.md)
2. [Installation & First Steps](docs/dynajs-guide/02-installation-and-first-steps.md)
3. [The Language & the Runtime](docs/dynajs-guide/03-language-and-runtime.md)
4. [The Standard Library, Module by Module](docs/dynajs-guide/04-standard-library.md)
5. [Performance Architecture](docs/dynajs-guide/05-performance-architecture.md)
6. [For AI Agents](docs/dynajs-guide/06-for-ai-agents.md)
7. [Roadmap & Philosophy](docs/dynajs-guide/07-roadmap-and-philosophy.md)
- [**Complete API Reference**](docs/dynajs-guide/API.md) — every module, every signature.

## Build from source

```sh
make CONFIG_NATIVE_MODULES=y -j"$(getconf _NPROCESSORS_ONLN)"   # engine + native stdlib
make test                                                       # language + module test suites
./dynajs -e 'print("ok")'
```

`make` alone builds the core engine plus the classic `std`/`os` modules. Sanitizer builds:
`make CONFIG_ASAN=y`, `make CONFIG_UBSAN=y`. See [Chapter 2](docs/dynajs-guide/02-installation-and-first-steps.md).

## Status

The QuickJS-lineage engine and the core language are mature (the project holds a fixed ECMAScript
`test262` conformance baseline). The standard library is solid and shipping — each module verified
against standard vectors or reference implementations and sanitizer-clean — and *growing*: the
"top npm functionality as first-party modules" plan is early, and some x86 SIMD paths (certain
AVX‑512 kernels) are conservatively gated pending hardware verification. The docs flag landed
vs. planned explicitly.

## License

MIT. DynaJS is derived from QuickJS, © 2017‑2021 Fabrice Bellard and Charlie Gordon; see the license
headers. DynaJS additions are under the same terms.
