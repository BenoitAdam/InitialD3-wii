// NAOMI 2 ELAN (area 2): the geometry/lighting processor that sits between
// the SH-4 and the two PowerVR CLX2 chips.
//
//   0x08000000-0x08FFFFFF  registers: 0x085F6800-0x085F9FFF reach the same
//                          system-bus/PVR register file as area 0 (the ELAN
//                          forwards them to both PVRs), 0x08800000 holds the
//                          ELAN's own registers
//   0x09000000-0x09FFFFFF  command FIFO (8 words, executed on the 8th)
//   0x0A000000-0x0BFFFFFF  ELAN RAM (display lists, models, textures)
//
// Register behaviour follows what Flycast documents for the hardware
// (reimplemented). This file holds the memory, registers and FIFO; the
// commands themselves are executed in naomi_elan_cmd.cpp.
//
// Status register 0x74: bit 0 DMA done, bit 1 command done (write 1 to
// clear). Bits 2-5 are read inverted by the game's driver (a 0 there would
// mean a geometry timeout) but the hardware reports them as 0 when idle.
#include "types.h"
#include "dc/mem/_vmem.h"
#include "dc/mem/sb.h"
#include "dc/pvr/pvr_if.h"
#include "naomi.h"
#include "naomi_elan.h"
#include "plugs/drkPvr/frame_prof.h"
#if HOST_OS != OS_WII
#include "host/host_hooks.h"
#else
#include "wii/wii_fastmem.h"
#endif

#include <cstdlib>
#include <cstring>
#if defined(_MSC_VER) || defined(GEKKO)
#include <malloc.h>
#endif

#if !defined(BUILD_NAOMI2)
#error "naomi_elan.cpp is only part of the NAOMI 2 build"
#endif

static u8* s_ram = nullptr;
static u32 s_ramSize = 0;
static NaomiElanStats s_stats;

static u32 s_reg10;
static u32 s_reg30 = 0x31;
static u32 s_reg74;
static u32 s_cmd[8];
static u32 s_recent[16][8];     // bring-up: last commands, dumped when the game polls

static u32 s_logged[64];
static s32 s_dmaCycles;         // > 0 while a texture DMA is in flight

static void logOnce(const char* what, u32 addr, u32 data)
{
    const u32 slot = (addr >> 2) & 63;
    if (s_logged[slot] == addr + 1)
        return;
    s_logged[slot] = addr + 1;
    naomi_Log("[elan] %s %08X = %08X\n", what, addr, data);
}

const NaomiElanStats& naomi_ElanStats()
{
    return s_stats;
}

NaomiElanStats& elan_Stats()
{
    return s_stats;
}

void naomi_ElanLogRecent()
{
    for (u32 i = 1; i <= 16; i++)
    {
        const u32* c = s_recent[(s_stats.commands + i) & 15];
        naomi_Log("[elan]   recent %u: %08X %08X %08X %08X %08X %08X %08X %08X\n", s_stats.commands + i - 16, c[0], c[1],
                  c[2], c[3], c[4], c[5], c[6], c[7]);
    }
}

void elan_StartDma(u32 cycles)
{
    s_dmaCycles = (s32)cycles;
}

bool elan_DmaBusy()
{
    return s_dmaCycles > 0;
}

void naomi_ElanUpdate(u32 cycles)
{
    if (s_dmaCycles > 0)
    {
        s_dmaCycles -= (s32)cycles;
        if (s_dmaCycles <= 0)
        {
            s_dmaCycles = 0;
            s_reg74 |= 1;
        }
    }
}

u8* naomi_ElanRam()
{
    return s_ram;
}

u32 naomi_ElanRamSize()
{
    return s_ramSize;
}

bool naomi_ElanAlloc(u32 bytes)
{
    // Power of two: the block is mirrored over the 32 MB window.
    u32 size = 1u << 20;
    while (size < bytes && size < ELAN_RAM_SIZE)
        size <<= 1;
#if HOST_OS == OS_WII
    // The demand-paged window (wii/wii_fastmem.cpp): full size, nothing is
    // allocated until the game writes to a page.
    size = ELAN_RAM_SIZE;
    s_ram = (u8*)WII_GUEST_ELAN_EA;
#else
    s_ram = (u8*)host_GuestAlloc(size);     // zeroed, write-watched
#endif
    if (!s_ram)
        return false;
    s_ramSize = size;
    naomi_Log("[elan] RAM: %u KB%s\n", size / 1024, size < ELAN_RAM_SIZE ? " (mirrored, smaller than hardware)" : "");
    return true;
}

