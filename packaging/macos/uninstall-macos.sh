#!/usr/bin/env bash
#
# katexdown - uninstall front door for macOS.
#
# The uninstaller is `install-macos.sh --uninstall`: install and uninstall share the Kate lookup,
# the running-Kate and Qt-minor guards, the install manifest, the qt.conf backup and the re-signing.
# Keeping them in one script is what stops the two from drifting apart (see
# packaging/macos/install-macos.sh's header). This file is deliberately tiny:
#
#   * if install-macos.sh sits next to this script (a manual download of both release assets, or a
#     checkout), exec that;
#   * otherwise (the documented `curl .../uninstall-macos.sh | bash` path) download
#     install-macos.sh from the same release this script came from, run it, and clean up the temp
#     copy afterwards.
#
# ENVIRONMENT
#   KATEXDOWN_INSTALLER       use this install-macos.sh instead of looking next to this script
#   KATEXDOWN_REPO            GitHub owner/name (default q0ppa/katexdown)
#   KATEXDOWN_RELEASE_BASE    base URL for the release assets (default GitHub "latest download")
#
# Untested on macOS; see packaging/macos/README.md for what must be checked on a Mac.

set -euo pipefail

PROG="uninstall-macos.sh"
KATEXDOWN_REPO="${KATEXDOWN_REPO:-q0ppa/katexdown}"
RELEASE_BASE="${KATEXDOWN_RELEASE_BASE:-https://github.com/${KATEXDOWN_REPO}/releases/latest/download}"

die() { printf '%s: error: %s\n' "$PROG" "$*" >&2; exit 1; }

script_dir() {
    # No readlink -f on macOS.
    local d
    d="$(cd "$(dirname "$0")" 2>/dev/null && pwd -P)" || d="."
    printf '%s' "$d"
}

if [ "$(uname -s)" != "Darwin" ]; then
    die "this uninstaller is for macOS. On Linux remove install-macos.sh's files the way they were installed (the AUR package or cmake --uninstall)."
fi

installer="${KATEXDOWN_INSTALLER:-}"
if [ -z "$installer" ]; then
    sibling="$(script_dir)/install-macos.sh"
    [ -f "$sibling" ] && installer="$sibling"
fi

if [ -n "$installer" ]; then
    [ -f "$installer" ] || die "KATEXDOWN_INSTALLER='$installer' does not exist"
    exec /bin/bash "$installer" --uninstall "$@"
fi

command -v curl >/dev/null 2>&1 || die "install-macos.sh is not next to this script and there is no curl to fetch it"

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/katexdown-uninstall.XXXXXX")"
cleanup() { rm -rf "$tmp_dir"; return 0; }
trap cleanup EXIT

url="${RELEASE_BASE}/install-macos.sh"
printf '%s: install-macos.sh not found next to this script; downloading %s\n' "$PROG" "$url" >&2
curl -fSL --retry 3 --connect-timeout 20 -o "$tmp_dir/install-macos.sh" "$url" \
    || die "could not download $url (pass KATEXDOWN_INSTALLER=/path/to/install-macos.sh)"

# Not exec: the temp copy has to be deleted when this exits.
/bin/bash "$tmp_dir/install-macos.sh" --uninstall "$@"
