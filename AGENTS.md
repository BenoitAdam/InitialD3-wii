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

### Status (2026-11-05)

- Boots BIOS-free, menus, attract, car select and races all work on a real Wii.
- Menus/attract/car select: 100% speed. Race: ~45-55 fps with every frame drawn
  (FRAMESKIPPING 0). FRAMESKIP AUTO holds 100% but shows only ~27-30 fps in a race: a dropped
  frame still costs ~12-13 ms (game code + sound), a drawn one ~20-21, and AUTO settles on
  d = (16.7 - S) / (D - S) drawn. The owner finds that unplayable and wants 100% at ~45 fps: every
  ms off a drawn frame helps (OPTIMIZATION.md, "The target").
- Last real-Wii log analysed: **log 19** (2026-11-05): PAGE GUARD exact and stable (owner
  committed it) but small: page checks ~0.15 -> 0.055 ms a render. Race 41-44 fps in its heavy
  stretch, 45-51 at ~80K vertices; the owner set SOUND CLOCK REAL TIME, which makes sound cost
  ~1.25x per frame below 100% speed (`arm` + `aica` 2.7-2.85 ms). Built after it, **awaiting log
  20**, from the owner's requests: MIRROR (RENDERING, default OFF: the in-car view's rear-view
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
| `port/src/game/core_options.h` | Launcher option table (6 pages: GENERAL, RENDERING, RECOMPILER, RECOMPILER 2, AUDIO, FIXES; a page that fills up gets a second one) |
| `port/src/game/game_runner.cpp` | Runs the core on the Wii: options -> `g_*_preset` globals (array in `CoreOptionId` order, static_assert), pads -> JVS, `[prof]` blocks, async log |
| `port/src/ui/launcher.cpp` | Menus; `options.txt` saved as `LABEL=VALUE`, matched by label |
| `port/src/core/log.cpp` | Log file, async ring mode (`log::setAsync`) |
| `port/src/core/dimm_image.cpp` | Decrypted reads of the game image from the CHD: run cache, stream read-ahead (Wii: thread under the emulation's priority, given 1 ms slices at vblank by `naomi_CartSlice`) |
| `port/src/run/run_main.cpp` | Windows headless runner `idas3wii_run` (all diagnostics flags) |
| `port/emu/naomi/naomi_frontend.cpp` | Core init/run/settings, idle-loop skip (`naomi_IdleCycleScale`), assert/BIOS-entry traps |
| `port/emu/naomi/naomi_board.cpp` | DIMM board, cart DMA, register counters |
| `port/emu/naomi/naomi_softfloat.cpp` | SOFT FLOAT: the game's software double divide/multiply (GCC soft-float, SH-4 integer code) run natively, fingerprinted; called by the JIT at the routines' entry |
| `port/emu/naomi/naomi_elan*.cpp` | ELAN: `_cmd` command walker (chained lists, model skip, user clip), `_tnl` CPU T&L + depth measure/pin + GPU light sets (point lights as GX local lights), `_gx` GPU T&L path, `_fetch.inc` GPU vertex-fetch programs (+ each list's bounding box, `cachedBox`) |
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
| `--press F:name,F:name,...` | press a button at frame F, e.g. `5000:coin,5300:start` (coin, start, view, up, down, left, right, gas, brake, test, service) |
| `--gpu-tnl` | take the lists the Wii GPU path would take; runs the **GPU fetch check** (`[run] GPU fetch check`: program triangles vs the CPU draw; must stay at 0 mismatches) |
| `--frameskip N`, `--model-skip 0/1`, `--fixes on/off` | same as the launcher options, for A/B runs |
| `--point-lights 0/1` | NIGHT LIGHTS: 1 = point lights as GX local lights; without `--gpu-tnl` the CPU draws those lists lit the way GX would (snapshot preview) |
| `--cull 0/1/2` | BACKFACE CULL in the snapshots: 2 = the polygons' cull modes (launcher ON), 1 = the other winding |
| `--list-cull 0/1/2/3` | 3D LIST CULL with `--gpu-tnl`: 1 = behind the camera, 2 = out of view, 3 = also whole models (launcher default). Prints `3D list cull per frame` and `3D list boxes ... N STALE` (every box used is checked against its list's records, every model stepped over against its block: must stay at 0) and `record digest` (all that the renderer is handed: for a refactor of the recording side, two runs at the same value must match line for line; run them one at a time, four at once lost one) |
| env `IDAS3_GX_DEPTH=N:F:rules;...` | each snapshot redrawn with the Wii's 24-bit Z (near N, far F; N 0 = exact) and other depth rules, `_diffK.png` + winning lists of the differing pixels; a config against itself with `j0.5` = pixels that flicker (rules in `soft_renderer.cpp`, `GxDepth`). Race: `0:1:geq+cull;0.00999:100100:geq+cull;0.00999:100100:geq+cull+j0.5+r1` |
| `--histo-from F` | PC histogram from frame F, top 64 ranges of 64 bytes (best tool when stuck without a hang); the wait-for-frame loop 0C20C840 + 0C211880 is ~half of it here (the runner has no idle skip). Env `IDAS3_HISTO_FILE=path` writes every bucket, to compare two parts of the game |
| `--disasm A:B[,C:D]` | SH-4 code of those hex ranges at the end of the run (opcode + format string; run past ~frame 4600 for the game's code) |
| `--work-from F[:T]` | a `[work]` line per frame from F: SH-4 instructions outside the wait loop (what a Wii frame really has to run), the reflection routine, the copy loop, CHD hunks decoded. With T, frames over T instructions and the others get a PC histogram each, heaviest difference first |
| `--trace-from PC`, `--trace-at F`, `--trace-pc a,b`, `--watch ADDR`, `--path-after-elan` | SH-4 tracing (`--trace-at`: arm at frame F; 40 hits per address) |
| `--dimm-ahead 0/1` | DIMM AHEAD (default 0 here, the launcher's is on): run cache + stream read-ahead; on Windows the decode is done on the spot |
| `--reflection N` | REFLECTIONS: one reflection map build in N (default every one, the launcher's is 4) |
| `--soft-float 0/1/2` | SOFT FLOAT (default 0 here, the launcher's is NATIVE): 1 = the game's double divide/multiply run natively, 2 = the game's code runs and every result is checked against the native one (`SOFT FLOAT ... MISMATCHES` must stay 0). `--soft-float-test N`: N random pairs against the host's doubles, no CHD needed |
| env `IDAS3_SF_PROBE=LO:HI[:FROM]` | names the routines called into that code range by trying native double operations on their r4-r7 and r0/r1 (how the soft-float routines were found) |
| `--depth-plane 0/1` | DEPTH MEASURE: 1 = RACE PLANE, frames with no pin keep the race's near plane |
| env `IDAS3_NEAR_TRACE=1` | a `[near]` line per render: pin, frame near plane, vertices measured and why |
| env `IDAS3_CART_TRACE=1` | a `[cart]` line per read of the game image (offset, length) |
| `--mirror 0/1` | MIRROR: 0 = models clipped inside the user clip rectangle (the in-car view's rear-view mirror) not drawn; `MIRROR OFF per frame` line. The runner's route is in the in-car view only between the `view` presses 9950 and 10550 |
| `--small N` | SMALL 3D with `--gpu-tnl`: lists and whole models under N pixels on screen not drawn; `SMALL 3D per frame` line. Snapshots cannot show it (GPU lists are not drawn in them) |
| env `IDAS3_FAR_CENSUS=1` | drawn GPU-path vertices and lists by their box's nearest depth and by its size on screen |
| env `IDAS3_AICA_KEYS=FROM:TO` | a `[keyon]` line per sound channel key-on in those frames: format, loop, address, loop points, pitch, levels and the sample fingerprint SOUND LEVELS matches. The exit line `SOUND LEVELS` counts the key-ons recognised |
| env `IDAS3_SNAP_FROM=F` | periodic snapshots only from frame F (with `--snap-every`) |
| `--area1-census` | who writes the 32-bit VRAM interface |
| `--dump-ram/-vram/-aica/-elan FILE` | memory dumps at the end (never commit them) |
| env `IDAS3_RENDER_TRACE=FROM:TO` | print each render's FB registers |
| env `IDAS3_FETCH_SCATTER=%` | % of pages put in random frames for the fetch check (default 33) |

The runner uses the **compiled-in preset defaults**, not the launcher's (e.g. CPU T&L unless
`--gpu-tnl`). Reference timeline at `--cpi 2`: first renders ~frame 1000, WARNING ~2400, SEGA
ROSSO ~3500, title ~4100, attract manufacturer screen ~13000. Race route: coin at 5000, then
start every 300 frames up to 11000; `view` presses at 9950, 10250 and 10550 cycle the camera
(in-car view from ~10300). That route stops before the player gets control (the scene freezes
from ~11700): to drive, keep pressing start up to 13100 and `gas` every 10 frames from 11500 (a
press lasts 10 frames); the car then races in the in-car view (no steering: it ends up against
the guardrail), and `view` presses during it (e.g. 12000, 12500) cycle the cameras.

**Real Wii (the reference):** the owner runs the build with SPEED OVERLAY + PROFILER and sends
`idas3wii_log.txt` (numbered "REAL WII LOG n"). Hand them a short list of what to test and which
log lines to read. Do not trust Dolphin for speed: it models no cache misses (on hardware the
SH-4 side costs ~2.3x and ELAN command walking ~3x the Dolphin time), ignores PPC page-table
read-only (PP) bits, needs its MMU option on, and the owner's Dolphin is overclocked.

Reading the log: `FPROF` per-frame buckets (`sh4`, `rend`, `elan`, `arm`, `aica`, `tex`, `mem`;
on NAOMI 2 the `elan` column is the old disc bucket and `rd=` counts ELAN command executions);
`[prof]` 120-frame blocks; `[HW]` Broadway PMC lines (IPC, L1 miss cycles, DTLB, L2, render
probe, ELAN draw split, fetch programs/patches/ring passes, ELAN page checks: count, time, PAGE
GUARD's read-only pages); `[CC]` JIT call-outs into C
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
- After installing PPC exception handlers put a `sync` + memory clobber: GCC moved the inline
  store past a volatile guest read once.
- The dynarec pins SH-4 GPRs in r14..r28: `Sh4cntx.r[]` read from C (e.g. in a trap) can be stale.
- Wii memory: newlib malloc never returns memory, and the guest page pool only takes what is
  above the heap's high-water mark. Never allocate big temporary buffers in the launcher; stream
  files (the BIOS zip is streamed, only 512 KB kept).
- Nothing in the core may grow on the heap during the game: a few hundred KB are left beside
  the page pool, and with `-fno-exceptions` a failed `new` is `abort()`. A path that a day race
  never takes can still be hit later: the CPU T&L vectors stayed empty in races (the GPU path
  takes every list) and grew to ~1.5 MB in the night attract demo. Use fixed buffers.
- The GX renderer's buffers were sized for Dreamcast: ELAN output is chunked (<=1024 vertices a
  header) and the NAOMI 2 caps raised (`GX_VERTEX_CAP`, `GX_LIST_CAP`).
- Writing the log with fflush+fsync per line cost ~10 ms spikes: keep log writes going through
  the async ring during the game.
- Text on screen is 8x16 sprites whose 13th vertex word repeats the character code (handy to
  read menus from dumps).
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
- The menus are driven with the **steering wheel** ("Steering SELECT, Accel OK"): the runner's
  `left`/`right` move the cursor, `up`/`down` are the shifter. To find a sound, compare the
  `IDAS3_AICA_KEYS` key-ons of a run with presses against one without: menu music keys on notes
  all the time, and two runs drift by a frame. Race sounds (engines, road, tire squeal) are long
  loops keyed on once and then changed in pitch and level, not repeated notes. Sound RAM is
  byte-swapped per word on the Wii: read guest bytes through `AICA_LANE8`.

## Open issues (besides speed)

- Specular is dropped on the GPU T&L path (visible on untextured lists, and at night on
  bumpers, grilles and the road's glints, where the headlights make it).
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
