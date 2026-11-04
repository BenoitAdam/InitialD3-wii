# Speed work: what was done, what is left

Notes for whoever continues the performance work (people or AI agents). Numbers come from
the profiler logs the owner sent from a real Wii ("REAL WII LOG n") and from Dolphin, between
2026-10-16 and 2026-10-28. Code paths are relative to `port/`.

## The target

A race frame must fit in **16.7 ms** (60 fps). On a real Wii, with every frame drawn
(FRAMESKIPPING 0), the race is at **~19.4 ms** (log 17, Akina uphill, ~80K ELAN vertices: 48.8-54.7
fps, p50 19.2-19.4, p99 22-23):

| Bucket | log 11 | log 12 | log 13 | log 15 | log 16 | log 17 | What it is |
| --- | --- | --- | --- | --- | --- | --- | --- |
| sh4 | 9.4-9.6 | 10.4 | 10.2 | 9.2 | 9.3 | 9.3 | the game's own code through the dynarec |
| rend | 7.2-7.45 | 3.15 | 2.85 | 2.85 | 2.7 | 2.2-2.35 | GX draw of the frame, mostly ELAN GPU lists |
| elan | 4.1-4.2 | 8.0 | 5.5 | 5.1 | 5.0 | 4.35-4.45 | ELAN command walk (1.9-2.0) + recording lists for the GPU (2.3-2.9) |
| arm | 1.4 | 1.25 | 1.25 | 1.23 | 1.16 | 1.17 | ARM7 sound CPU |
| aica | 1.0-1.7 | 1.1 | 1.05 | 1.2 | 1.06 | 1.07-1.12 | AICA sound synthesis |

So ~2.7 ms must still go for a full-speed, every-frame-drawn race, and the SH-4 is nearly half the
frame. Scenes differ: at the race's start (90-105K vertices in, 73-84K on the GPU) log 17 gives
43-45 fps, `elan` 6.4-7.0.

Log 17 (2026-11-04, MEM SWEEP SHORT set by hand, GX FIFO SIZE 512 KB, no diagnostics):
- **GX FIFO SIZE works:** `GX FIFO full` 0.0 in every block, race and demo; `cached programs`
  18-26 ns a vertex (the 23 expected with no wait): ~1.0-1.3 ms at 45-59K vertices, where log 16
  paid 2.2 ms at 58K. `rend` 2.7 -> 2.2-2.35 and `elan` 5.0 -> 4.35-4.45 at ~80K vertices.
- **Race:** 48.8-54.7 fps at ~80K vertices (log 16: 48-51), p50 19.2-19.6 (20.2-21), p99 22-23
  (25). Heavy start (90-105K in) 43.4-45.1 fps, `elan` 6.4-7.0 there. `sh4` 9.3-9.6, unchanged.
- **Night demo:** 56.4-60.9 fps over frames 5040-7440, 59+ in most blocks (log 16: 55.5-60.8).
- **MEM SWEEP SHORT works.** 36 reclaims freed 4.6 MB, 0 failed allocations; free fell to 656 KB
  at the race's load and never again under 1.8 MB. Race-start hitches f11720 158 ms (`tex` 75.6,
  `mem` 45, `elan` 28.5, `rend` 19.8: the first race frame's textures and pages) and f11962 97 ms
  (`tex` 46.9, `sh4` 26), against log 16's 195 and 261 ms (`mem` 238). No hang, no crash.
- Loading (not the race): `cart` stalls up to 166 ms (f11278) on the loading screen, reads the
  read-ahead could not see coming, as before.
- No TEST press logged.

Log 16 (two runs, 2026-11-01):
- **The "MEM SWEEP SHORT hang" was the TEST button.** The hang log's run read TEST as pressed at
  frame ~2050: the game resets the ELAN, turns the caches off at AC0193D6, restarts at its entry
  point and opens the BIOS SYSTEM MENU (the text screen at frames 4440-5400, 870 TA vertices). One
  `--press 2600:test` on Windows gives the same lines in the same order, and a run with no press
  none of them. The sweep code cannot have run: it needs under 256 KB free and this run never went
  under 4.5 MB, with 0 page faults at that moment. So SHORT is still **untested**. The log cannot
  say what pressed it; `[game] TEST button pressed at frame N` now does.
- **Night demo: done.** 21 blocks over frames 5040-7560, every frame drawn: 55.5-60.8 fps, 16 of
  them at 59+, the slowest at 88-102K vertices in. The slow stretch (frames 6240-7080) went from
  37-47 fps in log 14 (diagnostics on, ~12% slower) to 55.5-60.5. Log 14 left the demo at ~7300, so
  its later blocks are not the same scene.
- **The race costs 4.3 ms more than the demo at fewer vertices**, nearly all that separates it
  from 60 fps. Demo at frames 7320-7440 (98K vertices in, 75K on the GPU) against race at 13320-13440
  (80K, 52-58K): `sh4` 7.1 -> 9.3, `elan` 3.85 -> 5.1 (`commands` 1.54 -> 2.0), `arm` + `aica` 1.63 ->
  2.2, `rend` 2.4 -> 2.7. ELAN model lists walked 219 -> 270 a frame.
  Windows, same settings (`IDAS3_HISTO_FILE`, driving route in the in-car view): **only 11% more SH-4
  instructions**, 643K -> 712K a frame, against 33% more `sh4` time. It is not one job: ~200K go to
  race-only code over dozens of ranges, ~130K of demo-only code go away. The biggest, 0C222A00-E7F
  (+27K, 4%), is a large branchy function, not a loop, so nothing like the reflection map to
  throttle. JIT call-outs (log 14) 4500 -> 5800 a frame: `sync_fpscr` +650, `writemem` +370, ~0.25
  ms at most. That leaves ~1 ms of the `sh4` gap unexplained; the route does not drive like a player,
  so its count may be low. [HOT] lines of a race and a demo in the same log would show it.
