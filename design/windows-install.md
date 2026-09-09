# Windows distribution & installer — why an exe, and the invariants around it

## Goal / problem

Users on Windows need to install the Katexdown plugin plus the Qt WebEngine
runtime that Kate for Windows does not ship. The historical flow — download a
zip from a release, unzip it, open an *elevated* PowerShell, bypass the
execution policy, run `install.ps1` — failed in a new way at every hop, and the
pipeline itself was dead: the whole packaging chain still referenced the
upstream fork name (`katdown.dll`, `katdown-manifest.txt`, `katdown-*.zip`)
while the plugin actually builds as `katexdown.dll`/`katexdown.so`, so CI died
at the package step. The design goal is a distribution that is one file to
download and one double-click to install, with no PowerShell and no user
knowledge required, plus an uninstall story Windows users already know.

## Design

### The payload must sit next to kate.exe — so elevation is unavoidable

The plugin DLL imports `Qt6WebEngineWidgets` (and that pulls the rest of
WebEngine). When Windows loads the plugin into Kate, it resolves the plugin's
imports from the *host process* directory — the folder holding `kate.exe` — not
from the plugin's own folder, and not from anywhere on `PATH`. Kate for Windows
ships no WebEngine, so the runtime files (DLLs, `QtWebEngineProcess.exe`, the
`.pak`/`.dat`/`.bin` Chromium data files, one locale pak) have to be written
next to `kate.exe`. Kate lives under Program Files by default, so any installer
needs administrator rights. Once that is accepted, the only question is which
shape makes elevation and the copy painless.

### A self-elevating NSIS exe, not a script

`packaging/windows/katexdown.nsi` compiles (with `makensis`) into a single
self-elevating exe:

- **One file, one UAC prompt.** `RequestExecutionLevel admin` makes Windows
  prompt for consent at launch; there is no "open PowerShell as administrator"
  step, no execution-policy dance, no script runner the user can get wrong.
- **It finds Kate itself.** Scans the uninstall registry (HKLM, then HKCU, on
  the 64-bit view) for an entry whose DisplayName starts with `Kate` and whose
  `InstallLocation` actually holds `bin\kate.exe`, then the two stock
  locations (`$PROGRAMFILES64\Kate`, `$LOCALAPPDATA\Programs\Kate`). The GUI
  shows a directory page prefilled with the result and validates on Next.
- **It refuses to install into the wrong Kate before doing anything.** Three
  guards run up front (install section, and `.onVerifyInstDir` for the GUI):
  `bin\kate.exe` must exist; the path must not be a Microsoft Store install
  (no plugins can ever load there — its directory is ACL-locked); and Kate's
  `Qt6Core.dll` minor must equal the minor the payload was built against
  (`GetDllVersion` vs the compile-time `QT_MINOR` define). The Qt guard exists
  because a mismatched plugin is not merely broken — it *silently* never
  appears in Kate's plugin list.
- **It checks Kate is closed.** Replacing DLLs Kate has loaded fails; the
  installer detects a running `kate.exe` (a one-line `powershell.exe
  Get-Process` probe — case-insensitive, unlocalized, present on every
  supported Windows) and offers Retry/Cancel.
- **Apps & features integration.** A standard uninstall key (HKLM,
  `Software\Microsoft\Windows\CurrentVersion\Uninstall\Katexdown`) plus a
  `WriteUninstaller` exe. Uninstall = Settings → Apps → Katexdown.
- **Upgrades clean up after older versions.** The uninstaller has the file
  list it shipped *compiled in* — no manifest file is written at install time,
  so a crash mid-copy can never leave a half-written manifest that uninstall
  trusts (the old install.ps1 wrote its manifest before copying and tolerated
  missing entries; the compiled-in list cannot disagree with reality). An
  install over an older install silently executes the previous uninstaller
  first (`/S _?=` keeps it from deleting itself so the new one can overwrite
  it), which removes files newer payloads dropped.
- **Silent mode for the CI.** `/S` skips all pages and message boxes; an abort
  exits non-zero, which is what the verification job checks.

### The recorded recipe: one command, nothing hardcoded

`packaging/windows/build-installer.ps1` is the one command that produces an installer, and it is
what the CI calls, so a manual build and CI cannot diverge. It **derives** the two things that
otherwise drift: the version is read out of `CMakeLists.txt`, and the Qt minor out of the same
Craft `Qt6Core.dll` the payload was built against — neither is hardcoded anywhere. It stages the
payload and runs makensis with those values as compile-time defines, and will install NSIS itself
(via chocolatey, only when makensis is absent) so a human with Craft needs no other setup.

### Where the file list lives

