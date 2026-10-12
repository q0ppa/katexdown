#!/usr/bin/env bash
#
# Exercises packaging/macos/install-macos.sh's guard, manifest and signing logic without macOS,
# without Kate and without touching the network.
#
# WHY
#   Most of the installer's value is the part that must fail loudly: the Qt-minor guard, the
#   architecture guard, the "Kate is running" refusal, the "never copy a KF6 library" rule, and the
#   install/uninstall manifest symmetry. All of that is pure shell logic, so it is testable with a
#   fake environment - which is the only way to test it without a Mac (run it from a Linux
#   checkout; the macOS workflow does not run it because its first check asserts the installer
#   refuses to run on non-macOS). It does NOT test the parts that need a real Mac: macdeployqt's
#   deployment, real codesign behaviour, and whether Kate then loads the plugin. Those are covered
#   by the end-to-end step in the macOS workflow; packaging/macos/README.md lists them.
#
# HOW
#   A fake bin/ directory shadows uname, sw_vers, sysctl, mdfind, pgrep, lipo, otool,
#   install_name_tool, codesign, xattr, ditto, macdeployqt, brew, dscl and defaults; a fake Kate.app
#   tree and a fake Homebrew Qt tree stand in for the real ones. The script under test is then run
#   exactly as a user would run it.
#
# Usage: packaging/macos/test-install-macos.sh [-v]
#   -v  keep the fake environment and print it (for debugging a failure)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd -P)"
INSTALLER="$SCRIPT_DIR/install-macos.sh"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
VERBOSE="0"
[ "${1:-}" = "-v" ] && VERBOSE="1"

failures=0
checks=0
work_root="$(mktemp -d "${TMPDIR:-/tmp}/katexdown-test.XXXXXX")"
keep_work="0"
cleanup() {
    if [ "$keep_work" = "1" ] || [ "$VERBOSE" = "1" ]; then
        printf 'test: fake environment kept at %s\n' "$work_root"
    else
        rm -rf "$work_root"
    fi
    return 0
}
trap cleanup EXIT

note() { printf 'test: %s\n' "$*"; }
fail() {
    failures=$((failures + 1))
    printf 'test: FAIL: %s\n' "$*" >&2
}

check() { # $1 = description, $2 = command string (eval'd), rest: message on failure
    checks=$((checks + 1))
    if eval "$2" > /dev/null 2>&1; then
        printf 'test: ok   %s\n' "$1"
    else
        fail "$1"
    fi
}

assert_contains() { # $1 = haystack, $2 = needle, $3 = description
    checks=$((checks + 1))
    case "$1" in
        *"$2"*) printf 'test: ok   %s\n' "$3" ;;
        *) fail "$3 (expected to find '$2' in: $(printf '%s' "$1" | head -5 | tr '\n' '|'))" ;;
    esac
}

# --------------------------------------------------------------------------- fake environment

make_fakes() { # $1 = fake root (bin dir is $1/bin)
    local root="$1"
    mkdir -p "$root/bin"

    cat > "$root/bin/fake-head" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# shellcheck source=/dev/null
. "${FAKE_CONFIG:?FAKE_CONFIG is not set}"
EOF

    # shellcheck disable=SC2016
    cat > "$root/bin/uname" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
case "${1:-}" in
    -s|-o|-v) printf 'Darwin\n' ;;
    -m)       printf '%s\n' "${FAKE_ARCH:-arm64}" ;;
    *)        printf 'Darwin\n' ;;
esac
EOF

    cat > "$root/bin/sw_vers" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
case "${1:-}" in
    -productVersion) printf '%s\n' "${FAKE_MACOS:-15.0}" ;;
    *) printf 'ProductName:\tmacOS\nProductVersion:\t%s\n' "${FAKE_MACOS:-15.0}" ;;
esac
EOF

    cat > "$root/bin/sysctl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
if [ "${1:-}" = "-n" ] && [ "${2:-}" = "sysctl.proc_translated" ]; then
    printf '%s\n' "${FAKE_ROSETTA:-0}"
    exit 0
fi
exit 1
EOF

    cat > "$root/bin/mdfind" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
[ -n "${FAKE_MDFIND:-}" ] || exit 0
printf '%s\n' "${FAKE_MDFIND}"
EOF

    cat > "$root/bin/pgrep" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
[ "${FAKE_KATE_RUNNING:-0}" = "1" ] && exit 0
exit 1
EOF

    cat > "$root/bin/lipo" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
case "${1:-}" in
    -archs) printf '%s\n' "${FAKE_PLUGIN_ARCHS:-arm64}" ;;
    *) exit 1 ;;
esac
EOF

    # otool -L <file> prints the file itself first (as the real one does), then the refs recorded in
    # <file>.refs - so the tests can drive the closure pass with any reference list they like.
    cat > "$root/bin/otool" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
