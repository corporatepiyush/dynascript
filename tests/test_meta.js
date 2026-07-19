/*
 * dynascript — meta@ optimization directive front-end tests.
 *
 * Stderr-clean by construction (every case is either a legal+enabled directive
 * or a strict-mode error that is thrown and caught), so it can run in `make test`.
 *
 * Two things are asserted:
 *   1. INVARIANT — a directive never changes a program's result. Each case
 *      eval()s the same logic with the hint and without it and compares (the
 *      "hints honored vs ignored -> identical output" oracle from the design),
 *      then checks the value against a hand-computed constant.
 *   2. DIAGNOSTICS — under `meta@strict`, unknown / illegally-placed / unsafe-
 *      without-enable / malformed directives throw a catchable SyntaxError;
 *      valid ones do not.
 */

function assert(actual, expected, message) {
    if (arguments.length == 1)
        expected = true;
    if (actual === expected)
        return;
    throw Error("assertion failed: got |" + actual + "|, expected |" +
                expected + "|" + (message ? " (" + message + ")" : ""));
}

/* eval the plain source and the hinted source; require equal results. */
function sameResult(plain, hinted, expected, msg) {
    const a = eval(plain);
    const b = eval(hinted);
    assert(a, b, msg + " (hint changed result)");
    assert(b, expected, msg + " (wrong value)");
}

function expectThrows(src, msg) {
    let threw = false;
    try { eval(src); } catch (e) { threw = (e instanceof SyntaxError); }
    assert(threw, true, "expected SyntaxError: " + msg);
}

function expectOK(src, msg) {
    try { eval(src); } catch (e) {
        throw Error("unexpected throw for " + msg + ": " + e);
    }
}

/*--------------------------------------------------------------------------*/

function test_invariant_loop() {
    sameResult(
        "let a=0; for (let i=0;i<100;i++) a+=i; a",
        "// meta@enable(unsafe)\nlet a=0;\n// meta@unroll(4), meta@int32\nfor (let i=0;i<100;i++) a+=i; a",
        4950, "loop unroll+int32");

    sameResult(
        "let p=1; for (let i=1;i<=6;i++) p*=i; p",
        "let p=1;\n// meta@reduce(prod)\nfor (let i=1;i<=6;i++) p*=i; p",
        720, "loop reduce");

    sameResult(
        "let s=0; for (let i=0;i<50;i++) s+=i*i; s",
        "// meta@enable(unsafe)\nlet s=0;\n// meta@nobounds, meta@nopoll, meta@independent\nfor (let i=0;i<50;i++) s+=i*i; s",
        40425, "loop nobounds/nopoll/independent");
}

function test_invariant_function() {
    sameResult(
        "function f(n){return n<2?n:f(n-1)+f(n-2);} f(15)",
        "// meta@memoize\nfunction f(n){return n<2?n:f(n-1)+f(n-2);} f(15)",
        610, "function memoize");

    sameResult(
        "// meta@enable(unsafe)\nfunction g(x){return (x^(x>>>13))>>>0;} g(12345)",
        "// meta@enable(unsafe)\n// meta@pure\nfunction g(x){return (x^(x>>>13))>>>0;} g(12345)",
        12344, "function pure");

    sameResult(
        "function t(n,a){return n===0?a:t(n-1,a+n);} t(20,0)",
        "// meta@tailcall\nfunction t(n,a){return n===0?a:t(n-1,a+n);} t(20,0)",
        210, "function tailcall");
}

function test_invariant_class() {
    sameResult(
        "class V{constructor(x,y,z){this.x=x;this.y=y;this.z=z;}s(){return this.x+this.y+this.z;}} new V(1,2,3).s()",
        "// meta@sealed\nclass V{constructor(x,y,z){this.x=x;this.y=y;this.z=z;}s(){return this.x+this.y+this.z;}} new V(1,2,3).s()",
        6, "class sealed");

    sameResult(
        "// meta@enable(unsafe)\nclass P{constructor(a){this.a=a;}} new P(9).a",
        "// meta@enable(unsafe)\n// meta@pod, meta@final, meta@noproto\nclass P{constructor(a){this.a=a;}} new P(9).a",
        9, "class pod/final/noproto");
}

