#include "game_runner.h"
#include "../core/log.h"
#include "../platform/platform.h"

#include "naomi/naomi.h"
#include "naomi/naomi_frontend.h"
#include "naomi/naomi_elan.h"
#include "naomi/naomi_elan_gx.h"
#if defined(GEKKO)
#include "wii/wii_fastmem.h"
#include "wii/wii_audio.h"
#include "wii/wii_exit.h"
#include <gccore.h>
#include <malloc.h>
#endif

#include <cstdio>

// nullDC4Wii runtime presets (emu/host/presets_generated.cpp).
extern int g_4bpp_preset;
extern int g_8bpp_preset;
extern int g_fixed_depth_preset;
extern int g_depth_clip_preset;
extern int g_hud_pass_preset;
extern int g_isp_depth_func_preset;
extern int g_trans_sort_preset;
extern int g_punch_through_preset;
extern int g_autosort_preset;
extern int g_split_screen_preset;
extern int g_isp_cull_preset;
extern int g_reflection_rate_preset;  // emu/naomi/naomi_frontend.cpp
extern int g_depth_plane_preset;      // emu/naomi/naomi_elan_tnl.cpp
extern int g_vram_present_preset;     // emu/plugs/drkPvr/gxRend.cpp
extern int g_show_fps_overlay;
extern int g_speed_limiter_preset;
extern int g_frameskip_preset;
extern int g_texture_cache_preset;
extern int g_vq_cmpr_preset;
extern int g_tmem_cache_preset;
extern int g_jit_sbp_preset;
extern int g_jit_ici_preset;
extern int g_fastmem_preset;
extern int g_bcache_preset;
extern int g_dyn_ic_preset;
extern int g_fpu_pin_preset;
extern int g_jit_align_preset;
extern int g_ifb_flush_preset;
extern int g_jit_newops_preset;
extern int g_jit_tfwd_preset;
extern int g_jit_fmov_preset;
extern int g_jit_carry_preset;
extern int g_jit_mac_preset;
extern int g_jit_fschg_preset;
extern int g_jit_cr0_preset;
extern int g_jit_fsqrt_preset;
extern int g_jit_fipr_preset;
extern "C" int g_blockcopy_preset;    // emu/dc/mem/sh4_mem.cpp
extern "C" int g_frame_prof_preset;   // emu/plugs/drkPvr/frame_prof.cpp
extern "C" int g_jit_ccalls_preset;   // emu/dc/sh4/rec_v2/shil.cpp
extern "C" int g_idle_skip_preset;    // emu/naomi/naomi_frontend.cpp
extern "C" int g_vram32_fast_preset;
extern "C" int g_sq_fast_preset;
extern "C" int g_cold_entry_preset;
extern "C" int g_jit_frswap_preset;       // JIT FR SWAP (naomi_frontend.cpp)
extern "C" int g_aica_half_mix_preset;    // SOUND MIX (sgc_if.cpp)
extern "C" int g_fifo_wake_preset;
extern "C" int g_mem_sweep_preset;
extern "C" int g_fifo_size_preset;
extern "C" int g_soft_float_preset;
extern "C" int g_page_guard_preset;
extern "C" int g_mirror_preset;
extern "C" int g_sfx_menu_level_preset;
extern "C" int g_sfx_tire_level_preset;
extern "C" int g_small_cull_preset;
extern "C" int g_incar_reflect_preset;    // emu/naomi/naomi_frontend.cpp
extern "C" u32 g_reflection_incar_skips;
extern "C" int g_sound_realtime_preset;
extern "C" int g_two_player_preset;       // emu/naomi/naomi_2p.cpp
extern "C" int g_p2_car_preset;
extern "C" int g_p2_scenery_preset;
extern int g_audio_buffers_preset;    // emu/host/presets_generated.cpp (wii/wii_audio.cpp)
extern int g_audio_block_preset;
extern int g_audio_drc_preset;
extern int g_audio_stats_preset;
extern int g_hotblocks_preset;        // emu/host/presets_generated.cpp
extern int g_arm7_str_align_preset;
extern int g_adpcm_loop_preset;
extern int g_arm7_reset_fix_preset;
extern int g_aica_race_preset;
extern int g_aica_ca_clamp_preset;
extern int g_jit_link_guard_preset;
extern int g_div32_fix_preset;
extern int g_interp_fix_preset;
extern int g_spg_fix_preset;
extern int g_ta_guard_preset;
extern int g_inf_vtx_preset;
extern "C" int g_list_maths_preset;       // LIST MATHS (emu/naomi/naomi_elan_tnl.cpp)
extern "C" int g_walk_decode_preset;      // 3D DECODE (emu/naomi/naomi_elan_tnl.cpp)
extern "C" int g_draw_memo_preset;        // DRAW MEMO (emu/naomi/naomi_elan_gx.cpp)
extern "C" int g_sound_step_preset;       // SOUND STEP (emu/plugs/nullAICA/aica.cpp)
extern "C" int g_sh4_wide_slice_preset;   // SH-4 TIMESLICE (emu/dc/sh4/sh4_interpreter.cpp)
extern "C" int g_cache_hints_preset;      // CACHE HINTS (emu/naomi/naomi_elan_cmd.cpp)
extern "C" int g_pc_sampler_preset;       // PC SAMPLER (emu/plugs/drkPvr/frame_prof.cpp)
extern "C" int g_game_copies_preset;      // GAME COPIES (emu/naomi/naomi_copies.cpp)
extern "C" int g_int_divide_preset;       // INT DIVIDE (emu/naomi/naomi_copies.cpp)
extern "C" int g_jit_fpu_banks_preset;    // JIT FPU BANKS (emu/naomi/naomi_frontend.cpp, decoder.cpp)
extern "C" int g_screen_buffers_preset;   // SCREEN BUFFERS (emu/naomi/naomi_frontend.cpp, gxRend.cpp)
extern u32 g_dec_fpu_uncrossed, g_dec_fpu_physical;   // JIT FPU BANKS counts (decoder.cpp)
#if defined(GEKKO)
u32 recSh4_CodeCacheHighWater();      // emu/dc/sh4/rec_v2/driver.cpp
u32 recSh4_CodeCacheSize();
#endif

