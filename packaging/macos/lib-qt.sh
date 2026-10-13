# shellcheck shell=bash
#
# katexdown - shared Qt/Kate detection helpers for the macOS scripts.
#
# SOURCE THIS FILE, DO NOT EXECUTE IT. It defines functions only and has no
# side effects, so build-kf6.sh, build-plugin.sh and provision-qt.sh can all use
# the same detection rules.
#
# WHY
#   Kate bundles its own Qt/KF6 and links Qt *private* symbols, whose ABI tag
#   changes with every Qt patch release (QtPrivate_6_11_1 vs QtPrivate_6_11_2).
#   A plugin built against a different Qt patch installs, is listed, but can
#   never be enabled. The scripts therefore must detect the exact Qt Kate
#   bundles, and the Qt components a build needs, without a hand-maintained
#   list that goes stale on every upstream release.

# --------------------------------------------------------------------------- QtCore

# Path to a Qt prefix's QtCore binary, if present.
kdx_qt_core_binary() { # $1 = Qt prefix
    local p="$1" c
    for c in \
        "$p/lib/QtCore.framework/Versions/A/QtCore" \
        "$p/lib/QtCore.framework/Versions/Current/QtCore" \
        "$p/lib/QtCore.framework/QtCore" \
        "$p/lib/libQt6Core.dylib"; do
        [ -e "$c" ] && { printf '%s' "$c"; return 0; }
    done
    return 1
}

# Exact Qt private ABI tag of a QtCore binary, e.g. "QtPrivate_6_11_1".
kdx_qt_private_abi() { # $1 = QtCore binary
    local bin="$1"
    [ -f "$bin" ] || return 1
    command -v nm >/dev/null 2>&1 || return 1
    nm -gU "$bin" 2>/dev/null \
        | sed -n 's/.*\(QtPrivate_[0-9][0-9]*_[0-9][0-9]*_[0-9][0-9]*\).*/\1/p' \
        | head -1
}

# ABI tag of the QtCore inside a Qt prefix.
kdx_qt_prefix_abi() { # $1 = Qt prefix
    local core
    core="$(kdx_qt_core_binary "$1")" || return 1
    kdx_qt_private_abi "$core"
}

# "QtPrivate_6_11_1" -> "6.11.1"; anything else is echoed unchanged.
kdx_abi_to_version() { # $1 = ABI tag
    printf '%s' "$1" | sed 's/^QtPrivate_//; s/_/./g'
}

# --------------------------------------------------------------------------- versions

# CFBundleShortVersionString of a *.framework directory, if it records one.
kdx_framework_version() { # $1 = path to a *.framework dir
    local fw="$1" p ver
    for p in "$fw/Resources/Info.plist" \
             "$fw/Versions/Current/Resources/Info.plist" \
             "$fw"/Versions/*/Resources/Info.plist; do
        [ -f "$p" ] || continue
        if [ -x /usr/libexec/PlistBuddy ]; then
            ver="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$p" 2>/dev/null || true)"
        fi
        if [ -z "$ver" ]; then
            ver="$(sed -n 's/.*<key>CFBundleShortVersionString<\/key>[[:space:]]*<string>\([^<]*\)<.*/\1/p' "$p" 2>/dev/null | head -1)"
        fi
        [ -n "$ver" ] && { printf '%s' "$ver"; return 0; }
    done
    return 1
}

# Version of a Qt prefix: the framework's own stamp, else qmake, else the exact
# patch reconstructed from the private ABI tag.
kdx_qt_prefix_version() { # $1 = Qt prefix
    local prefix="$1" p ver abi
    ver="$(kdx_framework_version "$prefix/lib/QtCore.framework" || true)"
    [ -n "$ver" ] && { printf '%s' "$ver"; return 0; }
    for p in "$prefix/bin/qmake6" "$prefix/bin/qmake"; do
        [ -x "$p" ] || continue
        ver="$("$p" -query QT_VERSION 2>/dev/null || true)"
        [ -n "$ver" ] && { printf '%s' "$ver"; return 0; }
    done
    abi="$(kdx_qt_prefix_abi "$prefix" || true)"
    [ -n "$abi" ] && { kdx_abi_to_version "$abi"; return 0; }
    return 1
}

# --------------------------------------------------------------------------- Kate

# Locate Kate.app: explicit arg, KATEXDOWN_KATE_APP, Spotlight, then the usual
# install locations. Echoes the resolved bundle path.
kdx_find_kate_app() { # [$1 = explicit bundle path]
    local explicit="${1:-}" c
    if [ -n "$explicit" ]; then
        [ -f "$explicit/Contents/MacOS/kate" ] || return 1
        (cd "$explicit" && pwd -P)
        return 0
    fi
    if [ -n "${KATEXDOWN_KATE_APP:-}" ] && [ -f "$KATEXDOWN_KATE_APP/Contents/MacOS/kate" ]; then
        (cd "$KATEXDOWN_KATE_APP" && pwd -P)
        return 0
    fi
    if command -v mdfind >/dev/null 2>&1; then
        while IFS= read -r c; do
            [ -n "$c" ] || continue
            if [ -f "$c/Contents/MacOS/kate" ]; then (cd "$c" && pwd -P); return 0; fi
        done < <(mdfind 'kMDItemCFBundleIdentifier == "org.kde.Kate"' 2>/dev/null | head -20 || true)
    fi
    for c in /Applications/[Kk]ate.app "$HOME/Applications/[Kk]ate.app"; do
        if [ -f "$c/Contents/MacOS/kate" ]; then (cd "$c" && pwd -P); return 0; fi
    done
    return 1
}

