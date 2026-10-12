#!/usr/bin/env bash
#
# katexdown - build the macOS plugin (the thin .so that install-macos.sh installs).
#
# WHAT IT PRODUCES
#   <out-dir>/katexdown-<version>-macos-<arch>.so, and the path is printed on stdout.
#   The .so keeps its references to the Homebrew Qt / KF6 build prefixes on purpose: installing
#   into Kate rewrites every one of them to @executable_path/../Frameworks/... and knows which of
#   them Kate already ships. Do not run install_name_tool here.
#
# PREREQUISITES (Homebrew is an accepted prerequisite of this method)
#   macOS on arm64 (Method 1 is arm64-only: there is no Homebrew Qt bottle for Intel macOS - see
#   design/macos-install.md), Homebrew with `qt` (that pulls qtbase/qtdeclarative/qtspeech/qtsvg/
#   qttools/qtwebchannel/qtwebengine/qtpositioning - KTextEditor needs Qt Qml/Quick/PrintSupport/
#   TextToSpeech and KIconThemes needs Qt Svg, so these are build dependencies, not plugin ones),
#   plus cmake and ninja:
#
#       brew install cmake ninja qt gettext bison
#
#   A KF6 prefix built against the same Qt. Point --kf6-prefix at one, or let this script build it
#   with build-kf6.sh (pinned version, cached by CI).
#
# THE QT-MINOR GUARD
#   The Qt minor the release is built for is pinned once, in KATEXDOWN_QT_MINOR inside
#   install-macos.sh. This script refuses to build when Homebrew's Qt minor differs from that pin,
#   because Kate's plugin loader compares major.minor only: a mismatched plugin does not error, it
#   simply never appears in Kate's plugin list. The canary in .github/workflows/macos.yml compares
#   the same pin against the Qt in a freshly downloaded Kate and fails when Kate moves.
#
# RECORDED RECIPE
#   .github/workflows/macos.yml calls this script, so a manual build and the CI cannot diverge.
#   Untested on macOS as of this revision; see packaging/macos/README.md for the list of things
#   that must be verified on a Mac (the CI job is the end-to-end check).

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd -P)"
repo_root="$(cd "$script_dir/../.." && pwd -P)"

PROG="build-plugin.sh"

log()  { printf '%s: %s\n' "$PROG" "$*" >&2; }
info() { printf '%s: %s\n' "$PROG" "$*" >&2; }
warn() { printf '%s: warning: %s\n' "$PROG" "$*" >&2; }
die()  { printf '%s: error: %s\n' "$PROG" "$*" >&2; exit 1; }

pinned_qt_minor() {
    local f="$script_dir/install-macos.sh" v
    [ -f "$f" ] || die "install-macos.sh not found next to this script; cannot read the pinned Qt minor"
    v="$(sed -n 's/^KATEXDOWN_QT_MINOR="\([0-9][0-9.]*\)".*/\1/p' "$f" | head -1)"
    [ -n "$v" ] || die "could not find KATEXDOWN_QT_MINOR in $f"
    printf '%s' "$v"
}

usage() {
    cat <<EOF
${PROG} - build the Katexdown plugin for macOS (arm64)

Usage: ${PROG} [options]

Options:
  --build-dir DIR       CMake build directory (default ./build)
  --out-dir DIR         where the release .so is written (default ./dist)
  --kf6-prefix DIR      KF6 install prefix (default \$KATEXDOWN_KF6_PREFIX or ~/.local/katexdown-kf6)
  --qt-prefix DIR       Homebrew Qt prefix (default: \$(brew --prefix qt))
  --with-tests          also build the test binaries (previewlifecycletest etc.); CI needs them
  --no-build-kf6        never run build-kf6.sh; fail when the KF6 prefix is incomplete
  --clean               delete the build directory first
  --jobs N              parallel build jobs (default: the CPU count)
  --print-qt-minor      print the pinned Qt minor (read from install-macos.sh) and exit
  -h, --help            this text

Environment:
  KATEXDOWN_KF6_PREFIX  same as --kf6-prefix
  KATEXDOWN_QT_PREFIX   same as --qt-prefix
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
    [ "$(host_arch)" = "arm64" ] || die "this Mac is $(host_arch); Method 1 is arm64-only (no Homebrew Qt bottle exists for x86_64 macOS)"
}

find_brew() {
    local b=""
    if command -v brew >/dev/null 2>&1; then
        b="$(command -v brew)"
    elif [ -x /opt/homebrew/bin/brew ]; then
        b="/opt/homebrew/bin/brew"
    fi
    [ -n "$b" ] || die "Homebrew not found. Install it (https://brew.sh) and run 'brew install cmake ninja qt gettext bison'"
    printf '%s' "$b"
}