[ "${1:-}" = "-L" ] || exit 1
f="${2:?otool -L needs a file}"
[ -f "$f" ] || exit 1
# The reference list is a property of the binary, wherever it is copied to: <file>.refs wins, else
# the sidecar named after the file in FAKE_REFS_DIR (that is the one the installer's copy sees).
refs=""
if [ -f "$f.refs" ]; then
    refs="$f.refs"
elif [ -n "${FAKE_REFS_DIR:-}" ] && [ -f "$FAKE_REFS_DIR/$(basename "$f").refs" ]; then
    refs="$FAKE_REFS_DIR/$(basename "$f").refs"
fi
printf '%s:\n' "$f"
if [ -n "$refs" ]; then
    while IFS= read -r line; do
        [ -n "$line" ] && printf '\t%s (compatibility version 6.0.0, current version 6.11.2)\n' "$line"
    done < "$refs"
fi
EOF

    cat > "$root/bin/install_name_tool" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
[ "${1:-}" = "-change" ] || exit 1
old="$2"; new="$3"; f="$4"
printf '%s\n' "$old -> $new" >> "${FAKE_TOOL_LOG:-/dev/null}"
refs=""
if [ -f "$f.refs" ]; then
    refs="$f.refs"
elif [ -n "${FAKE_REFS_DIR:-}" ] && [ -f "$FAKE_REFS_DIR/$(basename "$f").refs" ]; then
    refs="$FAKE_REFS_DIR/$(basename "$f").refs"
fi
[ -n "$refs" ] || exit 1
python3 - "$old" "$new" "$refs" <<'PY'
import sys
old, new, path = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(path).read().splitlines()
open(path, "w").write("\n".join(new if l == old else l for l in lines) + "\n")
PY
EOF

    cat > "$root/bin/codesign" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
printf 'codesign %s\n' "$*" >> "${FAKE_CODESIGN_LOG:-/dev/null}"
# FAKE_CODESIGN_DEEP_BROKEN simulates the documented "--deep fails" case, so the bottom-up fallback
# is exercised rather than assumed: a --deep sign leaves the bundle broken (a marker) until some
# signing step without --deep fixes it - which is exactly the behaviour the installer must recover
# from and then verify.
case "$*" in
    *--verify*)
        if [ -e "$FAKE_ROOT/needs-resign" ]; then exit 1; fi
        exit 0
        ;;
esac
if [ "${FAKE_CODESIGN_DEEP_BROKEN:-0}" = "1" ]; then
    case "$*" in
        *--deep*) touch "$FAKE_ROOT/needs-resign"; exit 1 ;;
        *)        rm -f "$FAKE_ROOT/needs-resign"; exit 0 ;;
    esac
fi
# Real codesign drops a _CodeSignature/ directory into every bundle/framework it signs, and that
# happens AFTER the installer's pre-signing manifest snapshot. Simulate it so the uninstall path is
# forced to clear those too: otherwise a removed framework is left with only _CodeSignature and the
# re-sign fails with "bundle format unrecognized".
last=""
for a in "$@"; do last="$a"; done
if [ -n "$last" ] && [ -d "$last" ]; then
    mkdir -p "$last/_CodeSignature" 2>/dev/null || true
    # --deep signs nested code too; mirror that so the uninstall path has to clear signatures that
    # were not in the pre-signing manifest.
    case "$*" in
        *--deep*)
            find "$last" -type d \( -name '*.framework' -o -name '*.app' \) -print 2>/dev/null | while IFS= read -r nested; do
                mkdir -p "$nested/_CodeSignature" 2>/dev/null || true
            done
            ;;
    esac
fi
exit 0
EOF

    cat > "$root/bin/xattr" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
exit 0
EOF

    cat > "$root/bin/dscl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exit 1
EOF

    cat > "$root/bin/ditto" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
cp -R "$1" "$2"
EOF

    cat > "$root/bin/defaults" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
# `defaults read <path-without-.plist> <key>` - the fallback the installer uses when
# /usr/libexec/PlistBuddy is absent (it always is on Linux).
[ "${1:-}" = "read" ] || exit 1
python3 - "$2" "$3" <<'PY'
import plistlib, sys
path, key = sys.argv[1], sys.argv[2]
with open(path + ".plist", "rb") as fh:
    data = plistlib.load(fh)
value = data.get(key)
if value is None:
    sys.exit(1)
print(value)
PY
EOF

    # A stand-in for macdeployqt: deploys what Qt would (WebEngine + WebChannel + the render helper
    # and the Chromium data files) but deliberately NOT QtPositioning, so the closure pass's copy
    # path is exercised too. It also rewrites Contents/Resources/qt.conf, which is what the installer
    # has to back up and restore.
    cat > "$root/bin/macdeployqt" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
