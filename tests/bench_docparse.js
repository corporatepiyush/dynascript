/*
 * bench_docparse.js -- throughput benchmark + correctness oracle for the native
 * dyna:docparse CSV parser. Not part of `make test` (it is a bench). Kept
 * in-repo so any future parser rewrite can be measured AND proven equivalent:
 * the FNV-1a checksum of the parsed output is printed alongside MB/s, so two
 * builds (e.g. scalar vs a candidate SIMD path) must print the SAME checksum.
 *
 *   ./dynajs tests/bench_docparse.js [narrow|wide]
 *
 * History: a SIMD find_first_of rewrite of the field/row scans was tried and
 * REVERTED -- it matched the checksum but regressed 15-20% because CSV fields
 * are only a few bytes, so the per-field indirect call to the kernel costs more
 * than the inline compare it replaced. Use this bench before trying again.
 */
import { parseCsv } from "dyna:docparse";

function makeCsv(rows, cols, wide) {
    let out = [];
    let hdr = []; for (let c = 0; c < cols; c++) hdr.push("col" + c);
    out.push(hdr.join(","));
    for (let r = 0; r < rows; r++) {
        let f = [];
        for (let c = 0; c < cols; c++) {
            if (wide && (c % 3 === 0)) f.push('"' + ("x".repeat(40)) + ', embedded, "" quote"');
            else f.push(String((r * 31 + c) % 100000));
        }
        out.push(f.join(","));
    }
    return out.join("\r\n"); // CRLF terminators
}

// FNV-1a over every parsed cell -- the equivalence oracle across builds.
function checksum(rows) {
    let h = 2166136261 >>> 0;
    for (const row of rows)
        for (const cell of row) {
            for (let i = 0; i < cell.length; i++) { h ^= cell.charCodeAt(i); h = (h * 16777619) >>> 0; }
            h ^= 124;
        }
    return h >>> 0;
}

const MODE = scriptArgs[1] || "narrow";
const csv = MODE === "wide" ? makeCsv(4000, 12, true) : makeCsv(40000, 8, false);
const sum = checksum(parseCsv(csv)); // warm + oracle

const ITER = 50;
let best = Infinity;
for (let t = 0; t < 5; t++) {
    const s = performance.now();
    for (let i = 0; i < ITER; i++) parseCsv(csv);
    const e = performance.now();
    if (e - s < best) best = e - s;
}
const mbps = (csv.length * ITER) / 1e6 / (best / 1000);
print(MODE + "  checksum=0x" + sum.toString(16) +
      "  bestMs=" + best.toFixed(2) + "  MB/s=" + mbps.toFixed(1));
