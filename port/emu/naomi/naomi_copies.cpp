// GAME COPIES (launcher RECOMPILER 2): two of the game's bulk copy routines
// done natively, called at their entry by the recompiler (wii_driver.cpp
// ngen_Compile) and by the Windows runner (--game-copies).
//
// Real Wii log 32 (PC SAMPLER, night Akina, chase view): the word loop of the
// game's copy routine (0C20A2A0) took 2.5% of the time and its list copy loop
// (0C1D4F4C) 1.9%, plus their fastmem trampolines (2.8% in all): on a Wii each
// recompiled iteration pays a block entry, a timeslice test and a trampoline
// for a store the window does not map. In the chase view (runner trace) the
// first copies 32 or 64 bytes at a time from main RAM into the store queues,
// ~7000 words a frame on the Wii; the second copies display lists from main
// RAM into ELAN RAM, 32 bytes a burst.
//
// - 0C20A280, copy(dst r4, src r5, bytes r6): all three 8-aligned, an fmov.d
//   pair loop (left to the game: it needs the FPU registers); otherwise a word
//   loop, done here (into the store queues, plain memory or 32-bit VRAM). It
//   leaves r0 = the last word, r4/r5 past the copy, r6 = 0, T = 1.
// - 0C1D4EA0, a display list's first 96 bytes (src r4, dst r5): three 32-byte
//   bursts through the store queues to dst (QACR0/1 = dst >> 24, so they land
//   at dst & 0x1FFFFFE0), the word at dst + 60 read before and written back
//   after (the list's word 15 keeps its old value). It leaves r1 = the last
//   burst's queue address, r2 past it, r3 = 0, r4 = src + 96, r7 = that word,
//   T = 0, the store queues holding the last two bursts, and the old r14 below
//   r15 (pushed and popped).
// - 0C1D4F20, the rest of it (src r4, dst r5, called right after): the length
//   is the word at src + 24, bursts to dst + 96 on (QACR0/1 = (dst + 96) >> 24).
//   It leaves r1 = the last burst's queue address, r2 = 0, r3/r5 past the copy,
//   T = 0, the queues and the stack as above.
//
// What either cannot do exactly (other memory areas, misaligned addresses, the
// TA, lengths out of the ordinary) goes back to the game's code. The routines
// are fingerprinted (their code and literal pool): other code is never
// touched. Cycles: the recompiler charges an SH-4 instruction a cycle, so the
// copy returns what the game's blocks would have charged after the entry block.
#include "types.h"
#include "dc/mem/sh4_mem.h"
#include "dc/mem/_vmem.h"
#include "dc/mem/sh4_internal_reg.h"
#include "dc/pvr/pvr_if.h"
#include "dc/sh4/ccn.h"
#include "naomi.h"

#include <string.h>

extern "C" {
int g_game_copies_preset = 1;   // launch page GAME COPIES: 1 = NATIVE, 0 = the game's loops (OLD)
alignas(32) u32 g_game_copy_regs[8];   // aligned: the recompiler reads them at lo + 0..28
}