kate="${1:?macdeployqt needs the bundle}"
printf 'macdeployqt %s\n' "$*" >> "${FAKE_TOOL_LOG:-/dev/null}"
if [ "${FAKE_MACDEPLOYQT_FAIL:-0}" = "1" ]; then
    printf 'macdeployqt: ERROR: could not find dependency\n' >&2
    exit 1
fi
mkdir -p "$kate/Contents/Frameworks" "$kate/Contents/Resources"
for fw in QtWebEngineCore QtWebEngineWidgets QtWebChannel; do
    mkdir -p "$kate/Contents/Frameworks/$fw.framework/Versions/A/Resources"
    : > "$kate/Contents/Frameworks/$fw.framework/Versions/A/$fw"
    ln -sfn A "$kate/Contents/Frameworks/$fw.framework/Versions/Current"
done
mkdir -p "$kate/Contents/Frameworks/QtWebEngineProcess.app/Contents/MacOS"
: > "$kate/Contents/Frameworks/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
: > "$kate/Contents/Resources/icudtl.dat"
: > "$kate/Contents/Resources/qtwebengine_resources.pak"
mkdir -p "$kate/Contents/Resources/qtwebengine_locales"
: > "$kate/Contents/Resources/qtwebengine_locales/en-US.pak"
printf '[Paths]\nPlugins = PlugIns\n' > "$kate/Contents/Resources/qt.conf"
exit 0
EOF

    cat > "$root/bin/brew" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "${FAKE_CONFIG:?}"
case "$*" in
    "--prefix") printf '%s\n' "${FAKE_BREW_PREFIX:-/opt/homebrew}" ;;
    "--prefix qtwebengine")    printf '%s\n' "$FAKE_ROOT/qtwebengine" ;;
    "--prefix qtbase")         printf '%s\n' "$FAKE_ROOT/qtbase" ;;
    "--prefix qt")             printf '%s\n' "$FAKE_ROOT/qt" ;;
    "--prefix qttools")        printf '%s\n' "$FAKE_ROOT/qttools" ;;
    "--prefix qtwebchannel")   printf '%s\n' "$FAKE_ROOT/qtwebchannel" ;;
    "--prefix qtpositioning")  printf '%s\n' "$FAKE_ROOT/qtpositioning" ;;
    "list --versions qtbase")  printf 'qtbase %s\n' "${FAKE_BREW_QT:-6.11.2}" ;;
    install*)                  printf 'brew install %s\n' "$*" >> "${FAKE_TOOL_LOG:-/dev/null}"; exit 1 ;;
    *) exit 1 ;;
