# Chapter 3 — The Language & the Runtime

DynaJS runs modern JavaScript. This chapter pins down exactly *which* JavaScript, then covers the
runtime-level facilities that are not modules: deterministic resource disposal, the `std`/`os`
system modules, workers, and BigInt.

## 3.1 The ECMAScript baseline

DynaJS inherits QuickJS's close tracking of the ECMAScript standard and holds a fixed `test262`
conformance baseline that engine changes must not regress. In practice you get the full modern
language, on the ES2023–ES2026 track. A non-exhaustive tour of what is present and verified:

```js
// Arrays: non-mutating copies + reverse search (ES2023)
print([3, 1, 2].toSorted().join(","));        // 1,2,3   (original untouched)
print([1, 2, 3, 4].toReversed().join(","));   // 4,3,2,1
print([1, 2, 3].with(1, 99).join(","));       // 1,99,3
print([5, 1, 8, 2].findLast(x => x < 5));      // 2
print([5, 1, 8, 2].findLastIndex(x => x < 5)); // 3

// Grouping (ES2024)
const grouped = Object.groupBy([1, 2, 3, 4, 5], n => (n % 2 ? "odd" : "even"));
print(JSON.stringify(grouped));               // {"odd":[1,3,5],"even":[2,4]}

// Everyday modern syntax
const user = { name: "Ada", address: { city: "London" } };
print(user?.address?.city ?? "unknown");      // London
const { name, ...rest } = user;               // destructuring + rest
```

Also present: `async`/`await` and **top-level `await`** in modules, generators and async
generators, classes with private fields (`#x`) and private methods, static blocks, `Map`/`Set`/
`WeakMap`/`WeakSet`/`WeakRef`/`FinalizationRegistry`, all typed arrays and `DataView`,
`Array.prototype.flat`/`flatMap`, `String.prototype.replaceAll`/`at`, `Object.hasOwn`,
`Promise.any`/`allSettled`, logical assignment (`??=`, `||=`, `&&=`), numeric separators, `BigInt`,
`Symbol` with well-known symbols, `RegExp` named groups and the `d`/`v` flags, and the
error-handling additions `Error.cause` and `AggregateError`.

A couple of honest specifics so you are not surprised:

- **`structuredClone` is not provided** as a global. Use explicit copying, JSON round-trips for
  plain data, or a module. (This is the kind of gap the standard-library roadmap targets.)
- The **`using` declaration *syntax*** (`using f = open()`) is **not yet implemented** — but the
  entire **disposable *library*** it builds on *is* (next section). This is a deliberate ordering:
  the semantics landed first; the syntax needs dedicated VM opcodes and is on the roadmap.

## 3.2 Deterministic resource disposal

This is one of DynaJS's most important runtime ideas, and it connects directly to how every native
module manages memory (Chapter 5).

The **disposable protocol** from TC39 is fully present as a library:

- `Symbol.dispose` and `Symbol.asyncDispose` — well-known symbols an object implements to say "here
  is how to release me."
- `DisposableStack` and `AsyncDisposableStack` — containers that collect disposables and release
  them in reverse order.
- `SuppressedError` — the error type that correctly folds a disposer failure together with an
  in-flight error (so you never silently lose one).

Every native `dyna:*` resource object (a `FileWriter`, an `HttpServerAsync`, a `Hasher`, a
`Heap`) implements `[Symbol.dispose]()` and a matching `close()`. You release them explicitly:

```js
import { FileWriter } from "dyna:file";

const w = new FileWriter("/tmp/out.txt", { bufferSize: 4096 });
try {
  w.write("some data\n");
  w.sync();          // durable flush
} finally {
  w.close();         // deterministic release — memory returns *now*
}
```

For several resources, `DisposableStack` removes the nested-`try/finally` pyramid and disposes
everything in reverse on scope exit:

```js
import { FileReader, FileWriter } from "dyna:file";

function copyFiltered(src, dst) {
  const stack = new DisposableStack();
  const r = stack.use(new FileReader(src, { bufferSize: 1 << 16 }));
  const w = stack.use(new FileWriter(dst, { bufferSize: 1 << 16 }));

  let line;
  while ((line = r.readLine()) !== null) {
    if (!line.startsWith("#")) w.write(line + "\n");
  }
  w.sync();
  stack.dispose();     // disposes w, then r — reverse order, always runs
}
```

