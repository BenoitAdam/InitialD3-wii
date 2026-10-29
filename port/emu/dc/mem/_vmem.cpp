// _vmem.cpp – virtual memory subsystem for Dreamcast-on-Wii emulation
//
// _vmem v2 layer overview:
//   _vmem_MemInfo_ptr[256]  – top-level dispatch table (indexed by addr[31:24])
//   _vmem_RF8/16/32         – per-handler read  function tables
//   _vmem_WF8/16/32         – per-handler write function tables
//
// Each _vmem_MemInfo_ptr entry is either:
//   • A host pointer (≥256) with low bits encoding an address-wrap shift, OR
//   • A small integer (< HANDLER_COUNT*4) encoding a handler-table index.
// ---------------------------------------------------------------------------

#include "_vmem.h"
#include "dc/aica/aica_if.h"
#include "dc/pvr/pvr_if.h"
#include "sh4_mem.h"
#include "dc/sh4/rec_v2/ccall_census.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define HANDLER_MAX   0x1F
#define HANDLER_COUNT (HANDLER_MAX + 1)

// Safe sentinel returned on unmapped reads.
// NOTE: Using 0 here instead of 0xDEADC0D3 prevents the game from treating
// the error value as a valid pointer and cascading into garbage writes.
// The old value (0xDEADC0D3) was the root cause of the 0xDEADE5F3 freeze:
// the game read from an unmapped region, got 0xDEADC0D3 back, added an offset,
// and started writing to 0xDEADE5F3+ — corrupting state and hanging.
#define MEM_ERROR_RETURN_VALUE 0u

// ---------------------------------------------------------------------------
// Global tables
// ---------------------------------------------------------------------------
static _vmem_handler     _vmem_lrp;   // next free handler slot

static _vmem_ReadMem8FP*  _vmem_RF8  [HANDLER_COUNT];
static _vmem_WriteMem8FP* _vmem_WF8  [HANDLER_COUNT];

static _vmem_ReadMem16FP*  _vmem_RF16[HANDLER_COUNT];
static _vmem_WriteMem16FP* _vmem_WF16[HANDLER_COUNT];

static _vmem_ReadMem32FP*  _vmem_RF32[HANDLER_COUNT];
static _vmem_WriteMem32FP* _vmem_WF32[HANDLER_COUNT];

// Top-level dispatch table: 256 entries, one per 16 MB page.
#if HOST_OS == OS_WII
ALIGN(65536) void* _vmem_MemInfo_ptr[0x100];
#else
ALIGN(4096) void* _vmem_MemInfo_ptr[0x100];   // MSVC caps __declspec(align) at 8192
#endif

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Return the number of low address bits to mask away for a given memory block.
// e.g. a 16 MB block (mask=0x00FFFFFF) → returns 0 (no masking needed for
// a full 16 MB page), a 2 MB VRAM block → returns the appropriate shift.
static u32 FindMask(u32 msk)
{
    u32 s  = (u32)-1;
    u32 rv = 0;
    while (msk != s >> rv)
        rv++;
    return rv;
}

// Core read dispatcher (templated on access size).
template<typename T>
static INLINE T fastcall _vmem_readt(u32 addr)
{
    const u32 sz   = sizeof(T);
    const u32 page = addr >> 24;
    const unat iirf = (unat)_vmem_MemInfo_ptr[page];
    void* const ptr = (void*)(iirf & ~(unat)HANDLER_MAX);

    if (ptr == 0)
    {
        // MMIO / handler path
        const u32 id = (u32)iirf;
        if (sz == 1)
            return (T)_vmem_RF8[id / 4](addr);
        else if (sz == 2)
            return (T)_vmem_RF16[id / 4](addr);
        else if (sz == 4)
            return (T)_vmem_RF32[id / 4](addr);
        else if (sz == 8)
        {
            // 64-bit: two consecutive 32-bit reads. The word at addr is the
            // half the direct path below puts there: the low one on a
            // little-endian host, the high one on a big-endian host (the
            // SH-4 callers pass fr[n]:fr[n+1] as one u64, fr[n] at addr).
#if HOST_ENDIAN == ENDIAN_BIG
            T rv = (T)((u64)_vmem_RF32[id / 4](addr) << 32);
            rv  |= (T)_vmem_RF32[id / 4](addr + 4);
#else
            T rv = (T)_vmem_RF32[id / 4](addr);
            rv  |= (T)((u64)_vmem_RF32[id / 4](addr + 4) << 32);
#endif
            return rv;
        }
        die("_vmem_readt: invalid size");
    }
    else
    {
        // Direct RAM/VRAM path
        u32 shift = (u32)(iirf & HANDLER_MAX);
        addr <<= shift;
        addr >>= shift;
#if HOST_ENDIAN == ENDIAN_BIG
        if (sz < 4)
            addr ^= 4 - sz;
#endif
        return *((T*)&((u8*)ptr)[addr]);
    }
}

