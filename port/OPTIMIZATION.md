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

**FRAMESKIP AUTO's arithmetic.** AUTO drops renders only while the game is behind real time
(`spg_PaceErrorSec`, an absolute schedule), so it settles on the share of drawn frames that holds
100%: d = (16.7 - S) / (D - S), with D a drawn race frame and S a dropped one. A dropped frame
still runs the game's code and the sound (`sh4` ~9, sound ~2.2 at 100%, the ELAN walk of what is
not a model): S ~12-13 ms. With D ~20-21 ms that is d ~0.45, ~27 fps shown, which the owner read
as "a 30 fps limit" (log 19). It is no cap: every ms off D adds ~6-8 shown fps under AUTO, every ms
off S more. 45 fps shown at 100% needs D ~17.5 ms with today's S.

Log 25 (2026-11-05, two runs, MIRROR OFF, SMALL 3D 4 px, REAL TIME sound, in-car view all race):
- **Day course, every frame drawn** (logs 21/23's course; its race intro at frame 5280 here, 5520
  in log 23): **100% the whole race**, 59.4-60.1 emulated fps over frames 5520-8040, p50 15.46-16.34
  (log 23 in-car: 57.5-60.9 fps, p50 16.45-16.84), p99 17.7-18.6. COLOUR LISTS works: "CPU-fed
  (colour, other)" 1.1-1.6K a render -> 0, "the rest" of the ELAN draw 2.9-4.3 -> 2.0-2.1 us a list
  (day lists now cost what night ones do), a render's CPU at equal lists 1.83 -> 1.43 ms (344
  lists), 1.68 -> 1.34 (312), 1.61 -> 1.19 (240): -0.35 to -0.42 ms, `rend` 1.55-2.40 -> 1.02-1.78.
  Owner: no colour problem. Rest of the frame as log 23: `sh4` 7.7-8.3, `elan` 3.2-3.7 (walk
  2.2-2.6, recording 1.0-1.6, 2.4-2.7 us a box), `arm` 1.35, `aica` 1.0-1.3. The chase camera
  (log 23: 51.8-55.9 fps with 2.1-2.3K colour vertices) was not driven.
- **Night Akina, FRAMESKIP AUTO, AUDIO STATS ON**: the audio fix works. `snd` 0.0% all run, the
  ring at 1-3 blocks of 4, 0 drops and 0 waits; 3 underruns in all, at boot and loading screens,
  none in the race. 59.6-59.9 emulated fps from the race start (overlay 99.7-100.2%), 29-46 renders
  shown a second, ~36 on average (log 20, MIRROR OFF + 8 px: ~31; log 21, MIRROR ON: ~25; log 24:
  ~12), 57-77% drawn, p50 16.85-17.2: the frame budget fully used, as AUTO should. Typical frames
  (mostly dropped): `sh4` 9.4-9.5, `arm` 1.3, `aica` 1.0, `elan` 1.5-2.2, `rend` 0.8-1.0; tail
  (drawn): `elan` 5.2-5.8, `rend` 1.5-2.6. ELAN commands average 1.85-2.07 ms a frame at ~60%
  drawn against ~2.4 on a drawn frame, so a **dropped frame still walks ~1.2 ms of commands** (a
  dropped frame of frames 6960-7080: ~490 models stepped over from their summaries, ~1600 polygon
  lists not walked, ~190 walked and not drawn, ~1650 state commands not decoded). So S ~13.4 (`sh4` 9.5, sound 2.3, walk 1.2, the rest ~0.4) and D ~19-20.
- **Night `sh4` is ~1.3 ms over day's** (9.4-9.5 against 7.7-8.3), drawn and dropped frames alike,
  in every log since 20: per-frame game work, not rendering.
- What 45 shown at night takes: d = 0.75, so 0.75 D + 0.25 S <= 16.7: ~1.7 ms off D alone, or
  ~1.3 ms off both (game code, sound, the dropped part of the walk).

Log 24 (2026-11-05, night Akina uphill, two runs, MIRROR OFF, SMALL 3D 4 px, REAL TIME sound):
- **Every frame drawn**: as log 23, as expected (COLOUR LISTS only matters by day): 48.2-54.1
  emulated fps, p50 17.7-20.1, recording 2.9-3.6 us a list with a box, 8-64 light sets built a frame
  and 208-425 reused. Akina's ~24 CPU-fed colour vertices a render went to 0; nothing wrong seen.
- **FRAMESKIP AUTO**: 48.5-54.4 emulated fps (80-88%) and only 11.7-13.9 renders shown a second (AUTO
  at its 1-in-4 floor, being behind), where log 20 held 100% with 43-61% drawn. Cause: `snd`, the
  audio ring's pacing wait, 24-27% of the time from the race start (tail frames 20-29 ms in it,
  typical frame 0.3; 0 in logs 20/21 AUTO and in this log's every-frame run). With SOUND CLOCK
  REAL TIME the samples come from real elapsed time (stalls over 50 ms dropped), so the wait cannot
  pace the emulation; and it feeds itself, since the time spent waiting is turned into samples at
  the next `armUpdateARM`, which fill the ring again. How it gets full: a stall that empties the
  ring makes the callback play concealment without consuming, and the stall's samples are produced
  anyway (under 50 ms): each such stall raises the ring's level by up to a block, and the race start
  has many. Logs 20/21 did not reach the full ring by chance. Fixed after log 24 (below). The
  dropped frames cost S ~13.6 ms here at 85% speed (`sh4` 9.4, ELAN 1.4, `arm` 1.3, `aica` 0.95,
  `rend` 0.4), so with the fix AUTO should give ~35-40 shown at night (D ~19).

