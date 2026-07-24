/* test_search_simd.js — differential + throughput test for the SIMD substring
 * kernel (simd.strfind) wired into dyna:search. Oracle: native String.indexOf,
 * which for ASCII input has byte offsets == char indices, so the two must agree
 * exactly. Stresses lengths around the 16/32-byte SIMD block boundaries. */
import { indexOf, indexOfAll } from "dyna:search";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

/* deterministic PRNG so failures reproduce */
let seed = 0x9e3779b9 >>> 0;
function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }
const ALPHABET = "ab"; /* tiny alphabet => frequent partial/near matches */
function randStr(len) {
    let s = "";
    for (let i = 0; i < len; i++) s += ALPHABET[rnd() % ALPHABET.length];
    return s;
}

/* reference for overlapping indexOfAll */
function refAll(hay, needle) {
    if (needle.length === 0) return [];
    const out = [];
    let i = hay.indexOf(needle);
    while (i !== -1) { out.push(i); i = hay.indexOf(needle, i + 1); }
    return out;
}

let cases = 0;
/* sweep haystack + needle lengths across the SIMD block boundaries */
for (let hl = 0; hl <= 80; hl++) {
    for (let nl = 0; nl <= 6; nl++) {
        for (let rep = 0; rep < 40; rep++) {
            const hay = randStr(hl);
            /* half the time pull the needle out of the haystack so it hits */
            let needle;
            if (nl > 0 && hl >= nl && (rnd() & 1)) {
                const at = rnd() % (hl - nl + 1);
                needle = hay.slice(at, at + nl);
            } else {
                needle = randStr(nl);
            }
            const got = indexOf(hay, needle);
            const exp = hay.indexOf(needle); /* oracle */
            assert(got === exp,
                   "indexOf mismatch hay=" + JSON.stringify(hay) +
                   " needle=" + JSON.stringify(needle) +
                   " got=" + got + " exp=" + exp);
            const gotAll = indexOfAll(hay, needle);
            const expAll = refAll(hay, needle);
            assert(JSON.stringify(gotAll) === JSON.stringify(expAll),
                   "indexOfAll mismatch hay=" + JSON.stringify(hay) +
                   " needle=" + JSON.stringify(needle));
            cases++;
        }
    }
}
print("test_search_simd: " + cases + " differential cases OK");

/* --- throughput: large haystack, match only at the very end (full scan) --- */
const N = 4 << 20; /* 4 MiB */
let big = "a".repeat(N - 8) + "abXYZqrs"; /* needle only at the tail */
const needle = "abXYZqrs";
const expPos = N - 8;
function bench(fn, iters) {
    const t0 = performance.now();
    let acc = 0;
    for (let i = 0; i < iters; i++) acc += fn();
    return { ms: performance.now() - t0, acc };
}
const iters = 50;
const simd = bench(() => indexOf(big, needle), iters);
const native = bench(() => big.indexOf(needle), iters);
assert(simd.acc === expPos * iters, "SIMD found the tail match");
const mb = (N / (1 << 20)) * iters;
print("scan " + mb.toFixed(0) + " MiB -- dyna:search(SIMD): " + simd.ms.toFixed(1) +
      "ms (" + (mb / (simd.ms / 1000)).toFixed(0) + " MiB/s)  |  String.indexOf: " +
      native.ms.toFixed(1) + "ms (" + (mb / (native.ms / 1000)).toFixed(0) + " MiB/s)");
print("test_search_simd: all tests passed");