- **GX FIFO WAKE works, partly.** At 58K vertices a render: full 2.0 times a render (was 1.0),
  `cached programs` 2.2 ms (was 2.9), against ~1.33 without any wait (23 ns a vertex). The demo's
  heaviest blocks still spend 2.55-2.96 ms there. The FIFO fills because the index draws go through
  it, ~6 B a vertex. No crash or glitch reported with it on.
- **Race:** 48-51 fps at 80-86K vertices against log 15's 46.5-48.7, p50 20.2-21 against 20.8.
  `sh4` unchanged. The scenes differ too much to credit anything: the owner's "maybe a bit better"
  is what the log says.
- **Hitches, all at the race's start with MEM SWEEP 2 MB:** f11808 195 ms (`tex` 122, `mem` 72,
  224 textures, 471 pages), f12049 62 ms, f12366 261 ms (`mem` 238, 701 pages, 1 texture). Free
  fell to 268 KB. In the last one a single fault paid for 2 MB, which is what SHORT is for.
- **Memory** after that sweep: 3.4-3.7 MB free over the last ~22 s, 11 MB compressed into 3.8 MB. 3 reclaims
  and 0 failed at the only allocation line (frame 11760). The race lasted ~46 s, so two minutes stay
  unmeasured.
- Loading (not the race): `cart` frames of 85-177 ms (f3117, f4140, f4439, f11207, f11366), reads
  the read-ahead could not see coming.

Before log 16, with the outside cameras (102-127K vertices in) log 15 gave 41-46 fps, `elan` 6.3-7.3
and p50 22.5-23.3.

Log 15 (no diagnostics, REFLECTIONS 1 in 4, DIMM AHEAD, JIT COLD ENTRY, DEPTH MEASURE) settles
log 14's questions:
- **The stutter was the cart reads.** Race tail `sh4` ~21 -> 9.8-10, p99 38-40 -> 25-29. `cart`
  0.03-0.12 ms a frame, tail 0.1-0.4; every run in the race came from the read-ahead, none late.
- **Speed:** like for like (~80K vertices, frames 13440-13800) 46.5-48.7 fps, p50 20.8, against
  log 13's 45-47 fps, p50 21.9: -1.1 ms, all of it `sh4`. REFLECTIONS at 1 in 4 skips 15% of the
  instructions, so ~1.5 ms was expected: that code is cheaper than the average instruction.
- **JIT COLD ENTRY shows nothing yet:** rest-of-frame IPC 0.53-0.56 and L1I 6560-7340/ms against
  0.51-0.55 and 6200-7300 in log 13. No A/B was run.
- **Race intro fixed:** 50-60 fps at frames 12000-12240 against log 14's 32-37; 0.6-1.6K vertices
  measured against 7.6-16.6K, T&L 2.8-3.8 against 6-8 ms. The slow stretch of the night demo was
  not reached (coin at ~6240), so that fix has no Wii figure. Same frames before it: 590-1100
  measured against 500-2360.
- **New hitch, memory:** f12531 215 ms (`mem` 191, 511 pages) and part of f12003's 193 ms (`tex`
  119, `mem` 73.5, 220 textures, 517 pages): the synchronous sweep to 2 MB free, reached because
  free fell to 260 KB (log 14 stayed near 900, and read-ahead takes 140 KB from the pool). It fired
  in a longer race than before. See "Done after log 15".
- **GX FIFO:** full 1.0 times a render at frames 12240-12600, `cached programs` 2.9 ms against
  1.25, the GP still drawing 4.5-7 ms when the CPU ends. Each overflow waited for 112 KB.
- Guest memory after the sweep: 3.3-3.7 MB free, falling ~23 KB/s over ~43 s of race, 3 reclaims
  and 0 failed at the only allocation line (frame 11940). Longer races stay unmeasured.

Before log 15: outside the render the CPU runs at IPC 0.51-0.55 with 16-21% L1 load-miss cycles and
6200-7300 L1 instruction misses a ms (menus 2300-4500, attract race 5500-6000). The counters give
no cycle cost for instruction misses, so their share of the time is unmeasured. The frame-time
tail is the SH-4 too: frame p95 24.5 ms but p99 38; the slowest 5% of frames average `sh4` ~20 ms
against ~10 typical, worst 33, and `elan` only ~1 ms more. Cause unknown. `cc=0` on those lines
says nothing: that count only runs with JIT CCALLS on.

Log 14 (Akina uphill, 40-44 fps with JIT CCALLS + HOTBLOCKS on, so ~12% slower, not comparable)
is the SH-4 census. Typical `sh4` 11.0, tail ~21-22, p95 25.7, p99 40, max 55 ms.
- **The `sh4` spikes are not game work.** The runner counts each frame's SH-4 instructions
  outside the wait loop (`--work-from`): race p5 820K, p50 850K, max 880K, no heavy frame. `sh4`
  is FPROF's residual: anything unbracketed lands in it. Call-outs do not explain the spikes
  (typical ~5620, worst 7700-8200 while `sh4` doubles) and total only ~1 ms of 11.
- **Likely cause: cart reads on the emulation thread** (no bracket until now, so unproven: the
  `cart` bucket measures it in the next log). The race streams its music from the game image as
  two channels at once, A at image offset 02B8B040, B at 02D2B580, 8 KB each every ~44.4 frames,
  on consecutive frames. Reads are at odd offsets, so most span two 8-sector runs, and with one
  run kept each channel threw out the other's: 88 CHD hunks per 1000 frames (each an SD read and
  an LZMA decode) plus DES over 16 KB, where the data is 22.5 hunks. That is 2 frames in 44, 4.5%
  of them: the 5% tail. The worst frames sit on that lattice: 267 frames apart in log 14 (6 x
  44.5), 533 in log 13 (12 x 44.4), and not on the `[prof]` block (96, 3, 30 mod 120).
