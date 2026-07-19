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
/* Exhaustive, data-driven front-end coverage of EVERY directive in
 * js_meta_table[]. Each row is a MINIMAL LEGAL placement: correct host
 * construct + minimal legal arguments. The table is the oracle for tier
 * gating and legality:
 *   - SAFE  directives are accepted under `meta@strict` with no enable.
 *   - UNSAFE directives are REJECTED under strict WITHOUT
 *     `meta@enable(unsafe)` and ACCEPTED with it.
 * test_table_is_exhaustive() pins the row count, so if the registry grows a
 * directive this suite fails until the table is extended — keeping it complete.
 * (The 3 file-level directives enable/strict/dump attach to no construct and
 * are covered by test_file_directives().)
 */
const S = 0, U = 1;   /* mirror JS_META_SAFE / JS_META_UNSAFE */
const HOST_LOOP  = "for(let i=0;i<2;i++){}";
const HOST_FUNC  = "function _f(){}";
const HOST_CLASS = "class _C{}";
const HOST_IF    = "if(1){}";
const HOST_SWITCH= "switch(1){}";
const HOST_STMT  = "let _s=1;";

const META = [
    /* name,               tier, directive-text,            host */
    /* -- 4.1 loop level -- */
    ["unroll",             S, "unroll(4)",                  HOST_LOOP],
    ["autovec",            S, "autovec",                    HOST_LOOP],
    ["int32",              U, "int32",                      HOST_STMT],
    ["float64",            U, "float64",                    HOST_STMT],
    ["nobounds",           U, "nobounds",                   HOST_LOOP],
    ["nopoll",             U, "nopoll",                     HOST_LOOP],
    ["reduce",             S, "reduce(sum)",                HOST_LOOP],
    ["trip",               S, "trip(4)",                    HOST_LOOP],
    ["fixed",              S, "fixed",                      HOST_LOOP],
    ["stride1",            U, "stride1",                    HOST_LOOP],
    ["contiguous",         U, "contiguous",                 HOST_LOOP],
    ["prefetch",           S, "prefetch(16)",               HOST_LOOP],
    ["independent",        U, "independent",                HOST_LOOP],
    ["parallel",           U, "parallel",                   HOST_LOOP],
    /* -- 4.2 function level -- */
    ["inline",             S, "inline",                     HOST_FUNC],
    ["noinline",           S, "noinline",                   HOST_FUNC],
    ["pure",               U, "pure",                       HOST_FUNC],
    ["nosideeffects",      U, "nosideeffects",              HOST_FUNC],
    ["arena",              U, "arena(4096)",                HOST_FUNC],
    ["scoped_alloc",       U, "scoped_alloc(2048)",         HOST_FUNC],
    ["noalloc",            U, "noalloc",                     HOST_FUNC],
    ["memoize",            S, "memoize",                     HOST_FUNC],
    ["tailcall",           S, "tailcall",                   HOST_FUNC],
    ["monomorphic",        U, "monomorphic(i32,i32)",       HOST_FUNC],
    /* -- 4.3 class level -- */
    ["sealed",             S, "sealed",                     HOST_CLASS],
    ["fixed_layout",       S, "fixed_layout",               HOST_CLASS],
    ["pod",                U, "pod",                        HOST_CLASS],
    ["final",              U, "final",                      HOST_CLASS],
    ["preallocate_fields", S, "preallocate_fields(4)",      HOST_CLASS],
    ["soa",                S, "soa",                        HOST_CLASS],
    ["noproto",            U, "noproto",                    HOST_CLASS],
    /* -- 4.4 control flow -- */
    ["likely",             S, "likely",                     HOST_IF],
    ["unlikely",           S, "unlikely",                   HOST_IF],
    ["unpredictable",      S, "unpredictable",              HOST_IF],
    ["jumptable",          S, "jumptable",                  HOST_SWITCH],
    ["dense",              S, "dense",                       HOST_SWITCH],
    ["assume",             U, "assume(x)",                  HOST_STMT],
    ["invariant",          U, "invariant(x)",               HOST_STMT],
    ["noexcept",           U, "noexcept",                   HOST_STMT],
    ["nothrow",            U, "nothrow",                    HOST_STMT],
    /* -- 4.5 allocation level -- */
    ["preallocate",        S, "preallocate(8)",             HOST_STMT],
    ["reuse",              U, "reuse",                       HOST_STMT],
    ["pool",               U, "pool(64)",                    HOST_STMT],
    ["stack",              U, "stack",                       HOST_STMT],
    ["noescape",           U, "noescape",                   HOST_STMT],
    ["transient",          S, "transient",                  HOST_STMT],
    ["weak",               S, "weak",                        HOST_STMT],
    /* -- shared -- */
    ["hot",                S, "hot",                         HOST_LOOP],
    ["cold",               S, "cold",                        HOST_LOOP],
    /* -- variable-based -- */
    ["range",              U, "range(x,0,9)",               HOST_STMT],
    ["nonnull",            U, "nonnull(b)",                  HOST_STMT],
    ["nonzero",            U, "nonzero(q)",                  HOST_STMT],
    ["type",               U, "type(v,i32)",                HOST_STMT],
    ["const",              U, "const(k)",                    HOST_STMT],
    ["frozen",             U, "frozen(o)",                   HOST_STMT],
    ["align",              U, "align(b,16)",                 HOST_STMT],
    ["length",             U, "length(a,4)",                 HOST_STMT],
    ["init",               U, "init(v)",                     HOST_STMT],
    ["volatile",           S, "volatile(t)",                HOST_STMT],
];

