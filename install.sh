#!/usr/bin/env bash
#
# install.sh — install, reinstall, or upgrade the DynaJS runtime.
#
# This script always performs a CLEAN install: it (re)clones the source into a
# build cache, builds from scratch with the full native standard library, and
# OVERWRITES any previous `dynajs` binary at the install prefix. Running it again
# is how you upgrade (it pulls the latest source and rebuilds) or repair a broken
# install (it discards the old build tree entirely).
#
# Usage:
#   ./install.sh [options]
#   curl -fsSL <raw-url>/install.sh | bash
#   curl -fsSL <raw-url>/install.sh | bash -s -- --prefix "$HOME/.local"
#
# Options:
#   --prefix DIR    Install prefix (binary goes to DIR/bin). Default: /usr/local
#                   (falls back to $HOME/.local if /usr/local/bin is not writable
#                    and sudo is unavailable).
#   --repo URL      Git repository to clone. Default: the DynaJS upstream.
#   --ref REF       Branch, tag, or commit to build. Default: master.
#   --jobs N        Parallel build jobs. Default: number of CPUs.
#   --with-deps     Attempt to install missing build prerequisites via the
#                   system package manager (brew/apt/dnf/pacman/pkg). Off by
#                   default (dependency installs are left to you).
#   --uninstall     Remove the installed dynajs binary and exit.
#   --help          Show this help.
#
# Supported OS: macOS (Darwin), Linux, FreeBSD. Requires: git, make, and a C
# compiler (clang preferred, gcc accepted).

set -euo pipefail

# ----------------------------------------------------------------------------- config / defaults
REPO_URL_DEFAULT="https://github.com/corporatepiyush/dynascript.git"
REF_DEFAULT="master"
PREFIX_DEFAULT="/usr/local"
BINARY_NAME="dynajs"

REPO_URL="${DYNAJS_REPO:-$REPO_URL_DEFAULT}"
REF="${DYNAJS_REF:-$REF_DEFAULT}"
PREFIX="${DYNAJS_PREFIX:-$PREFIX_DEFAULT}"
JOBS=""
WITH_DEPS=0
DO_UNINSTALL=0

BUILD_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/dynajs-build"
SRC_DIR="$BUILD_ROOT/src"

# ----------------------------------------------------------------------------- pretty logging
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_BOLD=$(printf '\033[1m'); C_RED=$(printf '\033[31m'); C_GRN=$(printf '\033[32m')
    C_YLW=$(printf '\033[33m'); C_BLU=$(printf '\033[34m'); C_OFF=$(printf '\033[0m')
else
    C_BOLD=""; C_RED=""; C_GRN=""; C_YLW=""; C_BLU=""; C_OFF=""
fi
info()  { printf '%s==>%s %s\n'   "$C_BLU$C_BOLD" "$C_OFF" "$*"; }
step()  { printf '%s -%s %s\n'    "$C_GRN"        "$C_OFF" "$*"; }
warn()  { printf '%swarning:%s %s\n' "$C_YLW"     "$C_OFF" "$*" >&2; }
die()   { printf '%serror:%s %s\n'   "$C_RED$C_BOLD" "$C_OFF" "$*" >&2; exit 1; }

usage() { sed -n '3,45p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

# ----------------------------------------------------------------------------- arg parsing
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)    PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        --prefix=*)  PREFIX="${1#*=}"; shift ;;
        --repo)      REPO_URL="${2:?--repo needs a URL}"; shift 2 ;;
        --repo=*)    REPO_URL="${1#*=}"; shift ;;
        --ref)       REF="${2:?--ref needs a ref}"; shift 2 ;;
        --ref=*)     REF="${1#*=}"; shift ;;
        --jobs)      JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        --jobs=*)    JOBS="${1#*=}"; shift ;;
        --with-deps) WITH_DEPS=1; shift ;;
        --uninstall) DO_UNINSTALL=1; shift ;;
        -h|--help)   usage ;;
        *)           die "unknown option: $1 (try --help)" ;;
    esac
done

# ----------------------------------------------------------------------------- platform detection
UNAME_S="$(uname -s)"
UNAME_M="$(uname -m)"
case "$UNAME_S" in
    Darwin)  OS="macos" ;;
    Linux)   OS="linux" ;;
    FreeBSD) OS="freebsd" ;;
    *)       die "unsupported OS: $UNAME_S (supported: macOS, Linux, FreeBSD; on Windows use WSL)" ;;
esac

cpu_count() {
    if command -v nproc >/dev/null 2>&1; then nproc
    elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.ncpu
    else echo 4; fi
}
[ -n "$JOBS" ] || JOBS="$(cpu_count)"