namespace idas3 {

namespace {

void coreLog(const char* text)
{
    log::write("%s", text);
}

bool cartRead(void* user, u32 offset, u32 length, u8* dst)
{
    return static_cast<DimmImage*>(user)->read(offset, length, dst);
}

bool cartBusy(void* user)
{
    return static_cast<DimmImage*>(user)->readAheadBusy();
}

int g_dimm_ahead_preset = 1;
int g_gc_layout_preset = 0;             // platform::setPadLayout
int g_test_mode_preset = 0;             // TEST MODE: 0 the TEST button is ignored, 1 it works
const DimmImage* s_dimm = nullptr;      // for the profiler's lines

NaomiFrameInfo s_lastFrame;

// One tap of TEST restarts the game into the BIOS SYSTEM MENU, which reads as a
// hang in a log (log 16): name the press.
void logPress(const char* name, bool now, bool& was)
{
    if (now && !was)
        log::write("[game] %s button pressed at frame %u\n", name, s_lastFrame.vblanks);
    was = now;
}

// The controllers connected, logged when they change. 2 PLAYERS: which pad is
// player 2's is the first question when one does not answer. A device counts
// if it answered in any of the last 60 reads: the game reads its controls once
// or twice a frame, and a GameCube pad read twice in one SI poll answers with a
// transfer error (PAD_ScanPads keeps its buttons but leaves it out of the
// connected mask), so a mask that had to hold steady never did (2P log 1).
void logControllers()
{
    static uint32_t s_logged = ~0u, s_window = 0, s_reads = 0;
    s_window |= platform::controllersSeen();
    if (++s_reads < 60)
        return;
    const uint32_t seen = s_window;
    s_window = 0;
    s_reads = 0;
    if (seen == s_logged)
        return;
    s_logged = seen;
    static const char* const kExp[] = {"", " + Nunchuk", " + Classic", " + other"};
    char line[200];
    int n = snprintf(line, sizeof(line), "[game] controllers at frame %u: GameCube ports", s_lastFrame.vblanks);
    for (int i = 0; i < 4; i++)
        if (seen >> i & 1)
            n += snprintf(line + n, sizeof(line) - n, " %d", i + 1);
    n += snprintf(line + n, sizeof(line) - n, "%s; Wiimotes", (seen & 0xF) ? "" : " none");
    for (int i = 0; i < 4; i++)
        if (seen >> (4 + i) & 1)
            n += snprintf(line + n, sizeof(line) - n, " %d%s", i + 1, kExp[seen >> (8 + 2 * i) & 3]);
    snprintf(line + n, sizeof(line) - n, "%s\n", (seen & 0xF0) ? "" : " none");
    log::write("%s", line);
}

// 0..1 to the JVS 16-bit range, 0.5 -> 0x8000 (the wheel's centre).
u16 analog(float v)
{
    const float x = v * 65536.f;
    if (x < 0.f)
        return 0;
    if (x > 65535.f)
        return 0xFFFF;
    return (u16)x;
}

// Initial D's cabinet on the 837-13551 JVS board: steering wheel on analog
// 0, gas and brake on 1 and 2, VIEW CHANGE on player 1 DOWN, and the
// sequential shifter on player 2 UP / DOWN.
void pollControls(NaomiInputs& in)
{
    platform::GameControls c;
    if (g_two_player_preset)
    {
        // 2 PLAYERS: the second controller drives the CPU car (naomi_2p.cpp).
        platform::GameControls c2;
        platform::readGameControlsTwo(c, c2);
        naomi_2pSetPad({c2.steer, c2.accel, c2.brake, c2.shiftUp, c2.shiftDown});
    }
    else
        platform::readGameControls(c);
    logControllers();
    naomi_DefaultInputs(in);
    in.analog[0] = analog((c.steer + 1.f) * 0.5f);
    in.analog[1] = analog(c.accel);
    in.analog[2] = analog(c.brake);
    if (c.start)
        in.players[0] |= NAOMI_JVS_START;
    if (c.view)
        in.players[0] |= NAOMI_JVS_DOWN;
    if (c.service)
        in.players[0] |= NAOMI_JVS_SERVICE;
    if (c.shiftUp)
        in.players[1] |= NAOMI_JVS_UP;
    if (c.shiftDown)
        in.players[1] |= NAOMI_JVS_DOWN;
    in.coin[0] = c.coin;
    const bool testNow = c.test && g_test_mode_preset != 0;
    in.test = testNow;
    static bool s_testHeld, s_serviceHeld;
    logPress("TEST", testNow, s_testHeld);
    logPress("SERVICE", c.service, s_serviceHeld);
    if (c.quit)
    {
#if defined(GEKKO)
        // nullDC4Wii's ordered shutdown, from here like it does: a bare
        // exit with the DSP, the GPU's queued frame, SI and Bluetooth still
        // live can hang in libogc's reset (black screen, no loader).
        log::write("[game] quit requested: shutting down\n");
        WiiExitSetPowerOff(platform::powerOffRequested() ? 1 : 0);
        WiiExitToLoader();
#else
        naomi_Stop();
#endif
    }
}

#if defined(GEKKO)
// C heap use against the arenas left to it (the guest page pool took the rest).
void logHeap(const char* what)
{
    const struct mallinfo mi = mallinfo();
    log::write("[game] %s: %u KB in use, %u KB free inside malloc, arenas left MEM1 %u KB, MEM2 %u KB\n", what,
               (unsigned)mi.uordblks / 1024, (unsigned)mi.fordblks / 1024,
               (unsigned)((u8*)SYS_GetArena1Hi() - (u8*)SYS_GetArena1Lo()) / 1024,
               (unsigned)((u8*)SYS_GetArena2Hi() - (u8*)SYS_GetArena2Lo()) / 1024);
}
#endif

// One status line, logged on events only (low memory, end of the game).
void logStatus(const char* why, const NaomiFrameInfo& info)
{
    const NaomiStats& st = naomi_Stats();
    log::write("[game] %s at frame %u: pc %08X, %u renders, %u KB loaded from the DIMM\n", why, info.vblanks, info.pc,
               info.renders, st.cartDmaBytes / 1024);
#if defined(GEKKO)
    WiiGuestMemStats m;
    WiiGuestMem_Stats(&m);
    log::write("[game] guest memory: %u of %u KB free (ram %u, vram %u, elan %u, sound %u KB), %u reclaims freed %u KB, "
               "%u failed allocations\n",
               m.free_kb, m.pool_kb, m.ram_kb, m.vram_kb, m.elan_kb, m.aram_kb, m.reclaims, m.reclaimed_kb,
               m.alloc_fails);
    log::write("[game] compressed: %u KB of guest pages held in %u KB (%u compressions, %u decompressions%s)\n",
               m.comp_kb, m.store_kb, m.compressions, m.decompressions, m.corrupt ? ", CORRUPT DATA" : "");
    logHeap("heap");
    log::write("[game] SH-4 code cache: at most %u of %u KB used\n", recSh4_CodeCacheHighWater() / 1024,
               recSh4_CodeCacheSize() / 1024);
#endif
}

#if defined(GEKKO)
// With PROFILER on: guest memory activity per real second, every 300 frames.
// Page faults and decompressions are exceptions, which cost a lot (especially
// under Dolphin), so these rates show whether memory paging eats the frame.
u32 s_prevIch = 0, s_prevGpu = 0, s_prevLit = 0;
u64 s_prevExec = 0, s_prevTnl = 0, s_prevGpuNs = 0, s_prevConv = 0, s_prevTa = 0;
NaomiElanStats s_prevBox = {};        // the 3D LIST CULL counters at the last line
u32 s_prevDropped = 0;                 // g_rend_dropped_renders at the last line

void logMemoryActivity(const NaomiFrameInfo& info)
{
    static WiiGuestMemStats prev;
    static u64 prevUs = 0;
    static u32 prevFrame = 0;
    WiiGuestMemStats m;
    WiiGuestMem_Stats(&m);
    const u64 now = platform::microseconds();
    if (prevUs)
    {
        const double secs = (double)(now - prevUs) / 1e6;
        const double fps = (info.vblanks - prevFrame) / secs;
        log::write("[prof] frames %u-%u: %.1f s, %.1f emulated fps | per second: %.0f DSI (%.0f page faults), %.0f "
                   "decompressions, %.0f compressions | free %u KB, %u KB compressed into %u KB\n",
                   prevFrame, info.vblanks, secs, fps, (m.dsi - prev.dsi) / secs,
                   (m.page_faults - prev.page_faults) / secs, (m.decompressions - prev.decompressions) / secs,
                   (m.compressions - prev.compressions) / secs, m.free_kb, m.comp_kb, m.store_kb);
        const NaomiElanStats& es = naomi_ElanStats();
        const u32 frames = info.vblanks - prevFrame;
        log::write("[prof]   ELAN per frame: %u vertices in, %u on the GPU, %u lit on the CPU | %u lists "
                   "skipped by frameskip, %u state commands skipped, %u drawn lists held back (totals)\n",
                   (es.ichVertices - s_prevIch) / frames, (es.gpuVertices - s_prevGpu) / frames,
                   (es.litVertices - s_prevLit) / frames, es.ichSkipped, es.stateSkipped, es.ichHeldBack);
        log::write("[prof]   ELAN model lists: %u walked, %u stepped over on dropped frames (%u polygon lists not "
                   "walked); walked again with other commands %u, of which side effects changed %u (totals)\n",
                   es.modelsWalked, es.modelsSkipped, es.modelListsSkipped, es.modelsChanged, es.modelsFlipped);
        float canvasW, canvasH;
        elan_gx_canvas(canvasW, canvasH);
        log::write("[prof]   3D list cull per frame: %u lists with a box, %u dropped behind the camera, %u out of "
                   "view (%u vertices), whole models %u (%u lists) | depth measured on %u vertices | "
                   "canvas %.0fx%.0f\n",
                   (es.boxLists - s_prevBox.boxLists) / frames, (es.boxCulledBehind - s_prevBox.boxCulledBehind) / frames,
                   (es.boxCulledSide - s_prevBox.boxCulledSide) / frames,
                   (es.boxCulledVertices - s_prevBox.boxCulledVertices) / frames,
                   (es.modelsCulled - s_prevBox.modelsCulled) / frames,
                   (es.modelListsCulled - s_prevBox.modelListsCulled) / frames,
                   (es.depthMeasured - s_prevBox.depthMeasured) / frames, canvasW, canvasH);
        if (es.boxCulledSmall != s_prevBox.boxCulledSmall)
            log::write("[prof]   SMALL 3D per frame: %u lists not drawn (whole models among the culled above)\n",
                       (es.boxCulledSmall - s_prevBox.boxCulledSmall) / frames);
        if (es.lightSetsBuilt != s_prevBox.lightSetsBuilt || es.lightSetsReused != s_prevBox.lightSetsReused)
            log::write("[prof]   point-lit light sets per frame: %u built, %u reused (one of the last 4)\n",
                       (es.lightSetsBuilt - s_prevBox.lightSetsBuilt) / frames,
                       (es.lightSetsReused - s_prevBox.lightSetsReused) / frames);
        if (es.lightPicks != s_prevBox.lightPicks || es.lightCmds != s_prevBox.lightCmds)
            log::write("[prof]   night lights per frame: %u light commands decoded, %u repeats skipped | %u point-lit "
                       "picks, %u lights tested (%u settled by the quick test), %u in reach\n",
                       (es.lightCmds - s_prevBox.lightCmds) / frames,
                       (es.lightCmdsSame - s_prevBox.lightCmdsSame) / frames,
                       (es.lightPicks - s_prevBox.lightPicks) / frames,
                       (es.lightTests - s_prevBox.lightTests) / frames,
                       (es.lightsQuickFar - s_prevBox.lightsQuickFar) / frames,
                       (es.lightsInReach - s_prevBox.lightsInReach) / frames);
        if (es.beamLists != s_prevBox.beamLists || es.coneOut != s_prevBox.coneOut)
            log::write("[prof]   HEADLIGHTS per frame: %u lists with a projected beam (SMOOTH), %u of them masked "
                       "(reaching behind the lamp), %u headlights left out of lists their cone misses\n",
                       (es.beamLists - s_prevBox.beamLists) / frames, (es.beamMasked - s_prevBox.beamMasked) / frames,
                       (es.coneOut - s_prevBox.coneOut) / frames);
        if (es.sumHalf != s_prevBox.sumHalf || es.sumQuarter != s_prevBox.sumQuarter ||
            es.sumUnsafe != s_prevBox.sumUnsafe)
            log::write("[prof]   LIGHT SUM per frame: point-lit lists lit at 1/2 %u, at 1/4 %u, left over the safe sum "
                       "%u | drawn scaled %u\n",
                       (es.sumHalf - s_prevBox.sumHalf) / frames, (es.sumQuarter - s_prevBox.sumQuarter) / frames,
                       (es.sumUnsafe - s_prevBox.sumUnsafe) / frames, (es.sumDrawn - s_prevBox.sumDrawn) / frames);
        if (es.mirrorModels != s_prevBox.mirrorModels)
            log::write("[prof]   MIRROR OFF per frame: %u models not drawn (%u from their summary), %u lists, %u "
                       "vertices\n",
                       (es.mirrorModels - s_prevBox.mirrorModels) / frames,
                       (es.mirrorSummarised - s_prevBox.mirrorSummarised) / frames,
                       (es.mirrorLists - s_prevBox.mirrorLists) / frames,
                       (es.mirrorVertices - s_prevBox.mirrorVertices) / frames);
        {
            // MIRROR = OFF + GAME SKIP: the game's mirror views not drawn.
            static u32 s_prevSkips = 0;
            if (g_mirror_pass_skips != s_prevSkips)
                log::write("[prof]   MIRROR GAME SKIP: %u of %u frames had their mirror view skipped\n",
                           g_mirror_pass_skips - s_prevSkips, frames);
            s_prevSkips = g_mirror_pass_skips;
        }
        static u32 s_prevInCarSkips = 0;
        if (g_reflection_incar_skips != s_prevInCarSkips)
            log::write("[prof]   IN-CAR REFLECT: %u of %u frames without a reflection map build (in-car view)\n",
                       g_reflection_incar_skips - s_prevInCarSkips, frames);
        s_prevInCarSkips = g_reflection_incar_skips;
        {
            // GAME COPIES (naomi_copies.cpp): the game's copy routines done natively.
            static NaomiGameCopyStats s_prevCopies = {};
            const NaomiGameCopyStats& gc = g_game_copy_stats;
            const NaomiGameCopyStats& was = s_prevCopies;
            u32 now = 0, before = 0;
            for (int k = 1; k < kGameCopyKinds; k++)
            {
                now += gc.calls[k];
                before += was.calls[k];
            }
            if (now != before)
                log::write("[prof]   GAME COPIES per frame: word copy %u calls, %u native (%u words), %u left to its "
                           "8-aligned loop; its word loop called directly %u, %u native (%u words); %u turned down "
                           "(last: dst %08X src %08X, %u bytes) | list copy %u calls, %u native (%u words) | list "
                           "head %u calls, %u native | INT DIVIDE %u signed (%u native), %u unsigned (%u native)\n",
                           (gc.calls[1] - was.calls[1]) / frames, (gc.native[1] - was.native[1]) / frames,
                           (gc.words[1] - was.words[1]) / frames, (gc.aligned8 - was.aligned8) / frames,
                           (gc.calls[4] - was.calls[4]) / frames, (gc.native[4] - was.native[4]) / frames,
                           (gc.words[4] - was.words[4]) / frames,
                           (gc.declined - was.declined) / frames, gc.lastDst, gc.lastSrc, gc.lastLen,
                           (gc.calls[2] - was.calls[2]) / frames, (gc.native[2] - was.native[2]) / frames,
                           (gc.words[2] - was.words[2]) / frames, (gc.calls[3] - was.calls[3]) / frames,
                           (gc.native[3] - was.native[3]) / frames, (gc.calls[5] - was.calls[5]) / frames,
                           (gc.native[5] - was.native[5]) / frames, (gc.calls[6] - was.calls[6]) / frames,
                           (gc.native[6] - was.native[6]) / frames);
            s_prevCopies = gc;
        }
        {
            // JIT FPU BANKS (decoder.cpp): what the blocks compiled so far got.
            static u32 s_prevUncrossed = 0, s_prevPhysical = 0;
            if (g_dec_fpu_uncrossed != s_prevUncrossed || g_dec_fpu_physical != s_prevPhysical)
                log::write("[prof]   JIT FPU BANKS: blocks compiled have %u renamed float-bank stretches closed with no "
                           "swap, %u ending in a swap (totals)\n",
                           g_dec_fpu_uncrossed, g_dec_fpu_physical);
            s_prevUncrossed = g_dec_fpu_uncrossed;
            s_prevPhysical = g_dec_fpu_physical;
        }
        const NaomiStats& ns = naomi_Stats();
        log::write("[prof]   cart DMA: %u transfers, %u KB, %u status polls while one ran (totals)\n",
                   ns.cartDmaCount, ns.cartDmaBytes >> 10, ns.cartDmaPolls);
        if (s_dimm)
        {
            const DimmImage::ReadStats& rs = s_dimm->readStats();
            log::write("[prof]   DIMM runs (totals): %u read on demand, %u decoded ahead, %u of those late, %u started\n",
                       rs.runsAsked, rs.runsAhead, rs.runsLate, rs.runsStarted);
        }
        // Where the profiler's "elan" milliseconds go (per emulated frame).
        const double tnl = elan_TimeMs(es.tnlNs - s_prevTnl) / frames;
        const double gpu = elan_TimeMs(es.gpuNs - s_prevGpuNs) / frames;
        const double conv = elan_TimeMs(es.convNs - s_prevConv) / frames;
        const double ta = elan_TimeMs(es.taNs - s_prevTa) / frames;
        const double all = elan_TimeMs(es.execNs - s_prevExec) / frames;
        log::write("[prof]   ELAN ms per frame: %.2f total = %.2f commands + %.2f T&L | T&L: %.2f GPU lists, %.2f CPU "
                   "vertex loop, %.2f other | TA feed (in both) %.2f\n",
                   all, all - tnl, tnl, gpu, conv, tnl - gpu - conv, ta);
        // The command walk (T&L left out) on dropped frames and on the others.
        const u32 dropped = g_rend_dropped_renders - s_prevDropped;
        const u32 kept = frames > dropped ? frames - dropped : 0;
        const double walkDropped = elan_TimeMs(es.execNsDropped - s_prevBox.execNsDropped);
        const double walkKept = (all - tnl) * frames - walkDropped;
        const u32 readDropped = es.walkReadDropped - s_prevBox.walkReadDropped;
        const u32 readKept = es.walkRead - s_prevBox.walkRead - readDropped;
        if (dropped)
            log::write("[prof]   ELAN walk: %u dropped frames, %.2f ms each, %u commands read each (%.0f ns a command) | "
                       "%u other frames, %.2f ms each, %u read each (%.0f ns)\n",
                       dropped, walkDropped / dropped, readDropped / dropped,
                       readDropped ? walkDropped * 1e6 / readDropped : 0.0, kept, kept ? walkKept / kept : 0.0,
                       kept ? readKept / kept : 0u, readKept ? walkKept * 1e6 / readKept : 0.0);
        else
            log::write("[prof]   ELAN walk: no dropped frame | %.2f ms a frame, %u commands read (%.0f ns a command)\n",
                       walkKept / frames, readKept / frames, readKept ? walkKept * 1e6 / readKept : 0.0);
        if (g_elan_model_prefetch_preset)
            log::write("[prof]   MODEL PREFETCH: %u models a frame had their headers asked for ahead\n",
                       (es.modelPrefetches - s_prevBox.modelPrefetches) / frames);
    }
    const NaomiElanStats& es = naomi_ElanStats();
    s_prevBox = es;
    s_prevIch = es.ichVertices;
    s_prevGpu = es.gpuVertices;
    s_prevLit = es.litVertices;
    s_prevExec = es.execNs;
    s_prevTnl = es.tnlNs;
    s_prevGpuNs = es.gpuNs;
    s_prevConv = es.convNs;
    s_prevTa = es.taNs;
    s_prevDropped = g_rend_dropped_renders;
    prev = m;
    prevUs = now;
    prevFrame = info.vblanks;
}
#endif

// Every 30 frames: remember where the game is, and warn once if the guest
// memory pool is running out.
void frameCheck(const NaomiFrameInfo& info)
{
    s_lastFrame = info;
#if defined(GEKKO)
    static bool warned = false;
    WiiGuestMemStats m;
    WiiGuestMem_Stats(&m);
    if (!warned && m.free_kb < 2048)
    {
        warned = true;
        logStatus("guest memory nearly full", info);
    }
    if (g_frame_prof_preset && info.vblanks % 120 == 0)
        logMemoryActivity(info);
#endif
}

} // namespace

bool runGame(GameSession& session, const BiosImage& bios, const RunOptions& options, std::string& error)
{
    log::write("[game] starting %s, BIOS library from %s, %s\n", session.header().displayTitle().c_str(),
               bios.source.c_str(), options.dynarec ? "SH-4 recompiler" : "SH-4 interpreter");
    // Same order as CoreOptionId.
    int* const presets[] = {
        &g_speed_limiter_preset, &g_idle_skip_preset, &g_elan_gx_preset, &g_elan_fetch_preset, &g_frameskip_preset,
        &g_elan_model_skip_preset,
        &g_texture_cache_preset, &g_vq_cmpr_preset,
        &g_tmem_cache_preset,    &g_4bpp_preset,
        &g_8bpp_preset,          &g_show_fps_overlay,    &g_frame_prof_preset,   &g_jit_ccalls_preset,
        &g_hotblocks_preset,     &g_fixed_depth_preset,
        &g_depth_clip_preset,    &g_hud_pass_preset,     &g_isp_depth_func_preset, &g_trans_sort_preset,
        &g_punch_through_preset, &g_autosort_preset,     &g_split_screen_preset, &g_vram_present_preset,
        &g_jit_sbp_preset,       &g_jit_ici_preset,
        &g_fastmem_preset,       &g_bcache_preset,       &g_dyn_ic_preset,       &g_fpu_pin_preset,
        &g_jit_align_preset,     &g_ifb_flush_preset,    &g_jit_newops_preset,   &g_jit_tfwd_preset,
        &g_jit_fmov_preset,      &g_jit_carry_preset,    &g_jit_mac_preset,      &g_jit_fschg_preset,
        &g_jit_cr0_preset,       &g_blockcopy_preset,    &g_jit_fsqrt_preset,    &g_jit_fipr_preset,
        &g_vram32_fast_preset,   &g_sq_fast_preset,      &g_audio_buffers_preset, &g_audio_block_preset, &g_audio_drc_preset,
        &g_sound_realtime_preset, &g_audio_stats_preset,
        &g_arm7_str_align_preset, &g_adpcm_loop_preset,  &g_arm7_reset_fix_preset, &g_aica_race_preset,
        &g_aica_ca_clamp_preset, &g_jit_link_guard_preset, &g_div32_fix_preset,  &g_interp_fix_preset,
        &g_spg_fix_preset,       &g_ta_guard_preset,     &g_inf_vtx_preset,
        &g_elan_point_preset,    &g_isp_cull_preset,     &g_elan_box_preset,     &g_reflection_rate_preset,
        &g_depth_plane_preset,   &g_cold_entry_preset,   &g_dimm_ahead_preset,   &g_fifo_wake_preset,
        &g_mem_sweep_preset,     &g_fifo_size_preset,    &g_soft_float_preset,
        &g_page_guard_preset,    &g_mirror_preset,       &g_sfx_menu_level_preset, &g_sfx_tire_level_preset,
        &g_small_cull_preset,    &g_incar_reflect_preset, &g_elan_dl_preset,     &g_elan_colour_fetch_preset,
        &g_elan_prefetch_preset, &g_two_player_preset, &g_gc_layout_preset, &g_p2_car_preset,
        &g_p2_scenery_preset,    &g_motion_fx_preset,  &g_motion_level_preset, &g_test_mode_preset,
        &g_between_warp_preset,  &g_headlight_preset,  &g_jit_frswap_preset,  &g_elan_model_prefetch_preset,
        &g_aica_half_mix_preset, &g_list_maths_preset, &g_walk_decode_preset, &g_draw_memo_preset,
        &g_sound_step_preset,    &g_sh4_wide_slice_preset, &g_cache_hints_preset,
        &g_pc_sampler_preset,    &g_game_copies_preset, &g_int_divide_preset, &g_jit_fpu_banks_preset,
        &g_screen_buffers_preset, &g_light_sum_preset};
    static_assert(sizeof(presets) / sizeof(presets[0]) == kCoreOptionCount, "one preset per CoreOptionId");
    for (int i = 0; i < kCoreOptionCount; i++)
    {
        const CoreOption& o = kCoreOptions[i];
        const int k = options.presets[i] >= 0 && options.presets[i] < o.count ? options.presets[i] : o.defaultIndex;
        *presets[i] = o.values[k];
        log::write("[game]   %s: %s\n", o.label, o.names[k]);
    }
    platform::setPadLayout(g_gc_layout_preset);
    if (g_elan_gx_preset)
    {
        // GPU-drawn vertices can lie behind the camera, so the hardware must
        // clip them. (Their depth range still reaches the renderer: the ELAN
        // measures it, so dynamic depth keeps working.)
        if (g_depth_clip_preset == 2)
        {
            g_depth_clip_preset = 1;
            log::write("[game]   3D T&L on the GPU: DEPTH CLIP set to NEAR MARGIN\n");
        }
    }
    naomi_SetLog(coreLog);
    naomi_SetCart(cartRead, &session.dimm(), session.dimm().size());
    naomi_SetCartBusy(cartBusy);
    session.dimm().setReadAhead(g_dimm_ahead_preset != 0);
    s_dimm = &session.dimm();
    naomi_SetBiosImage(bios.data.data(), (u32)bios.data.size());
    naomi_SetInputPoll(pollControls);
    naomi_SetFrameHook(frameCheck, 30);

#if defined(GEKKO)
    logHeap("heap before the core");
    // Sound: the AICA's samples go to an ASND voice (nullDC4Wii's
    // wii/wii_audio.cpp); AICA_Init() inside naomi_Init() opens the sink.
    wii_audio_startup();
#endif
    platform::captureStdout();   // the core's printf diagnostics into the log
    if (!naomi_Init(options.dynarec))
    {
        platform::releaseStdout();
#if defined(GEKKO)
        wii_audio_teardown();
#endif
        error = "the NAOMI 2 core could not start (see the log)";
        return false;
    }
#if defined(GEKKO)
    logHeap("heap after core init");
#endif
    log::setAsync(true);         // card writes off the emulation thread
    naomi_Run();
    log::setAsync(false);
    logStatus("stopped", s_lastFrame);
    naomi_Term();
#if defined(GEKKO)
    // Same ordered shutdown as the quit buttons (it also stops the audio).
    WiiExitToLoader();
#endif
    platform::releaseStdout();
    return true;
}

} // namespace idas3
