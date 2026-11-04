#!/usr/bin/env bash
#
# katexdown - ensure a Qt matching Kate's exact patch is installed, and print
# the Qt prefix to build against.
#
# WHY THIS EXISTS
#   Kate for macOS bundles its own Qt and KF6, and links Qt *private* symbols.
#   Those ABI symbols are versioned per Qt patch (QtPrivate_6_11_1 vs
#   QtPrivate_6_11_2), so the plugin's Qt/KF6 must be built against Kate's
#   EXACT Qt patch. Homebrew's Qt is usually a different patch, which yields a
#   plugin Kate lists but can never enable.
#
# WHAT IT DOES (no curated Qt-module list anywhere)
#   1. Detect Kate's exact Qt version from the private ABI tag in its bundled
#      QtCore (e.g. QtPrivate_6_11_1 -> 6.11.1).
#   2. Reuse an existing matching Qt if one is already installed: --qt-prefix /
#      $KATEXDOWN_QT_PREFIX, then $KATEXDOWN_QT_ROOT/<ver>/macos, then the
#      Homebrew Qt (so the Homebrew-only workflow is preserved when it matches).
#   3. Otherwise install that Qt with aqtinstall, together with EVERY module
#      aqt reports for that version/arch. The module list comes from aqt itself
#      (minus debug symbols), so there is no hand-maintained list to fall stale -
#      the previous curated lists kept missing one more module (TextToSpeech,
#      TaskTree, ...). If aqt reports no modules, fall back to `-m all`.
#   4. Verify the chosen prefix carries Kate's ABI, and report which of the Qt
#      components the local sources need are missing from it.
#
# The resolved Qt prefix is the only thing on stdout; all progress goes to stderr.

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd -P)"
repo_root="$(cd "$script_dir/../.." && pwd -P)"
# shellcheck source=lib-qt.sh
. "$script_dir/lib-qt.sh"

PROG="provision-qt.sh"

log()  { printf '%s: %s\n' "$PROG" "$*" >&2; }
info() { printf '%s: %s\n' "$PROG" "$*" >&2; }
warn() { printf '%s: warning: %s\n' "$PROG" "$*" >&2; }
die()  { printf '%s: error: %s\n' "$PROG" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
${PROG} - ensure a Qt matching Kate's exact patch, and print its prefix

Usage: ${PROG} [options]

Options:
  --kate PATH        Kate.app to read the Qt version from (default: auto-detect)
  --qt-version X.Y.Z install/reuse this Qt version instead of reading Kate
  --qt-prefix DIR    use this Qt prefix if it matches (authoritative; no install)
  --qt-root DIR      where aqt installs Qt (default \$KATEXDOWN_QT_ROOT or ~/Qt)
  --aqt PATH         aqtinstall executable (default \$KATEXDOWN_AQT, ~/.venvs/aqt/bin/aqt, PATH)
  --arch NAME        aqt architecture (default clang_64)
  --scan-dir DIR     add a source tree to scan for required Qt components
                     (repeatable; default: this repo and the KF6 source cache)
  --print-version    print Kate's exact Qt version and exit
  -h, --help         this text

Environment:
  KATEXDOWN_KATE_APP   same as --kate
  KATEXDOWN_QT_PREFIX  same as --qt-prefix
  KATEXDOWN_QT_ROOT    same as --qt-root
  KATEXDOWN_AQT        same as --aqt
  KATEXDOWN_KF6_CACHE  KF6 source cache to scan (default ~/.cache/katexdown-kf6)
EOF
}

find_brew() {
    local b=""
    if command -v brew >/dev/null 2>&1; then b="$(command -v brew)"
    elif [ -x /opt/homebrew/bin/brew ]; then b="/opt/homebrew/bin/brew"
    fi
    printf '%s' "$b"
}

# Ensure an aqtinstall is available, bootstrapping one into a venv if needed.
ensure_aqt() { # $1 = explicit path or ""
    local explicit="$1" aqt
    if [ -n "$explicit" ]; then
        [ -x "$explicit" ] || die "--aqt '$explicit' is not executable"
        printf '%s' "$explicit"; return 0
    fi
    if [ -n "${KATEXDOWN_AQT:-}" ] && [ -x "$KATEXDOWN_AQT" ]; then
        printf '%s' "$KATEXDOWN_AQT"; return 0
    fi
    if [ -x "$HOME/.venvs/aqt/bin/aqt" ]; then
        printf '%s' "$HOME/.venvs/aqt/bin/aqt"; return 0
    fi
    if command -v aqt >/dev/null 2>&1; then
        printf '%s' "$(command -v aqt)"; return 0
    fi
    command -v python3 >/dev/null 2>&1 \
        || die "no aqtinstall and no python3 to install one. Install Python 3, then: python3 -m venv ~/.venvs/aqt && ~/.venvs/aqt/bin/pip install aqtinstall"
    log "aqtinstall not found; creating ~/.venvs/aqt and installing aqtinstall..."
    python3 -m venv "$HOME/.venvs/aqt" || die "could not create ~/.venvs/aqt"
    "$HOME/.venvs/aqt/bin/pip" install --quiet --upgrade pip aqtinstall \
        || die "could not install aqtinstall; run: ~/.venvs/aqt/bin/pip install aqtinstall"
    [ -x "$HOME/.venvs/aqt/bin/aqt" ] || die "aqtinstall install finished but ~/.venvs/aqt/bin/aqt is missing"
    printf '%s' "$HOME/.venvs/aqt/bin/aqt"
}