# Qt minor of a QtCore.framework directory, via its Info.plist or qmake as a fallback.
read_qt_minor_from_prefix() { # $1 = prefix
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

# Echoes a ';'-separated CMake prefix list with every Homebrew keg the Qt build needs.
qt_prefix_list() { # $1 = brew
    local brew="$1" f p out="" first="1"
    for f in qt qtbase qtdeclarative qtspeech qtsvg qttools qtwebchannel qtwebengine qtpositioning; do
        p="$("$brew" --prefix "$f" 2>/dev/null || true)"
        [ -n "$p" ] || continue
        if [ "$first" = "1" ]; then out="$p"; first="0"; else out="$out;$p"; fi
    done
    printf '%s' "$out"
}

kf6_complete() { # $1 = prefix
    [ -f "$1/lib/cmake/KF6TextEditor/KF6TextEditorConfig.cmake" ] \
        && [ -f "$1/lib/cmake/KF6Config/KF6ConfigConfig.cmake" ]
}

# The plugin path is not in a fixed place across KDEInstallDirs versions/platforms, so search and
# prefer a kf6/ktexteditor path. Avoids `find | head` (SIGPIPE under pipefail).
find_built() { # $1 = build dir, $2 = file name
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
    local kf6_prefix="${KATEXDOWN_KF6_PREFIX:-$HOME/.local/katexdown-kf6}"
    local qt_prefix_arg="${KATEXDOWN_QT_PREFIX:-}"
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
            --print-qt-minor) pinned_qt_minor; exit 0 ;;
            -h|--help)     usage; exit 0 ;;
            *) die "unknown argument '$1' (see --help)" ;;
        esac
    done

    require_macos_arm64
    local brew
    brew="$(find_brew)"

    local qt_prefix="$qt_prefix_arg"
    [ -n "$qt_prefix" ] || qt_prefix="$("$brew" --prefix qt 2>/dev/null || true)"
    [ -n "$qt_prefix" ] || die "Homebrew's 'qt' formula is not installed; run 'brew install qt'"
    [ -d "$qt_prefix" ] || die "--qt-prefix '$qt_prefix' does not exist"

    local pin brew_minor
    pin="$(pinned_qt_minor)"
    brew_minor="$(read_qt_minor_from_prefix "$qt_prefix" || true)"
    [ -n "$brew_minor" ] || die "could not read the Qt version from '$qt_prefix'"
    if [ "$brew_minor" != "$pin" ]; then
        die "Homebrew's Qt is $brew_minor but install-macos.sh pins Qt $pin. A plugin built against Qt $brew_minor would silently never load in a Kate on Qt $pin (the loader compares major.minor only). Install a Qt $pin and re-run, or rebuild the plugin for Qt $brew_minor and bump KATEXDOWN_QT_MINOR in install-macos.sh in the same change."
    fi
    log "Qt $brew_minor from $qt_prefix (matches the pin in install-macos.sh)"

    if ! kf6_complete "$kf6_prefix"; then
        if [ "$build_kf6" != "1" ]; then
            die "the KF6 prefix '$kf6_prefix' is incomplete and --no-build-kf6 was given. Build it with packaging/macos/build-kf6.sh --prefix '$kf6_prefix'"
        fi
        log "KF6 prefix '$kf6_prefix' is incomplete; building it with build-kf6.sh (pinned, cached)"
        "$script_dir/build-kf6.sh" --prefix "$kf6_prefix" --qt-prefix "$qt_prefix" --jobs "$jobs"
    fi
    kf6_complete "$kf6_prefix" || die "build-kf6.sh finished but '$kf6_prefix' still has no KF6TextEditor/KF6Config"

    local prefix_path
    prefix_path="$kf6_prefix;$(qt_prefix_list "$brew")"

    [ "$clean" = "1" ] && rm -rf "$build_dir"

    local testing="OFF"
    [ "$with_tests" = "1" ] && testing="ON"

    log "configuring the plugin (KF6 $kf6_prefix)"
    # CMAKE_OSX_ARCHITECTURES is explicit so a Rosetta shell cannot produce an x86_64 .so by accident.
    cmake -B "$build_dir" -S "$repo_root" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_PREFIX_PATH="$prefix_path" \
        -DBUILD_TESTING="$testing"
    log "building (${jobs} jobs)"
    cmake --build "$build_dir" --parallel "$jobs"

    local plugin
    plugin="$(find_built "$build_dir" katexdown.so || true)"
    [ -n "$plugin" ] || plugin="$(find_built "$build_dir" katexdown.dylib || true)"
    [ -n "$plugin" ] || die "the build produced no katexdown.so/.dylib under $build_dir"
    log "built plugin: $plugin ($(lipo -archs "$plugin" 2>/dev/null || file -b "$plugin"))"

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
    # stdout is the one machine-readable line, for the CI to capture.
    printf '%s\n' "$out_file"
}

main "$@"
