#!/usr/bin/env bash
#
# dynascript — meta@ directive diagnostic / dump assertions.
#
# Exercises the observable stderr behaviour of the meta@ front-end: the
# DYNASCRIPT_META_DUMP trace (name, arguments, safety tier), the warning paths
# (unknown / illegal placement / unsafe-without-enable / malformed args) and the
# meta@strict escalation to a non-zero exit. Complements tests/test_meta.js
# (which covers the result-invariant and the throwable strict errors).
#
# Usage:  tests/meta/run_dump_tests.sh          # uses ./qjs
#         QJS=/path/to/qjs tests/meta/run_dump_tests.sh
#
set -u
QJS="${QJS:-./qjs}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0

# run the source in $1, capture stderr; $2 = "dump" to enable DYNASCRIPT_META_DUMP
_run() {
    printf '%s\n' "$1" > "$TMP/src.js"
    if [ "${2:-}" = "dump" ]; then
        DYNASCRIPT_META_DUMP=1 "$QJS" "$TMP/src.js" >/dev/null 2>"$TMP/err"
    else
        "$QJS" "$TMP/src.js" >/dev/null 2>"$TMP/err"
    fi
    echo $? > "$TMP/ec"
}

ok()   { pass=$((pass+1)); }
bad()  { fail=$((fail+1)); echo "FAIL: $1"; echo "   --- stderr ---"; sed 's/^/   /' "$TMP/err"; }

# assert stderr contains the (fixed-string) pattern $2 after running $1 with dump on
expect_dump() { _run "$1" dump; if grep -qF -- "$2" "$TMP/err"; then ok; else bad "dump wanted [$2]"; fi; }
# assert stderr contains the warning pattern (dump off)
expect_warn() { _run "$1"; if grep -qF -- "$2" "$TMP/err"; then ok; else bad "warn wanted [$2]"; fi; }
# assert stderr is empty (no warnings) and exit 0
expect_clean() { _run "$1"; if [ -s "$TMP/err" ] || [ "$(cat "$TMP/ec")" != 0 ]; then bad "expected clean: $3"; else ok; fi; }
# assert the run exits non-zero (strict error)
expect_fail() { _run "$1"; if [ "$(cat "$TMP/ec")" != 0 ]; then ok; else bad "expected non-zero exit: $2"; fi; }
# assert stderr does NOT contain $2
expect_absent() { _run "$1" dump; if grep -qF -- "$2" "$TMP/err"; then bad "unexpected [$2]: $3"; else ok; fi; }
# assert stderr contains EXACTLY the line $2 after the temp source path is
# normalized to FILE (line number preserved) — the strongest dump oracle.
expect_dump_line() {
    _run "$1" dump
    norm_line=$(sed 's# @ [^ ]*:# @ FILE:#' "$TMP/err")
    if printf '%s\n' "$norm_line" | grep -qxF -- "$2"; then ok; else
        bad "dump line wanted [$2]"; printf '%s\n' "$norm_line" | sed 's/^/      got: /'
    fi
}
# adversarial: the process must exit normally (0 = ran, 1 = clean SyntaxError),
# never a signal (128+n = crash/abort). Used to prove no crash on bad input.
expect_nocrash() {
    _run "$1"; nc_ec=$(cat "$TMP/ec")
    if [ "$nc_ec" -le 1 ]; then ok; else bad "crash (exit $nc_ec): ${2:-}"; fi
}

echo "== capture + tier =="
expect_dump  $'// meta@unroll(4)\nfor(let i=0;i<2;i++){}'                 'meta: unroll(4)'
expect_dump  $'// meta@unroll(4)\nfor(let i=0;i<2;i++){}'                 '[SAFE]'
expect_dump  $'// meta@enable(unsafe)\n// meta@int32\nfor(let i=0;i<2;i++){}' 'meta: int32'
expect_dump  $'// meta@enable(unsafe)\n// meta@int32\nfor(let i=0;i<2;i++){}' '[UNSAFE]'
expect_dump  $'// meta@sealed\nclass C{}'                                 'meta: sealed'

