; ============================================================================
;  katexdown.nsi — the Katexdown Windows installer
;
;  WHAT THIS IS
;  A single self-elevating installer for the Katexdown Kate plugin plus the Qt
;  WebEngine runtime that Kate for Windows does not ship. It replaces the old
;  download-zip + elevated-PowerShell + install.ps1 flow: one file to download,
;  one double-click, one UAC prompt, done. Uninstall happens from Windows'
;  own "Apps & features" list.
;
;  WHY A SEPARATE RUNTIME, AND WHY IT MUST SIT NEXT TO kate.exe
;  The plugin DLL imports Qt6WebEngineWidgets, and Windows resolves a loaded
;  module's imports from the host process directory (the folder holding
;  kate.exe), not from the plugin's own folder. Kate for Windows ships no
;  WebEngine, so the runtime files have to land next to kate.exe — under
;  Program Files, which is why this installer requests elevation.
;
;  HOW TO BUILD (the CI does this; makensis runs with the payload staged by
;  packaging/windows/make-payload.ps1 next to this file):
;      makensis /DVERSION=0.3.6 /DQT_MINOR=6.11 /DPAYLOAD=dist\payload katexdown.nsi
;
;  Compile-time defines:
;      VERSION   the Katexdown version; goes into the exe file name and into
;                the Apps & features entry.
;      QT_MINOR  Qt minor series this payload was built against (e.g. 6.11).
;                The installer refuses to touch a Kate running a different
;                minor: across a minor bump Qt makes no ABI promise, and a
;                WebEngine plugin that mismatches simply never loads, with no
;                error shown in Kate.
;      PAYLOAD   the staged payload directory. Its layout mirrors a Kate
;                install root, so File /r below copies it 1:1 over the Kate
;                directory. make-payload.ps1 validates every file against the
;                Craft tree before staging; makensis additionally fails the
;                build if the staged tree is missing anything.
;      UNINST_FRAG  an NSIS fragment listing, for uninstall, every file the
;                payload staged. make-payload.ps1 generates it from the same
;                list it stages from, so the uninstaller's Delete list cannot
;                drift from the payload. !included in the Uninstall section.
;
;  UPGRADE / UNINSTALL MODEL
;  The uninstaller has the file list it will remove baked in at compile time
;  (the UNINST_FRAG above), so no manifest file is written at install time — a
;  manifest written before a copy could be left half-written by a crash, and
;  the baked-in list cannot. An install over an older install silently runs
;  the previous uninstaller first, which also removes files that newer
;  payloads no longer ship.
;
;  EASIEST WAY TO BUILD: run packaging/windows/build-installer.ps1. It derives
;  the version and Qt minor for you and drives make-payload.ps1 + makensis;
;  katexdown.nsi is only ever the hand-written part. The raw makensis
;  invocation it performs is:
;      makensis /DVERSION=… /DQT_MINOR=… /DEST_SIZE=… /DPAYLOAD=<payload> /DUNINST_FRAG=<list> katexdown.nsi
; ============================================================================

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef QT_MINOR
  !define QT_MINOR "6.11"
!endif
!ifndef PAYLOAD
  !error "Define /DPAYLOAD=<dir> pointing at the staged payload (see header)."
!endif
!ifndef UNINST_FRAG
  !error "Define /DUNINST_FRAG=<file> pointing at the uninstall file list written by make-payload.ps1 (see header)."
!endif

!define PRODUCT    "Katexdown"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Katexdown"
!define UNINST_EXE "katexdown-uninstall.exe"

Name "${PRODUCT}"
; ${__FILEDIR__} = the folder holding this .nsi, so the exe always lands next
; to the script no matter where makensis is invoked from. The CI moves it to
; dist\ afterwards.
OutFile "${__FILEDIR__}/katexdown-${VERSION}-windows-x86_64.exe"

InstallDir "$PROGRAMFILES64\Kate"
; An upgrade lands back where the previous install went (also covers a Kate on
; a custom drive/folder).
InstallDirRegKey HKLM "${UNINST_KEY}" "InstallDir"

RequestExecutionLevel admin
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"

; --------------------------------------------------------------------- pages
!insertmacro MUI_PAGE_WELCOME
!define MUI_DIRECTORYPAGE_TEXT_TOP "Katexdown installs into your Kate installation.$\r$\nThe installer finds Kate automatically — only change the folder if it picked the wrong copy. It must be the directory containing bin\kate.exe (Kate from kate-editor.org; the Microsoft Store version cannot host plugins)."
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
; Deliberately no "Start Kate" checkbox on the finish page: the installer runs
; elevated, and an editor must not be launched with an admin token.
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ------------------------------------------------------------------- helpers

