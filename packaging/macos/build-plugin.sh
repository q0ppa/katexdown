#!/usr/bin/env bash
#
# katexdown - build the macOS plugin locally for Apple Silicon (arm64).
#
# WHAT IT PRODUCES
#   <out-dir>/katexdown-<version>-macos-<arch>.so
#
# PREREQUISITES
#   macOS on Apple Silicon (arm64), with Homebrew:
#       brew install cmake ninja pkgconf qt gettext bison
#
#   A KF6 prefix built against Homebrew Qt (build-kf6.sh builds it once locally).

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd -P)"
repo_root="$(cd "$script_dir/../.." && pwd -P)"
# shellcheck source=lib-qt.sh
. "$script_dir/lib-qt.sh"

PROG="build-plugin.sh"

log()  { printf '%s: %s\n' "$PROG" "$*" >&2; }
info() { printf '%s: %s\n' "$PROG" "$*" >&2; }
warn() { printf '%s: warning: %s\n' "$PROG" "$*" >&2; }
die()  { printf '%s: error: %s\n' "$PROG" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
${PROG} - build the Katexdown plugin for macOS (arm64)

Usage: ${PROG} [options]

Options:
  --build-dir DIR       CMake build directory (default ./build)
  --out-dir DIR         where the release .so is written (default ./dist)
  --kf6-prefix DIR      KF6 install prefix (default: a Qt-specific ~/.local/katexdown-kf6[-<Qt version>])
  --qt-prefix DIR       Qt prefix to build against (default: auto-detect Kate and provision one)
  --with-tests          also build the test binaries
  --no-build-kf6        never run build-kf6.sh; fail when the KF6 prefix is incomplete
  --clean               delete the build directory first
  --jobs N              parallel build jobs (default: CPU count)
  --print-qt-minor      print the detected Qt minor and exit
  -h, --help            this text

Environment:
  KATEXDOWN_KF6_PREFIX  same as --kf6-prefix
  KATEXDOWN_QT_PREFIX   same as --qt-prefix
  KATEXDOWN_QT_ROOT     where provision-qt.sh installs Qt (default ~/Qt)
EOF
}

host_arch() {
    local m
    m="$(uname -m)"
    if [ "$m" = "x86_64" ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || printf '0')" = "1" ]; then
        m="arm64"
    fi
    printf '%s' "$m"
}

require_macos_arm64() {
    [ "$(uname -s)" = "Darwin" ] || die "this recipe builds a macOS plugin; it must run on macOS"
    [ "$(host_arch)" = "arm64" ] || die "this Mac is $(host_arch); Method 1 is arm64-only"
}

find_brew() {
    local b=""
    if command -v brew >/dev/null 2>&1; then
        b="$(command -v brew)"
    elif [ -x /opt/homebrew/bin/brew ]; then
        b="/opt/homebrew/bin/brew"
    fi
    [ -n "$b" ] || die "Homebrew not found. Install it (https://brew.sh) and run 'brew install cmake ninja pkgconf qt gettext bison'"
    printf '%s' "$b"
}

read_qt_minor_from_prefix() {
    local prefix="$1" p ver
    for p in "$prefix/lib/QtCore.framework/Resources/Info.plist" \
             "$prefix/lib/QtCore.framework/Versions/A/Resources/Info.plist"; do
        [ -f "$p" ] || continue
        ver="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$p" 2>/dev/null || true)"
        [ -n "$ver" ] && { printf '%s' "$ver" | awk -F. '{print $1"."$2}'; return 0; }
    done
    for p in "$prefix/bin/qmake6" "$prefix/bin/qmake"; do
        [ -x "$p" ] || continue
        ver="$("$p" -query QT_VERSION 2>/dev/null || true)"
        [ -n "$ver" ] && { printf '%s' "$ver" | awk -F. '{print $1"."$2}'; return 0; }
    done
    return 1
}