echo "== argument parsing =="
expect_dump  $'// meta@enable(unsafe)\n// meta@range(x,0,255)\nlet x=1;'  'meta: range(x,0,255)'
expect_dump  $'// meta@enable(unsafe)\n// meta@type(v,i32)\nlet v=1;'     'meta: type(v,i32)'
expect_dump  $'// meta@reduce(sum)\nfor(let i=0;i<2;i++){}'               'meta: reduce(sum)'
expect_dump  $'// meta@enable(unsafe)\n// meta@align(b,16)\nlet b=0;'     'meta: align(b,16)'
expect_dump  $'// meta@enable(unsafe)\n// meta@assume(x >= 0)\nlet x=1;'  'meta: assume(x,0)'   # operator skipped, target+int kept

echo "== warnings =="
expect_warn  $'// meta@bogus_directive\n1;'                              "unknown meta directive 'meta@bogus_directive'"
expect_warn  $'// meta@sealed\nfor(let i=0;i<1;i++){}'                    "'meta@sealed' applies to a class"
expect_warn  $'// meta@unroll(4)\nclass C{}'                              "'meta@unroll' applies to a loop"
expect_warn  $'// meta@int32\nfor(let i=0;i<1;i++){}'                     "requires a '// meta@enable(unsafe)'"
expect_warn  $'// meta@enable(unsafe)\n// meta@range\nlet x=1;'           "expects a variable target"
expect_warn  $'// meta@enable(unsafe)\n// meta@range(x)\nlet x=1;'        "expects 2..2 numeric argument"
expect_warn  $'// meta@enable(bogus)\n1;'                                 "meta@enable expects 'unsafe'"

echo "== gating: enable(unsafe) suppresses the warning =="
expect_clean $'// meta@enable(unsafe)\n// meta@int32\nfor(let i=0;i<1;i++){}' '' "int32 after enable"
expect_absent $'// meta@enable(unsafe)\n// meta@nobounds\nfor(let i=0;i<1;i++){}' 'requires' "nobounds gated"

echo "== strict escalation (non-zero exit) =="
expect_fail  $'// meta@strict\n// meta@bogus\n1;'                          "unknown under strict"
expect_fail  $'// meta@strict\n// meta@sealed\nfor(let i=0;i<1;i++){}'     "illegal under strict"
expect_fail  $'// meta@strict\n// meta@int32\nfor(let i=0;i<1;i++){}'      "ungated under strict"
expect_clean $'// meta@strict\n// meta@enable(unsafe)\n// meta@int32\nfor(let i=0;i<1;i++){}' '' "strict + valid"

echo "== forms: block, stacked, no-space, dump directive =="
expect_dump  $'/* meta@unroll(2) */\nfor(let i=0;i<2;i++){}'              'meta: unroll(2)'
expect_dump  $'/*\n meta@unroll(2)\n meta@reduce(sum)\n*/\nfor(let i=0;i<2;i++){}' 'meta: reduce(sum)'
expect_dump  $'//meta@unroll(3)\nfor(let i=0;i<2;i++){}'                  'meta: unroll(3)'
# a file-level meta@dump enables the trace without the env var
_run $'// meta@dump\n// meta@sealed\nclass C{}'; if grep -qF 'meta: sealed' "$TMP/err"; then ok; else bad "meta@dump self-enable"; fi

echo "== every registered directive name is recognized (no 'unknown') =="
names="unroll autovec int32 float64 nobounds nopoll reduce trip fixed stride1 contiguous prefetch independent parallel \
inline noinline pure nosideeffects arena scoped_alloc noalloc memoize tailcall monomorphic \
sealed fixed_layout pod final preallocate_fields soa noproto \
likely unlikely unpredictable jumptable dense assume invariant noexcept nothrow \
preallocate reuse pool stack noescape transient weak hot cold \
range nonnull nonzero type const frozen align length init volatile \
enable strict dump"
for n in $names; do
    # place on a loop with unsafe enabled: any known directive avoids the 'unknown' warning
    _run "// meta@enable(unsafe)