- **The biggest SH-4 job is the car's reflection map**: 0C048E80-0C0491BF, ~100 ops a cell x 2048
  cells, 170K of 850K instructions a frame (20%; ~2 ms if time follows instructions). It builds a
  64x32 RGB565 panorama of the road side at 0DD1C6C0, cell by cell through tables: seen in a RAM
  dump as road, trees and sky around the car.
- 0C20A2A0, ~8500 entries, is the word-copy loop to VRAM (and 32-byte store-queue bursts). Its
  big job runs every other frame (`copy` 2.8K / 35.6K instructions alternate). What it copies
  is not identified.
- Candidates checked and dropped: `sync_fpscr` is already inline, its ~1800 call-outs are real FR
  bank swaps (item 6). The fixed cost of a call-out kind is small next to 11 ms.
- Night demo and race intro measured far too many vertices (demo 11-35K a frame, T&L 5-11 ms, 37-47
  fps against ~60; race intro 7.6-16.6K, 32-37 fps; race proper ~2.3K). Mostly not the unpinned
  rule first suspected (its `zlo < 2 x nearest` measures ~0): lists wholly behind the camera
  never got a box. See "Done after log 14".
- Code cache 83% full (HOTBLOCKS' 15% included); a flush at 92% would stutter. Guest memory free
  864-1004 KB, flat over ~27 s of race: the 2-minute question stays open.

Log 13 is the first with VIEW + MODELS and the record-side fixes: `elan` -2.5 ms, `rend` -0.3,
p50 -2.9 ms. 106-170 whole models (131-288 lists) stepped over per frame. To watch: at frames
11040-11280 (~160 frames) the GX FIFO fills 0.6-0.9 times a render, `cached programs` costs
2.2-2.7 ms against 1.0-1.4 elsewhere in the race and the speed dips to ~40 fps; everywhere else
in the race the FIFO count is 0.0. Guest memory free fell from 700 KB to 492 KB over frames
11400-12120. The one allocation line, just before the race, shows 3 reclaims and 0 failed, and
the log ends before anything fails, so a longer race is the open question. Colour lists are
course-dependent: ~30 CPU-fed colour vertices a render on Akina against 4.6K on the runner's
route, so the TEV item below gains nothing here.

Log 12 was the first with 3D LIST CULL (OUT OF VIEW): `rend` lost 4.2 ms but `elan` gained 3.9,
so the frame barely moved (the ~3 ms expected did not come). Dropping ~750 of ~1430 lists a
frame works (drawn: ~60K vertices, ~650 lists), but the lookups moved to record time cost ~3.8
ms: ~2.7 us a list, on every list, dropped ones included. At the draw the same work is cheap
because `prefetchAhead` fetches it ahead; the ELAN walk has no such thing, and it runs between
stretches of SH-4 work that evict everything (load-miss cycles 19-22% in the race against 3-5%
in menus). The GP takes ~9 ms a frame whether it draws 60K or 116K vertices, so it is limited
by pixels, not vertices; at ~36% busy it is not the limit yet. In the race's first ~400 frames
the FIFO fills once a render, which costs ~1.3 ms of CPU stall (`cached programs` 2.3 against
0.9 ms): `GX FIFO full` line. The night attract demo runs at ~57-60 fps (was a few fps).

With **FRAMESKIP AUTO** (1 frame in 4 drawn) the game logic runs at 94-103%: an undrawn frame
costs ~13.5 ms (sh4 9.7, arm 1.35, aica 1.1, ELAN commands 1.2), a drawn one adds 6-8 ms of
render. Menus, attract mode and car select run at 100%.


## How we measure

- **Real Wii only for speed decisions.** Dolphin models no cache misses (IPC ~1): on hardware
  the SH-4 side cost ~2.3x and ELAN command walking ~3x the Dolphin time. The owner's Dolphin
  is also overclocked (1.71), so its "speed %" is not a stock Wii.
- Launcher options SPEED OVERLAY + PROFILER write to `idas3wii_log.txt`:
  - `FPROF` per-frame buckets (sh4, rend, elan, arm, aica, tex, mem), typical/p50 and worst
    frames. On NAOMI 2 the `elan` column is the old disc bucket and `rd=` counts ELAN command
    executions.
  - `[prof]` 120-frame blocks: ELAN split (commands / GPU lists / CPU vertex loop / TA feed),
    renders and VRAM copies per second, paging and compression rates, model skip counters.
  - `[HW]` Broadway performance counters, alternating set A (cycles, instructions, L1 load-miss
    cycles, DTLB walk cycles) and set B (L2 hits, L1I misses, L2 data misses, mispredicts),
    for the whole frame and for the 3D render; render probe (CPU ms, GX FIFO overflows, GP idle
    or still drawing at the end); "ELAN vertices a render", "ELAN draw a render" (time in cached
    programs vs the rest), fetch programs built / page gaps / patches / ring passes.
  - JIT CCALLS: `[CC]` calls from JIT code into C per frame, by kind and address.
  - JIT HOTBLOCKS: `[HOT]` hottest SH-4 blocks with disassembly (~12% slower, diagnostic only).
- The Windows runner (`idas3wii_run`) does what the Wii cannot: exact censuses (vertices,
  lists, writers of an address range), PC histograms, and correctness checks of each
  optimisation against the plain path (`--gpu-tnl` fetch check, before/after snapshots).
- Method: every change ships behind a launcher option with the old behaviour kept, the owner
  A/Bs on hardware, the next log decides.

## History of the race speed

| When | Change | Race |
| --- | --- | --- |
| 10-16 | Start: ELAN transform and lighting in software | 3% in Dolphin (520 ms a frame; ~148K ELAN vertices a frame) |
| 10-16 | ELAN frameskip + GPU T&L prototype (opaque lists) | 16% (Dolphin) |
| 10-16 | Per-list depth range instead of forced wide depth; VRAM PRESENT = NOT DURING 3D | 35%, no flicker |
| 10-16 | Translucent + punch-through lists on the GPU | ~90% (Dolphin) |
| 10-16 | Env-map lists on the GPU, depth pin, colour/normal emitters | 85-100% (Dolphin) |
| 10-16 | Lazy GX state restore, templated emitters, xform/light change counters | ~110% (Dolphin) |
| 10-17 | One GX_Begin per strip (no joined strips) | 125-130% (Dolphin) |
| 10-17 | **First real Wii log**: FRAMESKIP AUTO, GPU T&L | 38-46 fps (64-78%) |
| 10-17 | Depth pin kept over skipped renders | GPU lists 3.8 -> 0.46 ms |
| 10-17 | **GPU vertex fetch** (GX indexed arrays straight from guest pages) | render CPU 23-27 -> 8.2-8.9 ms; 86-90% |
| 10-21 | Pager frees frames lowest first (page gaps 92% -> 10%), mini-patches | 94-100% (AUTO) |
| 10-21 | Model skip, cart DMA poll credit, block-cache probe | 92-103% (AUTO), "more fluid" |
| 10-27 | Every frame drawn (log 9) | 40-44 fps |
| 10-28 | Black lights dropped: car select fully on the GPU | car select 28 -> 60 fps |
| 10-28 | Depth measuring spread over 32 renders (log 10 bug, fixed in log 11) | 35 -> 38-41 fps |
| 10-30 | BACKFACE CULL (flickering trees fix): back faces no longer rasterised, ~350-400 GX cull changes a frame | awaiting log 12 (expect equal or a little less GP time) |
| 10-30 | 3D LIST CULL: list bounding boxes kept with the fetch programs; lists out of the picture dropped, depth range from the box | log 12: drawn ~60K vertices, `rend` 7.3 -> 3.15 but `elan` 4.1 -> 8.0: race unchanged (36-41 fps); night demo a few fps -> ~57-60 |
| 10-30 | Whole hidden models stepped over, light sets not rebuilt per material, box rows per matrix, sampled checksums | log 13: `elan` 8.0 -> 5.5, p50 24.8 -> 21.9 ms, race 36-41 -> 42-47 fps |
| 10-30 | DIMM AHEAD, REFLECTIONS, JIT COLD ENTRY, DEPTH MEASURE, boxes for dropped lists | log 15: stutter gone (p99 38 -> 25), p50 21.9 -> 20.8, race intro 32-37 -> 50-60 fps |
| 11-01 | REFLECTIONS default 1 in 4, MEM SWEEP, GX FIFO WAKE | log 16: 48-51 fps at 80-86K vertices, night demo 55.5-60.8 fps, `cached programs` 2.9 -> 2.2 ms in heavy scenes; MEM SWEEP SHORT still untested (the "hang" was TEST) |
| 11-01 | GX FIFO SIZE | log 17: FIFO never full, `rend` + `elan` 7.7 -> 6.6-6.8 ms, race 48.8-54.7 fps at ~80K vertices, p50 19.2-19.6; MEM SWEEP SHORT: race hitches 261 -> 158 ms |
| 11-04 | SOFT FLOAT (native double divide and multiply) | awaiting log 18 |

## What was done

### ELAN 3D (the big one)

NAOMI 2 transforms and lights its geometry on the ELAN chip. A race frame is ~115-148K ELAN
vertices, which Broadway cannot transform in software at 60 fps (even a PC took 25-30 ms).

1. **Frameskip with an early verdict** (`rend_NextFrameSkipped()`): a dropped frame also skips
   its ELAN transform. FRAMESKIPPING 1 / 2 / AUTO (when > 0.5 frame late, max 3 in a row) /
   AUTO MAX.
2. **GPU T&L** (`emu/naomi/naomi_elan_gx.*`, option 3D T&L = GPU): ELAN lists become
   zero-vertex items in gxRend's stream and are drawn with GX position/normal matrices and
   hardware lights (parallel diffuse; projection folded into gxRend's matrix). Opaque, then
   translucent and punch-through (not while TRANS SORT/AUTOSORT are on), then env-map lists
   (GX normal texgen).
3. **Cheap GX submission**: lazy state restore (only changed projection, vertex format,
   channels, colours, texgen are set), templated emitters writing guest words straight to the
   GX FIFO, recordings keyed by transform/light change counters (no memcpy/memcmp), cached
   PNMTX per transform, one `GX_Begin` per strip (joined strips cost +51% indices).
4. **Depth range**: each list's view-Z range is measured on the CPU (`recordForGpu`) to set
   near/far like the hardware's dynamic depth. Then pinned once a frame reaches the near plane,
   and re-measured one list in 32 per render (`kDepthWindow`). Two bugs fixed on the way: the
   pin was reset by skipped renders (log 1), and lists culled behind the camera did not advance
   the slice index, so ~400 lists were measured every render (log 10).
5. **GPU vertex fetch** (`naomi_elan_fetch.inc`, option 3D VTX FETCH): the GPU reads vertices
   itself through GX indexed arrays based at the guest pages' physical frames. Per-list strip
   programs are cached in a 256 KB ring and validated each render through the page table's
   changed bit (`WiiGuestMem_ElanGpuPage`); lists in pages written since the last render and
   colour lists stay CPU-fed; triangles over two non-adjacent frames become small inline
   patches; each program carries a checksum. The pager now hands out free frames lowest first,
   so consecutive guest pages usually land in consecutive frames (gaps 92% -> 10%).
   Result: ~222 -> ~55 ns a vertex on hardware.
6. **Skipped frames do less**: ELAN state commands (matrix/light/material) and "pure" N2 model
   lists (no TA params, projection, waits, links) are stepped over on dropped frames
   (option DROPPED 3D = SKIP MODELS; 3.18M lists not walked in one race, 0 side effects).
7. **Lighting**: active-light lists, `frsqrte` reciprocal square root, fast pow (~20% on the CPU
   path); black lights (colour 0) left out, which moved car select (a black spotlight) fully to
   the GPU path.
8. Prefetch (`dcbt`) in the strip walk (8 vertices ahead), the depth loop and the ELAN command
   walk (next command header).
9. **List bounding boxes** (option 3D LIST CULL: OFF / BEHIND CAMERA / OUT OF VIEW, default
   OUT OF VIEW; runner `--list-cull`). Each fetch program keeps its list's model-space box (6
   floats right after it in the ring, under its checksum; same life as the program). When the
   ELAN records a list (`recordForGpu`), `cachedBox` checks the list's pages for this render
   (the draw reuses the check) and returns the box; its range under the instance matrix
   (`boxRange`, interval arithmetic) then:
   - drops the list if the box is behind the near plane or, OUT OF VIEW, wholly beyond one of
     the 4 planes through the eye and the canvas edges (`outOfPicture`, 8 px margin; canvas =
     gxRend's at the last draw). The list never reaches the renderer: no strip, no state, no
     page checks at draw, no indices. Windows race: ~210-230 lists behind and ~60-200 out of
     view a frame, ~40-54K vertices (of ~116K);
   - pinned (a race), gives the list's depth range instead of the planes, and a box that cannot
     reach its near plane (`zlo > 1.5 near`) needs no measuring at all: only lists that may
     reach it keep their 1-in-32 slice, so the window logic is unchanged. Depth measured
     ~3.7K -> ~0.7-1.6K vertices a frame. A list dropped out of view that may reach its near
     plane (the road under the camera lies below the picture) still counts for the pin as if
     drawn (assumed, or measured in its slice). Unpinned (menus, replays), only lists whose box
     may be the frame's nearest (`zlo` under 2x the last frame's near plane) are measured; the
     others take their box's range (they cannot set the near plane, which is the precision
     there: the runner's menu near planes are unchanged). Car select 13.7K -> 6.1K measured
     vertices, night demo after a camera cut 10-17K -> 4-8K;
   - point-lit lists (NIGHT LIGHTS = GPU) take their light cull from the box
     (`ModelBoxDistance`: the light taken into model space, its distance to the box times the
     least the matrix can shrink a length; exact for rotation + uniform scale, never above the
     true distance), not a vertex pass. Night demo: 13-37 light sets a render (most 50, arena 64).
   Lists without a box (colour lists, pages written, no program yet) keep the old path. Checks:
   the runner verifies every box it hands out against the list's records (0 stale over the race
   and the night demo), the fetch check stays at 0 mismatches, and OFF draws exactly what the
   old code drew (same vertices and lists per render). Ring room: programs now follow each
   other on 4 bytes instead of cache lines (the box would otherwise cost ~15% of the ring and
   the in-car view rebuilt live programs); ring passes over the race route as before (3).

