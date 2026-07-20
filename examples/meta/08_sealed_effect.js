/*
 * meta@sealed / meta@fixed_layout — the ACTUAL runtime effect (§4.3)
 *
 * Run:   dynajs examples/meta/08_sealed_effect.js
 * Trace: DYNASCRIPT_META_DUMP=1 dynajs examples/meta/08_sealed_effect.js
 *
 * Unlike every other directive (which is front-end-only today), meta@sealed is
 * the one landed codegen transform: when construction of the OUTERMOST class of
 * a `new` finishes, the instance is made non-extensible (Object.isExtensible ->
 * false). The shape the constructor built is therefore final — the enabler for
 * constant-offset inline caches. It is semantics-preserving for correct code:
 * reads, writes to existing fields and method calls are unchanged; only ADDING
 * a new property is refused (silently in sloppy mode, TypeError in strict).
 *
 * This is a SAFE directive: no `meta@enable(unsafe)` is required.
 */

// meta@sealed
class Vec3 {
    constructor(x, y, z) { this.x = x; this.y = y; this.z = z; }
    len2() { return this.x * this.x + this.y * this.y + this.z * this.z; }
}

// meta@fixed_layout — alias of sealed
class Point {
    constructor(x, y) { this.x = x; this.y = y; }
}

// a plain, unsealed class for contrast
class Loose {
    constructor(id) { this.id = id; }
}

// derived sealed class: super() must NOT seal early, or the derived
// constructor's own field writes would fail; the instance is sealed once,
// after the outermost constructor returns.
// meta@sealed
class Vec4 extends Vec3 {
    constructor(x, y, z, w) { super(x, y, z); this.w = w; }
}

// sealed base reached through a NON-sealed derived class: sealing keys on the
// outermost constructor (=== new_target), so this instance stays extensible.
class LooseVec extends Vec3 {
    constructor(x, y, z, tag) { super(x, y, z); this.tag = tag; }
}

/* --- base sealed instance --- */
const v = new Vec3(1, 2, 2);
const sealedExtensible = Object.isExtensible(v);     // false
v.w = 99;                                            // sloppy: silently ignored
const addedInSloppy = ("w" in v);                    // false

let strictThrew = false;
try {
    (function () { "use strict"; v.w = 99; })();     // strict: TypeError
} catch (e) {
    strictThrew = (e instanceof TypeError);
}

/* --- fixed_layout alias --- */
const p = new Point(3, 4);
const pointExtensible = Object.isExtensible(p);      // false

/* --- plain class stays extensible --- */
const l = new Loose(7);
l.extra = "ok";
const looseExtensible = Object.isExtensible(l);      // true

/* --- derived sealed: own field set before sealing --- */
const v4 = new Vec4(1, 2, 2, 5);
const derivedExtensible = Object.isExtensible(v4);   // false
const derivedFieldsOk = (v4.x === 1 && v4.w === 5);  // true

/* --- sealed base via non-sealed derived: extensible --- */
const lv = new LooseVec(1, 0, 0, "n");
const baseThroughLooseExtensible = Object.isExtensible(lv); // true

console.log("sealed effect ok:" +
    " Vec3.isExtensible=" + sealedExtensible +
    " sloppyAddIgnored=" + (!addedInSloppy) +
    " strictAddThrows=" + strictThrew +
    " len2=" + v.len2() +
    " | Point.isExtensible=" + pointExtensible +
    " | Loose.isExtensible=" + looseExtensible +
    " | Vec4.isExtensible=" + derivedExtensible +
    " Vec4.fields=" + derivedFieldsOk +
    " | LooseVec.isExtensible=" + baseThroughLooseExtensible);
