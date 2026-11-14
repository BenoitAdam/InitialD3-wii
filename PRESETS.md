# Presets

Every launcher option (the core presets), in the order each launcher page shows them.
Source of truth: `port/src/game/core_options.h`. The default is the value a fresh
`options.txt` gets. Values in brackets are the preset numbers `autolaunch.txt` holds, by
position: the numbers are not the labels, so a row added at the end of `CoreOptionId`
keeps the others' positions.

Options are saved by label (`LABEL=VALUE` in `options.txt`), so renaming a label drops the
saved choice.

Pages: GENERAL, RENDERING, RENDERING 2, AUDIO, CORE, RECOMPILER, RECOMPILER 2, FIXES, LOGS.

Notes:
- "(OLD)" is the earlier behaviour, kept so the owner can A/B it on the Wii.
- "(LEGACY)" means the same: the behaviour before the option existed.
- The launcher also has launch and navigation rows that are not listed here.

---

## GENERAL

| Option | Values [preset] | Default | What it does |
| --- | --- | --- | --- |
| 2 PLAYERS | OFF [0], SIDE BY SIDE [1] | OFF | Split-screen races. Controller 2 (GC port 2 / Wiimote 2) drives the CPU car. The game draws one view a frame; the cars take turns, each into its half of the picture. |
| MIRROR | ON (LEGACY) [1], OFF (FASTER) [0], OFF + GAME SKIP [2] | OFF + GAME SKIP | In-car view's rear-view mirror. The mirror draws the scene twice. OFF leaves the mirror out of the draw. OFF + GAME SKIP also skips the game's own mirror view, so neither its CPU work nor its commands are left. |
| FRAMESKIPPING | 0 (DEFAULT) [0], 1 (1 OF 2) [1], 2 (2 OF 3) [2], AUTO [3], AUTO MAX [4] | 0 (DEFAULT) | Dropped frames also skip their 3D transform. 0 draws every frame. 1 and 2 draw one frame in two or three. AUTO drops frames only while behind real time, so emulated time keeps 100% speed. AUTO MAX is AUTO with a tighter deadband (reacts a frame sooner) and at least 6 fps shown. |
| SPEED OVERLAY | OFF [0], ON [1] | ON | On-screen FPS and emulation speed %. |
| SPEED LIMITER | OFF (UNCAPPED) [0], ON (CAP 100%) [1] | ON (CAP 100%) | Caps emulation at 100%. Off, the menus run up to about 160%. |
| TEST MODE | OFF [0], ON [1] | OFF | TEST button (D-pad LEFT on every pad). OFF ignores it. ON enters the game's test menu. One tap is enough to enter the BIOS system menu, so it stays off unless asked for. |
| GC PAD LAYOUT | A/B PEDALS [0], R/L PEDALS (OLD) [1] | A/B PEDALS | GameCube pads: A/B are gas/brake and R/L shift (A/B PEDALS), or R/L are gas/brake and A/B shift (R/L PEDALS). |
| P2 CAR | ON RAILS (OLD) [0], FULL PHYSICS [1] | FULL PHYSICS | 2 PLAYERS only. ON RAILS: player 2 nudges the CPU car's speed and heading on its line. FULL PHYSICS: the player's physics runs a second time a frame for player 2. |
| SMALL 3D | DRAW ALL (LEGACY) [0], UNDER 2 PIXELS [2], UNDER 4 PIXELS [4], UNDER 8 PIXELS [8] | UNDER 2 PIXELS | GPU 3D T&L. 3D objects smaller than this on screen are not drawn. Needs 3D LIST CULL. At 8 px trees visibly clip. |
| IN-CAR REFLECT | AS REFLECTIONS [0], 1 FRAME IN 16 [16], FROZEN [-1] | 1 FRAME IN 16 | In-car view only (the view with rear-view mirror models on screen): only the opponent's car shows the reflection there. Rebuilt less often, or frozen. |

## RENDERING