### SH-4 dynarec and memory access

From the `[CC]` and `[HOT]` censuses of a race:

- **IDLE LOOP SKIP** (`naomi_IdleCycleScale`): the game's wait-for-frame loop ran ~9000 times a
  frame; it is now charged 30x its cycles (same timing, far fewer iterations).
- **VRAM32 FAST**: the game copies ~8K words a frame to the 32-bit VRAM interface (int loop and
  `fmov.d` pair loop). Fastmem store trampolines now store into the paged VRAM window inline
  (int, float pair, `sth`), and AICA wave RAM stores too.
- **SQ FLUSH FAST**: store-queue flushes (~4.2K a frame, all to ELAN RAM display lists) copy
  32 bytes inline (4.2K -> 9 C calls a frame). QACR trampoline stores inline.
- `sync_fpscr` inline when the FR bank does not change; the main loop probes the block cache
  before calling `bm_GetCode` (getcode 2.2K -> ~340 a frame).
- Cart DMA status polls (SB_GDST/SB_MDST) credit 4096 cycles each while a transfer runs:
  loading screens polled them 14-27K times a frame.
- All 18 nullDC4Wii JIT presets on (block cache, dynamic IC, FPU pin, fmov direct, T-forward,
  CR0 branch...), JIT ICI = CLEAR ALL (the game flushes the I-cache once).
