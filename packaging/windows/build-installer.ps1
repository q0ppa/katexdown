<#
.SYNOPSIS
Produce the Katexdown Windows installer exe from start to finish — the one command that records
how the installer is made.

.DESCRIPTION
This is the recorded recipe for building the installer. It derives what can be derived (the
version from CMakeLists.txt, the Qt minor from the Craft tree the payload is built against), and
drives the two hand-written pieces underneath:

  1. make-payload.ps1   — validates and stages the payload (the WebEngine runtime Kate does not
                          ship, plus the plugin), and emits the uninstall file list fragment.
  2. makensis            — compiles packaging/windows/katexdown.nsi with the staged payload and
                          that fragment into the self-elevating installer exe.

It is the ONLY thing that needs to be right to produce an installer; the CI
(.github/workflows/windows.yml) calls this script, so a human doing a manual build and the CI run
the identical steps. VERSION and QT_MINOR reach makensis as compile-time defines from this script,
so nothing in the repo hardcodes a Katexdown version or Qt minor that can silently drift from
reality.

What you need on the machine:
  - Windows (the whole toolchain — Craft, makensis, chocolatey — is Windows-only), and
  - either a Craft root with a built plugin (the -CraftRoot/-PluginDll path, what the CI uses),
    or an already-staged payload dir from a previous make-payload.ps1 run (-PayloadDir),
  - makensis, which this script installs via chocolatey when it is missing and -SkipProvision is
    not given (that needs an elevated shell; otherwise install NSIS yourself and it is found on
    PATH).

.PARAMETER CraftRoot
Craft installation to stage the payload from (the one that built the plugin). Mutually exclusive
with -PayloadDir.

.PARAMETER PluginDll
Path to the built katexdown.dll, required with -CraftRoot.

.PARAMETER PayloadDir
An already-staged payload directory (produced by a prior make-payload.ps1 run) to build the
installer from, instead of running make-payload.ps1. Mutually exclusive with -CraftRoot.

.PARAMETER OutDir
Where the payload, the uninstall file list, and finally the installer exe land. Relative paths
resolve against the repository root. Default: 'dist'.

.PARAMETER QtMinor
Qt minor series the payload was built against, e.g. '6.11'. Derived automatically from
CraftRoot\bin\Qt6Core.dll when -CraftRoot is given. Required when -PayloadDir is used without a
Craft root to read it from.

.PARAMETER SkipProvision
Do not attempt `choco install nsis`; fail instead if makensis is not already on PATH.

.EXAMPLE
# The CI path: full build from a Craft root.
.\build-installer.ps1 -CraftRoot C:\CraftRoot -PluginDll build\bin\kf6\ktexteditor\katexdown.dll -OutDir dist

.EXAMPLE
# Rebuild an installer from a payload you already staged (make-payload.ps1 needs the Craft root;
# this only needs its output).
.\build-installer.ps1 -PayloadDir dist\payload -QtMinor 6.11 -OutDir dist

.NOTES
The payload (a hundred-plus MB of Qt WebEngine) is not produced from thin air: it has to come out
of a Craft tree, which is why this still needs Craft or a prior staging run. Everything after that
point is fully scripted.
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$CraftRoot,
    [string]$PluginDll,
    [string]$PayloadDir,
    [string]$OutDir = 'dist',
    [string]$QtMinor,
    [switch]$SkipProvision
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Fail([string]$Message) {
    throw $Message
}

# makensis parses the raw command line, so a -D value containing a space must carry its own
# surrounding quotes (pwsh passes argv elements as-is, but makensis re-tokenizes on spaces).
function Q([string]$Value) {
    if ($Value -match ' ') { return '"' + $Value + '"' } else { return $Value }
}

# The repository root is the parent of the folder holding this script.
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)

if (-not $CraftRoot -and -not $PayloadDir) {
    Fail 'Pass either -CraftRoot (to stage a payload) or an existing -PayloadDir.'
}
if ($CraftRoot -and $PayloadDir) {
    Fail '-CraftRoot and -PayloadDir are mutually exclusive.'
}

# Make OutDir absolute up front so later steps do not depend on the caller's cwd.
$OutDirFull = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $RepoRoot $OutDir }

# --- 1. version: derive from the project, never hardcode ---------------------
$match = Select-String -Path (Join-Path $RepoRoot 'CMakeLists.txt') -Pattern '^project\(.*VERSION\s+([0-9.]+)'
if (-not $match) { Fail 'could not read the project version out of CMakeLists.txt' }
$version = $match.Matches[0].Groups[1].Value
Write-Host "Katexdown $version"

