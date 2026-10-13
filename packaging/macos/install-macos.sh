#!/usr/bin/env bash
#
# katexdown - macOS installer and uninstaller (--uninstall).
#
# WHAT THIS DOES
#   Kate for macOS ships as a Developer-ID signed app bundle with the hardened runtime
#   enabled and no library-validation entitlement. To allow the Katexdown plugin to load,
#   this installer:
#     1. Finds Kate.app and ensures Kate is not running.
#     2. Verifies Kate's Qt minor matches the plugin's Qt minor.
#     3. Verifies Homebrew and Qt WebEngine are available (`brew install qtwebengine`).
#     4. Installs katexdown.so into <kate>/Contents/PlugIns/kf6/ktexteditor/.
#     5. Re-signs Kate.app ad-hoc with entitlements (disabling library validation).
#     6. Provides a matching clean uninstall with `--uninstall`.
#
# COMPATIBILITY
#   bash 3.2 compatible (/bin/bash on macOS).
#   Idempotent: safe to re-run after `brew upgrade --cask kate`.

set -euo pipefail

# --------------------------------------------------------------------------- constants

KATEXDOWN_QT_MINOR="${KATEXDOWN_PLUGIN_QT_MINOR:-6.11}"
PLUGIN_ARCH="arm64"
PLUGIN_NAME="katexdown.so"
PLUGIN_DIR_REL="Contents/PlugIns/kf6/ktexteditor"
FRAMEWORKS_REL="Contents/Frameworks"
KATE_BUNDLE_ID="org.kde.Kate"

KATEXDOWN_REPO="${KATEXDOWN_REPO:-q0ppa/katexdown}"
RELEASE_BASE="${KATEXDOWN_RELEASE_BASE:-https://github.com/${KATEXDOWN_REPO}/releases/latest/download}"
PLUGIN_URL_DEFAULT="${RELEASE_BASE}/katexdown-macos-${PLUGIN_ARCH}.so"

PROG="install-macos.sh"
work_dir=""
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