// meta@$n
for(let i=0;i<1;i++){}"
    if grep -qF "unknown meta directive 'meta@$n'" "$TMP/err"; then bad "name not recognized: $n"; else ok; fi
done

echo "== exact dump lines (path normalized to FILE; every argument shape) =="
expect_dump_line $'// meta@unroll(4)\nfor(let i=0;i<2;i++){}'                              'meta: unroll(4) @ FILE:1 [SAFE]'
expect_dump_line $'// meta@autovec\nfor(let i=0;i<2;i++){}'                                'meta: autovec @ FILE:1 [SAFE]'
expect_dump_line $'// meta@enable(unsafe)\n// meta@int32\nlet x=1;'                        'meta: int32 @ FILE:2 [UNSAFE]'
expect_dump_line $'// meta@sealed\nclass C{}'                                              'meta: sealed @ FILE:1 [SAFE]'
expect_dump_line $'// meta@reduce(sum)\nfor(let i=0;i<2;i++){}'                            'meta: reduce(sum) @ FILE:1 [SAFE]'
expect_dump_line $'// meta@trip(-5)\nfor(let i=0;i<2;i++){}'                               'meta: trip(-5) @ FILE:1 [SAFE]'
expect_dump_line $'// meta@trip(999999999999999999999999999999)\nfor(let i=0;i<2;i++){}'  'meta: trip(9223372036854775807) @ FILE:1 [SAFE]'
expect_dump_line $'// meta@preallocate(10)\nlet z=[];'                                     'meta: preallocate(10) @ FILE:1 [SAFE]'
expect_dump_line $'// meta@enable(unsafe)\n// meta@range(x,0,255)\nlet x=1;'               'meta: range(x,0,255) @ FILE:2 [UNSAFE]'
expect_dump_line $'// meta@enable(unsafe)\n// meta@type(v,i32)\nlet v=1;'                  'meta: type(v,i32) @ FILE:2 [UNSAFE]'
expect_dump_line $'// meta@enable(unsafe)\n// meta@align(b,16)\nlet b=0;'                  'meta: align(b,16) @ FILE:2 [UNSAFE]'
expect_dump_line $'// meta@enable(unsafe)\n// meta@monomorphic(int32, int32)\nfunction f(){}'  'meta: monomorphic(int32,int32) @ FILE:2 [UNSAFE]'
expect_dump_line $'// meta@enable(unsafe)\n// meta@assume(x >= 0)\nlet x=1;'               'meta: assume(x,0) @ FILE:2 [UNSAFE]'  # operator skipped, target+int kept
expect_dump_line $'// meta@enable(unsafe)\n// meta@nonnull(b)\nlet b=0;'                   'meta: nonnull(b) @ FILE:2 [UNSAFE]'
# comma-separated directives each emit their own dump line at the same source line
expect_dump_line $'// meta@enable(unsafe)\n// meta@nobounds, meta@nopoll\nfor(let i=0;i<2;i++){}'  'meta: nobounds @ FILE:2 [UNSAFE]'
expect_dump_line $'// meta@enable(unsafe)\n// meta@nobounds, meta@nopoll\nfor(let i=0;i<2;i++){}'  'meta: nopoll @ FILE:2 [UNSAFE]'

