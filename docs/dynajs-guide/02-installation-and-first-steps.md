# Chapter 2 — Installation & First Steps

## 2.1 Building from source

DynaJS is a C11 project with no external dependencies for the core. You need a C compiler
(**clang** is preferred; gcc works on Linux) and `make`. On macOS the build auto-selects clang; on
Linux it defaults to gcc, and you can force clang with `CONFIG_CLANG=y`.

```sh
git clone <your-dynascript-remote> dynascript
cd dynascript

# Core build (the engine + the `std`/`os` modules), parallel:
make -j"$(getconf _NPROCESSORS_ONLN)"

# You now have a ./dynajs binary:
./dynajs -e 'print("hello from DynaJS")'
#   hello from DynaJS
```

That default build gives you the language and the two classic QuickJS system modules (`std`, `os`).
To get the **native standard library** — everything under the `dynajs:` namespace that this book is
about — build with `CONFIG_NATIVE_MODULES=y`:

```sh
make CONFIG_NATIVE_MODULES=y -j"$(getconf _NPROCESSORS_ONLN)"

./dynajs -e 'import("dynajs:crypto").then(c => print(c.sha256Hex("hi")))'
#   8f434346648f6b96df89dda901c5176b10a6d83961dd3c1ac88b59b2dc327aa4
```

> **One flag, one rule.** A `-D` flag change (like toggling `CONFIG_NATIVE_MODULES`) does **not**
> re-trigger recompilation on its own — `make` tracks file timestamps, not flags. After changing a
> config flag, run `make clean` first, or you will link stale objects. Fresh clones are immune.

Useful build variants:

| Command | What you get |
|---|---|
| `make` | core engine + `std`/`os` |
| `make CONFIG_NATIVE_MODULES=y` | + the entire `dynajs:*` standard library |
| `make CONFIG_ASAN=y` | AddressSanitizer build (own objdir) |
| `make CONFIG_UBSAN=y` | UndefinedBehaviorSanitizer build |
| `make test` | run the full test suite (language + modules) |
| `make CONFIG_LTO=y` | link-time optimization (smaller/faster, slower build) |

Run `make test` once to confirm a healthy build; it exercises the language suite and every native
module test.

## 2.2 The command-line interface

The binary self-describes with `--help`:

```
DynaJS version 2026-06-04
usage: dynajs [options] [file [args]]
-h  --help         list options
-e  --eval EXPR    evaluate EXPR
-i  --interactive  go to interactive mode
-m  --module       load as ES6 module (default=autodetect)
    --script       load as ES6 script (default=autodetect)
    --strict       force strict mode
-I  --include file include an additional file
    --std          make 'std' and 'os' available to the loaded script
-T  --trace        trace memory allocation
-d  --dump         dump the memory usage stats
    --no-unhandled-rejection  ignore unhandled promise rejections
-s                 strip all the debug info
-q  --quit         just instantiate the interpreter and quit
```

The three you will use constantly:

```sh
dynajs script.js          # run a file (module vs script auto-detected)
dynajs -e 'EXPR'          # evaluate an expression and exit
dynajs -i                 # open the interactive REPL
```

`dynajs -d script.js` prints a memory-usage report on exit — handy for the low-footprint story:

```sh
dynajs -d -e '1+1'
# ...prints allocation counts, peak RSS, object/atom/shape stats...
```

## 2.3 Your first script

Create `hello.js`:

```js
// Top-level code just runs. `print` is a global; so is `console`.
print("Hello, DynaJS!");
console.log("console works too, and its methods are enumerable (WHATWG).");

const nums = [5, 3, 8, 1, 9, 2];
const evens = nums.filter(n => n % 2 === 0);
console.log("evens:", evens, "sum:", evens.reduce((a, b) => a + b, 0));
```

```sh
dynajs hello.js
#   Hello, DynaJS!
#   console works too, and its methods are enumerable (WHATWG).
#   evens: 8,2 sum: 10
```