esac
EOF

    chmod +x "$root"/bin/*
    : > "$root/tool.log"
    : > "$root/codesign.log"
}

write_fake_config() { # $1 = fake root, rest = extra config lines
    local root="$1"
    shift
    {
        printf 'FAKE_ROOT=%s\n' "$root"
        printf 'FAKE_TOOL_LOG=%s/tool.log\n' "$root"
        printf 'FAKE_CODESIGN_LOG=%s/codesign.log\n' "$root"
        printf 'FAKE_ARCH=%s\n' "${FAKE_ARCH:-arm64}"
        printf 'FAKE_ROSETTA=%s\n' "${FAKE_ROSETTA:-0}"
        printf 'FAKE_PLUGIN_ARCHS=%s\n' "${FAKE_PLUGIN_ARCHS:-arm64}"
        printf 'FAKE_KATE_RUNNING=%s\n' "${FAKE_KATE_RUNNING:-0}"
        printf 'FAKE_BREW_QT=%s\n' "${FAKE_BREW_QT:-6.11.2}"
        printf 'FAKE_BREW_PREFIX=%s\n' "${FAKE_BREW_PREFIX:-/opt/homebrew}"
        printf 'FAKE_CODESIGN_DEEP_BROKEN=%s\n' "${FAKE_CODESIGN_DEEP_BROKEN:-0}"
        printf 'FAKE_MACDEPLOYQT_FAIL=%s\n' "${FAKE_MACDEPLOYQT_FAIL:-0}"
        printf 'FAKE_REFS_DIR=%s/refs\n' "$root"
        printf 'FAKE_MDFIND=%s\n' "${FAKE_MDFIND:-}"
        for line in "$@"; do printf '%s\n' "$line"; done
    } > "$root/fake.env"
}

# A fake framework: <root>/<pre>/lib/<name>.framework/Versions/A/{<name>,Resources/Info.plist}
make_framework() { # $1 = lib dir, $2 = framework name, $3 = version
    local lib="$1" name="$2" ver="$3"
    mkdir -p "$lib/$name.framework/Versions/A/Resources"
    : > "$lib/$name.framework/Versions/A/$name"
    ln -sfn A "$lib/$name.framework/Versions/Current"
    cat > "$lib/$name.framework/Versions/A/Resources/Info.plist" <<PLC
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key>
	<string>org.qt-project.$name</string>
	<key>CFBundleShortVersionString</key>
	<string>$ver</string>
</dict>
</plist>
PLC
}

# The fake Kate: shipped Qt (all of it the plugin needs except WebEngine, WebChannel, Positioning),
# the KF6 library, the KDE plugin dir, a decoy file that must survive uninstall, and a qt.conf.
make_kate() { # $1 = bundle path, $2 = Kate's Qt version
    local kate="$1" qtver="$2" fw
    mkdir -p "$kate/Contents/MacOS" "$kate/Contents/Resources" "$kate/Contents/PlugIns/kf6/ktexteditor"
    : > "$kate/Contents/MacOS/kate"
    chmod +x "$kate/Contents/MacOS/kate"
    cat > "$kate/Contents/Info.plist" <<PLC
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key>
	<string>org.kde.Kate</string>
	<key>LSMinimumSystemVersion</key>
	<string>12.0</string>
</dict>
</plist>
PLC
    for fw in QtCore QtGui QtWidgets QtNetwork; do
        make_framework "$kate/Contents/Frameworks" "$fw" "$qtver"
    done
    : > "$kate/Contents/Frameworks/libKF6TextEditor.6.dylib"
    printf 'kate own qt.conf\n' > "$kate/Contents/Resources/qt.conf"
    printf 'kate sentinel\n' > "$kate/Contents/Resources/kate-sentinel.txt"
}

# The fake plugin: a file plus the reference list fake otool hands out (rewritten in place by fake
# install_name_tool, so the closure pass is tested against its own output).
make_plugin() { # $1 = plugin path, $2 = extra ref lines (newline separated, may be empty)
    local plugin="$1" extra="${2:-}"
    mkdir -p "$(dirname "$plugin")/refs"
    printf 'not really a dylib\n' > "$plugin"
    {
        printf '/opt/homebrew/opt/qt/lib/QtCore.framework/Versions/A/QtCore\n'
        printf '/opt/homebrew/opt/qt/lib/QtGui.framework/Versions/A/QtGui\n'
        printf '/opt/homebrew/opt/qt/lib/QtWidgets.framework/Versions/A/QtWidgets\n'
        printf '/opt/homebrew/opt/qt/lib/QtNetwork.framework/Versions/A/QtNetwork\n'
        printf '/opt/homebrew/opt/qt/lib/QtWebEngineWidgets.framework/Versions/A/QtWebEngineWidgets\n'
        printf '/opt/homebrew/opt/qtwebchannel/lib/QtWebChannel.framework/Versions/A/QtWebChannel\n'
        printf '/opt/homebrew/opt/qtpositioning/lib/QtPositioning.framework/Versions/A/QtPositioning\n'
        printf '/opt/homebrew/opt/qtwebengine/lib/QtWebEngineCore.framework/Versions/A/QtWebEngineCore\n'
        printf '/Users/runner/kf6/lib/libKF6TextEditor.6.dylib\n'
        printf '/usr/lib/libSystem.B.dylib\n'
        [ -n "$extra" ] && printf '%s\n' "$extra"
    } > "$(dirname "$plugin")/refs/$(basename "$plugin").refs"
}

# The reference list the fake otool reports for the plugin, wherever it has been installed to.
plugin_refs_file() { # $1 = env root
    printf '%s/refs/katexdown.so.refs' "$1"
}

# A fresh fake machine: bin/, Kate.app, plugin, Homebrew Qt trees. Echoes the environment root.
setup_env() { # $1 = name, $2 = Kate Qt version [6.11.1]
    local name="$1" kate_qt="${2:-6.11.1}" root
    root="$work_root/$name"
    mkdir -p "$root/tmp" "$root/home"
    make_fakes "$root"
    write_fake_config "$root"
    make_kate "$root/Kate.app" "$kate_qt"
    make_plugin "$root/katexdown.so"

    # Fake Homebrew: Qt frameworks (QtCore for the version) + macdeployqt + one framework that only
    # Homebrew has (QtPositioning) so the closure pass has something to copy.
    mkdir -p "$root/qtwebengine/lib" "$root/qtbase/lib" "$root/qtbase/bin" "$root/qtpositioning/lib"
    make_framework "$root/qtwebengine/lib" QtWebEngineCore 6.11.2
    make_framework "$root/qtbase/lib" QtCore "$(printf '%s' "${FAKE_BREW_QT:-6.11.2}")"
    make_framework "$root/qtpositioning/lib" QtPositioning 6.11.2
    cp "$root/bin/macdeployqt" "$root/qtbase/bin/macdeployqt"
    printf 'qt.libs = %s\n' "$root/qtbase/lib" > "$root/fake.env.sentinel"
    printf '%s\n' "$root"
}

# Changes the Homebrew Qt minor the way a real `brew upgrade` would: the framework's own version
# stamp (which is what the installer reads) and the fake brew's report.
set_fake_brew_qt() { # $1 = env root, $2 = version
    local root="$1" ver="$2"
    python3 - "$root/qtbase/lib/QtCore.framework/Versions/A/Resources/Info.plist" "$ver" <<'PYVER'
import plistlib, sys
path, ver = sys.argv[1], sys.argv[2]
with open(path, "rb") as fh:
    data = plistlib.load(fh)
data["CFBundleShortVersionString"] = ver
with open(path, "wb") as fh:
    plistlib.dump(data, fh)
PYVER
    python3 - "$root/fake.env" "$ver" <<'PYENV'
import sys
path, ver = sys.argv[1], sys.argv[2]
lines = [l for l in open(path).read().splitlines() if not l.startswith("FAKE_BREW_QT=")]
lines.append("FAKE_BREW_QT=" + ver)
open(path, "w").write("\n".join(lines) + "\n")
PYENV
}

# Runs the installer as a user would: fake bin first, all XDG-ish state inside the fake root.
run_installer() { # $1 = env root, rest = installer arguments
    local root="$1"
    shift
    env -i \
        PATH="$root/bin:/usr/bin:/bin" \
        HOME="$root/home" \
        FAKE_CONFIG="$root/fake.env" \
        TMPDIR="$root/tmp" \
        KATEXDOWN_STATE_DIR="$root/state" \
        /bin/bash "$INSTALLER" "$@"
}

mkdir -p "$work_root"

# --------------------------------------------------------------------------- the tests

note "entitlements: the embedded copy must equal packaging/macos/katexdown.entitlements (the CI check)"
if diff -u "$SCRIPT_DIR/katexdown.entitlements" <(/bin/bash "$INSTALLER" --print-entitlements) > "$work_root/ents.diff" 2>&1; then
    check "embedded entitlements match the repo file" "true" || true
else
    fail "embedded entitlements differ from katexdown.entitlements:"; cat "$work_root/ents.diff" >&2
fi

check "--help documents every supported mode" \
    "/bin/bash \"$INSTALLER\" --help | grep -q -- '--uninstall' && /bin/bash \"$INSTALLER\" --help | grep -q -- '--doctor'"

check "an unknown argument is refused" \
    "! /bin/bash \"$INSTALLER\" --bogus 2>/dev/null"

# This suite is normally run on Linux, where the installer must refuse. On macOS the same bare
# invocation is a real doctor run against the host, so the refusal cannot be exercised here.
if [ "$(uname -s)" != "Darwin" ]; then
    out="$(/bin/bash "$INSTALLER" --doctor 2>&1 || true)"
    assert_contains "$out" "for macOS" "the installer refuses to run off macOS"
else
    printf 'test: ok   (off-macOS refusal not applicable: this host is macOS)\n'
    checks=$((checks + 1))
fi

# ---- doctor ------------------------------------------------------------------------------

ENV1="$(setup_env doctor)"
if out="$(run_installer "$ENV1" --doctor --kate "$ENV1/Kate.app" --plugin "$ENV1/katexdown.so" 2>&1)"; then
    printf 'test: ok   doctor passes on a complete fake machine\n'
    checks=$((checks + 1))
else
    fail "doctor should pass on a complete fake machine; got:
$out"
fi
assert_contains "$out" "Kate's Qt minor: 6.11" "doctor reads Kate's Qt minor from Info.plist"
assert_contains "$out" "ok " "doctor reports a summary line"

# Intel Mac: Method 1 is arm64-only, and this must be said loudly (a run of the installer too).
ENV_INTEL="$(setup_env intel)"
sed -i.bak 's/^FAKE_ARCH=.*/FAKE_ARCH=x86_64/' "$ENV_INTEL/fake.env"
rm -f "$ENV_INTEL/fake.env.bak"
out="$(run_installer "$ENV_INTEL" --doctor --kate "$ENV_INTEL/Kate.app" --plugin "$ENV_INTEL/katexdown.so" 2>&1 || true)"
assert_contains "$out" "FAIL" "doctor fails on an Intel Mac"
out="$(run_installer "$ENV_INTEL" --kate "$ENV_INTEL/Kate.app" --plugin "$ENV_INTEL/katexdown.so" 2>&1 || true)"
assert_contains "$out" "only covers arm64" "installing on an Intel Mac is refused with a reason"