// Core write dispatcher (templated on access size).
template<typename T>
static INLINE void fastcall _vmem_writet(u32 addr, T data)
{
    const u32 sz   = sizeof(T);
    const u32 page = addr >> 24;
    const unat iirf = (unat)_vmem_MemInfo_ptr[page];
    void* const ptr = (void*)(iirf & ~(unat)HANDLER_MAX);

    if (ptr == 0)
    {
        // MMIO / handler path
        const u32 id = (u32)iirf;
        if (sz == 1)
            _vmem_WF8[id / 4](addr, (u8)data);
        else if (sz == 2)
            _vmem_WF16[id / 4](addr, (u16)data);
        else if (sz == 4)
            _vmem_WF32[id / 4](addr, (u32)data);
        else if (sz == 8)
        {
            // Same word order as the direct path (see _vmem_readt): on the
            // Wii the high half goes to addr. It used to be swapped there, so
            // every fmov.d to a handler region (VRAM's 32-bit interface...)
            // landed with its two words exchanged.
#if HOST_ENDIAN == ENDIAN_BIG
            _vmem_WF32[id / 4](addr,     (u32)((u64)data >> 32));
            _vmem_WF32[id / 4](addr + 4, (u32)(u64)data);
#else
            _vmem_WF32[id / 4](addr,     (u32)(u64)data);
            _vmem_WF32[id / 4](addr + 4, (u32)((u64)data >> 32));
#endif
        }
        else
            die("_vmem_writet: invalid size");
    }
    else
    {
        // Direct RAM/VRAM path
        u32 shift = (u32)(iirf & HANDLER_MAX);
        addr <<= shift;
        addr >>= shift;
#if HOST_ENDIAN == ENDIAN_BIG
        if (sz < 4)
            addr ^= 4 - sz;
#endif
        *((T*)&((u8*)ptr)[addr]) = data;
    }
}

// ---------------------------------------------------------------------------
// Bulk block-transfer helper.
//
// The block helpers in sh4_mem.cpp used to run the dispatcher once per 32-bit
// word -- an indirect call, a table lookup and two shifts for every 4 bytes.
// That is not a rare path: do_sqw() pushes every non-TA store-queue flush
// through WriteMemBlock_nommu_ptr(.., 32), i.e. EIGHT dispatcher calls per
// flush, and PVR/ch2 DMA does one per word of the entire transfer. The [CC]
// census caught the result on Crazy Taxi -- 1,104,636 system-RAM writes/s, of
// which 95.6% arrived from plain C code rather than from JIT code (the
// from:tramp vs from:other split). It was never a dynarec problem.
//
// This resolves the whole range to a host pointer ONCE so the caller can do a
// single memmove. It returns 0 unless every byte of [Address, Address+size)
// lands in a single direct-mapped region, which is what keeps it safe:
//
//   * MMIO (ptr == 0) keeps the per-word path -- those writes have side
//     effects and each has to reach its handler
//   * the region mirror mask (the addr<<shift>>shift in _vmem_writet) must not
//     wrap inside the range: a bulk copy would run off the end of the region
//     where the per-word loop would have wrapped back to its start
//   * the range must not cross a 16 MB dispatch page into a different region
//
// Sub-word accesses are excluded by the alignment test, which matters: on a
// big-endian host _vmem_writet applies an address swizzle (addr ^= 4-sz) for
// those, while 32-bit words are stored natively. That is precisely why a bulk
// copy of whole words reproduces the per-word loop byte for byte.
// ---------------------------------------------------------------------------
u8* fastcall _vmem_GetBlockPtr(u32 Address, u32 size)
{
    if (size == 0 || (size & 3) != 0 || (Address & 3) != 0)
        return 0;

    const u32   page = Address >> 24;
    const unat  iirf = (unat)_vmem_MemInfo_ptr[page];
    void* const ptr  = (void*)(iirf & ~(unat)HANDLER_MAX);

    if (ptr == 0)
        return 0;                       // MMIO / handler region

    const u32 last = Address + size - 1;
    if ((last >> 24) != page)
        return 0;                       // crosses into another region

    const u32 shift = (u32)(iirf & HANDLER_MAX);
    const u32 base  = (Address << shift) >> shift;
    if (((last << shift) >> shift) != base + size - 1)
        return 0;                       // mirror wrap inside the range

    return &((u8*)ptr)[base];
}

