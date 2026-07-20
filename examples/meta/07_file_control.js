/*
 * meta@ directives — FILE-level control
 *
 *   meta@enable(unsafe)  opt in to UNSAFE directives for the rest of the file
 *   meta@strict          promote every meta diagnostic to a hard SyntaxError
 *   meta@dump            print each captured directive (== DYNASCRIPT_META_DUMP=1)
 *
 * With meta@strict active, EVERY directive below must be known, legally placed
 * and (if UNSAFE) enabled — otherwise the file fails to parse. This file is
 * therefore a good "does it all still validate?" canary.
 */

// meta@enable(unsafe)
// meta@dump
// meta@strict

let s = 0;
// meta@prefetch(8)
for (let i = 0; i < 32; i++) s += i;

// meta@sealed
class Ok { constructor(v) { this.v = v; } }

// meta@range(s, 0, 1024), meta@int32
for (let i = 0; i < 4; i++) s = (s + i) | 0;

console.log("file-control ok: s=" + s + " ok=" + new Ok(1).v);