# Install the matching Qt (all modules aqt reports) under $1/<version>/macos.
install_qt() { # $1 = qt_root, $2 = version, $3 = arch, $4 = aqt
    local qt_root="$1" version="$2" arch="$3" aqt="$4"
    local modules fallback="0"
    modules="$(kdx_aqt_modules "$aqt" "$version" "$arch" || true)"
    if [ -z "$modules" ]; then
        warn "aqt reported no module list for Qt $version/$arch; installing with -m all"
        fallback="1"
    fi

    mkdir -p "$qt_root"
    log "installing Qt $version ($arch) + all modules into $qt_root with aqtinstall"
    if [ "$fallback" = "1" ]; then
        # shellcheck disable=SC2086
        "$aqt" install-qt mac desktop "$version" "$arch" -m all -O "$qt_root" \
            || die "aqtinstall failed for Qt $version ($arch)"
    else
        # shellcheck disable=SC2086
        "$aqt" install-qt mac desktop "$version" "$arch" -m $modules -O "$qt_root" \
            || die "aqtinstall failed for Qt $version ($arch)"
    fi

    local installed="$qt_root/$version/macos"
    [ -d "$installed/lib/cmake/Qt6" ] || installed="$qt_root/$version/$arch"
    [ -d "$installed/lib/cmake/Qt6" ] || die "aqtinstall finished but no Qt6 cmake package was found under $qt_root/$version"
    printf '%s' "$installed"
}

