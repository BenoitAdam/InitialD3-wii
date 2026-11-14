# AGENTS.md

Guide for AI coding agents (Claude Code, Codex, Cursor...) working on this repository.
`CLAUDE.md` imports this file. Human docs: [README.md](README.md) (users) and
[port/README.md](port/README.md) (developers, source map). Speed work, done and left:
[port/OPTIMIZATION.md](port/OPTIMIZATION.md). Paths specific to the owner's PC (devkitPro,
Dolphin, reference emulators, SD folder) are in `AGENTS.local.md` (git-ignored): read it if
it exists.

## What this project is

- An **emulation-based port** of *Initial D Arcade Stage 3* (Sega NAOMI 2, GD-ROM GDS-0033)
  to the **Wii**, launched from the Homebrew Channel. App name: **Speed Boy 3**
  (`port/wii/meta.xml`). Version string in `port/src/version.h` (currently `alpha 0.26`).
- Core = fork of the owner's own **nullDC4Wii** (Dreamcast emulator for Wii, MIT; author
  handle Xale00 / BenoitAdam94), forked around nullDC4Wii commit 0996e7c on 2026-10-15,
  plus a NAOMI 2 layer written here (DIMM board, JVS/MIE, card reader, **ELAN** T&L chip).
- The same sources build for **Wii** (devkitPPC + libogc: the real target) and **Windows**
  (MSVC: launcher, headless runner and tests, used for bring-up, censuses and A/B checks).
- GitHub: `BenoitAdam/Initial-D-3-wii`, branch `main`. The owner commits often themselves.

### Status (2026-11-14)

- **LIGHT SUM (built 2026-11-14, awaiting the owner's Wii test)**. Owner: at night, where both cars' headlight
  beams overlap on the road, the road is black ("1 + 1 = 0"); each beam alone is fine. Every emulated path
  clamps the light sum (ELAN CPU lighting, the runner's GX preview, `toByte`, the beam's TEV stages), so it is
  the Wii's GX lighting. Runner census (env `IDAS3_HEADLIGHT_CENSUS`, `[gxsum]` lines: the sum GX's channel
  adds up before its clamp; night attract demo, `--point-lights 1`, frames 8400-12000): one car near = one GX
  light 4x strong (the game sends its headlight 4 times), sums past 2 at ~0.1% of the vertices it lights; both
  cars near = the game shares its 4 headlight slots (strengths 1+1, 2+2, 3+1, 4+1 seen), 2-3 headlights on
  a vertex, sums 2-6.6 at ~8% of those vertices (570K over the run). So the Wii's light sum most likely
  holds values up to ~2 and wraps past them (found documented nowhere; Dolphin clamps at 255 and will not
  show it). Built: **LIGHT SUM** (RENDERING 2, last row, last in `CoreOptionId`, default SCALED, FULL
  (OLD)): `pickLights` bounds what a point-lit list's GX sum can reach (ambient + each light on its axis,
  at the list's nearest point, N.L 1; headlights whose cone misses the list's sphere left out) and past
  `kSumSafe` 1.9 stores the set 1/2 or 1/4 as strong (`ElanGxLightSet::sumScale`); the draw
  (`sumBegin`) gives channel 0 a white material and three TEV stages after gxRend's stage 0: TEVREG2 =
  clamp(S x channel 0), x the material (TEVREG1), x the texture: the ELAN's material x min(1, sum), 1-2 bits
  less precise on those lists. Not for vertex-coloured, decal-alpha (TSP ShadInstr 2), AUTOSORT or
  SMOOTH-beam lists ("left over the safe sum"). Runner A/B (8400-11000): 13 snapshots pixel-identical,
  channel sums <= 1.89 (old: up to 6.6), ~100-130 lists a frame scaled. New `[prof]   LIGHT SUM per
  frame:` line (lists at 1/2, at 1/4, left over, drawn scaled). To read on the Wii: the overlap lit, no
  change elsewhere (banding on scaled lists?), `rend`. If the black stays with SCALED, the ~2 guess is
  wrong (the census line tells the sums; kSumSafe is the knob). Elf: `port/wii/build/boot_pcs_80043f74.elf`
  (also holds SCREEN BUFFERS, still awaiting log 35).
- **Log 34** (2026-11-14, `idas3wii_log_34prev.txt`: log 33's settings + INT DIVIDE NATIVE + JIT FPU BANKS
  RENAME, PC SAMPLER on, night Akina, in-car ~6480-12480 with two short chase stretches ~9500-9720 and
  ~9960-10080). In-car 59.4-60.8 fps, p50 14.5-15.0, typical `sh4` 6.55-6.8 against log 32's 6.96-7.2
  (**-0.4 ms**, same course and view); chase 52.7-58.6 (`elan` 5.6). The three hot loops of log 32/33 are
  gone from the profile (word copy 0C20A2A0 1.47% in-car in log 32, list copy 0C1D4F4C 0.63%, divide
  0C2223FA 0.45%); all calls native, 0 turned down. JIT FPU BANKS: 19 stretches in all (13 closed with no
  swap); the matrix library 0C1F6000-0C1F6BFF is still 3.0-3.7% of in-car samples, its gain not separable.
  **Owner: "some kind of tearing" with any of the three new rows on, fine with all three off.** Found in
  gxRend: with two XFBs, a render's display copy goes to "the other" buffer, which is still the one on screen
  until the last flip reaches the next real retrace; the emulation is not paced to the Wii's retrace (SPG
  deadline 59.79 Hz vs VI 59.94, audio-ring waits) and a flip is asked for only at the next emulated vblank
  or render, so the copy often lands under the scan. The sooner the SH-4 reaches STARTRENDER, the likelier:
  the three rows (all SH-4 speed) make it more frequent; the bug is older. Built, **awaiting log 35**:
  **SCREEN BUFFERS** (RENDERING, last row, last in `CoreOptionId`, default 3 (NO TEARING), 2 (OLD)): a third
  XFB (`xfb_target`: the copy goes to one neither on screen, nor asked for, nor waiting for its flip, from
  libogc's `VIDEO_GetCurrentFramebuffer`/`VIDEO_GetNextFramebuffer`); `_vmem.cpp` leaves the heap
  `kScreenBufferBytes` (720 KB) more when chosen. The `[prof] frames shown per second` line now ends with
  `| N screen buffers: X copied onto the picture on screen (torn)` (checked at each render's draw-done:
  the copied XFB still `currentFb`): with 2 it measures the tearing, with 3 it must stay 0. Elf:
  `port/wii/build/boot_pcs_80043f24.elf`. OPTIMIZATION.md "Log 34".
- **Log 33** (2026-11-13, `idas3wii_log_33.txt`: log 32's settings + GAME COPIES NATIVE, night Akina,
  in-car then chase). Speed unchanged (chase 49.5-55, p50 18.6-19.7). The list copy went native but its
  C version is memory-bound and cost the same (`runBursts` 1.4-2.0% chase); the word copy did not:
  runner census, the game calls the word loop's own entry 0C20A29E directly (2.1M against 0.13M calls
  at 0C20A280), which was not hooked (0C20A2A0 still 2.2-2.4% chase). New `[PCS] SH-4 code by 1 KB`:
  the matrix library 0C1F6000-0C1F6BFF ~4% (the recompiler's `frchg` bank swaps and block ends at
  `fschg`/`frchg`), integer divides 0C2223B8/0C2223E0 ~0.9% (GCC's FPU divides: FPSCR writes end
  blocks). Built after it, **awaiting log 34**: GAME COPIES hooks 0C20A29E too (kind 4); the burst
  copy uses `dcbz` + source prefetch on the Wii; **INT DIVIDE** (RECOMPILER 2, last row, last in
  `CoreOptionId`, default NATIVE, EMULATED (OLD)): both divides in C, exact (zero divisor and INT_MIN/-1
  left to the game); new `[prof]   GAME COPIES per frame:` line (calls, native, words, turned down with
  the last addresses). Also built for log 34, at the owner's request ("let's do next big target"):
  **JIT FPU BANKS** (RECOMPILER 2, last row, last in `CoreOptionId`, default RENAME, SWAP (OLD)): the
  decoder renames the float banks at `frchg` instead of swapping 16 registers (FR/XF exchanged in the
  operands of the ops decoded while crossed; the physical swap only at block end, before an interpreter
  fallback or an FPSCR load); `fschg`/`frchg` end no block. For the matrix library (~4%). Checked on
  Windows with the new **SHIL executor** (runner `--shil-exec 1`): night attract demo and race route,
  record digests identical with it off and on. New `[prof]   JIT FPU BANKS:` line. Spans:
  `port/wii/build/boot_pcs_80043f10.elf`. OPTIMIZATION.md "Log 33".
- **Log 32** (2026-11-12, `idas3wii_log32.txt`: night Akina, PROFILER + **PC SAMPLER** on, CCALLS and
  HOTBLOCKS off, FRAMESKIPPING 0; in-car ~5900-9000, chase to ~11160). The fixed sampler works (owner).
  Speed as log 30 (in-car 57-60 fps, chase 49.5-55, p50 18.75). Profile (OPTIMIZATION.md "Log 32"):
  in-car C 42-48%, SH-4 JIT 32-36%, ARM7 JIT ~5%, trampolines ~2.2%, **idle 8-16%**; chase C 48-51%, JIT
  41-43%, ARM7 5-5.5%, trampolines 2.6-2.8%, **idle 0.5%** (CPU-bound). Top C: the ELAN walk `execute`
  ~5.4% (1.9-2.1% at its header read), the recording's list lookup ~3.5% (`findList` 1.4%,
  `pagesSettled` 0.7%, `elan_gx_list_box`), `elan_gx_draw` ~3%, `tnl_DrawList` ~2.8%, `tnl_SetMatrix`
  1.1%, sound ~2.7%. Top SH-4 blocks (chase): 0C20A2A0 2.5% and 0C1D4F4C 1.9%, two game copy loops
  (runner trace: the word copy 0C20A280 moves 32/64 bytes from main RAM into the store queues; the
  display-list copy 0C1D4F20 bursts lists into ELAN RAM), then 0C1DC008 1.2% and a flat tail.
  Built after it, **awaiting log 33**: **GAME COPIES** (RECOMPILER 2, last row, last in `CoreOptionId`,
  default NATIVE, EMULATED (OLD)): `naomi_copies.cpp` does the word copy's 32-bit loop and both parts
  of the display-list copy (0C1D4EA0 head, 0C1D4F20 rest) in C at their entry (as SOFT FLOAT), same
  memory, registers, T and cycles, fingerprinted, anything unusual left to the game's code; expected
  ~0.5-0.8 ms off a chase frame. PC SAMPLER: block table 8192, new `[PCS] SH-4 code by 1 KB` line.
  Resolve this build's spans with `port/wii/build/boot_pcs_800436d8.elf`.
