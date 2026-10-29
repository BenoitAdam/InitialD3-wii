---
name: local-reference-sources
description: "Where useful emulator sources live on this machine for the NAOMI 2 / Wii port (Flycast, nullDC4Wii, WiiStation libchdr)"
metadata:
  node_type: memory
  type: reference
  originSessionId: 85dd9adc-0ec1-4108-993a-c23dab343adc
  modified: 2026-10-15T17:25:18.515Z
---

- `F:/PROJETS/wii/nullDC4Wii` — the user's own Dreamcast-on-Wii emulator (MIT, handle Xale00 / BenoitAdam94): SH-4 interpreter + rec_v2 PPC dynarec, PVR->GX renderer (plugs/drkPvr/gxRend.cpp), AICA, menus in wii/main.cpp. Its CLAUDE.md forbids building it and committing without approval.
- `F:/PROJETS/wii/Emulators/flycast-master` — Flycast (GPL-2): NAOMI 2 reference (core/hw/naomi/gdcartridge.cpp, naomi_cart.*, core/hw/pvr/elan.*). Its core/deps/libchdr submodule is EMPTY.
- `F:/PROJETS/wii/Emulators/WiiStation-main/deps/libchdr` — Wii-ready libchdr (Makefile.wii, lzma/zlib/zstd deps); vendored into port/third_party/libchdr.
- Other ports for reference in `F:/PROJETS/wii/Emulators/` (Wii64, pcsxgc, gxgeo, nulldce-psp, nulldc-360, yabause wii).

Related: [[project-wii-port-state]]