# A shell running under Rosetta is still an arm64 Mac.
ENV_ROSETTA="$(setup_env rosetta)"
sed -i.bak -e 's/^FAKE_ARCH=.*/FAKE_ARCH=x86_64/' -e 's/^FAKE_ROSETTA=.*/FAKE_ROSETTA=1/' "$ENV_ROSETTA/fake.env"
rm -f "$ENV_ROSETTA/fake.env.bak"
if run_installer "$ENV_ROSETTA" --doctor --kate "$ENV_ROSETTA/Kate.app" --plugin "$ENV_ROSETTA/katexdown.so" > /dev/null 2>&1; then
    printf 'test: ok   a Rosetta shell on Apple Silicon is treated as arm64\n'
    checks=$((checks + 1))
else
    fail "a Rosetta shell on Apple Silicon should still pass (arch should fold to arm64)"
fi

# Kate found by Spotlight instead of --kate.
ENV_MDFIND="$(setup_env mdfind)"
printf 'FAKE_MDFIND=%s\n' "$ENV_MDFIND/Kate.app" >> "$ENV_MDFIND/fake.env"
# find_kate_app resolves the bundle with pwd -P; on macOS /tmp is a symlink to /private/tmp, so
# compare against the resolved path rather than the raw one.
MDFIND_EXPECTED="$(cd "$ENV_MDFIND/Kate.app" && pwd -P)"
out="$(run_installer "$ENV_MDFIND" --doctor 2>&1 || true)"
assert_contains "$out" "Kate bundle: $MDFIND_EXPECTED" "Kate is found via mdfind"

