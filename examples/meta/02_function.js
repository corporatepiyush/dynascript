/*
 * meta@ directives — FUNCTION level (§4.2)
 *
 * Attached to a function declaration. pure/nosideeffects/arena/scoped_alloc/
 * noalloc/monomorphic are UNSAFE (assertions about the body) and need the
 * file-level opt-in.
 */

// meta@enable(unsafe)

// meta@inline
function add(x, y) { return x + y; }

// meta@noinline
function slowpath(x) { return x * x - 1; }

// meta@pure
function hash(x) { return (x ^ (x >>> 13)) >>> 0; }

// meta@nosideeffects
function square(x) { return x * x; }

// meta@arena(4096)
function buildList(n) {
    const out = [];
    for (let i = 0; i < n; i++) out.push(i * i);
    return out.length;
}

// meta@scoped_alloc
function transform(n) {
    let acc = 0;
    for (let i = 0; i < n; i++) acc += (i % 3);
    return acc;
}

// meta@noalloc
function dotp(a, b, n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

// meta@memoize
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

// meta@tailcall
function sumTo(n, acc) { return n === 0 ? acc : sumTo(n - 1, acc + n); }

// meta@monomorphic(int32, int32)
function mul(a, b) { return a * b; }

// meta@hot
function step(x) { return x + 1; }

// meta@cold
function onError(msg) { return "error: " + msg; }

console.log("function level ok: add=" + add(2, 3) + " hash=" + hash(1234) +
            " list=" + buildList(8) + " fib=" + fib(12) +
            " sumTo=" + sumTo(10, 0) + " mul=" + mul(6, 7) +
            " dotp=" + dotp([1, 2, 3], [4, 5, 6], 3) +
            " transform=" + transform(9) + " step=" + step(step(0)) +
            " square=" + square(5) + " slow=" + slowpath(4) +
            " err=" + onError("x"));
