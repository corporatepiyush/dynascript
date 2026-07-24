#!/usr/bin/env bash
# dev.sh — one entry point for building, testing and profiling dynascript, so we
# stop hand-writing shell. Output is terse: "<stage>: ok" or "FAIL: <why>" and a
# nonzero exit on any failure. Run `./dev.sh` for the command list.
#
#   ./dev.sh build [MAKEARGS...]     0-warning build (fails on any warning/error)
#   ./dev.sh run   FILE [args...]    build (incremental) then run a JS file
#   ./dev.sh test                    make test
#   ./dev.sh asan  FILE|test         ASan build + run (auto `make clean` on cfg switch)
#   ./dev.sh ubsan FILE|test         UBSan build + run
#   ./dev.sh t262  [SUBTREE]         run test262 (subtree, else full baseline check)
#   ./dev.sh bench FILE [args...]    CONFIG_NATIVE build + run
#   ./dev.sh rss   FILE [N...]       peak-RSS-plateau leak check (FILE reads scriptArgs[1]=N)
#   ./dev.sh openlibm FILE|test      CONFIG_OPENLIBM build + run
#   ./dev.sh amd64                   docker x86 SIMD verify (Dockerfile.amd64)
#   ./dev.sh gate  [TEST.js...]      full proof: 0-warn + ASan + UBSan + make test + test262
#   ./dev.sh clean
set -uo pipefail
cd "$(dirname "$0")" || exit 2

JOBS=$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )
BASELINE="${T262_BASELINE:-58/83744}"
CONF=tools/test262.conf
STAMP=.obj/.dev_cfg

die(){ echo "FAIL: $*" >&2; exit 1; }
have(){ command -v "$1" >/dev/null 2>&1; }
need_file(){ [ -f "$1" ] || die "no such file: $1"; }

# _build "<make args>": clean iff the config changed since last build; fail on
# any compiler error or (non-pre-existing) warning. Silent on success.
_build(){
  # NB: never pre-create .obj here -- that satisfies the Makefile's $(OBJDIR)
  # order-only prereq and skips its `mkdir .obj/examples .obj/tests`, racing a
  # parallel build after a clean. Let make own the dir; write the stamp after.
  local cfg="$*" log warn
  [ "$(cat "$STAMP" 2>/dev/null || true)" = "$cfg" ] || make clean >/dev/null 2>&1 || true
  log=$(mktemp)
  if ! make -j"$JOBS" $cfg >"$log" 2>&1; then
    echo "FAIL: build [$cfg]"; grep -iE "error:" "$log" | head -8; rm -f "$log"; exit 1
  fi
  echo "$cfg" >"$STAMP" 2>/dev/null || true
  warn=$(grep -iE "warning:" "$log" | grep -v "loop not vectorized" || true)
  rm -f "$log"
  [ -z "$warn" ] || { echo "FAIL: build warnings [$cfg]"; echo "$warn" | head -8; exit 1; }
}

# run a JS file or the `test` target under an optional sanitizer env
_run_target(){   # $1=target(file|test)  rest=args
  local t="$1"; shift || true
  if [ "$t" = "test" ]; then
    make test >/dev/null 2>&1 || die "make test"
    echo "make test: ok"
  else
    need_file "$t"
    ./dynajs "$t" "$@" || die "run $t"
  fi
}

_t262_full(){
  [ -f "$CONF" ] || die "$CONF missing (run: make test2-bootstrap)"
  local got
  got=$(./run-test262 -c "$CONF" -a 2>&1 | grep -oE '[0-9]+/[0-9]+ errors' | grep -oE '[0-9]+/[0-9]+')
  [ -n "$got" ] || die "test262 produced no Result line"
  if [ "$got" = "$BASELINE" ]; then echo "test262: $got ok"
  else echo "FAIL: test262 baseline drift: got $got want $BASELINE"; exit 1; fi
}

cmd="${1:-}"; shift 2>/dev/null || true
case "$cmd" in
  build)    _build "$@"; echo "build: ok" ;;

  run)      [ $# -ge 1 ] || die "usage: run FILE [args]"; _build ""; _run_target "$@" ;;

  test)     _build ""; make test || die "make test" ;;

  asan)     [ $# -ge 1 ] || die "usage: asan FILE|test"
            _build "CONFIG_ASAN=y"
            ASAN_OPTIONS=detect_leaks=0 _run_target "$@"; echo "asan: ok" ;;

  ubsan)    [ $# -ge 1 ] || die "usage: ubsan FILE|test"
            _build "CONFIG_UBSAN=y"
            UBSAN_OPTIONS=halt_on_error=1 _run_target "$@"; echo "ubsan: ok" ;;

  openlibm) [ $# -ge 1 ] || die "usage: openlibm FILE|test"
            [ -f third_party/openlibm/libopenlibm.a ] || \
              die "third_party/openlibm/libopenlibm.a missing (clone+make it; see Makefile)"
            _build "CONFIG_OPENLIBM=y"; _run_target "$@"; echo "openlibm: ok" ;;

  bench)    [ $# -ge 1 ] || die "usage: bench FILE [args]"
            _build "CONFIG_NATIVE=y"; need_file "$1"; ./dynajs "$@" || die "bench" ;;

  t262)     _build ""
            if [ $# -ge 1 ]; then
              [ -f "$CONF" ] || die "$CONF missing (run: make test2-bootstrap)"
              ./run-test262 -c "$CONF" -a -d "$1" 2>&1 | grep -E "^Result:" || die "test262 subtree"
            else _t262_full; fi ;;

  rss)      [ $# -ge 1 ] || die "usage: rss FILE [N...]"; f="$1"; shift
            need_file "$f"; _build ""
            [ $# -ge 1 ] || set -- 20000 100000 500000
            for N in "$@"; do
              if have /usr/bin/time && /usr/bin/time -l true >/dev/null 2>&1; then
                r=$(/usr/bin/time -l ./dynajs "$f" "$N" 2>&1 | awk '/maximum resident/{print $1}')
              else
                r=$(/usr/bin/time -v ./dynajs "$f" "$N" 2>&1 | awk -F': ' '/Maximum resident/{print $2"K"}')
              fi
              echo "N=$N peakRSS=${r:-?}"
            done
            echo "rss: flat across N => no leak" ;;

  amd64)    have docker || die "docker not installed"
            [ -f docker/Dockerfile.amd64 ] || die "docker/Dockerfile.amd64 missing"
            docker build --platform linux/amd64 -f docker/Dockerfile.amd64 . || die "docker amd64 build"
            echo "amd64: ok" ;;

  gate)     _build ""; echo "build: ok"
            for t in "$@"; do _run_target "$t"; done
            [ $# -gt 0 ] && echo "smoke: ok"
            _build "CONFIG_ASAN=y";  for t in "$@"; do ASAN_OPTIONS=detect_leaks=0 _run_target "$t"; done; echo "asan: ok"
            _build "CONFIG_UBSAN=y"; for t in "$@"; do UBSAN_OPTIONS=halt_on_error=1 _run_target "$t"; done; echo "ubsan: ok"
            _build ""; make test >/dev/null 2>&1 || die "make test"; echo "make test: ok"
            _t262_full
            echo "gate: ok" ;;

  clean)    make clean >/dev/null 2>&1; rm -f "$STAMP"; echo "clean: ok" ;;

  ""|-h|--help|help)
            sed -n '2,20p' "$0" ;;
  *)        die "unknown command: $cmd (try ./dev.sh help)" ;;
esac