# ----------------------------------------------------------------------------- uninstall path
resolve_bindir() {
    # An explicitly chosen prefix is ALWAYS honored (install_binary creates it,
    # using sudo if needed) — never silently redirected.
    if [ "$PREFIX" != "$PREFIX_DEFAULT" ]; then
        echo "$PREFIX/bin"; return
    fi
    # The default prefix (/usr/local): use it if writable/creatable or if sudo is
    # available; otherwise fall back to a per-user prefix that needs no privileges.
    if [ -w "$PREFIX/bin" ] 2>/dev/null || [ -w "$PREFIX" ] 2>/dev/null \
       || command -v sudo >/dev/null 2>&1; then
        echo "$PREFIX/bin"
    else
        echo "$HOME/.local/bin"
    fi
}

if [ "$DO_UNINSTALL" -eq 1 ]; then
    removed=0
    for dir in "$PREFIX/bin" "$HOME/.local/bin" "/usr/local/bin"; do
        target="$dir/$BINARY_NAME"
        if [ -e "$target" ]; then
            info "removing $target"
            if [ -w "$dir" ]; then rm -f "$target"
            elif command -v sudo >/dev/null 2>&1; then sudo rm -f "$target"
            else die "cannot remove $target (no write permission and no sudo)"; fi
            removed=1
        fi
    done
    [ "$removed" -eq 1 ] && step "uninstalled." || warn "no $BINARY_NAME binary found to remove."
    exit 0
fi

# ----------------------------------------------------------------------------- dependency checks
PKG_INSTALL=""
detect_pkg_mgr() {
    if   command -v brew   >/dev/null 2>&1; then PKG_INSTALL="brew install"
    elif command -v apt-get>/dev/null 2>&1; then PKG_INSTALL="sudo apt-get install -y"
    elif command -v dnf    >/dev/null 2>&1; then PKG_INSTALL="sudo dnf install -y"
    elif command -v yum    >/dev/null 2>&1; then PKG_INSTALL="sudo yum install -y"
    elif command -v pacman >/dev/null 2>&1; then PKG_INSTALL="sudo pacman -S --noconfirm"
    elif command -v pkg    >/dev/null 2>&1; then PKG_INSTALL="sudo pkg install -y"
    fi
}

# Pick the C compiler: clang preferred (the project's primary toolchain), gcc accepted.
CC_BIN=""
select_compiler() {
    if   command -v clang >/dev/null 2>&1; then CC_BIN="clang"
    elif command -v gcc   >/dev/null 2>&1; then CC_BIN="gcc"
    fi
}

install_dep_hint() {
    # $1 = human tool name, $2 = brew pkg, $3 = apt pkg, $4 = dnf pkg, $5 = pacman, $6 = pkg(bsd)
    case "$PKG_INSTALL" in
        brew*)   echo "$2" ;;
        *apt*)   echo "$3" ;;
        *dnf*|*yum*) echo "$4" ;;
        *pacman*)echo "$5" ;;
        *pkg*)   echo "$6" ;;
        *)       echo "$2" ;;
    esac
}

ensure_deps() {
    detect_pkg_mgr
    local missing=""
    command -v git  >/dev/null 2>&1 || missing="$missing git"
    command -v make >/dev/null 2>&1 || missing="$missing make"
    select_compiler
    [ -n "$CC_BIN" ] || missing="$missing compiler"

    if [ -z "$missing" ]; then
        step "prerequisites present: git, make, $CC_BIN"
        return
    fi

    # macOS: the compiler + git + make all come from the Command Line Tools.
    if [ "$OS" = "macos" ] && printf '%s' "$missing" | grep -q 'compiler\|git\|make'; then
        if ! xcode-select -p >/dev/null 2>&1; then
            warn "Xcode Command Line Tools are required (they provide clang, git, make)."
            info "installing Command Line Tools (a GUI prompt may appear)..."
            xcode-select --install 2>/dev/null || true
            die "re-run this script after the Command Line Tools finish installing."
        fi
    fi

    if [ "$WITH_DEPS" -eq 1 ] && [ -n "$PKG_INSTALL" ]; then
        info "installing missing prerequisites via: $PKG_INSTALL"
        local pkgs=""
        printf '%s' "$missing" | grep -q git      && pkgs="$pkgs $(install_dep_hint git git git git git git)"
        printf '%s' "$missing" | grep -q make     && pkgs="$pkgs $(install_dep_hint make make make make make gmake)"
        printf '%s' "$missing" | grep -q compiler && pkgs="$pkgs $(install_dep_hint clang llvm clang clang clang llvm)"
        # shellcheck disable=SC2086
        $PKG_INSTALL $pkgs || die "dependency install failed; install them manually and re-run."
        select_compiler
    else
        warn "missing prerequisites:$missing"
        if [ -n "$PKG_INSTALL" ]; then
            info "install them with:  $PKG_INSTALL git make clang     (or re-run with --with-deps)"
        else
            info "install git, make, and clang (or gcc) with your system package manager, then re-run."
        fi
        die "prerequisites missing."
    fi
}

