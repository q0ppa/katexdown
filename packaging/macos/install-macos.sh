#!/usr/bin/env bash
#
# katexdown - macOS installer, and (with --uninstall) the matching uninstaller.
#
# WHAT THIS SOLVES
#   Kate for macOS ships no Qt WebEngine, and it is Developer-ID signed with the hardened runtime
#   and no library-validation entitlement, so a plugin from us is rejected by library validation
#   unless Kate's bundle is re-signed. This script closes both gaps at install time:
#
#     1. finds Kate, and refuses to touch it while it is running;
#     2. refuses a plugin whose Qt minor is not Kate's (and an engine from a Homebrew Qt that is
#        not Kate's either), because a mismatched plugin is not merely broken - Kate ignores it
#        silently and it never appears in the plugin list;
#     3. makes sure the engine is available (brew install qtwebengine when it is not);
#     4. copies the plugin to <kate>/Contents/PlugIns/kf6/ktexteditor/katexdown.so;
#     5. pulls the engine into the bundle with qtbase's macdeployqt (-executable=<plugin>, so the
#        plugin's own closure is deployed too), then rewrites or copies anything the plugin still
#        points at outside the bundle;
#     6. re-signs the whole bundle ad-hoc with katexdown.entitlements and de-quarantines it;
#     7. records the files it added, so --uninstall can remove exactly those and re-sign.
#
#   Nothing is hosted or built by this project: the release assets are the thin .so plus this
#   script (see design/macos-install.md, Method 1). Method 1b - a self-hosted WebEngine payload -
#   is deliberately not implemented.
#
# WHY THE RELEASED PLUGIN IS THIN
#   The .so carries references to Qt / QtWebEngine / QtWebChannel / QtPositioning that are resolved
#   against the copy of Qt *inside Kate's bundle*, not against Homebrew. That is why the Qt-minor
#   guard below is load-bearing, and why every reference is rewritten to
#   @executable_path/../Frameworks/... (the layout Kate's own plugins use; Kate's plugins do not
#   use @rpath).
#
# ONE IMPLEMENTATION, TWO ENTRY POINTS
#   `install-macos.sh --uninstall` is the uninstaller; packaging/macos/uninstall-macos.sh is a thin
#   front door that execs this script, so the Kate lookup, the guards and the signing exist once
#   and cannot drift between install and uninstall.
#
# COMPATIBILITY
#   bash 3.2 on purpose: /bin/bash on macOS is 3.2 and the documented entry point is
#   `curl -fsSL .../install-macos.sh | bash`. No mapfile, no associative arrays, no ${var,,},
#   no `local -n`, no arrays in "$@"-style command construction.
#
# IDEMPOTENT
#   Re-running after `brew upgrade --cask kate` (which replaces the whole bundle and wipes the
#   plugin) restores a working install. Every modification happens after all guards passed, and the
#   plugin is always copied fresh, so re-running rewrites the same references to the same values.
#
# TESTED, BUT NOT YET AGAINST A REAL KATE
#   The install -> sign -> verify -> uninstall flow has been exercised on arm64 macOS against a
#   synthesized Kate.app with the real macdeployqt/otool/install_name_tool/codesign, and
#   test-install-macos.sh covers the guard/manifest logic. What is still unproven is a real Kate
#   dmg end to end - see packaging/macos/README.md for the exact list, and
#   .github/workflows/macos.yml, which runs that flow against one.

set -euo pipefail

# --------------------------------------------------------------------------- constants

# The Qt minor the released plugin was built against. build-plugin.sh refuses to build when
# Homebrew's Qt minor differs from this, and .github/workflows/macos.yml fails when this constant
# and the Qt minor of a freshly downloaded Kate disagree - that failure IS the canary for "Kate
# moved to a new Qt, rebuild the plugin". Kate's plugin loader compares major.minor only (patch
# drift is safe), so bump this only when the minor moves.
KATEXDOWN_QT_MINOR="6.11"

# Method 1 is arm64-only: Homebrew has Qt bottles for arm64 macOS but none for x86_64 macOS
# (formulae.brew.sh, qt/qtbase/qtwebengine 6.11.x), so an Intel Mac has no engine source this
# installer could borrow. Intel needs Method 1b (see design/macos-install.md).
PLUGIN_ARCH="arm64"

PLUGIN_NAME="katexdown.so"
PLUGIN_DIR_REL="Contents/PlugIns/kf6/ktexteditor"
FRAMEWORKS_REL="Contents/Frameworks"
KATE_BUNDLE_ID="org.kde.Kate"

# Frameworks the plugin needs that Kate lacks (verified for Kate 26.08 / Qt 6.11: Kate ships neither
# WebEngine nor WebChannel/Positioning). Used by the post-install assertions and by --doctor.
REQUIRED_ENGINE_FRAMEWORKS="QtWebEngineCore QtWebEngineWidgets QtWebChannel QtPositioning"

# Release assets are fetched from "latest" when the caller did not hand us a local plugin: the
# versionless alias asset (published next to the versioned one) exists precisely so that this URL
# needs no version to be known.
KATEXDOWN_REPO="${KATEXDOWN_REPO:-q0ppa/katexdown}"
RELEASE_BASE="${KATEXDOWN_RELEASE_BASE:-https://github.com/${KATEXDOWN_REPO}/releases/latest/download}"
PLUGIN_URL_DEFAULT="${RELEASE_BASE}/katexdown-macos-${PLUGIN_ARCH}.so"

PROG="install-macos.sh"
work_dir=""
state_dir=""
kate_arg=""
plugin_arg=""
plugin_url_arg=""
plugin_qt_minor_arg=""
allow_brew_install="1"
hardened_runtime="1"
force_uninstall="0"
purge="0"

# --------------------------------------------------------------------------- output

log()  { printf 'katexdown: %s\n' "$*"; }
info() { printf 'katexdown: %s\n' "$*" >&2; }
warn() { printf 'katexdown: warning: %s\n' "$*" >&2; }
die()  { printf 'katexdown: error: %s\n' "$*" >&2; exit 1; }

