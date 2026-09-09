# Windows packaging

End users never touch this folder. The one command that produces the installer is
`build-installer.ps1`; it drives the two hand-written pieces underneath:

```
.\packaging\windows\build-installer.ps1 `
    -CraftRoot C:\CraftRoot -PluginDll build\bin\kf6\ktexteditor\katexdown.dll -OutDir dist
```

That is the recorded recipe — the CI's `package` step in `.github/workflows/windows.yml` calls this
same script, so a manual build and CI can't diverge. The pieces:

1. **`make-payload.ps1`** — the single authoritative list of what the installer ships (the plugin
   plus the Qt WebEngine runtime Kate for Windows does not ship). It validates every file against
   the Craft tree, stages them into `dist\payload\` laid out like a Kate install root, and emits
   `dist\uninstall-payload.nsh` — the uninstall file list, generated from the *same* list, so the
   uninstaller can never drift from the payload.
2. **`katexdown.nsi`** — the hand-written NSIS installer. It embeds the staged payload (`File /r`),
   finds Kate, guards Qt-minor / Microsoft-Store / running-Kate, and compiles (with makensis) into
   the self-elevating `katexdown-<version>-windows-x86_64.exe`.
3. **`build-installer.ps1`** — derives the version from `CMakeLists.txt` and the Qt minor from the
   Craft `Qt6Core.dll` (nothing hardcoded), finds/installs makensis, and runs makensis with those
   defines. To rebuild just an installer from an already-staged payload (no Craft handy):
   `.\build-installer.ps1 -PayloadDir dist\payload -QtMinor 6.11 -OutDir dist`.

Why an installer exe instead of a zip + install script: the runtime DLLs must sit next to
`kate.exe` (Windows resolves a plugin's imports from the host process directory), which lives
under Program Files, so elevation is unavoidable. An exe self-elevates, needs no PowerShell, finds
Kate, refuses a Qt-mismatched or Microsoft Store Kate, and upgrades/uninstalls cleanly — every
step of the old download-zip → unzip → elevated-PowerShell → install.ps1 flow was a separate way
to fail.

See `design/windows-install.md` for the invariants and the pitfalls that shaped this.