void naomi_ElanReset()
{
    if (s_ram && !guestmem_Discard(s_ram, s_ramSize))
        memset(s_ram, 0, s_ramSize);
    s_reg10 = 0;
    s_reg30 = 0x31;
    s_reg74 = 0;
    memset(s_cmd, 0, sizeof(s_cmd));
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_logged, 0, sizeof(s_logged));
    s_dmaCycles = 0;
    elan_CmdReset();
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
static u32 regRead(u32 paddr, u32 sz)
{
    const u32 addr = paddr & 0x01FFFFFF;
    s_stats.regReads++;
    if ((s_stats.regReads & 0xFFFFF) == 0)
    {
        naomi_Log("[elan] poll sample: read %08X (reg74 %X)\n", addr, s_reg74);
        static u32 dumpedAt;
        if (dumpedAt != s_stats.commands)
        {
            dumpedAt = s_stats.commands;
            naomi_ElanLogRecent();
        }
    }
    if ((addr >> 16) == 0x5F)
    {
        if (addr >= 0x005F6800 && addr <= 0x005F7CFF)
            return sb_ReadMem(addr, sz);
        if (addr >= 0x005F8000 && addr <= 0x005F9FFF)
            return pvr_readreg_TA(addr, sz);
    }
    else if ((addr >> 16) == 0x80)
    {
        switch (addr & 0xFF)
        {
        case 0x00: return 0xE1AD0000;   // magic number
        case 0x04: return 0x10;         // revision
        case 0x0C: return 1;            // command queue depth (games wait for < 2/3)
        case 0x10: return s_reg10;      // SH-4 interface control
        case 0x14: return 0x2029;       // SDRAM refresh
        case 0x1C: return 0x87320961;   // SDRAM config
        case 0x30: return s_reg30;      // macro tiler config
        case 0x74: return s_reg74;      // status: b0 DMA done, b1 command done
        case 0x78: return 0;            // IRQ mask
        default: break;
        }
    }
    logOnce("read  unhandled", addr, 0);
    return 0;
}

static void regWrite(u32 paddr, u32 data, u32 sz)
{
    const u32 addr = paddr & 0x01FFFFFF;
    s_stats.regWrites++;
    if ((addr >> 16) == 0x5F)
    {
        if (addr >= 0x005F6800 && addr <= 0x005F7CFF)
        {
            sb_WriteMem(addr, data, sz);
            return;
        }
        if (addr >= 0x005F8000 && addr <= 0x005F9FFF)
        {
            // Initial D 3's assert handlers paint the border magenta and spin
            // forever; they are called with jsr, so PR names the failed check.
            if (addr == 0x005F8040 && data == 0x00FF00FF)
                naomi_ReportAssert();
            pvr_writereg_TA(addr, data, sz);
            return;
        }
    }
    else if ((addr >> 16) == 0x80)
    {
        switch (addr & 0xFF)
        {
        case 0x08:                      // reset-ish: 0 clears the status
            if (data == 0)
                s_reg74 = 0;
            return;
        case 0x10:
            s_reg10 = data;
            return;
        case 0x30:
            s_reg30 = data;
            return;
        case 0x74:                      // write 1 to clear status bits
            s_reg74 &= ~data;
            return;
        case 0x00: case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x78: case 0x7C: case 0x80: case 0x84: case 0x88: case 0x90: case 0xD0:
            return;                     // configuration we do not model
        default:
            break;
        }
    }
    logOnce("write unhandled", addr, data);
}

// One vmem handler serves both pages (the core only has 8 handler slots):
// 0x08xxxxxx registers, 0x09xxxxxx command FIFO.
static bool isCommandPage(u32 addr)
{
    return ((addr >> 24) & 1) != 0;
}

static void commandWrite(u32 addr, u32 data)
{
    const u32 index = (addr & 31) / 4;
    s_cmd[index] = data;
    if (index == 7)
    {
        s_stats.commands++;
        FP_T0(t0);                      // FRAME_PROF "elan" bucket (frame_prof.cpp)
        ELAN_TIME_BEGIN(tx);
        elan_Execute(s_cmd, sizeof(s_cmd));
        ELAN_TIME_END(execNs, tx);
        FP_ACC(disc, t0);
        FP_BUMP(n_disc);
        if (!elan_DmaBusy())
            s_reg74 |= 2;
        if (s_stats.commands <= 40)
            naomi_Log("[elan] cmd %4u: %08X %08X %08X %08X %08X %08X %08X %08X\n", s_stats.commands, s_cmd[0],
                      s_cmd[1], s_cmd[2], s_cmd[3], s_cmd[4], s_cmd[5], s_cmd[6], s_cmd[7]);
        memcpy(s_recent[s_stats.commands & 15], s_cmd, sizeof(s_cmd));
    }
}

template <u32 sz, class T>
static T FASTCALL elanRead(u32 addr)
{
    if (isCommandPage(addr))
    {
        logOnce("read  command FIFO", addr & 0x01FFFFFF, 0);
        return 0;
    }
    return (T)regRead(addr, sz);
}

template <u32 sz, class T>
static void FASTCALL elanWrite(u32 addr, T data)
{
    if (isCommandPage(addr))
        commandWrite(addr, (u32)data);
    else
        regWrite(addr, (u32)data, sz);
}

static _vmem_handler s_handler;

void naomi_ElanMapInit()
{
    s_handler = _vmem_register_handler_Template(elanRead, elanWrite);
}

void naomi_ElanMap(u32 base)
{
    _vmem_map_handler(s_handler, 0x08 | base, 0x09 | base);
    if (s_ram)
        _vmem_map_block(s_ram, 0x0A | base, 0x0B | base, s_ramSize - 1);
}