`stack.use(x)` registers `x` (returning it); `stack.defer(fn)` registers an arbitrary cleanup
callback; `stack.adopt(value, fn)` registers a non-disposable value with a disposer;
`stack.move()` transfers ownership to a new stack (for handing a half-built resource set to a
caller). When the `using` syntax lands, `using r = stack.use(...)` will be sugar over exactly this
machinery — your code written against the library today keeps working.

> **Why this matters.** In a tracing-GC runtime, a file handle or socket is released "eventually,"
> whenever the collector runs a finalizer. Under DynaJS, `close()` frees the native resource
> **immediately and deterministically**. The class finalizer is only a safety net for a leaked
> object, not the primary path. This is why DynaJS services hold flat RSS under load.

## 3.3 The `std` and `os` system modules

These are the classic QuickJS low-level modules — the thin layer over libc and the OS. They are
available in module context (or with the `--std` flag for scripts).

```js
import * as std from "std";
import * as os from "os";

// Low-level file I/O (std): open, read/write, printf-style formatting.
const f = std.open("/tmp/note.txt", "w");
f.puts("written via std\n");
f.close();
print(std.loadFile("/tmp/note.txt"));   // "written via std\n"

// OS facilities (os): filesystem, time, process.
const [entries, err] = os.readdir(".");
print(err ? "error" : `${entries.length} entries`);
os.sleep(10);                            // milliseconds
const t0 = os.now();                     // monotonic-ish clock

// Force a GC cycle — deterministic, useful in tests and benchmarks.
std.gc();
```

For higher-level, ergonomic file and filesystem work, prefer the native modules — `dyna:file`
(buffered reader/writer, one-shot `readFile`/`writeFile`) covered in Chapter 4. `std`/`os` remain
the escape hatch for raw syscall-level control.

## 3.4 Parallelism: workers and shared memory

DynaJS is single-threaded per context (like Node's main thread), and scales across cores with
**workers**. Each worker is an isolated JavaScript context with its own heap; they communicate by
message passing, and can share raw memory through `SharedArrayBuffer` + `Atomics`.

```js
// main.js
import * as os from "os";

const worker = new os.Worker("./compute-worker.js");

worker.onmessage = (e) => {
  const msg = e.data;
  if (msg.type === "result") {
    print("worker computed:", msg.value);
    worker.onmessage = null;             // let the program exit
  }
};

// Share a buffer both sides can read/write via Atomics — zero copy.
const sab = new SharedArrayBuffer(1024);
worker.postMessage({ type: "start", buf: sab, n: 1_000_000 });
```

```js
// compute-worker.js
import * as os from "os";
const parent = os.Worker.parent;

parent.onmessage = (e) => {
  const { type, n } = e.data;
  if (type === "start") {
    let acc = 0;
    for (let i = 0; i < n; i++) acc += Math.sqrt(i);
    parent.postMessage({ type: "result", value: acc });
  }
};
```

The pattern is: `new os.Worker(path)` on the parent, `os.Worker.parent` inside the worker,
`postMessage`/`onmessage` on both sides. `SharedArrayBuffer` gives you true shared memory for
data-parallel work; `Atomics` gives you the synchronization primitives. Because each worker starts
from the tiny QuickJS baseline, spinning up a worker is cheap.

## 3.5 BigInt and numeric range

DynaJS has arbitrary-precision `BigInt` with an optimized **short-BigInt** representation for
small values (they avoid heap allocation). Several standard-library modules use `BigInt` precisely
*because* it is exact where `Number` is not:

```js
import { factorial, gcd } from "dyna:mathx";
import { Mul64 } from "dyna:bits";

print(factorial(25));                 // 15511210043330985984000000n  (exact, > 2^53)
print(gcd(462n, 1071n));              // 21n
const [hi, lo] = Mul64(0xffffffffffffffffn, 3n);   // full 128-bit product
print(hi, lo);                        // 2n 18446744073709551613n
```

Rule of thumb: use `Number` for measurements and ordinary arithmetic; reach for `BigInt` (and the
modules that return it) whenever a value can exceed 2^53 or must be bit-exact — 64-bit integer math,
large factorials/LCMs, exact IDs.

---

*Next: [Chapter 4 — The Standard Library, Module by Module](04-standard-library.md) — the heart of
the book.*
