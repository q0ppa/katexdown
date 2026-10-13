# macOS distribution — minimal plugin installer + Homebrew QtWebEngine

Status: **Minimal Method 1** (thin plugin, engine from Homebrew, clean re-sign)
is the implemented design. The previous brittle approach involving
`macdeployqt` bundle surgery and 29-framework source builds in CI has been
retired in favor of a clean, robust, and minimal workflow.

## Goal / problem

Kate for macOS ships as a notarized, hardened-runtime app bundle built with
Craft, and it **does not ship Qt WebEngine**. The preview is a
`QWebEngineView`, so a macOS install must solve two problems at once:

1. **Supply the WebEngine runtime** (frameworks + `QtWebEngineProcess.app` +
   Chromium resources) that Kate lacks.
2. **Get a foreign plugin past code signing**: Kate is Developer-ID signed with
   hardened runtime and no `disable-library-validation` entitlement, so an
   unsigned/foreign plugin is rejected by library validation.

## Verified facts (Kate 26.08, build 12388, arm64)

| Fact | Value |
|---|---|
| Bundle | `kate.app`, `CFBundleIdentifier org.kde.Kate`, exec `Contents/MacOS/kate` |
| KTextEditor plugin dir | `Contents/PlugIns/kf6/ktexteditor/` (KDE plugins are `.so` on macOS) |
| Qt / KF | **Qt 6.11.1**, **KF 6.30.0** |
| WebEngine | **absent** from Kate bundle; provided by Homebrew `qtwebengine` |
| Signature | Developer ID `K Desktop Environment e.V. (5433B4KXM8)`, hardened runtime (`flags=0x10000 CS_RUNTIME`), **no entitlements** → library validation enforced |
| Qt plugin loader | compares **major/minor only**, patch ignored; plugin minor must be `<=` host minor (`qtbase/src/corelib/plugin/qlibrary.cpp`) |
| Homebrew | `qt` / `qtwebengine` **6.11.x arm64 bottles** exist; **no macOS x86_64 bottles**; **no KF6 formulas** (KDE tap is KF5 only) |

Consequences that drive the minimal design:

- The engine comes from Homebrew (`brew install qtwebengine`).
- Homebrew is an accepted prerequisite on macOS for Katexdown users.
- Re-signing `Kate.app` with ad-hoc identity and `disable-library-validation`
  is mandatory to allow the foreign plugin `.so` to load.
- Intel macOS has no Homebrew Qt bottle, so support targets arm64 (Apple Silicon).

## Chosen minimal design

**Release assets are tiny**: `katexdown-<ver>-macos-<arch>.so`,
`install-macos.sh`, `uninstall-macos.sh`.

### Install & build

Homebrew is an accepted prerequisite (the `brew install --cask kate` audience):

```sh
brew install --cask kate
brew install cmake ninja pkgconf qt qtwebengine gettext bison

packaging/macos/build-kf6.sh
packaging/macos/build-plugin.sh
packaging/macos/install-macos.sh
```

`install-macos.sh` performs the following minimal, reliable steps:

1. Locates `kate.app` case-insensitively: `$KATEXDOWN_KATE_APP`, then
   Spotlight for `org.kde.Kate`, then `/Applications/[Kk]ate.app` and
   `~/Applications/[Kk]ate.app`.
2. Refuses while Kate is running (`pgrep -x kate`), preventing live mutation.
3. Checks Kate's Qt minor against the plugin's build minor to avoid silent
   loader rejection.
4. Verifies Homebrew and Qt WebEngine are installed (`brew install qtwebengine`).
5. Copies `katexdown.so` into `<kate>/Contents/PlugIns/kf6/ktexteditor/`.
6. Re-signs the bundle ad-hoc with `packaging/macos/katexdown.entitlements` and
   de-quarantines:
   ```sh
   codesign --force --deep --options runtime --entitlements packaging/macos/katexdown.entitlements --sign - <kate>
   ```
   (with fallback to bottom-up signing if `--deep` fails).
7. Verifies the result (`codesign --verify --deep --strict`).
8. Provides a matching `--uninstall` mode to cleanly remove the plugin and
   re-sign the bundle.

### Why `macdeployqt` bundle surgery was discarded

An earlier design attempted to run `macdeployqt` directly on `Kate.app` to
force-copy WebEngine frameworks and rewrite Mach-O install names inside the
app bundle. This approach was discarded because:

- Running `macdeployqt` against an already-deployed third-party application
  frequently breaks resource paths, duplicate symbol references, and internal
  framework symlinks.
- It required an unwieldy 1,100+ line shell script with complex Mach-O
  dependency tracking.
- Every `brew upgrade --cask kate` wiped out the injected framework files
  anyway.
- Resolving WebEngine via Homebrew alongside an ad-hoc signed `Kate.app` is
  cleaner, smaller, and easier to maintain.

### Why CI source builds of KF6 were removed

KF6 is not packaged in Homebrew on macOS (only KF5 formulas exist). Compiling
the entire 29-framework dependency closure of `KTextEditor` from source on
GitHub Actions macOS runners took ~20 minutes, frequently broke on upstream
C++ standard changes, and caused continuous CI failures.

Instead:
- The fragile CI `build-kf6` workflow was decommissioned.
- Local developer build scripts (`packaging/macos/build-kf6.sh` and
  `packaging/macos/build-plugin.sh`) remain available for developers building
  the plugin locally on Apple Silicon Macs.

## Signature

`codesign --force --deep --options runtime --entitlements
packaging/macos/katexdown.entitlements --sign - <kate>` with:

- `com.apple.security.cs.disable-library-validation` — required to load our plugin
- `com.apple.security.cs.allow-jit` — V8/Chromium on Apple Silicon
- `com.apple.security.cs.allow-unsigned-executable-memory` — Chromium

If `--deep` signing fails on the bundle, the installer falls back to a bottom-up
sign of nested code followed by the bundle.

## Invariants

- The plugin **must** be placed in `<kate>/Contents/PlugIns/kf6/ktexteditor/katexdown.so`.
- The plugin's Qt **minor** must match Kate's Qt minor; the installer must refuse
  a mismatch instead of silently producing a plugin Kate ignores.
- Re-signing is mandatory; removing the vendor signature is irreversible without
  reinstalling Kate.
- Scripts must be idempotent: re-running the installer after
  `brew upgrade --cask kate` must restore the plugin cleanly.
- Uninstall must cleanly remove the plugin and re-sign Kate.

## Where it lives

- `packaging/macos/` — `install-macos.sh` (installer and uninstaller),
  `uninstall-macos.sh` (front door), `katexdown.entitlements`,
  `build-plugin.sh`, `build-kf6.sh`, `test-install-macos.sh` (offline tests),
  `Casks/katexdown.rb` (cask template), `README.md`.
- `README.md` — user-facing macOS documentation.