`packaging/windows/make-payload.ps1` is the **single authoritative file list**: it validates
every WebEngine file against the Craft tree (reporting *all* missing files in one run) and stages
them plus the plugin into `payload\` laid out exactly like a Kate install root. From that same
list it also emits `uninstall-payload.nsh` — the NSIS fragment the uninstaller `!include`s as its
Delete list — so uninstall and payload are generated from one source and cannot drift (the
earlier version duplicated the Delete list inside `katexdown.nsi` by hand, which was the one place
they could disagree). Nothing can silently drop out of the installer: staging fails on a missing
source, `makensis` fails if the staged tree lacks a file, and the uninstall list is, by
construction, exactly the staged tree.

## Invariants

- The WebEngine runtime and the plugin go **next to kate.exe / under Kate's
  root** (`bin\…`, `bin\kf6\ktexteditor\katexdown.dll`), never beside the
  plugin in a user-writable folder — import resolution makes any other layout
  dead on arrival.
- **Qt minor must match** between the payload and the target Kate. The
  installer aborts otherwise; a version that "mostly" matches must not install.
  `QT_MINOR` is derived in CI from the *same* Craft `Qt6Core.dll` the payload
  was built against — never hardcoded, never assumed.
- The uninstaller removes **only files it installed**, and `RMDir` runs only on
  directories it may have created, deepest first — a `kf6\ktexteditor` folder
  holding other plugins or Kate's own `bin` is never touched.
- An install is **idempotent and self-upgrading**: running the installer again
  must yield a working install, never a duplicate or a half-state.
- The exe is **verified against a real Kate before it ships** (see below); a
  payload change that passes Craft-tree checks but breaks inside a real Kate
  must fail the tag build, not reach users.

## Pitfalls

- **The fork rename broke the pipeline silently.** Every Windows artifact was
  still `katdown.*` after the rename to katexdown, while the build emits
  `katexdown.dll`. Nothing validated that the names agreed — the failure only
  surfaced when the package step looked for a DLL that cannot exist. Lesson:
  names of artifacts are load-bearing and must be derived, not copied from the
  upstream repo.
- **Runtime data files cannot be validated by linkage.** WebEngine opens
  `qtwebengine_resources*.pak`, `icudtl.dat`, `v8_context_snapshot.bin` and the
  locale paks *by name*; a clean link and clean imports say nothing about them.
  Two shipped missing before a real render test caught them (the locale pak in
  the wrong directory — it must be `bin\translations\qtwebengine_locales\`,
  not `bin\qtwebengine_locales\`; and `v8_context_snapshot.bin`, whose absence
  makes every renderer process abort ~34 ms in with an empty-looking preview).
  Only a render inside a real Kate install is evidence — see Test seams.
- **windeployqt cannot be trusted on a Craft layout.** It deploys a Qt-app
  closure Katexdown never uses and fails partway on WebEngine resources because
  Craft has no `resources\` subdirectory. Hence the explicit curated list.
- **A zip + PowerShell flow has one failure mode per step.** Execution policy,
  elevation knowledge, path/quoting, a hardcoded Qt minor inside the script,
  artifact naming drift, no uninstall entry. An exe removes the *class* of
  problems, not just individual instances — this is why the whole user-facing
  path is now one file, not a "better script".
- **The canary property is a feature.** The CI verifies against the *newest*
  Kate build on KDE's CDN. When Kate rolls to a new Qt minor, the tag build
  fails deliberately (the Qt guard refuses) until the payload is rebuilt
  against that Qt — the alternative is shipping an installer that loads into
  nothing.
- **`makensis` is an unpinned CI dependency** (installed via chocolatey, with
  retries). Like the mirror-served Craft archives this is a deliberate
  trade-off: pinning a choco version that disappears breaks the build for no
  gain. If `choco install nsis` ever becomes unreliable, the fallback is a
  pinned NSIS release zip fetched directly.

## Test seams

- The Windows job builds every test target (`BUILD_TESTING=ON`), so all tests must stay
  MSVC-clean even though only `previewlifecycletest.exe` is executed there. That is why
  `rendermemorytest`'s `unistd.h`/`sysconf(_SC_PAGESIZE)` are guarded by `Q_OS_UNIX` and its
  page size falls back to a constant off-POSIX — the `/proc` RSS tests skip at runtime on
  non-Linux via their `/proc/self/statm` existence check, but they still have to compile on
  MSVC, which has no `unistd.h`.
- The installer is **silent-installed into a freshly downloaded real Kate** in
  `.github/workflows/windows.yml` (`/S`), file placement is asserted, then
  `previewlifecycletest.exe` renders against that Kate with `PATH` restricted
  to Kate's own `bin` so Craft cannot backstop a missing payload file — the
  step that catches wrong/missing data files.
- The uninstaller is then run with `/S` and file removal is asserted, so the
  compiled-in Delete list is tested on every tag build, not just by reading it.
- `QT_MINOR` reaches the installer as a compile-time define from the Craft tree
  and is enforced with `GetDllVersion`, so a payload/Qt drift fails loudly in
  the guard rather than as a mystery.
- `makensis` compiles locally (any OS) as a syntax/embedding gate; the NSIS
  script is plain 3.x NSIS (MUI2 + LogicLib), compilable with the distro
  package.

## Where it lives

- `packaging/windows/build-installer.ps1` — the recorded recipe: derives version + Qt minor,
  stages the payload, and runs makensis. Called by CI so manual builds and CI match.
- `packaging/windows/make-payload.ps1` — authoritative payload file list, staged for `makensis`;
  also emits the generated uninstall file list (`uninstall-payload.nsh`).
- `packaging/windows/katexdown.nsi` — the installer (pages, Kate detection, guards, install /
  uninstall sections); the only hand-written piece of the packaging.
- `.github/workflows/windows.yml` — builds the payload with Craft, calls build-installer.ps1,
  verifies install + render + uninstall against a real Kate, uploads and attaches the exe.
