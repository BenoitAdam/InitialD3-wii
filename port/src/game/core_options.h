// Core presets inherited from nullDC4Wii (renderer, texture cache, SH-4
// recompiler), chosen on the launch pages. Names and values follow
// nullDC4Wii's menus; the game runner copies the chosen values into the
// core's preset variables before it starts (game_runner.cpp, same order).
#pragma once

namespace idas3 {

// The enum order is the pages' order in the launcher.
enum CoreOptionPage
{
    PAGE_GENERAL,        // what a player changes: 2 PLAYERS, mirror, frameskip, pads
    PAGE_DEPTH,          // RENDERING 1
    PAGE_DEPTH_2,        // RENDERING 2: textures, 3D T&L, effects (new rendering rows go here)
    PAGE_AUDIO,
    PAGE_CORE,           // SH-4 CPU choice (interpreter / recompiler), idle skip, guest memory
    PAGE_RECOMPILER,
    PAGE_RECOMPILER_2,   // the first one is full: new recompiler rows go here
    PAGE_FIXES,
    PAGE_LOGS,           // PROFILER, JIT CCALLS, JIT HOTBLOCKS, AUDIO STATS, PC SAMPLER: everything for logs
    kCoreOptionPages
};

inline constexpr const char* kCoreOptionPageNames[kCoreOptionPages] = {
    "GENERAL", "RENDERING", "RENDERING 2", "AUDIO", "CORE", "RECOMPILER", "RECOMPILER 2", "FIXES", "LOGS"};

enum CoreOptionId
{
    // General
    OPT_SPEED_LIMIT,
    OPT_IDLE_SKIP,
    OPT_ELAN_GX,
    OPT_ELAN_FETCH,
    OPT_FRAMESKIP,
    OPT_MODEL_SKIP,
    OPT_TEX_CACHE,
    OPT_VQ_CMPR,
    OPT_TMEM_CACHE,
    OPT_4BPP,
    OPT_8BPP,
    OPT_FPS_OVERLAY,
    OPT_PROFILER,
    OPT_JIT_CCALLS,
    OPT_JIT_HOTBLOCKS,
    // Rendering
    OPT_FIXED_DEPTH,
    OPT_DEPTH_CLIP,
    OPT_HUD_PASS,
    OPT_ISP_DEPTH_FUNC,
    OPT_TRANS_SORT,
    OPT_PUNCH_THROUGH,
    OPT_AUTOSORT,
    OPT_SPLIT_SCREEN,
    OPT_VRAM_PRESENT,
    // Recompiler
    OPT_JIT_SBP,
    OPT_JIT_ICI,
    OPT_FASTMEM,
    OPT_BCACHE,
    OPT_DYN_IC,
    OPT_FPU_PIN,
    OPT_JIT_ALIGN,
    OPT_IFB_FLUSH,
    OPT_JIT_NEWOPS,
    OPT_JIT_TFWD,
    OPT_JIT_FMOV,
    OPT_JIT_CARRY,
    OPT_JIT_MAC,
    OPT_JIT_FSCHG,
    OPT_JIT_CR0,
    OPT_BLOCKCOPY,
    OPT_JIT_FSQRT,
    OPT_JIT_FIPR,
    OPT_VRAM32_FAST,
    OPT_SQ_FAST,
    // Audio
    OPT_AUDIO_SYNC,
    OPT_AUDIO_BLOCK,
    OPT_AUDIO_DRC,
    OPT_SOUND_CLOCK,
    OPT_AUDIO_STATS,
    // Fixes (nullDC4Wii presets ported from the PS3 port, flycastPS3)
    OPT_ARM7_STR_ALIGN,
    OPT_ADPCM_LOOP,
    OPT_ARM7_RESET_FIX,
    OPT_AICA_RACE,
    OPT_AICA_CA_CLAMP,
    OPT_JIT_LINK_GUARD,
    OPT_DIV32_FIX,
    OPT_INTERP_FIX,
    OPT_SPG_FIX,
    OPT_TA_GUARD,
    OPT_INF_VTX,
    // Rendering, added last so that no saved or autolaunch position moves
    OPT_ELAN_POINT,
    OPT_ISP_CULL,
    OPT_ELAN_BOX,
    OPT_REFLECTIONS,
    OPT_DEPTH_MEASURE,
    // Their pages show them last, wherever they are here.
    OPT_JIT_COLD_ENTRY,
    OPT_DIMM_AHEAD,
    OPT_FIFO_WAKE,
    OPT_MEM_SWEEP,
    OPT_FIFO_SIZE,
    OPT_SOFT_FLOAT,
    OPT_PAGE_GUARD,
    OPT_MIRROR,
    OPT_SFX_MENU,
    OPT_SFX_TIRE,
    OPT_SMALL_CULL,
    OPT_INCAR_REFLECT,
    OPT_DISPLAY_LISTS,
    OPT_COLOUR_FETCH,
    OPT_ELAN_PREFETCH,
    OPT_TWO_PLAYER,
    OPT_GC_LAYOUT,
    OPT_P2_CAR,
    OPT_P2_SCENERY,
    OPT_MOTION_FX,
    OPT_MOTION_LEVEL,
    OPT_TEST_MODE,
    OPT_BETWEEN_WARP,
    OPT_HEADLIGHTS,
    OPT_JIT_FRSWAP,
    OPT_MODEL_PREFETCH,
    OPT_SOUND_MIX,
    OPT_LIST_MATHS,
    OPT_WALK_DECODE,
    OPT_DRAW_MEMO,
    OPT_SOUND_STEP,
    OPT_SH4_SLICE,
    OPT_CACHE_HINTS,
    OPT_PC_SAMPLER,
    OPT_GAME_COPIES,
    kCoreOptionCount
};

struct CoreOption
{
    const char* label;         // 14 characters, the menu's label column
    const char* hint;          // shown under the list while selected
    int page;
    int defaultIndex;          // position in names[] chosen by default
    int count;
    const char* names[11];     // 17 characters at most
    int values[11];            // the preset value behind each name
};

inline constexpr CoreOption kCoreOptions[kCoreOptionCount] = {
    // ---- General -----------------------------------------------------------
    {"SPEED LIMITER ", "caps emulation at 100% (the menus otherwise run up to 160%)", PAGE_GENERAL, 0, 2,
     {"OFF (UNCAPPED)", "ON (CAP 100%)"}, {0, 1}},
    {"IDLE LOOP SKIP", "Initial D's wait-for-frame loop runs 30x fewer times (same timing)", PAGE_CORE, 1,
     2, {"OFF", "ON"}, {0, 1}},
    {"3D T&L        ", "GPU: Wii GPU lights opaque 3D (prototype), CPU: all in software", PAGE_DEPTH_2, 1,
     2, {"CPU (SOFTWARE)", "GPU (PROTOTYPE)"}, {0, 1}},
    {"3D VTX FETCH  ", "GPU T&L: the GPU reads 3D models itself (NO PATCHES: CPU feeds strips over gaps)",
     PAGE_DEPTH_2, 1, 3, {"OFF (CPU FEEDS)", "ON (GPU READS)", "ON, NO PATCHES"}, {0, 1, 2}},
    {"FRAMESKIPPING ", "dropped frames also skip their NAOMI 2 3D transform (AUTO: when behind)", PAGE_GENERAL, 0,
     5, {"0 (DEFAULT)", "1 (1 OF 2)", "2 (2 OF 3)", "AUTO", "AUTO MAX"}, {0, 1, 2, 3, 4}},
    {"DROPPED 3D    ", "frames the frameskip drops: SKIP MODELS steps over 3D model lists (faster)",
     PAGE_GENERAL, 1, 2, {"WALK ALL", "SKIP MODELS"}, {0, 1}},
    // (OLD) values last: autolaunch.txt holds each row's index.
    {"TEXTURE CACHE ", "VERY FAST+: NORMAL + a memo of each render's binds. (OLD) ones alias here",
     PAGE_DEPTH_2, 3, 6,
     {"VERY FAST", "VERY FAST+", "FAST", "NORMAL (DEFAULT)", "QUALITY (SLOW)", "VERY FAST+ (OLD)"},
     {0, 7, 1, 2, 3, 6}},
    {"VQ AS CMPR    ", "VQ 565 textures as GX CMPR (4 bits a texel); ON (OLD): the old encoder",
     PAGE_DEPTH_2, 0, 3, {"OFF (DEFAULT)", "ON", "ON (OLD)"}, {0, 2, 1}},
    {"TMEM CACHE    ", "keep the GPU texture cache warm", PAGE_DEPTH_2, 1, 2, {"OFF (LEGACY)", "ON (FASTER?)"},
     {0, 1}},
    {"4BPP MODE     ", "4-bit palette textures", PAGE_DEPTH_2, 1, 3, {"I4 STUB", "4BPP OPTIMIZED", "CI4 (FAST)"},
     {0, 1, 2}},
    {"8BPP MODE     ", "8-bit palette textures", PAGE_DEPTH_2, 1, 3, {"I8 STUB", "8BPP OPTIMIZED", "CI8 (FAST)"},
     {0, 1, 2}},
    {"SPEED OVERLAY ", "on-screen FPS and emulation SPEED %", PAGE_GENERAL, 1, 2, {"OFF", "ON"}, {0, 1}},
    {"PROFILER      ", "logs where each frame's time goes (sh4/elan/rend/tex...)", PAGE_LOGS, 0, 2,
     {"OFF", "ON"}, {0, 1}},
    {"JIT CCALLS    ", "with PROFILER: SH-4 code's calls into C per second (cheap)", PAGE_LOGS, 0, 2,
     {"OFF", "ON (CENSUS)"}, {0, 1}},
    {"JIT HOTBLOCKS ", "with PROFILER: hottest SH-4 blocks and their code (~12% slower)", PAGE_LOGS, 0, 2,
     {"OFF", "ON (DIAGNOSTIC)"}, {0, 1}},
    // ---- Rendering (depth, sort, frame assembly) ---------------------------
    {"FIXED DEPTH   ", "fixed near/far planes (TIGHT: less Z-fighting, extremes clip)", PAGE_DEPTH, 0, 3,
     {"OFF (DYNAMIC)", "WIDE", "TIGHT"}, {0, 1, 2}},
    {"DEPTH CLIP    ", "NEAR MARGIN for real Wii, NO CLIP matches Dolphin", PAGE_DEPTH, 1, 3,
     {"OFF (LEGACY)", "NEAR MARGIN (WII)", "NO CLIP (DOLPHIN)"}, {0, 1, 2}},
    {"HUD PASS      ", "brings back a HUD clipped by FIXED DEPTH = TIGHT", PAGE_DEPTH, 0, 3,
     {"OFF", "OVERLAY (NO ZW)", "PROTECT (Z-WRITE)"}, {0, 1, 2}},
    {"ISP DEPTH FUNC", "per-polygon depth test (experimental)", PAGE_DEPTH, 0, 3,
     {"OFF (LEGACY)", "ON (OPAQUE/PT)", "ON (ALL LISTS)"}, {0, 1, 2}},
    {"TRANS SORT    ", "back-to-front sort of translucent polygons", PAGE_DEPTH, 0, 2,
     {"OFF (FASTER)", "ON (CORRECT)"}, {0, 1}},
    {"PUNCH THROUGH ", "punch-through list order and alpha test", PAGE_DEPTH, 0, 2,
     {"OFF (FASTER)", "ON (CORRECT)"}, {0, 1}},
    {"AUTOSORT      ", "real per-pixel translucent sort (very slow)", PAGE_DEPTH, 0, 5,
     {"OFF", "1 LAYER (SLOW)", "2 LAYERS (SLOW)", "3 LAYERS (SLOW)", "4 LAYERS (SLOW)"}, {0, 1, 2, 3, 4}},
    {"SPLIT SCREEN  ", "MULTI-PASS: one picture from renders that each draw part of it", PAGE_DEPTH, 0, 4,
     {"OFF (LEGACY)", "TILE CLIP", "MULTI-PASS", "BOTH"}, {0, 1, 2, 3}},
    {"VRAM PRESENT  ", "copies of video memory between 3D frames (NOT DURING 3D: no flashes)", PAGE_DEPTH,
     1, 2, {"ALWAYS (LEGACY)", "NOT DURING 3D"}, {0, 1}},
    // ---- Recompiler (SH-4 dynarec) ------------------------------------------
    {"JIT SBP       ", "stale/self-modified block guard", PAGE_RECOMPILER, 1, 3,
     {"OFF", "KNOWN (DEFAULT)", "ALL RAM (SLOW)"}, {0, 1, 2}},
    // Initial D 3 flushes its I-cache once (at boot) in 12000 frames, race
    // included: IGNORE would gain nothing and risks stale code.
    {"JIT ICI       ", "what a guest I-cache flush does (Initial D 3: once, at boot)", PAGE_RECOMPILER, 0, 3,
     {"CLEAR ALL (DEFLT)", "IGNORE (NEED SBP)", "IGNORE + SBP=ALL"}, {0, 1, 2}},
    {"FASTMEM       ", "MMU-mapped JIT memory", PAGE_RECOMPILER, 1, 2, {"OFF (LEGACY)", "ON (FASTER)"}, {0, 1}},
    {"JIT BCACHE    ", "1-cacheline dynamic jump dispatch", PAGE_RECOMPILER, 1, 2, {"OFF (LEGACY)", "ON (FLAT)"},
     {0, 1}},
    {"JIT DYN IC    ", "bake the last target at each branch site", PAGE_RECOMPILER, 2, 3,
     {"OFF", "ON (JSR/JMP)", "ON (+RTS)"}, {0, 1, 2}},
    {"FPU PIN       ", "pin fr0-15 to real FPU registers", PAGE_RECOMPILER, 1, 2,
     {"OFF (LEGACY)", "ON (EXPERIMENTAL)"}, {0, 1}},
    {"JIT ALIGN     ", "align JIT blocks to cache lines", PAGE_RECOMPILER, 1, 2, {"OFF (LEGACY)", "ON (32B LINES)"},
     {0, 1}},
    {"JIT IFB FLUSH ", "spill only the registers the opcode touches (+1%)", PAGE_RECOMPILER, 1, 2,
     {"OFF (FULL SPILL)", "ON (SELECTIVE)"}, {0, 1}},
    {"JIT NEW OPS   ", "jit rotcl/rotcr/tas.b/sr/fpscr (9 ops)", PAGE_RECOMPILER, 1, 2,
     {"OFF (LEGACY)", "ON (9 OPS JITTED)"}, {0, 1}},
    {"JIT T-FORWARD ", "skip the T store/reload on cmp+bt (+4%)", PAGE_RECOMPILER, 1, 2,
     {"OFF (LEGACY)", "ON (NO RELOAD)"}, {0, 1}},
    {"FMOV DIRECT   ", "fmov straight to/from pinned FPRs (+10%, needs FPU PIN + FASTMEM)", PAGE_RECOMPILER, 1, 2,
     {"OFF (GPR BOUNCE)", "ON (LFS/STFS)"}, {0, 1}},
    {"JIT CARRY OPS ", "jit addc/subc/div1/cmp-str (10 ops)", PAGE_RECOMPILER, 1, 2, {"OFF (LEGACY)", "ON (10 OPS)"},
     {0, 1}},
    {"JIT MAC OPS   ", "jit mac.l/mac.w + saturation", PAGE_RECOMPILER, 1, 2, {"OFF (LEGACY)", "ON (FASTMEM RDS)"},
     {0, 1}},
    {"JIT FSCHG FAST", "fschg: 1 store, no FPU spill + call", PAGE_RECOMPILER, 1, 2,
     {"OFF (LEGACY)", "ON (SZ-ONLY PATH)"}, {0, 1}},
    {"JIT CR0 BRANCH", "cmp+bt: branch on CR0, skip mfcr", PAGE_RECOMPILER, 1, 2, {"OFF (LEGACY)", "ON (NO MFCR)"},
     {0, 1}},
    {"BLOCK COPY    ", "store queue + DMA without a call per word (+2%)", PAGE_RECOMPILER, 1, 2,
     {"OFF (PER-WORD)", "ON (BULK COPY)"}, {0, 1}},
    {"JIT FSQRT     ", "frsqrte + Newton inline, no C call (+4%?)", PAGE_RECOMPILER, 1, 2,
     {"OFF (LIBM)", "ON (INLINE NR)"}, {0, 1}},
    {"JIT FIPR DP   ", "fipr/ftrv: round once, like the hardware (accuracy)", PAGE_RECOMPILER, 1, 2,
     {"OFF (SINGLE)", "ON (DOUBLE SUM)"}, {0, 1}},
    {"VRAM32 FAST   ", "VRAM and sound RAM stores inline, no C call (8K a frame, 13K loading)",
     PAGE_RECOMPILER, 1, 2,
     {"OFF (C CALL)", "ON (INLINE)"}, {0, 1}},
    {"SQ FLUSH FAST ", "store-queue flushes to memory inline, no C call (Initial D: 4K a frame)", PAGE_RECOMPILER,
     1, 2, {"OFF (C CALL)", "ON (INLINE)"}, {0, 1}},
    // ---- Audio (nullDC4Wii's wii_audio.cpp) -------------------------------------
    {"AUDIO SYNC    ", "paces emulation to the sound, EMULATED clock only (FREE RUN: never waits)", PAGE_AUDIO, 1, 3,
     {"0 (FREE RUN)", "4 (RECOMMENDED)", "8 (MOST SLACK)"}, {0, 4, 8}},
    {"AUDIO BLOCK   ", "samples per sound buffer: bigger = more slack, more delay", PAGE_AUDIO, 0, 3,
     {"1024 (23 MS)", "1536 (35 MS)", "2048 (46 MS)"}, {0, 1, 2}},
    {"AUDIO DRC     ", "retunes the output pitch slightly to avoid crackle", PAGE_AUDIO, 0, 4,
     {"OFF (DEFAULT)", "GENTLE (+-1%)", "NORMAL (+-3%)", "WIDE (+-15%)"}, {0, 1, 2, 3}},
    {"SOUND CLOCK   ", "REAL TIME: music keeps its tempo when the game is slow; EMULATED: exact", PAGE_AUDIO, 1, 2,
     {"EMULATED (EXACT)", "REAL TIME"}, {0, 1}},
    {"AUDIO STATS   ", "logs [WiiAudio] once a second: blocks played, underruns, waits", PAGE_LOGS, 0, 2,
     {"OFF", "ON"}, {0, 1}},
    // ---- Fixes (nullDC4Wii, from the PS3 port; logs say when they fire) ------
    {"ARM7 STR ALIGN", "sound CPU: unaligned word store writes the aligned word (ChuChu echo fix)",
     PAGE_FIXES, 1, 2, {"OFF (LEGACY)", "ON (ALIGNED WORD)"}, {0, 1}},
    {"ADPCM LOOP    ", "looped ADPCM sounds keep their loop-start decoder state (SA2 fix)", PAGE_FIXES, 1, 2,
     {"OFF (LEGACY)", "ON (FLYCAST)"}, {0, 1}},
    {"ARM7 RESET FIX", "sound CPU reset enters SVC mode and clears its pending FIQ (Flycast)", PAGE_FIXES, 1, 2,
     {"OFF (LEGACY)", "ON (FLYCAST)"}, {0, 1}},
    {"AICA RACE FIX ", "sound DMA and IRQ handshake rules as Flycast (Looney Tunes fix)", PAGE_FIXES, 1, 2,
     {"OFF (LEGACY)", "ON (FLYCAST)"}, {0, 1}},
    {"AICA CA CLAMP ", "game hack: sound position reads stop 3 samples short of the loop end", PAGE_FIXES, 0, 2,
     {"OFF (DEFAULT)", "ON (GAME HACK)"}, {0, 1}},
    {"JIT LINK GUARD", "block links patch only a live exit (freeze guard after a code cache clear)",
     PAGE_FIXES, 1, 2, {"OFF (LEGACY)", "ON (CHECK SITE)"}, {0, 1}},
    {"JIT DIV32 FIX ", "matched divide sequences give the SH-4's 64/32 result", PAGE_FIXES, 1, 2,
     {"OFF (LEGACY 32)", "ON (64/32)"}, {0, 1}},
    {"INTERP FIX    ", "interpreter div1/addv/subv/mul.l/mac as the SH-4 manual", PAGE_FIXES, 1, 2,
     {"OFF (LEGACY)", "ON (MANUAL)"}, {0, 1}},
    {"SPG FIX       ", "HBlank IRQ follows SPG_HBLANK_INT, field bit once a frame (Flycast)", PAGE_FIXES, 1, 2,
     {"OFF (LEGACY)", "ON (FLYCAST)"}, {0, 1}},
    {"TA GUARD      ", "bad TA records are skipped instead of hanging", PAGE_FIXES, 1, 2, {"OFF (LEGACY)", "ON (SKIP)"},
     {0, 1}},
    {"INF VTX CUT   ", "vertices with 1/W = +inf are cut out of their strips (no spikes)", PAGE_FIXES, 1, 2,
     {"OFF (LEGACY)", "ON (CUT)"}, {0, 1}},
    // ---- Rendering, continued ------------------------------------------------
    {"NIGHT LIGHTS  ", "3D T&L GPU: night lamps and headlights on the GPU (approximate, far faster)",
     PAGE_DEPTH, 1, 2, {"CPU (EXACT)", "GPU (APPROX)"}, {0, 1}},
    // gxRend's ISP_CULL: 2 pairs the polygon's cull mode with GX's winding the
    // right way for this renderer's projection, 1 the other way round.
    {"BACKFACE CULL ", "drops polygons facing away like the arcade: fixes flickering trees", PAGE_DEPTH, 1, 3,
     {"OFF (LEGACY)", "ON", "ON, OTHER SIDE"}, {0, 2, 1}},
    // GPU T&L with 3D VTX FETCH: each list's bounding box (kept with its fetch
    // program) drops the lists out of the picture before they are drawn.
    {"3D LIST CULL  ", "3D T&L GPU: skips 3D models out of the picture, and most depth measuring (faster)",
     PAGE_DEPTH, 3, 4, {"OFF (LEGACY)", "BEHIND CAMERA", "OUT OF VIEW", "VIEW + MODELS"}, {0, 1, 2, 3}},
    // The game rebuilds the car's reflection map in software. 1 in 4 is the Wii's
    // default (owner, log 15: looks fine from the outside cameras).
    {"REFLECTIONS   ", "how often the car body's reflection map is rebuilt: less often is faster", PAGE_DEPTH, 3, 5,
     {"EVERY FRAME", "1 FRAME IN 2", "1 FRAME IN 3", "1 FRAME IN 4", "1 FRAME IN 8"}, {1, 2, 3, 4, 8}},
    // Frames with no pin (fly-bys, replays): see recordForGpu.
    {"DEPTH MEASURE ", "fly-bys and replays: keep the race's near plane (faster) or measure the exact one", PAGE_DEPTH,
     1, 2, {"EXACT (LEGACY)", "RACE PLANE"}, {0, 1}},
    {"JIT COLD ENTRY", "timeslice exit after the block: no taken branch at each entry, 12 B less", PAGE_RECOMPILER_2,
     1, 2, {"OFF (LEGACY)", "ON (AFTER BLOCK)"}, {0, 1}},
    {"DIMM AHEAD    ", "decode streamed game data (the music) ahead on a thread: small slices, no stutter", PAGE_AUDIO,
     1, 2, {"OFF (ON DEMAND)", "ON (THREAD)"}, {0, 1}},
    {"GX FIFO WAKE  ", "FIFO full: the CPU goes on after 32 KB were read, not 112 KB (heavy scenes)", PAGE_DEPTH, 1, 2,
     {"HALF (LEGACY)", "NEAR FULL"}, {0, 1}},
    {"MEM SWEEP     ", "guest memory nearly full: free a little at a time, or 2 MB at once (a 200 ms stall)",
     PAGE_CORE, 1, 2, {"2 MB (LEGACY)", "SHORT"}, {0, 1}},
    {"GX FIFO SIZE  ", "room for what the CPU sends the GPU: heavy scenes wait less for it to catch up", PAGE_DEPTH, 1,
     2, {"256 KB (LEGACY)", "512 KB"}, {0, 1}},
    {"SOFT FLOAT    ", "game's software double maths (divide, multiply) run natively, same results",
     PAGE_RECOMPILER_2, 1, 2, {"EMULATED (LEGACY)", "NATIVE"}, {0, 1}},
    {"PAGE GUARD    ", "unchanged 3D data made read-only: no check each frame, writes still seen",
     PAGE_CORE, 1, 2, {"OFF (LEGACY)", "ON (READ-ONLY)"}, {0, 1}},
    // Models clipped inside the TA's user clip rectangle: the in-car view's
    // rear-view mirror (naomi_elan_cmd.cpp, mirrorModel).
    // OFF + GAME SKIP: the game does not even build the mirror's view
    // (naomi_frontend.cpp naomi_MirrorViewSkip), so neither its SH-4 work nor
    // its ELAN commands are left.
    {"MIRROR        ", "in-car view: the mirror draws the scene twice; OFF faster, OFF + GAME SKIP fastest",
     PAGE_GENERAL, 1, 3, {"ON (LEGACY)", "OFF (FASTER)", "OFF + GAME SKIP"}, {1, 0, 2}},
    // SOUND LEVELS (sgc_if.cpp): two sounds recognised by their sample.
    {"MENU SOUND    ", "menu cursor sound (left/right/up/down): 0 = muted, 10 = as the game", PAGE_AUDIO, 5, 11,
     {"0 (MUTED)", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10 (AS THE GAME)"},
     {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
    {"TIRE SOUND    ", "tire squeal when the car slides or drifts: 0 = muted, 10 = as the game", PAGE_AUDIO, 4,
     11, {"0 (MUTED)", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10 (AS THE GAME)"},
     {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
    // Needs 3D LIST CULL (the boxes): naomi_elan_tnl.cpp, tooSmall.
    {"SMALL 3D      ", "GPU T&L: 3D objects smaller than this on screen are not drawn (faster)", PAGE_DEPTH, 0, 4,
     {"DRAW ALL (LEGACY)", "UNDER 2 PIXELS", "UNDER 4 PIXELS", "UNDER 8 PIXELS"}, {0, 2, 4, 8}},
    // In-car view = rear-view mirror models on screen (naomi_frontend.cpp,
    // naomi_ReflectionSkip): only the opponent's car shows the reflection there.
    {"IN-CAR REFLECT", "in-car view: only the opponent's car shows the reflection; rebuild it less often",
     PAGE_DEPTH, 1, 3, {"AS REFLECTIONS", "1 FRAME IN 16", "FROZEN"}, {0, 16, -1}},
    // GPU fetch's cached lists as GX display lists (naomi_elan_fetch.inc);
    // the guest page pool gives up kElanDlRingBytes when ON (_vmem.cpp).
    {"DISPLAY LISTS ", "3D VTX FETCH: cached 3D sent as GX display lists, the CPU no longer writes indices",
     PAGE_DEPTH, 1, 2, {"OFF (LEGACY)", "ON (FASTER)"}, {0, 1}},
    // Vertex-coloured lists through the GPU fetch as RGB8 (naomi_elan_fetch.inc,
    // alphaOpaque): only when their vertex alpha is 255 or not used.
    {"COLOUR LISTS  ", "3D VTX FETCH: vertex-coloured 3D read by the GPU too (when its alpha is opaque)",
     PAGE_DEPTH_2, 1, 2, {"CPU (LEGACY)", "GPU FETCH"}, {0, 1}},
    // The ELAN command walk asks for the cache lines ahead of it in the lists
    // outside models (naomi_elan_cmd.cpp, kPrefetchAhead). Speed only.
    {"ELAN PREFETCH ", "3D command walk loads its next commands ahead (dropped frames walk faster)",
     PAGE_DEPTH_2, 1, 2, {"OFF (LEGACY)", "ON (FASTER?)"}, {0, 1}},
    // Split screen (emu/naomi/naomi_2p.cpp): the game draws one car's in-car
    // view a frame, the cars taking turns, each into its half of the picture;
    // the second pad drives the opponent's car.
    {"2 PLAYERS     ", "split screen races: controller 2 (GC port 2 / Wiimote 2) drives the CPU car",
     PAGE_GENERAL, 0, 2, {"OFF", "SIDE BY SIDE"}, {0, 1}},
    // GameCube pads (platform_wii.cpp, addPad): the owner's layout, A/B the
    // pedals, and the first one, the analog triggers the pedals.
    {"GC PAD LAYOUT ", "GameCube pads: A/B gas/brake and R/L shift, or R/L gas/brake and A/B shift",
     PAGE_GENERAL, 0, 2, {"A/B PEDALS", "R/L PEDALS (OLD)"}, {0, 1}},
    // 2 PLAYERS (naomi_2p.cpp): the CPU car's own AI body steered by player
    // 2, or the player's physics run a second time a frame for player 2.
    {"P2 CAR        ", "2 PLAYERS: player 2 nudges the CPU car along its line, or drives it with the real physics",
     PAGE_GENERAL, 1, 2, {"ON RAILS (OLD)", "FULL PHYSICS"}, {0, 1}},
    // 2 PLAYERS (naomi_2p.cpp): the course drawn around player 1 only (player
    // 2's view goes empty ~800 m away), or around the car each view follows.
    {"P2 SCENERY    ", "2 PLAYERS: course drawn around player 1 only (lighter), or around each player's car",
     PAGE_DEPTH_2, 1, 2, {"AROUND P1 (OLD)", "AROUND EACH CAR"}, {0, 1}},
    // Smoother races at 30 fps (gxRend.cpp mfx_*, emu/naomi/naomi_motion.cpp):
    // the scenery only, the HUD is left sharp. IN-BETWEEN fills the frames the
    // frameskip drops, in the in-car view; it needs FRAMESKIPPING.
    {"MOTION FX     ", "race smoothing, HUD kept sharp; IN-BETWEEN fills dropped frames (in-car, needs frameskip)",
     PAGE_DEPTH_2, 0, 5, {"OFF", "SPEED BLUR", "FRAME BLEND", "IN-BETWEEN", "BLUR + IN-BETWEEN"}, {0, 1, 2, 3, 4}},
    {"MOTION LEVEL  ", "MOTION FX strength: blur length, blend opacity, how far an in-between frame moves",
     PAGE_DEPTH_2, 1, 3, {"LOW", "MEDIUM", "HIGH"}, {1, 2, 3}},
    // The TEST button (GC pad D-pad left, Wiimote/Classic left): one tap enters
    // the BIOS SYSTEM MENU (game_runner.cpp, pollControls), so it is off unless asked.
    {"TEST MODE     ", "TEST button (D-pad LEFT, every pad): OFF ignored, ON enters the game's test menu",
     PAGE_GENERAL, 0, 2, {"OFF", "ON"}, {0, 1}},
    // IN-BETWEEN's picture. Real-Wii logs 2026-11-10: BANDS tears the opponent
    // car, SMOOTH (one pass) tears the scenery too. ROTATION ONLY and NO MOVE
    // are tests to tell the warp's depth guess from the path itself.
    {"BETWEEN WARP  ", "IN-BETWEEN picture: BANDS, SMOOTH, ROTATION ONLY (turn, no depth), NO MOVE (test: same picture)",
     PAGE_DEPTH_2, 0, 4, {"ROTATION ONLY", "BANDS (OLD)", "SMOOTH", "NO MOVE (TEST)"}, {2, 1, 0, 3}},
    // The cars' headlights at night (naomi_elan_tnl.cpp, naomi_elan_gx.cpp): the
    // ELAN lights per vertex and the cone's edge is hard, so the road's big
    // polygons show in the beam. SMOOTH needs 3D T&L GPU and NIGHT LIGHTS GPU.
    {"HEADLIGHTS    ", "night: ARCADE beam shows the road's polygons; SOFT wider edge (free); SMOOTH per pixel",
     PAGE_DEPTH_2, 0, 3, {"ARCADE (OLD)", "SOFT", "SMOOTH"}, {0, 1, 2}},
    // FPSCR.FR bank swaps (wii_driver.cpp shop_sync_fpscr): ~2000 a night frame,
    // each a 16-register spill, a C call and a reload; INLINE swaps in place.
    {"JIT FR SWAP   ", "float register bank swap done in the recompiled code (~2000 a night frame)",
     PAGE_RECOMPILER_2, 1, 2, {"C CALL (LEGACY)", "INLINE"}, {0, 1}},
    // The cache lines of a model's command headers asked for at its call, from
    // where they were last time (naomi_elan_cmd.cpp prefetchModel). Speed only.
    {"MODEL PREFETCH", "3D command walk loads a model's commands before reading them (drawn frames)",
     PAGE_DEPTH_2, 1, 2, {"OFF (LEGACY)", "ON (FASTER?)"}, {0, 1}},
    // Every other sound sample steps the voices without mixing them and plays
    // the last one again (sgc_if.cpp AICA_Sample): 22 kHz sound, what the game
    // and the sound CPU see is unchanged.
    {"SOUND MIX     ", "HALF: voices mixed every other sample (22 kHz, duller); no gain measured on a Wii", PAGE_AUDIO, 0, 2,
     {"FULL (EXACT)", "HALF (FASTER)"}, {0, 1}},
    // The Wii has no square root instruction: libm's sqrtf is a 25-step loop
    // (~300 cycles), and a night list took 2-3 of them (naomi_elan_tnl.cpp,
    // sqrtUp): its box's size, its sphere (now only for HEADLIGHTS SOFT/SMOOTH).
    {"LIST MATHS    ", "3D lists' light and cull bounds without the Wii's slow software square root (night)",
     PAGE_DEPTH_2, 1, 2, {"LIBM (OLD)", "FAST"}, {0, 1}},
    // Materials and lights decoded with tables instead of divisions and ldexpf,
    // the box test's matrix scale without sqrtf (naomi_elan_tnl.cpp). Exact.
    {"3D DECODE     ", "3D materials, lights and box tests decoded without libm calls (same results)",
     PAGE_DEPTH_2, 1, 2, {"LIBM (OLD)", "TABLES"}, {0, 1}},
    // The draw takes each list's GPU program from the slot found when the ELAN
    // recorded it, and the normal matrix's scale without sqrtf (naomi_elan_gx.cpp).
    {"DRAW MEMO     ", "3D draw reuses what recording found (program slot); matrix scale without libm",
     PAGE_DEPTH_2, 1, 2, {"OFF (OLD)", "ON"}, {0, 1}},
    // AICA timers and interrupt lines only updated when a sample can change them,
    // a voice's stream stepped only when it moves (aica.cpp, sgc_if.cpp). Exact.
    {"SOUND STEP    ", "sound chip timers and interrupts updated only when they change (same sound)", PAGE_AUDIO, 1, 2,
     {"PER SAMPLE (OLD)", "LAZY"}, {0, 1}},
    // SH-4 cycles between two peripheral updates (sh4_interpreter.cpp): 3584
    // halves the ~1870 C calls a frame; sound, DMA and the rest keep their
    // cadence in cycles, interrupts are seen up to 18 us later.
    {"SH-4 TIMESLICE", "SH-4 runs longer between device updates: half the C calls (devices keep their pace)",
     PAGE_CORE, 1, 2, {"1792 (OLD)", "3584 (WIDER)"}, {0, 1}},
    // More cache lines asked for ahead (a model's lists' fetch tables and boxes,
    // the draw's matrices and light sets, gxRend's strip arrays), and the 3D
    // tables' next lines zeroed in the cache instead of read (naomi_elan_gx.h).
    {"CACHE HINTS   ", "3D tables loaded ahead and written without reading them first (same picture)",
     PAGE_DEPTH_2, 1, 2, {"OFF (OLD)", "ON"}, {0, 1}},
    // Broadway's performance monitor interrupts 10,000 times a second and the
    // interrupted PC is counted (frame_prof.cpp, [PCS] lines): where the Wii's
    // cycles go, by C function and by SH-4 block. Takes the [HW] counters.
    {"PC SAMPLER    ", "with PROFILER: where the CPU time goes, by function and SH-4 block (~1% slower)",
     PAGE_LOGS, 0, 2, {"OFF", "ON"}, {0, 1}},
    // Two of the game's bulk copies (its word copy into the store queues, its
    // display-list copy into ELAN RAM) done in C instead of recompiled SH-4
    // loops (naomi_copies.cpp); the same memory and registers, cycles charged.
    {"GAME COPIES   ", "game's word and display-list copy loops run natively (same results, less CPU)",
     PAGE_RECOMPILER_2, 1, 2, {"EMULATED (OLD)", "NATIVE"}, {0, 1}},
};

// The order a page shows its rows in (the launcher walks this list and keeps the
// rows of the page). kCoreOptions itself stays in CoreOptionId order, which
// autolaunch.txt reads by position; to move a row on screen, move it here.
inline constexpr CoreOptionId kCoreOptionOrder[kCoreOptionCount] = {
    // GENERAL
    OPT_TWO_PLAYER, OPT_MIRROR, OPT_FRAMESKIP, OPT_MODEL_SKIP, OPT_FPS_OVERLAY, OPT_SPEED_LIMIT,
    OPT_TEST_MODE, OPT_GC_LAYOUT, OPT_P2_CAR,
    // RENDERING
    OPT_FIXED_DEPTH, OPT_DEPTH_CLIP, OPT_HUD_PASS, OPT_ISP_DEPTH_FUNC, OPT_TRANS_SORT, OPT_PUNCH_THROUGH,
    OPT_AUTOSORT, OPT_SPLIT_SCREEN, OPT_VRAM_PRESENT, OPT_ELAN_POINT, OPT_ISP_CULL, OPT_ELAN_BOX,
    OPT_REFLECTIONS, OPT_DEPTH_MEASURE, OPT_FIFO_WAKE, OPT_FIFO_SIZE, OPT_SMALL_CULL, OPT_INCAR_REFLECT,
    OPT_DISPLAY_LISTS,
    // RENDERING 2
    OPT_P2_SCENERY, OPT_4BPP, OPT_8BPP, OPT_ELAN_GX, OPT_ELAN_FETCH, OPT_TEX_CACHE, OPT_VQ_CMPR,
    OPT_TMEM_CACHE, OPT_COLOUR_FETCH, OPT_ELAN_PREFETCH, OPT_MODEL_PREFETCH, OPT_HEADLIGHTS,
    OPT_MOTION_FX, OPT_MOTION_LEVEL, OPT_BETWEEN_WARP, OPT_LIST_MATHS, OPT_WALK_DECODE, OPT_DRAW_MEMO,
    OPT_CACHE_HINTS,
    // AUDIO
    OPT_AUDIO_SYNC, OPT_AUDIO_BLOCK, OPT_AUDIO_DRC, OPT_SOUND_CLOCK, OPT_DIMM_AHEAD, OPT_SOUND_MIX,
    OPT_SFX_MENU, OPT_SFX_TIRE, OPT_SOUND_STEP,
    // CORE (the SH-4 CPU choice heads this page, the launcher adds it)
    OPT_IDLE_SKIP, OPT_MEM_SWEEP, OPT_PAGE_GUARD, OPT_SH4_SLICE,
    // RECOMPILER
    OPT_JIT_SBP, OPT_JIT_ICI, OPT_FASTMEM, OPT_BCACHE, OPT_DYN_IC, OPT_FPU_PIN, OPT_JIT_ALIGN,
    OPT_IFB_FLUSH, OPT_JIT_NEWOPS, OPT_JIT_TFWD, OPT_JIT_FMOV, OPT_JIT_CARRY, OPT_JIT_MAC, OPT_JIT_FSCHG,
    OPT_JIT_CR0, OPT_BLOCKCOPY, OPT_JIT_FSQRT, OPT_JIT_FIPR, OPT_VRAM32_FAST, OPT_SQ_FAST,
    // RECOMPILER 2
    OPT_JIT_COLD_ENTRY, OPT_SOFT_FLOAT, OPT_JIT_FRSWAP, OPT_GAME_COPIES,
    // FIXES
    OPT_ARM7_STR_ALIGN, OPT_ADPCM_LOOP, OPT_ARM7_RESET_FIX, OPT_AICA_RACE, OPT_AICA_CA_CLAMP,
    OPT_JIT_LINK_GUARD, OPT_DIV32_FIX, OPT_INTERP_FIX, OPT_SPG_FIX, OPT_TA_GUARD, OPT_INF_VTX,
    // LOGS
    OPT_PROFILER, OPT_JIT_CCALLS, OPT_JIT_HOTBLOCKS, OPT_AUDIO_STATS, OPT_PC_SAMPLER,
};

// Every option appears once in the order above, and the pages come in that order.
constexpr bool coreOptionOrderValid()
{
    bool seen[kCoreOptionCount] = {};
    int lastPage = 0;
    for (int i = 0; i < kCoreOptionCount; i++)
    {
        const int id = kCoreOptionOrder[i];
        if (id < 0 || id >= kCoreOptionCount || seen[id])
            return false;
        seen[id] = true;
        if (kCoreOptions[id].page < lastPage)
            return false;
        lastPage = kCoreOptions[id].page;
    }
    return true;
}
static_assert(coreOptionOrderValid(), "kCoreOptionOrder must list every option once, page by page");

} // namespace idas3