; Scan one registry hive's Uninstall keys for an entry whose DisplayName
; starts with "Kate" and whose InstallLocation actually holds bin\kate.exe.
; Leaves the first hit in $0 ("" = none). The core is a macro because
; ReadRegStr/EnumRegKey take hive constants, not variables.
!macro _ScanUninstallHive HIVE TAG
  StrCpy $1 0
${Do}
  EnumRegKey $2 ${HIVE} "Software\Microsoft\Windows\CurrentVersion\Uninstall" $1
  StrCmp $2 "" ${TAG}_none 0
  ReadRegStr $3 ${HIVE} "Software\Microsoft\Windows\CurrentVersion\Uninstall\$2" "DisplayName"
  StrCmp $3 "" ${TAG}_next 0
  StrCpy $4 $3 4
  StrCmp $4 "Kate" 0 ${TAG}_next
  ReadRegStr $5 ${HIVE} "Software\Microsoft\Windows\CurrentVersion\Uninstall\$2" "InstallLocation"
  StrCmp $5 "" ${TAG}_next 0
  IfFileExists "$5\bin\kate.exe" 0 ${TAG}_next
  StrCpy $0 $5
  Goto ${TAG}_done
${TAG}_next:
  IntOp $1 $1 + 1
${Loop}
${TAG}_none:
${TAG}_done:
!macroend

; Find Kate when the installer cannot know where it is: registry first (the
; installer keeps its per-machine entry in HKLM, a per-user Kate registers
; under HKCU), then the two stock locations. Result in $0 ("" = not found).
Function FindKateDir
  StrCpy $0 ""
  !insertmacro _ScanUninstallHive HKLM t_hklm
  StrCmp $0 "" 0 found_kate
  !insertmacro _ScanUninstallHive HKCU t_hkcu
  StrCmp $0 "" 0 found_kate
  IfFileExists "$PROGRAMFILES64\Kate\bin\kate.exe" 0 +3
    StrCpy $0 "$PROGRAMFILES64\Kate"
    Goto found_kate
  IfFileExists "$LOCALAPPDATA\Programs\Kate\bin\kate.exe" 0 +2
    StrCpy $0 "$LOCALAPPDATA\Programs\Kate"
found_kate:
FunctionEnd

; $1 <- "1" when a kate.exe process is running. Kate is single-instance, so the
; image name is stable. We ask powershell.exe (present on every supported
; Windows) because its process lookup is case-insensitive and its output is not
; localized; if powershell is somehow missing, report "not running" and let the
; installer's own in-use handling cope.
Function KateRunningCheck
  nsExec::ExecToStack 'powershell.exe -NoProfile -NonInteractive -Command "[bool](Get-Process kate -ErrorAction SilentlyContinue)"'
  Pop $0
  Pop $1
  StrCpy $2 $1 4
  StrCmp $2 "True" kate_running_ps kate_stopped_ps
kate_running_ps:
  StrCpy $1 "1"
  Goto krc_done
kate_stopped_ps:
  StrCpy $1 "0"
krc_done:
FunctionEnd

; Everything that can make an install pointless, checked up front with a
; message that says why. Aborts on any failure. (MessageBoxes are inert in a
; /S silent install; the abort alone fails it, which is what the CI checks.)
Function ValidateKateDir
  ; --- must be a real Kate root
  IfFileExists "$INSTDIR\bin\kate.exe" 0 validate_not_kate
  Goto validate_qt
validate_not_kate:
  MessageBox MB_OK|MB_ICONSTOP "Katexdown installs into an existing Kate installation, but there is no bin\kate.exe under:$\r$\n$\r$\n$INSTDIR$\r$\n$\r$\nInstall Kate from the installer at kate-editor.org first, then run Katexdown again and point it at that folder. (The Microsoft Store version of Kate is locked down and cannot host plugins.)"
  Abort
validate_qt:
  ; --- Kate's Qt must match the Qt this payload was built against
  GetDllVersion "$INSTDIR\bin\Qt6Core.dll" $R0 $R1
  IntOp $R2 $R0 / 65536          ; major
  IntOp $R3 $R0 % 65536          ; minor
  StrCmp $R2 0 validate_qt_unreadable 0
  StrCpy $R4 "$R2.$R3"
  StrCmp $R4 "${QT_MINOR}" validate_kate_stopped 0
  MessageBox MB_OK|MB_ICONSTOP "Kate at this location runs Qt $R4, but this Katexdown build was made against Qt ${QT_MINOR}.x.$\r$\nA plugin built against one Qt minor series will not load against another, and Kate shows no error when that happens.$\r$\nUpdate Kate, or fetch the Katexdown build that matches the Kate you run."
  Abort
