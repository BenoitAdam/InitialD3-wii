# Speed work: what was done, what is left

Notes for whoever continues the performance work (people or AI agents). Numbers come from
the profiler logs the owner sent from a real Wii ("REAL WII LOG n") and from Dolphin, between
2026-10-16 and 2026-10-28. Code paths are relative to `port/`.

## The target

A race frame must fit in **16.7 ms** (60 fps). On a real Wii, with every frame drawn
(FRAMESKIPPING 0), the race is at **~24-25 ms** (log 11, 38-41 fps):

| Bucket | ms per frame | What it is |
| --- | --- | --- |
| sh4 | 9.4-9.6 | the game's own code through the dynarec |
| rend | 7.2-7.45 | GX draw of the frame, mostly ELAN GPU lists |
| elan | 4.1-4.2 | ELAN command walk + per-render depth measuring |
| arm | 1.4 | ARM7 sound CPU |
| aica | 1.0-1.7 | AICA sound synthesis |

With **FRAMESKIP AUTO** (1 frame in 4 drawn) the game logic runs at 94-103%: an undrawn frame
costs ~13.5 ms (sh4 9.7, arm 1.35, aica 1.1, ELAN commands 1.2), a drawn one adds 6-8 ms of
render. Menus, attract mode and car select run at 100%.

So ~8 ms must still go for a full-speed, every-frame-drawn race. The SH-4 side alone is more
than half the budget.

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
| 10-30 | 3D LIST CULL: list bounding boxes kept with the fetch programs; lists out of the picture dropped, depth range from the box | Windows: drawn 116.6K -> 63.5-78K vertices, 965 -> 546-705 lists a render; awaiting Dolphin, then log 12 (expect ~3 ms) |

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

Music is an ADPCM stream the SH-4 refills, so its tempo follows emulation speed. SOUND CLOCK
REAL TIME was tried and breaks it (the AICA overtakes the refills): keep EMULATED. Correct music
needs 100% speed, hence FRAMESKIP AUTO for now.

## Tried and dropped

- Software T&L only: cannot reach 60 fps on Broadway (the lighting rewrite gave ~20%).
  Paired-single CPU T&L was considered; GPU T&L was chosen instead.
- BAT-mapping hot guest RAM: DTLB walks were 0-1% on hardware.
- Real-time sound clock: breaks the streamed music.
- Inlining the FR bank swap in `sync_fpscr` (~1.5K real swaps a frame): ~0.2 ms, not worth it
  yet.
- Trusting Dolphin's numbers for anything cache-related.

## What is left, best first

1. **Read log 12 for 3D LIST CULL** (done item 9): `[prof] 3D list cull per frame` and the
   `rend` / `elan` buckets against log 11. The page checks of boxed lists moved from `rend`
   to `elan` (when the ELAN records the list), so judge the sum. Owner, first build
   (2026-10-30): Wii "both work, ~+5-10%"; Dolphin: BEHIND CAMERA faster everywhere (over
   100% at times), OUT OF VIEW faster in the demo and race intro but **~10% speed once the
   player has control** (in-car and chase views). Not reproduced by the runner (driving with
   gas held, views cycled: pin held, ~0.7-1K vertices measured, nothing unusual). Suspected
   the pin falling (every list then measured; Dolphin's page-table memory reads are slow):
   culled lists now hold the pin and unpinned frames measure only the nearest lists. If the
   rebuilt OUT OF VIEW is still slow on Dolphin, its PROFILER log decides (FPROF buckets,
   `depth measured`, `canvas`).
2. **Cheaper fetch validation.** ~1000 page checks a render (PTE lookup + `dcbf` + `sync`,
   ~5 cache misses each; fewer pages now that culled lists are not drawn, but culled lists
   with a box still check theirs). A write-tracking bitmap set by the pager/trampolines would
   replace them. Keep a fallback: Dolphin ignores read-only page bits.
3. **Colour lists through the GPU fetch, with a TEV swap table.** They stay CPU-fed (3-4.6K
   vertices a render, ~220 ns each against ~55 fetched) because their colour word is ARGB and
   GX reads RGBA: in the records it is A,R,G,B in memory, so GX gets R=A, G=R, B=G, A=B, a
   rotation that `GX_SetTevSwapModeTable` (ras side, R<-G, G<-B, B<-A, A<-R) undoes; gxRend
   uses no swap table yet. Unlit lists: the swap alone. Lit lists: the guest alpha is in GX's
   red, so light and ambient colours rotate the same way, red light 0 with red ambient 255
   (alpha unlit, as on the ELAN), ALPHA0 lit with the same lights. Forced-opaque lists: alpha
   from KONST, or keep them CPU-fed. ~0.5 ms; they would also get boxes (item 9).
4. **The rest of the ELAN draw.** Of 6.0-6.5 ms of GPU lists (log 11, before item 9), only
   2.2-2.5 ms is spent sending cached programs; 3.6-4.3 ms is per-list overhead over ~960
   lists (state, matrices, light sets, validation). Item 9 takes ~40% of the lists away;
   batch the rest by state.
5. **GX display lists** for cached programs, if the "ELAN draw a render" line still shows send
   time dominating: an INDEX8 display-list ring (~400 KB for a race) would replace the 256 KB
   program ring (and hold the boxes); `SetArray` is not allowed inside a display list, so one
   list per array segment. The GP has the time (idle ~60% of a race frame).
6. **SH-4 side (~9.5 ms, largest bucket).** It is the game's own work: IPC ~0.6 with many L1
   instruction misses. Directions: block layout (hot/cold, cache-line aligned chains), fewer
   call-outs (timeslice ~1.9K, sync_fpscr ~1.5K, readmem ~700, writemem ~750 a frame), and the
   5% of frames where sh4 jumps to 27-48 ms even on skipped frames (cause unknown; look at them
   with HOTBLOCKS).
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