- **Log 31b** (2026-11-12, file `idas3wii_log31.txt`: the build below, night Akina, PROFILER + JIT
  CCALLS on, HOTBLOCKS off, CACHE HINTS on, FRAMESKIPPING 0, in-car to ~8640 then chase to the end).
  **PC SAMPLER froze the game at start** (owner), so this run has it off. Cause, from libogc's
  disassembly: its idle thread (`KThreadIdleMain`, a `b .` loop that runs whenever every thread waits)
  has a zeroed context but SRR0/SRR1, so **r1 = r2 = r13 = 0**; libogc's IRQ entry switches to
  `__ppc_excpt_sp` and never touches the interrupted stack, but the sampler's handler pushed its frame
  on r1: a DSI at 0xFFFFFFA0 inside the handler (RI = 0), libogc's stub then panics on the same bad
  stack. **Fixed, awaiting log 32**: the handler runs on its own 1 KB stack (`g_fp_pcs_stack`) and loads
  `_SDA2_BASE_`/`_SDA_BASE_` into r2/r13 like libogc's IRQ entry; samples in the idle loop (the `b .` at
  the PC) are counted apart, "idle (every thread waiting)" in the `[PCS]` header. Shipped elf:
  `port/wii/build/boot_pcs_800436b8.elf` (`frame_prof_dump` at 800436B8). Speed: in-car 59.1-60.3 fps,
  p50 14.6-15.9 (`sh4` 7.05-7.3, `elan` 3.0-3.7, `rend` 1.03-1.43, `arm` 1.25, `aica` 1.03-1.17);
  chase 46.8-53.8, p50 18.6-20.7 (`sh4` 9.0-9.6, `elan` 4.5-4.9, `rend` 1.9-2.1): log 30's numbers.
  DISPLAY LISTS 1 MB ring. CACHE HINTS shows no net gain against log 30 (other stretch, CCALLS on):
  walk 792-914 ns a command (log 30 750-800, ~5-10% slower), T&L ~2.95 us a box in-car and 3.3-3.4
  chase (log 30 3.2-3.8 and ~3.5, ~5-10% faster), the draw's rest 1.7-2.2 us a list (1.6-1.9).
  Counters without HOTBLOCKS: ELAN commands in-car 2.95-3.8 ms a frame at IPC 0.56-0.65, load-miss
  32-41% (chase 4.4-5.6 ms); game + sound + others IPC 0.37-0.43 in-car (0.47-0.51 chase), load-miss
  9-14%, and **9-11K L1 instruction misses/ms**: the game's own, not HOTBLOCKS' (log 30's 7.3K/ms was
  the whole rest, the ELAN commands' ~1K/ms included).
- **Log 31** (2026-11-12, `idas3wii_log31_akina_uphill`: night Akina uphill, PROFILER + JIT CCALLS +
  **JIT HOTBLOCKS** on, CACHE HINTS on, in-car then chase ~7380-8130 then in-car). A diagnostics run,
  not a speed reference: HOTBLOCKS' block table (16384 x 28 B = 448 KB, malloc'd at the first compile)
  left too little heap for the 1 MB DISPLAY LISTS ring (`[elan] GX display lists: no memory`), so
  `cached programs` took 0.7-1.35 ms a render instead of log 30's 0.23. In-car 59-60 fps, heavy
  stretch (8760-9000) 46.8-51.8, chase 46.9-53.3. First read of the ELAN-phase counters: the ELAN's
  own work (walk + recording) is 3.4-3.65 ms a frame in-car, 4.0-4.75 chase, at IPC 0.60-0.66 with
  32-38% of its cycles in L1 load misses (~2000-2250 L2 data misses/ms, ~120 cycles each): ~1/3
  memory stall, ~2/3 instructions (~1.5M PPC instructions a frame for ~2100-2700 commands and
  ~550-800 lists). Game + sound + the rest: IPC 0.47-0.53, load-miss 12-15%, but ~10-11.5K L1
  instruction misses/ms (one per ~35 instructions; log 31b shows the same without HOTBLOCKS). HOTBLOCKS (entry counts, table overflowed by 5559 blocks): chase runs the VRAM32 copy
  loop 0C20A2A0 ~1.6x more (400-450K entries/s vs 265K) and an SQ copy loop 0C1D4F4C (pref) that
  in-car barely runs. CACHE HINTS cannot be judged from it (the draw's rest 1.5-1.8 us a list vs
  1.6-1.9; walk 813-911 ns a command vs ~800, under HOTBLOCKS' code pressure).
  Built after it, **awaiting log 32**: (1) heap headroom for the diagnostics (`_vmem.cpp` adds
  `hotblocks_heap_bytes()` and `frame_prof_heap_bytes()` to the pool's headroom) and a DISPLAY LISTS
  ring that falls back to 768 then 512 KB; (2) **PC SAMPLER** (LOGS, last row, default OFF, with
  PROFILER): Broadway's performance monitor interrupt every 72,900 cycles (10 kHz; PMC1 counting
  cycles, MMCR0 ENINT + PMC1INTCONTROL, handler at PPC_EXCPT_PM through libogc's stub like the DSI
  handler) counts the interrupted PC: C code by 256-byte span from 0x80003F00, SH-4 JIT samples by
  block (walked back to the block's `addic. r29` prologue, the SH-4 address read from its timeslice
  exit's lis/ori), ARM7 JIT, trampolines, other. `[PCS]` lines every ~8 s; it takes the PMCs, so no
  `[HW] CPU counters` lines while on. Resolve the C spans with `powerpc-eabi-addr2line -f -C -e
  boot.elf ADDR` on the boot.elf whose `frame_prof_dump` address matches the `[PCS]` header (keep a
  copy of each shipped boot.elf as `port/wii/build/boot_pcs_<address>.elf`; `make clean` deletes it).
  This first build froze at start on the Wii (log 31b above: fixed).
- **Log 30** (2026-11-12, two runs of night Akina with the five rows below on, FRAMESKIPPING 0,
  MIRROR OFF + GAME SKIP; `idas3wii_log_30_akina_uphill` JIT CCALLS off, `..._downhill` CCALLS on).
  Owner: "more performance indeed". **In-car: 59.3-60.8 emulated fps nearly all race**, p50
  15.0-15.5 ms (typical frame: `sh4` 6.95, `elan` 3.5 = walk 1.68 ms at ~796 ns a command + T&L 2.06,
  `rend` 1.44, `arm` 1.16, `aica` 1.16); its heaviest stretch (uphill frames 9000-9360, ~74K
  vertices, 744-764 boxes) 53.6-58. Against log 27: walk 928 -> ~800 ns a command, the draw's rest
  1.6-1.9 us a list, timeslice call-outs ~1870 -> 933 a frame (SH-4 TIMESLICE: nothing broke).
  **Chase view at night (downhill frames ~8520-9360): 45-54 fps**, p50 18.9-19.8: `sh4` +2 ms over
  in-car (9.0-9.2 with CCALLS on: the game's own work, more objects in view), `elan` ~4.5 (+1.3),
  `rend` 1.8-2.0 (+0.6). Renders: IPC 0.60, load-miss 36-42% of cycles, L2 hits ~230/ms against
  ~2500 L2 data misses/ms: next to nothing survives in the 256 KB L2 between the ELAN recording and
  the render (~8 misses a list). FPROF tails there are `snd` (the pacing wait at 100%), not work.
  Built after it, **awaiting log 31**: CACHE HINTS (RENDERING 2, last row, default ON; exact, hints
  only): the renderer's per-render tables (items, matrices, light sets) are zeroed a line ahead with
  `dcbz` instead of read before being written, the draw prefetches items, matrices and light sets
  ahead, the walk asks for a model's lists' fetch-table entries, page states and boxes at its call
  (the fetch slot each head found is kept with MODEL PREFETCH's headers), gxRend's strip loop reads
  ahead. And the PMC split of the ELAN phase: `[HW] CPU counters A/B, the rest: ELAN commands ... |
  game, sound and the others` (IPC, load-miss, L2 per ms of the ELAN's own work vs the rest).
  OPTIMIZATION.md "Done 2026-11-12 (2)".
- **FIVE SPEED ROWS (built 2026-11-12, log 30: in-car night ~100%)**. Owner: "90% in night race, need 10%
  more" (no log: view and course not known). Found: Broadway has no square root instruction and
  devkitPPC's `sqrtf` is newlib's 25-step bit loop (~300 cycles); a night point-lit list paid 2-3 of
  them, each lit matrix change at draw 3 more, and each material 4 divisions + 2 `ldexpf`. New rows,
  all defaulting to the new way, old kept: LIST MATHS (RENDERING 2: frsqrte-based upper bounds,
  the list's sphere only for HEADLIGHTS SOFT/SMOOTH), 3D DECODE (RENDERING 2: exact tables for
  colours and gloss, `boxView` scale squared), DRAW MEMO (RENDERING 2: the draw reuses the fetch
  slot found when recording; normal-matrix scale via frsqrte; ambient not reconverted), SOUND STEP
  (AUDIO: exact, small), SH-4 TIMESLICE (CORE: 3584 cycles, devices keep their cadence; the only
  one that changes timing: first suspect). Estimated ~0.8-1.2 ms off a drawn night frame from the
  first three, ~0.2-0.4 from the timeslice. OPTIMIZATION.md "Done 2026-11-12".
  "Compiled models" (walk) was not built: MODEL PREFETCH already covers the headers it would save.

- **TEXTURE CACHE VERY FAST+ and VQ AS CMPR (built 2026-11-11, awaiting the owner's Wii test)**.
  Owner: VERY FAST+ with VQ AS CMPR "not very fast", greenish glitches. Runner census (env
  `IDAS3_TEXCACHE`, boot to a Myogi race): VERY FAST+ aliased on NAOMI 2 (skimp slots fold the 16
  MB of VRAM into 4 MB of the 8 MB buffer, and NAOMI 2 has no overflow arena): 33.5 decodes a
  render over the run, 56-93 in the race, against NORMAL's 0.25; ~31% of binds would show pixels
  or a texture object another decode wrote over (the glitches). **Initial D 3 has no VQ 565
  texture** (547 textures, one VQ of another format), so VQ AS CMPR does nothing in this game.
  Built: TEXTURE CACHE VERY FAST+ (preset 7) = NORMAL's sized arena + VERY FAST+'s fixes + a
  per-render bind memo (a race render binds ~600-750 headers, ~180-210 distinct: ~70% of binds skip
  the map probe, descriptor read and guest VRAM read); the old one is VERY FAST+ (OLD) (preset 6,
  last value of the row: autolaunch.txt holds indices). VQ AS CMPR ON = a new encoder
  (`cmpr_encode.h`: principal colour axis, always GX's 4-colour mode); ON (OLD) = the luma encoder,
  which the FMV paths keep. On 158 of the game's plain RGB565 textures (185K blocks, as GX
  decodes them): old 3.15% of blocks in 3-colour + transparent mode, PSNR 30.7 dB; new 0%, 31.2
  dB, 1.7x the encode time. To test on the Wii: the same race with NORMAL then VERY FAST+, `rend`
  per drawn frame (expected gain small: 2-3 cache misses a repeated bind, ~0.1-0.4 ms) and no
  glitch.
- **HEADLIGHTS (built 2026-11-10)**: the night headlight beam showed the road's polygons; RENDERING
  2, last row: ARCADE (OLD) / SOFT / SMOOTH (projected per-pixel cone). Owner's Wii test: "not much
  better"; the owner thinks the arcade had it too. Kept as options, ARCADE default; the look is dropped.
  Details: "Open issues", night headlight beam.
- **Night lighting made cheaper (2026-11-10, exact, no option; awaiting a night log)**: repeated
  light commands skipped, the last 4 point-lit picks cached, matrices and light sets found by key in
  the renderer, a quick reach test before each light's box test. Runner: sets built 13-67 -> 8-18 a
  frame, set compares 120-244 -> 45-90 a render, 55-67% of light tests settled cheaply; day-route
  digests identical. Expected ~0.3 ms off a drawn night frame. OPTIMIZATION.md, "Done 2026-11-10".
  2P log 2 (a night race in 2 PLAYERS) shows the counts the runner gave: 111 light commands a
  frame, 63 skipped; 14-21 sets built, 160-230 reused; the quick test settles 54-66% of 1.2-1.7K
  tests. Its time is not comparable to logs 23/24 (2 PLAYERS, JIT CCALLS on): T&L 1.7-1.8 ms for
  ~520 lists with a box, ~3.3 us a list. Log 26 (one player, night, in-car, every frame drawn,
  JIT CCALLS on): 3.1-4.1 us a list with a box (T&L 1.3-2.6 ms for 390-730 lists) against log 23's
  2.85-3.6, so no gain shows; the course and view may differ (not known).
- **Log 26** (2026-11-11, `idas3wii_log_26`, one player, a night race in-car, FRAMESKIPPING 0,
  MIRROR OFF, SMALL 3D 4 px, JIT CCALLS on): 51-57 emulated fps, p50 17.9-19.3 ms: `sh4` 9.5,
  `elan` 3.9-4.6 (walk 2.3-3.0 ms, 3100-3700 commands at 750-810 ns; T&L 1.3-2.6), `rend` 1.1-1.5,
  `arm` 1.52, `aica` 1.05-1.1. `[CC]`: `sync_fpscr` 1850-2070 call-outs a frame (30% of them; ~62
  in menus). MIRROR OFF per frame: 176-182 models not drawn (~110 from their summary), 600-690
  lists, 43-53K vertices. Mirror audit (owner: "no FPS boost, still calculated behind?"): MIRROR
  OFF takes the mirror's lists out of the recording and the draw (~620 lists: at ~3.4 us recorded
  and ~2.3 us drawn a list, ~3.5 ms a drawn in-car frame; logs 20/21: ~31 against ~25 shown fps
  under AUTO). Left: the game's own mirror pass on the SH-4 and the walk of its commands. Runner
  (day route, PC histograms, wait loop left out): in-car 864K SH-4 instructions a frame, chase
  848K; the routines that run more in-car add ~43K (~5%: 0C207C00-0C2083FF +15%, 0C1FC000-0C1FC7FF
  +45-50%, 0C1D3800-0C1D3FFF +40%, 0C1D8000 +8%...), so the game's mirror pass is ~0.45 ms of a Wii
  `sh4` at most; its walk (~170 model calls and matrices, ~65 models walked without drawing) maybe
  ~0.5 ms. Skipping it would need the game's mirror routine found and stepped over (not done).
  Summaries for mirror-only models were retried and dropped again (OPTIMIZATION.md, Tried and dropped).
- **Four speed items built after log 26 (2026-11-11)**, each behind a launcher row with the old
  behaviour kept (OPTIMIZATION.md, "Done 2026-11-11: four items"): MIRROR gains OFF + GAME SKIP
  (value 2, not the default): the game's per-view draw 0C024100 returns at once for the mirror's
  view (word +138 = 2), so the game builds no mirror at all (runner in-car: 2572 ELAN commands a
  frame instead of 4098; snapshots pixel-identical to MIRROR OFF, player state identical). JIT FR
  SWAP (RECOMPILER 2, default INLINE): the FPSCR.FR bank swap in the recompiled code, no C call
  (Wii only). MODEL PREFETCH (RENDERING 2, default ON): a model's command headers, remembered from
  its last walk, asked for at its call. SOUND MIX (AUDIO, default FULL): HALF mixes every other
  sample (exact registers); **no gain on the Wii, keep FULL** (log 27).
- **Log 27** (2026-11-11, `idas3wii_log_27`, one player, a night race in-car all race,
  FRAMESKIPPING 0, MIRROR OFF + GAME SKIP, JIT FR SWAP INLINE, MODEL PREFETCH ON, SOUND MIX HALF,
  JIT CCALLS off). Owner: "FPS improved". **52-61 emulated fps, mostly 59-60** (log 26: 48-57,
  mostly 52-54), p50 14.4-17.6 ms (17.9-19.3). On a matching stretch (log 26 frames 8400 / log 27
  8640: 343/364 lists drawn, 29/32K vertices, T&L 1.92/1.91 ms for 463/455 boxes) the typical
  frame is ~13.9 ms against ~17.6: `sh4` 9.50 -> 7.09, `elan` 4.05 -> 3.26 (walk 2.63 -> 1.80 ms,
  3229 -> 1937 commands, 813 -> 928 ns a command: the mirror's commands were the cheap re-walks),
  `arm` 1.52 -> 1.11, `rend` 1.27 -> 1.19, `aica` 1.07 -> 1.09. Log 26 had JIT CCALLS on, whose
  census costs `arm` ~0.4 (block-entry counts and timers in the ARM7 JIT) and `sh4` perhaps ~0.2:
  so the items gave ~3 ms. The `sh4` drop (~2.2 ms) is GAME SKIP + FR SWAP together, not separated
  (the runner put the game's mirror pass at <= ~0.45 ms, so FR SWAP may be most of it, far above
  the ~0.2 ms once estimated; a run with JIT FR SWAP = C CALL would tell). `[prof] MIRROR GAME
  SKIP` 120 of 120 frames all race; light commands decoded a frame 111 -> 54. MODEL PREFETCH:
  170-310 models a frame; its effect is not separable (an OFF run would tell). SOUND MIX HALF gave
  nothing: `vox` per voice per emulated frame is log 26's (~0.015% of a second), because a held voice
  (in decay with D2 level 0: engines, held notes) still reads its sample every sample for the
  attack-restart check, and stepping it (envelope, stream, LFO) is most of a voice's cost.
  Heaviest stretch (frames 9600-10400: 560-620 lists drawn, 58-67K vertices, 650-765 boxes): p50
  17.4-17.6, 55-58 fps (`sh4` 7.4-7.6, `elan` 4.4-4.8, `rend` 1.7-2.1, sound 2.25). Harmless:
  "Write to Boot ROM addr=40/50" lines in the attract demo (~frame 3360, also in 2P log 2), the
  game writing through a null pointer; the four `addr=74-77` ones are older still.
- **MOTION FX (built 2026-11-10, untested on a Wii; owner to test)**: smoothing a race shown at
  30 fps, launcher RENDERING 2 page, last two rows: MOTION FX (OFF default / SPEED BLUR / FRAME
  BLEND / IN-BETWEEN / BLUR + IN-BETWEEN) and MOTION LEVEL (LOW / MEDIUM default / HIGH). Drawn by
  gxRend (`mfx_*`) after the whole frame as quads at W 0.2 with a GREATER depth test, so only the
  scenery changes: the HUD is all nearer than W 0.15, the scenery farther than W 0.25 (runner,
  `IDAS3_STRIP_ORDER`), but the HUD is NOT drawn last (see the lessons). A Z8 copy of the depth
  (as AUTOSORT's) turns fetches of HUD pixels transparent (TEV alpha compare). SPEED BLUR: two
  fetches back along the motion (outward with the speed, sideways when turning), from a half-size
  copy. FRAME BLEND: the last drawn frame's half-size copy mixed over this one (25/40/55%, with
  the speed). IN-BETWEEN (needs FRAMESKIPPING, in-car view only, not 2 PLAYERS): the drawn frame's
  display copy keeps the EFB, and a dropped frame's STARTRENDER warps it by one frame of the car's
  turn (exact rotation) and step (depth from a road-and-walls corridor, drawn in three depth
  bands: past 0.2, 20, 120 m) and presents it; the next render's start clears a kept EFB no
  in-between used (`mfx_settle`). Its Z8 buffer (`kMotionMaskBytes`, 360 KB) widens the heap
  headroom (`_vmem.cpp`) only when chosen. Motion (`naomi_motion.cpp`): the followed car's render
  state (0C8FF388 player / 0C8FF430 CPU car: velocity a frame in metres, yaw in radians, + to the
  left), the race view from `naomi_RaceView` (2P's findRace), the in-car view from
  `g_elan_mirror_seen`, the focal length from the ELAN projection most vertices use (race: fx
  -404.8 the scene, +280.9 the mirror, -579.4 the HUD). The mirror's user-clip rectangle is kept
  out of every effect. `[prof] frames shown per second` adds the in-between frames. To check on
  the Wii: the look of each mode in-car and chase, HUD and mirror untouched, no hitch when the EFB
  is kept, the in-between's turn direction (wrong sign = double judder in corners), the cost
  (`rend` per drawn frame; dropped frames' time now has the in-between in `rend`).
  MOTION FX log 1 (2026-11-10, "BLUR_IN_BETWEEN_noframeskip"): options were MOTION FX IN-BETWEEN
  (not BLUR + IN-BETWEEN), LEVEL HIGH, FRAMESKIPPING 0, so it tested nothing of the effect: every
  frame drawn, `0.0 MOTION FX in-between frames` all through. Race (from frame ~7560) 56-61 emulated
  fps, p50 16.2-17.7 ms, `rend` 0.95-1.6, walk ~800 ns a command; no hang, no memory trouble. Still
  to test: FRAMESKIPPING AUTO with IN-BETWEEN and BLUR + IN-BETWEEN, in-car.
  MOTION FX log 2 (2026-11-10, 1 player, FRAMESKIPPING AUTO, in-car; `..._1P_FRAMEBLEND`, `..._1P_IN_BETWEEN`):
  both ~100% speed in the race, p50 ~16.5-16.9 ms; IN-BETWEEN shows 15-24 in-between frames a second, no
  measurable cost. Owner: FRAME BLEND is ok; IN-BETWEEN and BLUR + IN-BETWEEN **tear the opponent car**
  ("horrible"). Cause not proven (GX output cannot be seen on Windows); most likely the three depth bands:
  each repaints the pixels deeper than 20 / 120 m with a shift from the corridor *guess* of depth, which a
  car (not on the corridor's model: roof = "sky", wheels = "road") breaks, so a car near a band edge is cut
  along the contour into pieces shifted by different amounts. Built after it, **awaiting log 3**: BETWEEN WARP
  (RENDERING 2, last row, last in `CoreOptionId`, default SMOOTH): SMOOTH = one pass over the scenery with
  the same guess (no contour, so no seam; a car is bent smoothly with the field, and it is warped as if
  static: its own speed against ours is ignored); BANDS (OLD) = the three passes. Not done, if SMOOTH is not
  enough: the opponent's own relative motion/position (render state 0C8FF430, the `naomi_motion.cpp` `Car`
  has it; the up/lateral axes are not identified yet), or depth from `GX_PeekZ` at the grid vertices (the EFB
  depth is intact in a kept frame; W = p6 / (z - 1 + p5)). To test: IN-BETWEEN, then BLUR + IN-BETWEEN,
  BETWEEN WARP SMOOTH vs BANDS (OLD), in-car, FRAMESKIPPING AUTO, watching the opponent car and the road.
  MOTION FX log 3 (2026-11-10, `idas3wii_log_blur` = BANDS, `..._blur_prev` = SMOOTH, MOTION FX IN-BETWEEN,
  AUTO, in-car): owner: both tear heavily, SMOOTH worse, the scenery too. Speed fine in both (100%, 17-21
  in-between frames a second of ~29 dropped; so ~10 dropped frames a second keep the old picture). SMOOTH
  hypothesis wrong: it dropped the bands that kept the corridor guess (walls 3.5 m out) off far scenery, so
  buildings 20-60 m away were over-shifted. Best remaining guess for the car: the warp moves it as a static
  object while it keeps pace with us (a 5-10% size pulse at 30 Hz), plus the guess breaking on a body that
  is not road. Not proven. Built after it: BETWEEN WARP values ROTATION ONLY (the turn, no step: exact at
  any depth) and NO MOVE (TEST: the same picture through the same path), default back to BANDS (OLD). Reading
  the owner's answer: NO MOVE tears => the path (copy/grid/present), not the warp; NO MOVE clean but ROTATION
  ONLY tears => the yaw read (sign/size); both clean => the translation/depth guess is the cause, and the way
  on is the opponent's own state (0C8FF430; axes unidentified, print its position/velocity with
  IDAS3_MOTION on the runner) or the depth at the grid vertices (GX_PeekZ).
  Owner's answer (2026-11-10): ROTATION ONLY is clean => the forward-step/depth guess was the cause. It is now
  the default (BETWEEN WARP first value). In 2 PLAYERS only SPEED BLUR is wired (`mfx_after_render`: FRAME
  BLEND and IN-BETWEEN need the whole picture, `s_mfx.half < 0`); SPEED BLUR in 2P is untested.
- **2 PLAYERS (branch `two-player-splitscreen`), works on a real Wii** (owner, 2P log 1,
  2026-11-07: "amazing it works"). Launcher rows (the PLAYERS page was folded into GENERAL on 2026-11-11, P2 SCENERY went to RENDERING 2):
  2 PLAYERS (OFF / SIDE BY SIDE), GC PAD LAYOUT (A/B PEDALS default, R/L PEDALS
  (OLD)), P2 CAR (ON RAILS (OLD) / FULL PHYSICS default), P2 SCENERY (AROUND P1 (OLD) / AROUND
  EACH CAR default). The game still draws one view a frame; the cars take turns being the one
  its in-car camera follows (the rig's target car, `naomi_2p.cpp`), and the renderer squeezes
  each view into its car's half and keeps the other half (gxRend `split2p_*`: the half's
  viewport and scissor, each view clears its own half when it starts, two half EFB copies that
  clear nothing). 30 views a second each, no more drawing than one player; each view gets half
  the picture's aspect, the mirror is off. Timing: a render shows the scene built in the game's
  previous frame from the camera of the frame before, so the car changes at every STARTRENDER
  and a render belongs to the car of two STARTRENDERs before; the HUD and the view's course
  setup built in a frame belong to the car just given to the rig. Frameskip decides for a pair
  of views (`naomi_2pSkip`). The second pad (GameCube port 2 / Wiimote 2) drives the CPU car.
  FULL PHYSICS: the game's player physics runs a second time a frame for player 2 on a swapped
  copy of its state (block 0C9008A4-0C9017D3 plus what the physics writes elsewhere), player 2's
  pad as the JVS analog words; the CPU car's render state and AI body are put where it ends up.
  ON RAILS: the CPU car stays a kinematic AI body, player 2 sets its speed and nudges its
  heading. AROUND EACH CAR: the course draws 3 of its 9 areas around the view's own car's road
  segment; for player 2's scenes the view's own and opponent segment slots are swapped around
  the course's setSegment call (AROUND P1: player 2's view loses its scenery from ~800 m
  apart). HUD of player 2's view: its own speed and revs (JIT hook 0C0C9A98, `naomi_2pHud`).
  2P log 1 (a day course, in-car, two runs; P2 drove up to 181 km/h and steered, sat at 1-11
  km/h for ~50 s with the gas held, then drove on; cars up to ~670 road segments (~2 km, ~3.1 m
  a segment) apart): every frame drawn, 56-60 emulated fps the first ~80 s (p50 16.3-17.3 ms: `sh4` 8.55-8.8, `elan` 3.4-3.9,
  `rend` ~1.0, sound ~2.5), 54-58 later (p50 17.5-18, 360-410 lists a render), 50-54 in a
  stretch ~2 km apart under another camera (the renderer building 430-1150 GPU programs in 2 s,
  GPU waits, `rend` tail 6-7 ms: two cars bring in new scenery twice as often). FRAMESKIP AUTO:
  100% the whole race (59.5-60.1), 44-60 renders shown a second (mostly 52-59, 26-29 a player).
  Left and right views equal every 10 s; 600 physics runs per 10 s (every frame); the course
  placed around player 2 for all its views. Against log 25's single-player day race (p50
  15.5-16.3, `sh4` 7.7-8.3) 2 PLAYERS costs ~1 ms a frame, mostly `sh4`, if the course is
  comparable (not known). Built after it (2P log 2 read them): a `[2P]   player 2's physics:`
  line every 10 s (ms a run on average and the shortest, + the state swaps; the average can
  take in sound/timer work serviced in between), and the `[game] controllers` line fixed (it
  never printed, most likely because a GameCube pad read twice in one SI poll answers a
  transfer error, which PAD_ScanPads leaves out of its connected mask while keeping the
  buttons, so the mask never held steady; now a device counts if it answered in any of the
  last 60 reads).
  2P log 2 (2026-11-10, `idas3wii_log_nightrace2p`: a night race, in-car, FRAMESKIPPING 0,
  HEADLIGHTS ARCADE, JIT CCALLS on; player 2's pad never moved, its car sat at 0-4 km/h): every
  frame drawn, 50-54 emulated fps, p50 19.0-19.2 ms (`sh4` 10.1, `elan` 4.2-4.3 = walk 2.4-2.6 +
  T&L 1.7-1.8, `rend` 1.4, `arm` 1.6, `aica` 1.05). Player 2's physics 0.32-0.36 ms a run
  (shortest 0.22-0.31) + **0.41 ms of state swaps** a frame; `[CC]` shows the swaps as ~3.7K
  reads and ~3.7K writes of guest RAM from C a frame (13.5K call-outs a frame in all). The
  controllers line printed `GameCube ports 2; Wiimotes 1 + Nunchuk 2 3 4`: Wiimotes 2-4 were
  false (libogc never writes the data of a channel with no Wiimote, which stays zeroed, so its
  `err` read WPAD_ERR_NONE). Built after it (2P log 3 confirmed): the swaps are block copies
  (`_vmem_GetBlockPtr` + memcpy, the word loop kept as a fallback; runner 2P route: digests and
  `[2P]` lines identical), and the controllers line asks `WPAD_Probe` for the Wiimotes.
  2P log 3 (2026-11-11, `idas3wii_log_2P_26`, night, PROFILER and JIT CCALLS off, so no speed
  lines): swaps **0.087-0.096 ms** a frame (was 0.41 with CCALLS on), physics 0.28-0.33 ms a run;
  controllers line `GameCube ports 2; Wiimotes 1 + Nunchuk`; pad 2 still never moved. Still open:
  a GameCube pad on port 2 (did not answer in the first test, never retested with the log
  line); which controller drove player 2 in 2P log 1 is not known. Known limits: no manual
  shifting for player 2 (a manual gearbox stays in its gear), gear and ADVANTAGE in player 2's
  HUD are player 1's, player 2's view shows its own bonnet (the CPU car is drawn whole). Game
  addresses and how they were found: top of `port/emu/naomi/naomi_2p.cpp`. Runner tools for it:
  `IDAS3_PHYSW`, `IDAS3_POSREAD`, `IDAS3_MODELS`, `IDAS3_WRITEVAL`, `IDAS3_2P_HOLD`,
  `IDAS3_TIMELINE`, flags `--two-player`, `--p2-car`, `--p2-scenery`.
- Boots BIOS-free, menus, attract, car select and races all work on a real Wii.
- Menus/attract/car select: 100% speed. Race, every frame drawn (FRAMESKIPPING 0, MIRROR OFF,
  SMALL 3D 4 px): the day course of logs 21/23 at **100% in the in-car view** (log 25: 59.4-60.1
  fps the whole race, p50 15.5-16.3 ms) and 52-56 fps from the chase camera (log 23, before COLOUR
  LISTS); night, in-car: 49-54 fps before log 27, **52-61, mostly 59-60** with MIRROR OFF + GAME
  SKIP and JIT FR SWAP (log 27, p50 14.4-17.6), 59.3-60.8 nearly all race with the five rows of
  2026-11-12 (log 30, p50 15.0-15.5; heaviest stretch 53.6-58); night chase view 45-54 (log 30). FRAMESKIP AUTO settles on d = (16.7 - S) / (D - S)
  drawn (S a dropped frame, D a drawn one). Night in-car before log 27: S ~13.4 ms (`sh4` 9.5, sound
  ~2.3, ELAN walk ~1.2), D ~19-20, so AUTO showed ~36 (log 25). After it (estimate, AUTO not run
  yet): S ~11, D ~15.5-17.6, so AUTO should hold 100% with nearly every frame drawn. The owner's
  target (100% at ~45 shown) is met by day and, per log 27, at night in-car; the night chase view
  (no mirror to skip) is not measured since log 23.
- Latest real-Wii log: **log 34** (2026-11-14, the three SH-4 rows work, -0.4 ms `sh4` in-car; tearing
  found, entry above); before it **log 33** (2026-11-13, GAME COPIES half working); latest speed
  reference: **log 30** (2026-11-12, entry above). Before it **log 27** (2026-11-11, entry above), log 26 and 2P log 3
  (2026-11-11, entries above; log 26's walk, 750-810 ns a command on drawn frames with ELAN PREFETCH
  ON, matches 2P log 1's: the OFF A/B is still to do).
  Before them, **log 25** (2026-11-05, two runs). Day course, every frame drawn,
  in-car all race: 59.4-60.1 emulated fps, p50 15.5-16.3 (log 23: 57.5-60.9, 16.5-16.8); COLOUR
  LISTS took the 1.1-1.6K CPU-fed colour vertices a render to 0, "the rest" of the draw 2.9-4.3 ->
  2.0-2.1 us a list, a render's CPU -0.35 to -0.42 ms at equal lists; owner: no colour problem.
  Night Akina, FRAMESKIP AUTO, AUDIO STATS ON: the audio fix works (`snd` 0.0%, 0 drops, 0 waits,
  ring 1-3 of 4, no underrun in the race), 59.6-59.9 emulated fps, 29-46 renders shown a second
  (~36 on average; log 20 ~31, log 24 ~12), p50 16.85-17.2 (fully loaded). Night `sh4` is 9.4-9.5
  against 7.7-8.3 by day, in both drawn and dropped frames (game code: the runner's night demo
  spends its instructions differently, partly in FPU code over 32-byte records at 0C0225A0, but in
  no single routine worth replacing). Built after it, **awaiting log 26**: ELAN PREFETCH (RENDERING 2,
  second row, default ON): the ELAN walk asks for the cache lines 256 bytes ahead of it in the lists
  outside models (a dropped race frame's walk reads ~1700 command headers on the runner, so ~0.7
  us each on a Wii: about one miss apiece; a drawn one ~3400); new
  `[prof]   ELAN walk:` line splits the walk between dropped and other frames (ms, commands read,
  ns a command). First numbers (2P log 1, both runs 2 PLAYERS on a day course, PREFETCH ON): a
  dropped frame's walk 0.77-0.98 ms, 1580-2100 headers, 460-510 ns each; a drawn one 2.3-2.7 ms,
  2960-3500 headers, 760-810 ns. Against the ~700 ns estimated before, dropped frames (models
  stepped over, so nearly all of the walk in lists outside models, where it prefetches) look ~30%
  faster per header and drawn ones not; no OFF run yet, so **still to A/B** (ELAN PREFETCH OFF).
- Log 24 (2026-11-05, night Akina uphill, two runs). Every frame
  drawn: as log 23 (48-54 fps, p50 17.7-20.1; COLOUR LISTS took Akina's ~24 CPU-fed colour vertices
  to 0, nothing wrong seen). FRAMESKIP AUTO: only 80-88% speed and ~12 renders shown a second,
  because the emulation sat ~25% of the time in the audio ring's pacing wait (`snd` bucket, tail
  frames 20-29 ms; 0 in logs 20/21 AUTO and in the every-frame run). With SOUND CLOCK REAL TIME the
  wait cannot pace anything and feeds itself (waited time becomes samples that refill the ring);
  stalls that empty the ring ratchet its level up until it is full. Fixed after it (log 25
  confirmed): with REAL TIME the audio push never waits (free-run, a full ring drops one block);
  AUDIO SYNC only paces the EMULATED clock now (hints updated).
- Log 23 (2026-11-05, two runs, every frame drawn, MIRROR OFF,
  SMALL 3D 4 px): night Akina: the light-set reuse works (10-64 sets built a frame, 206-410 reused),
  recording 3.65-4.5 -> 2.85-3.6 us a list with a box, ~0.5 ms off a drawn night frame (log 22,
  same settings). Day course (log 21's): in-car ~100%, chase 52-56 fps; there ~1.1-2.3K CPU-fed
  colour vertices a render cost ~0.3-0.65 ms. The ELAN command walk (2.0-2.8 ms a drawn frame) is
  now as big as the recording at night and twice it by day. Built after it: COLOUR LISTS (new
  page RENDERING 2, after RENDERING, default GPU FETCH): colour lists through the GPU fetch as RGB8
  read one byte into their ARGB word, when their alpha is 255 or not used (runner race: all of
  them, ~1.2-1.3K vertices a render). Log 25 measured it by day (see above).
- Log 22 (2026-11-05, night Akina, every frame drawn, MIRROR OFF, SMALL 3D 4 px): DISPLAY LISTS
  (RENDERING, last row, default ON) work: `cached programs` 0.75-1.4 -> 0.10-0.28 ms a render, no
  GPU wait, no ring pass; a render's CPU -0.5 to -0.6 ms at equal lists. At night the recording
  cost ~5-6 us a list (light sets), hence the light-set reuse confirmed by log 23.
- Log 21 (2026-11-05, AUTO, SMALL 3D 4 px, in-car view, MIRROR ON in the owner's options.txt):
  night Akina ~25 shown fps (log 20, same course with MIRROR OFF: ~31), a day course ~34 (S ~11.4,
  D ~20.5 ms). IN-CAR REFLECT skips 15 builds in 16 as designed but `sh4` did not move (<0.1 ms):
  the reflection map is cheap on a Wii, so turning reflections off entirely would gain nothing.
- Log 20 (2026-11-05, two runs, in-car view all race, MIRROR OFF):
  MIRROR OFF, SMALL 3D and SOUND LEVELS work (owner). Every frame drawn, SMALL 3D 8 px: 43.5-54.7
  fps, p50 18.8-21.2 ms (`sh4` 9.2-9.6, `rend` 1.55-2.2, `elan` 4.2-4.95, `arm` 1.45, `aica`
  1.1-1.25); at 8 px trees visibly clip (owner: acceptable). FRAMESKIP AUTO + 4 px: 100% speed,
  43-61% of frames drawn; owner: "a little bit better but not that much". Owner set the sound
  defaults: MENU SOUND 5, TIRE SOUND 4 (done). Built after it: IN-CAR REFLECT (RENDERING, default 1
  FRAME IN 16): the car reflection map is rebuilt 1 frame in 16 while the in-car view (its mirror
  models) is up, where only the opponent's car shows it.
- Log 19 (2026-11-05): PAGE GUARD exact and stable (owner
  committed it) but small: page checks ~0.15 -> 0.055 ms a render. Race 41-44 fps in its heavy
  stretch, 45-51 at ~80K vertices; the owner set SOUND CLOCK REAL TIME, which makes sound cost
  ~1.25x per frame below 100% speed (`arm` + `aica` 2.7-2.85 ms). Built after it, from the
  owner's requests: MIRROR (RENDERING, default OFF: the in-car view's rear-view
  mirror, ~31% of that view's drawn lists, becomes an empty dark panel), SMALL 3D (RENDERING, default
  DRAW ALL: 3D lists and whole models under 2/4/8 pixels not drawn; at 4 px -27% drawn lists in the
  runner's race, look unchecked: the runner's snapshots cannot show GPU lists), REFLECTIONS gains 1
  FRAME IN 8, and SOUND LEVELS: MENU SOUND and TIRE SOUND (AUDIO, last rows, 0-10, default 5).
  Option rows now hold up to 11 values.
- Log 17 (2026-11-04, race Akina uphill, no diagnostics, MEM SWEEP
  SHORT, GX FIFO SIZE 512 KB): at ~80K vertices 48.8-54.7 fps, p50 19.2-19.6 ms (`sh4` 9.3, `rend`
  2.2-2.35, `elan` 4.35-4.45, sound 2.2), p99 22-23; heavy start of the race 43-45 fps. Night demo
  56.4-60.9 fps. GX FIFO never full; MEM SWEEP SHORT confirmed (36 reclaims, 0 failures, race
  hitches 261 -> 158 ms). Log 18 (same day): SOFT FLOAT (the game's software double
  divide/multiply run natively) took `sh4` 9.3-9.5 -> 8.9-9.1 ms in the race, night demo unchanged
  (control); its race drew more, so its fps is not comparable. The RECOMPILER page was full: JIT
  COLD ENTRY, MEM SWEEP and SOFT FLOAT moved to a new page RECOMPILER 2 (labels and
  `CoreOptionId` order unchanged, so saved choices and `autolaunch.txt` still apply); new
  recompiler rows go there. Owner: with SOFT FLOAT the car drives normally. Built after it,
  **awaiting log 19**: PAGE GUARD (RECOMPILER 2, last row): ELAN pages left unwritten 16 renders
  are mapped read-only, so the GPU fetch no longer checks their PTE every render; new [HW] line
  "ELAN page checks a render". REFLECTIONS default 1 in 4 is the owner's choice for the
  Wii (a Wii U could keep every frame, to decide later).
  Open bug: owner says the enemy car's lights display wrong (to look at in a later session).
- 3D LIST CULL (RENDERING page, last row, default VIEW + MODELS; 2026-10-30): each GPU fetch program keeps its list's model-space bounding box; when the ELAN
  records a list, the box under the list's matrix drops lists behind the camera or out of the
  picture and gives the depth range without measuring. Windows race: drawn per render 116.6K
  vertices / 965 lists -> 63.5-78K / 546-705, depth measured on ~0.6-0.9K vertices a frame
  instead of ~3.7K; night demo: point-lit lists take their lights from the box (no vertex pass).
  Needs 3D VTX FETCH on (it is where the boxes live). VIEW + MODELS also steps over whole
  models. Owner's first test: Dolphin OUT OF VIEW dropped to ~10% once the player had control;
  with the pin fix (out-of-view lists that reach the near plane hold the pin, unpinned frames
  measure only the nearest lists) the owner confirms it fixed.
- Music tempo follows emulation speed with SOUND CLOCK EMULATED (the game streams it through the
  SH-4), so it is right only at 100%. REAL TIME sounds right to the owner: it becomes the default
  once the speed tests are over (owner, 2026-11-01). Do not ask about its cost.
- Attract mode (no coin, ~20 s after the title) runs a **night** demo race: 6-11 point lights
  per list. Its crash on the Wii is fixed (CPU T&L fixed buffers; owner: "demo works now",
  2026-10-29) but it ran at a few fps, every list lit on the CPU. NIGHT LIGHTS = GPU (APPROX)
  (RENDERING page, default) now lights those lists with GX local lights: the whole demo goes
  to the GPU path on Windows (`--gpu-tnl`). Awaiting its speed and look on a Wii (log 12).
- Trees flickered in every race (owner, 2026-10-29): trees and guardrails are two-sided cards
  the arcade back-face culls; drawn both ways, the faces z-fight in GX's 24-bit Z. New option
  BACKFACE CULL (RENDERING page, last row, default ON = gxRend ISP_CULL 2). Windows emulation
  of the Wii's Z buffer: Z-rounding-sensitive race pixels 1.7-5K -> 32-98 a frame. Untested on
  a Wii (if the road or cars vanish, the winding is wrong: ON, OTHER SIDE).

## Hard rules

1. **No game data, ever**, in commits, issues or logs pasted publicly: CHD, PIC, BIOS, RAM/
   VRAM/ELAN dumps, snapshots, extracted assets, recordings. `.gitignore` blocks most of it;
   still check `git status` before any commit.
2. **Do not commit or push unless the owner asks.** Do not amend their commits.
3. **Do not scan drive roots** (`C:\`, `F:\`, Program Files...) to find a tool or file. Look in
   `AGENTS.local.md`, then ask.
4. Ask before opening Dolphin windows if the owner is around.
5. Licensing: the Wii binary is **GPL-2** (the ARM7 core in `port/emu/plugs/vbaARM` is GPL v2+).
   `port/emu/LICENSE` (MIT) covers the nullDC4Wii-derived code. Keep per-file headers; list new
   third-party code in `port/LICENSE.md`. Flycast (GPL-2) and MAME are references; never paste
   code from incompatible licences. Do not bring back the removed upstream "Native SH-4
   Recompilation" files or any Initial D artwork (old commits still contain them).
6. Game data is **little-endian**, the Wii is **big-endian**: read game bytes through
   `port/src/core/bytes.h`.

## Layout and key files

See the source map in [port/README.md](port/README.md). The files that matter most for the
current work:

| File | Role |
| --- | --- |
| `port/src/game/core_options.h` | Launcher option table (9 pages, reorganised 2026-11-11: GENERAL (LAUNCH GAME row first, then 2 PLAYERS, MIRROR, FRAMESKIPPING, SMALL 3D, IN-CAR REFLECT, pads, TEST MODE), RENDERING (DROPPED 3D), RENDERING 2 (textures, 3D T&L, effects), AUDIO, CORE (SH-4 CPU choice, idle skip, guest memory), RECOMPILER, RECOMPILER 2, FIXES, LOGS (PROFILER, JIT CCALLS, JIT HOTBLOCKS, AUDIO STATS, PC SAMPLER); a page that fills up gets a second one, ~20 rows fit). A page's row order is `kCoreOptionOrder` (the table stays in `CoreOptionId` order for autolaunch); launcher pages: 1/2 or -/+ |
| `port/src/game/game_runner.cpp` | Runs the core on the Wii: options -> `g_*_preset` globals (array in `CoreOptionId` order, static_assert), pads -> JVS, `[prof]` blocks, async log |
| `port/src/ui/launcher.cpp` | Menus; `options.txt` saved as `LABEL=VALUE`, matched by label |
| `port/src/core/log.cpp` | Log file, async ring mode (`log::setAsync`) |
| `port/src/core/dimm_image.cpp` | Decrypted reads of the game image from the CHD: run cache, stream read-ahead (Wii: thread under the emulation's priority, given 1 ms slices at vblank by `naomi_CartSlice`) |
| `port/src/run/run_main.cpp` | Windows headless runner `idas3wii_run` (all diagnostics flags) |
| `port/emu/naomi/naomi_frontend.cpp` | Core init/run/settings, idle-loop skip (`naomi_IdleCycleScale`), assert/BIOS-entry traps |
| `port/emu/naomi/naomi_board.cpp` | DIMM board, cart DMA, register counters |
| `port/emu/naomi/naomi_softfloat.cpp` | SOFT FLOAT: the game's software double divide/multiply (GCC soft-float, SH-4 integer code) run natively, fingerprinted; called by the JIT at the routines' entry |
| `port/emu/dc/sh4/rec_v2/decoder.cpp` | SH-4 -> SHIL decoder shared by the Wii recompiler and the runner's SHIL executor; JIT FPU BANKS (`dec_SwapBanks`, `dec_FpuPhysical`) |
| `port/emu/host/shil_exec.cpp` | Windows runner's SHIL executor (`--shil-exec`): blocks from the decoder run op by op in C, the canonical bodies through `ngen_CC_*` |
| `port/emu/naomi/naomi_copies.cpp` | GAME COPIES: the game's word copy (0C20A280 and its loop's own entry 0C20A29E, into the store queues in races) and display-list copy (0C1D4EA0 + 0C1D4F20, into ELAN RAM) done in C, same memory/registers/T/cycles, fingerprinted; INT DIVIDE: its FPU integer divides (0C2223B8, 0C2223E0); called by the JIT at the routines' entry (table-driven hook in `wii_driver.cpp`) |
| `port/emu/naomi/naomi_motion.cpp` | MOTION FX: the race camera's motion a vblank (followed car's step and turn, in-car view, the scene's focal length) for gxRend's `mfx_*` effects |
| `port/emu/naomi/naomi_2p.cpp` | 2 PLAYERS: whose turn the camera is (rig target, changed at every STARTRENDER), each view's aspect, the CPU car driven by the second pad, the HUD speed of its view; `naomi_2pHalf()` tells the renderers which half a render goes to, `naomi_2pSkip()` pairs their frameskip |
| `port/emu/naomi/naomi_elan*.cpp` | ELAN: `_cmd` command walker (chained lists, model skip, user clip), `_tnl` CPU T&L + depth measure/pin + GPU light sets (point lights as GX local lights), `_gx` GPU T&L path, `_fetch.inc` GPU vertex-fetch programs (+ each list's bounding box, `cachedBox`) and their GX display-list entries (DISPLAY LISTS: `dlEntry`, its own ring, GPU waits before writing over a queued entry) |
| `port/emu/plugs/drkPvr/gxRend.cpp` | PowerVR -> GX renderer (TA lists, ELAN items, VRAM present, tile clip, buffer caps) |
| `port/emu/plugs/drkPvr/frame_prof.cpp` | FPROF per-frame profiler |
| `port/emu/host/soft_renderer.cpp` | Windows software renderer for the runner (snapshots, stand-ins for the `rend_*` hooks) |
| `port/emu/wii/wii_fastmem.cpp` | Guest memory: MMU demand paging, zero page, compressed page store, GPU-fetch page validation |
| `port/emu/wii/dc/sh4/rec_v2/wii_driver.cpp` | SH-4 -> PPC dynarec back-end and fastmem trampolines (see `WII_DRIVER_MANUAL.md` next to it) |
| `port/emu/plugs/nullAICA/sgc_if.cpp` | AICA channels; SOUND LEVELS: a key-on's sample fingerprint (`sfx_Fingerprint`) picks extra attenuation for the menu cursor and the tire squeal (`kSfxMenu`, `kSfxTire`) |
| `port/emu/host/presets_generated.cpp` | Generated by `port/emu/tools/gen_presets.py` from nullDC4Wii's `wii/main.cpp`: nullDC4Wii defaults of every preset (its header mentions a `presets_overrides.cpp` that does not exist) |

## Building

Paths below assume the default devkitPro and VS install locations; see `AGENTS.local.md`.

**Wii** (`make` writes the SD layout `port/wii/dist/`, the only `boot.dol` being
`dist/apps/initiald3wii/boot.dol`; `boot.elf` and its map stay in `port/wii/build/`):

- `port\build_wii.bat` (runs `make` inside devkitPro's MSYS2; extra args go to make, e.g.
  `build_wii.bat clean`).
- Header changes rebuild only through `build/*.d`, which hold absolute paths: after moving the
  repo, run `make clean` once (stale paths made a header change rebuild nothing, so the
  launcher kept an old option table).
- From Git Bash, devkitPro's make does not see `DEVKITPRO`/`DEVKITPPC`; go through its own shell:
  `MSYS_NO_PATHCONV=1 /c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /<drive>/<path>/port/wii && make'`
- Verify hand-written PPC encodings in the dynarec with `powerpc-eabi-objdump`.

**Windows** (output in `build-port/`: `idas3wii.exe`, `idas3wii_run.exe`, `idas3wii_tests.exe`):

- `port\build_windows.bat` (vcvars64 + VS's CMake/Ninja, configure, build, ctest).
- `C:\devkitPro\msys2\usr\bin` is on PATH and **shadows `cmd` and `cmake`**: from PowerShell call
  `$env:SystemRoot\System32\cmd.exe` by full path; the MSYS cmake cannot drive MSVC (the bat
  prepends VS's `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` and `...\Ninja`).
- vcvars64 prints a harmless French "'vswhere.exe' n'est pas reconnu..." (Windows locale is
  French, so tool messages are too).
- CMake globs sources: re-run configure after adding a file (the bat always does).
- Core-only rebuild: `cmake --build build-port --target idas3wii_run`.
- `.vscode/settings.json` (git-ignored) holds an absolute `cmake.sourceDirectory`: fix it
  after moving the folder.

Build **both** after touching shared core code: Windows catches logic errors and runs the
checks below, the Wii build compiles the `_WII` paths (GX, dynarec, fastmem).

## Verifying changes

**Unit tests:** `ctest --test-dir build-port --output-on-failure` (synthetic CHD, no game data).

**Windows runner** (needs the owner's CHD; BIOS zip found next to it or `--bios`):

```
idas3wii_run --chd <gds-0033.chd> --frames N --cpi 2 [options]
```

| Option | Use |
| --- | --- |
| `--cpi 2` | SH-4 cycles-per-instruction override used for all reference timelines |
| `--snap-dir D --snap-every N` | software-rendered PNG every N frames |
| `--press F:name,F:name,...` | press a button at frame F, e.g. `5000:coin,5300:start` (coin, start, view, up, down, left, right, gas, brake, test, service; the second pad: gas2, brake2, left2, right2, up2, down2) |
| `--two-player 1` | 2 PLAYERS: split screen in races, snapshots `split_F.png` (the views of frames F and F+1 side by side). Driving from the start: start every 300 frames up to 9300, then `gas` (and `gas2`) every 10 frames from 9400 (the race starts ~9500, "GO!") |
| `--gpu-tnl` | take the lists the Wii GPU path would take; runs the **GPU fetch check** (`[run] GPU fetch check`: program triangles vs the CPU draw; must stay at 0 mismatches). Known exception, unexplained, in every driving-route run since at least 2026-11-05: 15 lists at two menu renders (frames ~5640, ELAN +17F1xxx-17F5xxx quads, and ~6720, +1750xxx-175Bxxx), cached programs whose records changed with no page write seen |
| `--frameskip N`, `--model-skip 0/1`, `--fixes on/off` | same as the launcher options, for A/B runs |
| `--headlights 0/1/2` | HEADLIGHTS (default 0 here and in the launcher): 1 SOFT (wider cone, CPU path too: shows in snapshots), 2 SMOOTH (with `--gpu-tnl --point-lights 1`): `HEADLIGHTS SMOOTH per drawn render` line, every beam's projected cone (texture through its matrix) against the ELAN's cone at each vertex in front of the lamp: max error must stay ~0.02, 0 over 0.1. the mask check (lists reaching behind the lamp) likewise. `HEADLIGHTS per frame` line: beams, of which masked, headlights left out of lists their cone misses |
| `--point-lights 0/1` | NIGHT LIGHTS: 1 = point lights as GX local lights; without `--gpu-tnl` the CPU draws those lists lit the way GX would (snapshot preview). With `--gpu-tnl`: `point-lit light sets per frame` line, every reused set rebuilt and compared (`STALE` must stay 0, parallel-lit sets kept by key included); `lights per frame` line (light commands and repeats skipped, picks, lights tested and those the quick test settled: `WRONG` must stay 0); the `GPU fetch per drawn render` line adds light sets loaded at draw and compared when recorded, sets found by key checked against their contents (`STALE` must stay 0). The night attract demo (no presses, 9000 frames) is not deterministic on Windows past render ~5300: compare `record digest` lines only up to where two runs of the same code agree |
| `--cull 0/1/2` | BACKFACE CULL in the snapshots: 2 = the polygons' cull modes (launcher ON), 1 = the other winding |
| `--list-cull 0/1/2/3` | 3D LIST CULL with `--gpu-tnl`: 1 = behind the camera, 2 = out of view, 3 = also whole models (launcher default). Prints `3D list cull per frame` and `3D list boxes ... N STALE` (every box used is checked against its list's records, every model stepped over against its block: must stay at 0) and `record digest` (all that the renderer is handed: for a refactor of the recording side, two runs at the same value must match line for line; run them one at a time, four at once lost one) |
| env `IDAS3_GX_DEPTH=N:F:rules;...` | each snapshot redrawn with the Wii's 24-bit Z (near N, far F; N 0 = exact) and other depth rules, `_diffK.png` + winning lists of the differing pixels; a config against itself with `j0.5` = pixels that flicker (rules in `soft_renderer.cpp`, `GxDepth`). Race: `0:1:geq+cull;0.00999:100100:geq+cull;0.00999:100100:geq+cull+j0.5+r1` |
| `--histo-from F` | PC histogram from frame F, top 64 ranges of 64 bytes (best tool when stuck without a hang); the wait-for-frame loop 0C20C840 + 0C211880 is ~half of it here (the runner has no idle skip). Env `IDAS3_HISTO_FILE=path` writes every bucket, to compare two parts of the game. Not with `--work-from` in the same run: both use the one SH-4 step hook and the histogram comes out empty |
| `--arm-histo-from F` | the ARM7 sound CPU's ticks by 64-byte range of its code from frame F (the Windows interpreter, which has no idle skip), top 40 at exit, plus ticks a frame |
| `--disasm A:B[,C:D]` | SH-4 code of those hex ranges at the end of the run (opcode + format string; run past ~frame 4600 for the game's code) |
| `--work-from F[:T]` | a `[work]` line per frame from F: SH-4 instructions outside the wait loop (what a Wii frame really has to run), the reflection routine, the copy loop, CHD hunks decoded. With T, frames over T instructions and the others get a PC histogram each, heaviest difference first |
| `--trace-from PC`, `--trace-at F`, `--trace-pc a,b`, `--watch ADDR`, `--path-after-elan` | SH-4 tracing (`--trace-at`: arm at frame F; 40 hits per address) |
| `--dimm-ahead 0/1` | DIMM AHEAD (default 0 here, the launcher's is on): run cache + stream read-ahead; on Windows the decode is done on the spot |
| `--reflection N` | REFLECTIONS: one reflection map build in N (default every one, the launcher's is 4) |
| `--soft-float 0/1/2` | SOFT FLOAT (default 0 here, the launcher's is NATIVE): 1 = the game's double divide/multiply run natively, 2 = the game's code runs and every result is checked against the native one (`SOFT FLOAT ... MISMATCHES` must stay 0). `--soft-float-test N`: N random pairs against the host's doubles, no CHD needed |
| `--game-copies 0/1/2` | GAME COPIES (default 0 here, the launcher's is NATIVE): 1 = the game's copy routines done natively, their cycles charged to the interpreter (`g_sh4_hook_cycles`); 2 = the game's code runs and every call the native copy would take is checked at its return: registers, T, memory, store queues, QACR, the pushed r14, the list's word 15 (`GAME COPIES ... MISMATCHES` must stay 0). Exit lines: calls a frame, native, words a frame, and a census (words still through the game's word loop, direct entries into it by caller, calls turned down by area) |
| `--shil-exec 1`, `--jit-fpu-banks 0/1` | the SH-4 run through the recompiler's decoder and SHIL ops executed in C (`emu/host/shil_exec.cpp`) instead of the interpreter: checks decoder changes on Windows. Compare two runs (a decoder preset off/on) by `record digest`; its timing is not the interpreter's (the race route's presses miss the race: use the night attract demo, no presses, to frame ~12000). Exit line: JIT FPU BANKS stretches decoded. Not the Wii back end (no pinned registers, fastmem, PPC code) |
| `--int-divide 0/1` | INT DIVIDE (default 0 here, the launcher's is NATIVE): the game's FPU integer divides (0C2223B8 signed, 0C2223E0 unsigned) done natively, or checked with `--game-copies 2` (FPUL, scratch registers, T, the pushed words, FPSCR: `MISMATCHES` must stay 0) |
| env `IDAS3_SF_PROBE=LO:HI[:FROM]` | names the routines called into that code range by trying native double operations on their r4-r7 and r0/r1 (how the soft-float routines were found) |
| `--depth-plane 0/1` | DEPTH MEASURE: 1 = RACE PLANE, frames with no pin keep the race's near plane |
| env `IDAS3_NEAR_TRACE=1` | a `[near]` line per render: pin, frame near plane, vertices measured and why |
| env `IDAS3_CART_TRACE=1` | a `[cart]` line per read of the game image (offset, length) |
| `--incar-reflect N` | IN-CAR REFLECT (default 0 here = as REFLECTIONS, the launcher's is 16): while the in-car view's mirror models are on screen, one reflection map build in N, -1 = none; an `IN-CAR REFLECT per 60 frames` line and an exit line count the builds not made. The race starts in the in-car view; `view` toggles it with the chase view |
| `--colour-fetch 0/1` | COLOUR LISTS with `--gpu-tnl` (default 0 here, the launcher's is GPU FETCH): colour lists get fetch programs (so boxes) and are drawn from them when TSP UseAlpha is 0 or every record's alpha is 255 (the Wii also takes gxRend's other forced-opaque cases); the `GPU fetch per drawn render` line shows "colour lists N (M more fetched)" and they join the fetch, display-list and box checks |
| `--display-lists 0/1` | DISPLAY LISTS with `--gpu-tnl` (default 0 here, the launcher's is ON): every cached list's display-list entry is built as on the Wii, its bytes read back as the GPU would and compared with the CPU draw. `DISPLAY LISTS per drawn render` line: MISMATCHES (only where the GPU fetch check has them too) and HAZARDS (an entry written over one still queued) must stay 0; also the ring bytes the drawn entries take, ring passes and GPU waits. Env `IDAS3_DL_RING_KB`: ring size (the Wii's is 1024) |
| `--mirror 0/1/2` | MIRROR: 0 = models clipped inside the user clip rectangle (the in-car view's rear-view mirror) not drawn; `MIRROR OFF per frame` line. 2 = OFF + GAME SKIP: the game's own mirror view is not drawn (0C024100 returns for the view whose word +138 is 2); exit line `MIRROR OFF + GAME SKIP: N mirror views not drawn`. The race starts in the in-car view (~9350) and each `view` press toggles in-car/chase: with presses at 9950, 10250, 10550 the route is in-car at 9350-9950 and 10300-10550 |
| `--sound-mix 0/1` | SOUND MIX: 1 = HALF (odd samples step the voices without mixing, the last sample is played again). Exit line `sound output: N samples, hash of all, of the even ones`: HALF must give FULL's even-sample hash exactly (and the same record digests) |
| `--model-prefetch 0/1` | MODEL PREFETCH: a cache hint only (no effect on the PC's output or digests) |
| `--list-maths 0/1`, `--walk-decode 0/1` | LIST MATHS, 3D DECODE: record digests must match the run without them (with HEADLIGHTS ARCADE; SOFT/SMOOTH may differ in borderline cone tests), `lights per frame` WRONG must stay 0 |
| `--draw-memo 0/1` | DRAW MEMO with `--gpu-tnl`: `DRAW MEMO per drawn render` line, every program taken from the recorded slot checked against the full lookup (WRONG must stay 0) |
| `--sound-step 0/1` | SOUND STEP: the exit line's sound hash must be the one without it |
| `--light-sum 0/1` | LIGHT SUM (default 1, as the launcher): point-lit lists whose GX light sum could pass 1.9 lit at 1/2 or 1/4; the `--point-lights 1` preview applies it (snapshots must not change against `0`), `LIGHT SUM per frame` line; with env `IDAS3_HEADLIGHT_CENSUS=1` the `[gxsum]` lines give the sums GX's channel holds (must stay under 2 with `1`) by headlights on the vertex |
| `--sh4-slice 0/1` | SH-4 TIMESLICE: 1 = 3584 cycles (the interpreter takes the same preset): changes timing, so digests and the sound hash change; the route must still reach and drive the race |
| `--cache-hints 0/1` | CACHE HINTS: prefetches are no-ops under MSVC and `dcbz` is Wii-only, so on Windows this checks the bookkeeping (the fetch slot kept per model head): record digests and the sound hash must be the ones without it |
| `--small N` | SMALL 3D with `--gpu-tnl`: lists and whole models under N pixels on screen not drawn; `SMALL 3D per frame` line. Snapshots cannot show it (GPU lists are not drawn in them) |
| env `IDAS3_FAR_CENSUS=1` | drawn GPU-path vertices and lists by their box's nearest depth and by its size on screen |
| env `IDAS3_TEXCACHE=N` | texture cache census (`host/texcache_census.cpp`): gxRend's TEXTURE CACHE rules (NORMAL, VERY FAST+ (OLD) slots, VERY FAST+ arena + bind memo, each with and without VQ AS CMPR) replayed on every textured header of a drawn render; a `[texc]` line per policy every N drawn renders (binds, lookups, decodes, KB and texels decoded, binds showing another decode's pixels, decodes over a texture bound earlier in the render, arena wraps), totals and the textures by kind at exit, and both CMPR encoders (`cmpr_encode.h`) checked on the game's VQ 565 and plain RGB565 textures as GX decodes them. Use with `--gpu-tnl` (the ELAN lists' binds) |
| env `IDAS3_AICA_KEYS=FROM:TO` | a `[keyon]` line per sound channel key-on in those frames: format, loop, address, loop points, pitch, levels and the sample fingerprint SOUND LEVELS matches. The exit line `SOUND LEVELS` counts the key-ons recognised |
| env `IDAS3_SNAP_FROM=F` | periodic snapshots only from frame F (with `--snap-every`) |
| `--area1-census` | who writes the 32-bit VRAM interface |
| env `IDAS3_ACCESS=LO:HI[:FROM[:TO]][;...]` | every SH-4 read and write into those guest ranges in those frames, by instruction address and caller (pr), with first/last values, printed at exit: which code uses a game variable (how 2 PLAYERS' addresses were found; the run is ~2x slower) |
| env `IDAS3_TIMELINE=FROM:TO:ADDR[-END][,...]` | every SH-4 access to those guest words or ranges (up to 8) in those frames printed as it happens (value, pc, pr, ELAN executions so far), with the vblanks and the renders (half of a 2 PLAYERS render) between them: the order of events in the game's frame (how the 2 PLAYERS render latency was found) |
| `--p2-car 0/1`, `--p2-scenery 0/1` | P2 CAR (0 ON RAILS, 1 FULL PHYSICS) and P2 SCENERY with `--two-player 1` (the launcher's defaults: 1) |
| env `IDAS3_PHYSW=FROM:TO` | every word of the player's physics block (0C9008A4-0C9017D3) written in those frames inside the physics (0C157880 to the return at 0C159944) or outside it (`[physw]`), and every word the physics writes outside the block (`[physo]`, stack excepted): what player 2's FULL PHYSICS run must have its own copy of |
| env `IDAS3_POSREAD=FROM:TO` | from the vblank to the drivers update (0C159920), every 32-bit read whose value is player 1's x position: where the frame takes player 1's position from (`[posread]`) |
| env `IDAS3_MODELS=FROM:TO[,...]` | the ELAN models (offsets) each of those frames calls and the lists it links, with the 2 PLAYERS half of the scene it builds (`[models]`, `[links]`) |
| env `IDAS3_WRITEVAL=FROM:TO:VALUE` | every 32-bit write of VALUE in those frames, address, pc and pr (`[writeval]`) |
| env `IDAS3_2P_HOLD=FROM-TO:CAR` | 2 PLAYERS keeps the camera on one car (0 player, 1 CPU) in those frames |
| env `IDAS3_STRIP_ORDER=FROM:TO` | each render in those frames as runs of polygon headers in submission order (list type; E a GPU-drawn ELAN list, P a TA polygon, S a TA sprite) with vertices, screen box, 1/W range; with snapshots, each run also drawn alone (`frame_F_runNN_L?.png`) with its nearest pixel. How the HUD's place and depth were found (CPU path: ELAN lists come as P runs) |
| env `IDAS3_MOTION=FROM:TO` | a `[motion]` line a vblank: race view, in-car view, both cars' metres and yaw a frame, focal length, and the vertices drawn under each ELAN projection |
| env `IDAS3_HEADLIGHT_CENSUS=1` | CPU path (no `--gpu-tnl`): two `[headlight]` lines per 60 renders: lists the cars' headlights reach, lit vertices and strip edges, hard edges (light changing > 0.25 along one) by the angle they span seen from the lamp and by distance, cone edges (the cone term alone changing > 0.5: the polygon seen in the beam) and the lists holding them by type, lit vertices over 1. The night attract demo (no presses, 9000 frames) has headlights from ~5000 |
| env `IDAS3_POKE=FROM-TO:ADDR=VALUE[;...]` | a guest word written at every vblank in those frames: try a game variable's effect in the snapshots |
| env `IDAS3_DUMP_AT=F1,F2,...` (+ `IDAS3_DUMP_DIR`) | guest RAM dumps `ram_F.bin` at those frames (32 MB each; never commit them) |
| `--dump-ram/-vram/-aica/-elan FILE` | memory dumps at the end (never commit them) |
| env `IDAS3_RENDER_TRACE=FROM:TO` | print each render's FB registers |
| env `IDAS3_FETCH_SCATTER=%` | % of pages put in random frames for the fetch check (default 33) |

The runner uses the **compiled-in preset defaults**, not the launcher's (e.g. CPU T&L unless
`--gpu-tnl`). Reference timeline at `--cpi 2`: first renders ~frame 1000, WARNING ~2400, SEGA
ROSSO ~3500, title ~4100, attract manufacturer screen ~13000. Race route: coin at 5000, then
start every 300 frames up to 11000; the race starts at ~9350 in the in-car view and each `view`
press toggles in-car/chase (presses at 9950, 10250, 10550: chase from ~10000, in-car from ~10300,
chase from ~10600; the chase view has no clip-mode-2 models). That route stops before the player
gets control (the scene freezes from ~11700): to drive, keep pressing start up to 13100 and `gas` every 10 frames from 11500 (a
press lasts 10 frames); the car then races in the in-car view (no steering: it ends up against
the guardrail), and `view` presses during it (e.g. 12000, 12500) cycle the cameras.
That route's race is **Myogi by day** (Legend of the Street, race 1 against IGGY, counter-clockwise,
day, dry), not the owner's night Akina. The course select screen shows ~7440-7520 (strip
Shomaru, Tsuchisaka, Myogi (default), Usui, Akagi...), then the opponent screen (~7570-7700: IGGY
race 1, KENJI race 2 clockwise day...). `left`/`right` move those cursors, but the window is short
and shifts by tens of frames between runs (a press at 7480 was lost in the strip's scroll-in, ones
at 7530+ moved the opponent cursor): a night race on the runner is not reached yet. Menu snapshots
need the CPU path (no `--gpu-tnl`: GPU-path lists are not drawn in snapshots, menus come out blank).

**Real Wii (the reference):** the owner runs the build with SPEED OVERLAY + PROFILER and sends
`idas3wii_log.txt` (numbered "REAL WII LOG n"). Hand them a short list of what to test and which
log lines to read. Do not trust Dolphin for speed: it models no cache misses (on hardware the
SH-4 side costs ~2.3x and ELAN command walking ~3x the Dolphin time), ignores PPC page-table
read-only (PP) bits, needs its MMU option on, and the owner's Dolphin is overclocked.

Reading the log: `FPROF` per-frame buckets (`sh4`, `rend`, `elan`, `arm`, `aica`, `tex`, `mem`;
on NAOMI 2 the `elan` column is the old disc bucket and `rd=` counts ELAN command executions);
`[prof]` 120-frame blocks; `[HW]` Broadway PMC lines (IPC, L1 miss cycles, DTLB, L2, render
probe, ELAN draw split, fetch programs/patches/ring passes, ELAN page checks: count, time, PAGE
GUARD's read-only pages; since log 30's build the rest of the frame is split into the ELAN's own
command work and the game + sound + others); `[PCS]` PC SAMPLER (where the cycles go, by C span
and SH-4 block; see log 31's entry); `[CC]` JIT call-outs into C
(JIT CCALLS); `[HOT]` hottest blocks (JIT HOTBLOCKS, ~12% cost); `[mem]` guest page pool;
`[2P]` split-screen passes.

## How to work here

- **Every risky change goes behind a launcher option** with the old behaviour kept as a value,
  so the owner can A/B on hardware: add a row in `core_options.h` (enum + table), the preset
  pointer in `game_runner.cpp` (same order), the global in the core, and a runner flag if
  useful. Options are saved by label: renaming a label drops the saved choice; moving rows
  changes the owner's button habits, so say so.
- Diagnostics stay off and cheap unless PROFILER / JIT CCALLS / HOTBLOCKS / AUDIO STATS are on.
- Before shipping a Wii change to the owner, prove it on Windows where possible (census, fetch
  check, snapshots before/after).
- Match the surrounding style: the core keeps nullDC4Wii's style, the launcher is C++17 with a
  stricter warning set. Comments explain why, briefly.
- Write edit scripts with the file-writing tool, **not bash heredocs**: here they turned `\\n`
  into a real newline inside generated C strings.
- Reference sources on the owner's PC (nullDC4Wii, Flycast for NAOMI 2/ELAN, MAME): see
  `AGENTS.local.md`. nullDC4Wii's own CLAUDE.md forbids building it or committing there without
  approval.

## NAOMI 2 / Wii lessons (check these first when something regresses)

- If the game reads the main-board TEST button as pressed (MIE maple 0x86 sub 0x31, reply
  byte in(5) bit 4, active low) it enters the BIOS SYSTEM MENU, which looks like progress.
  in(5)=0xF9 is the safe value. On the Wii, TEST is D-pad LEFT (GC pad, Classic, Wiimote with a
  Nunchuk) and one tap is enough. In a log it reads like a crash: `[elan] write unhandled
  00800018`, `CCN: I-cache disabled (pc=AC0193D6)`, `[naomi] write BOARDID`, no more cart DMA, then
  ~40 s later `005F7050` writes and a text screen. Check for `[game] TEST button pressed` first.
- ELAN reg 0x74: bit0 = texture DMA done, bit1 = command done. Missing bit0 = hang after
  WARNING.
- ELAN display lists are **chained** (a buffer's last command links to the next): follow a tail
  link as a jump (`kMaxChain` in `naomi_elan_cmd.cpp`), never a depth limit.
- N2 MODEL PCW bits 16-17 = user tile clip mode (the rear-view mirror uses mode 2 with a TA user
  clip rect): `rend_SetTileClipMode`.
- Per frame the TA sees SoftReset, ListInit, lists, SoftReset, STARTRENDER: a soft reset must not
  discard captured lists.
- Cart DMA offsets carry flag bits (0x2080xxxx / 0x4000xxxx): mask to the DIMM window.
- Red/magenta border + hang = the game's own assert handler; `naomi_ReportAssert()` logs the
  call site. A jump into BIOS ROM is trapped by `naomi_ReportBiosEntry()`.
- Dolphin ignores PP bits: anything relying on a write fault to a read-only page writes through
  (the RO zero page, PAGE GUARD).
  `wii_fastmem.cpp` probes this at init (`zero_page_protected`, `change_bit_tracked`) and falls
  back; keep the probes when adding such tricks.
- A VI flip (`VIDEO_SetNextFramebuffer` + `VIDEO_Flush`) only reaches the screen at the next real
  retrace (libogc's handler writes the registers, then `currentFb = nextFb`), and the emulation is not
  paced to that retrace. Never copy into an XFB that `VIDEO_GetCurrentFramebuffer()` or
  `VIDEO_GetNextFramebuffer()` returns: it is, or is about to be, under the scan (tearing; SCREEN
  BUFFERS). Faster emulation makes such a race show more, so a speed row can look like it causes it.
- GX lighting: a light's colour stops at 1, so a stronger light carries the excess in its attenuation
  (`gxPointLight`); but the Wii's per-channel light sum seems to hold only up to ~2 and wrap past it
  (black where both cars' beams meet, 2026-11-14; LIGHT SUM, awaiting confirmation). Keep a channel's
  possible sum under ~1.9 and multiply back in the TEV. Dolphin clamps at 255: it cannot show this.
- After installing PPC exception handlers put a `sync` + memory clobber: GCC moved the inline
  store past a volatile guest read once.
- The dynarec pins SH-4 GPRs in r14..r28: `Sh4cntx.r[]` read from C (e.g. in a trap) can be stale.
- Wii memory: newlib malloc never returns memory, and the guest page pool only takes what is
  above the heap's high-water mark. Never allocate big temporary buffers in the launcher; stream
  files (the BIOS zip is streamed, only 512 KB kept).
- A diagnostic that takes heap can starve a lazy allocation made later: in log 31 JIT HOTBLOCKS'
  448 KB table (malloc'd at the first compile) left no room for the DISPLAY LISTS ring (allocated at
  the first race), and `rend` grew ~0.6-1 ms. Size such tables into `_vmem.cpp`'s heap headroom
  (`hotblocks_heap_bytes()`, `frame_prof_heap_bytes()`), and read `[elan] GX display lists:` first.
- `sh4_registers.h` defines `r`, `fr`, `xf`, `pr`, `sr`, `fpul`, `fpscr` ... as macros for the SH-4's
  registers: never name a local or a parameter that in a file that includes it (decoder.cpp, the
  runner, naomi.h's users); the errors are confusing (`'->' attendu`, `'Sh4cntx': objet const`).
- An asynchronous exception handler (the PC SAMPLER's) must not use the interrupted r1, r2 or r13:
  libogc's idle thread (`KThreadIdleMain`, `b .`) runs with all three 0, and libogc's own IRQ entry
  switches to `__ppc_excpt_sp`. A synchronous one (the fastmem DSI handler) can, because its faults
  only come from code with a real stack. A fault inside a handler (MSR[RI] = 0) goes to libogc's panic
  path, which prints nothing on screen and loops: it looks like a freeze with a cut log.
- Nothing in the core may grow on the heap during the game: a few hundred KB are left beside
  the page pool, and with `-fno-exceptions` a failed `new` is `abort()`. A path that a day race
  never takes can still be hit later: the CPU T&L vectors stayed empty in races (the GPU path
  takes every list) and grew to ~1.5 MB in the night attract demo. Use fixed buffers.
- The GX renderer's buffers were sized for Dreamcast: ELAN output is chunked (<=1024 vertices a
  header) and the NAOMI 2 caps raised (`GX_VERTEX_CAP`, `GX_LIST_CAP`).
- Sound with SOUND CLOCK REAL TIME is produced from real elapsed time (`armUpdateARM`, stalls
  over 50 ms not caught up), so nothing may block on the audio ring in that mode: a block there
  turns into more samples (log 24: 25% of the time in `snd`, AUTO at 80% speed). `snd` in FPROF is
  that wait (`SndWaitTicks`); AUDIO STATS ON logs the ring's underruns, drops and waits.
- libogc's `PAD_ScanPads` (disassembled from libogc.a): a GameCube pad that answers PAD_ERR_TRANSFER
  (read twice in one SI poll; the game reads its controls once or twice a frame) keeps its last
  buttons but is left out of the returned connected mask, so that mask flickers. Never treat one
  read's mask as "connected"; only PAD_ERR_NO_CONTROLLER means the pad is gone.
- Writing the log with fflush+fsync per line cost ~10 ms spikes: keep log writes going through
  the async ring during the game.
- Text on screen is 8x16 sprites whose 13th vertex word repeats the character code (handy to
  read menus from dumps).
- The race HUD is not drawn last: the time, map, rev counter and panels are one ELAN group at
  the START of the translucent list (before translucent scenery, then the punch-through trees);
  only the km/h digits are TA sprites, at its end. Each list's groups are separated by dummy
  sprites at x -1..0. A dropped frame still sends those digits but no ELAN list. The HUD stays in
  front by depth (W 0.014-0.15; scenery from W 0.25): split HUD and scene by depth, not by order.
- Depth on GX is 24-bit integer 1/W, the PVR's was float. A race pins the frame's near plane at
  the matrices' 0.01 (the road under the camera), so one Z step is ~6e-6 x W^2 units (0.06 at
  W=100). Exact ties are stable (same vertices, same math); near-ties flicker as the camera
  moves. Flickering trees were two-sided cards: honour ISP CullMode (BACKFACE CULL). gxRend's
  ISP_CULL 2 is the right GX pairing (libogc: clockwise = front; mode 2 culls counter-clockwise);
  the ELAN flips CullMode per model (`cwCulling`) and for the mirror's projection
  (`naomi_elan_tnl.cpp`). Translucent lists must keep writing depth: without a sort the HUD's
  layers need it.
- FPROF's `sh4` is the residual: whatever nothing brackets lands in it. Spikes there are not
  game work if the runner's `--work-from` line is flat. Blocking host work on the emulation
  thread needs its own bucket (`cart` is the latest).
- The race streams its music from the image as **two channels at once** (8 KB each every ~44
  frames): a one-run cache threw out the other channel's run on every read and detected no
  sequence. Think in streams, not in "the last read".
- The launcher's `options.txt` is by label but `autolaunch.txt` holds preset values **by
  position**: add new rows at the end of `CoreOptionId`, whatever their page (a page shows its
  rows in table order, so they land last on it).
- With 3D LIST CULL a list's ELAN pages are checked (PTE change bit) when the ELAN records it,
  once a render, and the draw reuses that check: a game write between the ELAN run and the render
  would go unseen until the next render (Windows census: never happens in Initial D; the GPU
  path always read the records at draw time anyway). The Windows runner takes its write watch
  before each check after an ELAN run (`g_elan_exec_seq`) to see what the Wii would. A program's
  box lives in the ring right after it: ring room matters (lining programs up on cache lines
  as well cost ~15% and made the in-car view rebuild live programs).
- The game's doubles are software (GCC soft-float routines, SH-4 integer code around
  0C221F00-0C224FFF): an FPU-free hot loop can still be floating point. The runner's
  `IDAS3_SF_PROBE` names such routines from their registers. Native replacements follow the
  machine code (libgcc's source is GPL-3) and are checked against it (`--soft-float 2`).
- Guest words are stored in host order on the Wii (big-endian), so a record's bytes in memory are
  each word's bytes high first: the S8 normal is bytes 1-3 of word 0 (z y x), an ARGB colour is
  A R G B. GX array bases may be any byte address (the normals have always started at record+1),
  so COLOUR LISTS reads RGB8 from one byte into the colour word. No GX format reads ARGB.
- The menus are driven with the **steering wheel** ("Steering SELECT, Accel OK"): the runner's
  `left`/`right` move the cursor, `up`/`down` are the shifter. To find a sound, compare the
  `IDAS3_AICA_KEYS` key-ons of a run with presses against one without: menu music keys on notes
  all the time, and two runs drift by a frame. Race sounds (engines, road, tire squeal) are long
  loops keyed on once and then changed in pitch and level, not repeated notes. Sound RAM is
  byte-swapped per word on the Wii: read guest bytes through `AICA_LANE8`.

## Open issues (besides speed)

- Black road where both cars' headlight beams overlap (owner, 2026-11-14): LIGHT SUM built, awaiting
  the Wii test (Status, top entry). If it holds, the same limit may have left black spots in a lone
  car's beam (sums past 2 at ~0.1% of its vertices, e.g. the opponent's rear bumper up close).
- Specular is dropped on the GPU T&L path (visible on untextured lists, and at night on
  bumpers, grilles and the road's glints, where the headlights make it).
- Night headlight beam shows the road's polygons (owner, since day 1, NIGHT LIGHTS CPU and GPU
  alike): the ELAN lights per vertex (Flycast too, same formula) and the cone is hard, 0 from
  24.9 degrees and full within 14.8 (angle A 1.555, B -16.75; reach 200, full to 100; one light a car sent 4
  times, colour 0.5, so 4.0 with the x2 scale), so on big road polygons each corner is either lit or not.
  `IDAS3_HEADLIGHT_CENSUS` on the night demo's race (2026-11-10): the beam reaches 150-280 lists a
  render; 100-1100 cone edges a render, in 20-55 lists (half opaque, half translucent, 10-23K
  vertices); up to ~1.7K lit vertices over 1. Owner: "make it look good, then make it fast".
  Built (2026-11-10, untested on a Wii): HEADLIGHTS (RENDERING 2, last row, default ARCADE (OLD)).
  SOFT: a headlight's cone edge spread 10 degrees each way (4.8-34.9), per vertex, both paths (`softenCone`).
  SMOOTH (GPU path with NIGHT LIGHTS GPU): a list whose bounding sphere crosses the cone's edge gets the
  strongest such headlight as a beam (`ElanGxBeam`): GX light 7 lights colour channel 1 per vertex
  (colour x2 x copies, the excess over 1 on the angle term scale x c, which also gives 0 behind the lamp;
  distance fitted as NIGHT LIGHTS'), TEXCOORD3 projects a 64x64 I8 cone texture (TEXMAP7, TEXMTX3) from the
  lamp, TEV stage 1 puts channel 1 x cone in TEVREG2, stage 2 adds TEVREG2 x the list's texture (untextured:
  one stage); gxRend puts its offset and fog stages back after the item (`elan_gx_draw` returns true).
  Not for vertex-coloured or env-mapped lists, nor under AUTOSORT. A projection mirrors behind the lamp:
  a list whose sphere reaches behind the lamp's plane (85-95% of the lists where the polygon showed: the
  long road segments around the car) also gets a front-mask stage (TEXCOORD4 along the axis, TEXMTX4, a
  16x4 I8 step in TEXMAP3: 0 behind, 1 from 4 units in front), so 3-4 stages. SOFT and SMOOTH also leave a
  headlight out of a list its cone misses (exact). Windows: the cone check holds (max error 0.023 at ~2-3M
  vertices a run). SMOOTH gives beams to many more lists than show cone edges (bounding spheres are loose),
  so the cost is the first thing to read on the Wii (`rend`, `[prof]   HEADLIGHTS per frame`). To check
  on the Wii: the beam's look per value, no ghost beam behind the car (chase view); SMOOTH showing NO beam
  at all would mean the GX spot direction sign is wrong (it is unverified, see NIGHT LIGHTS below).
- Night scenes with NIGHT LIGHTS = GPU: point lights are GX local lights (`gxPointLight` in
  `naomi_elan_tnl.cpp`): fitted distance attenuation (dimmer core, faint tail past the reach),
  parabola for saturating spots, duplicates merged, lights out of a list's reach left out, at
  most 8. Unverified on hardware: GX attenuation above 1 (merged headlights) and the
  spot-direction sign (from libogc's disassembly: `GX_InitLightDir` negates). A point-lit
  list's light cull uses its bounding box; with no box (3D LIST CULL OFF) each such list is
  measured every render, ~55K vertex reads a night frame. NIGHT LIGHTS = CPU keeps the exact CPU path (a few fps at night on a Wii).
- Modifier volumes, the credit-digit glyph, in-race memory headroom, a live window + keyboard
  for the Windows build.
- Windows build has no audio; sound changes can only be heard on Wii/Dolphin.