- Correctness fixes found on the way: 64-bit handler path swapped words on big-endian.

### Done after log 17 (awaiting log 18)

- **SOFT FLOAT** (RECOMPILER, last row, default NATIVE; `emu/naomi/naomi_softfloat.cpp`, hook in
  `ngen_Compile` next to the reflection one). The game does its double-precision maths in
  software: GCC soft-float routines, SH-4 integer code (__divdf3 0C222D00, __muldf3 0C222A00,
  unpack 0C2225A0, pack 0C222440). A race calls each ~23 times a frame, ~40K SH-4 instructions,
  most of them the division's 61-round bit loop (blocks 0C222DD4/DDC, ~1250 entries a frame in
  log 14's [HOT]). The demo makes far fewer (~5.6K soft-float instructions a frame against ~55K in
  a race): part of the race-vs-demo `sh4` gap. The block at either entry now calls a native
  version that follows the machine code step for step (61-bit fractions, implicit bit at 60, 8
  rounding bits; quotient loop; exact 128-bit product; round to nearest even with the sticky
  bit folded in when the rounding bits are exactly half), written from the disassembly, not from
  libgcc's source (GPL-3). It hands NaN, infinity, denormal operands, division by zero and
  results outside the normal range back to the game's code. Both routines, unpack/pack and their
  constants are fingerprinted (FNV-1a), so other code or another game version is never touched.
  Windows: `--soft-float 2` runs the game's code and checks every result against the native one,
  `--soft-float-test N` checks N random pairs against the host's doubles (20M pairs: 0 wrong).
  Race route on Windows: every call taken natively (124K divisions and 124K multiplications over
  13000 frames, none handed back), 0 mismatches in check mode, `record digest` identical line for
  line; **38,083 SH-4 instructions fewer in every race frame** (5.3%, ~724K -> ~686K). On a Wii a
  native division is ~1000 PowerPC instructions (61 rounds of ~16), a multiplication ~60.
  Expected: ~0.3-0.5 ms off `sh4` (the loop is register-only code, cheaper than the average
  instruction). The other routines the race calls in that area are short FPU helpers (float to
  unsigned at 0C222300, int to double at 0C2223B8, ~130 calls a frame) and word copies (0C222348):
  not worth replacing.

