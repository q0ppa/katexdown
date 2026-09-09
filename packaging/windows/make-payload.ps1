<#
.SYNOPSIS
Stage the Windows distribution payload: the plugin plus the Qt WebEngine runtime Kate does not
ship, laid out like a Kate install root. packaging/windows/katexdown.nsi then embeds this tree
into the installer exe (File /r), so this script is the single authoritative file list.

.DESCRIPTION
The file list is explicit rather than whatever windeployqt produces, for two reasons. windeployqt
deploys the closure a generic Qt app might need (QML debug tooling, geolocation backends, a PDF
module, a bundled VC redist installer), roughly 42 MB of which katexdown never touches. And it
fails part way on a Craft layout, because it looks for the WebEngine .pak files under a resources\
subdirectory that Craft does not create.

The list has two halves, and they need different evidence.

DLLs are settled by import analysis: kept when they are a static import of Qt6WebEngineCore or
Qt6WebEngineWidgets, dropped when the plugin still resolved every import with the file absent, on
a real Kate install.

The D3D shader compilers windeployqt offers (d3dcompiler_47.dll, dxcompiler.dll, dxil.dll) are
deliberately not here. Windows ships d3dcompiler_47.dll in System32, so Chromium finds it without
help; dxcompiler.dll and dxil.dll exist in neither Craft nor Kate nor a stock Windows install, so
there is nothing to ship. Chromium falls back to software rendering when it cannot compile D3D
shaders, which is already what happens on hardware that fails GPU init.

Data files are settled ONLY by running something that renders. They are opened by name at runtime,
so a clean link, a clean `dumpbin /dependents`, and a successful `LoadLibraryExW` all stay green
with any of them missing. Two shipped as absent before a real render test caught them: the locale
pak (wrong directory) and `v8_context_snapshot.bin` (never copied at all, because windeployqt exits
1 partway through its WebEngine resources step on a Craft layout and the hand-written replacement
list did not know about it). Do not add or remove anything in the data-file half on the strength of
a load test.

Anything Kate already ships is deliberately absent: the installer refuses to overwrite files it did
not put there, so a payload that duplicated Kate's own Qt would refuse to install.

This script is normally driven by packaging/windows/build-installer.ps1 (the recorded one-command
recipe). Besides the payload it also emits <OutDir>\uninstall-payload.nsh — an NSIS fragment
listing every file it staged, which katexdown.nsi includes as its uninstall Delete list, so that
list is generated from the same source as the payload and cannot drift.

.PARAMETER CraftRoot
Craft installation the files come from (the one that built the plugin).

.PARAMETER PluginDll
Path to the built katexdown.dll.

.PARAMETER OutDir
Directory to assemble into. Created if absent, cleared if it already holds a payload. The payload
lands in <OutDir>\payload and the uninstall file list in <OutDir>\uninstall-payload.nsh.

.EXAMPLE
.\make-payload.ps1 -CraftRoot C:\CraftRoot -PluginDll build\bin\kf6\ktexteditor\katexdown.dll -OutDir dist
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CraftRoot,
    [Parameter(Mandatory)][string]$PluginDll,
    [Parameter(Mandatory)][string]$OutDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# source path relative to CraftRoot -> destination path relative to the Kate install root
