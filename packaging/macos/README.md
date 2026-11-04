# macOS packaging

End users never touch this folder directly. The main entry point is
`install-macos.sh` (or `uninstall-macos.sh`); this directory contains the
packaging and local build scripts.

```
packaging/macos/
├── install-macos.sh          # installer AND uninstaller (--uninstall)
├── uninstall-macos.sh        # front door: calls install-macos.sh --uninstall
├── katexdown.entitlements    # entitlements for the ad-hoc re-sign
├── lib-qt.sh                 # shared Kate/Qt detection helpers (sourced)
├── provision-qt.sh           # detects Kate's exact Qt and installs a matching one
├── build-plugin.sh           # local plugin build (auto-selects a matching Qt + KF6 prefix)
├── build-kf6.sh              # builds local KF6 prefix for building the plugin
├── test-install-macos.sh     # offline logic test suite for install-macos.sh
├── Casks/katexdown.rb        # Homebrew cask template for a project tap
└── README.md                 # this file
```

The architecture and rationale are documented in
[`design/macos-install.md`](../../design/macos-install.md).

## User installation & build

```sh
# 1. Install prerequisites (Apple Silicon arm64 with Homebrew):
brew install --cask kate
brew install cmake ninja pkgconf qt qtwebengine gettext bison

# 2. Build the KF6 dependencies once. This auto-selects a Qt whose private ABI
#    matches Kate's exact Qt patch, provisioning one with aqtinstall if
#    Homebrew's Qt does not match (cached in ~/.local/katexdown-kf6[-<Qt>]):
packaging/macos/build-kf6.sh

# 3. Build the plugin against the same Qt:
packaging/macos/build-plugin.sh

# 4. Install into Kate (quits Kate if running, copies plugin, re-signs Kate.app):
packaging/macos/install-macos.sh

# Diagnose preconditions without changing anything:
packaging/macos/install-macos.sh --doctor

# Uninstall (removes plugin, re-signs Kate.app):
packaging/macos/install-macos.sh --uninstall
```

The installer:
1. Locates `Kate.app` (or accepts `--kate /path/to/Kate.app`).
2. Checks that Kate is not running and the bundle is writable.
3. Checks Kate's Qt against the plugin's Qt: the minor **and** the exact
   private-ABI patch tag (see *Building against a specific Qt version*).
4. Ensures Homebrew and `qtwebengine` are installed (`brew install qtwebengine`).
5. Copies `katexdown.so` into `Kate.app/Contents/PlugIns/kf6/ktexteditor/` and
   relocates its load commands to Kate's bundled Qt/KF6 where possible.
6. Re-signs `Kate.app` ad-hoc with `packaging/macos/katexdown.entitlements`
   (disabling library validation so third-party plugins can load under hardened runtime).

## Qt selection: self-diagnosing, self-installing

KF6/KTextEditor link Qt **private** symbols, whose ABI tag changes every Qt
**patch** release (`QtPrivate_6_11_1` vs `QtPrivate_6_11_2`). Kate can only
load a plugin built against its **exact** Qt patch, so the build tooling must
find and use that exact Qt - and it does, without any hand-maintained module
list:

1. `lib-qt.sh` reads Kate's exact Qt version from the private ABI tag in its
   bundled `QtCore`, e.g. `QtPrivate_6_11_1` -> `6.11.1`.
2. It detects the Qt CMake components the sources need by scanning this repo's
   and the cached KF6 sources' `find_package(Qt6 ...)` calls, so nothing goes
   stale when upstream adds a dependency.
3. `provision-qt.sh` reuses an already-installed matching Qt
   (`--qt-prefix`/`$KATEXDOWN_QT_PREFIX`, then `$KATEXDOWN_QT_ROOT/<ver>/macos`,
   then Homebrew's Qt - the Homebrew-only path is preserved when it matches).
4. Otherwise it installs that Qt with `aqtinstall`, together with **every
   module aqt reports** for the version/arch (its own list, minus debug
   symbols; `-m all` if aqt reports none). There is no curated module list -
   the previous lists kept missing one more module (`TextToSpeech`, `TaskTree`,
   ...).

`build-kf6.sh` and `build-plugin.sh` run this automatically and pick a
Qt-specific KF6 prefix (`~/.local/katexdown-kf6-<Qt version>`) so prefixes
built against different Qt patches never mix. `provision-qt.sh` prints the
chosen Qt prefix, and can be run on its own:

```sh
packaging/macos/provision-qt.sh --print-version   # Kate's exact Qt version
packaging/macos/provision-qt.sh                   # resolve/provision, print prefix
```

### Manual override (only if you have your own matching Qt)

`--qt-prefix` / `KATEXDOWN_QT_PREFIX` is authoritative: it is passed straight
into `CMAKE_PREFIX_PATH`, and the plugin is configured and linked against
exactly that Qt. The same commands work for any matching Qt install (an
official/aqt Qt, a versioned Homebrew tap, a Craft build, ...).

```sh
QT_PREFIX="$HOME/Qt/6.11.1/macos"
KF6_PREFIX="$HOME/.local/katexdown-kf6-6.11.1"

packaging/macos/build-kf6.sh    --qt-prefix "$QT_PREFIX" --prefix "$KF6_PREFIX"
packaging/macos/build-plugin.sh --qt-prefix "$QT_PREFIX" --kf6-prefix "$KF6_PREFIX"
packaging/macos/install-macos.sh --plugin dist/katexdown-<version>-macos-arm64.so
```

`install-macos.sh --doctor` prints the private ABI of the local plugin and of
Kate and refuses to install on a mismatch, and `install-macos.sh` compares the
plugin's and Kate's ABI after building the resolution above into the installed
artifact.

## Manual plugin build

```sh
packaging/macos/build-plugin.sh \
    --kf6-prefix "$HOME/.local/katexdown-kf6-6.11.1" \
    --out-dir dist
```

This outputs `dist/katexdown-<version>-macos-arm64.so`. Without
`--qt-prefix`/`--kf6-prefix` the script resolves a Qt matching Kate and uses
the matching `~/.local/katexdown-kf6-<Qt version>` prefix.

## Testing the installer

`packaging/macos/test-install-macos.sh` tests the installer's guard logic,
discovery, architecture checks, and signing fallbacks without requiring macOS
or network access by using a simulated environment. Run it with:

```sh
packaging/macos/test-install-macos.sh
```