### Done after log 16 (log 17 confirmed)

- **GX FIFO SIZE** (RENDERING, last row, default 512 KB; `gp_fifo`, gxRend.cpp). The FIFO is in BSS,
  so the page pool gives up 256 KB, whichever value is chosen. Heavy scenes pay 0.9-1.6 ms a render
  waiting for it; lighter ones nothing, so nothing changes there. Read `GX FIFO full` (should be
  ~0) and `cached programs` (~23 ns a vertex). Only runs on a Wii.
- **`[game] TEST button pressed at frame N`** and the same for SERVICE: see "The target".

### Done after log 15 (log 16 read them)

- **REFLECTIONS** default 1 FRAME IN 4: the owner finds it fine from the outside cameras. A Wii U
  could keep every frame; that split is for later.
- **MEM SWEEP** (RECOMPILER, last row, default SHORT; `page_alloc`, wii_fastmem.cpp). The fault
  handler's sweep frees 32 pages and scans 2048 at most, ~12 ms if pages cost what they did in log
  15, and the next fault that finds under 256 KB free does the next bit. Below 32 KB free it looks
  for as long as it takes. 2 MB (LEGACY) is the old sweep. It runs with interrupts off, and Windows
  cannot run this code.
- **GX FIFO WAKE** (RENDERING, last row, default NEAR FULL). A full FIFO suspended the emulation
  until the GP had read 112 KB; now 32 KB. Written straight to the CP's low watermark after
  `GX_Init` (0xCC00002C/2E, checked in the linked binary: 0x34000), because `GX_SetGPFifo` would
  also reload the FIFO pointers. Expect `GX FIFO full` to count more, each one shorter: read
  `cached programs` ms, not the count. Log 16: count 1.0 -> 2.0, 2.9 -> 2.2 ms.

### Done after log 14 (log 15 confirmed)

All on Windows first. What the Wii made of them is under "The target".

- **DIMM AHEAD** (AUDIO page, last row; `dimm_image.cpp`). Six runs of 16 KB kept instead of one
  (a channel reads across two and has the next ahead, so it holds three; with four the channels
  still threw each other's out). A run that follows one still kept is a stream: the next is
  decoded before it is asked for. On a Wii by a thread at priority 32 (emulation 64, log writer
  80), which holds the CHD under a mutex and never reads guest memory. Below 100% speed the
  emulation never sleeps, so while the thread has work each vblank gives it 1 ms
  (`naomi_CartSlice`, in the frame and in the `cart` bucket). A run still being decoded when
  asked for is waited for. Windows: 88 -> 22 hunks per 1000 frames, exactly the data rate.
  Each run's slices add up to ~3.5 ms (from `cart`). OFF: one run, decoded when asked for, as
  before. `[prof]   DIMM runs` shows on demand / ahead /
  late / started. +80 KB of heap and a 32 KB stack, taken before the core starts.
- **REFLECTIONS** (RENDERING, default 1 FRAME IN 2). At the reflection map's first row (block
  0C048E86, R4 = 0) a skipped build jumps to the routine's own "no rows" exit, 0C0491CE, so all
  its cleanup runs and the last panorama stays in its buffer. Matched on opcodes. Windows: 38
  instructions instead of 175K on a skipped frame, the rest of the frame unchanged. The kept
  builds fall on the 32 KB copy's frames (875K / 667K instructions alternating). Snapshots at 1
  in 4 against every build (CPU path, outside cameras): differences only inside the car's
  outline, nearly all under 24/255, timer and speed identical to the frame. In the in-car view
  the car body is not on screen and the frames are byte-identical: that work is wasted there.
- **JIT COLD ENTRY** (RECOMPILER, last row, default ON). The timeslice exit goes after the block's
  tail: `blt` instead of a taken `bge` over 12 bytes in the first line of every block, ~95K
  entries a frame against ~1870 exits. Wii only, so unproven by the runner.
- **Dropped-list boxes** (no option of its own: 3D LIST CULL OFF turns it off). A list wholly
  behind the camera with no box was dropped before the renderer, so it never got a program, so
  never a box: measured again every render, point-lit ones always (log 14: `programs built 0`
  with 24K vertices measured). Its model-space box is now kept in a 256 x 4 table (40 KB BSS), good
  while its pages stay unwritten. Models holding such lists can now be stepped over whole.
  Direct-mapped, two lists on a slot threw each other out every render (~70 lists a render).
  Windows night demo, with DEPTH MEASURE: vertices measured over the run 425K -> 88K, worst 60
  frames 32.5K -> 3.8K a frame; every box checked against its records, 0 stale.