namespace {

const u32 kWordsStart = 0x0C20A280;
const u16 kWordsCode[] = {0x6043, 0x205B, 0x206B, 0xC807, 0x8B09, 0xF3FD, 0x4609, 0x4601, 0xF059, 0x4610, 0xF40A, 0x8FFB,
                          0x7408, 0x000B, 0xF3FD, 0x4609, 0x6056, 0x4610, 0x2402, 0x8FFB, 0x7404, 0x000B, 0x0009};

const u32 kHeadStart = 0x0C1D4EA0;
const u16 kHeadCode[] = {
    0x2FE6, 0x6EF3, 0xD219, 0x6153, 0x4129, 0x4119, 0x2212, 0x1211, 0xD117, 0x6253, 0x2219, 0xD117, 0x221B,
    0x575F, 0xE303, 0x6143, 0x7140, 0x0183,
    0x6146, 0x2212, 0x7204, 0x6146, 0x2212, 0x7204, 0x6146, 0x2212, 0x7204, 0x6146, 0x2212, 0x7204,
    0x6146, 0x2212, 0x7204, 0x6146, 0x2212, 0x7204, 0x6146, 0x2212, 0x7204, 0x6146, 0x2212, 0x7204,
    0x6123, 0x71E0, 0x0183, 0x73FF, 0x4315, 0x89DE, 0x157F, 0x6FE3, 0x6EF6, 0x000B, 0x0009};
const u32 kHeadPool = 0x0C1D4F0C;

const u32 kListStart = 0x0C1D4F20;
const u16 kListCode[] = {
    0x2FE6, 0x6EF3, 0xD21C, 0x6343, 0x7360, 0x7560, 0x6153, 0x4129, 0x4119, 0x2212, 0x1211, 0xD119, 0x2519,
    0xD119, 0x251B, 0x5146, 0x71BF, 0xE2FB, 0x412D, 0x4115, 0x8B21, 0x6213, 0x6133, 0x7140, 0x0183,
    0x6136, 0x2512, 0x7504, 0x6136, 0x2512, 0x7504, 0x6136, 0x2512, 0x7504, 0x6136, 0x2512, 0x7504,
    0x6136, 0x2512, 0x7504, 0x6136, 0x2512, 0x7504, 0x6136, 0x2512, 0x7504, 0x6136, 0x2512, 0x7504,
    0x6153, 0x71E0, 0x0183, 0x72FF, 0x4215, 0x89DE, 0x6FE3, 0x6EF6, 0x000B, 0x0009};
// The literal pools: QACR0's address, the queue address mask, the queue area.
const u32 kListPool = 0x0C1D4F98;
const u32 kListPoolWords[] = {0xFF000038, 0x03FFFFFF, 0xE0000000};

// SH-4 instructions of the game's blocks (a cycle each), beyond the entry
// block the recompiler has charged already:
//   word copy:  entry 5, 0C20A29E 6 (first word), loop 5 a word, return 2
//   list copy:  entry 21, 0C1D4F4A 34 (first burst), loop 33 a burst, return 4
//   its head:   entry 48 (to the first burst's bt), loop 33 a burst, return 5
const u32 kWordsCycles = 5, kWordsFixed = 6 - 5 + 2;
const u32 kListCycles = 33, kListFixed = 34 - 33 + 4;
const u32 kHeadCycles = 2 * 33 + 5;

const u32 kMaxWords = 1u << 20;     // 4 MB: larger is not a copy this game makes
const u32 kMaxBursts = 1u << 15;    // 1 MB

bool codeMatches(u32 start, const u16* code, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (ReadMem16(start + i * 2) != code[i])
            return false;
    return true;
}

bool poolMatches(u32 pool)
{
    for (u32 i = 0; i < 3; i++)
        if (ReadMem32(pool + i * 4) != kListPoolWords[i])
            return false;
    return true;
}

// The 32-bit VRAM interface (area 1, 0x05 and 0x07): its two banks interleave
// every word in VRAM (pvr_write_area1_32).
inline bool vram32(u32 a)
{
    return ((a >> 24) & 0x1D) == 0x05;
}

inline u32 vram32Offset(u32 a)
{
    return ((a & 0x3FFFFC) << 1) | ((a >> 20) & 4);
}

// Host pointer to n bytes of guest memory a plain store reaches, or null.
inline u8* direct(u32 a, u32 n)
{
    return _vmem_GetBlockPtr(a & 0x1FFFFFFF, n);
}

// The store queues (0xE0000000-0xE3FFFFFF): a store writes sq_both[addr & 0x3C]
// (both queues, 64 bytes; the 32-byte burst goes out at the caller's pref).
inline bool storeQueue(u32 a)
{
    return (a >> 26) == 0x38;
}

// Where the word copy writes: d (a plain store), else the store queues or the
// 32-bit VRAM interface (dst says which). False: the game's code runs.
bool wordsPlan(u32 dst, u32 src, u32 len, const u32*& s, u32*& d)
{
    const u32 n = len >> 2;
    if (!((dst | src | len) & 7) || ((dst | src) & 3) || n == 0 || n > kMaxWords)
        return false;
    s = (const u32*)direct(src, n * 4);
    if (!s)
        return false;
    // Race frames (runner trace, chase view): 32 or 64 bytes from main RAM into
    // the store queues, ~700 calls a frame; within the queues' area.
    d = nullptr;
    if (storeQueue(dst))
        return (dst & 0x03FFFFFF) + n * 4 <= 0x04000000;
    d = (u32*)direct(dst, n * 4);
    if (d)
        return true;
    // VRAM through the 32-bit interface: within one 4 MB bank (the bank bit is
    // address bit 22, the copy's words step 8 bytes apart in VRAM).
    const u32 a = dst & 0x1FFFFFFF;
    return vram32(a) && vram.data && (a & 0x3FFFFF) + n * 4 <= 0x400000;
}

struct ListPlan
{
    const u32* s;
    u8* d;
    u32* stack;
    u32 bursts, queue, qacr;
};

// Bursts from src to queue address p.queue on, landing at p.d; the stack slot
// the pushed r14 goes to.
bool burstPlan(u32 src, u32 a, u32 bursts, u32 r15, ListPlan& p)
{
    p.bursts = bursts;
    if (p.bursts > kMaxBursts || (a & 31) || (a & 0x03FFFFFF) + p.bursts * 32 > 0x04000000)
        return false;
    p.queue = (a & 0x03FFFFFF) | 0xE0000000;
    p.qacr = a >> 24;
    const u32 dest = a & 0x1FFFFFE0;      // QACR's area bits are a's bits 28:26
    if (((dest >> 26) & 7) == 4)          // the TA: its own path (do_sqw)
        return false;
    p.s = (const u32*)direct(src, p.bursts * 32);
    p.d = direct(dest, p.bursts * 32);
    p.stack = (u32*)direct(r15 - 4, 4);
    return p.s && p.d && p.stack && !((r15 - 4) & 3);
}

// The bursts, a whole one read before it is written, as the queues do; the
// queues keep the last two.
void runBursts(const ListPlan& p, u32 r14)
{
    *p.stack = r14;                             // mov.l r14,@-r15 (popped again)
    CCN_QACR0.reg_data = p.qacr;
    CCN_QACR1.reg_data = p.qacr;
    u32 burst[8];
    for (u32 b = 0; b < p.bursts; b++)
    {
        memcpy(burst, p.s + b * 8, 32);
        memcpy(p.d + b * 32, burst, 32);
        if (b + 2 >= p.bursts)
            memcpy(&sq_both[(p.queue + b * 32) & 0x20], burst, 32);
    }
}

bool headPlan(u32 src, u32 dst, u32 r15, ListPlan& p)
{
    return !(src & 3) && burstPlan(src, dst, 3, r15, p);
}

bool listPlan(u32 src, u32 dst, u32 r15, ListPlan& p)
{
    if (src & 3)
        return false;
    const u8* head = direct(src, 28);
    if (!head)
        return false;
    const u32 len = *(const u32*)(head + 24);
    // (len - 65) >> 5 bursts; under 97 none (left to the game, it is a few
    // instructions), under 65 the game's shift would make it ~134M.
    if (len < 97)
        return false;
    return burstPlan(src + 96, dst + 96, (len - 65) >> 5, r15, p);
}

} // namespace

