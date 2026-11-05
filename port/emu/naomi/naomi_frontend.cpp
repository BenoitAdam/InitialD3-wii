// Core lifecycle for the NAOMI 2 build: what nullDC4Wii's nullDC.cpp does
// for the Dreamcast (settings, CPU back-end choice, start/stop), minus the
// Dreamcast front end. Shared by the Wii and Windows builds.
#include "types.h"
#include "dc/dc.h"
#include "dc/sh4/sh4_if.h"
#include "plugins/plugin_manager.h"
#include "dc/mem/_vmem.h"
#include "dc/mem/sh4_mem.h"
#include "naomi.h"
#include "naomi_elan.h"
#include "naomi_frontend.h"

#include <ctime>
#if defined(GEKKO)
#include "wii/wii_fastmem.h"
#endif

// Last: defines one-letter register macros (r, sr, pr...).
#include "dc/sh4/sh4_registers.h"

__settings settings;
extern int g_audio_buffers_preset;   // host/presets_generated.cpp, set by the launch page

// Settings are fixed by the port rather than read from nullDC.cfg.
static bool s_useDynarec = false;

void LoadSettings()
{
    settings.dynarec.Enable = s_useDynarec;
    settings.dynarec.CPpass = true;
    settings.dynarec.UnderclockFpu = false;
    settings.dynarec.safemode = false;
    settings.dreamcast.cable = 0;      // NAOMI: 31 kHz VGA, no cable select
    settings.dreamcast.RTC = GetRTC_now();
    settings.emulator.AutoStart = true;
    settings.emulator.NoConsole = true;
    // Launch page AUDIO SYNC: 0 never paces emulation (drops on overrun),
    // N blocks the emulation once N audio blocks are queued (wii_audio.cpp).
    settings.emulator.AudioBuffers = g_audio_buffers_preset >= 0 ? g_audio_buffers_preset : 0;
}

void SaveSettings() {}

bool naomi_Init(bool useDynarec)
{
    s_useDynarec = useDynarec;
    LoadSettings();
#if defined(HOST_NO_REC)
    if (useDynarec)
        naomi_Log("[core] no recompiler on this host, using the interpreter\n");
    Get_Sh4Interpreter(&sh4_cpu);
#else
    if (useDynarec)
        Get_Sh4Recompiler(&sh4_cpu);
    else
        Get_Sh4Interpreter(&sh4_cpu);
#endif
    naomi_Log("[core] SH-4 back end: %s\n", (useDynarec && settings.dynarec.Enable) ? "recompiler" : "interpreter");
    // Guest RAM/VRAM/ARAM, before anything maps or initialises (nullDC4Wii's
    // EmuMain does the same).
    if (!_vmem_reserve())
    {
        naomi_Log("[core] cannot reserve the NAOMI 2 guest memory\n");
        return false;
    }
    // ELAN RAM: full 32 MB, or the largest power of two the host allows.
    bool elan = false;
    for (u32 size = ELAN_RAM_SIZE; size >= (4u << 20) && !elan; size >>= 1)
        elan = naomi_ElanAlloc(size);
    if (!elan)
    {
        naomi_Log("[core] cannot allocate ELAN RAM\n");
        return false;
    }
    naomi_CardReaderAttach();
    return Init_DC();
}

// ---- Frame heartbeat --------------------------------------------------------
static NaomiFrameHook s_frameHook = nullptr;
static u32 s_framePeriod = 120;
static NaomiFrameInfo s_frameInfo;

void naomi_SetFrameHook(NaomiFrameHook fn, u32 period)
{
    s_frameHook = fn;
    s_framePeriod = period ? period : 120;
}

void naomi_OnStartRender()
{
    s_frameInfo.renders++;
    tnl_OnStartRender();
#if !defined(GEKKO)
    tnl_DataCensusRender();
#endif
}

void naomi_VBlank()
{
    s_frameInfo.vblanks++;
#if defined(GEKKO)
    WiiGuestMem_Tick();   // compress a few cold guest pages when memory runs low
#endif
    naomi_CartSlice();    // the read-ahead thread's time, when it has work
    if (s_frameHook && s_frameInfo.vblanks % s_framePeriod == 0)
    {
        s_frameInfo.pc = next_pc;
        s_frameHook(s_frameInfo);
    }
}

static bool s_biosReported = false;
static u32 s_blockRing[32];
static u32 s_blockPos;

void naomi_NoteBlock(u32 pc)
{
    s_blockRing[s_blockPos++ & 31] = pc;
}

static bool inRam(u32 a)
{
    return (a & 0x1C000000) == 0x0C000000;
}

static void dumpCpuState(u32 site);

