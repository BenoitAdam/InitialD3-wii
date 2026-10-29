// Core presets inherited from nullDC4Wii (renderer, texture cache, SH-4
// recompiler), chosen on the launch pages. Names and values follow
// nullDC4Wii's menus; the game runner copies the chosen values into the
// core's preset variables before it starts (game_runner.cpp, same order).
#pragma once

namespace idas3 {

enum CoreOptionPage
{
    PAGE_GENERAL,
    PAGE_DEPTH,
    PAGE_RECOMPILER,
    PAGE_AUDIO,
    PAGE_FIXES,
    kCoreOptionPages
};

inline constexpr const char* kCoreOptionPageNames[kCoreOptionPages] = {"GENERAL", "RENDERING", "RECOMPILER", "AUDIO",
                                                                       "FIXES"};

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
    kCoreOptionCount
};

struct CoreOption
{
    const char* label;         // 14 characters, the menu's label column
    const char* hint;          // shown under the list while selected
    int page;
    int defaultIndex;          // position in names[] chosen by default
    int count;
    const char* names[5];      // 17 characters at most
    int values[5];             // the preset value behind each name
};

inline constexpr CoreOption kCoreOptions[kCoreOptionCount] = {
    // ---- General -----------------------------------------------------------
    {"SPEED LIMITER ", "caps emulation at 100% (the menus otherwise run up to 160%)", PAGE_GENERAL, 0, 2,
     {"OFF (UNCAPPED)", "ON (CAP 100%)"}, {0, 1}},
    {"IDLE LOOP SKIP", "Initial D's wait-for-frame loop runs 30x fewer times (same timing)", PAGE_GENERAL, 1,
     2, {"OFF", "ON"}, {0, 1}},
    {"3D T&L        ", "GPU: Wii GPU lights opaque 3D (prototype), CPU: all in software", PAGE_GENERAL, 1,
     2, {"CPU (SOFTWARE)", "GPU (PROTOTYPE)"}, {0, 1}},
    {"3D VTX FETCH  ", "GPU T&L: the GPU reads 3D models itself (NO PATCHES: CPU feeds strips over gaps)",
     PAGE_GENERAL, 1, 3, {"OFF (CPU FEEDS)", "ON (GPU READS)", "ON, NO PATCHES"}, {0, 1, 2}},
    {"FRAMESKIPPING ", "dropped frames also skip their NAOMI 2 3D transform (AUTO: when behind)", PAGE_GENERAL, 0,
     5, {"0 (DEFAULT)", "1 (1 OF 2)", "2 (2 OF 3)", "AUTO", "AUTO MAX"}, {0, 1, 2, 3, 4}},
    {"DROPPED 3D    ", "frames the frameskip drops: SKIP MODELS steps over 3D model lists (faster)",
     PAGE_GENERAL, 1, 2, {"WALK ALL", "SKIP MODELS"}, {0, 1}},
    {"TEXTURE CACHE ", "can have a huge FPS impact (VERY FAST: skimp algorithm)", PAGE_GENERAL, 3, 5,
     {"VERY FAST", "VERY FAST+", "FAST", "NORMAL (DEFAULT)", "QUALITY (SLOW)"}, {0, 6, 1, 2, 3}},
    {"VQ AS CMPR    ", "VQ textures as GX CMPR: fixes VQ glitches on VERY FAST/+", PAGE_GENERAL, 0, 2,
     {"OFF (DEFAULT)", "ON"}, {0, 1}},
    {"TMEM CACHE    ", "keep the GPU texture cache warm", PAGE_GENERAL, 1, 2, {"OFF (LEGACY)", "ON (FASTER?)"},
     {0, 1}},
    {"4BPP MODE     ", "4-bit palette textures", PAGE_GENERAL, 1, 3, {"I4 STUB", "4BPP OPTIMIZED", "CI4 (FAST)"},
     {0, 1, 2}},
    {"8BPP MODE     ", "8-bit palette textures", PAGE_GENERAL, 1, 3, {"I8 STUB", "8BPP OPTIMIZED", "CI8 (FAST)"},
     {0, 1, 2}},
    {"SPEED OVERLAY ", "on-screen FPS and emulation SPEED %", PAGE_GENERAL, 1, 2, {"OFF", "ON"}, {0, 1}},
    {"PROFILER      ", "logs where each frame's time goes (sh4/elan/rend/tex...)", PAGE_GENERAL, 0, 2,
     {"OFF", "ON"}, {0, 1}},
    {"JIT CCALLS    ", "with PROFILER: SH-4 code's calls into C per second (cheap)", PAGE_GENERAL, 0, 2,
     {"OFF", "ON (CENSUS)"}, {0, 1}},
    {"JIT HOTBLOCKS ", "with PROFILER: hottest SH-4 blocks and their code (~12% slower)", PAGE_GENERAL, 0, 2,
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
    {"AUDIO SYNC    ", "paces emulation to the sound (FREE RUN: never waits, drops)", PAGE_AUDIO, 1, 3,
     {"0 (FREE RUN)", "4 (RECOMMENDED)", "8 (MOST SLACK)"}, {0, 4, 8}},
    {"AUDIO BLOCK   ", "samples per sound buffer: bigger = more slack, more delay", PAGE_AUDIO, 0, 3,
     {"1024 (23 MS)", "1536 (35 MS)", "2048 (46 MS)"}, {0, 1, 2}},
    {"AUDIO DRC     ", "retunes the output pitch slightly to avoid crackle", PAGE_AUDIO, 0, 4,
     {"OFF (DEFAULT)", "GENTLE (+-1%)", "NORMAL (+-3%)", "WIDE (+-15%)"}, {0, 1, 2, 3}},
    {"SOUND CLOCK   ", "keep EMULATED: Initial D streams its music, REAL TIME glitches it", PAGE_AUDIO, 1, 2,
     {"EMULATED (EXACT)", "REAL TIME"}, {0, 1}},
    {"AUDIO STATS   ", "logs [WiiAudio] once a second: blocks played, underruns, waits", PAGE_AUDIO, 0, 2,
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
};

} // namespace idas3
