/*
 * meta@ directives — LOOP level (§4.1 of META_DIRECTIVES.md)
 *
 * Run:   qjs examples/meta/01_loop.js
 * Trace: DYNASCRIPT_META_DUMP=1 qjs examples/meta/01_loop.js
 *
 * A directive attaches to the *next statement*, so it must sit IMMEDIATELY
 * before the loop it targets (put accumulator initialisation above it). Every
 * directive here is advisory: the loop computes the same result whether or not
 * the engine honours it. UNSAFE directives (int32/float64/nobounds/nopoll/
 * stride1/contiguous/independent/parallel) require the file-level opt-in below.
 */

// meta@enable(unsafe)

const N = 64;
const a = new Float64Array(N).map((_, i) => i + 1);
const b = new Float64Array(N).map((_, i) => (i * 3) % 7);
const c = new Float64Array(N);

let dot = 0;
for (let i = 0; i < N; i++) dot += a[i] * b[i];

// meta@autovec
for (let i = 0; i < N; i++) c[i] = a[i] + b[i];

let isum = 0;
// meta@int32
for (let i = 0; i < N; i++) isum = (isum + (i | 0)) | 0;

let fsum = 0.0;
// meta@float64
for (let i = 0; i < N; i++) fsum += a[i];

let last = 0;
// meta@nobounds
for (let i = 0; i < N; i++) last = a[i];

let spin = 0;
// meta@nopoll
for (let i = 0; i < 1000; i++) spin++;

let racc = 0;
// meta@reduce(sum)
for (let i = 0; i < N; i++) racc += a[i];

let tacc = 0;
for (let i = 0; i < N; i++) tacc += 1;

for (let i = 0; i < 4; i++) tacc += 0;

let scpy = 0;
// meta@stride1
for (let i = 0; i < N; i++) scpy += c[i];

// meta@contiguous
for (let i = 0; i < N; i++) c[i] = c[i] * 1;

let pacc = 0;
// meta@prefetch(16)
for (let i = 0; i < N; i++) pacc += b[i];

// meta@independent
for (let i = 0; i < N; i++) c[i] = a[i] - b[i];

let pcount = 0;
// meta@parallel
for (let i = 0; i < N; i++) pcount += 1;

let hot = 0;
for (let i = 0; i < N; i++) hot += a[i] * 2;

let cold = 0;
for (let i = 0; i < N; i++) cold += 1;

console.log("loop level ok: dot=" + dot.toFixed(1) + " isum=" + isum +
            " racc=" + racc.toFixed(1) + " tacc=" + tacc + " pcount=" + pcount +
            " last=" + last.toFixed(1) + " fsum=" + fsum.toFixed(1) +
            " scpy=" + scpy.toFixed(1) + " spin=" + spin + " hot=" + hot.toFixed(1));