# No Kate anywhere: the doctor must say so, and name every place it looked.
ENV_NOKATE="$(setup_env nokate)"
out="$(run_installer "$ENV_NOKATE" --doctor 2>&1 || true)"
assert_contains "$out" "no Kate found" "doctor reports a missing Kate"
out="$(run_installer "$ENV_NOKATE" --plugin "$ENV_NOKATE/katexdown.so" 2>&1 || true)"
assert_contains "$out" "no Kate on this Mac" "installer refuses without a Kate"
assert_contains "$out" "Looked at" "the refusal names the search locations"

# ---- the Qt minor guard ------------------------------------------------------------------

ENV_QT="$(setup_env qtminor 6.10.1)"
out="$(run_installer "$ENV_QT" --kate "$ENV_QT/Kate.app" --plugin "$ENV_QT/katexdown.so" 2>&1 || true)"
assert_contains "$out" "Qt minor mismatch: this plugin is built for Qt 6.11, Kate uses Qt 6.10" "a Kate on another Qt minor is refused"
assert_contains "$out" "never appears in Kate's plugin list" "the refusal explains the silent failure mode"

# Patch drift must NOT be refused: Kate 6.11.0 against a 6.11 plugin is fine.
ENV_PATCH="$(setup_env qtpatch 6.11.0)"
if run_installer "$ENV_PATCH" --kate "$ENV_PATCH/Kate.app" --plugin "$ENV_PATCH/katexdown.so" > "$work_root/patch.log" 2>&1; then
    printf 'test: ok   patch drift (6.11.0 vs 6.11) installs\n'
    checks=$((checks + 1))
else
    fail "patch drift must be accepted, but the install failed:
$(cat "$work_root/patch.log")"
fi

# Homebrew moving past Kate's Qt is a real dead end and must abort with the reason.
ENV_BREWQT="$(setup_env brewqt)"
set_fake_brew_qt "$ENV_BREWQT" 6.12.0
out="$(run_installer "$ENV_BREWQT" --kate "$ENV_BREWQT/Kate.app" --plugin "$ENV_BREWQT/katexdown.so" 2>&1 || true)"
assert_contains "$out" "Homebrew's Qt is 6.12" "a Homebrew Qt past Kate's is refused"

# ---- a full install ---------------------------------------------------------------------

ENV_OK="$(setup_env install)"
if out="$(run_installer "$ENV_OK" --kate "$ENV_OK/Kate.app" --plugin "$ENV_OK/katexdown.so" 2>&1)"; then
    printf 'test: ok   install succeeds on the fake machine\n'
    checks=$((checks + 1))
else
    fail "install failed on the fake machine:
$out"
fi
KATE_OK="$ENV_OK/Kate.app"
PLUGIN_DST="$KATE_OK/Contents/PlugIns/kf6/ktexteditor/katexdown.so"
MANIFEST="$ENV_OK/state/installed-files.txt"
REFS_OK="$(plugin_refs_file "$ENV_OK")"

check "the plugin is installed where Kate looks for KDE plugins" "[ -f \"$PLUGIN_DST\" ]"
check "the engine frames were deployed by macdeployqt" "[ -d \"$KATE_OK/Contents/Frameworks/QtWebEngineCore.framework\" ]"
check "the closure pass copied QtPositioning in from Homebrew" \
    "[ -d \"$KATE_OK/Contents/Frameworks/QtPositioning.framework\" ]"
check "the render helper and Chromium data files are inside the bundle" \
    "[ -d \"$KATE_OK/Contents/Frameworks/QtWebEngineProcess.app\" ] && [ -f \"$KATE_OK/Contents/Resources/icudtl.dat\" ]"

if grep -q '@executable_path/../Frameworks/QtCore.framework/Versions/Current/QtCore' "$REFS_OK"; then
    printf 'test: ok   Qt references were rewritten to @executable_path/../Frameworks\n'
    checks=$((checks + 1))