function test_table_is_exhaustive() {
    /* 62 directives in js_meta_table[] = 59 construct-attached (this table)
       + 3 file-level (enable/strict/dump). Update this table if the registry
       changes so the suite stays exhaustive. */
    assert(META.length, 59, "META table must cover all 59 construct directives");
}

/* Every directive: legal placement accepted under strict; every UNSAFE one
   rejected under strict without enable, accepted with it. */
function test_every_directive_gating() {
    for (let i = 0; i < META.length; i++) {
        const name = META[i][0], tier = META[i][1];
        const dir = META[i][2], host = META[i][3];
        const en = (tier === U) ? "// meta@enable(unsafe)\n" : "";
        expectOK("// meta@strict\n" + en + "// meta@" + dir + "\n" + host,
                 "legal placement: " + name);
        if (tier === U) {
            expectThrows("// meta@strict\n// meta@" + dir + "\n" + host,
                         "unsafe ungated: " + name);
        }
    }
}

/* Legality diagnostics: a directive on the wrong construct is a strict error.
   Covers each construct-specific class (loop/func/class/branch/hot-cold). */
function test_wrong_construct() {
    /* loop-only on class / function */
    expectThrows("// meta@strict\n// meta@unroll(4)\nclass C{}", "unroll on class");
    expectThrows("// meta@strict\n// meta@reduce(sum)\nclass C{}", "reduce on class");
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@nobounds\nfunction f(){}",
                 "nobounds on function");
    /* class-only on loop / function */
    expectThrows("// meta@strict\n// meta@sealed\nfor(let i=0;i<1;i++){}", "sealed on loop");
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@pod\nfor(let i=0;i<1;i++){}",
                 "pod on loop");
    expectThrows("// meta@strict\n// meta@soa\nfunction f(){}", "soa on function");
    /* function-only on loop / class */
    expectThrows("// meta@strict\n// meta@inline\nfor(let i=0;i<1;i++){}", "inline on loop");
    expectThrows("// meta@strict\n// meta@memoize\nclass C{}", "memoize on class");
    /* branch-only on loop / function / class */
    expectThrows("// meta@strict\n// meta@likely\nfor(let i=0;i<1;i++){}", "likely on loop");
    expectThrows("// meta@strict\n// meta@jumptable\nfunction f(){}", "jumptable on function");
    expectThrows("// meta@strict\n// meta@dense\nclass C{}", "dense on class");
    /* hot/cold apply to loop|function only, NOT class or plain statement */
    expectThrows("// meta@strict\n// meta@hot\nclass C{}", "hot on class");
    expectThrows("// meta@strict\n// meta@cold\nlet z=1;", "cold on plain statement");
    /* unknown directive is a strict error too */
    expectThrows("// meta@strict\n// meta@definitely_not_a_directive\n1;", "unknown directive");
}

/* Argument parsing: numeric, target, keyword, negative, saturating-huge,
   multi-arg; and the arity / missing-target diagnostics. */
