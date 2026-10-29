// Windows-only implementations of what nullDC4Wii's Wii front end and Wii
// back ends provide (os_* hooks, audio output, recompiler call-outs,
// diagnostics that live in gxRend.cpp / wii_driver.cpp).
#include "types.h"

#if HOST_OS != OS_WII

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "dc/sh4/rec_v2/ngen.h"
#include "naomi/naomi.h"
#include "host_hooks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- os_base.h -------------------------------------------------------------
int os_msgbox(const char* text, unsigned int)
{
    naomi_Log("[msgbox] %s\n", text);
    return 0;
}

double os_GetSeconds()
{
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// ---- Guest memory (write-watched) ------------------------------------------
namespace {
struct GuestBlock
{
    void* base;
    u32 size;
    u8* taken;          // per page: written, and reset by host_GuestTakeWritten
    u32 takenCount;
};
GuestBlock s_guestBlocks[8];
u32 s_guestBlockCount;

const GuestBlock* findGuestBlock(const void* p)
{
    for (u32 i = 0; i < s_guestBlockCount; i++)
        if (p >= s_guestBlocks[i].base && (const u8*)p < (const u8*)s_guestBlocks[i].base + s_guestBlocks[i].size)
            return &s_guestBlocks[i];
    return nullptr;
}
} // namespace

void* host_GuestAlloc(u32 size)
{
    // VirtualAlloc returns zeroed, 64 KB aligned memory.
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p && s_guestBlockCount < 8)
        s_guestBlocks[s_guestBlockCount++] = {p, size, nullptr, 0};
    return p;
}

// GetWriteWatch fills an address array: 4 MB batches (1024 pages at most).
static void* s_watchAddresses[1024];

u32 host_GuestWrittenPages(const void* base, u32 size)
{
    const GuestBlock* b = findGuestBlock(base);
    u32 total = b ? b->takenCount : 0;
    for (u32 off = 0; off < size; off += 4u << 20)
    {
        ULONG_PTR count = 1024;
        DWORD granularity = 0;
        const u32 chunk = size - off < (4u << 20) ? size - off : (4u << 20);
        if (GetWriteWatch(0, (u8*)base + off, chunk, s_watchAddresses, &count, &granularity) != 0)
            continue;
        for (ULONG_PTR i = 0; i < count; i++)
            if (!b || !b->taken || !b->taken[((u8*)s_watchAddresses[i] - (u8*)b->base) >> 12])
                total++;
    }
    return total;
}

u32 host_GuestTakeWritten(const void* base, u32 size, u32* pages, u32 cap)
{
    GuestBlock* b = (GuestBlock*)findGuestBlock(base);
    if (!b || base != b->base || size > b->size)
        return 0;
    if (!b->taken)
        b->taken = (u8*)calloc(b->size >> 12, 1);
    u32 n = 0;
    for (u32 off = 0; off < size; off += 4u << 20)
    {
        ULONG_PTR count = 1024;
        DWORD granularity = 0;
        const u32 chunk = size - off < (4u << 20) ? size - off : (4u << 20);
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (u8*)base + off, chunk, s_watchAddresses, &count, &granularity) != 0)
            continue;
        for (ULONG_PTR i = 0; i < count; i++)
        {
            const u32 page = (u32)(((u8*)s_watchAddresses[i] - (u8*)base) >> 12);
            if (b->taken && !b->taken[page])
            {
                b->taken[page] = 1;
                b->takenCount++;
            }
            if (n < cap)
                pages[n++] = page;
        }
    }
    return n;
}

bool guestmem_Discard(void* data, u32 size)
{
    const GuestBlock* b = findGuestBlock(data);
    if (!b)
        return false;
    memset(data, 0, size);
    ResetWriteWatch(data, size);
    GuestBlock* w = (GuestBlock*)b;
    if (w->taken)
        memset(w->taken, 0, w->size >> 12);
    w->takenCount = 0;
    return true;
}

// ---- Audio output (nullAICA's wii_audiostream / wii_audio) ----------------
// Silent for now: the samples are produced and dropped.
void wii_InitAudio() {}
void wii_TermAudio() {}
void wii_WriteSample(s16, s16) {}
extern "C" void wii_audio_aica_ready() {}

// ---- Recompiler call-outs (rec_v2 is compiled out: HOST_NO_REC) ----------
void ngen_CC_Start(shil_opcode*) {}
void ngen_CC_Param(shil_opcode*, shil_param*, CanonicalParamType) {}
void ngen_CC_Call(shil_opcode*, void*) {}
void ngen_CC_Finish(shil_opcode*) {}

// ---- Wii-side diagnostics referenced by SPG.cpp ---------------------------
void strip_dedup_dump(double) {}
extern "C" void ifb_probe_dump(double) {}
extern "C" void hotblocks_dump(double) {}

// Bring-up statistics, indexed by SH-4 memory area (address >> 26 & 7).
u32 g_dbg_ch2_count[8], g_dbg_ch2_bytes[8];
u32 g_dbg_sqw_count[8];

#endif // HOST_OS != OS_WII