function test_invariant_control_flow() {
    sameResult(
        "function c(x){let t;if(x>0)t=1;else t=-1;return t;} c(5)+c(-5)",
        "function c(x){let t;\n// meta@likely\nif(x>0)t=1;else t=-1;return t;} c(5)+c(-5)",
        0, "if likely");

    sameResult(
        "function sw(x){switch(x&3){case 0:return 'a';case 1:return 'b';default:return 'c';}} sw(1)",
        "function sw(x){\n// meta@jumptable\nswitch(x&3){case 0:return 'a';case 1:return 'b';default:return 'c';}} sw(1)",
        "b", "switch jumptable");
}

function test_invariant_alloc() {
    sameResult(
        "const a=new Array(10); for(let i=0;i<10;i++)a[i]=i; a[9]",
        "// meta@preallocate(10)\nconst a=new Array(10); for(let i=0;i<10;i++)a[i]=i; a[9]",
        9, "alloc preallocate");

    sameResult(
        "// meta@enable(unsafe)\nfunction m(i){const t={i:i,sq:i*i};return t.sq;} m(7)",
        "// meta@enable(unsafe)\nfunction m(i){\n// meta@stack, meta@noescape\nconst t={i:i,sq:i*i};return t.sq;} m(7)",
        49, "alloc stack/noescape");
}

function test_invariant_variable() {
    sameResult(
        "// meta@enable(unsafe)\nfunction px(i,b){return b[i&0xff];} px(2,[10,20,30,40])",
        "// meta@enable(unsafe)\nfunction px(i,b){\n// meta@range(i,0,255), meta@nonnull(b)\nreturn b[i&0xff];} px(2,[10,20,30,40])",
        30, "variable range/nonnull");

    sameResult(
        "// meta@enable(unsafe)\nfunction d(n,q){return n/q;} d(84,2)",
        "// meta@enable(unsafe)\nfunction d(n,q){\n// meta@nonzero(q), meta@type(n,i32)\nreturn n/q;} d(84,2)",
        42, "variable nonzero/type");
}

function test_strict_diagnostics() {
    /* unknown directive */
    expectThrows("// meta@strict\n// meta@definitely_not_a_directive\n1;", "unknown");
    /* illegal placement: class directive on a loop */
    expectThrows("// meta@strict\n// meta@sealed\nfor(let i=0;i<1;i++){}", "sealed on loop");
    /* illegal placement: loop directive on a class */
    expectThrows("// meta@strict\n// meta@unroll(4)\nclass C{}", "unroll on class");
    /* unsafe without enable */
    expectThrows("// meta@strict\n// meta@int32\nfor(let i=0;i<1;i++){}", "int32 ungated");
    /* malformed enable */
    expectThrows("// meta@strict\n// meta@enable(bogus)\n1;", "bad enable");
    /* missing variable target */
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@range\nlet x=1;", "range no target");
    /* wrong numeric arity */
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@range(x)\nlet x=1;", "range missing bounds");
}

function test_strict_accepts_valid() {
    /* correctly placed + enabled directives must NOT throw under strict */
    expectOK("// meta@strict\n// meta@unroll(4)\nfor(let i=0;i<2;i++){}", "unroll on loop");
    expectOK("// meta@strict\n// meta@sealed\nclass C{}", "sealed on class");
    expectOK("// meta@strict\n// meta@enable(unsafe)\n// meta@int32\nfor(let i=0;i<2;i++){}", "int32 gated");
    expectOK("// meta@strict\n// meta@enable(unsafe)\n// meta@range(x,0,9)\nlet x=5;", "range with bounds");
    expectOK("// meta@strict\nfunction f(){}\nf();", "no directives at all");
}

function test_forms() {
    /* comma-separated, stacked lines, block comments, no-space, whitespace */
    expectOK("// meta@enable(unsafe)\n// meta@unroll(4), meta@int32\nfor(let i=0;i<2;i++){}", "comma-separated");
    expectOK("// meta@enable(unsafe)\n//meta@unroll(4)\nfor(let i=0;i<2;i++){}", "no space");
    expectOK("/* meta@sealed */\nclass C{}", "block comment");
    expectOK("//   meta@unroll(2)\nfor(let i=0;i<2;i++){}", "leading spaces");
    /* a directive on plain code is simply not attached to a loop/func/class */
    expectOK("// meta@enable(unsafe)\n// meta@preallocate(4)\nlet z=[];", "preallocate on stmt");
}

/*--------------------------------------------------------------------------*/

test_invariant_loop();
test_invariant_function();
test_invariant_class();
test_invariant_control_flow();
test_invariant_alloc();
test_invariant_variable();
test_strict_diagnostics();
test_strict_accepts_valid();
test_forms();

console.log("test_meta: all tests passed");