| Option | Values [preset] | Default | What it does |
| --- | --- | --- | --- |
| DROPPED 3D | WALK ALL [0], SKIP MODELS [1] | SKIP MODELS | 3D models in the frames the frameskip drops: skip them (faster) or walk them all. |
| FIXED DEPTH | OFF (DYNAMIC) [0], WIDE [1], TIGHT [2] | OFF (DYNAMIC) | Fixed near and far planes. TIGHT means less Z-fighting, but the extremes clip. |
| DEPTH CLIP | OFF (LEGACY) [0], NEAR MARGIN (WII) [1], NO CLIP (DOLPHIN) [2] | NEAR MARGIN (WII) | Depth clipping. NO CLIP matches Dolphin, not the Wii. |
| HUD PASS | OFF [0], OVERLAY (NO ZW) [1], PROTECT (Z-WRITE) [2] | OFF | Brings back a HUD clipped by FIXED DEPTH = TIGHT. |
| ISP DEPTH FUNC | OFF (LEGACY) [0], ON (OPAQUE/PT) [1], ON (ALL LISTS) [2] | OFF (LEGACY) | Per-polygon depth test (experimental). |
| TRANS SORT | OFF (FASTER) [0], ON (CORRECT) [1] | OFF (FASTER) | Back-to-front sort of translucent polygons. |
| PUNCH THROUGH | OFF (FASTER) [0], ON (CORRECT) [1] | OFF (FASTER) | Punch-through list order and alpha test. |
| AUTOSORT | OFF [0], 1 LAYER (SLOW) [1], 2 LAYERS (SLOW) [2], 3 LAYERS (SLOW) [3], 4 LAYERS (SLOW) [4] | OFF | Real per-pixel translucent sort. Very slow. |
| SPLIT SCREEN | OFF (LEGACY) [0], TILE CLIP [1], MULTI-PASS [2], BOTH [3] | OFF (LEGACY) | Multi-pass rendering: one picture from renders that each draw part of it. |
| VRAM PRESENT | ALWAYS (LEGACY) [0], NOT DURING 3D [1] | NOT DURING 3D | Copies of video memory between 3D frames. NOT DURING 3D avoids flashes. |
| NIGHT LIGHTS | CPU (EXACT) [0], GPU (APPROX) [1] | GPU (APPROX) | 3D T&L GPU: night lamps and headlights lit on the GPU. Approximate, far faster. CPU is exact and slow (a few fps at night). |
| BACKFACE CULL | OFF (LEGACY) [0], ON [2], ON, OTHER SIDE [1] | ON [2] | Drops polygons facing away, as the arcade does. Fixes flickering trees. If the road or cars vanish, try OTHER SIDE. |
| 3D LIST CULL | OFF (LEGACY) [0], BEHIND CAMERA [1], OUT OF VIEW [2], VIEW + MODELS [3] | VIEW + MODELS [3] | 3D T&L GPU: skips 3D lists behind the camera or out of the picture, and whole models with VIEW + MODELS. Needs 3D VTX FETCH. |
| REFLECTIONS | EVERY FRAME [1], 1 FRAME IN 2 [2], 1 FRAME IN 3 [3], 1 FRAME IN 4 [4], 1 FRAME IN 8 [8] | 1 FRAME IN 4 [4] | How often the car body's reflection map is rebuilt. Less often is faster. |
| DEPTH MEASURE | EXACT (LEGACY) [0], RACE PLANE [1] | RACE PLANE | Fly-bys and replays with no pin: keep the race's near plane (faster) or measure the exact one. |
| GX FIFO WAKE | HALF (LEGACY) [0], NEAR FULL [1] | NEAR FULL | When the GPU FIFO is full, the CPU goes on after 32 KB were read (NEAR FULL), not 112 KB (HALF). Helps heavy scenes. |
| GX FIFO SIZE | 256 KB (LEGACY) [0], 512 KB [1] | 512 KB | Room for what the CPU sends the GPU. Heavy scenes wait less for it to catch up. |
| DISPLAY LISTS | OFF (LEGACY) [0], ON (FASTER) [1] | ON (FASTER) | 3D VTX FETCH: cached 3D sent as GX display lists, so the CPU no longer writes indices. Takes 1 MB from guest memory. |
| SCREEN BUFFERS | 2 (OLD) [0], 3 (NO TEARING) [1] | 3 (NO TEARING) | Display buffers. With 2, a render's copy can land on the picture on screen and tear. 3 always has a free buffer (costs 0.7 MB). |