validate_qt_unreadable:
  MessageBox MB_OK|MB_ICONSTOP "Could not read the Qt version from:$\r$\n$INSTDIR\bin\Qt6Core.dll$\r$\n$\r$\nThis does not look like a working Kate installation. Reinstall Kate from kate-editor.org and try again."
  Abort
validate_kate_stopped:
  ; --- nothing Kate has loaded may be replaced while it runs
validate_kate_wait:
  Call KateRunningCheck
  StrCmp $1 "0" validate_done 0
  MessageBox MB_RETRYCANCEL|MB_ICONSTOP "Kate is running. Katexdown needs to replace files Kate has loaded, so close Kate first, then click Retry." IDRETRY validate_kate_wait IDCANCEL validate_kate_abort
validate_kate_abort:
  Abort
validate_done:
FunctionEnd

; --------------------------------------------------------------- .onInit

Function .onInit
  ; Kate for Windows and its plugins are 64-bit only: keep every registry
  ; access on the 64-bit view.
  SetRegView 64
  ; $INSTDIR already holds InstallDir / InstallDirRegKey / the /D= override.
  ; If it names a real Kate root, honor it (an upgrade reinstalls where the
  ; previous install went). Otherwise find Kate ourselves.
  IfFileExists "$INSTDIR\bin\kate.exe" 0 init_try_find
  Return
init_try_find:
  Call FindKateDir
  StrCmp $0 "" init_done 0
  StrCpy $INSTDIR $0
init_done:
FunctionEnd

; GUI-only: catch a wrong folder at the directory page instead of after the
; user clicks Install. Silent installs (/S) never visit the page; the install
; section's own ValidateKateDir call covers those.
Function .onVerifyInstDir
  Call ValidateKateDir
FunctionEnd

; ------------------------------------------------------------------- install

Section "Install" SEC_INSTALL
  SetRegView 64
  Call ValidateKateDir

  ; Upgrade: silently run any previous install's uninstaller first. Its file
  ; list is compiled in, so it knows about files this new payload dropped.
  ReadRegStr $0 HKLM "${UNINST_KEY}" "InstallDir"
  ${If} $0 == ""
    ReadRegStr $0 HKCU "${UNINST_KEY}" "InstallDir"
  ${EndIf}
  ${If} $0 != ""
  ${AndIf} $0 != $INSTDIR
    IfFileExists "$0\${UNINST_EXE}" 0 +2
      ExecWait '"$0\${UNINST_EXE}" /S _?=$0'
  ${EndIf}
  IfFileExists "$INSTDIR\${UNINST_EXE}" 0 +2
    ExecWait '"$INSTDIR\${UNINST_EXE}" /S _?=$INSTDIR'

  ; The payload (runtime + plugin, layout mirroring the Kate root) goes over
  ; the Kate install 1:1. _?= above keeps the old uninstaller from deleting
  ; itself, so WriteUninstaller can overwrite it in place.
  SetOutPath "$INSTDIR"
  File /r "${PAYLOAD}\*"

  WriteUninstaller "$INSTDIR\${UNINST_EXE}"

  ; Apps & features entry. InstallDir doubles as the "where did I go last
  ; time" pointer for upgrades (see InstallDirRegKey above).
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bin\kate.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" "$\"$INSTDIR\${UNINST_EXE}$\""
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" "$\"$INSTDIR\${UNINST_EXE}$\" /S"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  ; Size hint for Apps & features, in KiB. Computed by the CI and passed as a
  ; define; without it no size is shown.
  !ifndef EST_SIZE
    !define EST_SIZE 0
  !endif
  !if ${EST_SIZE} > 0
    WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" ${EST_SIZE}
  !endif
SectionEnd

; --------------------------------------------------------------- uninstall

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Uninstall"
  SetRegView 64
  ; Remove exactly the payload. The Delete list comes from the generated
  ; UNINST_FRAG (see header), which make-payload.ps1 wrote from the same list
  ; it staged the payload from — it cannot drift. RMDir below then only ever
  ; removes empty directories, so a kf6\ktexteditor folder holding other
  ; plugins (or Kate's own bin) is never touched.
  !include "${UNINST_FRAG}"

  RMDir "$INSTDIR\bin\translations\qtwebengine_locales"
  RMDir "$INSTDIR\bin\translations"
  RMDir "$INSTDIR\bin\kf6\ktexteditor"
  RMDir "$INSTDIR\bin\kf6"
  RMDir "$INSTDIR\bin"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKCU "${UNINST_KEY}"
SectionEnd
