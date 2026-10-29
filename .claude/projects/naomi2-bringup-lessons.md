---
name: naomi2-bringup-lessons
description: "Non-obvious NAOMI 2 / Initial D3 boot facts found while bringing up the core (BIOS reboot trap, MIE bits, ELAN status, TA order)"
metadata:
  node_type: memory
  type: project
  originSessionId: 85dd9adc-0ec1-4108-993a-c23dab343adc
  modified: 2026-10-16T09:58:45.794Z
---

- If the game reads the main-board TEST button as pressed (MIE maple 0x86 sub 0x31, reply byte in(5) bit 4, active low) it calls BIOS service +0x44, which copies the BIOS ROM over RAM and runs the BIOS SYSTEM MENU — looks like progress but is not the game. in(5)=0xF9 is the safe value.
- Game polls ELAN reg 0x74: bit0 = texture DMA done (Link cmd with offset bit31 = from SH-4 RAM at DMAC SAR2, bit29 = from ELAN RAM), bit1 = command done. Missing bit0 = hang after the WARNING screen.
- Per frame the TA sees SoftReset, ListInit, lists, SoftReset, STARTRENDER: soft reset must not discard captured lists.
- Text on screen = 8x16 sprites whose 13th vertex word repeats the character code; scratchpad scripts tatext.py/scantext.py read it from RAM/ELAN dumps.
- Cart DMA offsets carry flag bits (0x2080xxxx / 0x4000xxxx): mask to the DIMM window (pow2 >= image). Unmasked, every load returned zeros and the game sat on a black screen after WARNING with the scheduler idling.
- Boot timeline at --cpi 2 (after the DIMM fix): renders from ~frame 1000, WARNING ~2400, SEGA ROSSO logo ~3500, title ~4100.
- Wii: PPCExcptSetHandler is an inline plain store; GCC moved it past a volatile guest read, so the first page fault hit libogc's panic (first Wii test, DSI at self_test). Put a `sync` + memory clobber after installing handlers.
- ELAN display lists are CHAINED (a buffer's last command Links to the next), far deeper than 16: a depth limit silently dropped all 3D ("display lists nested too deep"). Follow a tail Link as a jump (naomi_elan_cmd.cpp kMaxChain). Attract demo then rendered on Windows (~63K ELAN input vertices/frame).
- nullDC4Wii GX renderer buffers are sized for Dreamcast (42K vertices, 8K strips/params, no strip/param guard): NAOMI 2 T&L output must be chunked (<=1024 verts per header) and the BUILD_NAOMI2 caps raised (GX_VERTEX_CAP/GX_LIST_CAP).
- Wii memory: newlib malloc never returns memory to the arena, and the guest page pool only takes what is above the heap's high-water mark. Any big temporary buffer in the launcher (the whole 9 MB naomi.zip was read once) is lost to the pool for the whole run: stream files, keep only what is needed (BIOS: first 512 KB). At the title screen the 27 MB pool was already 1.5 MB from full.
- Red/magenta border + hang = Initial D 3's own assert handlers (write 0x00FF00FF to 0xA85F8040, spin); naomi_ReportAssert() logs PR (the failing call site).
- Windows runner `--press F:name,...` (coin/start/view/up/down/left/right/gas/brake/test/service) reaches menus without a pad.
- Dolphin does NOT enforce PPC page-table PP (read-only) bits: anything relying on a write fault to a RO page (shared zero page, write tracking) silently writes through. Probe at init (wii_fastmem.cpp zero_page_protected) and fall back. Symptom was a "jump into BIOS ROM 00000000" at frame 239.
- Wii recompiler keeps SH-4 GPRs pinned in r14..r28: Sh4cntx.r[] printed from C (e.g. in a trap) can be stale.
- Fast Wii iteration: scratchpad dolphin_run.sh (private Dolphin -u dir + autolaunch.txt) and naomi_ReportBiosEntry() traps. Ask before launching Dolphin windows if the user is around.
- When stuck with no hang: `--histo-from F` (PC histogram) + naomi_BoardLogCounters (per-register counts) found the culprit fastest.

**Why:** each cost hours to find. **How to apply:** check these first when the boot regresses. Related: [[project-wii-port-state]].