// ---------------------------------------------------------------------------
// Public read/write accessors
// ---------------------------------------------------------------------------
u8  fastcall _vmem_ReadMem8  (u32 addr) { CCALL_MEM(CC_READMEM, 0, addr); return _vmem_readt<u8> (addr); }
u16 fastcall _vmem_ReadMem16 (u32 addr) { CCALL_MEM(CC_READMEM, 0, addr); return _vmem_readt<u16>(addr); }
u32 fastcall _vmem_ReadMem32 (u32 addr) { CCALL_MEM(CC_READMEM, 0, addr); return _vmem_readt<u32>(addr); }
u64 fastcall _vmem_ReadMem64 (u32 addr) { CCALL_MEM(CC_READMEM, 0, addr); return _vmem_readt<u64>(addr); }

// Windows runner (--area1-census): who writes VRAM through its 32-bit
// interface (area 1), and how wide - the Wii JIT inlines some of those
// stores and has to know which shapes the game uses.
#if HOST_OS != OS_WII
void (*g_area1_write_hook)(u32 addr, u32 size) = nullptr;
#define AREA1_NOTE(a, s) \
    do { if (g_area1_write_hook && (((a) >> 26) & 7) == 1) g_area1_write_hook(a, s); } while (0)
#else
#define AREA1_NOTE(a, s) do { } while (0)
#endif

void fastcall _vmem_WriteMem8  (u32 addr, u8  data) { CCALL_MEM(CC_WRITEMEM, 1, addr); CCALL_A1(1, addr); AREA1_NOTE(addr, 1); _vmem_writet<u8> (addr, data); }
void fastcall _vmem_WriteMem16 (u32 addr, u16 data) { CCALL_MEM(CC_WRITEMEM, 1, addr); CCALL_A1(2, addr); AREA1_NOTE(addr, 2); _vmem_writet<u16>(addr, data); }
void fastcall _vmem_WriteMem32 (u32 addr, u32 data) { CCALL_MEM(CC_WRITEMEM, 1, addr); CCALL_A1(4, addr); AREA1_NOTE(addr, 4); _vmem_writet<u32>(addr, data); }
void fastcall _vmem_WriteMem64 (u32 addr, u64 data) { CCALL_MEM(CC_WRITEMEM, 1, addr); CCALL_A1(8, addr); AREA1_NOTE(addr, 8); _vmem_writet<u64>(addr, data); }

// ---------------------------------------------------------------------------
// Default "not mapped" handlers
// These log once per unique address (rate-limited) to avoid log spam during
// level transitions, then return the safe sentinel value.
// ---------------------------------------------------------------------------

// Simple rate-limiter: skip logging if we've already seen this page recently.
static u32 s_last_unmapped_page = 0xFFFFFFFFu;
static u32 s_unmapped_log_count = 0;
static const u32 MAX_LOG_PER_PAGE = 8;

static bool should_log_unmapped(u32 addr)
{
    u32 page = addr >> 12; // 4 KB granularity for rate limiting
    if (page != s_last_unmapped_page)
    {
        s_last_unmapped_page  = page;
        s_unmapped_log_count  = 0;
    }
    if (s_unmapped_log_count < MAX_LOG_PER_PAGE)
    {
        s_unmapped_log_count++;
        return true;
    }
    return false;
}

