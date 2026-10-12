#!/usr/bin/env bash
#
# katexdown - build the pinned KF6 frameworks the macOS plugin links against.
#
# WHY THIS EXISTS
#   Kate for macOS ships KF6 libraries but Homebrew has no KF6 formula (only a KF5 tap), and the
#   plugin needs KTextEditor headers + a link-time libKF6TextEditor. So CI builds KF6 from source,
#   against Homebrew's Qt, once per pinned version, and caches the prefix. Only Homebrew's Qt is
#   used: the finished plugin resolves Kate's own Qt/KF6 copies at install time (install-macos.sh
#   rewrites every reference), so the build prefix is a means to an end, not something shipped.
#
# THE LIST IS THE KTextEditor CLOSURE
#   The repositories below are the transitive build dependency closure of KTextEditor 6.30,
#   generated from KDE's repo-metadata (dependency-data-kf6-qt6) and ordered topologically. Two
#   entries from that closure are deliberately omitted:
#     * kdoctools   - only needed for translated docs; CMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON
#                     makes kservice/kio skip it (it is an optional find_package in both).
#     * breeze-icons- only pulled in by KIconThemes' USE_BreezeIcons option, which is set to OFF.
#                     The plugin does not ship icons; the host app supplies the theme.
#   If a future KF6 release adds a required dependency, the configure of the framework that needs
#   it fails loudly with the missing package name, and this list must be updated. Bump
#   KATEXDOWN_KF6_VERSION only together with build-plugin.sh / the CI cache key.
#
# FLAGS THAT MATTER
#   BUILD_TESTING=OFF     - no test frameworks, no extra deps; the plugin's own tests are built by
#                           build-plugin.sh, not here.
#   CMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON - see above (docs only).
#   USE_BreezeIcons=OFF   - see above.
#   BUILD_DESIGNERPLUGIN=OFF - KIO's Qt Designer plugin is dead weight for a headless build.
#   CMAKE_OSX_ARCHITECTURES=arm64 - Method 1 is arm64-only (design/macos-install.md).
#   BISON_EXECUTABLE/FLEX_EXECUTABLE - macOS ships bison 2.3, and Solid (a KIO dependency)
#                           needs >= 3.0; Homebrew's bison is keg-only, so its path is passed in.
#
# NOT VERIFIED ON macOS YET: the KF6 configure/build was not run on a Mac in this repository. The
# first CI run is the check; packaging/macos/README.md lists what to look at if it fails.

set -euo pipefail

PROG="build-kf6.sh"

# Resolved in main(), used by build_one().
KF6_BISON_EXE=""
KF6_FLEX_EXE=""

KATEXDOWN_KF6_VERSION="${KATEXDOWN_KF6_VERSION:-6.30.0}"