else
    fail "QtCore was not rewritten; refs are:
$(cat "$REFS_OK")"
fi
check "no reference is left pointing outside the bundle" \
    "! grep -qE '^(/opt/homebrew|/Users/)' \"$REFS_OK\""
check "KF6 is rewritten to Kate's own library instead of being copied in" \
    "grep -q '@executable_path/../Frameworks/libKF6TextEditor.6.dylib' \"$REFS_OK\""
check "the manifest was written" "[ -f \"$MANIFEST\" ]"
check "the manifest lists the plugin" "grep -q '^./Contents/PlugIns/kf6/ktexteditor/katexdown.so$' \"$MANIFEST\""
check "the manifest lists the frameworks the closure pass copied" \
    "grep -q 'QtPositioning.framework' \"$MANIFEST\""
check "the manifest does NOT list Kate's own files" \
    "! grep -q 'kate-sentinel.txt' \"$MANIFEST\" && ! grep -q '_CodeSignature' \"$MANIFEST\""
check "the bundle was signed with the runtime and the entitlements" \
    "grep -q 'options runtime' \"$ENV_OK/codesign.log\" && grep -q 'katexdown.entitlements' \"$ENV_OK/codesign.log\""
check "the signature was verified strictly" "grep -q -- '--verify --deep --strict' \"$ENV_OK/codesign.log\""
check "Kate's original qt.conf was saved" "[ -f \"$ENV_OK/state/qt.conf.original\" ]"

# Re-running must be idempotent: same manifest, same outcome, no duplication.
if out="$(run_installer "$ENV_OK" --kate "$ENV_OK/Kate.app" --plugin "$ENV_OK/katexdown.so" 2>&1)"; then
    printf 'test: ok   re-running the installer succeeds (idempotent)\n'
    checks=$((checks + 1))
else
    fail "re-running the installer failed:
$out"
fi
check "the manifest did not grow on the second run" \
    "[ \"\$(grep -c . \"$MANIFEST\")\" = \"\$(sort -u \"$MANIFEST\" | grep -c .)\" ]"
check "the plugin still resolves inside the bundle after a re-run" \
    "! grep -qE '^(/opt/homebrew|/Users/)' \"$PLUGIN_DST.refs\""

# A failure inside macdeployqt must stop the install (a half-deployed bundle is worse than none).
ENV_MDQ="$(setup_env mdqfail)"
sed -i.bak 's/^FAKE_MACDEPLOYQT_FAIL=.*/FAKE_MACDEPLOYQT_FAIL=1/' "$ENV_MDQ/fake.env"
rm -f "$ENV_MDQ/fake.env.bak"
out="$(run_installer "$ENV_MDQ" --kate "$ENV_MDQ/Kate.app" --plugin "$ENV_MDQ/katexdown.so" 2>&1 || true)"
assert_contains "$out" "could not find dependency" "a macdeployqt failure is surfaced"
check "no manifest is written when macdeployqt fails" "[ ! -f \"$ENV_MDQ/state/installed-files.txt\" ]"

# Kate running must be refused (replacing loaded frameworks fails, re-signing a live bundle fails).
ENV_RUN="$(setup_env running)"
sed -i.bak 's/^FAKE_KATE_RUNNING=.*/FAKE_KATE_RUNNING=1/' "$ENV_RUN/fake.env"
rm -f "$ENV_RUN/fake.env.bak"
out="$(run_installer "$ENV_RUN" --kate "$ENV_RUN/Kate.app" --plugin "$ENV_RUN/katexdown.so" 2>&1 || true)"
assert_contains "$out" "Kate is running" "installing into a running Kate is refused"

# A KF6 library Kate does not ship must stop the install instead of being copied in.
ENV_KF6="$(setup_env kf6)"
python3 - "$(plugin_refs_file "$ENV_KF6")" <<'PY'
import sys
p = sys.argv[1]
open(p, "a").write("/Users/runner/kf6/lib/libKF6XmlGui.6.dylib\n")
PY
out="$(run_installer "$ENV_KF6" --kate "$ENV_KF6/Kate.app" --plugin "$ENV_KF6/katexdown.so" 2>&1 || true)"
assert_contains "$out" "does not ship it" "a KF6 library Kate lacks stops the install"
check "the foreign KF6 library was not copied into Kate" \
    "[ ! -e \"$ENV_KF6/Kate.app/Contents/Frameworks/libKF6XmlGui.6.dylib\" ]"

# A plugin that cannot be resolved at all (no macdeployqt, no Homebrew Qt) must fail loudly.
ENV_WIDGETS="$(setup_env widgets)"
python3 - "$(plugin_refs_file "$ENV_WIDGETS")" <<'PY'
import sys
p = sys.argv[1]
open(p, "a").write("/opt/homebrew/opt/qt/lib/QtSql.framework/Versions/A/QtSql\n")
PY
out="$(run_installer "$ENV_WIDGETS" --kate "$ENV_WIDGETS/Kate.app" --plugin "$ENV_WIDGETS/katexdown.so" 2>&1 || true)"
assert_contains "$out" "Homebrew has none" "an unresolvable framework is reported, not guessed"

