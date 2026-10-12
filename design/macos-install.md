# macOS distribution — thin plugin + Homebrew-provided engine

Status: **Method 1** (thin plugin, engine borrowed from Homebrew at install time)
is the implemented design. **Method 1b** (self-hosted WebEngine payload) is
deliberately **not implemented**; this doc is the checkpoint it must be built
from when a Homebrew-free install is needed (offline users, Intel without a
bottle, or a future where borrowing is impossible).

## Goal / problem

Kate for macOS ships as a notarized, hardened-runtime app bundle built with
Craft, and it **does not ship Qt WebEngine**. The preview is a
`QWebEngineView`, so a macOS install must solve two problems at once:

1. **Supply the WebEngine runtime** (frameworks + `QtWebEngineProcess.app` +
   Chromium resources) that Kate lacks.
2. **Get a foreign plugin past code signing**: Kate is Developer-ID signed with
   hardened runtime and no `disable-library-validation` entitlement, so an
   unsigned/foreign plugin is rejected by library validation.

The previous README sent users into a full Craft stack build, with no blueprint
in the repo and no signing step. That is why it could not be followed.

## Verified facts (Kate 26.08, build 12388, arm64)

Re-verify per release: download
`https://cdn.kde.org/ci-builds/utilities/kate/<series>/macos-<arch>/kate-<series>-<build>-macos-clang-<arch>.dmg`,
`hdiutil attach` on macOS (on Linux: `7z x` the HFS+ partition), then inspect
`kate.app`.

| Fact | Value |
|---|---|
| Bundle | `kate.app`, `CFBundleIdentifier org.kde.Kate`, exec `Contents/MacOS/kate` |
| KTextEditor plugin dir | `Contents/PlugIns/kf6/ktexteditor/` (KDE plugins are `.so` on macOS) |
| Qt / KF | **Qt 6.11.1**, **KF 6.30.0** |
| WebEngine | **absent** — no `QtWebEngineCore/Widgets.framework`, no `QtWebEngineProcess.app`, no `icudtl.dat`/`.pak`/`v8_context_snapshot.bin`; only `qtwebengine_*.qm` translations (macdeployqt copies translations regardless) |
| Other missing deps | `QtWebChannel`, `QtPositioning` are also absent |
| Signature | Developer ID `K Desktop Environment e.V. (5433B4KXM8)`, hardened runtime (`flags=0x10000 CS_RUNTIME`), **no entitlements** → library validation enforced |
| Plugin references | `@executable_path/../Frameworks/...` (not `@rpath`) |
| Qt plugin loader | compares **major/minor only**, patch ignored; plugin minor must be `<=` host minor (`qtbase/src/corelib/plugin/qlibrary.cpp`) |
| Homebrew | `qt` / `qtwebengine` **6.11.x arm64 bottles** exist; **no macOS x86_64 bottles**; **no KF6 formulas** (KDE tap is KF5 only) |
| Official Qt OSS macOS repo | **no `qtwebengine` component** for 6.11.1/6.10.1/6.9.3/6.8.3 |
| Public Craft macOS cache | `files.kde.org/craft/prebuilt/24.10/macos/clang/x86_64/` — stale, x86_64-only; 25.10 has Windows only |

Consequences that drive everything below:

- The engine cannot come from Kate or the official Qt installer; it must be
  built or borrowed. Homebrew is the only public prebuilt arm64 source.
- Qt **patch** drift is safe (loader ignores patch); Qt **minor** drift is not —
  every payload needs a Qt-minor guard and a canary.
- Intel macOS has no Homebrew Qt bottle, so Method 1 covers arm64 first.

## Chosen design (Method 1)

**Release assets are tiny**: `katexdown-<ver>-macos-<arch>.so`,
`katexdown-macos-<arch>.so` (versionless alias for the `latest/download` URL),
`install-macos.sh`, `uninstall-macos.sh`. No engine is hosted or built by us.

### Install (user)

Homebrew is an accepted prerequisite (the `brew install --cask kate` audience):

```sh
curl -fsSL https://github.com/<owner>/katexdown/releases/latest/download/install-macos.sh | bash
```

`install-macos.sh` must:

1. Locate `kate.app` case-insensitively: `$KATEXDOWN_KATE_APP`, then
   `mdfind 'kMDItemCFBundleIdentifier == "org.kde.Kate"'`, then
   `/Applications/{kate,Kate}.app`, `~/Applications/{kate,Kate}.app`.
2. Refuse while Kate is running (`pgrep -x kate`).
3. Check `uname -m` and Kate's Qt minor (`QtCore.framework/.../Info.plist`
   `CFBundleShortVersionString`) against the plugin's build minor; abort with a
   clear message on mismatch (silent rejection is the failure mode to avoid).