function test_arg_forms() {
    /* numeric arg (optional; present) */
    expectOK("// meta@strict\n// meta@unroll(8)\nfor(let i=0;i<2;i++){}", "numeric arg");
    /* negative number is a legal single arg */
    expectOK("// meta@strict\n// meta@unroll(-4)\nfor(let i=0;i<2;i++){}", "negative arg");
    /* saturating-huge number stays a legal single arg (no wrap/throw) */
    expectOK("// meta@strict\n// meta@trip(999999999999999999999999)\nfor(let i=0;i<2;i++){}",
             "huge number saturates");
    /* variable target */
    expectOK("// meta@strict\n// meta@enable(unsafe)\n// meta@nonnull(buf)\nlet buf=[];",
             "variable target");
    /* keyword-style arg (reduce op / type name) */
    expectOK("// meta@strict\n// meta@reduce(prod)\nfor(let i=0;i<2;i++){}", "keyword reduce");
    expectOK("// meta@strict\n// meta@enable(unsafe)\n// meta@type(v,i32)\nlet v=1;", "keyword type");
    /* two numeric args */
    expectOK("// meta@strict\n// meta@enable(unsafe)\n// meta@range(x,0,255)\nlet x=1;", "two numeric args");
    /* target + one numeric */
    expectOK("// meta@strict\n// meta@enable(unsafe)\n// meta@align(b,16)\nlet b=0;", "target + numeric");
    /* missing required target -> strict error */
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@range\nlet x=1;", "range no target");
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@nonnull\nlet x=1;", "nonnull no target");
    /* wrong numeric arity -> strict error */
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@range(x)\nlet x=1;", "range too few args");
    expectThrows("// meta@strict\n// meta@trip\nfor(let i=0;i<2;i++){}", "trip needs a count");
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@align(b)\nlet b=0;", "align needs N");
}

/* Capture forms: block comments (incl. directive on a later line), stacked
   consecutive lines, comma- and space-separated, no-space, leading tabs. */
function test_forms_extended() {
    expectOK("/* meta@sealed */\nclass C{}", "block comment");
    expectOK("/* meta@sealed */ class C{}", "inline block comment");
    expectOK("/*\n meta@unroll(2)\n*/\nfor(let i=0;i<2;i++){}", "block later-line");
    expectOK("/* meta@sealed (seal it) */\nclass C{}", "block trailing prose");
    expectOK("// meta@enable(unsafe)\n// meta@unroll(4), meta@int32, meta@nobounds\nfor(let i=0;i<2;i++){}",
             "three comma-separated");
    expectOK("// meta@enable(unsafe)\n// meta@unroll(4) meta@int32\nfor(let i=0;i<2;i++){}",
             "space-separated");
    expectOK("// meta@enable(unsafe)\n// meta@unroll(4)\n// meta@int32\n// meta@nobounds\nfor(let i=0;i<2;i++){}",
             "three stacked lines");
    expectOK("//meta@unroll(4)\nfor(let i=0;i<2;i++){}", "no space after //");
    expectOK("//\t  meta@unroll(2)\nfor(let i=0;i<2;i++){}", "leading tab+spaces");
}

/* Malformed / adversarial inputs that must be accepted without a diagnostic
   (so they run clean, no stderr) and never crash. */
function test_adversarial_clean() {
    expectOK("// meta@", "bare meta@ at buffer end");
    expectOK("// meta@   ", "meta@ then whitespace only");
    expectOK("// meta@()\n1;", "empty directive name");
    expectOK("// mentions meta@unroll but not as a directive prefix\n1;",
             "meta@ not at comment start");
    /* a SCOPE directive on a plain statement is legal -> no warning */
    expectOK("// meta@enable(unsafe)\n// meta@int32\nlet z=1;", "int32 on statement");
    expectOK("// meta@transient\nlet z=1;", "transient on statement");
}

/* Malformed / adversarial inputs that, under strict, are caught as a
   SyntaxError (proving no crash and clean escalation). */
function test_adversarial_strict_caught() {
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@range(x,\nlet x=1;",
                 "truncated arg list");
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@range()\nlet x=1;",
                 "empty parens, needs target");
    expectThrows("// meta@strict\n// meta@enable(unsafe)\n// meta@nonnull(>>>)\nlet x=1;",
                 "operators only, no target");
    expectThrows("// meta@strict\n// meta@enable(bogus)\n1;", "bad enable argument");
    expectThrows("// meta@strict\n// meta@enable\n1;", "enable with no argument");
}

/* File-level control directives, and the js_parse_directives fix: a meta@
   directive after a `use strict` prologue must not raise a spurious warning
   (which, under strict, would throw). */