# An arm64 plugin host cannot load an x86_64 plugin.
ENV_ARCH="$(setup_env arch)"
sed -i.bak 's/^FAKE_PLUGIN_ARCHS=.*/FAKE_PLUGIN_ARCHS=x86_64/' "$ENV_ARCH/fake.env"
rm -f "$ENV_ARCH/fake.env.bak"
out="$(run_installer "$ENV_ARCH" --kate "$ENV_ARCH/Kate.app" --plugin "$ENV_ARCH/katexdown.so" 2>&1 || true)"
assert_contains "$out" "is not a arm64 binary" "a mismatched plugin architecture is refused"

# ---- uninstall ---------------------------------------------------------------------------

if out="$(run_installer "$ENV_OK" --uninstall --kate "$ENV_OK/Kate.app" 2>&1)"; then
    printf 'test: ok   uninstall succeeds\n'
    checks=$((checks + 1))
else
    fail "uninstall failed:
$out"
fi
check "the plugin is gone" "[ ! -e \"$PLUGIN_DST\" ]"
check "the copied engine frameworks are gone" "[ ! -e \"$KATE_OK/Contents/Frameworks/QtPositioning.framework\" ]"
check "Kate's own Qt framework survived" "[ -e \"$KATE_OK/Contents/Frameworks/QtCore.framework/Versions/A/QtCore\" ]"
check "Kate's own KF6 library survived" "[ -e \"$KATE_OK/Contents/Frameworks/libKF6TextEditor.6.dylib\" ]"
check "the decoy file survived" "[ -f \"$KATE_OK/Contents/Resources/kate-sentinel.txt\" ]"
check "Kate's original qt.conf was restored" "grep -q 'kate own qt.conf' \"$KATE_OK/Contents/Resources/qt.conf\""
check "the manifest was removed" "[ ! -f \"$MANIFEST\" ]"
check "the bundle was re-signed after the removal" \
    "[ \"\$(grep -c -- '--verify --deep --strict' \"$ENV_OK/codesign.log\")\" -ge 2 ]"

# Uninstalling twice: no manifest any more, so it must refuse unless forced.
out="$(run_installer "$ENV_OK" --uninstall --kate "$ENV_OK/Kate.app" 2>&1 || true)"
assert_contains "$out" "no install manifest" "a second uninstall refuses instead of guessing"
out="$(run_installer "$ENV_OK" --uninstall --force --kate "$ENV_OK/Kate.app" 2>&1 || true)"
assert_contains "$out" "removing only the plugin" "--force downgrades to removing just the plugin"

# ---- the --deep fallback -----------------------------------------------------------------

ENV_DEEP="$(setup_env deepfallback)"
sed -i.bak 's/^FAKE_CODESIGN_DEEP_BROKEN=.*/FAKE_CODESIGN_DEEP_BROKEN=1/' "$ENV_DEEP/fake.env"
rm -f "$ENV_DEEP/fake.env.bak"
if out="$(run_installer "$ENV_DEEP" --kate "$ENV_DEEP/Kate.app" --plugin "$ENV_DEEP/katexdown.so" 2>&1)"; then
    printf 'test: ok   install survives a broken --deep sign (bottom-up fallback)\n'
    checks=$((checks + 1))
else
    fail "the bottom-up signing fallback did not recover from a failing --deep sign:
$out"
fi
assert_contains "$out" "bottom-up signing" "the fallback says what it is doing"
check "the fallback signed nested code too" \
    "grep -q 'Contents/Frameworks/QtWebEngineCore.framework' \"$ENV_DEEP/codesign.log\""

# ---- --no-brew ---------------------------------------------------------------------------

ENV_NOBREW="$(setup_env nobrew)"
rm -rf "$ENV_NOBREW/qtwebengine" "$ENV_NOBREW/qtpositioning"
out="$(run_installer "$ENV_NOBREW" --no-brew --kate "$ENV_NOBREW/Kate.app" --plugin "$ENV_NOBREW/katexdown.so" 2>&1 || true)"
assert_contains "$out" "WebEngine is not installed" "--no-brew reports the missing engine instead of installing it"
check "no brew install was attempted" "! grep -q 'brew install' \"$ENV_NOBREW/tool.log\""

# --------------------------------------------------------------------------- result

if [ "$failures" -eq 0 ]; then
    printf 'test: PASS (%s checks)\n' "$checks"
    exit 0
fi
printf 'test: FAILED (%s of %s checks failed)\n' "$failures" "$checks" >&2
exit 1