4. Ensure Homebrew and the engine: `brew install qtwebengine` when the
   WebEngine frameworks are not already resolvable.
5. Copy the plugin into `<kate>/Contents/PlugIns/kf6/ktexteditor/`.
6. Pull the engine into the bundle with `$(brew --prefix qt)/bin/macdeployqt`
   (deploys `QtWebEngineCore/Widgets`, the helper, resources, `QtWebChannel`,
   `QtPositioning`, and rewrites install names to
   `@executable_path/../Frameworks/...`). Any non-Qt Homebrew dylibs macdeployqt
   misses must be copied by an `otool -L` closure pass.
7. Re-sign the bundle with the repo's entitlements and ad-hoc identity, then
   de-quarantine: see `packaging/macos/katexdown.entitlements`.
8. Verify the result (`codesign --verify --deep --strict`; a `--doctor` mode
   that reports which step failed).

### Signature

`codesign --force --deep --options runtime --entitlements
packaging/macos/katexdown.entitlements --sign - <kate>` with:

- `com.apple.security.cs.disable-library-validation` — required to load our plugin
- `com.apple.security.cs.allow-jit` — V8/Chromium on Apple Silicon
- `com.apple.security.cs.allow-unsigned-executable-memory` — Chromium

If `--deep` signing fails on the bundle, fall back to a bottom-up sign of
nested code then the bundle. Dropping `--options runtime` (plain ad-hoc sign)
is a documented last resort; it disables hardened runtime entirely.

### Homebrew cask (trusted front door)

A cask in a project tap (template: `packaging/macos/Casks/katexdown.rb`):
`depends_on cask: "kate"`, `depends_on formula: "qtwebengine"` (so Homebrew
installs the engine and the installer is run with `--no-brew` under `sudo: true`
— the script must never invoke `brew` as root), an `installer script:` running
the same installer with `--plugin-url` for the cask's version, and a matching
`uninstall script:`. Publishing it gives checksummed downloads and a familiar
uninstall; the template ships with `sha256 :no_check` until a release exists.

### Release/CI

`.github/workflows/macos.yml` mirrors `.github/workflows/windows.yml`'s shape:
matrix arm64 (`macos-14`, Intel leg commented out until a runner+bottle/1b
exists); install Homebrew Qt and source-build the pinned KF6 into a
version+Qt-minor+arch-keyed cache; build the plugin and the test binaries with
`build-plugin.sh --with-tests`; **verify against the newest real Kate dmg**
(download from `cdn.kde.org/ci-builds/utilities/kate/<series>/macos-arm64/`,
hdiutil-mount, copy `kate.app` out, refuse when its Qt minor differs from the
pin — the canary); run `install-macos.sh` on the copy and then
`previewlifecycletest` with `DYLD_FRAMEWORK_PATH`/`QT_PLUGIN_PATH` pointed at the
patched app (plus `QTWEBENGINE_RESOURCES_PATH`/`QTWEBENGINE_LOCALES_PATH`
derived from where macdeployqt actually put the framework's Resources - never a
hardcoded `Contents/Resources`, which has no WebEngine data), so the engine under test is the one the installer
deployed, not Homebrew's complete Qt; uninstall and assert the plugin is gone;
publish `katexdown-<version>-macos-arm64.so`, the versionless
`katexdown-macos-arm64.so` alias the installer's `latest/download` URL needs,
`install-macos.sh` and `uninstall-macos.sh` as release assets.

The cask relies on Homebrew to install the engine: it declares
`depends_on formula: "qtwebengine"` and runs the installer with `--no-brew`,
because under the cask's `sudo: true` the installer must never invoke `brew` as
root (Homebrew refuses).

Building the plugin only needs Homebrew Qt/WebEngine **headers+libs** plus KF6
**headers+libs**; the link libs only have to exist at link time because the
plugin resolves Kate's copies at runtime. KF6 is not available prebuilt for
macOS, so the build recipe builds the needed KF6 frameworks from source against
Homebrew Qt (pinned version), cached between runs. See
`packaging/macos/README.md` for the recorded recipe.

## Build (implemented)

Two scripts, both called by `.github/workflows/macos.yml` so a manual build and
CI cannot diverge:

- **`packaging/macos/build-plugin.sh`** configures the plugin against Homebrew
  Qt (`brew install cmake ninja pkgconf qt gettext bison`) and a KF6 prefix, with
  `CMAKE_OSX_ARCHITECTURES=arm64`, and writes
  `dist/katexdown-<version>-macos-arm64.so`. It reads the pinned Qt minor out of
  `install-macos.sh` (`KATEXDOWN_QT_MINOR`) and refuses to build when Homebrew's
  Qt minor differs — the silent-failure half of the canary, enforced at build
  time. The `.so` deliberately keeps its Homebrew/KF6 references; the installer
  is what rewrites them.
- **`packaging/macos/build-kf6.sh`** source-builds KF6 `6.30.0` (pinned;
  `KATEXDOWN_KF6_VERSION`) into a prefix, skipping frameworks already installed
  there so the CI cache restores in place. Its repository list is the transitive
  build-dependency closure of KTextEditor, generated from KDE's
  `dependency-data-kf6-qt6` and topologically ordered, minus `kdoctools`,
  `breeze-icons` and `kglobalaccel`. `kdoctools`/`breeze-icons` are docs/themes
  only (disabled with `CMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON` and
  `USE_BreezeIcons=OFF`). `kglobalaccel` is a Linux/FreeBSD-only framework
  (X11/Plasma-Wayland, no macOS backend) whose platform check aborts the
  configure on Darwin; KXmlGui, its only consumer in the closure, already
  excludes it on APPLE, so dropping it removes a dependency rather than
  disabling needed behaviour. If a future KF6 release adds a required
  dependency, the framework that needs it fails its configure with the missing
  package name and the list must be updated.

  The configure also passes `-DKDE_INSTALL_BUNDLEDIR="$prefix/Applications"`.
  ECM defaults `KDE_INSTALL_BUNDLEDIR` to the *absolute* `/Applications/KDE` on
  macOS (it is not relative to `CMAKE_INSTALL_PREFIX`), and KTextEditor installs
  its `ktexteditor-script-tester` developer tool as a `.app` bundle. Without the
  override the install step tries to create `/Applications/KDE/...`, which does
  not exist on a CI runner and is not writable by the build user — so the
  framework configures and compiles, then fails at `cmake --install`. Keeping
  bundles inside the prefix leaves the host's `/Applications` untouched; the
  prefix is a build-time means to an end and is never shipped.

## Deferred: Method 1b (self-hosted WebEngine payload)

Prepare for this but do not implement it until a Homebrew-free install is
required. It is the same installer, with the engine source made pluggable
(`KATEXDOWN_ENGINE=brew|payload`).

To build the payload later:

1. Build Qt/WebEngine for the target Qt minor. Two sources:
   - **Craft pinned to Kate's exact Qt** (most faithful; KDE's own tooling;
     source build of Chromium is slow), or
   - **Homebrew's `qtwebengine` bottle** (fast; Qt 6.11.x), accepting patch
     drift (safe) and Homebrew's patches.
2. Take the official Kate dmg's `kate.app`, copy it to `pristine.app` and
   `staged.app`, drop in the plugin, run `macdeployqt` on `staged.app`, and
   **diff staged vs pristine**. That diff is the single authoritative payload
   file list (and the uninstall manifest), exactly analogous to
   `packaging/windows/make-payload.ps1`.
3. Stage `payload/Contents/...` plus `manifest.txt`; ship
   `katexdown-macos-<arch>-qt<minor>.tar.gz`.
4. The installer then copies the payload instead of invoking macdeployqt, and
   uses the manifest for uninstall.
5. Update `THIRD-PARTY-NOTICES.md` for the redistributed WebEngine (LGPLv3).

Invariants it must keep: Qt minor matches Kate's and is guarded; only files
Kate lacks are shipped; uninstall removes exactly the manifest files and
re-signs; the payload is verified against a real Kate before release.

## Rejected alternatives (and why)

- **Craft per user** (the old doc): hours, no blueprint, no signing step.
- **WKWebView backend**: would remove the engine entirely and is the lightest
  long-term, but it is a real port (Objective-C++ bridge, QWebChannel → message
  handler shim) and the `lazyrender.md` lifecycle/GC design is WebEngine
  specific. Revisit only if Method 1b becomes impossible.
- **KParts/markdownpart "lite" preview**: tiny, but loses KaTeX — a product
  regression, not a shipping option.
- **`.pkg` installer**: unsigned pkg is blocked by Gatekeeper; signing needs an
  Apple Developer ID the project does not have.

## Invariants

- A macOS install **must** place the plugin under
  `<kate>/Contents/PlugIns/kf6/ktexteditor/` and the engine under
  `<kate>/Contents/Frameworks/`, or `@executable_path/../Frameworks`
  resolution fails.
- The plugin's Qt **minor** must equal Kate's; the installer must refuse a
  mismatch instead of silently producing a plugin Kate never loads.