## RENDERING 2

| Option | Values [preset] | Default | What it does |
| --- | --- | --- | --- |
| P2 SCENERY | AROUND P1 (OLD) [0], AROUND EACH CAR [1] | AROUND EACH CAR | 2 PLAYERS only. The course is drawn around player 1 only (lighter; player 2's view goes empty about 800 m away), or around the car each view follows. |
| 4BPP MODE | I4 STUB [0], 4BPP OPTIMIZED [1], CI4 (FAST) [2] | 4BPP OPTIMIZED [1] | 4-bit palette textures. |
| 8BPP MODE | I8 STUB [0], 8BPP OPTIMIZED [1], CI8 (FAST) [2] | 8BPP OPTIMIZED [1] | 8-bit palette textures. |
| 3D T&L | CPU (SOFTWARE) [0], GPU (PROTOTYPE) [1] | GPU (PROTOTYPE) [1] | Who lights and transforms 3D: the Wii GPU for opaque 3D, or the CPU in software. |
| 3D VTX FETCH | OFF (CPU FEEDS) [0], ON (GPU READS) [1], ON, NO PATCHES [2] | ON (GPU READS) [1] | GPU T&L: the GPU reads 3D models itself. NO PATCHES: the CPU feeds strips over gaps. |
| TEXTURE CACHE | VERY FAST [0], VERY FAST+ [7], FAST [1], NORMAL (DEFAULT) [2], QUALITY (SLOW) [3], VERY FAST+ (OLD) [6] | NORMAL (DEFAULT) [2] | Texture cache policy. VERY FAST+ is NORMAL's arena plus a memo of each render's binds. The (OLD) VERY FAST+ aliases on NAOMI 2 and shows glitches. |
| VQ AS CMPR | OFF (DEFAULT) [0], ON [2], ON (OLD) [1] | OFF (DEFAULT) [0] | VQ 565 textures as GX CMPR (4 bits a texel). ON (OLD) is the old encoder. Initial D 3 has no VQ 565 texture, so this does nothing in the game. |
| TMEM CACHE | OFF (LEGACY) [0], ON (FASTER?) [1] | ON (FASTER?) [1] | Keeps the GPU texture cache warm. |
| COLOUR LISTS | CPU (LEGACY) [0], GPU FETCH [1] | GPU FETCH [1] | 3D VTX FETCH: vertex-coloured 3D read by the GPU too, when its alpha is opaque. |
| ELAN PREFETCH | OFF (LEGACY) [0], ON (FASTER?) [1] | ON (FASTER?) [1] | The 3D command walk loads its next commands ahead. Dropped frames walk faster. Speed only. |
| MODEL PREFETCH | OFF (LEGACY) [0], ON (FASTER?) [1] | ON (FASTER?) [1] | The 3D command walk loads a model's commands before reading them (drawn frames). Speed only. |
| HEADLIGHTS | ARCADE (OLD) [0], SOFT [1], SMOOTH [2] | ARCADE (OLD) [0] | Night headlight beams. ARCADE shows the road's polygons in the beam. SOFT widens the cone's edge by 10 degrees each way. SMOOTH draws a per-pixel beam (needs 3D T&L GPU and NIGHT LIGHTS GPU). |
| MOTION FX | OFF [0], SPEED BLUR [1], FRAME BLEND [2], IN-BETWEEN [3], BLUR + IN-BETWEEN [4] | OFF [0] | Race smoothing at 30 fps, HUD kept sharp. SPEED BLUR smears the scenery along the motion. FRAME BLEND mixes the last frame. IN-BETWEEN fills dropped frames (in-car view only, needs FRAMESKIPPING; not in 2 PLAYERS). |
| MOTION LEVEL | LOW [1], MEDIUM [2], HIGH [3] | MEDIUM [2] | MOTION FX strength: blur length, blend opacity, how far an in-between frame moves. |
| BETWEEN WARP | ROTATION ONLY [2], BANDS (OLD) [1], SMOOTH [0], NO MOVE (TEST) [3] | ROTATION ONLY [2] | The IN-BETWEEN picture. ROTATION ONLY warps only the turn (no depth). BANDS (OLD) warps in three depth bands. SMOOTH is one pass. NO MOVE is a test: the same picture through the same path, unwarped. |
| LIST MATHS | LIBM (OLD) [0], FAST [1] | FAST [1] | 3D lists' light and cull bounds without the Wii's slow software square root (night). |
| 3D DECODE | LIBM (OLD) [0], TABLES [1] | TABLES [1] | 3D materials, lights and box tests decoded without libm calls. Same results. |
| DRAW MEMO | OFF (OLD) [0], ON [1] | ON [1] | The 3D draw reuses the GPU program slot found when recording. Also the matrix scale without libm. |
| CACHE HINTS | OFF (OLD) [0], ON [1] | ON [1] | 3D tables loaded ahead and written without reading them first. Same picture. |
| LIGHT SUM | FULL (OLD) [0], SCALED [1] | SCALED [1] | Night: where lights add up (both cars' beams), the Wii drew black. SCALED lights such lists at 1/2 or 1/4 and multiplies back in the TEV. Same picture, 1-2 bits less precise. |

## AUDIO

| Option | Values [preset] | Default | What it does |
| --- | --- | --- | --- |
| AUDIO SYNC | 0 (FREE RUN) [0], 4 (RECOMMENDED) [4], 8 (MOST SLACK) [8] | 0 (FREE RUN) [0] | Paces emulation to the sound, on the emulated clock only. FREE RUN never waits. |
| AUDIO BLOCK | 1024 (23 MS) [0], 1536 (35 MS) [1], 2048 (46 MS) [2] | 1024 (23 MS) [0] | Samples per sound buffer. Bigger means more slack and more delay. |
| AUDIO DRC | OFF (DEFAULT) [0], GENTLE (+-1%) [1], NORMAL (+-3%) [2], WIDE (+-15%) [3] | OFF (DEFAULT) [0] | Retunes the output pitch slightly to avoid crackle. |
| SOUND CLOCK | EMULATED (EXACT) [0], REAL TIME [1] | REAL TIME [1] | REAL TIME: music keeps its tempo when the game is slow. EMULATED: exact, but the tempo follows the speed. |
| DIMM AHEAD | OFF (ON DEMAND) [0], ON (THREAD) [1] | ON (THREAD) [1] | Decodes streamed game data (the music) ahead on a thread: small slices, no stutter. |
| SOUND MIX | FULL (EXACT) [0], HALF (FASTER) [1] | FULL (EXACT) [0] | HALF mixes every other sample: 22 kHz, duller. No gain measured on a Wii. |
| MENU SOUND | 0 (MUTED), 1, 2, ... 9, 10 (AS THE GAME): preset = the number [0]...[10] | 5 [5] | Menu cursor sound (left/right/up/down). 0 is muted, 10 is as the game. |
| TIRE SOUND | 0 (MUTED), 1, 2, ... 9, 10 (AS THE GAME): preset = the number [0]...[10] | 4 [4] | Tire squeal when the car slides or drifts. 0 is muted, 10 is as the game. |
| SOUND STEP | PER SAMPLE (OLD) [0], LAZY [1] | LAZY [1] | Sound chip timers and interrupts updated only when they change. Same sound. |

## CORE

| Option | Values [preset] | Default | What it does |
| --- | --- | --- | --- |
| IDLE LOOP SKIP | OFF [0], ON [1] | ON [1] | The game's wait-for-frame loop runs 30x fewer times. Same timing. |
| MEM SWEEP | 2 MB (LEGACY) [0], SHORT [1] | SHORT [1] | Guest memory nearly full: free a little at a time (SHORT) or 2 MB at once (a 200 ms stall). |
| PAGE GUARD | OFF (LEGACY) [0], ON (READ-ONLY) [1] | ON (READ-ONLY) [1] | Unchanged 3D data made read-only: no check each frame, writes still seen. |
| SH-4 TIMESLICE | 1792 (OLD) [0], 3584 (WIDER) [1] | 3584 (WIDER) [1] | SH-4 cycles between device updates. Half the C calls. Devices keep their pace, but interrupts are seen up to 18 us later. Changes timing. |

## RECOMPILER

| Option | Values [preset] | Default | What it does |
| --- | --- | --- | --- |
| JIT SBP | OFF [0], KNOWN (DEFAULT) [1], ALL RAM (SLOW) [2] | KNOWN (DEFAULT) [1] | Stale or self-modified block guard. |
| JIT ICI | CLEAR ALL (DEFLT) [0], IGNORE (NEED SBP) [1], IGNORE + SBP=ALL [2] | CLEAR ALL (DEFLT) [0] | What a guest I-cache flush does. Initial D 3 flushes once, at boot. |
| FASTMEM | OFF (LEGACY) [0], ON (FASTER) [1] | ON (FASTER) [1] | MMU-mapped JIT memory. |
| JIT BCACHE | OFF (LEGACY) [0], ON (FLAT) [1] | ON (FLAT) [1] | One-cacheline dynamic jump dispatch. |
| JIT DYN IC | OFF [0], ON (JSR/JMP) [1], ON (+RTS) [2] | ON (+RTS) [2] | Bakes the last target at each branch site. |
| FPU PIN | OFF (LEGACY) [0], ON (EXPERIMENTAL) [1] | ON (EXPERIMENTAL) [1] | Pins fr0-15 to real FPU registers. |
| JIT ALIGN | OFF (LEGACY) [0], ON (32B LINES) [1] | ON (32B LINES) [1] | Aligns JIT blocks to cache lines. |
| JIT IFB FLUSH | OFF (FULL SPILL) [0], ON (SELECTIVE) [1] | ON (SELECTIVE) [1] | Spills only the registers the opcode touches (+1%). |
| JIT NEW OPS | OFF (LEGACY) [0], ON (9 OPS JITTED) [1] | ON (9 OPS JITTED) [1] | JIT rotcl, rotcr, tas.b, sr, fpscr (9 ops). |
| JIT T-FORWARD | OFF (LEGACY) [0], ON (NO RELOAD) [1] | ON (NO RELOAD) [1] | Skips the T store and reload on cmp+bt (+4%). |
| FMOV DIRECT | OFF (GPR BOUNCE) [0], ON (LFS/STFS) [1] | ON (LFS/STFS) [1] | fmov straight to and from pinned FPRs (+10%). Needs FPU PIN and FASTMEM. |
| JIT CARRY OPS | OFF (LEGACY) [0], ON (10 OPS) [1] | ON (10 OPS) [1] | JIT addc, subc, div1, cmp-str (10 ops). |
| JIT MAC OPS | OFF (LEGACY) [0], ON (FASTMEM RDS) [1] | ON (FASTMEM RDS) [1] | JIT mac.l, mac.w and saturation. |
| JIT FSCHG FAST | OFF (LEGACY) [0], ON (SZ-ONLY PATH) [1] | ON (SZ-ONLY PATH) [1] | fschg: one store, no FPU spill and call. |
| JIT CR0 BRANCH | OFF (LEGACY) [0], ON (NO MFCR) [1] | ON (NO MFCR) [1] | cmp+bt branches on CR0 and skips mfcr. |
| BLOCK COPY | OFF (PER-WORD) [0], ON (BULK COPY) [1] | ON (BULK COPY) [1] | Store queue and DMA without a call per word (+2%). |
| JIT FSQRT | OFF (LIBM) [0], ON (INLINE NR) [1] | ON (INLINE NR) [1] | frsqrte plus Newton inline, no C call. |
| JIT FIPR DP | OFF (SINGLE) [0], ON (DOUBLE SUM) [1] | ON (DOUBLE SUM) [1] | fipr and ftrv round once, like the hardware (accuracy). |
| VRAM32 FAST | OFF (C CALL) [0], ON (INLINE) [1] | ON (INLINE) [1] | VRAM and sound RAM stores inline, no C call. |
| SQ FLUSH FAST | OFF (C CALL) [0], ON (INLINE) [1] | ON (INLINE) [1] | Store-queue flushes to memory inline, no C call. |

## RECOMPILER 2

| Option | Values [preset] | Default | What it does |
| --- | --- | --- | --- |
| JIT COLD ENTRY | OFF (LEGACY) [0], ON (AFTER BLOCK) [1] | ON (AFTER BLOCK) [1] | Timeslice exit after the block: no taken branch at each entry, 12 B less. |
| SOFT FLOAT | EMULATED (LEGACY) [0], NATIVE [1] | NATIVE [1] | The game's software double maths (divide, multiply) run natively. Same results. |
| JIT FR SWAP | C CALL (LEGACY) [0], INLINE [1] | INLINE [1] | Float register bank swap done in the recompiled code instead of a C call (~2000 a night frame). |
| GAME COPIES | EMULATED (OLD) [0], NATIVE [1] | NATIVE [1] | The game's word and display-list copy loops run natively. Same results, less CPU. |
| INT DIVIDE | EMULATED (OLD) [0], NATIVE [1] | NATIVE [1] | The game's integer divisions (done on its FPU) run natively. Same results. |
| JIT FPU BANKS | SWAP (OLD) [0], RENAME [1] | RENAME [1] | Float bank switches renamed, not swapped, in the recompiled code (matrix maths). |

## FIXES

Fixes from nullDC4Wii (ported from the PS3 port, flycastPS3). Logs say when they fire.

| Option | Values [preset] | Default | What it does |
| --- | --- | --- | --- |
| ARM7 STR ALIGN | OFF (LEGACY) [0], ON (ALIGNED WORD) [1] | ON (ALIGNED WORD) [1] | Sound CPU: an unaligned word store writes the aligned word (ChuChu echo fix). |
| ADPCM LOOP | OFF (LEGACY) [0], ON (FLYCAST) [1] | ON (FLYCAST) [1] | Looped ADPCM sounds keep their loop-start decoder state (SA2 fix). |
| ARM7 RESET FIX | OFF (LEGACY) [0], ON (FLYCAST) [1] | ON (FLYCAST) [1] | Sound CPU reset enters SVC mode and clears its pending FIQ. |
| AICA RACE FIX | OFF (LEGACY) [0], ON (FLYCAST) [1] | ON (FLYCAST) [1] | Sound DMA and IRQ handshake rules as Flycast (Looney Tunes fix). |
| AICA CA CLAMP | OFF (DEFAULT) [0], ON (GAME HACK) [1] | OFF (DEFAULT) [0] | Game hack: sound position reads stop 3 samples short of the loop end. |
| JIT LINK GUARD | OFF (LEGACY) [0], ON (CHECK SITE) [1] | ON (CHECK SITE) [1] | Block links patch only a live exit (freeze guard after a code cache clear). |
| JIT DIV32 FIX | OFF (LEGACY 32) [0], ON (64/32) [1] | ON (64/32) [1] | Matched divide sequences give the SH-4's 64/32 result. |
| INTERP FIX | OFF (LEGACY) [0], ON (MANUAL) [1] | ON (MANUAL) [1] | Interpreter div1, addv, subv, mul.l, mac as the SH-4 manual. |
| SPG FIX | OFF (LEGACY) [0], ON (FLYCAST) [1] | ON (FLYCAST) [1] | HBlank IRQ follows SPG_HBLANK_INT, field bit once a frame. |
| TA GUARD | OFF (LEGACY) [0], ON (SKIP) [1] | ON (SKIP) [1] | Bad TA records are skipped instead of hanging. |
| INF VTX CUT | OFF (LEGACY) [0], ON (CUT) [1] | ON (CUT) [1] | Vertices with 1/W = +inf are cut out of their strips (no spikes). |

## LOGS

| Option | Values [preset] | Default | What it does |
| --- | --- | --- | --- |
| PROFILER | OFF [0], ON [1] | OFF [0] | Logs where each frame's time goes (sh4, elan, rend, arm, aica...). |
| JIT CCALLS | OFF [0], ON (CENSUS) [1] | OFF [0] | With PROFILER: the SH-4 code's calls into C per second. Cheap. |
| JIT HOTBLOCKS | OFF [0], ON (DIAGNOSTIC) [1] | OFF [0] | With PROFILER: hottest SH-4 blocks and their code. About 12% slower. |
| AUDIO STATS | OFF [0], ON [1] | OFF [0] | Logs `[WiiAudio]` once a second: blocks played, underruns, waits. |
| PC SAMPLER | OFF [0], ON [1] | OFF [0] | With PROFILER: where the CPU time goes, by function and SH-4 block. About 1% slower. |
