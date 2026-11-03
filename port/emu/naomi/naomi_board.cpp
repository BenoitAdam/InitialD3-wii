// NAOMI cartridge / DIMM-board registers (0x005F7000-0x005F70FF) and the
// G1 cart DMA, for the NAOMI 2 build of the core.
//
// This file implements dc/gdrom/gdrom_if.h: on a NAOMI the G1 bus carries the
// cartridge (here the GD-ROM DIMM board) instead of a GD-ROM drive, so the
// core's existing hooks - the area-0 register window, the SB_GDST/SB_GDEN
// DMA registers and the slow-update tick - land here unchanged.
//
// Register behaviour follows the hardware as documented by MAME's naomigd
// and Flycast's naomi_cart/gdcartridge (reimplemented, not copied).
#include "types.h"
#include "dc/gdrom/gdrom_if.h"
#include "dc/mem/sb.h"
#include "dc/mem/sh4_mem.h"
#include "dc/asic/asic.h"
#include "naomi.h"
#include "plugs/drkPvr/frame_prof.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#if !defined(BUILD_NAOMI)
#error "naomi_board.cpp is only part of the NAOMI 2 build (DC_PLATFORM=2)"
#endif

// ---------------------------------------------------------------------------
// Register map
// ---------------------------------------------------------------------------
enum : u32
{
    NAOMI_ROM_OFFSETH = 0x5F7000,
    NAOMI_ROM_OFFSETL = 0x5F7004,
    NAOMI_ROM_DATA = 0x5F7008,
    NAOMI_DMA_OFFSETH = 0x5F700C,
    NAOMI_DMA_OFFSETL = 0x5F7010,
    NAOMI_DMA_COUNT = 0x5F7014,
    NAOMI_COMM2_CTRL = 0x5F7018,     // 840-0001E communication board
    NAOMI_COMM2_STATUS1 = 0x5F7028,
    NAOMI_DIMM_COMMAND = 0x5F703C,
    NAOMI_DIMM_OFFSETL = 0x5F7040,
    NAOMI_DIMM_PARAMETERL = 0x5F7044,
    NAOMI_DIMM_PARAMETERH = 0x5F7048,
    NAOMI_DIMM_STATUS = 0x5F704C,
    NAOMI_MBOARD_OFFSET = 0x5F7050,
    NAOMI_MBOARD_DATA = 0x5F7054,
    NAOMI_MBOARD_STATUS = 0x5F705C,
    NAOMI_LED = 0x5F7068,
    NAOMI_BOARDID_WRITE = 0x5F7078,
    NAOMI_BOARDID_READ = 0x5F707C,
};

// ---------------------------------------------------------------------------
// Cart image + logging (set by the host)
// ---------------------------------------------------------------------------
static NaomiCartReadFn s_cartRead = nullptr;
static NaomiCartBusyFn s_cartBusy = nullptr;
static void* s_cartUser = nullptr;
static u32 s_cartSize = 0;
static NaomiLogFn s_log = nullptr;

// The emulation steps aside this long per vblank while the host decodes cart
// data in the background. Below 100% speed it never sleeps on its own, so a
// thread under its priority gets no time otherwise: a decode spreads over
// several frames instead of stopping one.
constexpr u32 kCartSliceUs = 1000;
static NaomiStats s_stats;

// The DIMM board decodes offsets within its RAM window, a power of two at
// least as large as the image: the game sets flag bits above it (it loads
// with DMA offsets such as 0x2080xxxx and 0x4000xxxx), which must not reach
// the image as part of the offset.
static u32 s_cartMask = 0xFFFFFFFF;

void naomi_SetCart(NaomiCartReadFn read, void* user, u32 size)
{
    s_cartRead = read;
    s_cartUser = user;
    s_cartSize = size;
    u32 window = 1u << 12;
    while (window < size && window < (1u << 31))
        window <<= 1;
    s_cartMask = window - 1;
}

void naomi_SetCartBusy(NaomiCartBusyFn busy)
{
    s_cartBusy = busy;
}

void naomi_CartSlice()
{
    if (s_cartBusy && s_cartBusy(s_cartUser))
    {
        FP_T0(t0);
        usleep(kCartSliceUs);
        FP_ACC(cart, t0);
    }
}

