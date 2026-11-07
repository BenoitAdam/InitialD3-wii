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

- **2 PLAYERS (branch `two-player-splitscreen`)**: launcher GENERAL page, last row, OFF /
  SIDE BY SIDE. Split screen in races: the game still draws one view a frame; the cars take
  turns being the one its in-car camera follows (the rig's target car, `naomi_2p.cpp`), and
  the renderer squeezes each view into its car's half and keeps the other half (gxRend
  `split2p_*`: the half's viewport and scissor, each view clears its own half when it starts,
  two half EFB copies that clear nothing). 30 views a second each, no more drawing than one
  player. Each view gets half the picture's aspect (the camera's per-frame projection), the
  mirror is off. The second pad (GameCube port 2 / Wiimote 2) drives the CPU car: it is a
  kinematic AI body (heading + speed), so player 2 sets its speed (the AI's braking caps it in
  bends) and nudges its heading (the AI's pull back to its line balances it: lane control, it
  cannot leave the road). First Wii test (owner, 2026-11-07): works, but (1) flickering
  sometimes, (2) player 2's speedometer showed player 1's speed, (3) a GameCube pad on port 2
  did not answer, a Wiimote 2 did. Fixed after it, **awaiting the owner's second test**: (1) a
  render shows the scene built in the game's previous frame from the camera of the frame
  before (runner timeline), so the car changes at every STARTRENDER and a render is labelled
  with the car of two STARTRENDERs before; the old code held the car back for a dropped frame,
  which put the next two views in the wrong halves; frameskip now decides for a pair of views
  (`naomi_2pSkip`); a half no longer goes blank when a view comes twice. (2) The HUD of the CPU
  car's view gets its speed (JIT block hook at 0C0C9BC8, `naomi_2pHud`). (3) Not found in the
  code (PAD_ScanPads' mask checked in libogc): the log now has a `[game] controllers` line
  (GameCube ports, Wiimotes and their expansions) whenever they change. Known limits: the
  rev counter, gear and ADVANTAGE in player 2's HUD are player 1's; player 2's car is "on
  rails" (no slides, no crashes); player 2's view shows its own bonnet (the CPU car is drawn
  whole). Game addresses and how they were found: top of `port/emu/naomi/naomi_2p.cpp`.
- Boots BIOS-free, menus, attract, car select and races all work on a real Wii.
- Menus/attract/car select: 100% speed. Race, every frame drawn (FRAMESKIPPING 0, MIRROR OFF,
  SMALL 3D 4 px): the day course of logs 21/23 at **100% in the in-car view** (log 25: 59.4-60.1
  fps the whole race, p50 15.5-16.3 ms) and 52-56 fps from the chase camera (log 23, before COLOUR
  LISTS); night Akina 49-54 fps (p50 18-20). FRAMESKIP AUTO settles on d = (16.7 - S) / (D - S)
  drawn (S a dropped frame, ~13.4 ms at night: `sh4` 9.5, sound ~2.3, ELAN walk ~1.2; D a drawn
  one, ~19-20 at night). The owner wants 100% at ~45 fps shown: by day it holds (in-car every frame
  drawn), at night log 25 gives 100% with ~36 shown (29-46); 45 needs ~1.3 ms off both S and D, or
  ~1.7 off D alone (OPTIMIZATION.md, "The target").