log()  { printf '%s: %s\n' "$PROG" "$*" >&2; }
warn() { printf '%s: warning: %s\n' "$PROG" "$*" >&2; }
die()  { printf '%s: error: %s\n' "$PROG" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
${PROG} - build the pinned KF6 frameworks for macOS (arm64)

Usage: ${PROG} [options]

Options:
  --prefix DIR       install prefix (default \$KATEXDOWN_KF6_PREFIX or ~/.local/katexdown-kf6)
  --qt-prefix DIR    Homebrew Qt prefix (default: \$(brew --prefix qt))
  --cache-dir DIR    downloads/extracted sources/build logs (default \$HOME/.cache/katexdown-kf6)
  --jobs N           parallel jobs (default: CPU count)
  --force            rebuild frameworks even when the prefix already has them
  --print-version    print the pinned KF6 version and exit
  -h, --help         this text

The prefix is written to be cached: a framework whose CMake config already exists there is skipped,
so restoring the CI cache and re-running this script is a no-op.
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

find_brew() {
    local b=""
    if command -v brew >/dev/null 2>&1; then
        b="$(command -v brew)"
    elif [ -x /opt/homebrew/bin/brew ]; then
        b="/opt/homebrew/bin/brew"
    fi
    [ -n "$b" ] || die "Homebrew not found; run 'brew install cmake ninja qt gettext bison' first"
    printf '%s' "$b"
}

# repo:installed-config-relative-to-prefix, in topological order (see the header).
framework_list() {
    cat <<'LIST'
extra-cmake-modules:share/ECM/cmake/ECMConfig.cmake
karchive:lib/cmake/KF6Archive/KF6ArchiveConfig.cmake
kcoreaddons:lib/cmake/KF6CoreAddons/KF6CoreAddonsConfig.cmake
kwindowsystem:lib/cmake/KF6WindowSystem/KF6WindowSystemConfig.cmake
kauth:lib/cmake/KF6Auth/KF6AuthConfig.cmake
kconfig:lib/cmake/KF6Config/KF6ConfigConfig.cmake
kwidgetsaddons:lib/cmake/KF6WidgetsAddons/KF6WidgetsAddonsConfig.cmake
kbookmarks:lib/cmake/KF6Bookmarks/KF6BookmarksConfig.cmake
kcodecs:lib/cmake/KF6Codecs/KF6CodecsConfig.cmake
kguiaddons:lib/cmake/KF6GuiAddons/KF6GuiAddonsConfig.cmake
ki18n:lib/cmake/KF6I18n/KF6I18nConfig.cmake
kcolorscheme:lib/cmake/KF6ColorScheme/KF6ColorSchemeConfig.cmake
kcompletion:lib/cmake/KF6Completion/KF6CompletionConfig.cmake
kconfigwidgets:lib/cmake/KF6ConfigWidgets/KF6ConfigWidgetsConfig.cmake
kcrash:lib/cmake/KF6Crash/KF6CrashConfig.cmake
kdbusaddons:lib/cmake/KF6DBusAddons/KF6DBusAddonsConfig.cmake
kglobalaccel:lib/cmake/KF6GlobalAccel/KF6GlobalAccelConfig.cmake
kiconthemes:lib/cmake/KF6IconThemes/KF6IconThemesConfig.cmake
kitemviews:lib/cmake/KF6ItemViews/KF6ItemViewsConfig.cmake
knotifications:lib/cmake/KF6Notifications/KF6NotificationsConfig.cmake
kjobwidgets:lib/cmake/KF6JobWidgets/KF6JobWidgetsConfig.cmake
kservice:lib/cmake/KF6Service/KF6ServiceConfig.cmake
solid:lib/cmake/KF6Solid/KF6SolidConfig.cmake
kio:lib/cmake/KF6KIO/KF6KIOConfig.cmake
sonnet:lib/cmake/KF6Sonnet/KF6SonnetConfig.cmake
ktextwidgets:lib/cmake/KF6TextWidgets/KF6TextWidgetsConfig.cmake
kxmlgui:lib/cmake/KF6XmlGui/KF6XmlGuiConfig.cmake
kparts:lib/cmake/KF6Parts/KF6PartsConfig.cmake
syntax-highlighting:lib/cmake/KF6SyntaxHighlighting/KF6SyntaxHighlightingConfig.cmake
ktexteditor:lib/cmake/KF6TextEditor/KF6TextEditorConfig.cmake
LIST
}

build_one() { # $1 repo, $2 version, $3 prefix, $4 qt_prefix, $5 cache_dir, $6 jobs
    local repo="$1" version="$2" prefix="$3" qt_prefix="$4" cache_dir="$5" jobs="$6"
    local series src_dir build_dir log_file tarball url
    series="$(printf '%s' "$version" | awk -F. '{print $1"."$2}')"
    tarball="$cache_dir/downloads/${repo}-${version}.tar.xz"
    src_dir="$cache_dir/src/${repo}-${version}"
    build_dir="$cache_dir/build/$repo"
    log_file="$cache_dir/logs/$repo.log"
    url="https://download.kde.org/stable/frameworks/${series}/${repo}-${version}.tar.xz"

    mkdir -p "$cache_dir/downloads" "$cache_dir/src" "$cache_dir/build" "$cache_dir/logs"
    if [ ! -f "$tarball" ]; then
        log "downloading ${repo}-${version}"
        curl -fSL --retry 3 --connect-timeout 20 -o "$tarball.part" "$url" \
            || die "could not download $url"
        mv "$tarball.part" "$tarball"
    fi
    if [ ! -d "$src_dir" ]; then
        tar -xf "$tarball" -C "$cache_dir/src" || die "could not extract $tarball"
        [ -d "$src_dir" ] || die "$tarball did not extract to $src_dir"
    fi

    # Extra flags: harmless (CMake only warns about unused ones) and they remove optional deps.
    log "building $repo (log: $log_file)"
    rm -rf "$build_dir"
    if ! cmake -B "$build_dir" -S "$src_dir" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$prefix" \
            -DCMAKE_PREFIX_PATH="$prefix;$qt_prefix" \
            -DCMAKE_OSX_ARCHITECTURES=arm64 \
            -DBUILD_TESTING=OFF \
            -DBUILD_QCH=OFF \
            -DBUILD_EXAMPLES=OFF \
            -DBUILD_DESIGNERPLUGIN=OFF \
            -DCMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON \
            -DUSE_BreezeIcons=OFF \
            -DBISON_EXECUTABLE="$KF6_BISON_EXE" \
            -DFLEX_EXECUTABLE="$KF6_FLEX_EXE" \
            > "$log_file" 2>&1; then
        tail -n 40 "$log_file" >&2 || true
        die "$repo: cmake configure failed (full log: $log_file)"
    fi
    if ! cmake --build "$build_dir" --parallel "$jobs" >> "$log_file" 2>&1; then
        tail -n 60 "$log_file" >&2 || true
        die "$repo: build failed (full log: $log_file)"
    fi
    if ! cmake --install "$build_dir" >> "$log_file" 2>&1; then
        tail -n 60 "$log_file" >&2 || true
        die "$repo: install failed (full log: $log_file)"
    fi
}

main() {
    local prefix="${KATEXDOWN_KF6_PREFIX:-$HOME/.local/katexdown-kf6}"
    local qt_prefix_arg=""
    local cache_dir="${KATEXDOWN_KF6_CACHE:-$HOME/.cache/katexdown-kf6}"
    local jobs force="0"
    jobs="$(sysctl -n hw.ncpu 2>/dev/null || printf '4')"

    while [ $# -gt 0 ]; do
        case "$1" in
            --prefix)      [ $# -ge 2 ] || die "--prefix needs a path"; prefix="$2"; shift 2 ;;
            --qt-prefix)   [ $# -ge 2 ] || die "--qt-prefix needs a path"; qt_prefix_arg="$2"; shift 2 ;;
            --cache-dir)   [ $# -ge 2 ] || die "--cache-dir needs a path"; cache_dir="$2"; shift 2 ;;
            --jobs)        [ $# -ge 2 ] || die "--jobs needs a number"; jobs="$2"; shift 2 ;;
            --force)       force="1"; shift ;;
            --print-version) printf '%s\n' "$KATEXDOWN_KF6_VERSION"; exit 0 ;;
            -h|--help)     usage; exit 0 ;;
            *) die "unknown argument '$1' (see --help)" ;;
        esac
    done

    [ "$(uname -s)" = "Darwin" ] || die "this builds macOS KF6 frameworks; it must run on macOS"
    [ "$(host_arch)" = "arm64" ] || die "this Mac is $(host_arch); Method 1 is arm64-only"

    command -v cmake >/dev/null 2>&1 || die "cmake not found; run 'brew install cmake ninja'"
    command -v ninja >/dev/null 2>&1 || die "ninja not found; run 'brew install cmake ninja'"
    command -v msgfmt >/dev/null 2>&1 || warn "msgfmt not found; some frameworks' translations may fail. Install gettext: 'brew install gettext'"

    local brew qt_prefix
    brew="$(find_brew)"
    qt_prefix="$qt_prefix_arg"
    [ -n "$qt_prefix" ] || qt_prefix="$("$brew" --prefix qt 2>/dev/null || true)"
    [ -n "$qt_prefix" ] || die "Homebrew's 'qt' formula is not installed; run 'brew install qt'"
    [ -d "$qt_prefix/lib/cmake/Qt6" ] || die "'$qt_prefix' does not look like a Qt prefix (no lib/cmake/Qt6)"

    # Solid needs bison >= 3.0; macOS ships 2.3 and Homebrew's bison is keg-only. Prefer the
    # Homebrew one, then a PATH bison that is new enough.
    local bison_prefix bison_version
    bison_prefix="$("$brew" --prefix bison 2>/dev/null || true)"
    if [ -n "$bison_prefix" ] && [ -x "$bison_prefix/bin/bison" ]; then
        KF6_BISON_EXE="$bison_prefix/bin/bison"
    elif command -v bison >/dev/null 2>&1; then
        KF6_BISON_EXE="$(command -v bison)"
    fi
    [ -n "$KF6_BISON_EXE" ] || die "bison not found; run 'brew install bison' (macOS's 2.3 is too old for Solid)"
    bison_version="$("$KF6_BISON_EXE" --version 2>/dev/null | awk 'NR == 1 { print $NF; exit }' || true)"
    case "$bison_version" in
        3.*|4.*) : ;;
        *) die "$KF6_BISON_EXE is bison $bison_version; Solid requires >= 3.0. Run 'brew install bison' and re-run." ;;
    esac
    log "bison: $KF6_BISON_EXE ($bison_version)"

    local flex_prefix
    if command -v flex >/dev/null 2>&1; then
        KF6_FLEX_EXE="$(command -v flex)"
    else
        flex_prefix="$("$brew" --prefix flex 2>/dev/null || true)"
        [ -n "$flex_prefix" ] && KF6_FLEX_EXE="$flex_prefix/bin/flex"
    fi
    [ -n "$KF6_FLEX_EXE" ] && [ -x "$KF6_FLEX_EXE" ] || die "flex not found; install the Xcode command line tools ('xcode-select --install')"

    mkdir -p "$prefix"
    log "KF6 $KATEXDOWN_KF6_VERSION -> $prefix (Qt: $qt_prefix, cache: $cache_dir)"
    log "Building KDE Frameworks from source takes a while on the first run; the CI caches $prefix."

    local entry repo check missing="0"
    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        repo="${entry%%:*}"
        check="${entry#*:}"
        if [ "$force" != "1" ] && [ -f "$prefix/$check" ]; then
            log "cached: $repo"
            continue
        fi
        build_one "$repo" "$KATEXDOWN_KF6_VERSION" "$prefix" "$qt_prefix" "$cache_dir" "$jobs"
    done <<EOF
$(framework_list)
EOF

    # The two packages build-plugin.sh cares about, checked loudly so a partial prefix cannot look
    # like a success.
    for check in "lib/cmake/KF6TextEditor/KF6TextEditorConfig.cmake" "lib/cmake/KF6Config/KF6ConfigConfig.cmake"; do
        [ -f "$prefix/$check" ] || missing="1"
    done
    [ "$missing" = "0" ] || die "KF6 finished but $prefix is missing KF6TextEditor/KF6Config; the build did not complete"
    log "KF6 $KATEXDOWN_KF6_VERSION ready in $prefix"
}

main "$@"