bool naomi_CartRead(u32 offset, u32 length, u8* dst)
{
    if (!s_cartRead)
    {
        memset(dst, 0, length);
        return false;
    }
    offset &= s_cartMask;
    if (length > s_cartMask + 1 - offset)
    {
        // Wraps at the top of the window.
        const u32 first = s_cartMask + 1 - offset;
        return s_cartRead(s_cartUser, offset, first, dst) && s_cartRead(s_cartUser, 0, length - first, dst + first);
    }
    return s_cartRead(s_cartUser, offset, length, dst);
}

u32 naomi_CartSize()
{
    return s_cartSize;
}

const NaomiStats& naomi_Stats()
{
    return s_stats;
}

void naomi_SetLog(NaomiLogFn fn)
{
    s_log = fn;
}

void naomi_Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_log)
        s_log(buf);
    else
        fputs(buf, stdout);
}

// ---------------------------------------------------------------------------
// Board state
// ---------------------------------------------------------------------------
static u32 s_romPioOffset;
static bool s_romPioAutoInc;
static u32 s_dmaOffset;
static u32 s_dmaCount;

static u16 s_dimmCommand;
static u16 s_dimmOffsetL;
static u16 s_dimmParamL;
static u16 s_dimmParamH;

// Cart DMA in flight: the data is copied when it starts, the completion IRQ
// is raised after the time the transfer takes on hardware.
static bool s_dmaPending;
static s32 s_dmaCyclesLeft;
static u32 s_dmaEnd;
static u32 s_dmaLen;

// ~20 MB/s from the DIMM board to system RAM (Flycast's measurement-based
// default): 200 MHz / 20 MB/s = 10 SH-4 cycles per byte.
static const u32 kDmaCyclesPerByte = 10;

// A game waiting for the transfer polls SB_GDST in a loop of a few SH-4
// cycles, and the completion is only checked every 14 336 cycles (the slow
// update tier): ~2000 polls for a small refill, ~20K a frame on Initial D's
// loading screens, each one a trip through the register handlers. So every
// poll while a transfer runs also counts as this many cycles of it: the loop
// ends after a few dozen polls, and a game that polls now and then only sees
// its transfer finish a few thousand cycles early.
static const s32 kDmaPollCredit = 4096;

// Unhandled registers are logged once each so a run log stays readable.
static u32 s_loggedReads[64];
static u32 s_loggedWrites[64];

static void logOnce(u32* table, const char* what, u32 addr, u32 data, u32 sz)
{
    const u32 slot = (addr >> 2) & 63;
    if (table[slot] == addr + 1)
        return;
    table[slot] = addr + 1;
    naomi_Log("[naomi] %s %08X (size %u) data %08X\n", what, addr, sz, data);
}

// ---------------------------------------------------------------------------
// DIMM board command channel
// ---------------------------------------------------------------------------
// The NAOMI and the DIMM board talk through four 16-bit registers and the
// EXP_PCI interrupt. Commands from the NAOMI side are signalled by writing
// DIMM_STATUS with bit 0 clear; the board answers by updating the registers
// and raising EXP_PCI.
static void dimmReturn(bool failed, u16 offsetl, u32 parameter)
{
    s_dimmCommand = (u16)(((s_dimmCommand & 0x7e00) + 0x400) | (failed ? 0xff : 0x4));
    s_dimmOffsetL = offsetl;
    s_dimmParamH = (u16)(parameter >> 16);
    s_dimmParamL = (u16)parameter;
    asic_RaiseInterrupt(holly_EXP_PCI);
}

static void dimmProcess()
{
    s_stats.dimmCommands++;
    const int group = (s_dimmCommand >> 13) & 3;
    const int cmd = (s_dimmCommand >> 9) & 0xf;
    naomi_Log("[naomi] DIMM command group %d cmd %X (raw %04X off %04X param %04X%04X)\n", group, cmd,
              s_dimmCommand, s_dimmOffsetL, s_dimmParamH, s_dimmParamL);
    if (group == 0 && cmd == 0)
        return;                         // nop
    // Network and unknown commands: this board has no network.
    dimmReturn(true, 0, 0xffffffff);
}

