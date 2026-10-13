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
3. Checks Kate's Qt minor against the plugin's Qt minor.
4. Ensures Homebrew and `qtwebengine` are installed (`brew install qtwebengine`).
5. Copies `katexdown.so` into `Kate.app/Contents/PlugIns/kf6/ktexteditor/`.
6. Re-signs `Kate.app` ad-hoc with `packaging/macos/katexdown.entitlements`
   (disabling library validation so third-party plugins can load under hardened runtime).
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