u8 fastcall _vmem_ReadMem8_not_mapped(u32 addr)
{
    if (should_log_unmapped(addr))
        printf("[vmem] Read8  from unmapped 0x%08X\n", addr);
    return (u8)MEM_ERROR_RETURN_VALUE;
}
u16 fastcall _vmem_ReadMem16_not_mapped(u32 addr)
{
    if (should_log_unmapped(addr))
        printf("[vmem] Read16 from unmapped 0x%08X\n", addr);
    return (u16)MEM_ERROR_RETURN_VALUE;
}
u32 fastcall _vmem_ReadMem32_not_mapped(u32 addr)
{
    if (should_log_unmapped(addr))
        printf("[vmem] Read32 from unmapped 0x%08X\n", addr);
    return (u32)MEM_ERROR_RETURN_VALUE;
}

void fastcall _vmem_WriteMem8_not_mapped(u32 addr, u8 data)
{
    if (should_log_unmapped(addr))
        printf("[vmem] Write8  to unmapped 0x%08X = 0x%02X\n", addr, data);
}
void fastcall _vmem_WriteMem16_not_mapped(u32 addr, u16 data)
{
    if (should_log_unmapped(addr))
        printf("[vmem] Write16 to unmapped 0x%08X = 0x%04X\n", addr, data);
}
void fastcall _vmem_WriteMem32_not_mapped(u32 addr, u32 data)
{
    if (should_log_unmapped(addr))
        printf("[vmem] Write32 to unmapped 0x%08X = 0x%08X\n", addr, data);
}

// ---------------------------------------------------------------------------
// Handler registration
// ---------------------------------------------------------------------------
_vmem_handler _vmem_register_handler(
    _vmem_ReadMem8FP*   read8,
    _vmem_ReadMem16FP*  read16,
    _vmem_ReadMem32FP*  read32,
    _vmem_WriteMem8FP*  write8,
    _vmem_WriteMem16FP* write16,
    _vmem_WriteMem32FP* write32)
{
    _vmem_handler rv = _vmem_lrp++;
    // Handlers are stored as id*4 in the low bits of the page table entry
    // (and the Wii recompiler inlines that layout), so only ids 0-7 fit.
    // verify() compiles to nothing in some files: fail loudly instead of
    // turning entry 8+ into a bogus host pointer.
    if (rv * 4 > HANDLER_MAX)
    {
        printf("FATAL: _vmem handler table full (id %u, max %u)\n", (unsigned)rv, (unsigned)(HANDLER_MAX / 4));
        abort();
    }

    _vmem_RF8 [rv] = read8   ? read8   : _vmem_ReadMem8_not_mapped;
    _vmem_RF16[rv] = read16  ? read16  : _vmem_ReadMem16_not_mapped;
    _vmem_RF32[rv] = read32  ? read32  : _vmem_ReadMem32_not_mapped;

    _vmem_WF8 [rv] = write8  ? write8  : _vmem_WriteMem8_not_mapped;
    _vmem_WF16[rv] = write16 ? write16 : _vmem_WriteMem16_not_mapped;
    _vmem_WF32[rv] = write32 ? write32 : _vmem_WriteMem32_not_mapped;

    return rv;
}

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------
void _vmem_map_handler(_vmem_handler Handler, u32 start, u32 end)
{
    verify(start < 0x100);
    verify(end   < 0x100);
    verify(start <= end);
    for (u32 i = start; i <= end; i++)
        _vmem_MemInfo_ptr[i] = (u8*)0 + (Handler * 4);
}

void _vmem_map_block(void* base, u32 start, u32 end, u32 mask)
{
    verify(start < 0x100);
    verify(end   < 0x100);
    verify(start <= end);
    verify(((unat)base & 0xFF) == 0);
    verify(base != 0);

    const u32 shift = FindMask(mask);
    u32 j = 0;
    for (u32 i = start; i <= end; i++)
    {
        // The access path masks the WHOLE address with `mask`, so every page
        // of a block points at the block base (upstream nullDC's
        // &base[j & mask] - (j & mask)). "base + j" was only equivalent for
        // blocks of 16 MB or less; NAOMI's 32 MB RAM spans two 16 MB pages.
        _vmem_MemInfo_ptr[i] = &((u8*)base)[j & mask] + shift - (j & mask);
        j += 0x1000000; // advance one 16 MB page
    }
}