// ---------------------------------------------------------------------------
// G1 cart DMA (SB_GDST / SB_GDEN)
// ---------------------------------------------------------------------------
static void cartDmaStart(u32 data)
{
    if ((data & 1) == 0 || SB_GDST == 1)
        return;
    if (SB_GDEN == 0)
    {
        naomi_Log("[naomi] cart DMA start ignored: SB_GDEN=0\n");
        return;
    }

    const u32 len = (SB_GDLEN + 31) & ~31u;
    u32 dst = SB_GDSTAR & 0x1FFFFFE0;
    s_stats.cartDmaCount++;
    s_stats.cartDmaBytes += len;

    // Copy now, through the core's endian-correct block writer.
    static u32 buffer[16 * 1024];
    u32 done = 0;
    while (done < len)
    {
        const u32 chunk = (len - done) < sizeof(buffer) ? (len - done) : (u32)sizeof(buffer);
        FP_T0(t0);
        naomi_CartRead(s_dmaOffset + done, chunk, (u8*)buffer);
        FP_ACC(cart, t0);
        WriteMemBlock_nommu_ptr_le32(dst + done, buffer, chunk);
        done += chunk;
    }
    s_dmaOffset += len;

    SB_GDST = 1;
    s_dmaPending = true;
    s_dmaLen = len;
    s_dmaEnd = dst + len;
    s_dmaCyclesLeft = (s32)(len * kDmaCyclesPerByte);
}

static void cartDmaFinish()
{
    s_dmaPending = false;
    SB_GDST = 0;
    SB_GDSTARD = s_dmaEnd;
    SB_GDLEND = s_dmaLen;
    asic_RaiseInterrupt(holly_GDROM_DMA);
}

static void sb_GDST_write(u32 data)
{
    cartDmaStart(data);
}

static u32 sb_GDST_read()
{
    if (s_dmaPending)
    {
        s_stats.cartDmaPolls++;
        s_dmaCyclesLeft -= kDmaPollCredit;
        if (s_dmaCyclesLeft <= 0)
            cartDmaFinish();
    }
    return SB_GDST;
}

static void sb_GDEN_write(u32 data)
{
    SB_GDEN = data & 1;
    if (SB_GDEN == 0 && s_dmaPending)
    {
        naomi_Log("[naomi] cart DMA aborted\n");
        s_dmaPending = false;
        SB_GDST = 0;
    }
}

// ---------------------------------------------------------------------------
// gdrom_if.h implementation
// ---------------------------------------------------------------------------
void gdrom_reg_Init()
{
    sb_regs[(SB_GDST_addr - SB_BASE) >> 2].flags = REG_32BIT_READWRITE;
    sb_regs[(SB_GDST_addr - SB_BASE) >> 2].readFunction = sb_GDST_read;
    sb_regs[(SB_GDST_addr - SB_BASE) >> 2].writeFunction = sb_GDST_write;
    sb_regs[(SB_GDEN_addr - SB_BASE) >> 2].flags = REG_32BIT_READWRITE | REG_READ_DATA;
    sb_regs[(SB_GDEN_addr - SB_BASE) >> 2].writeFunction = sb_GDEN_write;
}

void gdrom_reg_Term() {}

void gdrom_reg_Reset(bool Manual)
{
    s_romPioOffset = 0;
    s_romPioAutoInc = false;
    s_dmaOffset = 0;
    s_dmaCount = 0;
    s_dimmCommand = 0;
    s_dimmOffsetL = 0;
    s_dimmParamL = 0;
    s_dimmParamH = 0;
    s_dmaPending = false;
    SB_GDST = 0;
    SB_GDEN = 0;
    memset(s_loggedReads, 0, sizeof(s_loggedReads));
    memset(s_loggedWrites, 0, sizeof(s_loggedWrites));
}

// Bring-up: accesses per board register (0x5F7000-0x5F70FF), see naomi_BoardLogCounters.
static u32 s_regReads[64], s_regWrites[64];

void naomi_BoardLogCounters()
{
    for (u32 i = 0; i < 64; i++)
        if (s_regReads[i] || s_regWrites[i])
            naomi_Log("[naomi] reg %08X: %u reads, %u writes\n", 0x005F7000 + i * 4, s_regReads[i], s_regWrites[i]);
}