Log 23 (2026-11-05, two runs, every frame drawn, MIRROR OFF, SMALL 3D 4 px, REAL TIME sound, the
in-car view most of the time):
- **Night Akina uphill** (log 22's settings): the light-set reuse works, 10-64 sets built a frame
  and 206-410 reused (80-97%). Recording (`elan` T&L "GPU lists") per list with a box: 2.85-3.6 us
  against 3.65-4.5 in log 22; at equal load (~600 boxes, frames 7440 in both) 2.36 -> 1.87 ms, and
  the frame's `elan` 4.79 -> 4.31: **~0.5 ms off a drawn night frame**, the low end of the 0.5-1
  expected. 48.9-54.4 emulated fps, p50 18.1-20.0 (`sh4` 9.25-9.43, `rend` 1.45-2.03, `elan`
  3.98-4.94 of which the command walk 2.14-2.80, `arm` 1.54-1.60, `aica` 1.13-1.25).
- **Day race** (the course of log 21's day run, 146K vertices in): in the in-car view **~100% with
  every frame drawn**: 57.5-60.9 fps, p50 16.45-16.84 (`sh4` 8.3, `rend` 1.55-2.4, `elan` 3.5-3.76 =
  walk 2.2-2.5 + recording 1.16-1.37, `arm` 1.36, `aica` 0.9-1.2). Chase camera 51.8-55.9 fps, p50
  16.8-17.7 (walk 2.0-2.2, recording 1.5-1.96). A render's CPU 1.54-2.27 ms in-car (243-456 lists),
  2.5-2.8 chase (438-504). Day lists cost more to draw than night ones ("the rest" 2.9-4.3 us a list
  against 2.1-2.4): the course's colour lists, 1.1-1.6K CPU-fed vertices a render in-car and
  2.1-2.3K chase, ~0.28 us each, ~0.3-0.65 ms (item 3). Recording by day: 2.35-2.66 us a box.
- Under FRAMESKIP AUTO (S ~11.4-12 ms) this means ~52 shown fps by day (D ~17.5) and ~40 at night
  (D ~19): the day course meets the owner's target, night Akina needs ~1.5 ms more off D.
- The ELAN command walk is now the largest ELAN cost of a drawn frame: 2.0-2.8 ms (log 20: 2.4-2.9),
  as much as the recording at night and twice it by day. In-car it walks ~65 mirror models a frame
  that MIRROR OFF does not draw (only those with a main-view summary are stepped over; see "Tried
  and dropped").
- `[game] TEST button pressed at frame 6840` (night run) at the race start, but the game did not
  enter its test menu (no ELAN reset, the race went on): harmless.

Log 22 (2026-11-05, night Akina uphill, every frame drawn, MIRROR OFF, SMALL 3D 4 px, DISPLAY
LISTS ON; the owner drove backwards at the end): compared with log 20's every-frame run on the same
course (SMALL 3D 8 px there):
- **DISPLAY LISTS work on the Wii.** `cached programs` 0.10-0.28 ms a render for 20-57K vertices
  (~4-5 ns a vertex) against 0.44-1.42 ms (~24). Ring passes 0 and GPU waits 0 over the whole run;
  607 entries (447 KB) written at the race's load, then 0-118 a block. The pool gave up 1 MB (40420
  KB), guest memory still never failed (732 KB free at its lowest, frame 2070). No glitch reported.
- But "the rest" of the ELAN draw went up ~0.4 us a list (2.1-2.4 against 1.8-1.9): render IPC
  0.66-0.73 -> 0.57-0.63 at the same load-miss share, likely the GP now fetching at full speed and
  the CPU's misses waiting longer. Net, a render's CPU at equal lists and vertices: 360 lists / 36K
  vertices 2.04 -> 1.56 ms, ~420 lists / 35-40K 2.3-2.4 -> 1.72, ~620 lists / 54K ~2.9 -> 2.44 to
  2.52: **-0.5 to -0.6 ms a drawn frame**, less than the ~1 ms hoped at night.
- The frame barely moved (45.1-57.0 emulated fps, p50 18.1-21.2, against 43.5-54.7 and 18.2-21.2)
  because this run drew more: SMALL 3D 4 px instead of 8 left 360-640 lists a render against
  220-530, and **at night each recorded list costs ~5-6 us** (`elan` T&L 2.0-3.9 ms a frame, light
  sets), more than its draw. `elan` 4.0-5.65 a frame against 4.2-4.95. That is the next target
  (done after log 22).

Log 21 (2026-11-05, two runs, FRAMESKIP AUTO, SMALL 3D 4 px, IN-CAR REFLECT 1 in 16, in-car view
the whole race, REAL TIME sound, and **MIRROR ON** from the owner's options.txt):
- **Night Akina**, the course and settings of log 20's AUTO run but for MIRROR and IN-CAR REFLECT
  (its race intro, frames 6480 here and 6120 there, has the same vertices and ELAN times). Race
  frames 6720-8640: 17-34 renders shown a second (log 20: 19-48, ~31 on average; here ~25), 37-60%
  drawn, p50 16.4-16.9. A render's CPU 2.3-3.55 ms with 45-64K fetched vertices (log 20 mostly
  1.7-3.0 at 23-35K): the mirror's second pass, ~0.8-1 ms of `rend` and as much recording per drawn
  frame. The GP is still drawing 3.9-6.3 ms after the CPU is done (point lights). MIRROR OFF is
  worth ~6 shown fps at night.
- **IN-CAR REFLECT** skips 112-113 builds of 120 as designed (log 20 built 30). Yet typical `sh4`
  matches log 20 window for window (aligned on the race start, 14 windows: -0.07 to +0.08 ms), where
  ~0.25 was expected. A build must cost the Wii well under the 1.3 ms assumed: log 15, where that
  figure came from, also moved the cart reads out of `sh4`. So turning the reflections off entirely
  would gain <0.1 ms (owner's offer: not taken). The mirror, drawn here and not in log 20, is in
  `rend` and `elan`, not in `sh4`, so it should not hide the difference.
- **Day race** (another course, 146K vertices in, 41-62K fetched): 24-38 renders shown a second
  (~34), 54-69% drawn, p50 16.1-16.8; typical `sh4` 7.8-8.3. A render's CPU 4.3-4.9 ms, of it ELAN
  GPU lists 3.5-4.0 (66-72K vertices): `cached programs` 1.75-1.9 ms (11-12.5K strips) and the
  rest 1.9-2.1 ms, ~0.8 of it 3.0-3.9K CPU-fed colour vertices (item 3; ~30 on Akina). ELAN
  2.7-3.2 ms a frame (commands 1.6-1.9, recording 1.1-1.4). So S ~11.4 ms (`sh4` 8.3, sound 2.0,
  ELAN walk ~0.9) and D ~20.5: d ~0.58, as measured. 45 fps shown there needs D ~18.5.
- Night recording costs more than day: ~3.9 ms of T&L a drawn night frame for ~35K vertices
  against ~2.1 ms for ~53K by day (light sets: 6-11 point lights a list, box tests, merges).
- Owner: "Easy Day race: nothing particular"; at night the rival stays ahead (game difficulty).
  Guest memory nearly full at frame ~2130-2160 in both, as in every log since 16.

Log 20 (2026-11-05, two runs, in-car view the whole race, MIRROR OFF, REAL TIME sound):
- **Every frame drawn, SMALL 3D 8 px** (frames 6600-9480): 43.5-54.7 fps, p50 18.8-21.2 ms, p99
  22-26; `sh4` 9.2-9.6, `rend` 1.55-2.2, `elan` 4.2-4.95 (commands 2.4-2.9, T&L 1.8-3.2), `arm`
  1.43-1.52, `aica` 1.08-1.25. 105-140K vertices in, 37-72K on the GPU. MIRROR OFF steps over
  ~175-185 models a drawn frame (560-690 lists, 42-53K vertices); SMALL 3D drops 3-85 lists. The
  in-car view with its mirror gone draws about like log 19's race at ~80K vertices (p50 19.7-20.6,
  `rend` 2.3-2.6; its camera is not known), so MIRROR OFF mostly bought back what the mirror
  costs. The chase view has no mirror models at all (runner), so MIRROR changes nothing there.
- **FRAMESKIP AUTO, SMALL 3D 4 px**: 59.5-60.2 emulated fps all race, 43-61% of frames drawn
  (26-37 renders shown a second), p50 16.4-17.0, p99 21.4-23.6. Typical (mostly dropped) frames:
  `sh4` 9.0-9.9, `elan` 1.1-1.7, `arm` 1.21-1.31, `aica` 0.80-1.03. SMALL 3D at 4 px drops only
  2-8 lists a frame here. ELAN commands average 1.45-1.85 ms a frame against 2.4-2.9 on a drawn
  one, so a dropped frame still walks ~1 ms of commands (list-end waits, texture DMAs, TA runs and
  the top level must be walked; models are stepped over). So S ~13 ms (`sh4` ~9.5, sound ~2.2 at
  100%, ELAN walk ~1, the rest ~0.3) and D ~19-20: d ~0.55, as measured. Owner: "a little bit
  better but not that much". Every 0.5 ms off S is ~+2.5 shown fps, off D ~+2.
- Owner: MIRROR OFF works; SMALL 3D works, at 8 px trees visibly clip ("not a big deal"); SOUND
  LEVELS work, defaults set to MENU 5, TIRE 4. Both runs: guest memory nearly full at frame 2130
  (624 KB free), then fine; the AUTO run logged one SERVICE press (harmless).

Log 19 (2026-11-05, PAGE GUARD ON, SOUND CLOCK REAL TIME chosen by the owner, otherwise as log 18):
- **PAGE GUARD works and is exact**: a whole race, no glitch, no crash (owner: "more fast and
  stable", committed). But the checks were cheap already. A race render checks 480-690 pages in
  0.051-0.063 ms (91-125 ns each), 94-97% of them read-only. Fitted over the blocks: a guarded
  check ~60 ns, an unguarded clean one ~0.2-0.3 us, a written page ~1.35 us (clear C, `tlbie`,
  store 4 KB of cache). So the old cost was ~0.12-0.17 ms a render and the gain ~0.07-0.1 ms; the
  ~17-22 pages written each render are most of what is left. "Guards dropped" is 0 in a steady
  race and comes in bursts (100-490) at loads and course changes.
- **Race** (driven by the owner, not log 17's line): heavy stretch 11280-11760 at 41-44 fps
  (100-105K vertices in, 75-84K on the GPU, `elan` 6.2-6.7 a frame); steady 12480-13440 at
  45.1-51.3 fps at ~80K in (53-64K on the GPU), p50 19.7-20.6 ms. Typical frame there: `sh4`
  9.0-9.2, `rend` 2.3-2.6, `elan` 4.5-4.8, `arm` 1.55-1.6, `aica` 1.15-1.25.
- **REAL TIME costs sound time below 100% speed**: the ARM7 and AICA follow the wall clock, so at
  ~80% they run ~1.25x per emulated frame: `arm` + `aica` 2.7-2.85 ms against 2.2-2.3 in log 17.
  It comes back at 100% (and helps AUTO there). The owner's choice; not a regression.
- Night demo 55.5-60.9 fps (frames 5040-6720), as in log 17.
- Same race-load hitches as before: f11041 154.8 ms (`tex` 72.9, `mem` 34.6, `elan` 25.7, `rend`
  20.9), f11283 101.9 ms (`tex` 46.8, `sh4` 29). Guest memory 1712 KB free at frame 10980, 36
  reclaims, 0 failed allocations; 3.2-3.7 MB free through the race.

Log 18 (2026-11-04, SOFT FLOAT NATIVE, otherwise as log 17):
- **SOFT FLOAT works: `sh4` -0.31 to -0.45 ms** in the steady race windows (frames 13320-14040:
  9.31-9.47 -> 8.93-9.13), the 0.3-0.5 expected. Control: the night demo replays the same frames
  in both logs and its `sh4` matches log 17 within +-0.08 ms over 18 windows (5400-7440); `elan`
  and `rend` match too. Nothing wrong reported.
- **The race itself is not comparable**: the owner's race loaded ~240 frames later, its heavy start
  lasted longer (12120-12840: 98-116K vertices in, 43-46 fps) and the steady part drew more (~80K
  in as before, but 58-68K on the GPU against 50-59K, ~110 whole models stepped over against
  ~150: another line or camera). So `elan` +0.5-0.9 and `rend` +0.4-0.7 there, 47.3-51.8 fps,
  p50 20-21 ms. Compare buckets, not fps, between two driven races.
- Same hitches as log 17 at the race's load (f11901 152 ms: `tex` 74.6, `mem` 39.6; f12143 92 ms),
  36 reclaims, 0 failed allocations, free down to 1036 KB once. Night demo 56-61 fps.
- The four "Write to Boot ROM (read-only), addr=74-77, pc=0C0E2154" lines near frame 4800 are in
  every log since 14: not new.

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
| 11-04 | SOFT FLOAT (native double divide and multiply) | log 18: `sh4` -0.31 to -0.45 ms in the race (night demo control unchanged); race not comparable (heavier scenes driven) |
| 11-05 | MIRROR OFF (in-car view), SMALL 3D, REFLECTIONS 1 in 8 (owner: downgrade options for speed) | log 20 (in-car view all race): every frame drawn 43.5-54.7 fps, p50 18.8-21.2, `rend` 1.55-2.2; AUTO + 4 px: 100% speed, 43-61% of frames drawn |
| 11-05 | IN-CAR REFLECT (reflection map rebuilt 1 frame in 16 in the in-car view) | log 21: 112-113 builds of 120 skipped as designed, but `sh4` unchanged within 0.1 ms against log 20 on the same course: a build costs the Wii far less than the 1.3 ms assumed |
| 11-05 | DISPLAY LISTS (cached lists' index draws written once as GX display lists) | log 22 (night Akina, every frame drawn): `cached programs` 0.75-1.4 -> 0.10-0.28 ms a render, render CPU -0.5 to -0.6 ms at equal lists and vertices; no GPU wait, no ring pass in the race |
| 11-05 | Point-lit light sets reused while a list picks the same lights | log 23 (night Akina, every frame drawn): 80-97% of sets reused, recording 3.65-4.5 -> 2.85-3.6 us a list with a box, `elan` -0.5 ms at equal load; day course in-car ~100% every frame drawn |
| 11-05 | COLOUR LISTS (colour lists through the GPU fetch as RGB8) | log 25 (day course, in-car, every frame drawn): colour vertices 1.1-1.6K -> 0, render CPU -0.35 to -0.42 ms, **100% the whole race** (59.4-60.1 fps, p50 15.5-16.3) |
| 11-05 | Audio push never waits with SOUND CLOCK REAL TIME | log 25 (night, AUTO): `snd` 25% -> 0, 0 drops; 100% speed with ~36 renders shown a second (29-46) |
| 11-05 | ELAN PREFETCH (run-ahead dcbt in the lists outside models), walk split by dropped/drawn frames | 2P log 1 (2 PLAYERS, day, ON only): dropped-frame walk 460-510 ns a header (~700 estimated before), drawn 760-810; an OFF run still to come |
| 11-12 | LIST MATHS, 3D DECODE, DRAW MEMO (newlib's `sqrtf` loop and the divisions out of the recording, decode and draw), SOUND STEP, SH-4 TIMESLICE 3584 | log 30 (night Akina, every frame drawn, MIRROR OFF + GAME SKIP): **in-car 59.3-60.8 fps nearly all race**, p50 15.0-15.5, walk ~800 ns a command (928), timeslice call-outs halved; chase view 45-54 |
| 11-12 | CACHE HINTS (`dcbz` on the per-render tables, prefetches in the draw, the walk and gxRend's strip loop), PMC split of the ELAN phase | log 31 (HOTBLOCKS run, DISPLAY LISTS starved of heap): split read, CACHE HINTS not judged; awaiting log 32 |
| 11-12 | PC SAMPLER (performance monitor interrupt, 10 kHz PC profile), diagnostics' heap headroom, DISPLAY LISTS ring fallback | log 31b: the sampler froze at start (its handler used the idle thread's r1 = 0; fixed: own stack); CACHE HINTS no net gain (walk ~5-10% slower, T&L ~5-10% faster); log 32: the profile (chase CPU-bound, idle 0.5%; in-car idle 8-16%) |
| 11-12 | GAME COPIES (the game's word copy and display-list copy in C), PC SAMPLER block table 8192 + SH-4 by 1 KB | log 33: no speed change; the list copy went native but is memory-bound (C cost = loop cost), the word copy's hot calls enter at 0C20A29E (not hooked) |
| 11-13 | GAME COPIES at 0C20A29E too, `dcbz` bursts, INT DIVIDE (the game's FPU integer divides in C), `[prof] GAME COPIES` line | log 34 (with the row below, night in-car): `sh4` 6.96-7.2 -> 6.55-6.8 ms; the copy loops and the divides gone from the profile |
| 11-13 | JIT FPU BANKS (frchg renames the float banks in the decoder, fschg/frchg end no block), SHIL executor for the runner | log 34: 19 stretches renamed; matrix library still 3.0-3.7% of in-car samples, gain not separable from the row above |
| 11-14 | SCREEN BUFFERS (a third XFB: no display copy onto the picture on screen), torn-copy count | awaiting log 35 (owner saw tearing with the faster SH-4; no speed effect expected) |
| 11-04 | PAGE GUARD (unwritten ELAN pages read-only, no per-render check) | log 19: exact, page checks ~0.15 -> 0.055 ms a render; race 45-51 fps at ~80K vertices with REAL TIME sound |

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

### Log 34 (2026-11-14): the three SH-4 rows, and the tearing they uncovered

Settings: log 33's + INT DIVIDE NATIVE + JIT FPU BANKS RENAME, PC SAMPLER on, CCALLS and HOTBLOCKS off,
FRAMESKIPPING 0, MIRROR OFF + GAME SKIP. Night Akina, in-car from ~6480 to the end (~12480), the chase
view only at ~9500-9720 and ~9960-10080. Same course and view as log 32's in-car stretch, so comparable.

| In-car, typical frame (ms) | log 32 | log 34 |
| --- | --- | --- |
| `sh4` | 6.96-7.2 | 6.55-6.8 |
| `elan` | 3.1-3.8 | 3.1-4.4 (heavier stretches) |
| `rend` | 1.2-1.5 | 1.2-1.8 |
| `arm` + `aica` | ~2.1 | ~2.1 |
| p50 | 14.7-15.7 | 14.5-15.0 |

- In-car 59.4-60.8 emulated fps (already ~100% in log 32), chase 52.7-58.6 (`elan` 5.55-5.7: 2.6-2.7
  commands, 2.9-3.0 T&L). The `sh4` saving is ~0.4 ms (~6%).
- PC SAMPLER in-car: C 43-50%, SH-4 JIT 31-34% (log 32 35%), trampolines 1.8% (2.2%), ARM7 5%. Gone:
  0C20A2A0 (word copy, 1.47% in log 32's in-car), 0C1D4F4C (list copy, 0.63%), 0C2223FA (divide, 0.45%).
  0C222000's KB keeps 0.26-0.32% (block 0C222354, another routine than the two divides).
- `[prof] GAME COPIES per frame` in-car: word copy 30 calls (360 words), the word loop's own entry a few big
  calls (~4096 words a frame), list copy + head 46-52 calls (3.3-4.7K words), INT DIVIDE 30-62 signed +
  72-79 unsigned: all native, 0 turned down. (Per-frame integers: "0 calls, 4096 words" is < 1 call a frame.)
- `[prof] JIT FPU BANKS`: 13 stretches closed with no swap, 6 ending in one, in all the code compiled. The
  matrix library 0C1F6000-0C1F6BFF still takes 3.0-3.7% of in-car samples (0C1F6400 1.5-1.9, 0C1F6800
  1.0-1.35, 0C1F6000 0.4); its blocks merged (0C1F6510, 0C1F6610 now hold what were separate blocks). Its
  share of the 0.4 ms is not separable; the rest of its time is FPU work, not bank swaps.
- Frame pacing as log 32: VI histogram in the race 0x 44-59, 2x 36-46, 3+ 1-8 per 512 frames; the FPROF
  tails are the audio ring's waits (`snd` 20-29 ms), at 100% speed.

**Tearing.** The owner saw "some kind of tearing" with any of the three rows on, none with all three
off. gxRend's present (ASYNC_RENDER, on): render N's display copy is queued into XFB A, its flip asked for
at the next emulated vblank (if the GPU is done) or at render N+1's start; the flip reaches the screen at
the next real retrace. Render N+1 copies into B, "the other one", which stays on screen until that retrace.
The emulation runs on its own clock (SPG deadline at the game's 59.79 Hz, the Wii's VI at 59.94, the audio
ring's waits), so that retrace falls anywhere: whenever it falls after N+1's copy (~6 ms after its
STARTRENDER in-car: 1.4 ms CPU + ~5 ms GPU), the copy is written under the scan. The sooner the SH-4 gets
to STARTRENDER after the flip was asked for, the likelier, so the three speed rows made it show more; the
race is older than them. The two synchronous 2D presents (VRAM copies, 2D after 3D) copy into the same XFB
every time, also onto the shown one (no race frames take them).

Built, awaiting log 35: SCREEN BUFFERS (RENDERING, last row, default 3 (NO TEARING), 2 (OLD)). A third XFB,
and every display copy goes through `xfb_target`: an XFB that is neither `VIDEO_GetCurrentFramebuffer()`
(on screen) nor `VIDEO_GetNextFramebuffer()` (asked for) nor the one waiting for its flip (libogc's retrace
handler only moves currentFb to nextFb, so nothing can show it before its own flip). `_vmem.cpp` leaves the
heap `kScreenBufferBytes` (640 x 576 x 2 = 720 KB) more when chosen; libogc gives VI XFB addresses past
16 MB, so MEM2 is fine. A copy's XFB is checked at its draw-done interrupt (still `currentFb`: torn), and the
`[prof] frames shown per second` line ends with `| N screen buffers: X copied onto the picture on screen
(torn)` (and `, N with no free buffer` if `xfb_target` ever found none: must not happen). To read: a run
with 2 (OLD) should show the torn count (the proof); with 3 it must be 0. Not done: asking for the flip
from the draw-done interrupt (shows each frame up to a frame sooner, steadier cadence); worth it if the
judder (VI 0x/2x) bothers more than the tearing did.

### Log 33 (2026-11-13): GAME COPIES on the Wii, half of it working

File `idas3wii_log_33.txt`: log 32's settings with GAME COPIES NATIVE, night Akina, in-car ~6360-9720,
chase ~9720-10920. Spans: `port/wii/build/boot_pcs_800436d8.elf`.

- Speed unchanged: in-car 57-60.4 fps (p50 14.6-15.6, `sh4` 6.9-7.1), chase 49.5-55 (p50 18.6-19.7,
  `sh4` 8.8-9.3, `elan` 4.5-4.9, `rend` 1.8-2.1). PCS chase: C 52.5-54.6%, JIT 38-39%, ARM7 5-5.4%,
  trampolines 2.0% (log 32: 2.6-2.8), idle 0.3-2.2%. Block table: 0 over (8192).
- The display-list copy went native (0C1D4F4C gone), but its C code (`runBursts`, 1.4-2.0% of a
  chase frame, ~0.8 in-car) cost what the recompiled loop did: memory-bound, each 32-byte burst a
  miss on the source and one on the destination line (which a store first reads in).
- The word copy did not: 0C20A2A0 still 2.2-2.4% of a chase frame, 1.4-1.5% in-car, and its C code
  never shows. Runner census (`--game-copies 1`, new exit lines): the game calls the word loop's own
  entry 0C20A29E directly far more than 0C20A280 (2.1M against 0.13M calls over the race route; from
  0C2189A6, 0C21B4F0-0C21B6D0, 0C2109AE-0C210A3A), and only 0C20A280 was hooked. Its 12K calls turned
  down are the 8-aligned ones (the `fmov.d` loop, by design).
- The new `[PCS] SH-4 code by 1 KB` line (chase / in-car): 0C20A000 2.3/1.4 (the word loop),
  0C1F6400 + 0C1F6800 + 0C1F6000 ~4/3.6 (the game's matrix library: XF loads, 4x `ftrv`, push/pop;
  58 `frchg` and 46 `fschg` in 3 KB), 0C1D8000 1.7/1.4, 0C1DC000 1.4/1.2 (`fmov.d` store-queue
  bursts from the FPU registers), 0C1DA000 1.0/0.7, 0C222000 0.9/0.85 (integer divides, below),
  0C207C00-0C208000 ~1.5, 0C1D9800/0C1D3800/0C0D1800 ~0.6 each.
- 0C222000-0C2223FF (runner `IDAS3_SF_PROBE`): GCC's FPU integer divides, `__sdivsi3_i4` (0C2223B8,
  ~96 calls a race frame) and `__udivsi3_i4` (0C2223E0, ~20): FPSCR pushed, PR set, fdiv in double,
  `ftrc` into FPUL, FPSCR popped; each FPSCR write ends a recompiled block.
- The matrix library's cost is the recompiler's `frchg` (a 16-register bank swap inline, ~50
  instructions) and `fschg`/`frchg` ending every block (`decoder.cpp`): the library swaps banks
  around each XF load. A decoder change (an XF load between two `frchg` done straight into the
  back bank), not checkable on the Windows runner (interpreter only): left for a later build.

Built after it, **awaiting log 34**:

- GAME COPIES also takes the word loop's own entry 0C20A29E (kind 4: same work and registers;
  cycles from that block). Runner, race route: 2.22M direct calls checked, 0 mismatches (most from
  0C2189A6, 2^21 of them); natively, no word left through the game's loop, the same race at the
  end (same vertices and census), but record digests split from render ~3905: an interrupt now
  lands after a whole copy instead of between two of its blocks (timing, as SH-4 TIMESLICE).
- The burst copy on the Wii zeroes each destination line with `dcbz` (no read before the write) and
  asks for the source four bursts ahead; eight scalars, so nothing is spilled around the `dcbz`.
- INT DIVIDE (RECOMPILER 2, last row, default NATIVE, EMULATED (OLD)): both divides in C at their
  entry (FPUL, the scratch registers, T, the two words pushed below r15, FPSCR untouched); a zero
  divisor or INT_MIN / -1 runs the game's code. Runner `--int-divide 1` (checked with
  `--game-copies 2`): race route, 185,762 signed and 799,533 unsigned calls checked (~18 and ~76 a
  frame over the run), 0 mismatches; 13 returns found a pushed word below r15 already reused by an
  interrupt (dead stack, counted apart as for the list copy's r14). Natively, the same race.
- A `[prof]   GAME COPIES per frame:` line: calls, native calls and words for each routine and
  divide, the word copy's 8-aligned calls and the last one turned down with its addresses.
- Expected together: ~0.5-0.7 ms off a chase frame (word loop ~0.45 with its trampolines, bursts
  ~0.1, divides ~0.15), ~0.3-0.4 in-car.
- **JIT FPU BANKS** (RECOMPILER 2, last row, default RENAME; SWAP (OLD)), the matrix library above.
  `decoder.cpp`: `frchg` flips FPSCR.FR and marks the banks crossed instead of swapping them; the
  ops decoded while crossed get FR and XF exchanged in their operands when the stretch closes
  (`dec_SwapBanks`: every float operand is a base register in two parallel blocks), so a
  `frchg ... frchg` pair costs no swap, and `ftrv` inside it reads XMTRX from the pinned
  registers. The physical swap (a plain `shop_sync_fpscr`) is emitted only where the banks must be
  real (`dec_FpuPhysical`): at the block's end, before an interpreter fallback, before any other
  op that sets FPSCR. `fschg` and `frchg` end no block; inside a crossed stretch `fschg` skips its
  sync (the SZ-only sync copies all of FPSCR into `old_fpscr`, FR bit included, which would hide
  the swap still owed). The back end needs nothing new: XF operands take the generic memory paths
  (as `fmov.d` XD and `ftrv`'s XMTRX always did), the appended sync uses r3/r4/f0 while the jump
  target sits in r31 and CR0/T forwarding only spans adjacent ops. New `[prof]   JIT FPU BANKS:`
  line (stretches closed without a swap, stretches ending in one, totals of the blocks compiled).
  Expected: most of the ~4% (~0.4-0.6 ms a chase frame), not measured.
- **SHIL executor** (runner `--shil-exec 1`, `emu/host/shil_exec.cpp`): the SH-4 run block by
  block through the recompiler's own decoder, each SHIL op executed in C (the canonical bodies
  through `ngen_CC_*`, which on Windows were empty stubs; moves, memory, jumps and interpreter
  fallbacks by hand; block exits as `ngen_End`). It checks decoder changes against themselves:
  JIT FPU BANKS off vs on (`--jit-fpu-banks`), night attract demo to frame 12000 (half the
  stretch over 20K opaque vertices a frame, point lights): record digests identical at all 106
  shared renders (the rename run fits one more render in: block ends moved, so timing, and the
  sound hash, differ); the race route: 109 identical. It is not the Wii back end (no pinned
  registers, fastmem or PowerPC code), and its timing differs from the interpreter's (the race
  route's presses miss the race under it).

Spans of this build: `port/wii/build/boot_pcs_80043f10.elf`.

### Log 32 (2026-11-12): the first PC SAMPLER profile

File `idas3wii_log32.txt`: night Akina, PROFILER + PC SAMPLER on, JIT CCALLS and HOTBLOCKS off, CACHE
HINTS on, FRAMESKIPPING 0; in-car ~5900-9000, chase ~9000-11160. Spans resolved with
`port/wii/build/boot_pcs_800436b8.elf` (`powerpc-eabi-nm -n -S -C`; 1% of the samples is ~0.17 ms of a
60 fps frame, ~0.19 ms of a chase frame).

- Speed as logs 30/31b, so the sampler costs nothing visible: in-car 57-60 emulated fps (p50 14.9;
  `sh4` 7.0, `elan` 3.6, `rend` 1.5, `arm` 1.07, `aica` 0.99), walk 1.6-2.2 ms a frame (1800-2500
  commands, 830-920 ns); chase 49.5-55 (p50 18.75; `sh4` 9.13, `elan` 4.40, `rend` 1.82, `arm` 1.23,
  `aica` 1.09), walk 2.1-2.8 ms (2550-3160 commands, 820-900 ns).
- Where the time goes. In-car: C code 42-48%, SH-4 JIT 32-36%, ARM7 JIT ~5%, fastmem trampolines
  ~2.2%, **idle 8-16%** (every thread waiting: the frame pacing, time to spare). Chase: C 48-51%, JIT
  41-43%, ARM7 5-5.5%, trampolines 2.6-2.8%, **idle 0.3-0.6%**: the CPU never waits, the chase view is
  CPU-bound. The block table (2048) overflowed by ~3000-3800 samples a report (~10% of the JIT's).
- C by function, chase (% of all samples):
  - ELAN walk `execute` ~5.4% (~1 ms a frame); its loop head, where a command header that misses
    the cache is first read, 1.9-2.1% alone.
  - The recording's lookups: `findList` 1.4% (linear probing in a 4096-slot table holding up to 3072
    lists), `pagesSettled` 0.7%, `elan_gx_list_box` 0.9-1.5%, CACHE HINTS' helpers 0.4-0.5%: ~3.5%.
  - `tnl_DrawList` (the recording, inlined) ~2.8%, `tnl_SetMatrix` 1.1-1.2%, `elan_gx_draw` ~3%,
    `measureList`/`buildLightSet` 0.4%, `drawStrips<UV>` (CPU-fed lists) 0.4-0.75%.
  - `memcpy` 0.6-0.8%, `memset` 0.4%; sound: `AICA_Sample` ~2% spread, `StreamStep` ~0.7%;
    `SetTextureParams` ~0.6%, `bm_GetCode` 0.3%, `libPvr_UpdatePvr` 0.3%, the GX token/region
    callbacks 0.3%, `WiiGuestMem_ElanGpuPage` 0.3%, the sampler's own drain 0.3%.
  - Menus and loading: CHD decode (`bitstream_flush`/`ecc_compute_bytes` ~6%, `inflate_fast` 2.3%),
    `desCrypt` ~5%, `texture_TW<conv565>` 2.5%.
- SH-4 blocks, chase: **0C20A2A0 2.5%** (the word loop of the game's copy routine 0C20A280: runner
  trace, every call in a race frame copies 32 or 64 bytes from main RAM into the store queues),
  **0C1D4F4C 1.9%** (the burst loop of the display-list copy 0C1D4F20: main RAM to ELAN RAM through
  the store queues), 0C1DC008 1.2% (an `fmov.d` store-queue burst from the FPU registers), 0C1D9900
  0.75, 0C1D80BE 0.74, 0C0D1A60 0.72, 0C2223FA 0.50, 0C1400EE 0.46, 0C1D3A94 0.43, 0C048E9E 0.38 (the
  reflection map), then a long tail at 0.15-0.35 each. In-car: 0C20A2A0 1.47, 0C1DC008 1.07,
  0C1D80BE 0.70, 0C1D4F4C 0.64. Of chase's +2.1 ms of `sh4`, the two copies are ~0.5 ms and
  0C0D1A60/0C1400EE/0C048E9E/0C15114C ~0.3; the rest is spread.

Built after it, **awaiting log 33**:

- **GAME COPIES** (RECOMPILER 2, last row, default NATIVE; EMULATED (OLD) keeps the recompiled
  loops). `naomi_copies.cpp`: three of the game's routines done in C, called at their entry by the
  recompiler (as SOFT FLOAT's): the word copy 0C20A280 (its 32-bit loop; the 8-aligned `fmov.d`
  loop stays the game's), the display-list copy's head 0C1D4EA0 (3 bursts, word 15 kept) and the
  rest 0C1D4F20. Same memory (store queues, ELAN RAM, the pushed r14, QACR, word 15), same registers
  and T, and the cycles the game's blocks would have charged (so timing stays as before). Each is
  fingerprinted (code and literal pool); what it cannot do exactly (other areas, misalignment, the
  TA, lengths out of the ordinary) runs the game's code. Expected: most of the copies' ~4.4% of the
  chase frame and part of the trampolines' 2.8%, ~0.5-0.8 ms in the chase view, ~0.3-0.4 in-car.
  Runner `--game-copies 0/1/2` (2 checks every call's registers, memory, queues against the native
  model). Checked on the race route to frame 10500 (in-car, chase, in-car): word copy 119,590 of
  131,625 calls taken natively (the rest is the 8-aligned `fmov.d` path), list head 166,786 of
  166,786, list rest 166,783 of 166,786, 0 mismatches (one return found the popped r14's stack slot
  reused by an interrupt: memory below r15, now counted apart). With `--game-copies 1` the same
  calls and **record digests identical** to the game's code at every render; the sound hash
  differs (the copy's cycles are charged at once, so SH-4 writes land a few hundred cycles
  differently against the sound chip, as SH-4 TIMESLICE did; no sound RAM is copied).
- PC SAMPLER: block table 8192 (was 2048) and a new `[PCS] SH-4 code by 1 KB` line (JIT samples by
  1 KB of guest code from 0C000000, whatever the block). New elf to resolve spans:
  `boot_pcs_800436d8.elf`.

### Log 31b (2026-11-12): the PC SAMPLER build, sampler off (it froze at start)

File `idas3wii_log31.txt`: night Akina, PROFILER + JIT CCALLS on, HOTBLOCKS and PC SAMPLER off,
CACHE HINTS on, FRAMESKIPPING 0; in-car to ~8640, chase to the end (~10800).

- In-car 59.1-60.3 emulated fps, p50 14.6-15.9; typical `sh4` 7.05-7.3, `elan` 3.0-3.7, `rend`
  1.03-1.43, `arm` 1.25, `aica` 1.03-1.17. Chase 46.8-53.8, p50 18.6-20.7; `sh4` 9.0-9.6, `elan` 4.5-4.9,
  `rend` 1.9-2.1. Log 30's numbers in both views. The DISPLAY LISTS ring got its 1 MB.
- CACHE HINTS against log 30 (another stretch, CCALLS on here): the walk 792-914 ns a command (log 30:
  in-car 796 with CCALLS off, chase 750-790 with it on), so ~5-10% slower; T&L ~2.95 us a box in-car,
  3.3-3.4 chase (log 30: 3.2-3.8, ~3.5), ~5-10% faster; the draw's rest 1.7-2.2 us a list (1.6-1.9),
  no faster. No net gain visible. The walk's part (prefetches at each model call) is the suspect for
  the slower walk, the `dcbz` zeroing the likely T&L gain; they share one row, so only splitting it or
  the PC SAMPLER can separate them.
- Counters without HOTBLOCKS: ELAN commands in-car 2.95-3.8 ms a frame (IPC 0.56-0.65, load-miss
  32-41%), chase 4.4-5.6 (IPC 0.59-0.63); game + sound + others in-car IPC 0.37-0.43, load-miss 9-11%,
  chase 0.47-0.51 and 12-14%, both with **9-11K L1 instruction misses/ms** and ~4K mispredicted
  branches/ms. So log 31's I-miss rate was not HOTBLOCKS': log 30's 7.3K/ms was the whole rest of the
  frame, the ELAN commands' ~1K/ms included. At one miss per ~30 instructions it may be a large share
  of `sh4`; the sampler should show whether the time sits in a few blocks or is spread out.

**PC SAMPLER fix** (built after log 31b, awaiting log 32). libogc's idle thread
(`KThreadIdleMain`, a `b .` loop that runs whenever every thread waits: vsync, audio, GPU waits) has a
zeroed context except SRR0/SRR1 (`KThreadInit` disassembled), so it runs with r1 = r2 = r13 = 0.
libogc's IRQ entry (`PPCExcptIrqHandler`) never uses the interrupted stack: it moves to
`__ppc_excpt_sp` and reloads r2/r13. The sampler's handler pushed its frame on the interrupted r1. The
first sample in the idle loop stored to 0xFFFFFFA0, a DSI with MSR[RI] = 0, and libogc's stub sent it
to `PPCExcptDefaultHandler`, which stays on the faulting stack: a freeze with no message. Now the
handler moves to its own 1 KB stack (`g_fp_pcs_stack`, the interrupted r1 kept as the back chain),
saves r2/r13 and loads `_SDA2_BASE_`/`_SDA_BASE_`. A sample whose PC holds `b .` (0x48000000) counts as
idle, shown as "idle (every thread waiting)" in the `[PCS]` header: the share of time the CPU waits.
Checked in the disassembly (boot.elf, `frame_prof_dump` at 800436B8, kept as
`port/wii/build/boot_pcs_800436b8.elf`): the frame at `g_fp_pcs_stack` + 0x390, 16-byte aligned; r1
restored before the `rfi`; `fp_pcs_sample` still has no FPU instruction and no stack frame.

### Log 31 (2026-11-12): night Akina uphill with JIT HOTBLOCKS (a diagnostics run)

PROFILER, JIT CCALLS and JIT HOTBLOCKS on, CACHE HINTS on; in-car, chase ~7380-8130, in-car. Not a
speed reference: HOTBLOCKS costs ~12%, and its 448 KB block table took the heap the DISPLAY LISTS
ring needed (`no memory`), so `cached programs` took 0.7-1.35 ms a render (log 30: 0.23).

- ELAN phase (walk + recording, the new split): in-car 3.4-3.65 ms a frame, chase 4.0-4.75; IPC
  0.60-0.66, 32-38% of cycles in L1 load misses; per ms ~2000-2250 L2 data misses (~120 cycles
  each: ~1/3 of the phase), ~600-720 L2 hits, ~1000 L1 instruction misses, 5100-6800 mispredicted
  branches (a few % at the 750's short penalty). So ~2/3 of it is instructions: ~1.5M PPC
  instructions a frame for ~2100-2700 commands and ~550-800 lists, ~350 a command for the walk.
  Prefetching can only take the stall third; the instruction count is the larger lever, and a
  per-function profile is needed to know where it goes (the PC SAMPLER below).
- Game, sound and the rest: IPC 0.47-0.53, load-miss 12-15%, ~1.2-1.5K L2 data misses/ms, but
  ~10-11.5K L1 instruction misses/ms (one per ~35 instructions); log 31b, without HOTBLOCKS, has
  9-11K/ms too (log 30's 7.3K/ms was the whole rest of the frame, the ELAN phase included). The JIT'd game code does not
  fit the 32 KB L1I, and whether those misses hit the L2 is unknown (the 750CL has an "L2
  instruction misses" event, not counted yet). If they mostly do not, code layout (hot paths
  together, smaller block prologues/epilogues) becomes a lever on `sh4`.
- HOTBLOCKS (block entries, not time; the 16384-entry table overflowed by 5559 blocks, so blocks
  compiled late are missing): in-car the wait loop 0C20C854-0C211880 is ~13-20% of entries, the
  VRAM32 copy loop 0C20A2A0 ~7.7%; chase runs 0C20A2A0 ~1.6x more (400-450K entries/s, ~7000
  iterations a frame against ~4400) and the SQ copy loop 0C1D4F4C (`pref`, 32 bytes to ELAN RAM)
  ~50-75K entries/s, which in-car barely runs: the chase view's extra `sh4` is the game writing more
  (display lists, VRAM), as expected with more objects in view.
- [CC] in the race: timeslice 933, writemem ~700-800 (G2 DMA suspend registers 0x5F781C-7C ~17-25
  each, AICA wave RAM 70-140, ELAN FIFO 72), getcode 250-420, sync_sr ~260, interrupt ~255, readmem
  ~400 a frame: ~3000 call-outs, not the lever.

### Done 2026-11-12 (3): PC SAMPLER and the diagnostics' heap (awaiting log 32)

- **Heap headroom for the diagnostics**: `_vmem.cpp` keeps `hotblocks_heap_bytes()` (448 KB with
  HOTBLOCKS on) and `frame_prof_heap_bytes()` (64 KB with the sampler on) out of the guest page pool,
  and the DISPLAY LISTS ring falls back to 768 then 512 KB when 1 MB cannot be had (a night render's
  entries take ~350-380 KB on the runner). Windows, the day route to 13500 with
  `IDAS3_DL_RING_KB=768` (and the five items + CACHE HINTS): all 166 record digests and the sound
  hash identical, 0 MISMATCHES, 0 HAZARDS; 4274 entries written against 3340 with 1 MB, 5 ring passes
  against 3, 31 GPU waits against 16. A fallback, not a choice.
- **PC SAMPLER** (LOGS, last row, default OFF; with PROFILER): a hardware PC profile. PMC1 counts
  cycles and the performance monitor interrupt (MMCR0 ENINT + PMC1INTCONTROL, vector 0xF00, which
  libogc's PPCExcptInit fills with its general stub like every vector from 0x100 to 0x1300) fires every
  72,900 cycles (10 kHz). The handler (`__fp_pcs_handler`, the DSI handler's pattern) re-arms PMC1,
  counts the interrupted PC: C code into 256-byte spans from 0x80003F00 (.init + .text, 2 MB), SH-4 JIT
  code into a ring that the frame loop resolves to SH-4 blocks (walk back to `addic. r29,r29,-n` and
  its blt/bge, read the SH-4 address from the timeslice exit's lis/ori), ARM7 JIT, trampolines, other.
  Every ~8 s: `[PCS]` header (shares, sample rate, `frame_prof_dump`'s address to match the boot.elf),
  the 40 hottest C spans and the 32 hottest SH-4 blocks, in % of all samples. Time-weighted with
  stalls, unlike HOTBLOCKS (entries) and the counters (rates). It takes the PMCs (no `[HW] CPU
  counters` lines; the render probe lines stay). Interrupt handlers' time (EE off) lands on the
  instruction they return to. Resolve spans with `powerpc-eabi-addr2line -f -C -e boot.elf`; a copy of
  each shipped boot.elf must be kept.

### Log 30 (2026-11-12): night Akina with the five items

Two runs, FRAMESKIPPING 0, MIRROR OFF + GAME SKIP, the five items below on (owner: "more
performance indeed"): uphill with JIT CCALLS off, downhill with it on (its census costs `arm` ~0.4
and `sh4` some).

- **In-car: 59.3-60.8 emulated fps for nearly all of the race**, p50 15.0-15.5 ms. Typical frame
  (uphill 8400-8520, 477 lists drawn, 44K vertices, 567 boxes): `sh4` 6.95, `elan` 3.50 (walk 1.68
  ms, 2113 commands at 796 ns; T&L 2.06), `rend` 1.44 (CPU 1.67 ms a render, of it the draw 1.01:
  cached programs 0.23 + the rest 0.78 = 1.6 us a list), `arm` 1.16, `aica` 1.16. Heaviest stretch
  (uphill 9000-9360: ~74K vertices in, 744-764 boxes, ELAN 4.6 ms): 53.6-58 fps.
- Against log 27 (same course and view, items not separable): the walk 928 -> ~800 ns a command,
  the draw's rest 1.6-1.9 us a list (log 25: 2.0-2.1), T&L 3.2-3.8 us a box (log 27 up to 4.2), the
  timeslice call-outs ~1870 -> 933 a frame. Nothing broke with the 3584-cycle timeslice (music,
  input, races).
- **Chase view at night (downhill ~8520-9360): 45-54 fps**, p50 18.9-19.8. Against in-car: `sh4`
  +2 ms (9.0-9.2 with CCALLS on: the game's own work, more objects in view; the rest-of-frame IPC is
  higher there, so it is more instructions, not more stalls), `elan` ~4.5 (+1.3: walk 2.1-2.3 ms at
  750-790 ns, T&L 2.6-3.0 at ~3.5 us a box), `rend` 1.8-2.0 (+0.6). FRAMESKIPPING AUTO is the
  answer there for now (S ~12, D ~19: ~70% of frames drawn).
- Memory behaviour: renders run at IPC ~0.60 with 36-42% of their cycles in L1 load misses; set B
  gives ~230 L2 hits against ~2400-2600 L2 data misses a ms. Next to nothing the ELAN recording
  touches is still in the 256 KB L2 when the render reads it (~5000 misses a render, ~8 a list):
  the tables are walked once each way, so only ordering and hints help (no reuse to keep).
  Rest of the frame: IPC 0.45, load-miss 16%.
- FPROF tails in these runs are `snd` (+20 ms in the p95 frames): the pacing wait at 100% speed,
  not work. `mem` tails of 2-4 ms are page compressions; one-off texture decode bursts of 40-67 ms.

### Done 2026-11-12 (2): CACHE HINTS and the ELAN phase's counters (awaiting log 31)

**CACHE HINTS** (RENDERING 2, last row, default ON; OFF (OLD) keeps the old code). Exact: hints and
line zeroing only, the picture cannot change. Broadway has no hardware prefetcher, and every
write to a line not in the cache first reads it from memory (write-allocate).

- The Wii's per-render tables (`s_items` 24 B, `s_xforms` 108 B, `s_lsets` 560 B entries, all
  `memalign(32)`, filled front to back from 0 each render) are zeroed one line at a time with `dcbz`
  just before the recording first writes into a line (`zeroFront`: a front pointer per table,
  reset with the tables; a static_assert keeps every table a whole number of lines). Each line is
  then written without being read from memory first: ~500 items, ~250 matrices, ~15-20 sets a
  night render, ~2.4K lines.
- The draw (`prefetchAhead`): the item 5 ahead, the matrix (4 lines) and light set of the item 2
  ahead when they differ from the one before, the whole light set (its lit lights and beam) of the
  next item, ahead of the fetch-table prefetches it already did.
- The walk: MODEL PREFETCH's 8 remembered headers per model now also keep the fetch slot the list
  found when it was last recorded (`ModelHeads`, 32 KB of bss), or mark a material header. At the
  model's call, before its commands are read, the fetch-table entry and page state of each list
  (`prefetchListTables`) and the material's body are asked for, then each list's box in the program
  ring (`prefetchListBox`: needs the entry, so asked for second). With MODEL PREFETCH OFF, CACHE
  HINTS still prefetches the tables; the headers only with MODEL PREFETCH.
- gxRend's strip loop: the vertex list 16 entries ahead, the poly param 4 ahead and the ELAN item
  index 32 ahead.

Expected: ~0.3-1 ms in heavy scenes (render load-miss 36-42% of ~1.7 ms, the walk ~800 ns a
command), unmeasured. To read in log 31: `rend` and "ELAN draw a render: the rest us a list",
the walk's ns a command, `elan` T&L us a box, and the new counters.

**PMC split of the ELAN phase** (PROFILER): `frame_prof_elan_begin/end` around each ELAN command
execution (`naomi_elan.cpp`), outside renders. The `[HW] CPU counters` lines gain "the rest: ELAN
commands X ms a frame, IPC, load-miss, DTLB | game, sound and the others ..." (set A) and per ms:
L2 hits, L2 data misses, L1 instruction misses, mispredicted branches for both (set B). They tell
whether the walk and recording are stall-bound (low IPC, high load-miss: hints and layout) or
instruction-bound (code to cut).

Windows checks (2026-11-12): `--cache-hints 1` with the five items on, on the day driving route to
frame 13500 (`--mirror 2`, same flags as the five items' check below). The prefetches compile to
nothing under MSVC and `dcbz` is Wii-only, so this checks the slot bookkeeping. Results: all 166
record digests and the sound hash (76C69E74C0595C38) identical to the run without it, DRAW MEMO
0 WRONG, 0 STALE boxes. Unit tests pass. The Wii build compiles; boot.elf has the three `dcbz`
loops in `elan_gx_record`. Every Item field is written by the recording, and matrices and light
sets are whole-struct copies, so a zeroed line hides nothing. The front pointers are only reset
with the counts (allocation, `elan_gx_frame_reset`).

### Done 2026-11-12: five items behind presets (log 30: in-car night ~100%)

The owner reported 90% in a night race (no log yet: view and course not known; log 27's heaviest
in-car stretch was 55-58 fps, ~17.6 ms). Found on the way, the lever behind items 1-3: **Broadway
has no square root instruction and devkitPPC's `sqrtf` is newlib's bit-by-bit loop**
(`__ieee754_sqrtf`: 25 iterations with a data-dependent branch, a store/load through the stack,
plus the errno wrapper: ~300 cycles, ~0.4 us). `powerpc-eabi-objdump` of boot.elf showed it in
`tnl_DrawList` (4 sites, the inlined recording), `elan_gx_draw` (3), `coneSide` (2), `boxView`
(1), and `ldexpf` twice in `tnl_SetMaterial`. A night point-lit list paid 2-3 of them (its box's
half diagonal for the quick light test, its bounding sphere, which only HEADLIGHTS SOFT/SMOOTH
read, and the matrix's scale once a matrix): an estimated ~0.8-1 us of the ~1.5 us a list night
costs over day (not measured on a Wii yet).
`rsqrt` (frsqrte + 2 Newton steps, ~4e-6) was already used per vertex; these sites were missed.
No other hot libm/libgcc call is left (the `__udivdi3` in `libPvr_UpdatePvr` are in its
once-a-second stats block; JIT FSQRT already inlines fsrra).

1. **LIST MATHS** (RENDERING 2, default FAST): bounds without libm. `sqrtUp`/`rsqrtUp` = x rsqrt(x)
   and rsqrt(x) times 1 + 2^-16 (Newton from frsqrte lands below the root, ~4e-6 at most, so the
   factor makes them upper bounds). Used where only a bound is needed: the box's half diagonal and
   the matrix's 1/sqrt(min scale) in `ModelBoxDistance::farFrom` (the quick "out of reach" test,
   which the exact test after it settles: the picks are the exact test's either way), a light's
   reach (same test), the spheres' radii. The sphere itself is only worked out when HEADLIGHTS is
   SOFT or SMOOTH (`sphereNeeded`); with ARCADE (default) it is never read. `coneSide`'s two roots
   to ~4e-6 (SOFT/SMOOTH only). Expected: ~0.4-0.6 ms off a drawn night frame (400-700 point-lit
   lists); read the `elan` T&L us a list with a box (log 27: 3.1-4.2 at night, 2.4-2.7 by day).
2. **3D DECODE** (RENDERING 2, default TABLES): materials and lights decoded without divisions or
   libm: n / 255 from a table filled with the same division (bit-identical), the gloss
   (1 + f/32) 2^(e-1) by a table of powers of two (exact; was `ldexpf` and a division), `boxView`'s
   scale kept squared (`tooSmall` only used its square; was a `sqrtf` once a matrix, in the box
   test of every model call on a drawn frame). Was meant to be "compiled models" for the ELAN walk,
   dropped before writing it: MODEL PREFETCH already asks for a model's first 8 command headers at
   its call (a model has ~6), so a compiled copy would mostly fetch what is already coming; the
   walk's own arithmetic was the measurable waste (~450 materials a frame: 4 divisions and 2
   `ldexpf` each; ~300 matrices: a `sqrtf`). Expected ~0.15-0.3 ms; read the walk's ns a command.
3. **DRAW MEMO** (RENDERING 2, default ON): the draw takes a list's program from the slot
   `cachedBox` found when the list was recorded (`ElanGxRecord::fetchSlot`, `Item::slot`,
   `fetchMemo`): its pages were checked then, once a render, so the draw's `pagesSettled` and
   hash probe are skipped; still checked: the slot holds the list, the program is still in the
   ring, the sampled checksum is not due (else the full lookup). `prefetchAhead` asks for the
   right slot. The normal matrix's scale (3 `sqrtf` each time a lit list's matrix changes, and for
   every env-mapped list) through frsqrte; the ambient colour is not converted again for the same
   light set. Expected ~0.3-0.5 ms a drawn frame (the roots: ~250 matrix changes a render);
   read `rend` and the [HW] "ELAN draw a render" rest per list.
4. **SOUND STEP** (AUDIO, default LAZY): `update_arm_interrupts` (every sample) leaves out the call
   into the ARM's latch when it would change nothing (same pending state and level, FIQ line
   already up: exactly what ArmInterruptChange + update_e68k would leave); a voice that does not
   reach its next sample only moves its fraction (all `StreamStep` does then) without the indirect
   call; the two clock reads a sample for the `vox` statistic only with PROFILER on. Exact, but
   small: ~0.05-0.1 ms. (Lazy timers were dropped: StepTimer is a decrement and a compare.)
5. **SH-4 TIMESLICE** (CORE, default 3584): with the FAST accuracy preset (the port's), 3584 cycles
   between UpdateSystem calls instead of 1792, the medium/slow/very slow tiers half as many slices
   apart, so sound (ARM batches, AICA samples), DMA, the cart, Maple and the ELAN DMA keep their
   cadence in cycles (7168 / 14336 / 28672). Halves the ~1870 timeslice call-outs a frame (and
   the dispatch after each). TMU, the SPG's scanline events and pending interrupts are seen every
   18 us instead of 9. The only item that changes timing: first suspect if anything acts up
   (music, hangs, input). Expected ~0.2-0.4 ms a frame, drawn or dropped.

Runner flags: `--list-maths 1`, `--walk-decode 1`, `--draw-memo 1` (`DRAW MEMO per drawn render`
line: every memo checked against the full lookup, WRONG must stay 0), `--sound-step 1` (the sound
hash must not move), `--sh4-slice 1`. The runner's defaults stay the old behaviour.

Windows checks (2026-11-12, `--gpu-tnl --point-lights 1 --list-cull 3 --display-lists 1
--colour-fetch 1 --small 4`): day driving route to frame 13500 (`--mirror 2`) with items 1-4 on:
all 166 record digests and the sound hash (76C69E74C0595C38) identical to the run without them,
DRAW MEMO 117-118 lists a render from the memo, 0 WRONG, the fetch check's mismatches the known
menu exception, line for line. Night attract demo (9000 frames): digests identical up to render
5761 (the demo is not repeatable on Windows past that: two runs differ in ELAN command words from
boot on), the sound hash identical, ~410 lists a render from the memo, 0 WRONG, quick-test
WRONG 0, set STALE 0, the quick test settling the same counts (the bounds' 1.5e-5 changed no
verdict there). Item 5 on the day route: the same renders and cart DMAs at every checkpoint, the
same 4192 mirror views skipped and 3 tire squeals, **all 166 record digests identical**, the car
at 142 km/h at frame 12014; only the sound hash differs (ARM/AICA events shift within a slice).
Unit tests pass. The Wii build compiles; `tnl_DrawList` now has 4 frsqrte sites, `elan_gx_draw` 3.

### Done 2026-11-11: four items behind presets (log 27: ~3 ms off a drawn night in-car frame)

1. **MIRROR = OFF + GAME SKIP** (value 2). The game draws a frame view by view: 0C024100 (r4 =
   the view) calls 0C1FB320 with the view's word +138 (user clip mode: 0, 2 inside the mirror's
   rectangle, 3 outside), which puts it in bits 16-17 of 0C989FF0, ORed into every model call
   (0C1D80C6: 08000800 | flags); then the scene, then mode 0 again. Each frame: a view with mode 0,
   the mirror (2), the main view (3). Found with IDAS3_WRITEVAL on a mirror model call (08020800,
   written at 0C1D80C8) and IDAS3_TIMELINE on 0C989FF0. At 0C024100, for the view with mode 2, the
   recompiler's block entry (and the runner's PC hook) returns to PR; `g_elan_mirror_seen` is
   bumped so IN-CAR REFLECT and MOTION FX still know the in-car view. Runner, day route in-car
   (frames 9300-9900): ELAN commands a frame 4098 -> 2572, lists 1562 -> 931, vertices in 147K ->
   106K; CPU-path snapshots of in-car frames 9500/9700/9900 pixel-identical to MIRROR OFF;
   player block, render states, probes and vehicle identical in a RAM dump at frame 9900;
   11 pages differ (the command-buffer counters, the mirror view's own data, the engine sound state
   by timing). Record digests differ once in-car: they hash each list's ELAN RAM address, and the
   game's per-frame buffers land elsewhere without the mirror's share. Expected on a Wii: the
   game's mirror pass (~5% of `sh4`, <= ~0.45 ms) plus its walk (~0.5 ms?) in the in-car view.
2. **JIT FR SWAP** (RECOMPILER 2, INLINE default): `shop_sync_fpscr`'s bank swap inline. With FPU
   PIN the front bank lives in f14-f29 (its memory copy stale): each register swaps with its xf
   word through f0 (`lfs f0,xf; stfs fN,xf; fmr fN,f0`), the same conversions the spill/reload
   pair did; without FPU PIN, 16 word swaps in memory. old_fpscr is stored first;
   SetFloatStatusReg has no body on a Wii. Replaces a 16-register spill, a C call (census, loop
   over 16 words) and a 16-register reload, ~2000 times a night frame. Only runs on a Wii.
3. **MODEL PREFETCH** (RENDERING 2, ON default): each model's first 8 command headers (offsets
   in its block, 32-byte units, `s_modelHeads`, 32 KB beside the summaries) are remembered when a
   drawn walk summarises it; at its next call they are all asked for (`dcbt`) before the walk,
   and from the top-level list the next model's too (peeking up to 4 commands ahead, already in
   the cache by ELAN PREFETCH). A hint only: a changed block is walked from its own words.
   `[prof]   MODEL PREFETCH: N models a frame`.
4. **SOUND MIX** (AUDIO, FULL default): HALF steps the voices on odd samples without mixing
   (`ChannelStepState`: envelope, stream, LFO, and the attack restart a silent voice in decay
   triggers, which depends on the mix), and plays the last sample again; not with the DSP on.
   Runner: 7.29M samples, the even-sample hash and the 109 record digests identical to FULL.
   Expected: the mixing part of `vox` on half the samples (~0.1-0.2 ms); sound at 22 kHz.
   **Log 27: no gain** (see below). Keep FULL.

Log 27 (2026-11-11, one player, night, in-car all race, every frame drawn, all four on, JIT
CCALLS off; log 26 had it on): 52-61 emulated fps, mostly 59-60, against 48-57, mostly 52-54.
Matching stretches (log 26 frames 8400-8520 against log 27 8640-8760: 343/364 lists drawn, 29/32K
vertices, T&L 1.92/1.91 ms for 463/455 boxes), typical frame:

| Bucket | log 26 | log 27 | |
| --- | --- | --- | --- |
| sh4 | 9.50 | 7.09 | GAME SKIP + FR SWAP (+ ~0.2 census) |
| elan | 4.05 | 3.26 | walk 2.63 -> 1.80 ms: 3229 -> 1937 commands, 813 -> 928 ns each |
| rend | 1.27 | 1.19 | |
| arm | 1.52 | 1.11 | JIT CCALLS off: the ARM7 JIT's census (block-entry counts, timers) |
| aica | 1.07 | 1.09 | SOUND MIX HALF: nothing |
| p50 | 18.2 | 14.6 | |

- The items gave ~3 ms (the census ~0.6 of the ~3.6). `sh4` -2.2 ms is GAME SKIP and FR SWAP
  together; the runner puts the game's mirror pass at <= ~0.45 ms of instructions, so either it
  costs more on a Wii than its instruction count says (scene walk, 48 KB of commands written to
  ELAN RAM a frame) or FR SWAP is worth far more than the ~0.2 ms estimated (~2000 swaps: the old
  path spilled and reloaded 16 pinned registers around a C call). One run with JIT FR SWAP = C
  CALL separates them. If it is FR SWAP, it is also the likely cause of night `sh4` being ~1.3 ms
  over day's in every log (the night's FPU code bank-switches; menus swap ~62 times a frame).
- The walk: the mirror's commands were the cheap ones (its models walked again, already cached),
  so the walk lost 40% of its commands and ~30% of its time. MODEL PREFETCH (170-310 models a
  frame) cannot be told apart from that: an OFF run would.
- Light commands decoded a frame 111 -> 54 (the mirror view's light setup gone), point-lit picks
  as before. `MIRROR GAME SKIP` 120 of 120 frames over the whole race (in-car).
- SOUND MIX HALF: `vox` per voice per emulated frame is log 26's (~0.015% of a second in both).
  A held voice (in decay with D2 level 0: engines, sustained notes) still reads its sample every
  sample for the attack-restart check, and the stepping (envelope, stream, LFO), which HALF keeps,
  is most of a voice's cost. "Tried and dropped".
- Heaviest stretch (frames 9600-10400, 560-620 lists drawn, 58-67K vertices, 650-765 boxes): p50
  17.4-17.6, 55-58 fps: `sh4` 7.4-7.6, `elan` 4.4-4.8 (walk 2.0-2.4, T&L 2.5-3.2), `rend` 1.7-2.1,
  sound 2.25. ~1 ms short of 60 there with every frame drawn.
- Guest memory as in log 26 (636 KB free at frame 2070, 2-28 compressions a second in the race,
  `mem` 1.2-3.2 ms in the tail of some windows): hitches, not the average.

### Mirror audit and log 26 (2026-11-11)

Log 26 (one player, night, in-car, every frame drawn, MIRROR OFF, JIT CCALLS on): p50 17.9-19.3
ms, `sh4` 9.5, `elan` 3.9-4.6 (walk 2.3-3.0), `rend` 1.1-1.5, sound 2.6. ~2 ms off a drawn frame
is what 60 fps needs. What the mirror still costs with MIRROR OFF, measured on the runner's day
route (in-car 9500-9900 against chase 10050-10300, PC histograms without the wait loop): the game
runs ~43K more SH-4 instructions a frame in the routines that run more in-car (~5% of 864K;
the chase view's own extra work elsewhere brings the net to +16K). So skipping the game's mirror
pass is worth at most ~0.45 ms of `sh4`, plus the walk of what it emits (~170 model calls with
their matrices, ~65 models a frame walked without drawing: perhaps ~0.5 ms). It needs the routine
found (IDAS3_WRITEVAL on a mirror model call, its backtrace) and a JIT hook stepping over it,
behind a new MIRROR value. Next on a night frame, besides that: `sync_fpscr` (1850-2070 call-outs
a frame in the race, the FR bank swap; ~0.2 ms estimated, "Tried and dropped" as not worth it
yet), the walk (item 1's compiled models), sound (item 8).

### Done 2026-11-11: 2 PLAYERS state swaps as block copies (2P log 3: 0.41 -> 0.09 ms)

2P log 2 (a night race, every frame drawn, FULL PHYSICS) gives player 2's physics 0.32-0.36 ms
a run and **0.41 ms of state swaps** a frame around it: ~29 KB moved a frame (the player's
block, the state kept outside it, the collision probes, the render states) one
`ReadMem32`/`WriteMem32` call a word, ~3.7K reads and ~3.7K writes that `[CC]` counts as C
call-outs to guest RAM. Guest RAM is host memory in host word order (`_vmem`'s direct path is a
plain load or store), so `naomi_2p.cpp`'s `readWords`/`writeWords` now take
`_vmem_GetBlockPtr` and memcpy, with the word loop kept for a range it has no pointer for.
Runner, 2P route (`--two-player 1 --p2-car 1 --gpu-tnl --list-cull 3`, player 2 driving to 140
km/h and steering): all 131 record digests and the `[2P]` lines identical to the old code, swaps
0.027 -> 0.006 ms a frame on the PC. Expected ~0.35 ms off every 2 PLAYERS FULL PHYSICS frame (read the
`[2P]   player 2's physics: ... ms of state swaps` line, and `[CC]` writemem/readmem `a3:RAM`
`from:other` should drop by ~3.7K a frame each). The night lighting counters in the same log
match the runner's (see the section below); its time needs a one-player night log.

### Done 2026-11-10: night lighting on the recording side (awaiting a night log)

The owner gave up on the headlight beam's look (HEADLIGHTS SOFT / SMOOTH kept as options, ARCADE
default) and asked for the lighting work to be as cheap as it can be. All of it exact, no option;
checked by the runner on every use. Night attract demo, `--gpu-tnl --point-lights 1 --list-cull 3
--display-lists 1 --colour-fetch 1`, new `[run]   lights per frame:` line and two numbers on the
`GPU fetch per drawn render` line:
- **Light commands that repeat the slot's light skip** (`tnl_SetLight`, `tnl_SetLightModel`: the
  same 8 words change nothing). ~53 of ~78 light commands a night frame, 36 of 48 by day; the light
  generation then stands, and with it the light plan and the keys below.
- **The last 4 point-lit picks kept** with their sets and keys (`s_setCache`, LRU), not only the
  last: a model's lists switch between a few picks (A, B, A), and each return to A was a build and,
  in the renderer, a new set compared by contents. Sets built 13-67 -> 8-18 a frame. Every reuse
  is rebuilt and compared by the runner (`STALE` 0).
- **The renderer finds matrices and light sets by key first** (`findShared`, naomi_elan_gx.cpp:
  each arena entry keeps its latest key, a key always names the same contents; then the old
  4-deep memcmp). Light-set compares (~600 bytes each) when recorded 120-244 -> 45-90 a render
  (the runner mirrors the Wii's lookup and checks every key hit against the contents: `STALE` 0);
  matrices (~110 bytes, one per model, by day too) are not counted on the runner.
- **A quick test before each light's box test** (`ModelBoxDistance::farFrom`): the box's centre in
  lighting space and a bound on the matrix's largest stretch (Gershgorin on A^T A) settle 55-67% of
  the 1.2-3.9K tests a frame with ~9 flops instead of ~25, never rejecting a light the full test
  keeps (the runner runs the full test on each: `WRONG` 0). The matrix part (a 3x3 inverse) is kept
  per instance matrix instead of made for every point-lit list.
- Parallel-lit sets kept by key are now checked too (rebuilt and compared: `STALE` 0).
- Day race route: record digests identical to the previous code on all 136 lines (renders to
  8705). Night demo: identical as far as two runs of the old code agree, then the demo's own
  timing drift (two runs of the same old code part at render 5825).
Expected on the Wii: ~0.3 ms off a drawn night frame (recording ~3.2 us a list at night against
~2.5 by day), a little by day (matrix compares). New [prof] line `night lights per frame:`
(commands decoded and repeats skipped, picks, lights tested and those the quick test settled);
`point-lit light sets per frame: N built, M reused`. To read: the ELAN T&L "GPU lists" ms of a
drawn night frame against log 23/24 (night Akina uphill, MIRROR OFF, SMALL 3D 4 px: 2.85-3.6 us a
list with a box).

### Done after log 25 (awaiting log 26)

- **ELAN PREFETCH** (RENDERING 2, second row, default ON; OFF (LEGACY) keeps the old walk).
  Log 25's night AUTO run puts a dropped frame's command walk at ~1.2 ms. The runner's day race
  with every other frame dropped (`--frameskip 1`, new `[run]   elan walk:` line) reads 1620-1740
  command headers on a dropped frame (~490 model calls stepped over from their summaries at
  night, the matrices, materials and lights between them skipped after their header) and ~3300 on
  a drawn one: ~0.7 us a header on a Wii, about one cache miss each, the walk waiting for every
  one. The lists outside models (the per-frame lists the game writes into ELAN RAM with
  store queues, ~140 KB a race frame) are read in order, so `execute` (naomi_elan_cmd.cpp) now asks
  for the lines `kPrefetchAhead` = 256 bytes ahead of the walk there (dcbt; one that misses the TLB is
  dropped, which in a list only loses the first lines of each new 4 KB page). Not inside models, where polygon lists'
  vertices (read by the GPU, not the CPU) lie between the headers; at a top-level polygon list the
  run-ahead jumps over its vertices. Nothing else changes (the runner's digests cannot move:
  MSVC has no `__builtin_prefetch`). Expected: the dropped-frame walk well under 1.2 ms, the drawn
  one under 2.4, if the 750CL keeps several prefetches in flight; to read in log 26's new line.
  First reading, 2P log 1 (two runs in 2 PLAYERS on a day course, so not log 25's night race;
  PREFETCH ON only): under FRAMESKIP AUTO a dropped frame's walk is 0.77-0.98 ms for 1580-2100
  headers (460-510 ns each), a drawn one 2.3-2.7 ms for 2960-3500 (760-810 ns); every frame drawn,
  2.4-2.7 ms for 3000-3500 (~770 ns). The dropped walk, almost all in lists outside models, is
  ~30% under the ~700 ns a header estimated before; the drawn one, half of it inside models where
  nothing is prefetched, is not. Needs the same course with ELAN PREFETCH OFF to count as measured.
- **`[prof]   ELAN walk:` line** (PROFILER): the walk's time (T&L left out) and the command
  headers it read, on frames already known to be dropped (`rend_FrameKnownSkipped`, which never
  takes the verdict, so AUTO decides exactly as before) and on the others, per frame and per
  command. Dropped renders come from `g_rend_dropped_renders` (counted where the verdict is used).
  The runner prints the same as `[run]   elan walk:`.
- Runner: `--arm-histo-from F`, the ARM7's ticks by code range (see the sound item in "What is
  left").

### Done after log 24 (log 25 confirmed: `snd` 0, night AUTO at 100% with ~36 shown)

- **No audio pacing with SOUND CLOCK REAL TIME** (wii_audio.cpp `wii_audio_push_sample`): in that
  mode the ring runs free whatever AUDIO SYNC says (a full ring drops the block being written, one
  23 ms skip, and its level comes back down) instead of blocking the emulation for the rest of the
  race. AUDIO SYNC keeps pacing the EMULATED clock, where waiting is what makes 100% (its hint and
  SOUND CLOCK's now say so). No option: the old behaviour cannot pace a real-time clock, it only
  stalled the game (log 24). The same result was available by hand with AUDIO SYNC = 0 (FREE RUN).

### Done after log 23 (log 25 confirmed: day in-car at 100% every frame drawn)

- **COLOUR LISTS** (new page RENDERING 2, the first row; default GPU FETCH, CPU (LEGACY) keeps the
  old path; runner `--colour-fetch`). Colour lists (layouts 0x042/0x04A with the vertex colour as
  material) were the only race lists still CPU-fed (~0.28 us a vertex on the Wii, 1.1-2.3K a render
  on log 23's day course, ~30 on Akina), and without a program they had no box either. Their
  colour word is ARGB, which no GX vertex format reads; but the guest's words are stored big-endian,
  so its bytes are A R G B in memory and GX reads the colour as RGB8 from one byte into the word,
  with alpha 255 (array bases at odd bytes were already proven by the S8 normals at record+1).
  Exact when the list's vertex alpha is not used (gxRend's `force_vtx_alpha_opaque`, e.g. TSP
  UseAlpha 0) or is 255 in every record: `alphaOpaque` (naomi_elan_fetch.inc) scans the records once
  per program (they cannot change while it is valid) and keeps the answer in spare bits of
  `FetchList.valid`; a list that fails stays CPU-fed but keeps its program, so its box. Colour adds
  a fourth index to a vertex (`putIndex<4>`, display-list entries with 4 attributes). Runner census
  first: in the race every colour list has UseAlpha 0 and every alpha at 255 (type 0 ~1.1K vertices a
  render, type 2 ~250, a few punch-through); only the car select's translucent untextured effects
  carry a real vertex alpha (14-58K vertices a render there), they stay CPU-fed. Runner, driving
  route in the in-car view (`--gpu-tnl --list-cull 3 --small 4 --mirror 0 --display-lists 1
  --colour-fetch 1`): colour lists 1242-1326 vertices a render all drawn from programs, fetch,
  display-list and box checks clean (only the 15 known menu lists mismatch, as without it), 0
  hazards. With `--colour-fetch 0` the record digests match the log-23 code. Expected on the Wii:
  ~0.3-0.65 ms off a drawn day frame, nothing at night.

### Done after log 22 (log 23 confirmed: ~0.5 ms off a drawn night frame)

- **Point-lit light sets reused** (no option: exact, and checked on every reuse by the runner).
  At night every point-lit list built its GX light set from scratch: 500 bytes cleared, 6-11
  lights tested against its box, copies merged by comparing every pair (`sameLight`), each kept
  light converted (`gxPointLight`, several divisions), then 500 bytes compared with the previous
  set. Neighbouring lists nearly always keep the same lights. Now `pointLightRecord`
  (naomi_elan_tnl.cpp) first picks the lights (box tests only; which copies merge is worked out once
  per light generation, `lightPlan`) and rebuilds the set only when the pick (slots, copies, light
  generation) differs from the last one built; otherwise the last set and its key stand, so the
  renderer shares it without comparing. Runner, night attract demo (`--gpu-tnl --point-lights 1
  --list-cull 3`): 87% of the sets reused (~40 built a frame instead of ~320); every reuse rebuilt
  and compared, 0 stale; record digests identical to the old code as far as two runs of the old
  code agree with each other (the night demo is not deterministic on Windows past render ~5300).
  One trap on the way: the cache first lived in the function template's statics, one copy per
  distance type, whose keys collided (4 stale sets caught by the check). New [prof] line
  "point-lit light sets per frame: N built, M the last one again". Expected on the Wii: ~0.5-1 ms
  off the recording of a drawn night frame.

### Done after log 21 (log 22 confirmed)

- **DISPLAY LISTS** (RENDERING, last row, default ON; `dlEntry` in naomi_elan_fetch.inc, `drawEntry`
  in naomi_elan_gx.cpp). Item 5 below. A cached list's draws used to be sent by the CPU index by
  index into the GX FIFO at every draw (`cached programs`: 1.75-1.9 ms a day-race render in log
  21, ~25 ns a vertex). Now each program gets a display-list entry once, in a 1 MB ring of its
  own: a short script of steps the CPU runs (set the array bases, call these bytes, CPU-feed these
  strips) and the strips as GX bytes (`0x9A`, count, the index once per fetched attribute), each
  call on 32 bytes and padded with NOPs, flushed from the cache. A draw is then a few CP writes
  and `GX_CallDispList` (which flushes GX's pending state, checked in libogc's code) per base.
  Entries follow their program's life (a rebuild clears them) and the ring's passes, like
  programs; one per list for the attribute count it is drawn with. The GPU reads the bytes after
  the CPU went on, so nothing may be written over an entry still queued: gxRend waits for the
  previous render's GPU work before a render draws, so only this render's calls count, and a
  write over one of them (or a ring pass after any) waits for the GPU first (`GX_DrawDone`, as
  gxRend does mid-render before recycling textures). OFF: the old path. ON costs the page pool 1
  MB (`_vmem.cpp` keeps that much more heap; the ring is taken at the first 3D).
  Runner (`--display-lists 1`, race route with the in-car mirror drawn): every entry's bytes read
  back as the GPU would and compared with the CPU draw, 0 mismatches beyond the GPU fetch check's
  own (the 15 known menu lists, identical with display lists off: see the runner table); every
  entry written checked against the ones still queued, 0 hazards. A race render draws ~520-730
  lists from entries taking ~330-545 KB. With a 4 MB ring entries are rewritten exactly when their
  program is (block for block); the 1 MB ring passed 5 times over frames 9300-13000 (~1200 extra
  rewrites, 57 GPU waits), on a route that builds far more programs than a Wii race (log 21: 0-22
  a block in steady stretches). The launcher's settings (MIRROR OFF, 1 MB) with the chase view
  too: 290-690 lists from entries taking 210-490 KB, 3 ring passes and 18 GPU waits over frames
  9000-13000, 0 hazards, the same 15 known mismatches. Expected on the Wii: most of `cached programs` off every drawn
  frame, ~1.2-1.6 ms by day, ~1 at night; the GP does the same work. New [HW] line "ELAN display
  lists a render" (lists drawn from one, entries written, ring passes, GPU waits and their ms);
  `cached programs` in the "ELAN draw" line then times the calls.

### Done after log 20 (log 21 read it)

- **IN-CAR REFLECT** (RENDERING, last row; AS REFLECTIONS = legacy, 1 FRAME IN 16 = default,
  FROZEN). The in-car view shows no body of the player's car; the car reflection map (`sh4` ~1.3
  ms a build, ~0.33 ms a frame at the owner's REFLECTIONS 1 in 4) is seen there only on the
  opponent's car. The ELAN counts MODEL commands with user clip mode 2 (`g_elan_mirror_seen`: the
  rear-view mirror pass, drawn or not); `naomi_ReflectionSkip` treats the view as up while that
  count moved since its last call, and one build past it, and then builds one frame in N instead
  of REFLECTIONS' rate. The hook was already in the JIT and the runner. Expected ~0.25 ms off every
  in-car frame, dropped or drawn, on the owner's settings (~0.3 with FROZEN). New [prof] line
  "IN-CAR REFLECT: N of M frames without a reflection map build".
  Runner (CPU path snapshots, REFLECTIONS 4): the race starts in the in-car view and `view`
  toggles it with the chase view. The in-car view has ~75 plain, ~168 mirror (clip mode 2) and
  ~230 clip-mode-3 models a frame, the chase view ~430-450 plain ones and no mirror model, so the
  chase view always rebuilds as REFLECTIONS says. In-car with the opponent far ahead (frames
  10275-13000, 110 snapshots), FROZEN gives the same pixels as the legacy run. With the opponent
  alongside and pulling away (9530-9780), FROZEN changes up to 9.3K pixels of its body (1.9K by
  more than 24/255); 1 FRAME IN 16 up to 3.7K, at most 7 by more than 24/255 except in the frame
  just before a rebuild (463). 648 of 691 in-car builds not made at 16.
- **TIRE SOUND default 4** (owner, log 20); MENU SOUND stays 5. A saved options.txt keeps its value.

### Done after log 19 (log 20 confirmed)

The owner wants 100% speed in the race with as many frames shown as possible, and asked for
options that lower the picture for it. All three are on the RENDERING page.
- **MIRROR** (default OFF). In the in-car view the game draws its rear-view mirror as a second
  pass of the scene from behind, models whose MODEL command has user clip mode 2 (inside the TA's
  user clip rectangle). OFF: such a model is not drawn (`mirrorModel`, naomi_elan_cmd.cpp): stepped
  over from its summary when its block is unchanged since it was walked (the mirror shows the main
  view's own models, so most are), else walked with its lists dropped (`s_dropLists`), which
  leaves every state as drawing would; its lists take no depth slice, and a walk inside it never
  rewrites a summary (no boxes). Runner, in-car stretch of the route (frames 10380-10500): drawn
  lists 760 -> 525 a render (-31%), fetched vertices 81.7K -> 61K, 174 models a frame not drawn
  (101 from summaries), 0 mismatches, 0 stale. Snapshots: only the mirror's rectangle
  (161,32)-(479,95) changes, to the dark panel behind the mirror; every other pixel and every
  frame outside the in-car view is identical. Expected on the Wii: ~1.5-2 ms in the in-car view.
- **SMALL 3D** (default DRAW ALL; 2, 4 or 8 pixels). A list or a whole model whose box diagonal,
  at its nearest depth, spans fewer pixels than that is not drawn (`tooSmall`,
  naomi_elan_tnl.cpp; `BoxView::scale` = the projection's x scale times the matrix's longest
  axis). Needs 3D LIST CULL. A model is small only if all its lists are. Runner census of the
  drawn race lists (`IDAS3_FAR_CENSUS`): ~100-160 of ~600 under 4 px a render (far trees, posts),
  8K of 55K vertices under 2 px. At 4 px: drawn lists 592-617 -> 431-451 (-27%), triangles
  30.5-35.4K -> 26.3-28.2K, ~45 more whole models stepped over, 0 stale, 0 mismatches. The look is
  unchecked (snapshots draw no GPU lists): far objects under 4 px are dots; a distant opponent
  may blink out. Expected ~1 ms a frame on the Wii at 4 px.
- **REFLECTIONS 1 FRAME IN 8** (new value): the remaining 1-in-4 cost of the car's reflection
  map (~0.35 ms of `sh4` a frame) halved, on dropped frames too. In the in-car view the map is not
  seen at all (log 15's snapshots), so 8 costs nothing there.
- **SOUND LEVELS** (AUDIO page, last rows, not a speed item): MENU SOUND and TIRE SOUND, 0 =
  muted to 10 = as the game, default 5 (-12 dB: level n plays at (n/10)^2 of the amplitude). At
  each key-on the sample is fingerprinted (format, loop flag, loop points, first 32 bytes in guest
  byte order) and a match adds attenuation to the channel's direct and effect sends until its next
  key-on (sgc_if.cpp). Menu cursor `5D13B4DD` (one-shot 16-bit, full level, keyed 1-6 frames after
  each steering move on the make, course and card screens and never without one); tire squeal
  `AE718FA6` (a looped ~330 Hz whine, keyed at the start's wheelspin and at each slide under
  steering, never when driving straight). The rail impact keyed with it (`2E3AE289`) is left alone.

### Done after log 18 (log 19 confirmed)

- **PAGE GUARD** (RECOMPILER 2, last row, default ON; `WiiGuestMem_ElanGpuPage` and `pte_map`,
  wii_fastmem.cpp). The GPU fetch checks every ELAN page a render uses once a render (~800 in a
  race on Windows): a page-table search, a `dcbf` + `sync` of the PTE's line and a read from
  memory, the page table's lines most likely out of the cache by the next render. A page found
  unwritten 16 checks in a row is now mapped read-only (PP=3), and while it is its check is a
  flag. The next store to it takes a protection fault: `guest_fault` maps any backed page
  read-write with C set (as for a demand fault), and `pte_map` drops the guard of every ELAN page
  it maps, so the following check sees C exactly as before. The same goes for a page whose PTE was
  evicted (any access maps it anew) or compressed (not resident, seen before the guard). Exact, as
  the C bit is: every write to ELAN RAM goes through its one window (no mirrors). Off where the MMU
  ignores PP (`s_zero_page_ro`: Dolphin). Pages the game writes every frame never qualify; a page
  written every ~17 renders pays a fault (~1-2 us) each time. Wii only: the Windows runner's
  fetch check models writes with a write watch, unchanged. New line with PROFILER:
  `[HW] ELAN page checks a render: N, X ms (Y ns each) | full F (W written), read-only R | made
  read-only M, guards dropped D`. It times the checks with PAGE GUARD OFF too, which gives the
  old cost.

### Done after log 17 (log 18 confirmed)

- **SOFT FLOAT** (RECOMPILER 2 since log 18, default NATIVE; `emu/naomi/naomi_softfloat.cpp`, hook in
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
- **MEM SWEEP** (RECOMPILER 2 since log 18, default SHORT; `page_alloc`, wii_fastmem.cpp). The fault
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
- **JIT COLD ENTRY** (RECOMPILER 2 since log 18, default ON). The timeslice exit goes after the block's
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
- SCREEN BUFFERS (2026-11-14, log 34): two XFBs tore (a copy onto the picture still on screen when the
  last flip had not reached the retrace); a third, chosen against libogc's current/next framebuffer.
  See "Log 34".
- Texture cache modes (owner's choice: NORMAL); 4/8bpp CI paths; TMEM cache.
- Texture cache census (2026-11-11, runner env `IDAS3_TEXCACHE`, boot to a Myogi race): NORMAL
  decodes 0.25 textures a render (2 arena wraps in the run, at the race's load), so the texture
  cache is not a speed lever in steady state. VERY FAST+ as nullDC4Wii built it aliased on NAOMI 2
  (33.5 decodes a render, 56-93 in the race, ~31% of binds showing another texture's pixels): kept
  as VERY FAST+ (OLD). VERY FAST+ is now NORMAL plus a per-render bind memo: a race render binds
  ~600-750 textured headers, only ~180-210 distinct, so ~70% of binds skip the map probe, the
  descriptor read and the guest VRAM read (gain unmeasured on a Wii: 2-3 cache misses a bind
  would be ~0.1-0.4 ms a drawn frame). Initial D 3 has no VQ 565 texture: VQ AS CMPR does nothing
  in this game (its encoder was still fixed: 3.15% of blocks fell in the 3-colour + transparent
  mode, now none).
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
is the default (SOUND CLOCK). The cost does not need measuring, and the question is closed.
With REAL TIME nothing may wait on the audio ring: log 24 (AUTO, night) spent 25% of its time
there; the push runs free in that mode since (Done after log 24).

## Tried and dropped

- Software T&L only: cannot reach 60 fps on Broadway (the lighting rewrite gave ~20%).
  Paired-single CPU T&L was considered; GPU T&L was chosen instead.
- BAT-mapping hot guest RAM: DTLB walks were 0-1% on hardware.
- Real-time sound clock: broke the streamed music on 2026-10-17; OK in the log 15 build (see Sound).
- Inlining the FR bank swap in `sync_fpscr` (~1.5K real swaps a frame): ~0.2 ms, not worth it
  yet. Done after log 26 (JIT FR SWAP), when the margin left made it worth it; log 27 suggests
  the estimate was far too low (see "Done 2026-11-11: four items").
- SOUND MIX = HALF (2026-11-11, kept as an option, default FULL): no gain in log 27. Mixing is a
  small part of a voice; the envelope, stream and LFO steps it keeps are the rest, and held voices
  still read their sample for the attack-restart check. Halving the sound work would need the
  voice state stepped at half rate, which the sound CPU can see (envelope levels): not exact.
- Trusting Dolphin's numbers for anything cache-related.
- MIRROR OFF summaries for mirror-only models (after log 23): ~65 mirror models a frame are still
  walked in-car, because only models with a main-view (boxed) summary are stepped over. A summary
  without a box written by the mirror walk raised the stepped-over share 100 -> 118 of ~167 on the
  runner, but those summaries evicted main-view ones from the 2048-slot model table (record digests
  split from render ~7169); allowed only into free slots, the gain fell to ~3 models a frame (~0.05
  ms) and digests still split later (render ~9089, cause not found). Reverted; the runner's check of
  every skipped model now also compares its list and vertex counts. Retried by mistake on
  2026-11-11 (a box-less summary not replacing a boxed one of the same model): 99 -> 116-117 of 168
  on the day route in-car, digests identical up to render ~7078 only (before the known split at
  ~7169), estimated ~0.1 ms on a Wii. Reverted again: check this list first.

## What is left, best first

After log 33: read log 34's `[prof]   GAME COPIES per frame:` line first (the word loop's direct
calls native, few turned down) and the `[PCS]` share of 0C20A000, `runBursts`, 0C222000. Then, by
size: the matrix library (~4% of a frame, `frchg`/`fschg` in the recompiler, see log 33), the
recording's list lookup (~3.5%), the walk's header misses, `tnl_SetMatrix`. JIT FPU BANKS is built
for the matrix library; next in the recompiler, if it pays: `ftrv` loading XMTRX once per block
instead of 16 loads each, and the block-end swap of a stretch left crossed (0C1F6504's
`frchg; fmov x4; rts`) folded into the next block.

After log 32 (the first PC SAMPLER profile): the chase view is CPU-bound (idle 0.5%), ~2 ms short of
60 fps; in-car has 8-16% to spare. GAME COPIES is built (log 33 to read: the `[PCS]` share of
0C20A2A0/0C1D4F4C and the trampolines, `sh4` in the chase view). Next candidates, by the profile:
1. The recording's list lookup (~3.5% chase: `findList` 1.4%, `pagesSettled` 0.7%, `list_box`): a
   model's remembered fetch slot (CACHE HINTS keeps it per head) checked first, the hash probe only
   when it does not match (exact: one entry a key). ~0.1-0.2 ms.
2. The walk's own loop (~5.4%, 1.9-2.1% at its header read): headers inside models past the 8
   remembered ones still miss; more heads a model, or the model's block prefetched whole when small.
3. `tnl_SetMatrix` (1.1%): a 160-byte matrix command read and converted per call.
4. The SH-4 game code: no single block over 1.2% once the copies are gone (0C1DC008, an `fmov.d`
   store-queue burst, needs the FPU registers), so the JIT's own cost per instruction (I-cache
   misses ~10K/ms) is the lever there: the new `[PCS] SH-4 code by 1 KB` line says which routines.
5. Sound (~2.3 ms, ~7.5% of samples with the ARM7 JIT): no exact lever known.

After log 31 (and 31b, where the first sampler build froze): the next step is measurement, not a
guess. Log 32 with the fixed PC SAMPLER (night Akina, in-car then chase, JIT CCALLS off) says which C
functions take the ELAN phase's ~1.5M instructions a frame and the render's time, which SH-4 blocks
take the most time (not entries) in each view, and how much of the frame the CPU idles. Log 31b
found no net CACHE HINTS gain (walk slower, T&L faster): the sampler's split of the walk will say
whether its model-call prefetches should go. Candidates to check against it:
the walk's per-command cost (~350 instructions), the recording per list (~1400), the JIT'd game's
instruction fetch (~10K L1I misses/ms: block prologue/epilogue size, hot/cold layout), the VRAM32
copy loop 0C20A2A0 and the SQ copy loop 0C1D4F4C in the chase view.

After log 30: night in-car is ~100% with every frame drawn except its heaviest stretch (53.6-58
fps, ~17.5 ms); the night chase view is 45-54 (p50 ~19-20 ms), ~2.5-3 ms short, of which `sh4`
is +2 ms of the game's own work. CACHE HINTS (awaiting log 31) aims at the render's and the walk's
memory stalls. Next, by what log 31's counters say: if the ELAN phase is stall-bound, the layout of
what the recording writes and the render reads (one pass, a smaller Item, light sets by index);
if instruction-bound, its code. For the chase view: a JIT HOTBLOCKS run there (which game routines
grow against in-car; the mirror pass was found that way). Under FRAMESKIPPING AUTO the chase view
should already hold 100% with ~70% of frames drawn.

After the five items of 2026-11-12 (log 30 read them above): first read what they gave (`elan` T&L us a
list with a box, the walk's ns a command, `rend`, `sh4`), and the night view/course that showed 90%.
Left after them: the walk's memory stalls (a compiled form of models only if a log shows the
headers inside models still stall, see item 2 above), per-list GX state in gxRend's draw loop for
ELAN items (texture lookups: TEXTURE CACHE VERY FAST+ bind memo, untested), sound (~2.2 ms, no
exact lever left: SOUND STEP took the last cheap ones), the game's own SH-4 work.

After log 27: night in-car runs 59-60 fps with every frame drawn for most of the race (MIRROR
OFF + GAME SKIP, JIT FR SWAP); its heaviest stretch is ~17.6 ms (55-58 fps). Under AUTO (not run
since) S ~11 and D ~15.5-17.6 should hold 100% with nearly every frame drawn, so the owner's
target (100% at ~45 shown) looks met at night in-car. Not measured since: the chase view at night
(no mirror to skip; FR SWAP applies), 2 PLAYERS at night (GAME SKIP stays on in 2P). Left on a
drawn night frame: `sh4` ~7.3, `elan` 3.1-4.8 (walk 1.6-2.4 ms at ~900 ns a command, T&L 1.3-3.2
at 3.1-4.1 us a box), `rend` 1.1-2.1, sound ~2.2 (HALF dropped). Below: the state after log 25.

After log 25: the owner's target is 100% speed with ~45 fps shown under FRAMESKIP AUTO. The day
course runs its in-car view at 100% with every frame drawn (log 25, COLOUR LISTS); night Akina
under AUTO gives 100% with ~36 shown (S ~13.4, D ~19-20) and needs ~1.3 ms off both S and D, or
~1.7 off D alone. ELAN PREFETCH is built for log 26 (the walk: ~1.2 ms a dropped frame, ~2.4 a
drawn one). Night `sh4` is ~1.3 ms over day's in every log; the runner's night demo runs no more
SH-4 instructions than the day race (690-825K against 800-815K a frame, reflection routine
included), but spends them differently: 0C022580-0C022A3F (FPU code over 32-byte records, likely
the per-object light choice) 4.7% against 1.8% (day histogram of 2026-11-01), and ~6% in five
regions the day race does not run (0C0D1000, 0C0E7000, 0C140000, 0C1A9000, 0C206000, ~1% each).
So on a Wii the gap is game code spread over several routines, no kernel to replace (the same
shape as log 16's race-vs-demo gap).
DISPLAY LISTS (item 5, log 22) and the light-set reuse (log 23) took ~0.5 ms each off a drawn
frame; COLOUR LISTS (item 3) ~0.4 by day (log 25). Next
on a drawn frame: the ELAN command walk (2.0-2.8 ms, now the largest ELAN cost: compiled models,
item 1), the rest of the per-list draw (item 4, ~2.1-2.4 us a list, ~0.8-1.4 ms), the recording
(~2.5 us a box by day, ~3.2 at night). On a dropped frame besides
the game's own code: sound (`arm` 1.2 + `aica` ~0.85 at 100%, item 8: the ARM7 already runs in
batches of 4 timeslices and skips its idle loops; the AICA mixes ~6 voices at 44.1 kHz, half rate
would halve it at a cost in sound quality) and the ELAN walk (~0.9 ms: ~1400 command headers a
frame, each a likely cache miss; the next header is already prefetched). Not a lever: the car's
reflection map (log 21, IN-CAR REFLECT changed nothing measurable).

Numbers are kept because the items refer to each other. After log 18 the order is: item 1 (`elan`
4.4-7 ms), item 6 (SH-4 9.0 ms after SOFT FLOAT: spread out, see "The target") and item 8 (sound,
2.2 ms, never profiled).
Sending the index draws as display lists (`cached programs`, 1.1-1.9 ms a render) is done after
log 21 (item 5). JIT COLD ENTRY needs an A/B before it counts as a gain.

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
2. **Cheaper fetch validation.** Done: PAGE GUARD (log 19: ~0.055 ms a race render left, ~60%
   of it the ~20 pages written each render, ~1.35 us each). Not worth more work.
3. **Colour lists through the GPU fetch.** Done after log 23 (COLOUR LISTS, see "Done after log
   23"): read as RGB8 one byte into the ARGB word, when the alpha is 255 or not used; on the
   day course ~0.3-0.65 ms a drawn frame. Left: the lists whose vertex alpha shows (car select's
   translucent untextured effects, none in a race on the runner) would need the TEV swap: in
   the records the word is A,R,G,B in memory, so GX's RGBA8 gets R=A, G=R, B=G, A=B, a rotation
   that `GX_SetTevSwapModeTable` (ras side, R<-G, G<-B, B<-A, A<-R) undoes; lit ones would also
   need the light colours rotated and ALPHA0 lit.
4. **The rest of the ELAN draw.** Of 6.0-6.5 ms of GPU lists (log 11, before item 9), only
   2.2-2.5 ms is spent sending cached programs; 3.6-4.3 ms is everything else inside
   `drawItem`, over ~960 lists: the CPU-fed colour vertices (item 3), matrices, light sets,
   the list lookup, and each list's whole program read for its checksum (`progSum` in
   `fetchProgram`, every list every render; the check has never fired on hardware: sample it,
   one list in 32). Item 9 takes ~40% of the lists away. The lookup is done twice now (record
   and draw): keep the slot in the item. Cheaper page checks without a bitmap (item 2): a
   per-page pointer to the PTE and an uncached read of it needs no `pte_find`, `dcbf`, `sync`.
5. **GX display lists** for cached programs. Done after log 21 (DISPLAY LISTS): a 1 MB ring of
   entries next to the program ring, INDEX16 as before (one call per array base, `SetArray`
   being outside display lists). Left: INDEX8 bytes for segments under 256 vertices (half the
   ring, at the cost of a second vertex format), entries written outside the render if the GPU
   waits show up in the [HW] line.
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
8. **Sound side (~2.3 ms a race frame at 100%: `arm` 1.3 + `aica` 1.0).** First look after log
   25. ARM7: the runner's `--arm-histo-from` over the day race's driving stretch (frames
   11600-13000) gives ~167.6K ticks a frame, the full clock the port gives it (200 MHz / 20 =
   10 MHz), spread over ~25 code ranges (the largest 9%, 000A40-000A7F; others at 000600-001140
   and 007180-007540). So the driver is busy, not spinning in one poll loop the idle skip could
   charge for. nullDC4Wii measured batching at 1 -> 4 slices worth only 7.4 -> 6.2% `arm` (the
   port uses 4), with 8 and 16 making music wobble, and a halved ARM clock broke audio timing in
   other games: no cheap ARM lever. On a Wii, JIT CCALLS also turns on the ARM JIT's census
   (`[ARMJIT]` / `[ARMHOT]` lines once a second: instructions a second, hottest blocks, time in C).
   AICA: ~0.7 ms of `aica` is voice mixing (`vox`, ~5 voices, ~150 cycles a voice-sample through
   `ChannelStepFast`'s calls) and ~0.35 ms the per-sample rest (`libAICA_TimeStep`: three timers,
   two interrupt updates and the sample push, 44,100 times a second). Mixing a channel several
   samples at a time would cut both but changes how the ARM sees channel positions between
   samples: a project of its own, checkable on Windows by hashing every output sample.
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