void _vmem_mirror_mapping(u32 new_region, u32 start, u32 size)
{
    u32 end = start + size - 1;
    verify(start < 0x100);
    verify(end   < 0x100);
    verify(start <= end);
    // Destination region must not overlap source range
    verify(!((new_region >= start) && (new_region <= end)));

    for (u32 i = 0; i < size; i++)
        _vmem_MemInfo_ptr[(new_region + i) & 0xFF] = _vmem_MemInfo_ptr[(start + i) & 0xFF];
}

// ---------------------------------------------------------------------------
// Dynarec helpers
// ---------------------------------------------------------------------------
void _vmem_get_ptrs(u32 sz, bool write, void*** vmap, void*** func)
{
    *vmap = _vmem_MemInfo_ptr;
    switch (sz)
    {
    case 1: *func = write ? (void**)_vmem_WF8  : (void**)_vmem_RF8;  return;
    case 2: *func = write ? (void**)_vmem_WF16 : (void**)_vmem_RF16; return;
    case 4:
    case 8: *func = write ? (void**)_vmem_WF32 : (void**)_vmem_RF32; return;
    default: die("_vmem_get_ptrs: invalid size");
    }
}

void* _vmem_read_const(u32 addr, bool& ismem, u32 sz)
{
    const u32  page = addr >> 24;
    const unat iirf = (unat)_vmem_MemInfo_ptr[page];
    void* const ptr  = (void*)(iirf & ~(unat)HANDLER_MAX);

    if (ptr == 0)
    {
        ismem = false;
        const u32 id = (u32)iirf;
        if (sz == 1) return (void*)_vmem_RF8 [id / 4];
        if (sz == 2) return (void*)_vmem_RF16[id / 4];
        if (sz == 4) return (void*)_vmem_RF32[id / 4];
        die("_vmem_read_const: invalid size");
    }
    else
    {
        ismem = true;
        u32 shift = (u32)(iirf & HANDLER_MAX);
        addr <<= shift;
        addr >>= shift;
#if HOST_ENDIAN == ENDIAN_BIG
        if (sz < 4)
            addr ^= 4 - sz;
#endif
        return &((u8*)ptr)[addr];
    }
}

