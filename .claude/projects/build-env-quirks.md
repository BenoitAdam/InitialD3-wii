---
name: build-env-quirks
description: "How to actually invoke devkitPPC make, MSVC/CMake and cmd.exe on this Windows machine (PATH shadowing traps)"
metadata:
  node_type: memory
  type: reference
  originSessionId: 85dd9adc-0ec1-4108-993a-c23dab343adc
  modified: 2026-10-16T02:02:15.605Z
---

- devkitPro make: Git Bash does not pass DEVKITPRO/DEVKITPPC (=/opt/devkitpro, devkitPro-MSYS paths) to devkitPro's make. Run through devkitPro's own shell: `MSYS_NO_PATHCONV=1 /c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /f/PROJETS/... && make'` (or port/build_wii.bat, which uses CHERE_INVOKING=1).
- `C:\devkitPro\msys2\usr\bin` is on PATH and shadows: `cmd` (call `$env:SystemRoot\System32\cmd.exe` by full path from PowerShell) and `cmake` (MSYS cmake cannot drive MSVC; prepend VS's `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` and `...\CMake\Ninja`).
- MSVC: VS 2022 Build Tools 14.44 at `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`; vcvars64.bat prints a harmless "'vswhere.exe' n'est pas reconnu" error.
- Windows locale is French (tool messages in French).
- Dolphin IS installed (not in Program Files): see [[dolphin-setup]] for the path. Run test builds with a separate `-u` user dir so the user's config/SD folder stay untouched.
- Bash heredocs (even quoted 'EOF') here turn `\\n` into a real newline inside generated C strings: write edit scripts/specs with the Write tool, never heredocs, when they contain backslashes. Scratchpad fedit.py applies (old,new) spec lists preserving CRLF/LF.
- Core build: scratchpad build_emu.bat (vcvars + VS cmake/ninja, re-runs configure so new globbed files are picked up) `--target idas3wii_run`; errsum.py summarises the log.

Related: [[project-wii-port-state]]
