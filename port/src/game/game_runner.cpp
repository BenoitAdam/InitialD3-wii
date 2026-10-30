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
extern "C" int g_sound_realtime_preset;
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
    platform::readGameControls(c);
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
    in.test = c.test;
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

NaomiFrameInfo s_lastFrame;

#if defined(GEKKO)
// With PROFILER on: guest memory activity per real second, every 300 frames.
// Page faults and decompressions are exceptions, which cost a lot (especially
// under Dolphin), so these rates show whether memory paging eats the frame.
u32 s_prevIch = 0, s_prevGpu = 0, s_prevLit = 0;
u64 s_prevExec = 0, s_prevTnl = 0, s_prevGpuNs = 0, s_prevConv = 0, s_prevTa = 0;
NaomiElanStats s_prevBox = {};        // the 3D LIST CULL counters at the last line

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
        log::write("[prof]   3D list cull per frame: %u lists with a box, %u dropped behind the camera, %u out of "
                   "view (%u vertices) | depth measured on %u vertices\n",
                   (es.boxLists - s_prevBox.boxLists) / frames, (es.boxCulledBehind - s_prevBox.boxCulledBehind) / frames,
                   (es.boxCulledSide - s_prevBox.boxCulledSide) / frames,
                   (es.boxCulledVertices - s_prevBox.boxCulledVertices) / frames,
                   (es.depthMeasured - s_prevBox.depthMeasured) / frames);
        const NaomiStats& ns = naomi_Stats();
        log::write("[prof]   cart DMA: %u transfers, %u KB, %u status polls while one ran (totals)\n",
                   ns.cartDmaCount, ns.cartDmaBytes >> 10, ns.cartDmaPolls);
        // Where the profiler's "elan" milliseconds go (per emulated frame).
        const double tnl = elan_TimeMs(es.tnlNs - s_prevTnl) / frames;
        const double gpu = elan_TimeMs(es.gpuNs - s_prevGpuNs) / frames;
        const double conv = elan_TimeMs(es.convNs - s_prevConv) / frames;
        const double ta = elan_TimeMs(es.taNs - s_prevTa) / frames;
        const double all = elan_TimeMs(es.execNs - s_prevExec) / frames;
        log::write("[prof]   ELAN ms per frame: %.2f total = %.2f commands + %.2f T&L | T&L: %.2f GPU lists, %.2f CPU "
                   "vertex loop, %.2f other | TA feed (in both) %.2f\n",
                   all, all - tnl, tnl, gpu, conv, tnl - gpu - conv, ta);
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
        &g_elan_point_preset,    &g_isp_cull_preset,     &g_elan_box_preset};
    static_assert(sizeof(presets) / sizeof(presets[0]) == kCoreOptionCount, "one preset per CoreOptionId");
    for (int i = 0; i < kCoreOptionCount; i++)
    {
        const CoreOption& o = kCoreOptions[i];
        const int k = options.presets[i] >= 0 && options.presets[i] < o.count ? options.presets[i] : o.defaultIndex;
        *presets[i] = o.values[k];
        log::write("[game]   %s: %s\n", o.label, o.names[k]);
    }
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