- **DEPTH MEASURE** (RENDERING, default RACE PLANE). Boxes cross the near plane in fly-bys though
  no vertex does (road lists), and one unmeasured list holds the frame's near plane at its box
  anyway, so measuring is all or nothing. Lists that may reach the plane now keep their 1-in-32
  slice on those frames too, whose near plane is then the race's. EXACT is the old rule.
  Windows night demo: the near plane drops by 2x or more on 2.1% of renders, the largest
  0.395 -> 0.01. Wii Z emulation at the race plane, demo frames: median 56 pixels of 307K differ
  from exact. The worst frame (5087 pixels, 7516) was already at 0.01 under the old rule.
  Not reproduced: the race intro's 7.6-16.6K measured vertices on a Wii (32-37 fps). The runner's
  route never passes ~2K there, so log 15 alone tells if it is fixed.
- `cart` FPROF bucket, `recSh4: cache N% full` once a percent (1128 lines in log 14).

### Guest memory

NAOMI 2 needs 88 MB of guest memory, the race touches ~46.5 MB of non-zero data.
`emu/wii/wii_fastmem.cpp` maps guest RAM/VRAM/ELAN/sound RAM through the MMU and pages on
demand: reads of untouched pages hit a zero page, a pool from the tops of MEM1/MEM2 (~42 MB,
raised from 27 by streaming the BIOS zip and trimming tables), and a compressed page store
(own LZ codec, clock sweep on the PTE R bit, ~2x). Tuned after log 6: pages that compress
badly are skipped (`COMP_MAX_BYTES` 2048). On hardware: DTLB walks 0-1% of cycles, paging
~0 ms a frame. **Not a bottleneck now.**

### Renderer and presentation

- VRAM PRESENT = NOT DURING 3D: VRAM framebuffer copies between 3D renders caused the flicker
  and wasted presents.
- Texture cache modes (owner's choice: NORMAL); 4/8bpp CI paths; TMEM cache.
- User tile clip for N2 models (in-car view with its mirror, correctness).

### Logging overhead

- The log did fflush + fsync per line: ~10 ms spikes every 120 frames whenever PROFILER or the
  overlay was on. Now an async ring (`log::setAsync`, writer thread, fsync every 500 ms).
- The call-out census capped (it cost ~10 ms a frame during loading storms).

### Sound (not a speed item)

Music is an ADPCM stream the SH-4 refills, so with SOUND CLOCK EMULATED its tempo follows emulation
speed and it is only right at 100%. REAL TIME broke it at the 2026-10-17 speeds (the AICA overtook
the refills), but the owner finds it OK with the log 15 build. Its cost is not measured: at 70%
speed it should run ~1.4x the sound work per emulated frame, up to ~1 ms on `arm` + `aica`.
Owner's decision (2026-11-01): good music matters more than those fps below 100%, so REAL TIME
becomes the default once the speed tests are over. The default stays EMULATED until then, to keep
logs comparable. The cost does not need measuring, and the question is closed.

## Tried and dropped

- Software T&L only: cannot reach 60 fps on Broadway (the lighting rewrite gave ~20%).
  Paired-single CPU T&L was considered; GPU T&L was chosen instead.
- BAT-mapping hot guest RAM: DTLB walks were 0-1% on hardware.
- Real-time sound clock: broke the streamed music on 2026-10-17; OK in the log 15 build (see Sound).
- Inlining the FR bank swap in `sync_fpscr` (~1.5K real swaps a frame): ~0.2 ms, not worth it
  yet.
- Trusting Dolphin's numbers for anything cache-related.

## What is left, best first

Numbers are kept because the items refer to each other. After log 17 the order is: measure SOFT
FLOAT on a Wii, then item 1 (`elan` 4.4-7 ms), item 6 (SH-4 9.3 ms: spread out, see "The target")
and item 8 (sound, 2.2 ms, never profiled).
Sending the index draws as display lists would take `cached programs` (1.1 ms, 2-3 in heavy scenes)
and the FIFO load off the CPU, at the price of a larger ring and a rewrite of the fetch programs
(CP arrays cannot be set inside a display list). JIT COLD ENTRY needs an A/B before it counts as a
gain.

1. **The cost of recording lists** (`elan`, log 12: 5.4 ms over ~1430 lists, ~2.7 us of it
   per list new with item 9). Done after log 12, log 13 confirms ~2.5 ms of the ~2.5 expected:
   - 3D LIST CULL = VIEW + MODELS (value 3, default): whole models stepped over before their
     commands are read (`hiddenModel`, naomi_elan_cmd.cpp). Models are 100% pure, hold no
     matrix or light (census) and ~3.1 lists inline in their block, so the union of their
     lists' boxes under the current matrix and one range check of the block replace the walk.
     Only ~23% of lists are in wholly hidden models (~29% more are dropped singly in models that
     stay partly in view), expected ~1.5 ms. What a skip must leave: the model's last material
     (deferred, decoded only if a list comes first), its depth slices (`tnl_ListsIndexed`).
     A model that may reach its near plane is walked, for the pin.
   - Light sets keyed on the light generation and the lit bit: a material no longer rebuilds
     them (~900 times a frame: 500 bytes cleared, filled, compared). Expected ~0.5 ms.
   - Box tests against rows built once per matrix (`BoxView`), three multiplications a row.
   - Program checksums one render in 32 (`kSumWindow`): summing read the whole ring.
   Checks: `record digest` (everything handed to the renderer) identical to the previous code
   at OUT OF VIEW over the race and the night demo; every model stepped over is walked by the
   runner against its summary (0 stale). Value 3 is not comparable by digest with value 2: a
   hidden list that lost its own box (its program overwritten) is then not drawn at all.
   `elan` is still 5.5 ms. Next steps there: prefetch the next list's table entries in the
   walk, a per-page PTE pointer with an uncached read for page checks (item 2), compiled models
   (list descriptors kept per model, no ELAN RAM read for a model that did not change).
2. **Cheaper fetch validation.** ~1000 page checks a render (PTE lookup + `dcbf` + `sync`,
   ~5 cache misses each; fewer pages now that culled lists are not drawn, but culled lists
   with a box still check theirs). A write-tracking bitmap set by the pager/trampolines would
   replace them. Keep a fallback: Dolphin ignores read-only page bits.
3. **Colour lists through the GPU fetch, with a TEV swap table** (log 13: pointless on Akina,
   ~30 vertices a render). They stay CPU-fed (4.6K vertices a render on the runner's route,
   ~220 ns each against ~55 fetched) because their colour word is ARGB and
   GX reads RGBA: in the records it is A,R,G,B in memory, so GX gets R=A, G=R, B=G, A=B, a
   rotation that `GX_SetTevSwapModeTable` (ras side, R<-G, G<-B, B<-A, A<-R) undoes; gxRend
   uses no swap table yet. Unlit lists: the swap alone. Lit lists: the guest alpha is in GX's
   red, so light and ambient colours rotate the same way, red light 0 with red ambient 255
   (alpha unlit, as on the ELAN), ALPHA0 lit with the same lights. Forced-opaque lists: alpha
   from KONST, or keep them CPU-fed. ~0.5 ms; they would also get boxes (item 9).
4. **The rest of the ELAN draw.** Of 6.0-6.5 ms of GPU lists (log 11, before item 9), only
   2.2-2.5 ms is spent sending cached programs; 3.6-4.3 ms is everything else inside
   `drawItem`, over ~960 lists: the CPU-fed colour vertices (item 3), matrices, light sets,
   the list lookup, and each list's whole program read for its checksum (`progSum` in
   `fetchProgram`, every list every render; the check has never fired on hardware: sample it,
   one list in 32). Item 9 takes ~40% of the lists away. The lookup is done twice now (record
   and draw): keep the slot in the item. Cheaper page checks without a bitmap (item 2): a
   per-page pointer to the PTE and an uncached read of it needs no `pte_find`, `dcbf`, `sync`.