void naomi_ReportBiosEntry(u32 pc, u32 from)
{
    if (s_biosReported)
        return;
    s_biosReported = true;
    naomi_Log("[core] jump into BIOS ROM: %08X from %08X after %u frames | pr %08X sr %08X spc %08X ssr %08X vbr %08X "
              "expevt %03X intevt %03X\n",
              pc, from, s_frameInfo.vblanks, pr, sr.GetFull(), spc, ssr, vbr, ReadMem32(0xFF000024),
              ReadMem32(0xFF000028));
    dumpCpuState(from ? from : pr - 4);
}

void naomi_ReportAssert()
{
    static bool reported = false;
    if (reported)
        return;
    reported = true;
    // The handler is a leaf called with jsr: PR is the return address in the
    // function whose check failed.
    naomi_Log("[game] the game stopped on a failed internal check after %u frames: assert called from %08X "
              "(recompiler: registers below may be stale)\n",
              s_frameInfo.vblanks, pr - 4);
    dumpCpuState(pr - 4);
}

static void dumpCpuState(u32 site)
{
    naomi_Log("[core]   r0-7  %08X %08X %08X %08X %08X %08X %08X %08X\n", r[0], r[1], r[2], r[3], r[4], r[5], r[6],
              r[7]);
    naomi_Log("[core]   r8-15 %08X %08X %08X %08X %08X %08X %08X %08X\n", r[8], r[9], r[10], r[11], r[12], r[13],
              r[14], r[15]);
    if (inRam(r[15]))
        for (u32 i = 0; i < 24; i += 8)
        {
            const u32 a = r[15] + i * 4;
            naomi_Log("[core]   stack %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n", a, ReadMem32(a),
                      ReadMem32(a + 4), ReadMem32(a + 8), ReadMem32(a + 12), ReadMem32(a + 16), ReadMem32(a + 20),
                      ReadMem32(a + 24), ReadMem32(a + 28));
        }
    if (inRam(site))
        naomi_Log("[core]   code at %08X: %04X %04X %04X %04X %04X %04X\n", site - 6, ReadMem16(site - 6),
                  ReadMem16(site - 4), ReadMem16(site - 2), ReadMem16(site), ReadMem16(site + 2), ReadMem16(site + 4));
    // The BIOS service library the game copies to 0x0C018000 at boot.
    naomi_Log("[core]   ROM 0x60000: %08X %08X %08X %08X | RAM 0C018000: %08X %08X %08X %08X | table 0CB11E6C: %08X\n",
              ReadMem32(0xA0060000), ReadMem32(0xA0060004), ReadMem32(0xA0060008), ReadMem32(0xA006000C),
              ReadMem32(0xAC018000), ReadMem32(0xAC018004), ReadMem32(0xAC018008), ReadMem32(0xAC01800C),
              ReadMem32(0xAC000000 | 0x0CB11E6C));
#if defined(GEKKO)
    WiiGuestMemStats m;
    WiiGuestMem_Stats(&m);
    naomi_Log("[core]   guest memory: %u of %u KB free, ram %u vram %u elan %u aram %u KB | %u reclaims freed %u KB | "
              "%u failed allocations (last EA %08X)\n",
              m.free_kb, m.pool_kb, m.ram_kb, m.vram_kb, m.elan_kb, m.aram_kb, m.reclaims, m.reclaimed_kb,
              m.alloc_fails, m.last_fail_ea);
#endif
    if (s_blockPos == 0)
        return;
    naomi_Log("[core]   last compiled blocks, oldest first:\n");
    for (u32 i = 0; i < 32; i += 8)
    {
        const u32* b = s_blockRing;
        const u32 p = s_blockPos + i;
        naomi_Log("[core]     %08X %08X %08X %08X %08X %08X %08X %08X\n", b[p & 31], b[(p + 1) & 31], b[(p + 2) & 31],
                  b[(p + 3) & 31], b[(p + 4) & 31], b[(p + 5) & 31], b[(p + 6) & 31], b[(p + 7) & 31]);
    }
}

void naomi_CheckPc()
{
    if (s_biosReported || (next_pc & 0x1FFFFFFF) >= 0x00200000)
        return;
    s_biosReported = true;
    naomi_Log("[core] CPU entered BIOS ROM at %08X after %u frames: pr %08X sr %08X vbr %08X | last exception "
              "expevt %03X intevt %03X spc %08X ssr %08X\n",
              next_pc, s_frameInfo.vblanks, pr, sr.GetFull(), vbr, ReadMem32(0xFF000024), ReadMem32(0xFF000028), spc,
              ssr);
    naomi_Log("[core]   r0-7  %08X %08X %08X %08X %08X %08X %08X %08X\n", r[0], r[1], r[2], r[3], r[4], r[5], r[6],
              r[7]);
    naomi_Log("[core]   r8-15 %08X %08X %08X %08X %08X %08X %08X %08X\n", r[8], r[9], r[10], r[11], r[12], r[13],
              r[14], r[15]);
}

