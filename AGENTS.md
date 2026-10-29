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

### Status (2026-10-29)

- Boots BIOS-free, menus, attract, car select and races all work on a real Wii.
- Menus/attract/car select: 100% speed. Race: ~38-41 fps with every frame drawn
  (FRAMESKIPPING 0); ~94-103% speed with FRAMESKIP AUTO (1 frame in 4 drawn).
- Last real-Wii log analysed: **log 11** (rear-view mirror / in-car view fixed, depth-spread
  bug fixed). **Awaiting log 12** from the owner. Next speed idea: per-list model-space AABB
  (see OPTIMIZATION.md).
- Music tempo follows emulation speed (the game streams it through the SH-4), so music is
  right only at 100%.
- Attract mode (no coin, ~20 s after the title) runs a **night** demo race: 6-11 point lights
  per list. Its crash on the Wii is fixed (CPU T&L fixed buffers; owner: "demo works now",
  2026-10-29) but it ran at a few fps, every list lit on the CPU. NIGHT LIGHTS = GPU (APPROX)
  (RENDERING page, default) now lights those lists with GX local lights: the whole demo goes
  to the GPU path on Windows (`--gpu-tnl`). Awaiting its speed and look on a Wii (log 12).

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
| `port/src/game/core_options.h` | Launcher option table (5 pages: GENERAL, RENDERING, RECOMPILER, AUDIO, FIXES) |
| `port/src/game/game_runner.cpp` | Runs the core on the Wii: options -> `g_*_preset` globals (array in `CoreOptionId` order, static_assert), pads -> JVS, `[prof]` blocks, async log |
| `port/src/ui/launcher.cpp` | Menus; `options.txt` saved as `LABEL=VALUE`, matched by label |
| `port/src/core/log.cpp` | Log file, async ring mode (`log::setAsync`) |
| `port/src/run/run_main.cpp` | Windows headless runner `idas3wii_run` (all diagnostics flags) |
| `port/emu/naomi/naomi_frontend.cpp` | Core init/run/settings, idle-loop skip (`naomi_IdleCycleScale`), assert/BIOS-entry traps |
| `port/emu/naomi/naomi_board.cpp` | DIMM board, cart DMA, register counters |
| `port/emu/naomi/naomi_elan*.cpp` | ELAN: `_cmd` command walker (chained lists, model skip, user clip), `_tnl` CPU T&L + depth measure/pin + GPU light sets (point lights as GX local lights), `_gx` GPU T&L path, `_fetch.inc` GPU vertex-fetch programs |
| `port/emu/plugs/drkPvr/gxRend.cpp` | PowerVR -> GX renderer (TA lists, ELAN items, VRAM present, tile clip, buffer caps) |
| `port/emu/plugs/drkPvr/frame_prof.cpp` | FPROF per-frame profiler |
| `port/emu/host/soft_renderer.cpp` | Windows software renderer for the runner (snapshots, stand-ins for the `rend_*` hooks) |
| `port/emu/wii/wii_fastmem.cpp` | Guest memory: MMU demand paging, zero page, compressed page store, GPU-fetch page validation |
| `port/emu/wii/dc/sh4/rec_v2/wii_driver.cpp` | SH-4 -> PPC dynarec back-end and fastmem trampolines (see `WII_DRIVER_MANUAL.md` next to it) |
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
| `--histo-from F` | PC histogram from frame F (best tool when stuck without a hang) |
| `--trace-from PC`, `--trace-pc a,b`, `--watch ADDR`, `--path-after-elan` | SH-4 tracing |
| `--area1-census` | who writes the 32-bit VRAM interface |
| `--dump-ram/-vram/-aica/-elan FILE` | memory dumps at the end (never commit them) |
| env `IDAS3_RENDER_TRACE=FROM:TO` | print each render's FB registers |
| env `IDAS3_FETCH_SCATTER=%` | % of pages put in random frames for the fetch check (default 33) |

The runner uses the **compiled-in preset defaults**, not the launcher's (e.g. CPU T&L unless
`--gpu-tnl`). Reference timeline at `--cpi 2`: first renders ~frame 1000, WARNING ~2400, SEGA
ROSSO ~3500, title ~4100, attract manufacturer screen ~13000. Race route: coin at 5000, then
start every 300 frames up to 11000; `view` presses at 9950, 10250 and 10550 cycle the camera
(in-car view from ~10300).

**Real Wii (the reference):** the owner runs the build with SPEED OVERLAY + PROFILER and sends
`idas3wii_log.txt` (numbered "REAL WII LOG n"). Hand them a short list of what to test and which
log lines to read. Do not trust Dolphin for speed: it models no cache misses (on hardware the
SH-4 side costs ~2.3x and ELAN command walking ~3x the Dolphin time), ignores PPC page-table
read-only (PP) bits, needs its MMU option on, and the owner's Dolphin is overclocked.

Reading the log: `FPROF` per-frame buckets (`sh4`, `rend`, `elan`, `arm`, `aica`, `tex`, `mem`;
on NAOMI 2 the `elan` column is the old disc bucket and `rd=` counts ELAN command executions);
`[prof]` 120-frame blocks; `[HW]` Broadway PMC lines (IPC, L1 miss cycles, DTLB, L2, render
probe, ELAN draw split, fetch programs/patches/ring passes); `[CC]` JIT call-outs into C
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
  in(5)=0xF9 is the safe value.
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
- Dolphin ignores PP bits: anything relying on a write fault to a read-only page writes through.
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

## Open issues (besides speed)

- Specular is dropped on the GPU T&L path (visible on untextured lists, and at night on
  bumpers, grilles and the road's glints, where the headlights make it).
- Night scenes with NIGHT LIGHTS = GPU: point lights are GX local lights (`gxPointLight` in
  `naomi_elan_tnl.cpp`): fitted distance attenuation (dimmer core, faint tail past the reach),
  parabola for saturating spots, duplicates merged, lights out of a list's reach left out, at
  most 8. Unverified on hardware: GX attenuation above 1 (merged headlights) and the
  spot-direction sign (from libogc's disassembly: `GX_InitLightDir` negates). Each point-lit
  list is measured every render (box for the light cull): ~55K vertex reads a night frame.
  NIGHT LIGHTS = CPU keeps the exact CPU path (a few fps at night on a Wii).
- Modifier volumes, the credit-digit glyph, in-race memory headroom, a live window + keyboard
  for the Windows build.
- Windows build has no audio; sound changes can only be heard on Wii/Dolphin.