- Last real-Wii log analysed: **log 25** (2026-11-05, two runs). Day course, every frame drawn,
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
  ns a command).
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
| `port/src/game/core_options.h` | Launcher option table (7 pages: GENERAL, RENDERING, RENDERING 2, RECOMPILER, RECOMPILER 2, AUDIO, FIXES; a page that fills up gets a second one, ~20 rows fit) |
| `port/src/game/game_runner.cpp` | Runs the core on the Wii: options -> `g_*_preset` globals (array in `CoreOptionId` order, static_assert), pads -> JVS, `[prof]` blocks, async log |
| `port/src/ui/launcher.cpp` | Menus; `options.txt` saved as `LABEL=VALUE`, matched by label |
| `port/src/core/log.cpp` | Log file, async ring mode (`log::setAsync`) |
| `port/src/core/dimm_image.cpp` | Decrypted reads of the game image from the CHD: run cache, stream read-ahead (Wii: thread under the emulation's priority, given 1 ms slices at vblank by `naomi_CartSlice`) |
| `port/src/run/run_main.cpp` | Windows headless runner `idas3wii_run` (all diagnostics flags) |
| `port/emu/naomi/naomi_frontend.cpp` | Core init/run/settings, idle-loop skip (`naomi_IdleCycleScale`), assert/BIOS-entry traps |
| `port/emu/naomi/naomi_board.cpp` | DIMM board, cart DMA, register counters |
| `port/emu/naomi/naomi_softfloat.cpp` | SOFT FLOAT: the game's software double divide/multiply (GCC soft-float, SH-4 integer code) run natively, fingerprinted; called by the JIT at the routines' entry |
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
| `--point-lights 0/1` | NIGHT LIGHTS: 1 = point lights as GX local lights; without `--gpu-tnl` the CPU draws those lists lit the way GX would (snapshot preview). With `--gpu-tnl`: `point-lit light sets per frame` line, every reused set rebuilt and compared (`STALE` must stay 0). The night attract demo (no presses, 9000 frames) is not deterministic on Windows past render ~5300: compare `record digest` lines only up to where two runs of the same code agree |
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
| env `IDAS3_SF_PROBE=LO:HI[:FROM]` | names the routines called into that code range by trying native double operations on their r4-r7 and r0/r1 (how the soft-float routines were found) |
| `--depth-plane 0/1` | DEPTH MEASURE: 1 = RACE PLANE, frames with no pin keep the race's near plane |
| env `IDAS3_NEAR_TRACE=1` | a `[near]` line per render: pin, frame near plane, vertices measured and why |
| env `IDAS3_CART_TRACE=1` | a `[cart]` line per read of the game image (offset, length) |
| `--incar-reflect N` | IN-CAR REFLECT (default 0 here = as REFLECTIONS, the launcher's is 16): while the in-car view's mirror models are on screen, one reflection map build in N, -1 = none; an `IN-CAR REFLECT per 60 frames` line and an exit line count the builds not made. The race starts in the in-car view; `view` toggles it with the chase view |
| `--colour-fetch 0/1` | COLOUR LISTS with `--gpu-tnl` (default 0 here, the launcher's is GPU FETCH): colour lists get fetch programs (so boxes) and are drawn from them when TSP UseAlpha is 0 or every record's alpha is 255 (the Wii also takes gxRend's other forced-opaque cases); the `GPU fetch per drawn render` line shows "colour lists N (M more fetched)" and they join the fetch, display-list and box checks |
| `--display-lists 0/1` | DISPLAY LISTS with `--gpu-tnl` (default 0 here, the launcher's is ON): every cached list's display-list entry is built as on the Wii, its bytes read back as the GPU would and compared with the CPU draw. `DISPLAY LISTS per drawn render` line: MISMATCHES (only where the GPU fetch check has them too) and HAZARDS (an entry written over one still queued) must stay 0; also the ring bytes the drawn entries take, ring passes and GPU waits. Env `IDAS3_DL_RING_KB`: ring size (the Wii's is 1024) |
| `--mirror 0/1` | MIRROR: 0 = models clipped inside the user clip rectangle (the in-car view's rear-view mirror) not drawn; `MIRROR OFF per frame` line. The race starts in the in-car view (~9350) and each `view` press toggles in-car/chase: with presses at 9950, 10250, 10550 the route is in-car at 9350-9950 and 10300-10550 |
| `--small N` | SMALL 3D with `--gpu-tnl`: lists and whole models under N pixels on screen not drawn; `SMALL 3D per frame` line. Snapshots cannot show it (GPU lists are not drawn in them) |
| env `IDAS3_FAR_CENSUS=1` | drawn GPU-path vertices and lists by their box's nearest depth and by its size on screen |
| env `IDAS3_AICA_KEYS=FROM:TO` | a `[keyon]` line per sound channel key-on in those frames: format, loop, address, loop points, pitch, levels and the sample fingerprint SOUND LEVELS matches. The exit line `SOUND LEVELS` counts the key-ons recognised |
| env `IDAS3_SNAP_FROM=F` | periodic snapshots only from frame F (with `--snap-every`) |
| `--area1-census` | who writes the 32-bit VRAM interface |
| env `IDAS3_ACCESS=LO:HI[:FROM[:TO]][;...]` | every SH-4 read and write into those guest ranges in those frames, by instruction address and caller (pr), with first/last values, printed at exit: which code uses a game variable (how 2 PLAYERS' addresses were found; the run is ~2x slower) |
| env `IDAS3_TIMELINE=FROM:TO:ADDR[-END][,...]` | every SH-4 access to those guest words or ranges (up to 8) in those frames printed as it happens (value, pc, pr, ELAN executions so far), with the vblanks and the renders (half of a 2 PLAYERS render) between them: the order of events in the game's frame (how the 2 PLAYERS render latency was found) |
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
- Sound with SOUND CLOCK REAL TIME is produced from real elapsed time (`armUpdateARM`, stalls
  over 50 ms not caught up), so nothing may block on the audio ring in that mode: a block there
  turns into more samples (log 24: 25% of the time in `snd`, AUTO at 80% speed). `snd` in FPROF is
  that wait (`SndWaitTicks`); AUDIO STATS ON logs the ring's underruns, drops and waits.
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
