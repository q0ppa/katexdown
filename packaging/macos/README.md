# macOS packaging

End users never touch this folder. The one command they are told to run is
`install-macos.sh` (or its `uninstall-macos.sh` front door); everything here is
the recipe behind it.

```
packaging/macos/
├── install-macos.sh          # installer AND uninstaller (--uninstall), one script
├── uninstall-macos.sh        # tiny front door: execs install-macos.sh --uninstall
├── katexdown.entitlements    # entitlements for the ad-hoc re-sign
├── build-plugin.sh           # recorded plugin build (Homebrew Qt + KF6 prefix)
├── build-kf6.sh              # pins + source-builds the KF6 closure build-plugin.sh needs
├── test-install-macos.sh     # drives install-macos.sh on Linux with a faked macOS toolchain
├── Casks/katexdown.rb        # Homebrew cask template for a project tap
└── README.md                 # this file
```

The design — why the plugin is thin, why the bundle has to be re-signed, why
Homebrew supplies the WebEngine, and the deferred self-hosted-payload method
(1b) — is in [`design/macos-install.md`](../../design/macos-install.md). Read
that first; this file is the *how to run it*.

## User flow

```sh
# install (arm64; Homebrew required, quits any Kate first)
curl -fsSL https://github.com/q0ppa/katexdown/releases/latest/download/install-macos.sh | bash

# diagnose without changing anything (one line to paste into a bug report)
curl -fsSL .../install-macos.sh | bash -s -- --doctor

# uninstall (removes exactly what was installed, re-signs Kate)
curl -fsSL .../uninstall-macos.sh | bash
```

The release assets are tiny — `katexdown-<version>-macos-arm64.so`,
`katexdown-macos-arm64.so` (the versionless alias the `latest/download` URL
needs), `install-macos.sh` and `uninstall-macos.sh`. No WebEngine is hosted or
built by this project: the installer borrows it from Homebrew at install time.

`install-macos.sh` is deliberately bash-3.2 (macOS `/bin/bash`) and idempotent;
re-running after `brew upgrade --cask kate` restores the install. The installer
and uninstaller are one script so their Kate lookup, Qt-minor guard, manifest
and re-signing cannot drift apart.

## Recorded build recipe

Builds run on an arm64 Mac (`macos-14`/`macos-15`) with Homebrew:

```sh
brew install cmake ninja qt gettext bison

# 1. KF6 (KTextEditor + its closure), pinned, built against the Homebrew Qt.
#    The prefix is cacheable; re-running skips frameworks already installed.
packaging/macos/build-kf6.sh --prefix "$PWD/.kf6-prefix"

# 2. the plugin (and the test binaries the CI verifies with)
packaging/macos/build-plugin.sh \
    --kf6-prefix "$PWD/.kf6-prefix" \
    --with-tests \
    --out-dir dist
```

`build-plugin.sh` prints the release `.so` path on stdout. It refuses to build
when Homebrew's Qt minor differs from `KATEXDOWN_QT_MINOR` in
`install-macos.sh`: the pin is read from that file, not duplicated here, and a
Qt-minor mismatch is the one failure Kate does not report (a mismatched plugin
is simply never listed). `build-kf6.sh` pins KF6 `6.30.0`; bump it only together
with the CI cache key.

`build-kf6.sh`'s list is the transitive build dependency closure of KTextEditor
6.30, generated from KDE's `dependency-data-kf6-qt6` and ordered topologically,
minus `kdoctools` and `breeze-icons` (docs/themes only — disabled with
`CMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON` / `USE_BreezeIcons=OFF`). If a
future KF6 adds a required dependency, the framework that needs it fails its
configure loudly with the missing package name; add it to the list. The plugin
build itself does **not** link the Homebrew/KF6 prefixes at runtime — installing
into Kate rewrites every reference to `@executable_path/../Frameworks/...`.

`.github/workflows/macos.yml` calls these same two scripts, so a manual build
and the CI cannot diverge.

## Homebrew cask

`Casks/katexdown.rb` is a **template** for a project tap. It declares
`depends_on cask: "kate"`, `depends_on formula: "qtwebengine"` (the installer's
engine, installed by Homebrew so the script never runs `brew` as root under
`sudo`) and `depends_on arch: :arm64`, then runs the same installer with
`--no-brew` and `--plugin-url` for the cask's own version, and the same script
with `--uninstall --purge`.