u32 ReadMem_gdrom(u32 addr, u32 sz)
{
    s_stats.boardRegReads++;
    s_regReads[(addr >> 2) & 63]++;
    switch (addr)
    {
    case NAOMI_ROM_OFFSETH:
        return (s_romPioOffset >> 16) | (s_romPioAutoInc ? 0x8000 : 0);
    case NAOMI_ROM_OFFSETL:
        return s_romPioOffset & 0xFFFF;
    case NAOMI_ROM_DATA:
    {
        u8 b[2];
        naomi_CartRead(s_romPioOffset, 2, b);
        s_stats.cartPioReads++;
        if (s_romPioAutoInc)
            s_romPioOffset += 2;
        return (u32)b[0] | ((u32)b[1] << 8);
    }
    case NAOMI_DMA_OFFSETH:
        return s_dmaOffset >> 16;
    case NAOMI_DMA_OFFSETL:
        return s_dmaOffset & 0xFFFF;
    case NAOMI_DMA_COUNT:
        return s_dmaCount & 0xFFFF;
    case NAOMI_DIMM_COMMAND:
        return s_dimmCommand;
    case NAOMI_DIMM_OFFSETL:
        return s_dimmOffsetL;
    case NAOMI_DIMM_PARAMETERL:
        return s_dimmParamL;
    case NAOMI_DIMM_PARAMETERH:
        return s_dimmParamH;
    case NAOMI_DIMM_STATUS:
        // Bit 8 low while the board's interrupt to the NAOMI is pending.
        return 0x111 & ~(((SB_ISTEXT >> 3) & 1) << 8);
    case NAOMI_BOARDID_WRITE:
        return 1;
    case NAOMI_BOARDID_READ:
        logOnce(s_loggedReads, "read  BOARDID", addr, 0, sz);
        return 0;
    case NAOMI_MBOARD_DATA:
    case NAOMI_MBOARD_OFFSET:
        return 1;
    default:
        break;
    }
    logOnce(s_loggedReads, "read  unhandled", addr, 0, sz);
    return 0xFFFF;
}

void WriteMem_gdrom(u32 addr, u32 data, u32 sz)
{
    s_stats.boardRegWrites++;
    s_regWrites[(addr >> 2) & 63]++;
    switch (addr)
    {
    case NAOMI_ROM_OFFSETH:
        s_romPioAutoInc = (data & 0x8000) != 0;
        s_romPioOffset = (s_romPioOffset & 0x0000FFFF) | ((data << 16) & 0x7FFF0000);
        return;
    case NAOMI_ROM_OFFSETL:
        s_romPioOffset = (s_romPioOffset & 0xFFFF0000) | (data & 0xFFFF);
        return;
    case NAOMI_ROM_DATA:
        // The DIMM image is read-only from the game's side.
        if (s_romPioAutoInc)
            s_romPioOffset += 2;
        return;
    case NAOMI_DMA_OFFSETH:
    {
        static u32 logged, last = ~0u;
        if (data != last && logged < 40)
        {
            logged++;
            naomi_Log("[naomi] DMA_OFFSETH <= %04X (dma offset %08X count %X)\n", data, s_dmaOffset, s_dmaCount);
        }
        last = data;
        s_dmaOffset = (s_dmaOffset & 0x0000FFFF) | ((data & 0x7FFF) << 16);
        return;
    }
    case NAOMI_DMA_OFFSETL:
        s_dmaOffset = (s_dmaOffset & 0xFFFF0000) | (data & 0xFFFF);
        return;
    case NAOMI_DMA_COUNT:
        s_dmaCount = data;
        return;
    case NAOMI_DIMM_COMMAND:
        s_dimmCommand = (u16)data;
        return;
    case NAOMI_DIMM_OFFSETL:
        s_dimmOffsetL = (u16)data;
        return;
    case NAOMI_DIMM_PARAMETERL:
        s_dimmParamL = (u16)data;
        return;
    case NAOMI_DIMM_PARAMETERH:
        s_dimmParamH = (u16)data;
        return;
    case NAOMI_DIMM_STATUS:
        if (data & 0x100)
            asic_CancelInterrupt(holly_EXP_PCI);
        if ((data & 1) == 0)
            dimmProcess();
        return;
    case NAOMI_LED:
        return;
    case NAOMI_BOARDID_WRITE:
        logOnce(s_loggedWrites, "write BOARDID", addr, data, sz);
        return;
    default:
        break;
    }
    if (addr >= NAOMI_COMM2_CTRL && addr <= NAOMI_COMM2_STATUS1)
    {
        logOnce(s_loggedWrites, "write COMM2", addr, data, sz);
        return;
    }
    logOnce(s_loggedWrites, "write unhandled", addr, data, sz);
}

void NotifyEvent_gdrom(u32, void*) {}

// Slow-update tier: the "drive clock". Completes the cart DMA in time.
void gdrom_CreditCycles(u32 cycles)
{
    if (!s_dmaPending)
        return;
    s_dmaCyclesLeft -= (s32)cycles;
    if (s_dmaCyclesLeft <= 0)
        cartDmaFinish();
}

void UpdateGDRom() {}

// No CD-DA on a NAOMI.
void FASTCALL gdrom_get_cdda(s16* sector)
{
    memset(sector, 0, 2352);
}