echo "== every construct directive dumps with the correct safety tier =="
nl=$'\n'
# tier|directive-text(no internal spaces)|host — the 59 construct-attached
# directives (file-level enable/strict/dump apply immediately and never dump).
dirtable='SAFE|unroll(4)|for(let i=0;i<2;i++){}
SAFE|autovec|for(let i=0;i<2;i++){}
UNSAFE|int32|let _s=1;
UNSAFE|float64|let _s=1;
UNSAFE|nobounds|for(let i=0;i<2;i++){}
UNSAFE|nopoll|for(let i=0;i<2;i++){}
SAFE|reduce(sum)|for(let i=0;i<2;i++){}
SAFE|trip(4)|for(let i=0;i<2;i++){}
SAFE|fixed|for(let i=0;i<2;i++){}
UNSAFE|stride1|for(let i=0;i<2;i++){}
UNSAFE|contiguous|for(let i=0;i<2;i++){}
SAFE|prefetch(16)|for(let i=0;i<2;i++){}
UNSAFE|independent|for(let i=0;i<2;i++){}
UNSAFE|parallel|for(let i=0;i<2;i++){}
SAFE|inline|function _f(){}
SAFE|noinline|function _f(){}
UNSAFE|pure|function _f(){}
UNSAFE|nosideeffects|function _f(){}
UNSAFE|arena(4096)|function _f(){}
UNSAFE|scoped_alloc(2048)|function _f(){}
UNSAFE|noalloc|function _f(){}
SAFE|memoize|function _f(){}
SAFE|tailcall|function _f(){}
UNSAFE|monomorphic(i32,i32)|function _f(){}
SAFE|sealed|class _C{}
SAFE|fixed_layout|class _C{}
UNSAFE|pod|class _C{}
UNSAFE|final|class _C{}
SAFE|preallocate_fields(4)|class _C{}
SAFE|soa|class _C{}
UNSAFE|noproto|class _C{}
SAFE|likely|if(1){}
SAFE|unlikely|if(1){}
SAFE|unpredictable|if(1){}
SAFE|jumptable|switch(1){}
SAFE|dense|switch(1){}
UNSAFE|assume(x)|let _s=1;
UNSAFE|invariant(x)|let _s=1;
UNSAFE|noexcept|let _s=1;
UNSAFE|nothrow|let _s=1;
SAFE|preallocate(8)|let _s=1;
UNSAFE|reuse|let _s=1;
UNSAFE|pool(64)|let _s=1;
UNSAFE|stack|let _s=1;
UNSAFE|noescape|let _s=1;
SAFE|transient|let _s=1;
SAFE|weak|let _s=1;
SAFE|hot|for(let i=0;i<2;i++){}
SAFE|cold|for(let i=0;i<2;i++){}
UNSAFE|range(x,0,9)|let _s=1;
UNSAFE|nonnull(b)|let _s=1;
UNSAFE|nonzero(q)|let _s=1;
UNSAFE|type(v,i32)|let _s=1;
UNSAFE|const(k)|let _s=1;
UNSAFE|frozen(o)|let _s=1;
UNSAFE|align(b,16)|let _s=1;
UNSAFE|length(a,4)|let _s=1;
UNSAFE|init(v)|let _s=1;
SAFE|volatile(t)|let _s=1;'
tier_rows=0
while IFS='|' read -r tier dir host; do
    [ -z "$tier" ] && continue
    tier_rows=$((tier_rows+1))
    en=""; [ "$tier" = "UNSAFE" ] && en="// meta@enable(unsafe)$nl"
    src="$en// meta@$dir$nl$host"
    expect_dump "$src" "meta: $dir"   # captured with its arguments
    expect_dump "$src" "[$tier]"      # dumped with the correct safety tier
done <<< "$dirtable"
if [ "$tier_rows" -eq 59 ]; then ok; else bad "dirtable must list all 59 construct directives (has $tier_rows)"; fi