function test_file_and_prologue() {
    expectOK("// meta@enable(unsafe)\n// meta@int32\nlet _x=1;", "enable then int32");
    expectOK("// meta@strict\nlet _x=1;", "strict alone");
    expectOK("// meta@strict\n// meta@dump\nlet _x=1;", "dump alone (no trailing directive)");
    /* use strict prologue at program scope */
    expectOK('"use strict";\n// meta@strict\n// meta@sealed\nclass C{}',
             "sealed after use strict prologue");
    expectOK('"use strict";\n// meta@strict\n// meta@enable(unsafe)\n// meta@int32\nfor(let i=0;i<2;i++){}',
             "int32 after use strict prologue");
    /* use strict prologue inside a function body */
    expectOK('function f(){ "use strict";\n// meta@strict\n// meta@enable(unsafe)\n// meta@nobounds\nfor(let i=0;i<2;i++){} return 1; } f();',
             "directive after function-body use strict");
}

/* Broader result-invariant oracle: the hint must never change the result.
   Covers directives beyond the original set (SAFE and enabled-UNSAFE). */
function test_invariant_more() {
    sameResult(
        "let s=0; for(let i=0;i<32;i++) s+=i; s",
        "let s=0;\n// meta@autovec, meta@fixed, meta@trip(32)\nfor(let i=0;i<32;i++) s+=i; s",
        496, "loop autovec/fixed/trip");
    sameResult(
        "let s=0; for(let i=0;i<16;i++) s+=i; s",
        "// meta@enable(unsafe)\nlet s=0;\n// meta@prefetch(8), meta@stride1, meta@contiguous\nfor(let i=0;i<16;i++) s+=i; s",
        120, "loop prefetch/stride1/contiguous");
    sameResult(
        "function f(x){return x*2;} f(21)",
        "// meta@inline\nfunction f(x){return x*2;} f(21)",
        42, "function inline");
    sameResult(
        "function f(x){return x+1;} f(41)",
        "// meta@noinline\n// meta@hot\nfunction f(x){return x+1;} f(41)",
        42, "function noinline/hot");
    sameResult(
        "class C{constructor(a,b,c,d){this.a=a;this.b=b;this.c=c;this.d=d;}} new C(1,2,3,4).d",
        "// meta@preallocate_fields(4)\n// meta@soa\nclass C{constructor(a,b,c,d){this.a=a;this.b=b;this.c=c;this.d=d;}} new C(1,2,3,4).d",
        4, "class preallocate_fields/soa");
    sameResult(
        "function c(x){let t;if((x&1)===0)t='e';else t='o';return t;} c(4)+c(3)",
        "function c(x){let t;\n// meta@unlikely\nif((x&1)===0)t='e';else t='o';return t;} c(4)+c(3)",
        "eo", "if unlikely");
    sameResult(
        "function c(x){let t;if(x>0)t=1;else t=0;return t;} c(9)",
        "function c(x){let t;\n// meta@unpredictable\nif(x>0)t=1;else t=0;return t;} c(9)",
        1, "if unpredictable");
    sameResult(
        "function sw(x){switch(x&3){case 0:case 1:case 2:case 3:return x&3;}} sw(6)",
        "function sw(x){\n// meta@dense\nswitch(x&3){case 0:case 1:case 2:case 3:return x&3;}} sw(6)",
        2, "switch dense");
    sameResult(
        "// meta@enable(unsafe)\nfunction g(a){return a<<1;} g(50)",
        "// meta@enable(unsafe)\nfunction g(a){\n// meta@assume(a>=0), meta@invariant(a>0)\nreturn a<<1;} g(50)",
        100, "assume/invariant");
    sameResult(
        "// meta@enable(unsafe)\nfunction sum4(b){let s=0;for(let i=0;i<4;i++)s+=b[i];return s;} sum4([1,2,3,4])",
        "// meta@enable(unsafe)\nfunction sum4(b){\n// meta@length(b,4), meta@align(b,16)\nlet s=0;for(let i=0;i<4;i++)s+=b[i];return s;} sum4([1,2,3,4])",
        10, "length/align");
    sameResult(
        "let t=0; for(let i=0;i<5;i++) t++; t",
        "// meta@enable(unsafe)\nlet t=0;\n// meta@reuse, meta@pool(16), meta@volatile(t)\nfor(let i=0;i<5;i++) t++; t",
        5, "reuse/pool/volatile");
    sameResult(
        "// meta@enable(unsafe)\nfunction h(x){return x?1:0;} h(1)",
        "// meta@enable(unsafe)\nfunction h(x){\n// meta@noexcept, meta@nothrow\nreturn x?1:0;} h(1)",
        1, "noexcept/nothrow");
    sameResult(
        "// meta@enable(unsafe)\nfunction fin(){return 3;} fin()",
        "// meta@enable(unsafe)\n// meta@final\n// meta@pod\nclass K{constructor(){this.v=3;}}\nnew K().v",
        3, "class final/pod result");
}