- The engine source (`brew`) is a runtime concern only; it never changes what
  the plugin links against at install time.
- Re-signing is mandatory; removing the vendor signature is irreversible
  without reinstalling Kate.
- Scripts must be idempotent: re-running the installer after
  `brew upgrade --cask kate` must restore the plugin.

## Pitfalls

- **Silent failure**: a mismatched or unsigned plugin simply never appears in
  Kate's plugin list. Every check must fail loudly with a reason.
- **Patch ignored, minor enforced**: do not compare full Qt versions; compare
  major/minor, and treat Kate moving minor as "rebuild the plugin".
- **`brew upgrade --cask kate` replaces the bundle** and wipes the plugin;
  document re-running the installer.
- **Notarization is not restorable**: the clean revert is
  `brew reinstall --cask kate`.
- **Codesign artifacts are not in the manifest, and `--deep` makes it worse**:
  the manifest is snapshotted *before* `sign_bundle`, so the `_CodeSignature/`
  directories codesign drops into every nested framework and `.app` are absent
  from it. Uninstall must delete those before pruning the payload's directories:
  otherwise a removed framework keeps only `_CodeSignature`, becomes an invalid
  bundle, and `codesign --verify --deep --strict` (and the re-sign before it)
  fails with "bundle format unrecognized". `remove_from_manifest` clears every
  `_CodeSignature` under the bundle first; `sign_bundle` regenerates the ones
  still needed. `test-install-macos.sh`'s fake `codesign` re-creates nested
  signatures for `--deep` so this cannot silently regress.
- **Translations lie**: `qtwebengine_*.qm` in the bundle do not mean WebEngine
  is present; check for the framework/helper/resources.
- **`--deep` sign is deprecated**; provide a bottom-up fallback and verify with
  `codesign --verify --deep --strict`.
- **Intel**: no Homebrew macOS Qt bottle → Method 1 is arm64-first; Intel needs
  Method 1b or a Craft build.
- **Homebrew refuses to run as root**: the cask runs the installer under
  `sudo: true`, so the installer must be called with `--no-brew` and the engine
  itself declared as `depends_on formula: "qtwebengine"` — letting the script
  run `brew install` as root fails.
- **The CI cache must be keyed on the Qt minor too**: the KF6 prefix embeds the
  Qt it was built against and its absolute path, so a cache keyed on KF6 alone
  could restore frameworks with the wrong Qt ABI.
- **`find` on macOS is BSD find**: no `-maxdepth`, no `-quit`, no `-perm -u+x`
  assumptions in the scripts/workflow; use loops instead.

## Test seams

- Qt-minor guard and arch detection are pure shell logic; unit-test with a fake
  `kate.app` tree and a fake `uname`/`mdfind`.
- `packaging/macos/test-install-macos.sh` is that unit test: a fake `bin/`
  shadows `uname`, `sw_vers`, `sysctl`, `mdfind`, `pgrep`, `lipo`, `otool`,
  `install_name_tool`, `codesign`, `xattr`, `ditto`, `dscl`, `defaults`, `brew`
  and `macdeployqt`, and the real `install-macos.sh` is run against a fake
  `Kate.app`. It runs on Linux (the logic is pure shell, and the fake macOS
  tools make it testable without a Mac), and covers the guards, the closure
  pass, the manifest/idempotency/uninstall symmetry and the signing fallback —
  but not a real macdeployqt or codesign.
- The end-to-end render check is the real gate: install into a copy of the
  official Kate dmg, re-sign, then run the C++ `previewlifecycletest` with
  `DYLD_FRAMEWORK_PATH`/`QT_PLUGIN_PATH` pointed at the patched app
  (`QTWEBENGINE_RESOURCES_PATH`/`QTWEBENGINE_LOCALES_PATH` are set too, since
  the test binary is not `kate` and WebEngine may otherwise look next to the
  wrong executable).
- `install-macos.sh --doctor` must report each precondition independently so a
  user can paste one line.

## Where it lives

- `packaging/macos/` — `install-macos.sh` (install + `--uninstall` + `--doctor`),
  `uninstall-macos.sh` (front door), `katexdown.entitlements`, `build-plugin.sh`,
  `build-kf6.sh` (pinned KF6 closure), `test-install-macos.sh` (Linux logic
  tests), `Casks/katexdown.rb` (cask template), `README.md` (recorded recipe).
- `.github/workflows/macos.yml` — build + verify + release assets.
- `README.md` — user-facing macOS section.
- Engine/payload layout knowledge is in this doc; the Windows analogue is
  `design/windows-install.md`.
