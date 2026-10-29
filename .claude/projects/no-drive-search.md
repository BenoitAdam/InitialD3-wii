---
name: no-drive-search
description: "Never search the user's drives broadly for a program or file; ask the user for the path instead"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 85dd9adc-0ec1-4108-993a-c23dab343adc
  modified: 2026-10-16T01:58:57.382Z
---

Don't run recursive searches across the user's drives (C:\, F:\, D:\, Program Files...) to locate an executable or file. Ask the user where it is.

**Why:** the user rejected a recursive Get-ChildItem over several drive roots while I was looking for Dolphin and said "If you need info don't search everywhere in my drive, ask me."

**How to apply:** when a tool/file location isn't in the repo, memory, or a config file I already have, ask with one short question instead of scanning. Known locations: [[dolphin-setup]], [[local-reference-sources]].
