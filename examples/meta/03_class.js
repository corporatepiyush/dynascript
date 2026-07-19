/*
 * meta@ directives — CLASS level (§4.3)
 *
 * Attached to a class declaration. pod/final/noproto are UNSAFE (assertions
 * about instance shape / method overriding) and need the file-level opt-in.
 */

// meta@enable(unsafe)

// meta@sealed
class Vec3 {
    constructor(x, y, z) { this.x = x; this.y = y; this.z = z; }
    len2() { return this.x * this.x + this.y * this.y + this.z * this.z; }
}

// meta@fixed_layout
class Point { constructor(x, y) { this.x = x; this.y = y; } }

// meta@pod
class Rgba { constructor(r, g, b, a) { this.r = r; this.g = g; this.b = b; this.a = a; } }

// meta@final
class Shape { area() { return 0; } }

// meta@preallocate_fields(4)
class Particle {
    constructor(x, y, vx, vy) { this.x = x; this.y = y; this.vx = vx; this.vy = vy; }
}

// meta@soa
class Body { constructor(m, p) { this.m = m; this.p = p; } }

// meta@noproto
class Flat { constructor(id) { this.id = id; } }

const v = new Vec3(1, 2, 2);
const p = new Point(3, 4);
const px = new Particle(0, 0, 1, 1);
console.log("class level ok: len2=" + v.len2() + " point=(" + p.x + "," + p.y + ")" +
            " rgba=" + new Rgba(1, 2, 3, 4).a + " area=" + new Shape().area() +
            " particle=" + px.vx + " body=" + new Body(5, 6).m +
            " flat=" + new Flat(42).id);