Before publishing it in a tap: replace `sha256 :no_check` with the real sha256
of `install-macos.sh` for the release (`shasum -a 256`), run
`brew audit --cask`, and check `brew install --cask ./Casks/katexdown.rb` and
`brew uninstall --cask katexdown` on a Mac. **The cask has not been exercised on
a Mac** — in particular whether Homebrew still has the downloaded scripts staged
when `uninstall script:` runs.

## The CI job

`.github/workflows/macos.yml` mirrors `.github/workflows/windows.yml`:

1. verifies the runner is an arm64 Mac with `/opt/homebrew`;
2. installs/builds KF6 (cached by KF6 version + Qt minor + runner arch);
3. builds the plugin and tests with `build-plugin.sh --with-tests`;
4. downloads the **newest real Kate dmg** from KDE's CDN
   (`cdn.kde.org/ci-builds/utilities/kate/<series>/macos-arm64/`), mounts it,
   copies `kate.app` out and — the canary — refuses to continue when Kate's
   `QtCore.framework` minor differs from the pin;
5. runs `install-macos.sh` against that copy (no `sudo`), then
   `previewlifecycletest` with `DYLD_FRAMEWORK_PATH`/`QT_PLUGIN_PATH` pointed at
   the patched app, so the test renders with **the engine the installer
   deployed**, not with Homebrew's complete Qt (this is the macOS analogue of
   the Windows v8_context_snapshot bug);
6. uninstalls and checks the plugin is gone;
7. uploads the `.so` + scripts, and attaches them to the release on a `v*` tag.

Intel is deliberately not in the matrix: Homebrew publishes no `qt`/`qtwebengine`
bottle for `x86_64` macOS, so there is nothing for Method 1 to borrow. When a
bottle (or Method 1b) exists, uncomment the `x86_64`/`macos-15-intel` leg.

## What has and has not been verified

**Verified here (Linux, no Mac, no network):**

- `install-macos.sh`'s logic is covered by `packaging/macos/test-install-macos.sh`,
  which shadows `uname`, `brew`, `macdeployqt`, `otool`,
  `install_name_tool`, `codesign`, `hdiutil`'s friends etc. with fakes and runs
  the real script: the Qt-minor guard (patch drift accepted, minor refused), the
  arch/Rosetta/host guards, Kate discovery, the running-Kate refusal, the
  closure pass including the "never copy a KF6 library" rule, the manifest, the
  `--deep` → bottom-up signing fallback, idempotency, uninstall symmetry and
  `--force`/`--purge`. Run it with `packaging/macos/test-install-macos.sh`.
- `build-plugin.sh --print-qt-minor` and `build-kf6.sh --print-version` are pure
  text/version logic and run anywhere.

**NOT verified on macOS (first CI run is the check).** Everything that needs the
real tools or a real Kate:

1. `build-kf6.sh` source build — the exact list/flags, and `kauth`'s backend on
   macOS (`-DENABLE_KAUTH` is on by default for KTextEditor on non-Windows).
2. `macdeployqt` actually deploying WebEngine/Channel/Positioning into Kate and
   the closure pass producing a plugin with no external references.
3. `codesign` with the entitlements: whether `--deep --options runtime` succeeds
   on this bundle, and whether the bottom-up fallback verifies.
4. Whether Kate actually loads the plugin after the ad-hoc signature — launch
   Kate with `--doctor` first if the plugin does not appear.
5. The render test with `DYLD_FRAMEWORK_PATH`/`QT_PLUGIN_PATH`: dyld searches
   `DYLD_FRAMEWORK_PATH`/`DYLD_LIBRARY_PATH` *before* a dependency's install
   name, so it should override both `@rpath` and absolute Homebrew paths for
   every framework Kate ships — but confirm that Kate's deployed Qt frameworks
   keep the versioned `Versions/A` layout that match (a flat framework would not
   match the install-name form and dyld would fall back to Homebrew), and that
   `QtWebEngineProcess.app` + `icudtl.dat`/`.pak` are found when the binary is
   not `kate` itself. The workflow also sets
   `QTWEBENGINE_RESOURCES_PATH`/`QTWEBENGINE_LOCALES_PATH` to Kate's copies as a
   belt-and-braces measure.
6. The Homebrew cask (above) and the uninstall-script staging question.
7. Intel: no path at all yet (Method 1b in design/macos-install.md).

When these are checked on a Mac, update this section and the "untested" notes at
the top of `install-macos.sh` / the workflow.
