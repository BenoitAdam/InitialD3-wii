---
name: dolphin-setup
description: "Where the user's Dolphin emulator lives and how it is configured (SD sync folder, OSReport log file)"
metadata:
  node_type: memory
  type: reference
  originSessionId: 85dd9adc-0ec1-4108-993a-c23dab343adc
  modified: 2026-10-16T01:58:52.783Z
---

- Executable: `F:\Jeux-emulation(old)\NINTENDO\Wii\Dolphin\Dolphin-x64\Dolphin.exe`
- User config: `%APPDATA%\Dolphin Emulator` (Config\Dolphin.ini, Config\Logger.ini).
- Wii SD: `WiiSDCardSyncFolder = F:\Jeux_emulations\a@Recalbox\roms\wii\WiiSDSync\` (packed into `...\roms\wii\WiiSD.raw` at boot). The Initial D3 port's files sit in `WiiSDSync\initiald3\` (gds-0033.chd, naomi.zip, 317-0384-com.pic).
- Logger: OSREPORT + OSREPORT_HLE on, WriteToFile on -> `%APPDATA%\Dolphin Emulator\Logs\dolphin.log` (where SYS_Report output from the port lands).

- Settings seen 2026-10-17 (Dolphin.ini [Core]): OverclockEnable = True, Overclock = 1.71 (~1.25 GHz, about a Wii U Espresso), MMU = True (the port's demand paging needs it), DSPHLE = False (LLE DSP, DSP JIT on a DSP thread). So the port's speed % from Dolphin = an overclocked Wii in emulated time, not a stock Wii; Dolphin's own FPS/VPS counter is real time (user saw 18 FPS while the port said 100%).

Related: [[no-drive-search]], [[project-wii-port-state]]
