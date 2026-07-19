/*
 * meta@ directives — CONTROL-FLOW level (§4.4)
 *
 * Attached to if / switch (and, for noexcept/nothrow, any statement block).
 * assume/invariant/noexcept/nothrow are UNSAFE and need the file-level opt-in.
 */

// meta@enable(unsafe)

function classify(x) {
    let tag = "";

    // meta@likely
    if (x > 0) tag = "pos"; else tag = "nonpos";

    // meta@unlikely
    if (x === 0x7fffffff) tag += "!max";

    // meta@unpredictable
    if ((x & 1) === 0) tag += ".even"; else tag += ".odd";

    // meta@jumptable
    switch (x % 4) {
    case 0: tag += "|a"; break;
    case 1: tag += "|b"; break;
    case 2: tag += "|c"; break;
    default: tag += "|d"; break;
    }

    // meta@dense
    switch (x & 3) {
    case 0: case 1: case 2: case 3: tag += "#"; break;
    }

    return tag;
}

// meta@assume(x >= 0)
function toUnsignedIndex(x) { return x & 0xffff; }

// meta@invariant(n > 0)
function firstOf(arr, n) { return n > 0 ? arr[0] : -1; }

// meta@noexcept
function safeAdd(a, b) { return a + b; }

// meta@nothrow
function neverThrows() { return 42; }

console.log("control-flow ok: " + classify(6) + " / " + classify(0) +
            " idx=" + toUnsignedIndex(70000) + " first=" + firstOf([9], 1) +
            " add=" + safeAdd(2, 3) + " nt=" + neverThrows());