$Files = [ordered]@{
    # WebEngine itself, and the two Qt modules Qt6WebEngineCore statically imports that Kate lacks
    'bin\Qt6WebEngineCore.dll'              = 'bin\Qt6WebEngineCore.dll'
    'bin\Qt6WebEngineWidgets.dll'           = 'bin\Qt6WebEngineWidgets.dll'
    'bin\Qt6WebChannel.dll'                 = 'bin\Qt6WebChannel.dll'
    'bin\Qt6Positioning.dll'                = 'bin\Qt6Positioning.dll'
    # Chromium's render process. WebEngine spawns this as a separate executable.
    'bin\QtWebEngineProcess.exe'            = 'bin\QtWebEngineProcess.exe'
    # Chromium data files, all opened by name at runtime. None of them appears in any import table,
    # so nothing about a successful link or a successful LoadLibrary says they are present.
    'bin\icudtl.dat'                        = 'bin\icudtl.dat'
    'bin\qtwebengine_resources.pak'         = 'bin\qtwebengine_resources.pak'
    'bin\qtwebengine_resources_100p.pak'    = 'bin\qtwebengine_resources_100p.pak'
    'bin\qtwebengine_resources_200p.pak'    = 'bin\qtwebengine_resources_200p.pak'
    # Omitting this one costs every renderer process: V8 aborts at
    # gin/v8_initializer.cc "Error loading V8 startup snapshot file" ~34 ms in, so pages navigate,
    # renderers respawn, and no script ever runs. The preview looks empty with nothing on stdout.
    'bin\v8_context_snapshot.bin'           = 'bin\v8_context_snapshot.bin'
    # English locale only. The full set is 53 files and 45 MB for strings a markdown preview never
    # shows; Chromium falls back to en-US when a locale is missing.
    # It goes under bin\translations\, not bin\, measured: at bin\qtwebengine_locales\ Chromium
    # logged "locale resources are not loaded" 166 times per run and found nothing.
    'translations\qtwebengine_locales\en-US.pak' = 'bin\translations\qtwebengine_locales\en-US.pak'
}

if (-not (Test-Path $CraftRoot)) {
    throw "No Craft root at '$CraftRoot'."
}
if (-not (Test-Path $PluginDll)) {
    throw "No plugin at '$PluginDll'."
}

$payloadDir = Join-Path $OutDir 'payload'
if (Test-Path $payloadDir) {
    Remove-Item -LiteralPath $payloadDir -Recurse -Force
}
New-Item -ItemType Directory -Path $payloadDir -Force | Out-Null

# Collect every missing source before failing, so a Qt layout change reports all of its damage in
# one run instead of one file per attempt.
$missing = @()
foreach ($src in $Files.Keys) {
    if (-not (Test-Path (Join-Path $CraftRoot $src))) {
        $missing += $src
    }
}
if ($missing.Count -gt 0) {
    throw "Missing from '$CraftRoot':`n  $($missing -join "`n  ")"
}

$total = 0
foreach ($src in $Files.Keys) {
    $from = Join-Path $CraftRoot $src
    $to = Join-Path $payloadDir $Files[$src]
    $toDir = Split-Path -Parent $to
    if (-not (Test-Path $toDir)) {
        New-Item -ItemType Directory -Path $toDir -Force | Out-Null
    }
    Copy-Item -LiteralPath $from -Destination $to -Force
    $total += (Get-Item $to).Length
}

$pluginDest = Join-Path $payloadDir 'bin\kf6\ktexteditor\katexdown.dll'
New-Item -ItemType Directory -Path (Split-Path -Parent $pluginDest) -Force | Out-Null
Copy-Item -LiteralPath $PluginDll -Destination $pluginDest -Force
$total += (Get-Item $pluginDest).Length

# Emit the NSIS fragment the installer's uninstaller !includes. It is generated from the same
# $Files list that staged the payload, so the two can never drift (the alternative — a hand-kept
# Delete list inside katexdown.nsi — was the one place payload and uninstall could disagree).
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('; Generated by make-payload.ps1 from the same list that staged the payload.')
$lines.Add('; Relative paths, rooted at the Kate install root; consumed inside the Uninstall section.')
foreach ($rel in $Files.Values) {
    $lines.Add('  Delete "$INSTDIR\' + $rel + '"')
}
$lines.Add('  Delete "$INSTDIR\bin\kf6\ktexteditor\katexdown.dll"')
$fragment = Join-Path $OutDir 'uninstall-payload.nsh'
Set-Content -LiteralPath $fragment -Value $lines -Encoding Ascii

$count = $Files.Count + 1
Write-Host ("Staged {0} files, {1:N0} bytes, into {2}" -f $count, $total, $payloadDir)
Write-Host ("Wrote the uninstall file list to {0}" -f $fragment)
Write-Host 'Next: packaging/windows/build-installer.ps1 turns this payload into the installer exe.'
