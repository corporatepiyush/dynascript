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

echo "----------------------------------------"
echo "meta dump/diagnostic tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
