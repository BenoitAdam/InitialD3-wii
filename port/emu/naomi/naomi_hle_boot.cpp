// BIOS-free NAOMI boot.
//
// The NAOMI BIOS, once the DIMM board has decrypted the game, copies the
// program segments listed in the game header into SH-4 RAM, leaves a copy of
// the header at 0x0C01F400 and jumps to the entry point. The DIMM board's
// own startup command leaves its configuration at 0x0C01FC00. This file
// does the same work directly, so no BIOS image is needed.
//
// It runs from dc_elf_pre_run_hook(), which dc.cpp calls right after every
// CPU reset (the hook nullDC4Wii uses for ELF boot).
#include "types.h"
#include "dc/mem/sh4_mem.h"
#include "dc/sh4/sh4_registers.h"
#include "naomi.h"
#include "naomi_elan.h"

#include <cstring>

static const u8* s_biosImage = nullptr;
static u32 s_biosSize = 0;

void naomi_SetBiosImage(const u8* data, u32 size)
{
    s_biosImage = data;
    s_biosSize = size;
}

static u32 loadLe32(const u8* p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// Copies `size` bytes of the decrypted image to an SH-4 address, through the
// core's block writer (which stores RAM in the host's word order).
static bool copyToGuest(u32 imageOffset, u32 address, u32 size)
{
    static u32 buffer[16 * 1024];
    for (u32 done = 0; done < size;)
    {
        u32 chunk = size - done;
        if (chunk > sizeof(buffer))
            chunk = sizeof(buffer);
        const u32 padded = (chunk + 3) & ~3u;
        memset(buffer, 0, padded);
        if (!naomi_CartRead(imageOffset + done, chunk, (u8*)buffer))
            return false;
        WriteMemBlock_nommu_ptr_le32(address + done, buffer, padded);
        done += chunk;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Exception dispatcher (replaces the BIOS's small kernel).
//
// The NAOMI BIOS sets VBR = 0x0C000000. Its exception entry points read the
// event code (EXPEVT / INTEVT), save the interrupted context and call
//     handler = *(u32*)(0x0C0001C0 + (code >> 3))
// as an ordinary C function, still in register bank 1 with SR.BL set. Games
// install their handlers by writing that table themselves (Initial D3 does
// `stc vbr` then stores at VBR+0x224, +0x22C, +0x240..0x248, +0x2A0..0x2AC:
// Holly IRLs, TMU0-2 and SCIF). The BIOS also multiplexes tasks there; a
// game only needs the dispatch, which is what this stub provides, written as
// SH-4 code so the interpreter and the Wii recompiler both run it unchanged.
// ---------------------------------------------------------------------------
namespace {

constexpr u32 kVbr = 0x0C000000;
constexpr u32 kHandlerTable = 0x0C0001C0;   // indexed by event code >> 3
constexpr u32 kHandlerTableEnd = 0x0C0002C0; // codes up to 0x7E0
constexpr u32 kCommonEntry = 0x0C000800;
constexpr u32 kDefaultHandler = 0x0C000C00;
constexpr u32 kIrqStackTop = 0x8C010000;     // 16 KB below, unused without a BIOS

// Tiny SH-4 code emitter with a literal pool placed after the code.
struct StubAsm
{
    u32 base;
    u16 code[512];
    u32 lits[32];
    u32 fixCode[32];
    u32 fixLit[32];
    u32 nCode = 0, nLits = 0, nFix = 0;

    explicit StubAsm(u32 at) : base(at) {}
    void op(u16 o) { code[nCode++] = o; }
    // mov.l @(disp,PC),Rn with `value` in the pool
    void movLit(u32 rn, u32 value)
    {
        fixCode[nFix] = nCode;
        fixLit[nFix++] = nLits;
        lits[nLits++] = value;
        op((u16)(0xD000 | (rn << 8)));
    }
    // Lays out the pool, patches displacements and writes it to guest RAM.
    u32 emit()
    {
        if (nCode & 1)
            op(0x0009);                       // pool must be 4-aligned
        const u32 pool = base + nCode * 2;
        for (u32 i = 0; i < nFix; i++)
        {
            const u32 pc = base + fixCode[i] * 2;
            const u32 lit = pool + fixLit[i] * 4;
            const u32 disp = (lit - ((pc & ~3u) + 4)) / 4;
            code[fixCode[i]] |= (u16)disp;    // < 256 for these small stubs
        }
        for (u32 i = 0; i < nCode; i++)
            WriteMem16_nommu(base + i * 2, code[i]);
        for (u32 i = 0; i < nLits; i++)
            WriteMem32_nommu(pool + i * 4, lits[i]);
        return pool + nLits * 4;
    }
};

// NAOMI games run in register bank 1 (SR.RB is never cleared after reset) -
// the same bank exceptions switch to - so nothing may be touched before it is
// saved: r0/r1 go on the interrupted stack first, exactly like the BIOS stubs.
void emitVectorEntry(u32 at, u32 eventRegister)
{
    StubAsm a(at);
    a.op(0x2F06);                 // mov.l r0,@-r15
    a.op(0x2F16);                 // mov.l r1,@-r15
    a.movLit(0, eventRegister);   // mov.l #EXPEVT/INTEVT,r0
    a.op(0x6102);                 // mov.l @r0,r1        r1 = event code
    a.movLit(0, kCommonEntry);    // mov.l #common,r0
    a.op(0x402B);                 // jmp @r0
    a.op(0x0009);                 // nop
    a.emit();
}

void emitCommonEntry()
{
    // In: SR.BL=1, r1 = event code, interrupted r0/r1 at @r15 (r1, then r0).
    StubAsm a(kCommonEntry);
    const u32 kNoPrSz = ~0x00180000u;     // FPSCR without PR (19) and SZ (20)
    const u32 kNoPrSzFr = ~0x00380000u;   // ... and without FR (21)

    a.op(0x60F3);                 // mov r15,r0
    a.movLit(15, kIrqStackTop);   // own stack
    a.op(0x2F06);                 // mov.l r0,@-r15      interrupted r15 (r0/r1 above it)
    for (u32 i = 2; i <= 7; i++)
        a.op((u16)(0x2F06 | (i << 4)));   // mov.l r_i,@-r15
    a.op(0x4F22);                 // sts.l pr,@-r15
    a.op(0x4F02);                 // sts.l mach,@-r15
    a.op(0x4F12);                 // sts.l macl,@-r15
    a.op(0x4F13);                 // stc.l gbr,@-r15
    a.op(0x4F33);                 // stc.l ssr,@-r15
    a.op(0x4F43);                 // stc.l spc,@-r15
    a.op(0x4F52);                 // sts.l fpul,@-r15
    a.op(0x4F62);                 // sts.l fpscr,@-r15
    a.op(0x006A);                 // sts fpscr,r0
    a.movLit(3, kNoPrSz);
    a.op(0x2039);                 // and r3,r0
    a.op(0x406A);                 // lds r0,fpscr        single moves, FR kept
    for (u32 i = 0; i < 16; i++)
        a.op((u16)(0xFF0B | (i << 4)));   // fmov.s fr_i,@-r15   current bank
    a.op(0xFBFD);                 // frchg
    for (u32 i = 0; i < 16; i++)
        a.op((u16)(0xFF0B | (i << 4)));   // fmov.s fr_i,@-r15   other bank
    a.op(0xFBFD);                 // frchg
    a.movLit(3, kNoPrSzFr);
    a.op(0x2039);                 // and r3,r0
    a.op(0x406A);                 // lds r0,fpscr        C default for the handler

    a.op(0x6013);                 // mov r1,r0
    a.op(0x4009);                 // shlr2 r0
    a.op(0x4001);                 // shlr r0             r0 = code >> 3
    a.movLit(3, kHandlerTable);
    a.op(0x033E);                 // mov.l @(r0,r3),r3   handler
    a.op(0x6433);                 // mov r3,r4           (the BIOS passes it in r4 too)
    a.op(0x430B);                 // jsr @r3
    a.op(0x0009);                 // nop

    a.op(0x61F3);                 // mov r15,r1
    a.op(0x7140);                 // add #64,r1
    a.op(0x7140);                 // add #64,r1          -> saved fpscr
    a.op(0x6012);                 // mov.l @r1,r0
    a.movLit(3, kNoPrSz);
    a.op(0x2039);                 // and r3,r0
    a.op(0x406A);                 // lds r0,fpscr        original FR again
    a.op(0xFBFD);                 // frchg               bank saved last
    for (int i = 15; i >= 0; i--)
        a.op((u16)(0xF0F9 | (i << 8)));   // fmov.s @r15+,fr_i
    a.op(0xFBFD);                 // frchg
    for (int i = 15; i >= 0; i--)
        a.op((u16)(0xF0F9 | (i << 8)));
    a.op(0x4F66);                 // lds.l @r15+,fpscr
    a.op(0x4F56);                 // lds.l @r15+,fpul
    a.op(0x4F47);                 // ldc.l @r15+,spc
    a.op(0x4F37);                 // ldc.l @r15+,ssr
    a.op(0x4F17);                 // ldc.l @r15+,gbr
    a.op(0x4F16);                 // lds.l @r15+,macl
    a.op(0x4F06);                 // lds.l @r15+,mach
    a.op(0x4F26);                 // lds.l @r15+,pr
    for (int i = 7; i >= 2; i--)
        a.op((u16)(0x60F6 | (i << 8)));   // mov.l @r15+,r_i
    a.op(0x60F6);                 // mov.l @r15+,r0      interrupted r15
    a.op(0x6F03);                 // mov r0,r15
    a.op(0x61F6);                 // mov.l @r15+,r1
    a.op(0x60F6);                 // mov.l @r15+,r0
    a.op(0x002B);                 // rte
    a.op(0x0009);                 // nop
    a.emit();
}

void installExceptionDispatcher()
{
    StubAsm def(kDefaultHandler);
    def.op(0x000B);               // rts
    def.op(0xE000);               // mov #0,r0
    def.emit();
    for (u32 a = kHandlerTable; a < kHandlerTableEnd; a += 4)
        WriteMem32_nommu(a, kDefaultHandler);
    emitVectorEntry(kVbr + 0x100, 0xFF000024);   // general exceptions: EXPEVT
    emitVectorEntry(kVbr + 0x400, 0xFF000024);   // TLB miss: EXPEVT
    emitVectorEntry(kVbr + 0x600, 0xFF000028);   // interrupts: INTEVT
    emitCommonEntry();
    vbr = kVbr;
}

} // namespace

bool naomi_BiosFreeBoot()
{
    alignas(4) u8 header[0x500];
    if (!naomi_CartRead(0, sizeof(header), header))
    {
        naomi_Log("[boot] cannot read the game header\n");
        return false;
    }
    if (memcmp(header, "NAOMI", 5) != 0 && memcmp(header, "Naomi2", 6) != 0)
    {
        naomi_Log("[boot] no NAOMI header in the game image\n");
        return false;
    }

    naomi_ElanReset();

    // 0. BIOS ROM area: the game copies the BIOS service library out of it.
    if (s_biosImage && bios_b.data)
    {
        const u32 n = s_biosSize < BIOS_STORE_SIZE ? s_biosSize : BIOS_STORE_SIZE;
        memcpy(bios_b.data, s_biosImage, n);
        naomi_Log("[boot] BIOS ROM image: %u KB (service library only, the BIOS itself is not run)\n", n / 1024);
    }
    else
    {
        naomi_Log("[boot] no BIOS ROM image: BIOS service calls will fail\n");
    }

    // 1. Program segments (gameLoad table at 0x360: offset, address, size).
    u32 total = 0;
    for (int i = 0; i < 8; i++)
    {
        const u8* e = header + 0x360 + i * 12;
        const u32 offset = loadLe32(e);
        const u32 address = loadLe32(e + 4);
        const u32 size = loadLe32(e + 8);
        if (offset == 0xffffffff)
            break;
        naomi_Log("[boot] segment %d: image %08X -> %08X, %u bytes\n", i, offset, address, size);
        if (!copyToGuest(offset & 0x1fffffff, address, size))
        {
            naomi_Log("[boot] read error in segment %d\n", i);
            return false;
        }
        total += size;
    }

    // 2. The BIOS leaves the header in system RAM for the game.
    WriteMemBlock_nommu_ptr_le32(0x0C01F400, (u32*)header, sizeof(header));

    // 3. DIMM board configuration, as its startup command writes it:
    //    bits 16-17 DIMM0 size code (1=128, 2=256, 3=512 MB), bit 29/30 set
    //    (GD-ROM connected), low bits = buffer offset in MB.
    u32 dimmBytes = 128u << 20;
    while (dimmBytes < naomi_CartSize() && dimmBytes < (512u << 20))
        dimmBytes <<= 1;
    const u32 sizeCode = dimmBytes == (512u << 20) ? 3 : dimmBytes == (256u << 20) ? 2 : 1;
    WriteMem32_nommu(0x0C01FC04, (sizeCode << 16) | 0x60000000 | (dimmBytes >> 20));
    WriteMem32_nommu(0x0C01FC0C, 0x01020264);   // DIMM firmware 1.02
    for (u32 a = 0x0C01FC40; a < 0x0C01FC50; a += 4)
        WriteMem32_nommu(a, 0x20202020);         // board serial: blank

    // 4. What the BIOS kernel provides at run time: VBR and the exception
    //    dispatch table games register their interrupt handlers in.
    installExceptionDispatcher();

    // 5. Start the game at its entry point (SR/FPSCR keep their reset values;
    //    the game sets its own stack and interrupt masks).
    const u32 entry = loadLe32(header + 0x420);
    next_pc = entry;
    naomi_Log("[boot] BIOS-free boot: %u bytes loaded, DIMM %u MB, entry %08X\n", total, dimmBytes >> 20, entry);
    return true;
}

extern "C" void dc_elf_pre_run_hook()
{
    naomi_BiosFreeBoot();
}