// ---- SH-4 speed hacks ---------------------------------------------------------
// Launch options (game_runner.cpp).
extern "C" {
int g_idle_skip_preset = 1;     // idle loop below: charge it 30x its cycles
int g_vram32_fast_preset = 1;   // rec: inline 32-bit VRAM stores (wii_driver.cpp)
int g_sq_fast_preset = 1;       // rec: inline store-queue flushes to memory (wii_driver.cpp shop_pref)
int g_cold_entry_preset = 1;    // rec: timeslice exit after the block's tail (wii_driver.cpp ngen_Begin)
int g_fifo_wake_preset = 1;     // gxRend: wake from a full GX FIFO early (InitRenderer)
int g_mem_sweep_preset = 1;     // wii_fastmem: short synchronous sweeps (page_alloc)
int g_fifo_size_preset = 1;     // gxRend: 512 KB GX FIFO (InitRenderer)
int g_page_guard_preset = 1;    // wii_fastmem: unwritten ELAN pages read-only (WiiGuestMem_ElanGpuPage)
int g_sound_realtime_preset = 0; // ARM7 + AICA follow the real clock (plugs/vbaARM/arm_aica.cpp)
}

// Initial D Arcade Stage 3 (GDS-0033) waits for the frame interrupt in a loop
// spread over four blocks - an empty idle hook called through a pointer, a
// timeout check, a flag poll - which the decoder's one-block idle heuristic
// never recognises: ~9000 iterations a race frame, about 1M PowerPC
// instructions spent waiting. Charging its blocks 30x their cycles lets the
// same emulated time pass in a thirtieth of the iterations; interrupts still
// arrive at the same emulated time. Each block is matched on its address,
// length and first and last opcodes, so other code is never touched.
namespace {
struct IdleBlock
{
    u32 pc, bytes;
    u16 first, last;
};
const IdleBlock kIdleBlocks[] = {
    {0x0C20C854, 8, 0x54AC, 0x0009},    // r4 = [r10+48]; r1 = [r10+44]; jsr @r1; nop
    {0x0C20C85C, 14, 0x528B, 0x89BA},   // limit = [r8+44] + [r8+40] + 1; counter > limit -> leave
    {0x0C20C86A, 6, 0x519D, 0x8BF1},    // [r9+52] != 0 -> loop
    {0x0C211880, 12, 0x2FE6, 0x0009},   // the idle hook: push r14, pop r14, rts
};
} // namespace

extern "C" u32 naomi_IdleCycleScale(u32 pc, u32 bytes)
{
    if (!g_idle_skip_preset || bytes < 2)
        return 1;
    const u32 p = pc & 0x1FFFFFFF;
    for (const IdleBlock& b : kIdleBlocks)
        if (p == b.pc && bytes == b.bytes && ReadMem16(pc) == b.first && ReadMem16(pc + bytes - 2) == b.last)
            return 30;
    return 1;
}

// REFLECTIONS. The game's SH-4 code rebuilds the car body's reflection map
// every frame: a 64x32 panorama of the road side, put together cell by cell in
// a buffer it keeps (0C048E80-0C0491BF, 20% of its instructions in a race; 1
// build in 4 took ~1 ms off `sh4` on a Wii). Sent back at the first row to the
// routine's own "no rows" exit, a build costs a few dozen instructions, cleans
// up as usual and leaves the last panorama in place: the reflection only
// updates less often. One build in g_reflection_rate_preset is made. Matched on
// its opcodes, as the idle loop is.
int g_reflection_rate_preset = 4;

extern "C" u32 naomi_ReflectionExit(u32 pc)
{
    const u32 exitPc = (pc & 0xE0000000) | 0x0C0491CE;
    if (g_reflection_rate_preset < 2 || (pc & 0x1FFFFFFF) != 0x0C048E86 || ReadMem16(pc) != 0x9A3C ||
        ReadMem16(exitPc) != 0x9029)
        return 0;
    return exitPc;
}

extern "C" u32 naomi_ReflectionSkip(u32 row)
{
    static u32 builds = 0;
    return row == 0 && ++builds % (u32)g_reflection_rate_preset != 0;
}

void naomi_Run()
{
    Start_DC();   // blocks until naomi_Stop() is called from inside the run
}

void naomi_Stop()
{
    if (sh4_cpu.Stop)
        sh4_cpu.Stop();
}

void naomi_Term()
{
    Term_DC();
}