# --- 2. payload --------------------------------------------------------------
if ($CraftRoot) {
    if (-not $PluginDll) { Fail '-PluginDll is required with -CraftRoot.' }
    if (-not (Test-Path $CraftRoot)) { Fail "No Craft root at '$CraftRoot'." }
    if (-not (Test-Path $PluginDll)) { Fail "No plugin at '$PluginDll'." }
    # make-payload.ps1 throws on failure (StrictMode/Stop inside it), which propagates here.
    & (Join-Path $ScriptDir 'make-payload.ps1') -CraftRoot $CraftRoot -PluginDll $PluginDll -OutDir $OutDirFull
    $payload = Join-Path $OutDirFull 'payload'
} else {
    if (-not (Test-Path $PayloadDir)) { Fail "No staged payload at '$PayloadDir'." }
    $payload = (Resolve-Path $PayloadDir).Path
}
if (-not (Test-Path $payload)) { Fail "No staged payload at '$payload'." }

# --- 3. Qt minor the payload was built against -------------------------------
if (-not $QtMinor) {
    if (-not $CraftRoot) {
        Fail "Cannot derive the Qt minor from a staged payload alone. Pass -QtMinor (e.g. 6.11), or stage with -CraftRoot."
    }
    $qtFile = Join-Path $CraftRoot 'bin\Qt6Core.dll'
    if (-not (Test-Path $qtFile)) { Fail "No Qt6Core.dll at '$qtFile' to read the Qt minor from." }
    $qtVersion = (Get-Item $qtFile).VersionInfo.FileVersion
    if (-not $qtVersion) { Fail "Could not read a version from '$qtFile'." }
    $QtMinor = ($qtVersion -split '\.')[0..1] -join '.'
}
Write-Host "payload targets Qt $QtMinor"

# --- 4. makensis (install if needed and allowed) ------------------------------
function Find-MakensisExe {
    # chocolatey's shims land in a dir already on PATH (Get-Command finds them); a manual install
    # can instead live in NSIS's own folder, so check that too before giving up.
    $cmd = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($candidate in @('C:\Program Files (x86)\NSIS\makensis.exe', 'C:\Program Files\NSIS\makensis.exe')) {
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}
$makensisExe = Find-MakensisExe
if (-not $makensisExe) {
    if ($SkipProvision) {
        Fail 'makensis not found and -SkipProvision is set. Install NSIS (choco install nsis) or drop -SkipProvision.'
    }
    Write-Host 'makensis not found; installing NSIS via chocolatey...'
    $ok = $false
    foreach ($attempt in 1..3) {
        choco install nsis -y --no-progress
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }
        Write-Warning "choco exited $LASTEXITCODE, retrying"
    }
    if (-not $ok) { Fail 'choco install nsis failed. Install NSIS manually and re-run.' }
    $makensisExe = Find-MakensisExe
    if (-not $makensisExe) { Fail 'NSIS installed but makensis.exe is not on PATH. Start a new shell and re-run, or install it manually.' }
}
Write-Host ("makensis: " + $makensisExe)

# --- 5. assemble the defines and compile --------------------------------------
# Size hint for Apps & features, in KiB.
$est = [int][math]::Ceiling(((Get-ChildItem $payload -Recurse -File | Measure-Object Length -Sum).Sum) / 1024)
# make-payload.ps1 writes the uninstall fragment next to the payload (in OutDir).
$uninstFrag = Join-Path (Split-Path -Parent $payload) 'uninstall-payload.nsh'
if (-not (Test-Path $uninstFrag)) {
    Fail "No uninstall file list at '$uninstFrag'. Re-stage the payload with make-payload.ps1 (it emits it)."
}
$script = Join-Path $ScriptDir 'katexdown.nsi'
& $makensisExe `
    "-DVERSION=$version" `
    "-DQT_MINOR=$QtMinor" `
    "-DEST_SIZE=$est" `
    "-DPAYLOAD=$(Q $payload)" `
    "-DUNINST_FRAG=$(Q $uninstFrag)" `
    (Q $script)
if ($LASTEXITCODE -ne 0) { Fail "makensis exited $LASTEXITCODE" }

# --- 6. land the exe in OutDir -------------------------------------------------
# katexdown.nsi writes the exe next to itself (${__FILEDIR__}); move it into OutDir.
$exeName = "katexdown-$version-windows-x86_64.exe"
$produced = Join-Path $ScriptDir $exeName
if (-not (Test-Path $produced)) { Fail "makensis produced no $exeName next to the script" }
if (-not (Test-Path $OutDirFull)) { New-Item -ItemType Directory -Path $OutDirFull -Force | Out-Null }
$target = Join-Path $OutDirFull $exeName
Copy-Item -LiteralPath $produced -Destination $target -Force
Remove-Item -LiteralPath $produced -Force
Write-Host ("Installer: {0}  ({1:N0} bytes)" -f $target, (Get-Item $target).Length)