die_hint() { # $1 = error, remaining = hint lines
    printf 'katexdown: error: %s\n' "$1" >&2
    if [ $# -gt 1 ]; then
        shift
        local line
        for line in "$@"; do printf 'katexdown:   %s\n' "$line" >&2; done
    fi
    exit 1
}

cleanup() { [ -n "$work_dir" ] && rm -rf "$work_dir"; return 0; }
trap cleanup EXIT

usage() {
    cat <<EOF
${PROG} - install the Katexdown plugin into Kate on macOS (arm64)

Usage:
  ${PROG} [options]              install (or reinstall) into the found Kate
  ${PROG} --uninstall [options]  remove what a previous install added
  ${PROG} --doctor [options]     report every precondition, change nothing
  ${PROG} --print-entitlements   print the entitlements used for re-signing, exit

Options:
  --kate PATH              Kate.app to patch (default: \$KATEXDOWN_KATE_APP, Spotlight for
                           ${KATE_BUNDLE_ID}, then /Applications and ~/Applications)
  --plugin PATH            the katexdown .so to install (default: a katexdown*.so next to this
                           script, else a download of ${PLUGIN_URL_DEFAULT})
  --plugin-url URL         download the plugin from URL instead (the Homebrew cask uses this so it
                           fetches the release matching its own version)
  --plugin-qt-minor X.Y    Qt minor the plugin was built for. Default ${KATEXDOWN_QT_MINOR}.
                           Developer switch only: the released plugin and this constant must agree
  --no-brew                never run \`brew install\`; fail instead if the engine is missing
  --no-hardened-runtime    last resort: sign without --options runtime, which disables the hardened
                           runtime for Kate entirely
  --force                  with --uninstall: remove the plugin even if no install manifest exists
                           (the engine files then need \`brew reinstall --cask kate\`)
  --purge                  with --uninstall: also delete the recorded install state
  -h, --help               this text

Environment:
  KATEXDOWN_KATE_APP         same as --kate
  KATEXDOWN_PLUGIN           same as --plugin
  KATEXDOWN_PLUGIN_QT_MINOR  same as --plugin-qt-minor
  KATEXDOWN_ENTITLEMENTS     use this entitlements file instead of the embedded copy
  KATEXDOWN_STATE_DIR        where the install manifest is recorded
                             (default: ~/Library/Application Support/katexdown/install)
EOF
}

# --------------------------------------------------------------------------- entitlements

# The embedded copy is the one a `curl | bash` user gets, so for that path it is the source of
# truth. packaging/macos/katexdown.entitlements is the same text in the repository, and
# .github/workflows/macos.yml diffs the two via --print-entitlements so they cannot drift.
entitlements_text() {
    cat <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.security.cs.disable-library-validation</key>
	<true/>
	<key>com.apple.security.cs.allow-jit</key>
	<true/>
	<key>com.apple.security.cs.allow-unsigned-executable-memory</key>
	<true/>
</dict>
</plist>
PLIST
}

script_dir() {
    # No readlink -f (not on macOS); $0 is enough for a downloaded file.
    local d
    d="$(cd "$(dirname "$0")" 2>/dev/null && pwd -P)" || d="."
    printf '%s' "$d"
}

# Echoes the entitlements file to sign with: an explicit override, a sibling file (a manual or CI
# run from the repository), else the embedded copy written to the temp dir.
resolve_entitlements() {
    if [ -n "${KATEXDOWN_ENTITLEMENTS:-}" ]; then
        [ -f "$KATEXDOWN_ENTITLEMENTS" ] || die "KATEXDOWN_ENTITLEMENTS='$KATEXDOWN_ENTITLEMENTS' does not exist"
        printf '%s' "$KATEXDOWN_ENTITLEMENTS"
        return 0
    fi
    local sibling
    sibling="$(script_dir)/katexdown.entitlements"
    if [ -f "$sibling" ]; then
        printf '%s' "$sibling"
        return 0
    fi
    local out="$work_dir/katexdown.entitlements"
    entitlements_text > "$out"
    printf '%s' "$out"
}

# --------------------------------------------------------------------------- small helpers

host_arch() {
    local m
    m="$(uname -m)"
    # A shell running under Rosetta (x86_64) on Apple Silicon still faces an arm64 Kate, so treat it
    # as arm64; only a real Intel Mac has no Homebrew Qt bottle to borrow from.
    if [ "$m" = "x86_64" ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || printf '0')" = "1" ]; then
        m="arm64"
    fi
    printf '%s' "$m"
}

require_macos() {
    [ "$(uname -s)" = "Darwin" ] \
        || die "this installer is for macOS; on Linux use the AUR package or build from source (see README.md)"
}

require_host_arch() {
    [ "$(host_arch)" = "$PLUGIN_ARCH" ] || die_hint "this Mac is $(host_arch), and Method 1 only covers ${PLUGIN_ARCH}." \
        "There is no Homebrew Qt bottle for x86_64 macOS, so an Intel Mac has no engine to borrow: it needs the" \
        "self-hosted payload method (Method 1b in design/macos-install.md), which is not implemented. Run Kate in a" \
        "container/VM instead until then."
}

version_minor() { printf '%s' "$1" | awk -F. '{ if (NF >= 2) print $1"."$2; else print $1 }'; }

real_path() { (cd "$1" 2>/dev/null && pwd -P); }

append_line() { # $1 = accumulated, $2 = new line -> echoes the new accumulator
    if [ -z "$1" ]; then
        printf '%s' "$2"
    else
        printf '%s\n%s' "$1" "$2"
    fi
}

plist_value() { # $1 file, $2 key
    local file="$1" key="$2" out=""
    [ -f "$file" ] || return 1
    if [ -x /usr/libexec/PlistBuddy ]; then
        out="$(/usr/libexec/PlistBuddy -c "Print :${key}" "$file" 2>/dev/null || true)"
    fi
    if [ -z "$out" ]; then
        out="$(defaults read "${file%.plist}" "$key" 2>/dev/null || true)"
    fi
    [ -n "$out" ] || return 1
    printf '%s' "$out"
}

# Qt minor of a QtCore.framework *directory* (Kate's or Homebrew's). The loader compares
# major.minor, so the patch is dropped here on purpose.
qt_minor_from_framework() {
    local fw="$1" p ver
    for p in "$fw/Resources/Info.plist" "$fw/Versions/Current/Resources/Info.plist" "$fw"/Versions/*/Resources/Info.plist; do
        [ -f "$p" ] || continue
        ver="$(plist_value "$p" CFBundleShortVersionString || true)"
        [ -n "$ver" ] && { version_minor "$ver"; return 0; }
    done
    return 1
}

kate_qt_minor() { # $1 = Kate.app
    local fw="$1/${FRAMEWORKS_REL}/QtCore.framework"
    [ -d "$fw" ] || return 1
    qt_minor_from_framework "$fw"
}

# --------------------------------------------------------------------------- Kate lookup

is_kate_bundle() {
    local b="$1" id
    [ -d "$b/Contents" ] || return 1
    [ -f "$b/Contents/MacOS/kate" ] || return 1
    id="$(plist_value "$b/Contents/Info.plist" CFBundleIdentifier || true)"
    [ "$id" = "$KATE_BUNDLE_ID" ] || return 1
    return 0
}

# Echoes the first Kate bundle that validates, or returns 1. Case-insensitively, as the design
# requires: macOS filesystems fold case, so /Applications/Kate.app and /Applications/kate.app are
# the same file, and the globs below cover either spelling.
find_kate_app() {
    local explicit="${1:-}" candidates="" c
    if [ -n "$explicit" ]; then
        [ -d "$explicit" ] || return 1
        is_kate_bundle "$explicit" || return 1
        printf '%s' "$(cd "$explicit" && pwd -P)"
        return 0
    fi
    if [ -n "${KATEXDOWN_KATE_APP:-}" ]; then
        candidates="$(append_line "$candidates" "$KATEXDOWN_KATE_APP")"
    fi

    if command -v mdfind >/dev/null 2>&1; then
        while IFS= read -r c; do
            [ -n "$c" ] && candidates="$(append_line "$candidates" "$c")"
        done < <(mdfind "kMDItemCFBundleIdentifier == \"${KATE_BUNDLE_ID}\"" 2>/dev/null | head -20 || true)
    fi

    for c in /Applications/[Kk]ate.app "${HOME}/Applications/[Kk]ate.app"; do
        [ -d "$c" ] && candidates="$(append_line "$candidates" "$c")"
    done

    while IFS= read -r c; do
        [ -n "$c" ] || continue
        if is_kate_bundle "$c"; then
            printf '%s' "$(cd "$c" && pwd -P)"
            return 0
        fi
    done <<EOF
${candidates}
EOF
    return 1
}

kate_running() { # $1 = Kate.app
    if pgrep -x kate >/dev/null 2>&1; then return 0; fi
    if pgrep -f "$1/Contents/MacOS/kate" >/dev/null 2>&1; then return 0; fi
    return 1
}

require_kate_not_running() {
    kate_running "$1" || return 0
    die_hint "Kate is running." \
        "Quit Kate and run this again: replacing frameworks Kate has loaded fails, and so does re-signing" \
        "a bundle that is in use."
}

require_writable_bundle() {
    local kate="$1" probe="$kate/Contents/.katexdown-write-probe.$$"
    if ! ( : > "$probe" ) 2>/dev/null; then
        if [ "$(id -u)" -eq 0 ]; then
            die "cannot write to '$kate' even as root"
        fi
        die_hint "no write permission on '$kate'." \
            "Kate was installed by another user or by an installer, so the plugin and the re-signing need" \
            "root: re-run this script with sudo."
    fi
    rm -f "$probe"
}

# --------------------------------------------------------------------------- Homebrew / engine

find_brew() {
    local b=""
    if command -v brew >/dev/null 2>&1; then
        b="$(command -v brew)"
    elif [ -x /opt/homebrew/bin/brew ]; then
        b="/opt/homebrew/bin/brew"
    fi
    printf '%s' "$b"
}

brew_opt_prefix() { # $1 = brew, $2 = formula; empty when not installed
    "$1" --prefix "$2" 2>/dev/null || true
}

# Every directory a Qt framework may be reachable from with Homebrew on Apple Silicon. Homebrew's
# Qt formulae install with the prefix set to HOMEBREW_PREFIX and CMAKE_STAGING_PREFIX set to the
# keg, then link the keg's lib/ entries into /opt/homebrew/lib - so the same framework is visible
# under <keg>/lib, /opt/homebrew/lib and /opt/homebrew/Frameworks. Nothing is assumed: all are
# probed.
framework_search_dirs() { # $1 = brew
    local brew="$1" f p
    for f in qtwebengine qtbase qt qttools qtwebchannel qtpositioning; do
        p="$(brew_opt_prefix "$brew" "$f")"
        [ -n "$p" ] && printf '%s\n' "$p/lib"
    done
    printf '%s\n' "/opt/homebrew/lib" "/opt/homebrew/Frameworks"
}

find_framework_dir() { # $1 = brew, $2 = framework name (no .framework)
    local brew="$1" name="$2" d hit
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        if [ -d "$d/${name}.framework" ]; then
            hit="$(real_path "$d/${name}.framework")"
            [ -n "$hit" ] && { printf '%s' "$hit"; return 0; }
        fi
    done < <(framework_search_dirs "$brew")
    return 1
}

find_dylib() { # $1 = brew, $2 = dylib file name
    local brew="$1" base="$2" d
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        [ -f "$d/$base" ] && { printf '%s' "$d/$base"; return 0; }
    done < <(framework_search_dirs "$brew")
    return 1
}

engine_present() { # $1 = brew
    find_framework_dir "$1" "QtWebEngineCore" >/dev/null 2>&1
}

# Qt minor of the Homebrew Qt that will supply the engine. The framework Info.plist is exact for the
# files we are about to deploy; qmake6 and `brew list --versions` are fallbacks for a Qt that does
# not stamp its frameworks.
brew_qt_minor() { # $1 = brew
    local brew="$1" d fw v p
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        fw="$d/QtCore.framework"
        [ -d "$fw" ] || continue
        v="$(qt_minor_from_framework "$fw" || true)"
        [ -n "$v" ] && { printf '%s' "$v"; return 0; }
    done < <(framework_search_dirs "$brew")
    for p in "$(brew_opt_prefix "$brew" qtbase)/bin/qmake6" "$(brew_opt_prefix "$brew" qt)/bin/qmake6"; do
        [ -x "$p" ] || continue
        v="$("$p" -query QT_VERSION 2>/dev/null || true)"
        [ -n "$v" ] && { version_minor "$v"; return 0; }
    done
    v="$("$brew" list --versions qtbase 2>/dev/null | awk 'NR==1 {print $2}' || true)"
    [ -n "$v" ] && { version_minor "$v"; return 0; }
    return 1
}

# macdeployqt is built by qtbase (qtbase/src/tools/macdeployqt in Qt 6, guarded by
# QT_FEATURE_macdeployqt), even though older Qt kept it in qttools - so probe both, then PATH, then
# the linked prefix.
macdeployqt_path() { # $1 = brew
    local brew="$1" c p
    for p in qtbase qt qttools; do
        c="$(brew_opt_prefix "$brew" "$p")/bin/macdeployqt"
        if [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    done
    c="/opt/homebrew/bin/macdeployqt"
    if [ -x "$c" ]; then printf '%s' "$c"; return 0; fi
    if command -v macdeployqt >/dev/null 2>&1; then command -v macdeployqt; return 0; fi
    return 1
}

find_brew_checked() {
    local brew prefix
    brew="$(find_brew)"
    [ -n "$brew" ] || die_hint "Homebrew was not found." \
        "This installer borrows Qt WebEngine from Homebrew (Kate ships none), so Homebrew is a prerequisite:" \
        "  /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"" \
        "then run this installer again. A Homebrew-free install is Method 1b, which is not implemented."
    prefix="$("$brew" --prefix)"
    [ "$prefix" = "/opt/homebrew" ] || die_hint "'$brew' has prefix '$prefix', not /opt/homebrew." \
        "That is the Intel (x86_64) Homebrew. Kate and this plugin are arm64, and an x86_64 Homebrew would" \
        "deploy x86_64 frameworks into an arm64 bundle. Install native Homebrew at /opt/homebrew and use it."
    printf '%s' "$brew"
}

# Makes sure both the engine frameworks and macdeployqt are there, and that the engine's Qt minor is
# Kate's. Echoes the brew path.
ensure_brew_and_engine() { # $1 = kate's Qt minor
    local kate_minor="$1" brew brew_minor
    brew="$(find_brew_checked)"
    if ! engine_present "$brew"; then
        [ "$allow_brew_install" = "1" ] || die_hint "Qt WebEngine is not installed and --no-brew was given." \
            "Run 'brew install qtwebengine' (which also brings macdeployqt's qtbase) and re-run this installer."
        info "Qt WebEngine is not installed; running 'brew install qtwebengine' (this is a large download)"
        "$brew" install qtwebengine || die "brew install qtwebengine failed"
        engine_present "$brew" \
            || die "brew install qtwebengine reported success but QtWebEngineCore.framework is still missing"
    fi
    if ! macdeployqt_path "$brew" >/dev/null; then
        [ "$allow_brew_install" = "1" ] || die "macdeployqt is missing and --no-brew was given; run 'brew install qtbase'"
        info "macdeployqt is missing; running 'brew install qtbase'"
        "$brew" install qtbase || die "brew install qtbase failed"
        macdeployqt_path "$brew" >/dev/null || die "macdeployqt is still missing after brew install qtbase"
    fi
    brew_minor="$(brew_qt_minor "$brew" || true)"
    if [ -z "$brew_minor" ]; then
        warn "could not read Homebrew Qt's version; skipping the engine/Qt-minor check (the plugin one still runs)"
    elif [ "$brew_minor" != "$kate_minor" ]; then
        die_hint "Homebrew's Qt is $brew_minor, this Kate uses Qt $kate_minor." \
            "The engine must match Kate's Qt minor: a WebEngine built against a $brew_minor QtCore cannot" \
            "resolve against the Qt $kate_minor frameworks inside Kate, and the preview would stay blank." \
            "Homebrew has moved past this Kate's Qt. Options: a Kate build on Qt $brew_minor plus a plugin" \
            "rebuilt for it, or an older Homebrew Qt that is still $kate_minor."
    fi
    printf '%s' "$brew"
}

# --------------------------------------------------------------------------- the plugin

plugin_qt_minor() {
    if [ -n "$plugin_qt_minor_arg" ]; then printf '%s' "$plugin_qt_minor_arg"; return 0; fi
    if [ -n "${KATEXDOWN_PLUGIN_QT_MINOR:-}" ]; then printf '%s' "$KATEXDOWN_PLUGIN_QT_MINOR"; return 0; fi
    printf '%s' "$KATEXDOWN_QT_MINOR"
}

# A plugin sitting next to this script (the repository, or the release asset a user downloaded by
# hand). Echoes the path or returns 1; never touches the network, so --doctor can use it.
find_local_plugin() {
    local want="${1:-}" d f
    if [ -n "$want" ]; then
        [ -f "$want" ] || return 1
        printf '%s' "$(cd "$(dirname "$want")" && pwd -P)/$(basename "$want")"
        return 0
    fi
    if [ -n "${KATEXDOWN_PLUGIN:-}" ]; then
        find_local_plugin "${KATEXDOWN_PLUGIN}"
        return $?
    fi
    d="$(script_dir)"
    for f in "$d"/katexdown*.so; do
        [ -f "$f" ] || continue
        printf '%s' "$(cd "$(dirname "$f")" && pwd -P)/$(basename "$f")"
        return 0
    done
    return 1
}

# Local file first (the cask and manual path), download second (the documented `curl | bash` path).
resolve_plugin() { # $1 = --plugin value (may be empty)
    local p url out
    if p="$(find_local_plugin "${1:-}")"; then
        printf '%s' "$p"
        return 0
    fi
    if [ -n "${1:-}" ]; then
        die "--plugin '$1' does not exist"
    fi
    url="${plugin_url_arg:-}"
    [ -n "$url" ] || url="$PLUGIN_URL_DEFAULT"
    info "no local plugin found; downloading $url"
    command -v curl >/dev/null 2>&1 || die "no local plugin and no curl to download one (pass --plugin PATH)"
    out="$work_dir/${PLUGIN_NAME}"
    curl -fSL --retry 3 --connect-timeout 20 -o "$out" "$url" \
        || die_hint "could not download the plugin from $url" \
            "Download katexdown-<version>-macos-${PLUGIN_ARCH}.so from the release page and pass --plugin PATH."
    printf '%s' "$out"
}

plugin_archs() { # $1 = plugin path
    if command -v lipo >/dev/null 2>&1; then
        lipo -archs "$1" 2>/dev/null || true
    else
        file -b "$1" 2>/dev/null || true
    fi
}

require_plugin_arch() { # $1 = plugin, $2 = wanted arch
    local archs
    archs="$(plugin_archs "$1")"
    case "$archs" in
        *"$2"*) return 0 ;;
    esac
    die_hint "'$1' is not a $2 binary (lipo says: '$archs')." \
        "Kate for macOS is $2, and a plugin of another architecture cannot load into it."
}

# --------------------------------------------------------------------------- state / manifest

resolve_state_dir() {
    local home="${HOME:-}"
    # Under sudo HOME is root's; the manifest belongs to the user who owns Kate and who will run the
    # uninstaller the same way.
    if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
        local h
        h="$(dscl . -read "/Users/${SUDO_USER}" NFSHomeDirectory 2>/dev/null | awk '{print $2}' || true)"
        [ -n "$h" ] && home="$h"
    fi
    [ -n "$home" ] || die "cannot determine a home directory for the install manifest; set KATEXDOWN_STATE_DIR"
    printf '%s' "${KATEXDOWN_STATE_DIR:-$home/Library/Application Support/katexdown/install}"
}

init_state() {
    state_dir="$(resolve_state_dir)"
    [ -n "$state_dir" ] || exit 1
}

manifest_path() { printf '%s/installed-files.txt' "$state_dir"; }

# A snapshot of every regular file and symlink in the bundle. Two of them, taken either side of the
# install, give the exact list of what this run added - the analogue of
# packaging/windows/make-payload.ps1's authoritative file list, except that with Method 1 the
# payload is whatever macdeployqt and the closure pass added, so it can only be measured, not
# declared.
# Directories are in the snapshot too: macdeployqt creates empty ones (a framework's Resources/,
# qtwebengine_locales/), and an uninstall that removed the files but left the empty tree behind
# would not be "remove exactly what was added".
snapshot_bundle() { # $1 = kate, $2 = output file
    ( cd "$1" && find . \( -type f -o -type l -o -type d \) -print ) | LC_ALL=C sort > "$2"
}

record_manifest() { # $1 = before file, $2 = after file
    local before="$1" after="$2" new="$work_dir/added.txt" mf n
    mf="$(manifest_path)"
    mkdir -p "$state_dir"
    comm -13 "$before" "$after" | grep -v '^\./Contents/_CodeSignature/' > "$new" || true
    # The plugin path is listed explicitly: on a reinstall it is already present and would not show
    # up in the diff, and uninstall must remove it either way. The previous manifest is merged in so
    # that a reinstall never loses track of files an earlier run added.
    {
        [ -f "$mf" ] && cat "$mf"
        printf './%s/%s\n' "$PLUGIN_DIR_REL" "$PLUGIN_NAME"
        cat "$new"
    } | LC_ALL=C sort -u > "$mf.tmp"
    mv "$mf.tmp" "$mf"
    n="$(grep -c . "$mf" || true)"
    log "recorded $n installed file(s) in $mf"
}

# --------------------------------------------------------------------------- signing

# $1 = sign one path, $2 = entitlements. No arrays: bash 3.2's set -u makes "${arr[@]}" on an empty
# array an error.
sign_one() {
    if [ "$hardened_runtime" = "1" ]; then
        codesign --force --options runtime --entitlements "$2" --sign - "$1"
    else
        codesign --force --entitlements "$2" --sign - "$1"
    fi
}

sign_bundle() { # $1 = kate, $2 = entitlements
    local kate="$1" ents="$2"
    log "re-signing $kate ad-hoc (this replaces Kate's vendor signature, which cannot be restored)"
    if codesign --force --deep --options runtime --entitlements "$ents" --sign - "$kate" 2> "$work_dir/sign.err" \
        && codesign --verify --deep --strict "$kate" 2>> "$work_dir/sign.err"; then
        return 0
    fi
    if [ "$hardened_runtime" != "1" ]; then
        # Already the no-runtime fallback: retry it once without --deep so the failure is a real one.
        if codesign --force --entitlements "$ents" --sign - "$kate" 2> "$work_dir/sign.err" \
            && codesign --verify --deep --strict "$kate" 2>> "$work_dir/sign.err"; then
            return 0
        fi
    fi
    warn "--deep signing failed or did not verify; falling back to bottom-up signing (--deep is deprecated)"
    if [ -s "$work_dir/sign.err" ]; then cat "$work_dir/sign.err" >&2; fi
    sign_bottom_up "$kate" "$ents"
    codesign --verify --deep --strict "$kate" \
        || die "codesign --verify --deep --strict still fails after bottom-up signing; Kate would refuse to load the plugin"
}

sign_bottom_up() { # $1 = kate, $2 = entitlements
    local kate="$1" ents="$2" list="$work_dir/nested.txt" order="$work_dir/nested-order.txt" f
    : > "$list"
    # Nested bundles and every Mach-O executable, tagged with its path depth so the deepest can be
    # signed first (nested code must be signed before whatever contains it).
    find "$kate" -type f -perm -u+x 2>/dev/null | while IFS= read -r f; do
        case "$(file -b "$f" 2>/dev/null || true)" in
            *Mach-O*) printf '%s\t%s\n' "$(printf '%s' "$f" | awk -F/ '{print NF}')" "$f" ;;
        esac
    done > "$list"
    find "$kate/${FRAMEWORKS_REL}" -type d \( -name '*.framework' -o -name '*.app' \) 2>/dev/null | while IFS= read -r f; do
        printf '%s\t%s\n' "$(printf '%s' "$f" | awk -F/ '{print NF}')" "$f"
    done >> "$list"
    sort -t"$(printf '\t')" -k1,1nr "$list" > "$order"
    while IFS="$(printf '\t')" read -r _ f; do
        [ -n "$f" ] || continue
        sign_one "$f" "$ents" 2>/dev/null || warn "could not sign nested code: $f"
    done < "$order"
    # Then the bundle itself, without --deep.
    sign_one "$kate" "$ents"
}

dequarantine() { # $1 = kate
    xattr -dr com.apple.quarantine "$1" 2>/dev/null || true
}

# --------------------------------------------------------------------------- install

plugin_refs() { otool -L "$1" | awk 'NR > 1 && NF >= 1 { print $1 }'; }

framework_binary_suffix() { # $1 = kate, $2 = framework name -> path after <name>.framework/
    local fw="$1/${FRAMEWORKS_REL}/$2.framework" p
    if [ -e "$fw/Versions/Current/$2" ]; then
        printf 'Versions/Current/%s' "$2"
        return 0
    fi
    for p in "$fw"/Versions/*/"$2"; do
        [ -e "$p" ] || continue
        printf 'Versions/%s/%s' "$(basename "$(dirname "$p")")" "$2"
        return 0
    done
    if [ -e "$fw/$2" ]; then
        printf '%s' "$2"
        return 0
    fi
    return 1
}

# The closure pass: after macdeployqt the plugin must resolve every library out of Kate's own
# bundle, with nothing left pointing at Homebrew or at the build prefix. Anything Kate already ships
# is only *rewritten* (never copied - overwriting Kate's own Qt/KF6 with another build's copy is a
# good way to break the bundle); anything Kate lacks is copied in from Homebrew first, which is
# exactly what the manifest then records.
closure_pass() { # $1 = brew, $2 = kate
    local brew="$1" kate="$2" plugin="$2/$PLUGIN_DIR_REL/$PLUGIN_NAME"
    local ref base name src fw new changed="0"
    log "checking the plugin's library references"
    while IFS= read -r ref; do
        [ -n "$ref" ] || continue
        case "$ref" in
            /usr/lib/*|/System/*|@executable_path/*|@loader_path/*) continue ;;
        esac
        base="${ref##*/}"
        case "$ref" in
            *.framework/*)
                fw="${ref%%".framework/"*}"
                name="${fw##*/}"
                name="${name%.framework}"
                if [ ! -e "$kate/${FRAMEWORKS_REL}/${name}.framework" ]; then
                    src="$(find_framework_dir "$brew" "$name" || true)"
                    [ -n "$src" ] || die_hint "the plugin needs ${name}.framework, Kate has none, and Homebrew has none." \
                        "Install or refresh it with 'brew install qtwebengine' (that covers QtWebEngineCore," \
                        "QtWebChannel and QtPositioning), then run this installer again."
                    mkdir -p "$kate/${FRAMEWORKS_REL}"
                    log "copying ${name}.framework into Kate (Kate does not ship it)"
                    ditto "$src" "$kate/${FRAMEWORKS_REL}/${name}.framework" \
                        || die "could not copy $src into the bundle"
                fi
                new="@executable_path/../Frameworks/${name}.framework/$(framework_binary_suffix "$kate" "$name" || printf 'Versions/Current/%s' "$name")"
                ;;
            *.dylib)
                case "$base" in
                    libKF6*)
                        # A KDE Framework library never gets copied in: the plugin implements
                        # KTextEditor interfaces that must be the same library object Kate has
                        # loaded. If Kate lacks it, this plugin cannot work at all.
                        [ -e "$kate/${FRAMEWORKS_REL}/${base}" ] || die_hint "the plugin needs $base and this Kate does not ship it." \
                            "The plugin was built against a different KDE Frameworks build than this Kate's, so it can" \
                            "never load. Install a plugin release built for this Kate (or a Kate matching the plugin)."
                        ;;
                    *)
                        if [ ! -e "$kate/${FRAMEWORKS_REL}/${base}" ]; then
                            src="$(find_dylib "$brew" "$base" || true)"
                            [ -n "$src" ] || die_hint "the plugin needs $base, Kate has none, and Homebrew has none." \
                                "It is a Homebrew dependency of the Qt that built the plugin; 'brew install qtwebengine'" \
                                "usually brings those along. If the plugin's reference list changed, this needs a look" \
                                "from the maintainer."
                            mkdir -p "$kate/${FRAMEWORKS_REL}"
                            log "copying $base into Kate (Kate does not ship it)"
                            cp "$src" "$kate/${FRAMEWORKS_REL}/${base}" || die "could not copy $src into the bundle"
                        fi
                        ;;
                esac
                new="@executable_path/../Frameworks/${base}"
                ;;
            *)
                die_hint "the plugin references '$ref', which is neither a framework nor a .dylib." \
                    "Refusing to guess how to resolve it inside the bundle."
                ;;
        esac
        if [ "$ref" != "$new" ]; then
            install_name_tool -change "$ref" "$new" "$plugin" || die "install_name_tool could not rewrite '$ref'"
            log "  $ref -> $new"
            changed="1"
        fi
    done < <(plugin_refs "$plugin")
    [ "$changed" = "1" ] || log "  (nothing to rewrite: the plugin already resolves inside the bundle)"
}

# Fails loudly rather than leaving behind a plugin that silently will not load.
assert_plugin_refs_clean() { # $1 = kate
    local plugin="$1/$PLUGIN_DIR_REL/$PLUGIN_NAME" bad="" ref
    while IFS= read -r ref; do
        case "$ref" in
            /usr/lib/*|/System/*|@executable_path/*|@loader_path/*) continue ;;
        esac
        bad="$(append_line "$bad" "  $ref")"
    done < <(plugin_refs "$plugin")
    [ -z "$bad" ] || die "after installation the plugin still resolves libraries outside Kate's bundle:
${bad}"
}

bundle_has() { # $1 = kate, $2 = file/dir name anywhere in it
    local out
    out="$(find "$1" -name "$2" -print -quit 2>/dev/null || true)"
    [ -n "$out" ]
}

assert_engine_in_bundle() { # $1 = kate
    local kate="$1" name missing=""
    for name in $REQUIRED_ENGINE_FRAMEWORKS; do
        if [ ! -d "$kate/${FRAMEWORKS_REL}/${name}.framework" ] && ! bundle_has "$kate" "${name}.framework"; then
            missing="$(append_line "$missing" "${name}.framework")"
        fi
    done
    [ -z "$missing" ] || die "the install did not put ${missing} inside '$kate'; the preview would stay blank"
    # The render process is a separate app bundle, and Chromium's data files are opened by name at
    # runtime - a clean link or a clean otool says nothing about either.
    bundle_has "$kate" 'QtWebEngineProcess.app' \
        || die "QtWebEngineProcess.app is not inside '$kate'; every render would fail to spawn a renderer"
    [ -n "$(find "$kate" -name 'icudtl.dat' -print -quit 2>/dev/null || true)" ] \
        || die "icudtl.dat is not inside '$kate'; Chromium's ICU data would be missing"
    [ -n "$(find "$kate" -name '*.pak' -print -quit 2>/dev/null || true)" ] \
        || die "no Chromium .pak resources inside '$kate'; WebEngine would render nothing"
}

backup_qt_conf() { # $1 = kate
    local kate="$1" conf="$kate/Contents/Resources/qt.conf" saved="$state_dir/qt.conf.original"
    [ -f "$conf" ] || return 0
    mkdir -p "$state_dir"
    # macdeployqt rewrites Contents/Resources/qt.conf, and uninstall puts Kate's own file back. On a
    # re-run the conf on disk is already macdeployqt's, so the saved copy must NOT be clobbered with
    # it - otherwise uninstall would "restore" macdeployqt's file as if it were Kate's.
    if [ -f "$saved" ] && grep -q '^Plugins *= *PlugIns' "$conf"; then
        log "keeping the Contents/Resources/qt.conf already saved in $saved (this bundle was patched before)"
        return 0
    fi
    cp "$conf" "$saved"
    log "saved Kate's existing Contents/Resources/qt.conf to $saved (macdeployqt rewrites it)"
}

cmd_install() {
    require_macos
    require_host_arch
    local kate plugin minor kate_minor brew macdeployqt before after

    kate="$(find_kate_app "$kate_arg")" || die_hint "no Kate on this Mac." \
        "Looked at \$KATEXDOWN_KATE_APP, Spotlight for ${KATE_BUNDLE_ID}, /Applications and ~/Applications." \
        "Install Kate ('brew install --cask kate') or point at it explicitly: --kate /path/to/Kate.app"
    log "Kate: $kate"

    kate_minor="$(kate_qt_minor "$kate" || true)"
    [ -n "$kate_minor" ] || die_hint "could not read Kate's Qt version from '$kate'." \
        "Expected ${FRAMEWORKS_REL}/QtCore.framework/.../Info.plist. Is this a complete Kate bundle? Kate is" \
        "patched as a whole - a partial copy cannot work."
    log "Kate uses Qt $kate_minor"

    minor="$(plugin_qt_minor)"
    [ "$minor" = "$kate_minor" ] || die_hint "Qt minor mismatch: this plugin is built for Qt $minor, Kate uses Qt $kate_minor." \
        "Kate's plugin loader compares major.minor only, and a mismatch does not produce an error - the plugin" \
        "simply never appears in Kate's plugin list. Use a Katexdown release built for Qt $kate_minor, or rebuild" \
        "with packaging/macos/build-plugin.sh."
    log "plugin Qt minor: $minor (matches)"

    require_kate_not_running "$kate"

    plugin="$(resolve_plugin "$plugin_arg")"
    require_plugin_arch "$plugin" "$PLUGIN_ARCH"
    log "plugin: $plugin ($(plugin_archs "$plugin"))"

    brew="$(ensure_brew_and_engine "$kate_minor")"
    macdeployqt="$(macdeployqt_path "$brew")"
    log "macdeployqt: $macdeployqt"
    log "Homebrew Qt engine: $(brew_qt_minor "$brew" || printf '?') (from $brew)"

    require_writable_bundle "$kate"
    init_state

    log "--- installing into $kate"
    backup_qt_conf "$kate"
    before="$work_dir/before.txt"
    after="$work_dir/after.txt"
    snapshot_bundle "$kate" "$before"

    copy_plugin "$plugin" "$kate"
    run_macdeployqt "$macdeployqt" "$kate"
    closure_pass "$brew" "$kate"

    # The manifest is written *before* signing: codesign rewrites Contents/_CodeSignature, which
    # belongs to Kate's signature (the uninstaller regenerates it by re-signing), not to the payload.
    snapshot_bundle "$kate" "$after"
    record_manifest "$before" "$after"

    sign_bundle "$kate" "$(resolve_entitlements)"
    dequarantine "$kate"
    assert_plugin_refs_clean "$kate"
    assert_engine_in_bundle "$kate"

    log "--- installed"
    log "Start Kate, then enable the preview: Settings -> Configure Kate -> Plugins -> Katexdown."
    warn "Kate's vendor signature is gone and cannot be restored: the clean revert is 'brew reinstall --cask kate'."
    warn "'brew upgrade --cask kate' replaces the bundle and wipes this install; re-run this installer afterwards."
}

copy_plugin() { # $1 = plugin, $2 = kate
    local dst="$2/$PLUGIN_DIR_REL"
    mkdir -p "$dst"
    # Always a fresh copy: the released plugin is not signed by us, and every rewrite below must
    # start from the same pristine file, which is what makes a re-run idempotent.
    install -m 0755 "$1" "$dst/$PLUGIN_NAME"
    log "plugin installed: $dst/$PLUGIN_NAME"
}

run_macdeployqt() { # $1 = macdeployqt, $2 = kate
    log "macdeployqt: deploying the engine into Kate's bundle (this rewrites files inside the app)"
    # -executable also deploys the *plugin's* closure: without it macdeployqt only looks at Kate's own
    # binary, and WebEngine - which only the plugin needs - would never be copied in.
    # -always-overwrite makes a re-run idempotent; -no-strip keeps the deployment from running strip
    # inside a bundle we are about to sign (size is not the point here, correctness is).
    "$1" "$2" \
        -executable="$2/$PLUGIN_DIR_REL/$PLUGIN_NAME" \
        -always-overwrite -no-strip -verbose=1
}

# --------------------------------------------------------------------------- uninstall

remove_from_manifest() { # $1 = kate -> 0 when a manifest existed
    local kate="$1" mf list dirs rel target removed=0
    mf="$(manifest_path)"
    [ -f "$mf" ] || return 1
    list="$work_dir/removable.txt"
    grep -v '^[[:space:]]*$' "$mf" > "$list" || true
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        case "$rel" in
            ./*) ;;
            *) warn "ignoring manifest entry that is not relative: $rel"; continue ;;
        esac
        target="$kate/${rel#./}"
        case "$target" in
            "$kate"/*) ;;
            *) warn "ignoring manifest entry outside the bundle: $rel"; continue ;;
        esac
        if [ -L "$target" ] || [ -f "$target" ]; then
            rm -f "$target"
            removed=$((removed + 1))
        fi
        # Directories are left to the rmdir pass below, which only removes what is empty by then.
    done < "$list"
    log "removed $removed file(s) recorded in $mf"
    # Codesign creates _CodeSignature/ directories after the manifest snapshot, so they are not in
    # the manifest. Left behind, a framework whose files were just removed keeps only _CodeSignature
    # and the re-sign below fails with "bundle format unrecognized". Drop every one; sign_bundle
    # regenerates the ones that are still needed.
    find "$kate" -type d -name _CodeSignature -prune -print 2>/dev/null | while IFS= read -r d; do
        rm -rf "$d"
    done
    # Prune directories the payload created, deepest first; rmdir refuses any directory something
    # else still uses, so Kate's own structure is never touched.
    dirs="$work_dir/dirs.txt"
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        printf '%s\t%s\n' "$(printf '%s' "$kate/${rel#./}" | awk -F/ '{print NF}')" "$(dirname "$kate/${rel#./}")"
    done < "$list" | sort -rn | cut -f2- | awk '!seen[$0]++' > "$dirs"
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        case "$rel" in
            "$kate"|"$kate/Contents"|"$kate/${FRAMEWORKS_REL}"|"$kate/Contents/MacOS"|"$kate/Contents/PlugIns") continue ;;
        esac
        rmdir "$rel" 2>/dev/null || true
    done < "$dirs"
    return 0
}

restore_qt_conf() { # $1 = kate
    local saved="$state_dir/qt.conf.original"
    [ -f "$saved" ] || return 0
    cp "$saved" "$1/Contents/Resources/qt.conf"
    log "restored Kate's original Contents/Resources/qt.conf"
}

cmd_uninstall() {
    require_macos
    local kate had_manifest="0"
    kate="$(find_kate_app "$kate_arg")" || die_hint "no Kate on this Mac." \
        "Looked at \$KATEXDOWN_KATE_APP, Spotlight for ${KATE_BUNDLE_ID}, /Applications and ~/Applications." \
        "Pass --kate /path/to/Kate.app to clean up a copy of the bundle."
    log "Kate: $kate"
    require_kate_not_running "$kate"
    require_writable_bundle "$kate"
    init_state

    if remove_from_manifest "$kate"; then
        had_manifest="1"
    fi
    if [ "$had_manifest" = "0" ]; then
        if [ "$force_uninstall" != "1" ]; then
            die_hint "no install manifest at $(manifest_path)." \
                "What this installer deployed can only be known from that manifest, so there is nothing safe to" \
                "remove automatically. Remove the plugin alone with '--uninstall --force', then reinstall Kate" \
                "for a clean bundle:  brew reinstall --cask kate"
        fi
        warn "no manifest: removing only the plugin, leaving the engine files in place"
        rm -f "$kate/$PLUGIN_DIR_REL/$PLUGIN_NAME"
        rmdir "$kate/$PLUGIN_DIR_REL" 2>/dev/null || true
    fi

    restore_qt_conf "$kate"

    # Deleting nested code invalidates the signature for strict verification, and a bundle whose
    # signature does not verify does not start at all, so re-sign with the same entitlements.
    sign_bundle "$kate" "$(resolve_entitlements)"

    [ ! -e "$kate/$PLUGIN_DIR_REL/$PLUGIN_NAME" ] || die "the plugin is still at $kate/$PLUGIN_DIR_REL/$PLUGIN_NAME"
    log "plugin removed; Kate re-signed ad-hoc"

    if [ "$had_manifest" = "1" ]; then
        rm -f "$(manifest_path)"
        log "removed $(manifest_path)"
    fi
    if [ "$purge" = "1" ]; then
        rm -rf "$state_dir"
        log "removed $state_dir"
    fi
    warn "Kate's vendor signature is gone: for a pristine bundle run 'brew reinstall --cask kate'."
    log "--- uninstalled"
}

# --------------------------------------------------------------------------- doctor

doctor() {
    require_macos
    local ok="0" fail="0" kate="" kate_minor="" plugin="" plugin_minor="" brew="" brew_minor="" mdq=""

    report() { # $1 = ok|fail|skip, $2 = text
        case "$1" in
            ok)   log "doctor: [ ok ] $2"; ok=$((ok + 1)) ;;
            fail) log "doctor: [FAIL] $2"; fail=$((fail + 1)) ;;
            *)    log "doctor: [skip] $2" ;;
        esac
    }

    report ok "macOS $(sw_vers -productVersion 2>/dev/null || printf '?'), arch $(host_arch)"

    if [ "$(host_arch)" = "$PLUGIN_ARCH" ]; then
        report ok "architecture is ${PLUGIN_ARCH}"
    else
        report fail "architecture is $(host_arch), and Method 1 only covers ${PLUGIN_ARCH} (see design/macos-install.md)"
    fi

    kate="$(find_kate_app "$kate_arg" || true)"
    if [ -n "$kate" ]; then
        report ok "Kate bundle: $kate"
        if kate_running "$kate"; then
            report fail "Kate is running (quit it before installing)"
        else
            report ok "Kate is not running"
        fi
        kate_minor="$(kate_qt_minor "$kate" || true)"
        if [ -n "$kate_minor" ]; then
            report ok "Kate's Qt minor: $kate_minor"
        else
            report fail "cannot read Kate's Qt minor from ${FRAMEWORKS_REL}/QtCore.framework"
        fi
        if ( : > "$kate/Contents/.katexdown-doctor-probe.$$" ) 2>/dev/null; then
            rm -f "$kate/Contents/.katexdown-doctor-probe.$$"
            report ok "Kate's bundle is writable"
        else
            report fail "Kate's bundle is not writable (re-run with sudo, or fix ownership of $kate)"
        fi
    else
        report fail "no Kate found (set KATEXDOWN_KATE_APP or pass --kate)"
    fi

    if command -v codesign >/dev/null 2>&1; then
        report ok "codesign: $(command -v codesign)"
    else
        report fail "codesign not found (install the command line tools: xcode-select --install)"
    fi

    plugin="$(find_local_plugin "$plugin_arg" || true)"
    if [ -n "$plugin" ]; then
        report ok "plugin: $plugin ($(plugin_archs "$plugin"))"
        case "$(plugin_archs "$plugin")" in
            *"$PLUGIN_ARCH"*) report ok "plugin architecture matches ${PLUGIN_ARCH}" ;;
            *) report fail "plugin architecture does not match ${PLUGIN_ARCH}: $(plugin_archs "$plugin")" ;;
        esac
    elif [ -n "$plugin_arg" ] || [ -n "${KATEXDOWN_PLUGIN:-}" ]; then
        report fail "no plugin at '$plugin_arg${KATEXDOWN_PLUGIN:-}'"
    else
        report skip "no local plugin; the installer would download ${PLUGIN_URL_DEFAULT}"
    fi

    plugin_minor="$(plugin_qt_minor)"
    report ok "plugin's Qt minor (this script / --plugin-qt-minor): $plugin_minor"
    if [ -n "$kate_minor" ] && [ "$plugin_minor" != "$kate_minor" ]; then
        report fail "Qt minor mismatch: plugin $plugin_minor vs Kate $kate_minor"
    elif [ -n "$kate_minor" ]; then
        report ok "plugin's Qt minor matches Kate's"
    fi

    brew="$(find_brew)"
    if [ -n "$brew" ]; then
        local prefix
        prefix="$("$brew" --prefix 2>/dev/null || true)"
        if [ "$prefix" = "/opt/homebrew" ]; then
            report ok "Homebrew: $brew (prefix $prefix)"
        else
            report fail "Homebrew prefix is '$prefix', not /opt/homebrew (that is the Intel Homebrew; this needs the arm64 one)"
        fi
        if engine_present "$brew"; then
            report ok "Qt WebEngine: $(find_framework_dir "$brew" QtWebEngineCore)"
        else
            report fail "Qt WebEngine not installed (run: brew install qtwebengine)"
        fi
        brew_minor="$(brew_qt_minor "$brew" || true)"
        if [ -z "$brew_minor" ]; then
            report fail "cannot read Homebrew's Qt version (is the qtbase formula installed?)"
        elif [ -n "$kate_minor" ] && [ "$brew_minor" != "$kate_minor" ]; then
            report fail "Homebrew Qt is $brew_minor but Kate is $kate_minor (Homebrew has moved past this Kate)"
        else
            report ok "Homebrew Qt minor: $brew_minor"
        fi
        mdq="$(macdeployqt_path "$brew" || true)"
        if [ -n "$mdq" ]; then
            report ok "macdeployqt: $mdq"
        else
            report fail "macdeployqt not found (run: brew install qtbase)"
        fi
    else
        report fail "Homebrew not found (https://brew.sh, then re-run)"
    fi

    init_state
    if [ -f "$(manifest_path)" ]; then
        report ok "previous install recorded: $(grep -c . "$(manifest_path)" || printf 0) file(s) in $(manifest_path)"
    else
        report skip "no previous install recorded at $(manifest_path) (uninstall would need --force)"
    fi

    log "doctor: $ok ok, $fail failed"
    log "doctor: paste this with a report: macos=$(sw_vers -productVersion 2>/dev/null || printf '?') arch=$(host_arch) kate=${kate:-none} kate_qt=${kate_minor:-none} plugin_qt=$(plugin_qt_minor) brew_qt=${brew_minor:-none}"
    [ "$fail" -eq 0 ] || exit 1
    return 0
}

# --------------------------------------------------------------------------- main

main() {
    local mode="install"
    work_dir="$(mktemp -d "${TMPDIR:-/tmp}/katexdown.XXXXXX")"

    while [ $# -gt 0 ]; do
        case "$1" in
            --kate) [ $# -ge 2 ] || die "--kate needs a path"; kate_arg="$2"; shift 2 ;;
            --plugin) [ $# -ge 2 ] || die "--plugin needs a path"; plugin_arg="$2"; shift 2 ;;
            --plugin-url) [ $# -ge 2 ] || die "--plugin-url needs a URL"; plugin_url_arg="$2"; shift 2 ;;
            --plugin-qt-minor) [ $# -ge 2 ] || die "--plugin-qt-minor needs a value"; plugin_qt_minor_arg="$2"; shift 2 ;;
            --no-brew) allow_brew_install="0"; shift ;;
            --no-hardened-runtime) hardened_runtime="0"; shift ;;
            --uninstall) mode="uninstall"; shift ;;
            --doctor) mode="doctor"; shift ;;
            --print-entitlements) entitlements_text; exit 0 ;;
            --force) force_uninstall="1"; shift ;;
            --purge) purge="1"; shift ;;
            -h|--help) usage; exit 0 ;;
            *) die "unknown argument '$1' (see --help)" ;;
        esac
    done

    if [ "$hardened_runtime" = "0" ]; then
        warn "--no-hardened-runtime: signing without --options runtime disables the hardened runtime for Kate entirely."
        warn "That is the documented last resort; the plugin needs only disable-library-validation, plus JIT for Chromium."
    fi

    case "$mode" in
        install) cmd_install ;;
        uninstall) cmd_uninstall ;;
        doctor) doctor ;;
    esac
}

main "$@"