qt_prefix_list() {
    local brew="$1" f p out="" first="1" add="qt qtbase"
    while IFS= read -r f; do
        [ -n "$f" ] && add="$add $f"
    done < <("$brew" list --formula 2>/dev/null | grep -E '^qt' || true)
    for f in $add; do
        p="$("$brew" --prefix "$f" 2>/dev/null || true)"
        [ -n "$p" ] || continue
        [ -d "$p/lib/cmake/Qt6" ] || continue
        case ";$out;" in *";$p;"*) continue ;; esac
        if [ "$first" = "1" ]; then out="$p"; first="0"; else out="$out;$p"; fi
    done
    printf '%s' "$out"
}

kf6_complete() {
    [ -f "$1/lib/cmake/KF6TextEditor/KF6TextEditorConfig.cmake" ] \
        && [ -f "$1/lib/cmake/KF6Config/KF6ConfigConfig.cmake" ]
}

find_built() {
    local f best="" all
    all="$(find "$1" -type f -name "$2" 2>/dev/null || true)"
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        case "$f" in
            *kf6/ktexteditor/*) printf '%s' "$f"; return 0 ;;
        esac
        [ -n "$best" ] || best="$f"
    done <<EOF
${all}
EOF
    [ -n "$best" ] || return 1
    printf '%s' "$best"
}

main() {
    local build_dir="$repo_root/build"
    local out_dir="$repo_root/dist"
    local kf6_prefix="${KATEXDOWN_KF6_PREFIX:-}"
    local qt_prefix_arg="${KATEXDOWN_QT_PREFIX:-}"
    local cache_dir="${KATEXDOWN_KF6_CACHE:-$HOME/.cache/katexdown-kf6}"
    local with_tests="0" clean="0" build_kf6="1"
    local jobs
    jobs="$(sysctl -n hw.ncpu 2>/dev/null || printf '4')"

    while [ $# -gt 0 ]; do
        case "$1" in
            --build-dir)   [ $# -ge 2 ] || die "--build-dir needs a path"; build_dir="$2"; shift 2 ;;
            --out-dir)     [ $# -ge 2 ] || die "--out-dir needs a path"; out_dir="$2"; shift 2 ;;
            --kf6-prefix)  [ $# -ge 2 ] || die "--kf6-prefix needs a path"; kf6_prefix="$2"; shift 2 ;;
            --qt-prefix)   [ $# -ge 2 ] || die "--qt-prefix needs a path"; qt_prefix_arg="$2"; shift 2 ;;
            --with-tests)  with_tests="1"; shift ;;
            --no-build-kf6) build_kf6="0"; shift ;;
            --clean)       clean="1"; shift ;;
            --jobs)        [ $# -ge 2 ] || die "--jobs needs a number"; jobs="$2"; shift 2 ;;
            --print-qt-minor)
                local b q
                b="$(find_brew)"
                q="$qt_prefix_arg"
                [ -n "$q" ] || q="$("$b" --prefix qt 2>/dev/null || true)"
                read_qt_minor_from_prefix "$q" || printf "unknown\n"
                exit 0
                ;;
            -h|--help)     usage; exit 0 ;;
            *) die "unknown argument '$1' (see --help)" ;;
        esac
    done

    require_macos_arm64
    local brew
    brew="$(find_brew)"

    local qt_prefix
    if [ -n "$qt_prefix_arg" ]; then
        qt_prefix="$qt_prefix_arg"
    else
        log "resolving a Qt matching Kate..."
        qt_prefix="$("$script_dir/provision-qt.sh" \
            --scan-dir "$repo_root" --scan-dir "$cache_dir/src")" \
            || die "could not resolve a Qt matching Kate (see provision-qt.sh output above)"
    fi
    [ -n "$qt_prefix" ] || die "Homebrew's 'qt' formula is not installed; run 'brew install qt'"
    [ -d "$qt_prefix" ] || die "--qt-prefix '$qt_prefix' does not exist"

    local brew_minor
    brew_minor="$(read_qt_minor_from_prefix "$qt_prefix" || true)"
    log "Qt ${brew_minor:-unknown} from $qt_prefix"

    # Fail early when the chosen Qt lacks a component the plugin needs, naming
    # the exact component. The set is detected from the local CMakeLists, so no
    # curated list can go stale (Positioning/WebChannel/WebEngineWidgets, ...).
    local qt_components qt_missing
    qt_components="$(kdx_detect_qt_components "$repo_root" "$cache_dir/src" || true)"
    if [ -n "$qt_components" ]; then
        qt_missing="$(kdx_missing_qt_components "$qt_prefix" $qt_components || true)"
        [ -z "$qt_missing" ] || die "'$qt_prefix' is missing Qt components:$(printf ' %s' $qt_missing) (provision-qt.sh installs all aqt modules; or point --qt-prefix at a complete Qt)"
    fi

    # Default the KF6 prefix to a Qt-specific one so prefixes built against
    # different Qt patches never mix; Homebrew keeps the historical default.
    if [ -z "$kf6_prefix" ]; then
        local qt_ver brew_qt
        qt_ver="$(kdx_qt_prefix_version "$qt_prefix" || true)"
        brew_qt="$("$brew" --prefix qt 2>/dev/null || true)"
        if [ -n "$brew_qt" ] && [ "$qt_prefix" = "$brew_qt" ]; then
            kf6_prefix="$HOME/.local/katexdown-kf6"
        elif [ -n "$qt_ver" ]; then
            kf6_prefix="$HOME/.local/katexdown-kf6-$qt_ver"
        else
            kf6_prefix="$HOME/.local/katexdown-kf6"
        fi
    fi

    if ! kf6_complete "$kf6_prefix"; then
        if [ "$build_kf6" != "1" ]; then
            die "the KF6 prefix '$kf6_prefix' is incomplete and --no-build-kf6 was given. Build it with packaging/macos/build-kf6.sh --prefix '$kf6_prefix'"
        fi
        log "KF6 prefix '$kf6_prefix' is incomplete; building locally with build-kf6.sh..."
        "$script_dir/build-kf6.sh" --prefix "$kf6_prefix" --qt-prefix "$qt_prefix" --jobs "$jobs"
    fi
    kf6_complete "$kf6_prefix" || die "build-kf6.sh finished but '$kf6_prefix' still has no KF6TextEditor/KF6Config"

    # A provisioned/explicit Qt is complete, so trust it directly; fall back to
    # the Homebrew split-keg list when the primary prefix is incomplete.
    local prefix_path
    prefix_path="$kf6_prefix;$qt_prefix"
    if [ ! -d "$qt_prefix/lib/cmake/Qt6Widgets" ]; then
        prefix_path="$prefix_path;$(qt_prefix_list "$brew")"
    fi

    [ "$clean" = "1" ] && rm -rf "$build_dir"

    local testing="OFF"
    [ "$with_tests" = "1" ] && testing="ON"

    log "configuring the plugin (KF6 $kf6_prefix)"
    cmake -B "$build_dir" -S "$repo_root" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_CXX_STANDARD_REQUIRED=ON \
        -DCMAKE_PREFIX_PATH="$prefix_path" \
        -DBUILD_TESTING="$testing"
    log "building (${jobs} jobs)"
    cmake --build "$build_dir" --parallel "$jobs"

    local plugin
    plugin="$(find_built "$build_dir" katexdown.so || true)"
    [ -n "$plugin" ] || plugin="$(find_built "$build_dir" katexdown.dylib || true)"
    [ -n "$plugin" ] || die "the build produced no katexdown.so/.dylib under $build_dir"
    log "built plugin: $plugin"

    local version
    version="$(awk '/^project\(/ { for (i = 1; i <= NF; i++) if ($i == "VERSION") { print $(i + 1); exit } }' "$repo_root/CMakeLists.txt")"
    [ -n "$version" ] || die "could not read project(VERSION) from CMakeLists.txt"

    mkdir -p "$out_dir"
    local out_file
    out_file="$out_dir/katexdown-${version}-macos-$(host_arch).so"
    cp "$plugin" "$out_file"
    chmod 0755 "$out_file"

    if [ "$with_tests" = "1" ]; then
        local test_bin
        test_bin="$(find_built "$build_dir" previewlifecycletest || true)"
        [ -n "$test_bin" ] || die "--with-tests was given but no previewlifecycletest was built"
        info "test binary: $test_bin"
    fi

    log "wrote $out_file"
    printf '%s\n' "$out_file"
}

main "$@"
