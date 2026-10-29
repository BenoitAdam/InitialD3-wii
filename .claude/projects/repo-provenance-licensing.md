---
name: repo-provenance-licensing
description: "Legal cleanup done 2026-10-28 on branch legal-cleanup (artwork and upstream Windows-recompilation files removed, GPL-2 added); remaining licence caveats"
metadata:
  node_type: memory
  type: project
  originSessionId: 33536f51-beb2-447a-9efb-69457a952aa1
  modified: 2026-10-28T16:27:13.059Z
---

The repo root used to hold the upstream "Initial D Arcade Stage 3 Native SH-4 Recompilation" Windows project (distilledorion-sketch, unlicensed), copied into the first commit on 2026-10-15. On 2026-10-28 (branch `legal-cleanup`, commits b5cd107 and b658948, not merged or pushed) I removed it: logo/GIF/strip, translator/, src/runtime, tests/, tools/, docs/, STATUS/ROADMAP/CHANGELOG/BUILDING, root CMakeLists and the CI. The README, LEGAL.md, CONTRIBUTING.md, the issue template and port/README.md were rewritten for the Wii port. The user's non-affiliation disclaimer is verbatim in README and LEGAL.

The repo is now only port/ plus control.txt and the docs. The Wii binary is GPL-2 because port/emu/plugs/vbaARM comes from VisualBoyAdvance (GPL v2+). port/COPYING holds the GPL-2 text and port/LICENSE.md lists the components. At runtime users need their own gds-0033.chd, 317-0384-com.pic and a NAOMI BIOS zip.

Still open: port/emu/LICENSE says MIT only (the user may want to reconcile it); per-file GPL/MIT headers; the history still contains the removed upstream files and images (a history rewrite would be needed to drop them from old commits); the root `.gitignore` is unchanged; no Windows or Wii build was run after the cleanup.

**Why:** the user asked about legal exposure (Sega libraries, embedded game data, trademarks, reverse-engineering exceptions) and then asked for the cleanup.
**How to apply:** before suggesting a public release, mention the history caveat and the pending merge of `legal-cleanup`. Related: [[project-wii-port-state]]