# ----------------------------------------------------------------------------- clone + build
fetch_source() {
    info "fetching source ($REF) from $REPO_URL"
    rm -rf "$SRC_DIR"                        # override any previous build tree entirely
    mkdir -p "$BUILD_ROOT"
    git clone --depth 1 --branch "$REF" "$REPO_URL" "$SRC_DIR" 2>/dev/null \
        || git clone "$REPO_URL" "$SRC_DIR"  # fall back for a commit sha / non-branch ref
    ( cd "$SRC_DIR" && git checkout --quiet "$REF" 2>/dev/null || true )
    step "source at $SRC_DIR ($(cd "$SRC_DIR" && git rev-parse --short HEAD))"
}

build() {
    info "building DynaJS with the native standard library ($JOBS jobs)"
    local mk_args="CONFIG_NATIVE_MODULES=y"
    # On Linux the Makefile defaults to gcc; force clang if that's what we found.
    if [ "$OS" = "linux" ] && [ "$CC_BIN" = "clang" ]; then
        mk_args="$mk_args CONFIG_CLANG=y"
    fi
    ( cd "$SRC_DIR"
      make clean >/dev/null 2>&1 || true     # flag changes need a clean tree
      # shellcheck disable=SC2086
      make $mk_args -j"$JOBS" )
    [ -x "$SRC_DIR/$BINARY_NAME" ] || die "build did not produce $BINARY_NAME (see output above)."
    step "built $SRC_DIR/$BINARY_NAME"
}

# ----------------------------------------------------------------------------- install (overwrite)
install_binary() {
    local bindir; bindir="$(resolve_bindir)"
    local use_sudo=0
    if [ ! -d "$bindir" ]; then
        mkdir -p "$bindir" 2>/dev/null || { command -v sudo >/dev/null 2>&1 && sudo mkdir -p "$bindir"; } \
            || die "cannot create $bindir"
    fi
    if [ ! -w "$bindir" ]; then
        if command -v sudo >/dev/null 2>&1; then use_sudo=1
        else die "no write permission to $bindir and sudo is unavailable; re-run with --prefix \"\$HOME/.local\""; fi
    fi

    local dest="$bindir/$BINARY_NAME"
    info "installing to $dest (overwriting any previous version)"
    if [ "$use_sudo" -eq 1 ]; then
        sudo install -m 0755 "$SRC_DIR/$BINARY_NAME" "$dest"
    else
        install -m 0755 "$SRC_DIR/$BINARY_NAME" "$dest"
    fi
    INSTALLED_PATH="$dest"; INSTALLED_BINDIR="$bindir"
    step "installed."
}

verify_and_report() {
    "$INSTALLED_PATH" -e 'print("ok")' >/dev/null 2>&1 || die "installed binary failed to run."
    info "verifying native standard library"
    "$INSTALLED_PATH" -e 'import("dynajs:crypto").then(c=>print(" dynajs:crypto sha256(\"dynajs\")=", c.sha256Hex("dynajs").slice(0,16)+"..."))' 2>/dev/null \
        || warn "native modules did not load (unexpected — build with CONFIG_NATIVE_MODULES=y)."

    printf '\n%s%sDynaJS installed successfully.%s\n' "$C_GRN" "$C_BOLD" "$C_OFF"
    printf '  binary : %s\n' "$INSTALLED_PATH"
    "$INSTALLED_PATH" --help 2>&1 | head -1 | sed 's/^/  version: /'
    case ":$PATH:" in
        *":$INSTALLED_BINDIR:"*) : ;;
        *) printf '\n%s%s is not on your PATH.%s Add this to your shell profile:\n' "$C_YLW" "$INSTALLED_BINDIR" "$C_OFF"
           printf '    export PATH="%s:$PATH"\n' "$INSTALLED_BINDIR" ;;
    esac
    printf '\nTry it:\n    %s -e '\''import("dynajs:uuid").then(u=>print(u.v7()))'\''\n\n' "$BINARY_NAME"
}

# ----------------------------------------------------------------------------- main
main() {
    info "DynaJS installer — OS=$OS arch=$UNAME_M prefix=$PREFIX ref=$REF"
    ensure_deps
    fetch_source
    build
    install_binary
    verify_and_report
}
main