main() {
    local kate_arg="${KATEXDOWN_KATE_APP:-}"
    local qt_version_arg="" qt_prefix_arg="${KATEXDOWN_QT_PREFIX:-}"
    local qt_root="${KATEXDOWN_QT_ROOT:-$HOME/Qt}"
    local aqt_arg="${KATEXDOWN_AQT:-}"
    local arch="clang_64"
    local scan_dirs=()

    while [ $# -gt 0 ]; do
        case "$1" in
            --kate)         [ $# -ge 2 ] || die "--kate needs a path"; kate_arg="$2"; shift 2 ;;
            --qt-version)   [ $# -ge 2 ] || die "--qt-version needs a value"; qt_version_arg="$2"; shift 2 ;;
            --qt-prefix)    [ $# -ge 2 ] || die "--qt-prefix needs a path"; qt_prefix_arg="$2"; shift 2 ;;
            --qt-root)      [ $# -ge 2 ] || die "--qt-root needs a path"; qt_root="$2"; shift 2 ;;
            --aqt)          [ $# -ge 2 ] || die "--aqt needs a path"; aqt_arg="$2"; shift 2 ;;
            --arch)         [ $# -ge 2 ] || die "--arch needs a value"; arch="$2"; shift 2 ;;
            --scan-dir)     [ $# -ge 2 ] || die "--scan-dir needs a path"; scan_dirs+=("$2"); shift 2 ;;
            --print-version)
                [ -n "$qt_version_arg" ] && { printf '%s\n' "$qt_version_arg"; exit 0; }
                local k
                k="$(kdx_find_kate_app "$kate_arg")" || die "no Kate found to read the Qt version from"
                kdx_kate_qt_version "$k" || die "could not read Kate's Qt version"
                exit 0 ;;
            -h|--help)      usage; exit 0 ;;
            *) die "unknown argument '$1' (see --help)" ;;
        esac
    done

    # --- 1. Kate's exact Qt ---------------------------------------------------
    local target_version="$qt_version_arg" target_abi="" kate=""
    if [ -z "$target_version" ]; then
        if kate="$(kdx_find_kate_app "$kate_arg")"; then
            target_version="$(kdx_kate_qt_version "$kate")" \
                || die "could not read the Qt version from $kate"
            target_abi="$(kdx_qt_private_abi "$(kdx_kate_qt_core_binary "$kate")" || true)"
            log "Kate: $kate"
            log "Kate's Qt: $target_version${target_abi:+ ($target_abi)}"
        else
            warn "no Kate found; falling back to Homebrew's Qt (pass --kate or --qt-version to be explicit)"
        fi
    else
        log "Qt version requested explicitly: $target_version"
    fi

    # --- 2. Reuse an already-installed matching Qt ----------------------------
    local brew candidates=() c c_abi c_ver
    brew="$(find_brew)"
    if [ -n "$qt_prefix_arg" ]; then
        candidates=("$qt_prefix_arg")
    else
        [ -n "$target_version" ] && candidates+=("$qt_root/$target_version/macos")
        if [ -n "$brew" ]; then
            c="$("$brew" --prefix qt 2>/dev/null || true)"
            [ -n "$c" ] && candidates+=("$c")
        fi
    fi
    if [ "${#candidates[@]}" -eq 0 ]; then
        die "no Kate and no Homebrew Qt found; install Kate ('brew install --cask kate') or pass --qt-version"
    fi

    for c in "${candidates[@]}"; do
        [ -n "$c" ] || continue
        [ -d "$c/lib/cmake/Qt6" ] || continue
        c_abi="$(kdx_qt_prefix_abi "$c" || true)"
        c_ver="$(kdx_qt_prefix_version "$c" || true)"
        # An explicit --qt-prefix is authoritative: use it, refusing only when
        # its ABI is readable and contradicts Kate's.
        if [ -n "$qt_prefix_arg" ]; then
            if [ -n "$target_abi" ] && [ -n "$c_abi" ] && [ "$c_abi" != "$target_abi" ]; then
                die "--qt-prefix '$c' is $c_abi, but Kate needs $target_abi"
            fi
            log "using --qt-prefix $c (Qt $c_ver${c_abi:+, ABI $c_abi})"
            verify_components "$c" "${c_ver:-$target_version}" ${scan_dirs[@]+"${scan_dirs[@]}"}
            printf '%s\n' "$c"
            exit 0
        fi
        if [ -z "$target_abi" ]; then
            target_abi="$c_abi"
            [ -n "$c_ver" ] && target_version="$c_ver"
        fi
        if { [ -n "$target_abi" ] && [ "$c_abi" = "$target_abi" ]; } || { [ -z "$target_abi" ] && [ -n "$c_abi" ]; }; then
            log "using existing Qt $c_ver at $c (ABI $c_abi matches Kate)"
            verify_components "$c" "$target_version" ${scan_dirs[@]+"${scan_dirs[@]}"}
            printf '%s\n' "$c"
            exit 0
        fi
        [ -n "$c_abi" ] && log "skipping $c (Qt $c_ver, ABI $c_abi does not match ${target_abi:-Kate})"
    done

    if [ -n "$qt_prefix_arg" ]; then
        die "the Qt prefix '$qt_prefix_arg' does not provide Qt $target_version${target_abi:+ (ABI $target_abi)}"
    fi

    # --- 3. Install the matching Qt ------------------------------------------
    local aqt installed installed_abi
    aqt="$(ensure_aqt "$aqt_arg")"
    installed="$(install_qt "$qt_root" "$target_version" "$arch" "$aqt")"
    installed_abi="$(kdx_qt_prefix_abi "$installed" || true)"
    if [ -n "$target_abi" ] && [ -n "$installed_abi" ] && [ "$installed_abi" != "$target_abi" ]; then
        die "installed Qt at $installed has ABI $installed_abi, but Kate needs $target_abi"
    fi
    log "installed Qt $target_version at $installed${installed_abi:+ (ABI $installed_abi)}"

    verify_components "$installed" "$target_version" ${scan_dirs[@]+"${scan_dirs[@]}"}
    printf '%s\n' "$installed"
}

# Report the Qt components the local sources need but the prefix lacks.
verify_components() { # $1 = Qt prefix, $2 = version, $3.. = extra scan dirs
    local prefix="$1" version="$2"
    shift 2
    local kf6_cache="${KATEXDOWN_KF6_CACHE:-$HOME/.cache/katexdown-kf6}"
    local dirs=() d comp components missing count=0
    dirs+=("$repo_root")
    [ -d "$kf6_cache/src" ] && dirs+=("$kf6_cache/src")
    for d in "$@"; do dirs+=("$d"); done

    components="$(kdx_detect_qt_components "${dirs[@]}" || true)"
    [ -n "$components" ] || return 0
    log "Qt components required by the local sources: $(printf '%s' "$components" | tr '\n' ' ')"
    missing="$(kdx_missing_qt_components "$prefix" $components || true)"
    if [ -z "$missing" ]; then
        log "all required Qt components are present in $prefix"
        return 0
    fi
    # Advisory only: some components are platform-guarded or optional, so the
    # ground truth is the CMake configure of the frameworks themselves.
    while IFS= read -r comp; do
        [ -n "$comp" ] || continue
        warn "Qt component '$comp' is not present in $prefix (may be optional/platform-specific)"
        count=$((count + 1))
    done <<EOF
$missing
EOF
    warn "$count component(s) not found; if a build fails, its configure log names the exact component"
    return 0
}

main "$@"
