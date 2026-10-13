# macOS packaging

End users never touch this folder directly. The main entry point is
`install-macos.sh` (or `uninstall-macos.sh`); this directory contains the
packaging and local build scripts.

```
packaging/macos/
├── install-macos.sh          # installer AND uninstaller (--uninstall)
├── uninstall-macos.sh        # front door: calls install-macos.sh --uninstall
├── katexdown.entitlements    # entitlements for the ad-hoc re-sign
├── build-plugin.sh           # local plugin build (Homebrew Qt + local KF6 prefix)
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

# 2. Build the KF6 dependencies once (cached in ~/.local/katexdown-kf6):
packaging/macos/build-kf6.sh

# 3. Build the plugin:
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

## Building against a specific Qt version

KF6/KTextEditor link Qt **private** symbols, whose ABI tag changes every Qt
**patch** release. Kate can only load a plugin built against its exact Qt patch,
so if Homebrew's Qt differs, point the build at a matching Qt. The full
walk-through (including how to install the matching Qt) lives in the main
README: [*Building against Kate's exact Qt*](../../README.md#building-against-kates-exact-qt-only-if-homebrews-qt-differs).

In short, derive Kate's version and pass it to both scripts:

```sh
# e.g. QtPrivate_6_11_1 -> 6.11.1
KATE_QT_VERSION=$(nm -gU /Applications/kate.app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore \
    | grep -o 'QtPrivate_[0-9]*_[0-9]*_[0-9]*' | head -1 | sed 's/^QtPrivate_//; s/_/./g')
QT_PREFIX="$HOME/Qt/$KATE_QT_VERSION/macos"
KF6_PREFIX="$HOME/.local/katexdown-kf6-$KATE_QT_VERSION"

packaging/macos/build-kf6.sh    --qt-prefix "$QT_PREFIX" --prefix "$KF6_PREFIX"
packaging/macos/build-plugin.sh --qt-prefix "$QT_PREFIX" --kf6-prefix "$KF6_PREFIX"
packaging/macos/install-macos.sh --plugin dist/katexdown-<version>-macos-arm64.so
```

`--qt-prefix` / `KATEXDOWN_QT_PREFIX` is authoritative (it is passed straight
into `CMAKE_PREFIX_PATH`), and `install-macos.sh --doctor` prints the ABI tag of
the local plugin and of Kate and refuses to install on a mismatch. Without
`--qt-prefix` the scripts use `$(brew --prefix qt)`, which may not match Kate.

## Manual plugin build

```sh
packaging/macos/build-plugin.sh \
    --kf6-prefix "$HOME/.local/katexdown-kf6" \
    --out-dir dist
```

This outputs `dist/katexdown-<version>-macos-arm64.so`.

## Testing the installer

`packaging/macos/test-install-macos.sh` tests the installer's guard logic,
discovery, architecture checks, and signing fallbacks without requiring macOS
or network access by using a simulated environment. Run it with:

```sh
packaging/macos/test-install-macos.sh
```