/*--------------------------------------------------------------------------*/
/* BEHAVIORAL: meta@sealed / meta@fixed_layout — the one landed transform.
 * These classes are declared at this file's top level, so they are compiled
 * with the meta front-end active and the sealing transform really applies.
 */

// meta@sealed
class SealedBase { constructor(x) { this.x = x; } sum() { return this.x; } }

// meta@fixed_layout
class FixedLayout { constructor(a, b) { this.a = a; this.b = b; } }

class PlainClass { constructor(x) { this.x = x; } }

// meta@sealed
class SealedDerived extends SealedBase { constructor(x, y) { super(x); this.y = y; } }

class PlainDerivedOfSealed extends SealedBase { constructor(x, y) { super(x); this.y = y; } }

function test_sealed_behavior() {
    /* base sealed class: instance non-extensible, fields intact */
    const b = new SealedBase(5);
    assert(Object.isExtensible(b), false, "sealed base: !isExtensible");
    assert(b.x, 5, "sealed base: field intact");
    assert(b.sum(), 5, "sealed base: method works");
    /* sloppy add is silently rejected */
    b.newf = 123;
    assert(b.newf, undefined, "sealed base: sloppy add rejected");
    /* strict add throws TypeError */
    let threw = false;
    try { (function () { "use strict"; b.zz = 1; })(); }
    catch (e) { threw = e instanceof TypeError; }
    assert(threw, true, "sealed base: strict add throws TypeError");

    /* fixed_layout is an alias of sealed */
    const fl = new FixedLayout(1, 2);
    assert(Object.isExtensible(fl), false, "fixed_layout: !isExtensible");
    assert(fl.a + fl.b, 3, "fixed_layout: fields intact");

    /* plain (unsealed) class: extensible */
    const p = new PlainClass(5);
    assert(Object.isExtensible(p), true, "plain class: isExtensible");
    p.extra = 9;
    assert(p.extra, 9, "plain class: accepts new prop");

    /* derived sealed class: super() must NOT seal early — the derived ctor's
       own field is written, THEN the outermost instance is sealed. */
    const d = new SealedDerived(1, 2);
    assert(Object.isExtensible(d), false, "sealed derived: !isExtensible");
    assert(d.x, 1, "sealed derived: base field set through super()");
    assert(d.y, 2, "sealed derived: own field set before seal");

    /* sealed base + NON-sealed derived: sealing keys on the OUTERMOST
       constructor (new_target), so the instance stays extensible. */
    const dp = new PlainDerivedOfSealed(1, 2);
    assert(Object.isExtensible(dp), true, "sealed base, plain derived: isExtensible");
    assert(dp.x, 1, "plain derived: base field set");
    assert(dp.y, 2, "plain derived: own field set");
}

/* Result-invariant of the sealing transform itself: reading fields / calling
   methods yields identical results whether or not the class is sealed. */
function test_sealed_invariant() {
    sameResult(
        "class V{constructor(x,y,z){this.x=x;this.y=y;this.z=z;}s(){return this.x+this.y+this.z;}} new V(1,2,3).s()",
        "// meta@sealed\nclass V{constructor(x,y,z){this.x=x;this.y=y;this.z=z;}s(){return this.x+this.y+this.z;}} new V(1,2,3).s()",
        6, "sealed result invariant");
}

/*--------------------------------------------------------------------------*/

/* original coverage */
test_invariant_loop();
test_invariant_function();
test_invariant_class();
test_invariant_control_flow();
test_invariant_alloc();
test_invariant_variable();
test_strict_diagnostics();
test_strict_accepts_valid();
test_forms();

/* exhaustive additions */
test_table_is_exhaustive();
test_every_directive_gating();
test_wrong_construct();
test_arg_forms();
test_forms_extended();
test_adversarial_clean();
test_adversarial_strict_caught();
test_file_and_prologue();
test_invariant_more();

/* behavioral sealed transform */
test_sealed_behavior();
test_sealed_invariant();

console.log("test_meta: all tests passed");
