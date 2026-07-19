/*
 * meta@ directives — ALLOCATION level (§4.5)
 *
 * Attached to a statement, loop or function scope. reuse/pool/stack/noescape
 * are UNSAFE (assert nothing escapes) and need the file-level opt-in.
 */

// meta@enable(unsafe)

// meta@preallocate(1000)
const big = new Array(1000);
for (let i = 0; i < big.length; i++) big[i] = i;

// meta@reuse
let scratch = { x: 0, y: 0 };
let acc = 0;
for (let i = 0; i < 100; i++) {
    scratch = { x: i, y: -i };
    acc += scratch.x + scratch.y;
}

// meta@pool(64)
const buffers = [];
for (let i = 0; i < 8; i++) buffers.push(new Uint8Array(64));

// meta@stack
function centroid(ps) {
    let cx = 0, cy = 0;
    for (const p of ps) { cx += p.x; cy += p.y; }
    return cx + cy;
}

// meta@noescape
function makeTemp(i) {
    const t = { i, sq: i * i };
    return t.sq;
}

// meta@transient
const temps = [];
for (let i = 0; i < 4; i++) temps.push({ tick: i });

// meta@weak
const cacheLike = new Map();
for (let i = 0; i < 4; i++) cacheLike.set(i, { v: i });

console.log("alloc level ok: big[999]=" + big[999] + " acc=" + acc +
            " buffers=" + buffers.length + " centroid=" +
            centroid([{ x: 1, y: 2 }, { x: 3, y: 4 }]) +
            " temp=" + makeTemp(5) + " temps=" + temps.length +
            " cache=" + cacheLike.size);