echo "== adversarial / malformed input: no crash (exit 0 or clean SyntaxError) =="
expect_nocrash $'// meta@enable(unsafe)\n// meta@range(x,\nlet x=1;'                "truncated arg list"
expect_nocrash '/* meta@sealed'                                                    "unterminated block comment"
expect_nocrash '// meta@'                                                          "meta@ at buffer end"
expect_nocrash $'// meta@   '                                                       "meta@ then whitespace"
expect_nocrash $'// meta@()\n1;'                                                    "empty directive name"
expect_nocrash $'// meta@enable(unsafe)\n// meta@type(caf\xc3\xa9, i32)\nlet x=1;'  "unicode in args"
expect_nocrash $'// meta@enable(unsafe)\n// meta@assume(a>=b && c!=d)\nlet a=1;'    "stray operators"
expect_nocrash $'// mentions meta@unroll not as a prefix\n1;'                       "meta@ mid-comment"
expect_nocrash $'// meta@enable(unsafe)\n// meta@nonnull(***)\nlet x=1;'            "operators-only argument"
expect_nocrash $'// meta@enable(unsafe)\n// meta@range(((x)),0,9)\nlet x=1;'        "nested parens"
expect_nocrash $'// meta@strict\n// meta@bogus\n// meta@sealed\nclass C{}'          "strict abort mid-set"
# CRLF line endings (written raw, bypassing _run's added \n)
printf '// meta@unroll(4)\r\nfor(let i=0;i<2;i++){}\r\n' > "$TMP/src.js"
"$QJS" "$TMP/src.js" >/dev/null 2>/dev/null; cr_ec=$?
if [ "$cr_ec" -le 1 ]; then ok; else bad "crash (exit $cr_ec): CRLF line endings"; fi

echo "== meta@sealed: behavior + serialization round-trip (QJS_BYTECODE_CACHE) =="
cat > "$TMP/seal.js" <<'JS'
// meta@sealed
class Base { constructor(x){ this.x = x; } }
class Plain { constructor(x){ this.x = x; } }
// meta@sealed
class Der extends Base { constructor(x,y){ super(x); this.y = y; } }
class PDer extends Base { constructor(x,y){ super(x); this.y = y; } }
const b = new Base(5), pl = new Plain(5), d = new Der(1,2), pd = new PDer(1,2);
let add = false;
try { (function(){ "use strict"; b.z = 1; })(); } catch (e) { add = (e instanceof TypeError); }
console.log([Object.isExtensible(b), b.x, add, Object.isExtensible(pl),
             Object.isExtensible(d), d.x, d.y, Object.isExtensible(pd)].join(","));
JS
SEAL_EXP="false,5,true,true,false,1,2,true"
rm -f "$TMP/seal.js.qbc"
seal_plain=$("$QJS" "$TMP/seal.js" 2>/dev/null)
[ "$seal_plain" = "$SEAL_EXP" ] && ok || bad "sealed (no cache): got [$seal_plain] want [$SEAL_EXP]"
seal_cold=$(QJS_BYTECODE_CACHE=1 "$QJS" "$TMP/seal.js" 2>/dev/null)
[ -f "$TMP/seal.js.qbc" ] && ok || bad "sealed cold: .qbc cache not written"
[ "$seal_cold" = "$SEAL_EXP" ] && ok || bad "sealed cold: got [$seal_cold]"
seal_warm=$(QJS_BYTECODE_CACHE=1 "$QJS" "$TMP/seal.js" 2>/dev/null)
[ "$seal_warm" = "$SEAL_EXP" ] && ok || bad "sealed warm (from .qbc): got [$seal_warm] want [$SEAL_EXP]"
# directive on a non-class statement warns; after a use-strict prologue there is none
expect_warn  $'// meta@sealed\nlet z=1;'                     "'meta@sealed' applies to a class"
expect_clean $'"use strict";\n// meta@sealed\nclass C{}'     '' "sealed after use strict prologue"
expect_clean $'function f(){ "use strict";\n// meta@enable(unsafe)\n// meta@int32\nreturn 1; }\nf();' '' "directive after fn-body use strict"

echo "----------------------------------------"
echo "meta dump/diagnostic tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