# Kate's bundled QtCore binary (versioned inside Contents/Frameworks).
kdx_kate_qt_core_binary() { # $1 = Kate.app
    local fw="$1/Contents/Frameworks/QtCore.framework"
    for c in "$fw/Versions/A/QtCore" "$fw/Versions/Current/QtCore" "$fw/QtCore"; do
        [ -e "$c" ] && { printf '%s' "$c"; return 0; }
    done
    return 1
}

# Exact Qt version Kate bundles, e.g. "6.11.1". Reconstructed from the private
# ABI tag, which carries the patch (Kate's Info.plist only records "6.11").
kdx_kate_qt_version() { # $1 = Kate.app
    local core abi ver
    core="$(kdx_kate_qt_core_binary "$1")" || return 1
    abi="$(kdx_qt_private_abi "$core" || true)"
    if [ -n "$abi" ]; then kdx_abi_to_version "$abi"; return 0; fi
    ver="$(kdx_framework_version "$1/Contents/Frameworks/QtCore.framework" || true)"
    [ -n "$ver" ] && { printf '%s' "$ver"; return 0; }
    return 1
}

# --------------------------------------------------------------------------- components

# Extract the Qt components named by find_package(Qt6 ...) calls. Components
# after OPTIONAL_COMPONENTS are dropped (the build's optional deps), as are
# keywords, version literals and ${...} variables. Reads one file per argument.
_kdx_components_of_file() { # $1 = a CMakeLists.txt
    perl -0777 -ne '
        s{#.*?$}{}mg;
        while (/find_package\s*\(\s*Qt6\b\s*([^)]*)\)/g) {
            my @t = split /\s+/, $1;
            for my $tok (@t) {
                next if $tok eq "";
                last if $tok eq "OPTIONAL_COMPONENTS";
                next if $tok =~ /^(CONFIG|NO_MODULE|REQUIRED|QUIET|EXACT|MODULE|GLOBAL|COMPONENTS)$/;
                next if $tok =~ /^\$\{/;
                next if $tok =~ /^[0-9]/;
                next unless $tok =~ /^[A-Za-z][A-Za-z0-9_]*$/;
                print "$tok\n";
            }
        }
    ' "$1"
}

# Resolve a mix of directories and files to the CMakeLists.txt files to scan:
# each argument that is a file is used as-is; a directory that itself holds a
# CMakeLists.txt is scanned there; any other directory contributes its
# immediate subdirectories' CMakeLists.txt (a KF6 source cache layout).
kdx_collect_cmake_lists() { # $1.. = dirs/files
    local d
    for d in "$@"; do
        [ -e "$d" ] || continue
        if [ -f "$d" ]; then
            printf '%s\n' "$d"
        elif [ -f "$d/CMakeLists.txt" ]; then
            printf '%s\n' "$d/CMakeLists.txt"
        elif [ -d "$d" ]; then
            find "$d" -mindepth 2 -maxdepth 2 -name CMakeLists.txt 2>/dev/null
        fi
    done
}

# Union of the Qt components the given CMakeLists.txt files require. Sorted,
# unique, one per line.
kdx_detect_qt_components() { # $1.. = dirs/files
    local f
    command -v perl >/dev/null 2>&1 || return 1
    {
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            _kdx_components_of_file "$f"
        done <<EOF
$(kdx_collect_cmake_lists "$@")
EOF
    } | sort -u
}

# Components a Qt prefix actually ships: a component is present when its CMake
# package config exists. Prints the missing ones (one per line).
kdx_missing_qt_components() { # $1 = Qt prefix, $2.. = components
    local prefix="$1" comp
    shift
    for comp in "$@"; do
        [ -n "$comp" ] || continue
        if [ ! -f "$prefix/lib/cmake/Qt6$comp/Qt6${comp}Config.cmake" ]; then
            printf '%s\n' "$comp"
        fi
    done
    :
}

# The Qt component a CMake configure log says is missing, if any. Recognises
# both the shared-module ("Failed to find required Qt component \"X\"") and the
# per-module ("Could NOT find Qt6X") messages CMake emits.
kdx_missing_component_from_log() { # $1 = log file
    local log="$1"
    [ -f "$log" ] || return 1
    sed -n \
        -e 's/.*Failed to find required Qt component "\([^"]*\)".*/\1/p' \
        -e 's/.*Could NOT find Qt6\([A-Za-z][A-Za-z0-9_]*\).*/\1/p' \
        "$log" | head -1
}

# The aqt module list aqt reports for a Qt version/arch, minus debug symbols.
# Derived from aqt itself, so there is no curated list to go stale; empty output
# means the caller should fall back to `-m all`.
kdx_aqt_modules() { # $1 = aqt, $2 = version, $3 = arch
    local aqt="$1"
    "$aqt" list-qt mac desktop --modules "$2" "$3" 2>/dev/null \
        | tr ' ' '\n' \
        | grep -v '^$' \
        | grep -v '^debug_info$' \
        | grep -v '\.debug_information$'
}