extern "C" u32 naomi_GameCopyEntry(u32 pc)
{
    if (!g_game_copies_preset)
        return kGameCopyNone;
    const u32 p = pc & 0x1FFFFFFF;
    if (p == kWordsStart && codeMatches(kWordsStart, kWordsCode, sizeof(kWordsCode) / 2))
        return kGameCopyWords;
    if (p == kListStart && codeMatches(kListStart, kListCode, sizeof(kListCode) / 2) && poolMatches(kListPool))
        return kGameCopyList;
    if (p == kHeadStart && codeMatches(kHeadStart, kHeadCode, sizeof(kHeadCode) / 2) && poolMatches(kHeadPool))
        return kGameCopyHead;
    return kGameCopyNone;
}

extern "C" u32 naomi_GameCopyWould(u32 kind, u32 r4, u32 r5, u32 r6, u32 r15)
{
    const u32* s;
    u32* d;
    ListPlan p;
    switch (kind)
    {
    case kGameCopyWords: return wordsPlan(r4, r5, r6, s, d);
    case kGameCopyList:  return listPlan(r4, r5, r15, p);
    case kGameCopyHead:  return headPlan(r4, r5, r15, p);
    default:             return 0;
    }
}

extern "C" u32 naomi_GameCopyWords(u32 dst, u32 src, u32 len)
{
    const u32* s;
    u32* d;
    if (!wordsPlan(dst, src, len, s, d))
        return 0;
    const u32 n = len >> 2;
    // Word by word, forward, as the game's loop (an overlap copies the same way).
    u32 w = 0;
    if (d)
        for (u32 i = 0; i < n; i++)
        {
            w = s[i];
            d[i] = w;
        }
    else if (storeQueue(dst))
        for (u32 i = 0; i < n; i++)
        {
            w = s[i];
            *(u32*)&sq_both[(dst + i * 4) & 0x3C] = w;
        }
    else
    {
        u8* const v = vram.data;
        const u32 a = dst & 0x1FFFFFFF;
        for (u32 i = 0; i < n; i++)
        {
            w = s[i];
            *(u32*)(v + vram32Offset(a + i * 4)) = w;
        }
    }
    g_game_copy_regs[0] = w;
    g_game_copy_regs[1] = dst + n * 4;
    g_game_copy_regs[2] = src + n * 4;
    g_game_copy_regs[3] = 0;
    return n * kWordsCycles + kWordsFixed;
}

extern "C" u32 naomi_GameCopyList(u32 src, u32 dst, u32 r14, u32 r15)
{
    ListPlan p;
    if (!listPlan(src, dst, r15, p))
        return 0;
    runBursts(p, r14);
    const u32 end = p.queue + p.bursts * 32;
    g_game_copy_regs[0] = end - 32;             // r1
    g_game_copy_regs[1] = 0;                    // r2
    g_game_copy_regs[2] = src + 96 + p.bursts * 32;   // r3
    g_game_copy_regs[3] = end;                  // r5
    return p.bursts * kListCycles + kListFixed;
}

extern "C" u32 naomi_GameCopyHead(u32 src, u32 dst, u32 r14, u32 r15)
{
    ListPlan p;
    if (!headPlan(src, dst, r15, p))
        return 0;
    u32* const word15 = (u32*)(p.d + 60);
    const u32 old = *word15;                    // mov.l @(60,r5),r7 before the bursts
    runBursts(p, r14);
    *word15 = old;                              // mov.l r7,@(60,r5) after them
    g_game_copy_regs[0] = p.queue + 64;         // r1
    g_game_copy_regs[1] = p.queue + 96;         // r2
    g_game_copy_regs[2] = 0;                    // r3
    g_game_copy_regs[3] = src + 96;             // r4
    g_game_copy_regs[4] = old;                  // r7
    return kHeadCycles;
}
