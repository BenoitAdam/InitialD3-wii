#pragma once

/*
    wii_fastmem.h - PowerPC MMU: demand-paged guest memory + JIT fastmem

    See wii_fastmem.cpp. The guest memory regions live at fixed effective
    addresses, backed page by page on first touch:

        EA 0x0C000000-0x0FFFFFFF -> main RAM  (32 MB, mirrored once)
        EA 0x04000000-0x04FFFFFF -> VRAM      (16 MB, 64-bit path)
        EA 0x06000000-0x06FFFFFF -> VRAM      (mirror)
        EA 0x0A000000-0x0BFFFFFF -> ELAN RAM  (32 MB)
        EA 0x20000000-0x207FFFFF -> sound RAM (segment 2, not a guest EA)

    The dynarec's fastmem accesses use EA = sh4_addr & 0x1FFFFFFF.

    EA 0x00000000-0x03FFFFFF (the area-0 image) must NEVER be mapped, no
    matter how tempting (AICA wave RAM lives there): store-queue accesses
    0xE0000000-0xE3FFFFFF mask onto exactly this range, and only the
    fault path can disambiguate them (it dispatches on the ORIGINAL
    address register). Mapping any of it makes SQ stores with offset bits
    >= the mapped base silently write host memory instead of the SQ
    buffer - upper-half-RAM SQ memcpy, TA YUV FIFO and AICA SQ uploads
    all corrupt. This shipped once in nullDC4Wii (v1) and broke half the
    library. Wave RAM is served by the trampolines instead, which is also
    why the sound RAM window lives in segment 2.

    Everything else (MMIO, BIOS/flash, wave RAM, TA FIFO, VRAM 32-bit
    path, P4/SQ after masking) is left unmapped: a JIT load/store there
    takes a DSI and the handler BACK-PATCHES the faulting site with a
    branch to a generated slow-path trampoline (rec_fastmem_* in
    wii_driver.cpp), then retries.
*/

#include "types.h"

#define WII_GUEST_RAM_EA          0x0C000000u
#define WII_GUEST_VRAM_EA         0x04000000u
#define WII_GUEST_VRAM_MIRROR_EA  0x06000000u
#define WII_GUEST_ELAN_EA         0x0A000000u
#define WII_GUEST_ARAM_EA         0x20000000u

#ifdef __cplusplus
extern "C" {
#endif

// 1 once the dynarec may emit direct (fastmem) guest accesses.
extern int g_wii_fastmem_active;

// Builds the page table and the page pool (every arena byte above the given
// headroom, which stays for the C heap), installs the DSI handler and turns
// translation on. Returns 0 on failure (the caller must stop the launch).
int WiiGuestMem_Init(u32 mem1_keep, u32 mem2_keep);

typedef struct
{
    u32 pool_kb, free_kb;
    u32 ram_kb, vram_kb, elan_kb, aram_kb;
    u32 reclaims, reclaimed_kb;   // emergency sweeps in the fault handler, KB they freed
    u32 alloc_fails, last_fail_ea; // stores that found the pool empty
    u32 comp_kb, store_kb;        // guest KB held compressed, and the frames holding it
    u32 compressions, decompressions, corrupt;
    u32 dsi, page_faults;         // DSI exceptions taken, of which demand-paging faults
} WiiGuestMemStats;
void WiiGuestMem_Stats(WiiGuestMemStats* out);

// Once per emulated vblank: compresses a few cold pages when the pool runs
// low, so the fault handler rarely has to sweep synchronously.
void WiiGuestMem_Tick(void);

// GPU reads of ELAN RAM (naomi_elan_gx.cpp): the GPU fetches vertex records
// straight from the page frames.
// Ready: the paged memory is up and stores can be detected (PTE change bit).
int WiiGuestMem_ElanGpuReady(void);
// ELAN RAM page `page` (offset >> 12): its frame's physical address, or 0 if
// it is not resident (never written, or held compressed). *written = 1 when
// the page was (or may have been) written since the previous call for it;
// the CPU cache's copy of it has then been written back to memory. Keeps the
// pager's sweep off the page for a few passes.
u32 WiiGuestMem_ElanGpuPage(u32 page, int* written);

// Turns on fastmem emission when the preset asks for it (called at
// ngen_mainloop entry, after WiiGuestMem_Init).
void WiiFastmem_Init(void);

// Implemented in wii_driver.cpp: decode the faulting fastmem site at
// `pc` (one of the fixed shapes emitted by shop_readm/shop_writem),
// build a slow-path trampoline in the trampoline pool and patch the
// site with a branch to it. Returns 0 on success, nonzero if `pc` is
// not a recognizable fastmem site (the DSI handler then panics via
// libogc's default handler).
int rec_fastmem_patch(unsigned int pc);

#ifdef __cplusplus
}
#endif