// ---------------------------------------------------------------------------
// Diagnostic helper
// ---------------------------------------------------------------------------
bool _vmem_is_mapped(u32 addr)
{
    const u32  page = addr >> 24;
    const unat iirf = (unat)_vmem_MemInfo_ptr[page];
    return (void*)(iirf & ~(unat)HANDLER_MAX) != nullptr;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void _vmem_init()
{
    _vmem_reset();
}

void _vmem_reset()
{
    memset(_vmem_RF8,  0, sizeof(_vmem_RF8));
    memset(_vmem_RF16, 0, sizeof(_vmem_RF16));
    memset(_vmem_RF32, 0, sizeof(_vmem_RF32));
    memset(_vmem_WF8,  0, sizeof(_vmem_WF8));
    memset(_vmem_WF16, 0, sizeof(_vmem_WF16));
    memset(_vmem_WF32, 0, sizeof(_vmem_WF32));

    memset(_vmem_MemInfo_ptr, 0, sizeof(_vmem_MemInfo_ptr));

    _vmem_lrp = 0;

    // Slot 0 = the "not-mapped" default; must be registered first.
    verify(_vmem_register_handler(0, 0, 0, 0, 0, 0) == 0);

    // Reset log-rate-limiter state.
    s_last_unmapped_page = 0xFFFFFFFFu;
    s_unmapped_log_count = 0;
}

void _vmem_term()
{
    // Nothing to do for the software-only mapping tables.
}

// ---------------------------------------------------------------------------
// Memory reservation
// ---------------------------------------------------------------------------
// Live texture-cache preset (wii/main.cpp). By the time EmuMain() runs, the
// game preset has been applied and the user has been through the options menu,
// so this is the final value -- which lets us skip the CACHE_VERY_FAST_PLUS
// overflow arena entirely for the games that will never read it.
extern "C" int get_texture_cache_preset();
// wii/main.cpp: 0 = off. Settled in the options menu before the game launches,
// same lifetime as the texture-cache preset above, so it is final here.
extern "C" int get_autosort_preset();
// gxRend.cpp: CACHE_VERY_FAST_PLUS() is preset 6. Keep the two in step.
#define TEXCACHE_VERY_FAST_PLUS 6
// wii/main.cpp: writes to the on-screen console AND appends to /ndclog.txt.
// Everything here runs before InitRenderer redirects stdout, so a plain printf
// would reach the screen only - and the screen is gone the moment a failed boot
// drops back to the Homebrew Channel.
extern "C" void wii_boot_log(const char* fmt, ...);
#if HOST_OS == OS_WII
#include "wii/wii_fastmem.h"
#include "plugs/drkPvr/gxRend.h"
#define WII_HEAP_KEEP_MEM1 (3u * 1024u * 1024u)
// Heap left to the C code once the guest page pool takes the rest. Measured
// on Initial D 3: ~6.8 MB used through the menus (3 MB of it in MEM1).
#define WII_HEAP_KEEP_MEM2 (4u * 1024u * 1024u)
#endif

#if HOST_OS != OS_WII
#include "host/host_hooks.h"
#endif

bool _vmem_reserve()
{
#if HOST_OS != OS_WII
    // Host build (Initial D3 port, Windows): zeroed, write-watched blocks
    // (host/host_win.cpp), no fastmem page table, no MEM2 arena.
    aica_ram.data = (u8*)host_GuestAlloc(ARAM_SIZE);
    vram.data     = (u8*)host_GuestAlloc(VRAM_SIZE);
    mem_b.data    = (u8*)host_GuestAlloc(RAM_SIZE);
    aica_ram.size = ARAM_SIZE;
    vram.size     = VRAM_SIZE;
    mem_b.size    = RAM_SIZE;
    return aica_ram.data && vram.data && mem_b.data;
#else
    // NAOMI 2 port: guest memory is demand paged through the MMU
    // (wii/wii_fastmem.cpp) - 88 MB of NAOMI 2 address space backed only
    // where the game writes. First the decoded-texture cache, which must be
    // real memory (the GPU reads it by physical address), off the top of
    // MEM2; then everything left above the heap headroom becomes the pool.
    extern u8* vram_buffer;
    {
        const u32 level = IRQ_Disable();
        u8* const a_lo = (u8*)SYS_GetArena2Lo();
        u8* const a_hi = (u8*)SYS_GetArena2Hi();
        u8* const buf = (u8*)(((u32)a_hi - GX_TEX_CACHE_BYTES - 65536u) & ~(u32)63);
        if (buf < a_lo)
        {
            IRQ_Restore(level);
            wii_boot_log("[vmem] no room for the %u KB texture cache (MEM2 free %d KB)\n",
                         (GX_TEX_CACHE_BYTES + 65536u) / 1024u, (s32)(a_hi - a_lo) / 1024);
            return false;
        }
        SYS_SetArena2Hi(buf);
        vram_buffer = buf;
        IRQ_Restore(level);
    }
    // Headroom left to the C heap: the renderer's frame buffers and GX FIFO
    // (MEM1), the CHD reader, audio and libogc allocations made after this.
    if (!WiiGuestMem_Init(WII_HEAP_KEEP_MEM1, WII_HEAP_KEEP_MEM2))
        return false;

    aica_ram.size = ARAM_SIZE;
    aica_ram.data = (u8*)WII_GUEST_ARAM_EA;
    vram.size     = VRAM_SIZE;
    vram.data     = (u8*)WII_GUEST_VRAM_EA;
    mem_b.size    = RAM_SIZE;
    mem_b.data    = (u8*)WII_GUEST_RAM_EA;
    return true;
#endif // HOST_OS != OS_WII
}

void _vmem_release()
{
    // Memory is either statically allocated (non-Wii) or managed by the Wii
    // arena allocator; no explicit free is needed here.
}