die_hint() {
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
${PROG} - install the Katexdown plugin into Kate on macOS (${PLUGIN_ARCH})

Usage:
  ${PROG} [options]              install (or reinstall) into the found Kate
  ${PROG} --uninstall [options]  remove the plugin and re-sign Kate
  ${PROG} --doctor [options]     report preconditions without changing anything
  ${PROG} --print-entitlements   print the entitlements used for re-signing, exit

Options:
  --kate PATH            Kate.app to patch (default: \$KATEXDOWN_KATE_APP, Spotlight,
                         or /Applications / ~/Applications)
  --plugin PATH          path to katexdown.so (default: local file or download)
  --plugin-url URL       download plugin from this URL instead of default release
  --plugin-qt-minor X.Y  Qt minor the plugin was built for (default: ${KATEXDOWN_QT_MINOR})
  --no-brew              fail if qtwebengine is missing; do not run \`brew install\`
  --no-hardened-runtime  sign without --options runtime (disables hardened runtime)
  --force                with --uninstall: proceed even if plugin is already absent
  --purge                with --uninstall: clean up any recorded state directory
  -h, --help             this text

Environment:
  KATEXDOWN_KATE_APP         same as --kate
  KATEXDOWN_PLUGIN           same as --plugin
  KATEXDOWN_PLUGIN_QT_MINOR  same as --plugin-qt-minor
  KATEXDOWN_ENTITLEMENTS     use this entitlements file instead of the embedded copy
  KATEXDOWN_STATE_DIR        optional state directory
EOF
}

# --------------------------------------------------------------------------- entitlements

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
    local d
    d="$(cd "$(dirname "$0")" 2>/dev/null && pwd -P)" || d="."
    printf '%s' "$d"
}

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

# --------------------------------------------------------------------------- helpers

host_arch() {
    local m
    m="$(uname -m)"
    if [ "$m" = "x86_64" ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || printf '0')" = "1" ]; then
        m="arm64"
    fi
    printf '%s' "$m"
}

require_macos() {
    [ "$(uname -s)" = "Darwin" ] \
        || die "this installer is for macOS; on Linux use your package manager or build from source (see README.md)"
}

require_host_arch() {
    [ "$(host_arch)" = "$PLUGIN_ARCH" ] || die_hint "this Mac is $(host_arch), and Method 1 only covers ${PLUGIN_ARCH}." \
        "Intel Macs lack prebuilt Qt WebEngine bottles in Homebrew. Run Kate in a VM or build from source."
}

version_minor() {
    printf '%s' "$1" | awk -F. '{ if (NF >= 2) print $1"."$2; else print $1 }'
}

append_line() {
    if [ -z "$1" ]; then
        printf '%s' "$2"
    else
        printf '%s\n%s' "$1" "$2"
    fi
}

plist_value() {
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

qt_minor_from_framework() {
    local fw="$1" p ver
    for p in "$fw/Resources/Info.plist" "$fw/Versions/Current/Resources/Info.plist" "$fw"/Versions/*/Resources/Info.plist; do
        [ -f "$p" ] || continue
        ver="$(plist_value "$p" CFBundleShortVersionString || true)"
        [ -n "$ver" ] && { version_minor "$ver"; return 0; }
    done
    return 1
}

kate_qt_minor() {
    local fw="$1/${FRAMEWORKS_REL}/QtCore.framework"
    [ -d "$fw" ] || return 1
    qt_minor_from_framework "$fw"
}

target_qt_minor() {
    if [ -n "$plugin_qt_minor_arg" ]; then
        printf '%s' "$plugin_qt_minor_arg"
    else
        printf '%s' "$KATEXDOWN_QT_MINOR"
    fi
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

kate_running() {
    if pgrep -x kate >/dev/null 2>&1; then return 0; fi
    if pgrep -f "$1/Contents/MacOS/kate" >/dev/null 2>&1; then return 0; fi
    return 1
}

require_kate_not_running() {
    kate_running "$1" || return 0
    die_hint "Kate is running." \
        "Quit Kate and run this again: replacing loaded plugins fails, and so does re-signing a running bundle."
}

require_writable_bundle() {
    local kate="$1" probe="$kate/Contents/.katexdown-write-probe.$$"
    if ! ( : > "$probe" ) 2>/dev/null; then
        if [ "$(id -u)" -eq 0 ]; then
            die "cannot write to '$kate' even as root"
        fi
        die_hint "no write permission on '$kate'." \
            "Re-run this script with sudo."
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

find_brew_checked() {
    local brew prefix
    brew="$(find_brew)"
    [ -n "$brew" ] || die_hint "Homebrew was not found." \
        "This installer requires Qt WebEngine from Homebrew (Kate ships none), so Homebrew is a prerequisite:" \
        "  /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"" \
        "then run this installer again."
    prefix="$("$brew" --prefix 2>/dev/null || true)"
    [ "$prefix" = "/opt/homebrew" ] || die_hint "'$brew' has prefix '$prefix', not /opt/homebrew." \
        "That is the Intel Homebrew. Kate and this plugin are arm64. Install native Homebrew at /opt/homebrew."
    printf '%s' "$brew"
}

engine_present() {
    local brew="$1" p
    for p in "$("$brew" --prefix qtwebengine 2>/dev/null || true)" \
             "$("$brew" --prefix qt 2>/dev/null || true)" \
             "/opt/homebrew"; do
        [ -n "$p" ] || continue
        if [ -d "$p/lib/QtWebEngineCore.framework" ] || [ -d "$p/Frameworks/QtWebEngineCore.framework" ]; then
            return 0
        fi
    done
    return 1
}

brew_qt_minor() {
    local brew="$1" p v
    for p in "$("$brew" --prefix qtbase 2>/dev/null || true)" \
             "$("$brew" --prefix qt 2>/dev/null || true)"; do
        [ -n "$p" ] || continue
        if [ -d "$p/lib/QtCore.framework" ]; then
            v="$(qt_minor_from_framework "$p/lib/QtCore.framework" || true)"
            [ -n "$v" ] && { printf '%s' "$v"; return 0; }
        fi
    done
    v="$("$brew" list --versions qtbase 2>/dev/null | awk 'NR==1 {print $2}' || true)"
    [ -n "$v" ] && { version_minor "$v"; return 0; }
    return 1
}

ensure_brew_and_engine() {
    local kate_minor="${1:-}" brew brew_minor
    brew="$(find_brew_checked)"
    if ! engine_present "$brew"; then
        [ "$allow_brew_install" = "1" ] || die_hint "Qt WebEngine is not installed and --no-brew was given." \
            "Run 'brew install qtwebengine' and re-run this installer."
        info "Qt WebEngine is not installed; running 'brew install qtwebengine'..."
        "$brew" install qtwebengine || die "brew install qtwebengine failed"
        engine_present "$brew" || die "brew install qtwebengine succeeded but QtWebEngineCore.framework is still missing"
    fi
    brew_minor="$(brew_qt_minor "$brew" || true)"
    if [ -n "$brew_minor" ] && [ -n "$kate_minor" ] && [ "$brew_minor" != "$kate_minor" ]; then
        die_hint "Homebrew's Qt is $brew_minor, this Kate uses Qt $kate_minor." \
            "The engine must match Kate's Qt minor: a WebEngine built against a $brew_minor QtCore cannot" \
            "resolve against the Qt $kate_minor frameworks inside Kate, and the preview would stay blank."
    fi
    printf '%s' "$brew"
}

# --------------------------------------------------------------------------- plugin resolution

plugin_archs() {
    local plugin="$1"
    if command -v lipo >/dev/null 2>&1; then
        lipo -archs "$plugin" 2>/dev/null || true
    else
        file -b "$plugin" 2>/dev/null || true
    fi
}

require_plugin_arch() {
    local plugin="$1" expected="$2" archs
    archs="$(plugin_archs "$plugin")"
    case "$archs" in
        *"$expected"*) return 0 ;;
        *) die "plugin '$plugin' is not a $expected binary (reports '$archs')" ;;
    esac
}

find_local_plugin() {
    local explicit="${1:-}" cand
    if [ -n "$explicit" ]; then
        [ -f "$explicit" ] && { printf '%s' "$explicit"; return 0; }
        return 1
    fi
    if [ -n "${KATEXDOWN_PLUGIN:-}" ] && [ -f "$KATEXDOWN_PLUGIN" ]; then
        printf '%s' "$KATEXDOWN_PLUGIN"; return 0
    fi
    for cand in \
        "$(script_dir)"/katexdown*.so \
        "$(script_dir)/../../dist"/katexdown*.so \
        "$(script_dir)/../../build"/katexdown*.so \
        ./dist/katexdown*.so \
        ./build/katexdown*.so; do
        [ -f "$cand" ] && { printf '%s' "$cand"; return 0; }
    done
    return 1
}

resolve_plugin() {
    local explicit="${1:-}" p url
    if p="$(find_local_plugin "$explicit")"; then
        printf '%s' "$p"
        return 0
    fi
    url="${plugin_url_arg:-$PLUGIN_URL_DEFAULT}"
    p="$work_dir/$PLUGIN_NAME"
    info "downloading $url"
    curl -fSL --retry 3 --connect-timeout 20 -o "$p" "$url" \
        || die "could not download $url"
    printf '%s' "$p"
}

# --------------------------------------------------------------------------- codesign

sign_one() {
    local path="$1" ents="$2"
    if [ "$hardened_runtime" = "1" ]; then
        codesign --force --options runtime --entitlements "$ents" --sign - "$path"
    else
        codesign --force --entitlements "$ents" --sign - "$path"
    fi
}

sign_bundle() {
    local kate="$1" ents="$2"
    log "re-signing $kate ad-hoc with entitlements"
    if [ "$hardened_runtime" = "1" ]; then
        if codesign --force --deep --options runtime --entitlements "$ents" --sign - "$kate" 2> "$work_dir/sign.err" \
            && codesign --verify --deep --strict "$kate" 2>> "$work_dir/sign.err"; then
            return 0
        fi
    else
        if codesign --force --deep --entitlements "$ents" --sign - "$kate" 2> "$work_dir/sign.err" \
            && codesign --verify --deep --strict "$kate" 2>> "$work_dir/sign.err"; then
            return 0
        fi
    fi

    warn "--deep signing failed or did not verify; falling back to bottom-up signing"
    if [ -s "$work_dir/sign.err" ]; then cat "$work_dir/sign.err" >&2; fi
    sign_bottom_up "$kate" "$ents"
    codesign --verify --deep --strict "$kate" \
        || die "codesign --verify --deep --strict failed after bottom-up signing"
}

sign_bottom_up() {
    local kate="$1" ents="$2" list="$work_dir/nested.txt" order="$work_dir/nested-order.txt" f
    : > "$list"
    find "$kate" -type f -perm -u+x 2>/dev/null | while IFS= read -r f; do
        case "$(file -b "$f" 2>/dev/null || true)" in
            *Mach-O*) printf '%s\t%s\n' "$(printf '%s' "$f" | awk -F/ '{print NF}')" "$f" ;;
        esac
    done > "$list"
    find "$kate/${FRAMEWORKS_REL}" -type d \( -name '*.framework' -o -name '*.app' \) 2>/dev/null | while IFS= read -r f; do
        printf '%s\t%s\n' "$(printf '%s' "$f" | awk -F/ '{print NF}')" "$f"
    done >> "$list"
    find "$kate/Contents/PlugIns" -type f -name '*.so' 2>/dev/null | while IFS= read -r f; do
        printf '%s\t%s\n' "$(printf '%s' "$f" | awk -F/ '{print NF}')" "$f"
    done >> "$list"
    sort -t"$(printf '\t')" -k1,1nr "$list" > "$order"
    while IFS="$(printf '\t')" read -r _ f; do
        [ -n "$f" ] || continue
        sign_one "$f" "$ents" 2>/dev/null || warn "could not sign nested code: $f"
    done < "$order"
    sign_one "$kate" "$ents"
}

dequarantine() {
    xattr -dr com.apple.quarantine "$1" 2>/dev/null || true
}

# --------------------------------------------------------------------------- commands

cmd_install() {
    require_macos
    require_host_arch

    local kate plugin brew ents dst_dir dst_file kate_minor exp_minor

    kate="$(find_kate_app "$kate_arg")" || die_hint "no Kate on this Mac." \
        "Looked at \$KATEXDOWN_KATE_APP, Spotlight, /Applications and ~/Applications." \
        "Install Kate ('brew install --cask kate') or pass --kate /path/to/Kate.app"
    log "Kate: $kate"

    kate_minor="$(kate_qt_minor "$kate" || true)"
    exp_minor="$(target_qt_minor)"
    if [ -n "$kate_minor" ] && [ "$kate_minor" != "$exp_minor" ]; then
        die_hint "Qt minor mismatch: this plugin is built for Qt $exp_minor, Kate uses Qt $kate_minor." \
            "Kate's plugin loader compares major.minor only, and a mismatch does not produce an error - the plugin" \
            "simply never appears in Kate's plugin list. Use a Katexdown release built for Qt $kate_minor, or rebuild" \
            "with packaging/macos/build-plugin.sh."
    fi

    require_kate_not_running "$kate"
    require_writable_bundle "$kate"

    brew="$(ensure_brew_and_engine "$kate_minor")"
    log "Homebrew: $brew"

    plugin="$(resolve_plugin "$plugin_arg")"
    require_plugin_arch "$plugin" "$PLUGIN_ARCH"
    log "plugin: $plugin ($(plugin_archs "$plugin"))"

    ents="$(resolve_entitlements)"
    dst_dir="$kate/$PLUGIN_DIR_REL"
    dst_file="$dst_dir/$PLUGIN_NAME"

    log "installing plugin into $dst_file"
    mkdir -p "$dst_dir"
    install -m 0755 "$plugin" "$dst_file"

    sign_bundle "$kate" "$ents"
    dequarantine "$kate"

    log "--- installed successfully"
    log "Start Kate, then enable: Settings -> Configure Kate -> Plugins -> Katexdown."
}

cmd_uninstall() {
    require_macos

    local kate dst_file dst_dir ents
    kate="$(find_kate_app "$kate_arg")" || die_hint "no Kate on this Mac." \
        "Pass --kate /path/to/Kate.app to target a specific copy."
    log "Kate: $kate"

    require_kate_not_running "$kate"
    require_writable_bundle "$kate"

    dst_dir="$kate/$PLUGIN_DIR_REL"
    dst_file="$dst_dir/$PLUGIN_NAME"

    if [ ! -e "$dst_file" ]; then
        if [ "$force_uninstall" != "1" ]; then
            die_hint "no Katexdown plugin found at $dst_file." \
                "Pass --force to re-sign Kate anyway: ${PROG} --uninstall --force"
        fi
        warn "no plugin file found at $dst_file; re-signing anyway"
    else
        log "removing $dst_file"
        rm -f "$dst_file"
        rmdir "$dst_dir" 2>/dev/null || true
    fi

    find "$kate" -type d -name _CodeSignature -prune -print 2>/dev/null | while IFS= read -r d; do
        rm -rf "$d"
    done

    ents="$(resolve_entitlements)"
    sign_bundle "$kate" "$ents"

    if [ "$purge" = "1" ] && [ -n "${KATEXDOWN_STATE_DIR:-}" ]; then
        rm -rf "$KATEXDOWN_STATE_DIR"
    fi

    log "--- uninstalled successfully"
}

cmd_doctor() {
    require_macos
    local ok="0" fail="0" kate="" brew="" brew_minor="" plugin="" kate_minor=""

    report() {
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
        report fail "architecture is $(host_arch); Method 1 covers ${PLUGIN_ARCH} only"
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
            report ok "Kate bundle is writable"
        else
            report fail "Kate bundle is not writable (run with sudo)"
        fi
    else
        report fail "no Kate found (set KATEXDOWN_KATE_APP or pass --kate)"
    fi

    if command -v codesign >/dev/null 2>&1; then
        report ok "codesign tool available: $(command -v codesign)"
    else
        report fail "codesign tool not found"
    fi

    brew="$(find_brew)"
    if [ -n "$brew" ]; then
        report ok "Homebrew: $brew"
        if engine_present "$brew"; then
            report ok "Qt WebEngine installed"
        else
            report fail "Qt WebEngine not installed (run: brew install qtwebengine)"
        fi
        brew_minor="$(brew_qt_minor "$brew" || true)"
        if [ -n "$brew_minor" ]; then
            report ok "Homebrew Qt minor: $brew_minor"
        fi
    else
        report fail "Homebrew not found"
    fi

    plugin="$(find_local_plugin "$plugin_arg" || true)"
    if [ -n "$plugin" ]; then
        report ok "local plugin: $plugin"
        case "$(plugin_archs "$plugin")" in
            *"$PLUGIN_ARCH"*) report ok "plugin architecture matches ${PLUGIN_ARCH}" ;;
            *) report fail "plugin architecture does not match ${PLUGIN_ARCH}" ;;
        esac
    else
        report skip "no local plugin; installer will fetch from ${PLUGIN_URL_DEFAULT}"
    fi

    log "doctor: $ok ok, $fail failed"
    [ "$fail" -eq 0 ] || exit 1
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

    case "$mode" in
        install) cmd_install ;;
        uninstall) cmd_uninstall ;;
        doctor) cmd_doctor ;;
    esac
}

main "$@"
