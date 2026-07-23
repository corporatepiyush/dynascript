/*
 * dynajs:structures3 -- more native data structures from secure-c-libs, with
 * DETERMINISTIC memory management (one arena per object, O(1) close()).
 *
 * Requires the SCL-modules build:
 *     make CONFIG_SCL_MODULES=y
 *     ./dynajs examples/js/dynajs_structures3.js
 *
 *   LRUCache(capacity) : int32 key -> double value, evicts least-recently-used
 *   UnionFind(n)       : disjoint-set forest over elements 0..n-1
 *   SortedSet()        : ordered set of doubles (skiplist)
 */
import { LRUCache, UnionFind, SortedSet } from "dynajs:structures3";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }

/* Deterministic-dispose helper: always closes the resource, even on throw. */
function withResource(resource, fn) {
    try { return fn(resource); }
    finally { resource.close(); }
}

/* ---- LRUCache: capacity-bounded, least-recently-used eviction ---- */
function demo_lru() {
    return withResource(new LRUCache(2), (c) => {
        c.put(1, 1.0);
        c.put(2, 2.0);
        assert(c.get(1) === 1.0, "get 1");   // touch 1 -> 1 is now MRU
        c.put(3, 3.0);                        // capacity 2 exceeded -> evict LRU (2)
        assert(c.get(2) === undefined, "2 evicted");
        assert(c.get(1) === 1.0 && c.get(3) === 3.0, "1 and 3 kept");
        return c.size;                        // 2
    });
}

/* ---- UnionFind: connectivity / components ---- */
function demo_unionfind() {
    return withResource(new UnionFind(6), (uf) => {
        uf.union(0, 1);
        uf.union(2, 3);
        uf.union(1, 2);                       // merges {0,1} and {2,3}
        assert(uf.connected(0, 3) === true, "0..3 connected");
        assert(uf.connected(0, 5) === false, "0,5 separate");
        assert(uf.find(0) === uf.find(3), "same root");
        return uf.count;                      // components: {0,1,2,3},{4},{5} -> 3
    });
}

/* ---- SortedSet: ordered set of doubles (skiplist-backed) ---- */
function demo_sortedset() {
    return withResource(new SortedSet(), (s) => {
        for (const v of [5, 3, 8, 1, 3]) s.add(v);  // 3 is a duplicate
        assert(s.size === 4, "dedup");
        assert(s.has(8) && !s.has(9), "membership");
        assert(s.delete(3) === true && !s.has(3), "delete");
        return s.size;                        // 3
    });
}

print("LRUCache demo: size =", demo_lru());
print("UnionFind demo: components =", demo_unionfind());
print("SortedSet demo: size =", demo_sortedset());

/* ---- The point: deterministic release means flat memory ---- */
for (let i = 0; i < 100000; i++) {
    const uf = new UnionFind(8);
    uf.union(0, 1);
    uf.close();                               // explicit, immediate O(1) free
}
print("Deterministic-free demo: 100000 UnionFind created+closed in constant memory");

// closed resources reject further use (fail fast, not silent corruption)
const dead = new LRUCache(1);
dead.close();
let threw = false;
try { dead.get(0); } catch (e) { threw = true; }
assert(threw, "use-after-close must throw");
assert(dead.closed === true, "closed flag");

print("PASS");