5. **GX display lists** for cached programs, if the "ELAN draw a render" line still shows send
   time dominating: an INDEX8 display-list ring (~400 KB for a race) would replace the 256 KB
   program ring (and hold the boxes); `SetArray` is not allowed inside a display list, so one
   list per array segment. The GP has the time (idle ~60% of a race frame).
6. **SH-4 side (10.2 ms in log 13, largest bucket).** It is the game's own work: IPC ~0.55 with
   many L1 instruction misses. PC histogram while driving (runner, interpreter instruction counts, not
   Wii time; wait-for-frame loop left out): 0C048E80-0C0491BF ~20% (the reflection map, see log 14),
   0C1DA1C0-0C1DA57F ~10%, 0C207D80-0C2081FF ~9%, 0C1D4C40-0C1D4FBF ~7%, then a long flat tail. All of it is plain compiled code, 40-50% `mov.l` and ~0% floating
   point, some with `pref` (store-queue copies): there is no matrix kernel to replace with
   native code. Its doubles are software, though: SOFT FLOAT runs the divide and multiply natively
   (after log 17). Left: what the 32 KB copy is and whether it needs doing (~4% of the instructions
   on its frames), a JIT memcpy idiom for it, wider timeslices, hot guest data in MEM1 (whether
   first touch gives MEM1 to the right pages is unchecked, and load misses are ~18% of cycles).
   Call-outs are ~1 ms in all (timeslice 1866, sync_fpscr ~1800, writemem ~760, readmem ~420,
   getcode ~390, sync_sr ~275, interrupt ~270 a frame): not the lever.
7. **Overlap CPU and GPU.** The GP is still drawing ~2.5 ms after the CPU finishes the render;
   async render is on, but the next frame's SH-4 work could start earlier.
8. **Sound side (~2.5-3 ms: ARM7 + AICA).** Not profiled yet.
9. **Texture decode bursts**: tail frames with ~110 decodes (~10 ms). Spread or pre-decode.
10. **Loading screens (35-60% speed)**: AICA wave RAM uploads over the G2 bus (SB_FFST polls,
    suspend registers): the same poll-credit trick as the cart DMA would help.
11. Accuracy items with a speed cost to measure: specular on the GPU path (COLOR1 +
    GX_AF_SPEC and one TEV add stage; the GP has the time), point lights on the GPU (ELAN uses
    a linear clamped distance attenuation that GX local lights do not match). Per-pixel TEV
    work is not a speed lever: the frame waits on the CPU, not the GP.
12. **Night scenes (attract demo, night courses).** Every lit list carries 6-11 point lights
    (street lamps reach 40 units, the car's lights 100-200; a car's headlight is sent up to 4
    times), no parallel light: ~600 lists, 55K vertices a frame. On the CPU path that was
    30-90 ms of T&L on a PC, a few fps on a Wii. Done: (a) point lights beyond their reach are
    skipped per vertex on the CPU path, exactly; (b) option NIGHT LIGHTS = GPU (APPROX), the
    default: point-lit lists go to the GPU path with GX local lights (`GX_AF_SPOT`, fitted
    k0/k2, a parabola for saturating spots, duplicates merged, a light cull against each list's
    box). On Windows (`--gpu-tnl`) the demo leaves 0 vertices to the CPU (T&L 25 ms -> ~4 ms a
    frame on a PC, host census included); `--point-lights 1` without `--gpu-tnl` previews the
    look in snapshots. The box used to cost every point-lit list a full vertex pass each drawn
    render (~55K reads a night frame): with 3D LIST CULL the lights come from the cached
    model-space box (done item 9); only renders right after a camera cut (unpinned) still
    measure, ~10-17K vertices for a few renders.