Everything you know from modern JavaScript works: arrow functions, destructuring, spread,
`async`/`await`, generators, classes, `Map`/`Set`/`WeakMap`, typed arrays, `BigInt`, template
literals, optional chaining, nullish coalescing, `Array` group/`findLast`, and more (Chapter 3
covers the exact baseline).

## 2.4 ES modules and the `dynajs:` namespace

DynaJS uses standard ES modules. Auto-detection treats a file that uses `import`/`export` as a
module; force it with `-m` if needed.

```js
// math-utils.js
export function mean(xs) {
  return xs.reduce((a, b) => a + b, 0) / xs.length;
}
```

```js
// main.js
import { mean } from "./math-utils.js";
import { v4 } from "dynajs:uuid";

console.log("mean:", mean([2, 4, 6, 8]));   // 5
console.log("id:", v4());                    // a random UUIDv4
```

```sh
dynajs main.js
```

There are **three kinds of module specifier**:

1. **Relative / absolute paths** — `"./math-utils.js"`, `"/abs/path/mod.js"`. Your own code.
2. **`dynajs:*`** — the native standard library. `"dynajs:crypto"`, `"dynajs:simd"`,
   `"dynajs:http"`, etc. These are compiled into the binary (with `CONFIG_NATIVE_MODULES=y`).
3. **`std` and `os`** — the classic QuickJS system modules (low-level I/O, process control). Made
   available by `--std` or by importing them directly in a module context.

```js
import * as std from "std";
import * as os from "os";

const [dir, err] = os.readdir(".");
if (!err) console.log("entries:", dir.length);
std.gc();   // force a GC cycle (useful in tests / benchmarks)
```

> **Namespacing is a feature.** Because the standard library lives under `dynajs:`, there is never
> ambiguity about whether an import is yours, a third party's, or the runtime's — there *is* no
> third party. `dynajs:` always means "native, in-binary, curated."

## 2.5 The REPL

`dynajs -i` gives you an interactive session with the full runtime, including dynamic imports:

```
$ dynajs -i
DynaJS version 2026-06-04
> const { toHex } = await import("dynajs:bytes")
undefined
> toHex(new Uint8Array([222, 173, 190, 239]))
"deadbeef"
> import("dynajs:mathx").then(m => m.gcd(48, 36))
Promise { 12 }
```

Inside a plain script (not a module), use dynamic `import()` to reach the standard library, as in
the one-liners throughout this book. Inside a module, prefer static `import`.

## 2.6 A slightly bigger first program

Let's combine a few modules — a tiny content-addressed store that hashes a payload, gives it a
time-ordered id, and reports its gzip-compressed size. Every capability here is native and
dependency-free.

```js
import { sha256Hex } from "dynajs:crypto";
import { v7 } from "dynajs:uuid";
import { gzip } from "dynajs:compress";
import { toBase64 } from "dynajs:bytes";

function store(payload) {
  const id = v7();                      // time-ordered id (sorts by creation)
  const digest = sha256Hex(payload);    // content hash
  const packed = gzip(payload);         // real DEFLATE, native
  return {
    id,
    digest,
    originalBytes: payload.length,
    packedBytes: packed.length,
    packedB64: toBase64(packed),
  };
}

const rec = store("the quick brown fox ".repeat(100));
console.log("id:        ", rec.id);
console.log("sha256:    ", rec.digest);
console.log("orig/packed:", rec.originalBytes, "→", rec.packedBytes, "bytes");
```

```sh
dynajs bigger.js
#   id:         019f8f...-7...   (a v7 UUID)
#   sha256:     9c1185a5c5e9fc54612808977ee8f548b2258d31   ...
#   orig/packed: 2000 → 48 bytes
```

You now have a working DynaJS and a feel for its shape: standard JavaScript at the top, native
capabilities one `import` away. Chapter 3 covers the language baseline precisely; Chapter 4 is the
full standard-library tour.

---

*Next: [Chapter 3 — The Language & the Runtime](03-language-and-runtime.md).*
