/*
    wii_fastmem.cpp - PowerPC MMU: demand-paged guest memory + JIT fastmem

    NAOMI 2 port (Initial D3). nullDC4Wii used the MMU only to give the SH4
    dynarec a direct window onto guest RAM/VRAM arrays that were allocated
    in full. A NAOMI 2 has 32 MB RAM + 16 MB VRAM + 8 MB sound RAM + 32 MB
    ELAN RAM, more than the Wii has, while the game only ever writes about
    55 MB of it and keeps under 40 MB non-zero. So here the MMU owns all
    guest memory:

      * a hand-built classic-PPC hashed page table (HTAB) maps guest
        windows at fixed effective addresses:

            EA 0x0C000000-0x0FFFFFFF  main RAM  (32 MB, mirrored once)
            EA 0x04000000-0x04FFFFFF  VRAM      (16 MB, 64-bit path)
            EA 0x06000000-0x06FFFFFF  VRAM      (mirror)
            EA 0x0A000000-0x0BFFFFFF  ELAN RAM  (32 MB)
            EA 0x20000000-0x207FFFFF  sound RAM (8 MB, segment 2: private)

        mem_b.data, vram.data, aica_ram.data and the ELAN RAM pointer ARE
        those addresses, so the interpreter, the C code and the dynarec all
        go through the page table;
      * nothing is mapped up front. The DSI (data storage) exception handler
        maps pages as they are touched: a load from a page never written
        maps one shared read-only zero page, a store gets a real 4 KB page
        from a pool carved out of MEM1 and MEM2;
      * when the pool runs low a clock sweep frees pages written with zeros
        only (the game clears ~30 MB at boot) and compresses cold ones into a
        compressed store (see "Compressed page store");
      * clearing a whole region (reset) releases its pages (guestmem_Discard).

    The sound RAM window sits in segment 2 on purpose: the dynarec reaches
    guest memory at EA = sh4_addr & 0x1FFFFFFF and wave RAM must keep going
    through the area-0 handlers (byte lanes), and EA 0x00000000-0x03FFFFFF
    must never be mapped (store-queue addresses mask onto it - see
    wii_fastmem.h).

    JIT fastmem (FASTMEM preset): the dynarec emits direct loads/stores at
    EA = sh4_addr & 0x1FFFFFFF. Inside the windows they now fault in like
    any other access; anywhere else (MMIO, BIOS, wave RAM...) a fault is
    still back-patched to a slow-path trampoline by rec_fastmem_patch().

    Why this is safe alongside libogc (per the libogc maintainer):
    libogc only uses BATs; SRs/SDR1 are left to the application, and the
    whole EA range 0x00000000-0x7FFFFFFF is untranslated/free. Data cache
    aliasing between the windows and the 0x8/0x9xxxxxxx views is coherent
    on Broadway: the 32KB 8-way L1 has 4KB per way, so cache index bits
    never exceed the page offset (VIPT with no aliasing), and L2 is physical.

    Exception-handler contract (from disassembly of the tuxedo libogc
    PPCExcptEntryGeneral stub, installed at each vector by PPCExcptInit):
      - the stub runs in real mode, saves r11/r12 to SPRG0/SPRG1, saves
        CR to __ppc_excpt_buf[2], SRR0 (fault PC) to buf[0], SRR1 (fault
        MSR) to buf[1], then rfi's into the registered handler with
        translation RE-ENABLED (MSR.IR/DR=1) and EE=0, RI=0, FP=0.
      - r11 = exception id at entry; all other GPRs still hold the
        interrupted values.
      - DAR/DSISR are still live in their SPRs.
      - if RI was already 0 (fault inside a handler) the stub panics via
        PPCExcptDefaultHandler before ever reaching us.
    So the handler below runs at its normal virtual address and may call
    plain (FPU-free!) C code, but must restore SRR0/SRR1/CR/r11/r12 from
    buf/SPRGs itself before rfi.
*/

#include "types.h"

#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include <tuxedo/ppc/exception.h>

#include "dc/mem/sh4_mem.h"      // mem_b
#include "dc/pvr/pvr_if.h"       // vram
#include "dc/aica/aica_if.h"     // aica_ram
#include "wii_fastmem.h"
#include "plugs/drkPvr/frame_prof.h" // FRAME_PROF "mem" bucket

// Preset accessor. 0=off, 1=on (JIT fastmem emission only; the paged guest
// memory is always on).
extern "C" int get_fastmem_preset();
extern "C" void wii_boot_log(const char* fmt, ...);

int g_wii_fastmem_active = 0;

// ============================================================================
// Page table
//
// 256 KB HTAB = 4096 PTEGs of 8 PTEs (32768 translations). The game's working
// set is ~14K pages plus mirror and zero-page mappings, well inside it; the
// secondary hash is the backstop and a full PTEG pair evicts a translation
// (it simply faults back in later).
// The hardware walks the table PHYSICALLY (uncached), so every PTE update is
// followed by a flush of its cache line.
// ============================================================================

#define HTAB_SIZE   (256 * 1024)
#define HTAB_MASK   0x03                      // HTABMASK for 256 KB
#define HASH_MASK   ((HTAB_MASK << 10) | 0x3FF)

// Arbitrary VSID base; SR0-SR7 get distinct VSIDs. Segments with no PTEs take
// a clean DSI instead of falsely hash-matching.
#define VSID_BASE   0x00DC0

#define PP_RW       0x2
#define PP_RO       0x3

static u8* s_htab      = 0;
static u32 s_htab_phys = 0;

static inline u32* pteg_for(u32 ea, u32 secondary)
{
    const u32 vsid = VSID_BASE + (ea >> 28);
    const u32 hash = (vsid & 0x7FFFF) ^ ((ea >> 12) & 0xFFFF);
    const u32 idx  = (secondary ? ~hash : hash) & HASH_MASK;
    return (u32*)(s_htab + (idx << 6));
}

static inline u32 pte_word0(u32 ea, u32 secondary)
{
    const u32 vsid = VSID_BASE + (ea >> 28);
    return 0x80000000 | (vsid << 7) | (secondary << 6) | ((ea >> 22) & 0x3F);
}

static inline void flush_pte(u32* pte)
{
    asm volatile("dcbst 0,%0; sync" :: "r"(pte) : "memory");
}

static inline void tlb_invalidate(u32 ea)
{
    asm volatile("sync; tlbie %0; sync; isync" :: "r"(ea) : "memory");
}

// Returns the PTE currently translating `ea`, or 0.
static u32* pte_find(u32 ea)
{
    for (u32 h = 0; h < 2; h++)
    {
        u32* pteg = pteg_for(ea, h);
        const u32 w0 = pte_word0(ea, h);
        for (u32 slot = 0; slot < 8; slot++)
            if (pteg[slot * 2] == w0)
                return &pteg[slot * 2];
    }
    return 0;
}

static void pte_remove(u32 ea)
{
    u32* pte = pte_find(ea);
    if (!pte)
        return;
    pte[0] = 0;
    flush_pte(pte);
    tlb_invalidate(ea);
}

// Maps `ea` onto physical page `pa`. Replaces an existing translation of the
// same EA; a full PTEG pair evicts slot 0 of the primary group.
// `rc`: the R (0x100) and C (0x80) bits to start with. Both are pre-set by
// default so the hardware never needs to write the table back; the ELAN GPU
// reads below map with C clear to see the next store.
static void pte_map(u32 ea, u32 pa, u32 pp, u32 rc = 0x180)
{
    const u32 w1 = (pa & 0xFFFFF000) | rc | pp;
    u32* pte = pte_find(ea);
    if (!pte)
    {
        for (u32 h = 0; h < 2 && !pte; h++)
        {
            u32* pteg = pteg_for(ea, h);
            for (u32 slot = 0; slot < 8; slot++)
                if (!(pteg[slot * 2] & 0x80000000))
                {
                    pte = &pteg[slot * 2];
                    pte[0] = pte_word0(ea, h);
                    break;
                }
        }
        if (!pte)
        {
            // Evict: the victim's EA simply faults back in when touched. Its
            // TLB entry may sit in another congruence class and must not
            // outlive the PTE (the page could be recycled), so flush them all.
            pte = pteg_for(ea, 0);
            pte[0] = 0;
            flush_pte(pte);
            for (u32 i = 0; i < 64; i++)
                asm volatile("tlbie %0" :: "r"(i << 12));
            asm volatile("sync; isync" ::: "memory");
            pte[0] = pte_word0(ea, 0);
        }
    }
    pte[0] &= ~0x80000000u;      // invalid while the RPN changes
    flush_pte(pte);
    pte[1] = w1;
    asm volatile("eieio" ::: "memory");
    pte[0] |= 0x80000000u;
    flush_pte(pte);
    tlb_invalidate(ea);
}

// ============================================================================
// Page pool
// ============================================================================

#define MAX_POOL_PAGES  (80u * 256u)          // 80 MB worth of page frames

static u32 s_free[MAX_POOL_PAGES];            // physical addresses
static u32 s_free_count = 0;
static u32 s_pool_pages = 0;
static u32 s_zero_page  = 0;                  // shared read-only zero page (physical)
// Reads of unwritten pages map the zero page only where the MMU enforces its
// read-only protection (real hardware). Dolphin ignores the PP bits, so there
// every first access, read or write, gets its own frame.
static bool s_zero_page_ro = true;
static u32 s_reclaims   = 0;
static u32 s_reclaimed  = 0;                  // pages freed by all reclaims
static u32 s_alloc_fails = 0;
static u32 s_last_fail_ea = 0;

static inline void* phys_to_k0(u32 pa)
{
    return (void*)(pa | 0x80000000);
}

static void clear_page(u32 pa)
{
    u8* p = (u8*)phys_to_k0(pa);
    for (u32 off = 0; off < 4096; off += 32)
        asm volatile("dcbz %0,%1" :: "b"(p), "r"(off) : "memory");
}

static bool page_is_zero(u32 pa)
{
    const u32* p = (const u32*)phys_to_k0(pa);
    for (u32 i = 0; i < 1024; i += 4)
        if (p[i] | p[i + 1] | p[i + 2] | p[i + 3])
            return false;
    return true;
}

// Adds [start, end) of an arena (cached virtual addresses) to the pool.
static void pool_add(u8* start, u8* end)
{
    u32 pa = MEM_VIRTUAL_TO_PHYSICAL(start);
    pa = (pa + 0xFFF) & ~0xFFFu;
    const u32 pa_end = MEM_VIRTUAL_TO_PHYSICAL(end) & ~0xFFFu;
    // Pushed high to low so allocation starts at the low end.
    for (u32 p = pa_end; p > pa && s_free_count < MAX_POOL_PAGES;)
    {
        p -= 0x1000;
        s_free[s_free_count++] = p;
        s_pool_pages++;
    }
}

// ============================================================================
// Guest regions
// ============================================================================

struct Window
{
    u32 base;
    u32 span;           // EA range covered; mirrors every `size` bytes
};

struct Region
{
    const char* name;
    u32 size;           // guest region size (power of two)
    Window win[2];
    u32 windows;
    u32* backing;       // per 4 KB page: 0 = not backed, a frame's physical
                        // address, or a compressed page (bit 0 set, see below)
    u32 backed;         // pages currently backed by a frame
    u8* skip;           // per page: clock sweeps to leave it alone
    u32 compressed;     // pages currently held compressed
};

static u32 s_ram_pages[RAM_SIZE >> 12];
static u32 s_vram_pages[VRAM_SIZE >> 12];
static u32 s_elan_pages[(32u << 20) >> 12];
static u32 s_aram_pages[ARAM_SIZE >> 12];
static u8 s_ram_skip[RAM_SIZE >> 12];
static u8 s_vram_skip[VRAM_SIZE >> 12];
static u8 s_elan_skip[(32u << 20) >> 12];
static u8 s_aram_skip[ARAM_SIZE >> 12];

static Region s_regions[4] = {
    {"ram",  RAM_SIZE,   {{WII_GUEST_RAM_EA, 0x04000000}, {0, 0}}, 1, s_ram_pages, 0, s_ram_skip, 0},
    {"vram", VRAM_SIZE,  {{WII_GUEST_VRAM_EA, VRAM_SIZE}, {WII_GUEST_VRAM_MIRROR_EA, VRAM_SIZE}}, 2, s_vram_pages, 0,
     s_vram_skip, 0},
    {"elan", 32u << 20,  {{WII_GUEST_ELAN_EA, 32u << 20}, {0, 0}}, 1, s_elan_pages, 0, s_elan_skip, 0},
    {"aram", ARAM_SIZE,  {{WII_GUEST_ARAM_EA, ARAM_SIZE}, {0, 0}}, 1, s_aram_pages, 0, s_aram_skip, 0},
};

static Region* region_for(u32 ea, u32& offset)
{
    for (u32 r = 0; r < 4; r++)
    {
        Region& reg = s_regions[r];
        for (u32 w = 0; w < reg.windows; w++)
            if (ea - reg.win[w].base < reg.win[w].span)
            {
                offset = (ea - reg.win[w].base) & (reg.size - 1);
                return &reg;
            }
    }
    return 0;
}

// Removes every translation of guest page `offset` of `reg` (all mirrors).
static void unmap_everywhere(const Region& reg, u32 offset)
{
    for (u32 w = 0; w < reg.windows; w++)
        for (u32 m = 0; m < reg.win[w].span; m += reg.size)
            pte_remove(reg.win[w].base + m + offset);
}

// ============================================================================
// Compressed page store
//
// The guest working set outgrows the pool (Initial D 3 holds ~46 MB non-zero
// in a race against ~42 MB of frames) but compresses ~2.3x page by page. When
// the pool runs low, a clock sweep over the resident pages frees all-zero
// ones and compresses those not referenced since its previous pass (PTE R
// bit, plus a few sweeps of grace after a page comes back) into 64-byte
// units packed inside pool frames ("store frames"). The next access to a
// compressed page faults like any unmapped one and is decompressed into a
// fresh frame.
//
// The codec is a small byte-oriented LZ77 (LZ4-like token format) written
// here rather than zlib: the DSI handler runs with the FPU off, so nothing it
// calls may touch an FP register, and it is several times faster.
//
// A compressed backing[] entry: bits 31-13 store frame, 12-7 first unit,
// 6-1 units - 1, bit 0 set (frame addresses are page aligned, bit 0 clear).
// ============================================================================

#define STORE_UNIT      64u
#define STORE_UNITS     (4096u / STORE_UNIT)  // 64 per frame: one u64 bitmap
#define MAX_STORE       8192u                 // 32 MB of compressed data
// Kept compressed only at half a page or less. A page at 85% of its size
// takes 54-56 of a frame's 64 units, so nothing else fits beside it and the
// frame it frees goes straight back into the store: a race showed 400
// compressions a second gaining nothing (2.7 MB into 2.6 MB) and 24 ms frames.
// At most 32 units, every compression frees at least half a frame.
#define COMP_MAX_BYTES  2048u

struct StoreFrame
{
    u32 pa;             // 0 = slot unused
    u64 used;           // unit bitmap
};
static StoreFrame s_store[MAX_STORE];
static u32 s_store_top = 0;                   // slots ever used
static u32 s_store_frames = 0;                // slots holding a frame now
static u32 s_store_cursor = 0;
static u32 s_compressions = 0, s_decompressions = 0, s_zero_freed = 0, s_corrupt = 0;

static inline u32 comp_handle(u32 slot, u32 unit, u32 units)
{
    return slot << 13 | unit << 7 | (units - 1) << 1 | 1;
}
static inline u32 comp_slot(u32 h) { return h >> 13; }
static inline u32 comp_unit(u32 h) { return (h >> 7) & 63; }
static inline u32 comp_units(u32 h) { return ((h >> 1) & 63) + 1; }
static inline u64 unit_mask(u32 unit, u32 units)
{
    return (units >= 64 ? ~(u64)0 : (((u64)1 << units) - 1)) << unit;
}

// First run of `units` free units in a store frame, or -1.
static int store_fit(const StoreFrame& f, u32 units)
{
    for (u32 u = 0; u + units <= STORE_UNITS; u++)
        if (!(f.used & unit_mask(u, units)))
            return (int)u;
    return -1;
}

// Reserves room for `units`; returns its handle or 0. A new store frame is
// taken straight from the free list (never through page_alloc: no sweep
// recursion).
static u32 store_alloc(u32 units)
{
    for (u32 n = 0; n < 32 && s_store_top; n++)
    {
        const u32 slot = (s_store_cursor + n) % s_store_top;
        StoreFrame& f = s_store[slot];
        if (!f.pa)
            continue;
        const int u = store_fit(f, units);
        if (u >= 0)
        {
            f.used |= unit_mask((u32)u, units);
            s_store_cursor = slot;
            return comp_handle(slot, (u32)u, units);
        }
    }
    if (s_free_count == 0)
        return 0;
    u32 slot = 0;
    while (slot < s_store_top && s_store[slot].pa)
        slot++;
    if (slot == s_store_top)
    {
        if (s_store_top == MAX_STORE)
            return 0;
        s_store_top++;
    }
    s_store[slot].pa = s_free[--s_free_count];
    s_store[slot].used = unit_mask(0, units);
    s_store_frames++;
    s_store_cursor = slot;
    return comp_handle(slot, 0, units);
}

static void store_free(u32 h)
{
    StoreFrame& f = s_store[comp_slot(h)];
    f.used &= ~unit_mask(comp_unit(h), comp_units(h));
    if (!f.used)
    {
        s_free[s_free_count++] = f.pa;
        f.pa = 0;
        s_store_frames--;
    }
}

static inline u8* store_data(u32 h)
{
    return (u8*)phys_to_k0(s_store[comp_slot(h)].pa) + comp_unit(h) * STORE_UNIT;
}

// ---- LZ codec (4 KB blocks) ------------------------------------------------
// Sequence: token (literal count << 4 | match length - 4, 15 = more bytes
// follow, each 255 = continue), literals, then unless the block is complete
// a 16-bit little-endian offset and the match length extension.
static u32 s_lz_table[4096];                  // hash -> gen << 12 | position
static u32 s_lz_gen = 0;

static inline u32 lz_read32(const u8* p)
{
    u32 v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static bool lz_emit(u8*& op, const u8* oend, const u8* lit, u32 nlit, u32 off, u32 ml)
{
    if (op + 1 + nlit + nlit / 255 + 1 + 2 + ml / 255 + 1 > oend)
        return false;
    u8* token = op++;
    u32 t;
    if (nlit >= 15)
    {
        t = 15u << 4;
        u32 n = nlit - 15;
        for (; n >= 255; n -= 255)
            *op++ = 255;
        *op++ = (u8)n;
    }
    else
        t = nlit << 4;
    for (u32 k = 0; k < nlit; k++)
        op[k] = lit[k];
    op += nlit;
    if (ml)
    {
        *op++ = (u8)off;
        *op++ = (u8)(off >> 8);
        u32 m = ml - 4;
        if (m >= 15)
        {
            t |= 15;
            for (m -= 15; m >= 255; m -= 255)
                *op++ = 255;
            *op++ = (u8)m;
        }
        else
            t |= m;
    }
    *token = (u8)t;
    return true;
}

// Compresses one 4 KB page; returns the size, or -1 if it exceeds `cap`.
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static int lz_compress(const u8* src, u8* dst, u32 cap)
{
    if (++s_lz_gen >= (1u << 20))
    {
        for (u32 k = 0; k < 4096; k++)       // no memset: may run in the fault handler
            s_lz_table[k] = 0;
        s_lz_gen = 1;
    }
    const u32 gen = s_lz_gen;
    u8* op = dst;
    const u8* const oend = dst + cap;
    u32 anchor = 0, i = 0;
    while (i + 4 <= 4096)
    {
        const u32 v = lz_read32(src + i);
        const u32 h = (v * 2654435761u) >> 20;
        const u32 e = s_lz_table[h];
        s_lz_table[h] = gen << 12 | i;
        if ((e >> 12) == gen)
        {
            const u32 c = e & 0xFFF;
            if (lz_read32(src + c) == v)
            {
                u32 ml = 4;
                while (i + ml < 4096 && src[c + ml] == src[i + ml])
                    ml++;
                if (!lz_emit(op, oend, src + anchor, i - anchor, i - c, ml))
                    return -1;
                i += ml;
                anchor = i;
                continue;
            }
        }
        i++;
    }
    if (!lz_emit(op, oend, src + anchor, 4096 - anchor, 0, 0))
        return -1;
    return (int)(op - dst);
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static bool lz_decompress(const u8* ip, const u8* iend, u8* dst)
{
    u8* op = dst;
    u8* const oend = dst + 4096;
    while (op < oend)
    {
        if (ip >= iend)
            return false;
        const u32 t = *ip++;
        u32 lit = t >> 4;
        if (lit == 15)
        {
            u32 b;
            do
            {
                if (ip >= iend)
                    return false;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (lit > (u32)(oend - op) || lit > (u32)(iend - ip))
            return false;
        for (u32 k = 0; k < lit; k++)
            op[k] = ip[k];
        op += lit;
        ip += lit;
        if (op >= oend)
            break;
        if (iend - ip < 2)
            return false;
        const u32 off = ip[0] | (u32)ip[1] << 8;
        ip += 2;
        u32 ml = (t & 15) + 4;
        if ((t & 15) == 15)
        {
            u32 b;
            do
            {
                if (ip >= iend)
                    return false;
                b = *ip++;
                ml += b;
            } while (b == 255);
        }
        if (off == 0 || off > (u32)(op - dst) || ml > (u32)(oend - op))
            return false;
        const u8* m = op - off;
        for (u32 k = 0; k < ml; k++)
            op[k] = m[k];
        op += ml;
    }
    return true;
}

// ---- Clock sweep -----------------------------------------------------------
static u8 s_lz_out[4096] ATTRIBUTE_ALIGN(32);

// Integer word copy: this runs in the fault handler (FPU off), where a libc
// memcpy is not guaranteed to stay off the FP registers.
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void copy_words(u32* dst, const u32* src, u32 words)
{
    for (u32 k = 0; k < words; k++)
        dst[k] = src[k];
}
static u32 s_clock_r = 0, s_clock_i = 0;

// Whether any translation of the page was used since the last look (the MMU
// sets the PTE R bit); clears it, and the TLB entry so the next use sets it.
static bool referenced_clear(const Region& reg, u32 offset)
{
    bool ref = false;
    for (u32 w = 0; w < reg.windows; w++)
        for (u32 m = 0; m < reg.win[w].span; m += reg.size)
        {
            const u32 ea = reg.win[w].base + m + offset;
            u32* pte = pte_find(ea);
            if (!pte)
                continue;
            asm volatile("dcbf 0,%0; sync" :: "r"(pte) : "memory");   // the MMU writes R behind the cache
            if (pte[1] & 0x100)
            {
                ref = true;
                pte[1] &= ~0x100u;
                flush_pte(pte);
                tlb_invalidate(ea);
            }
        }
    return ref;
}

// Frees page i of reg if it is all zeros or compresses well. 1 = frame freed.
static int evict(Region& reg, u32 i)
{
    const u32 pa = reg.backing[i];
    if (page_is_zero(pa))
    {
        unmap_everywhere(reg, i << 12);
        reg.backing[i] = 0;
        reg.backed--;
        s_free[s_free_count++] = pa;
        s_zero_freed++;
        return 1;
    }
    const int len = lz_compress((const u8*)phys_to_k0(pa), s_lz_out, COMP_MAX_BYTES);
    if (len < 0)
    {
        reg.skip[i] = 64;                     // incompressible: look again much later
        return 0;
    }
    const u32 units = ((u32)len + STORE_UNIT - 1) / STORE_UNIT;
    const u32 h = store_alloc(units);
    if (!h)
        return 0;
    unmap_everywhere(reg, i << 12);
    copy_words((u32*)store_data(h), (const u32*)s_lz_out, ((u32)len + 3) / 4);
    reg.backing[i] = h;
    reg.backed--;
    reg.compressed++;
    s_free[s_free_count++] = pa;
    s_compressions++;
    FP_BUMP(n_mem);
    return 1;
}

static u32 total_pages(void)
{
    u32 n = 0;
    for (u32 r = 0; r < 4; r++)
        n += s_regions[r].size >> 12;
    return n;
}

// Advances the clock until `want` frames are free, `max_evict` pages were
// freed, or two full turns found nothing more. Interrupts must be off.
// Frames pushed since s_free[from] go back so they come off the stack lowest
// first. Freed in page order, their frames mostly ascend, and popped highest
// first consecutive faults got descending frames: no two pages of a list
// were then adjacent, and the GPU fetch of ELAN lists (naomi_elan_fetch.inc)
// needs a patch at every gap between two pages' frames (real Wii, race:
// nearly every page boundary was one).
static void free_lowest_first(u32 from)
{
    for (u32 a = from, b = s_free_count; a + 1 < b; a++, b--)
    {
        const u32 t = s_free[a];
        s_free[a] = s_free[b - 1];
        s_free[b - 1] = t;
    }
}

static void clock_run(u32 want, u32 max_evict, u32 max_scan)
{
    const u32 first_free = s_free_count;
    const u32 turns = 2 * total_pages();
    const u32 limit = max_scan < turns ? max_scan : turns;
    u32 evicted = 0, tried = 0;
    // Failed compressions cost about as much as good ones: they count too,
    // so a run of incompressible pages (textures) can't stall a tick.
    const u32 max_tries = max_evict < 0x7FFFFFFFu ? 2 * max_evict : ~0u;
    for (u32 n = 0; n < limit && s_free_count < want && evicted < max_evict && tried < max_tries; n++)
    {
        Region& reg = s_regions[s_clock_r];
        const u32 i = s_clock_i;
        if (++s_clock_i >= (reg.size >> 12))
        {
            s_clock_i = 0;
            s_clock_r = (s_clock_r + 1) & 3;
        }
        const u32 pa = reg.backing[i];
        if (!pa || (pa & 1))
            continue;
        if (reg.skip[i])
        {
            reg.skip[i]--;
            continue;
        }
        // Zero pages go first, referenced or not: they cost nothing to bring back.
        if (!page_is_zero(pa) && referenced_clear(reg, i << 12))
            continue;
        tried++;
        evicted += evict(reg, i);
    }
    free_lowest_first(first_free);
}

// Below this many free frames the fault handler sweeps synchronously.
#define POOL_LOW_PAGES     64u                // 256 KB
#define POOL_REFILL_PAGES  512u               // ...until 2 MB are free again
// The per-vblank tick keeps this many free so the handler rarely has to.
#define POOL_SOFT_PAGES    1024u              // 4 MB
#define TICK_MAX_EVICT     24u
#define TICK_MAX_SCAN      512u               // pages looked at per tick (each may cost a tlbie)

// Returns a cleared page, or 0 when memory is truly exhausted.
static u32 page_alloc(void)
{
    if (s_free_count < POOL_LOW_PAGES)
    {
        s_reclaims++;
        const u32 before = s_free_count;
        clock_run(POOL_REFILL_PAGES, ~0u, ~0u);
        s_reclaimed += s_free_count > before ? s_free_count - before : 0;
    }
    if (s_free_count == 0)
        return 0;
    const u32 pa = s_free[--s_free_count];
    clear_page(pa);
    return pa;
}

// Demand-paging fault. Returns 1 when the access can be retried.
static int guest_fault(u32 ea, u32 dsisr)
{
    u32 offset;
    Region* reg = region_for(ea, offset);
    if (!reg)
        return 0;
    const u32 page_ea = ea & ~0xFFFu;
    const u32 index = offset >> 12;
    const bool store = (dsisr & 0x02000000) != 0;
    u32 pa = reg->backing[index];

    if (pa & 1)
    {
        // Compressed: bring it back into a fresh frame.
        const u32 h = pa;
        pa = page_alloc();
        if (!pa)
        {
            s_alloc_fails++;
            s_last_fail_ea = ea;
            return 0;
        }
        const u8* src = store_data(h);
        if (!lz_decompress(src, src + comp_units(h) * STORE_UNIT, (u8*)phys_to_k0(pa)))
            s_corrupt++;                      // cannot happen; zeros are the least bad outcome
        store_free(h);
        reg->backing[index] = pa;
        reg->backed++;
        reg->compressed--;
        reg->skip[index] = 4;                 // in use: a few sweeps of grace
        s_decompressions++;
    }
    else if (!pa)
    {
        if (!store && s_zero_page_ro)
        {
            // Never written: read zeros without spending a page.
            pte_map(page_ea, s_zero_page, PP_RO);
            return 1;
        }
        pa = page_alloc();
        if (!pa)
        {
            s_alloc_fails++;
            s_last_fail_ea = ea;
            return 0;                         // out of memory: panic with the fault
        }
        reg->backing[index] = pa;
        reg->backed++;
        // Other mirrors may still show the zero page: drop them.
        unmap_everywhere(*reg, offset & ~0xFFFu);
    }
    pte_map(page_ea, pa, PP_RW);
    return 1;
}

static bool s_guest_ready = false;

bool guestmem_Discard(void* data, u32 size)
{
    if (!s_guest_ready)
        return false;
    u32 offset;
    Region* reg = region_for((u32)data, offset);
    if (!reg || offset != 0 || size != reg->size)
        return false;
    const u32 level = IRQ_Disable();
    const u32 first_free = s_free_count;
    for (u32 i = 0; i < (reg->size >> 12); i++)
    {
        const u32 pa = reg->backing[i];
        reg->skip[i] = 0;
        if (!pa)
            continue;
        reg->backing[i] = 0;
        if (pa & 1)
        {
            store_free(pa);
            continue;
        }
        unmap_everywhere(*reg, i << 12);
        s_free[s_free_count++] = pa;
    }
    free_lowest_first(first_free);
    reg->backed = 0;
    reg->compressed = 0;
    IRQ_Restore(level);
    return true;
}

// Once per emulated vblank, outside the fault handler: keeps some frames
// free by compressing a few cold pages, so loads rarely stall in a big sweep.
extern "C" void WiiGuestMem_Tick(void)
{
    if (!s_guest_ready || s_free_count >= POOL_SOFT_PAGES)
        return;
    FP_T0(t0);
    const u32 level = IRQ_Disable();
    clock_run(POOL_SOFT_PAGES, TICK_MAX_EVICT, TICK_MAX_SCAN);
    IRQ_Restore(level);
    FP_ACC(mem, t0);
}

// ============================================================================
// GPU reads of ELAN RAM (naomi_elan_gx.cpp)
//
// The ELAN draw lets the GPU fetch a list's vertex records straight from
// their page frames once the list's strips are known. Per page it needs the
// frame (the GPU reads physical memory), whether the page was written since
// it last asked (its cached strips are then stale, and what the CPU wrote
// must reach memory first), and the frame to stay put while the GPU may
// still read it.
// Stores are seen through the PTE change bit: pte_map sets C up front, this
// clears it, and the MMU sets it again on the next store to the page. A page
// mapped anew (fault, decompression into another frame) comes back with C
// set, so it counts as written too.
// ============================================================================

static bool s_c_tracking = false;

extern "C" int WiiGuestMem_ElanGpuReady(void)
{
    return s_guest_ready && s_c_tracking;
}

extern "C" u32 WiiGuestMem_ElanGpuPage(u32 page, int* written)
{
    Region& reg = s_regions[2];
    *written = 1;
    if (!s_guest_ready || page >= (reg.size >> 12))
        return 0;
    const u32 level = IRQ_Disable();
    const u32 pa = reg.backing[page];
    if (!pa || (pa & 1))
    {
        IRQ_Restore(level);
        return 0;                             // never written, or compressed
    }
    if (reg.skip[page] < 4)
        reg.skip[page] = 4;                   // the GPU may read it: no sweep for a while
    const u32 ea = WII_GUEST_ELAN_EA + (page << 12);
    u32* pte = pte_find(ea);
    bool dirty = true;
    if (pte)
    {
        asm volatile("dcbf 0,%0; sync" :: "r"(pte) : "memory");   // the MMU writes C behind the cache
        dirty = (pte[1] & 0x80) != 0;
        if (dirty)
        {
            pte[1] &= ~0x80u;
            flush_pte(pte);
            tlb_invalidate(ea);
        }
    }
    else
    {
        // Its translation was evicted (full PTEG pair), C with it: map it
        // clean again and call it written, not knowing better.
        pte_map(ea, pa, PP_RW, 0x100);
    }
    IRQ_Restore(level);
    if (dirty)
        DCStoreRange(phys_to_k0(pa), 4096);
    *written = dirty;
    return pa;
}

// ============================================================================
// DSI handler
// ============================================================================

extern "C" void __wii_fastmem_dsi_handler(void);
extern "C" int  WiiFastmem_HandleDSI(u32 pc);
asm(R"(
    .text
    .align  2
    .globl  __wii_fastmem_dsi_handler
    .type   __wii_fastmem_dsi_handler, %function
__wii_fastmem_dsi_handler:
    stwu    1, -96(1)
    stw     0,  16(1)
    stw     3,  20(1)
    stw     4,  24(1)
    stw     5,  28(1)
    stw     6,  32(1)
    stw     7,  36(1)
    stw     8,  40(1)
    stw     9,  44(1)
    stw     10, 48(1)
    mflr    0
    stw     0,  52(1)
    mfctr   0
    stw     0,  56(1)
    mfxer   0
    stw     0,  60(1)
    lis     3, __ppc_excpt_buf@ha
    lwz     3, __ppc_excpt_buf@l(3)         # r3 = fault PC (saved SRR0)
    bl      WiiFastmem_HandleDSI            # 0 = handled, retry
    cmpwi   3, 0
    lwz     0,  60(1)
    mtxer   0
    lwz     0,  56(1)
    mtctr   0
    lwz     0,  52(1)
    mtlr    0
    lwz     10, 48(1)
    lwz     9,  44(1)
    lwz     8,  40(1)
    lwz     7,  36(1)
    lwz     6,  32(1)
    lwz     5,  28(1)
    lwz     4,  24(1)
    lwz     3,  20(1)
    lwz     0,  16(1)
    addi    1, 1, 96
    bne     1f                              # not ours -> panic
    lis     12, __ppc_excpt_buf@ha
    la      12, __ppc_excpt_buf@l(12)
    lwz     11, 0(12)
    mtsrr0  11                              # retry the faulting instruction
    lwz     11, 4(12)
    mtsrr1  11                              # restore interrupted MSR
    lwz     11, 8(12)
    mtcr    11
    mfsprg  11, 0
    mfsprg  12, 1
    rfi
1:  # Chain to the libogc panic path with the state it expects.
    li      11, 3                           # PPC_EXCPT_DSI
    cmpwi   1, 0                            # CR0.EQ = 0 (normal dispatch)
    b       PPCExcptDefaultHandler
    .size   __wii_fastmem_dsi_handler, . - __wii_fastmem_dsi_handler
    .previous
)");

extern "C" u32 __ppc_excpt_buf[];     // libogc lowmem save: [0]=SRR0 [1]=SRR1 [2]=CR

static u32 s_dsi_count = 0, s_page_faults = 0;

extern "C" int WiiFastmem_HandleDSI(u32 pc)
{
    s_dsi_count++;
    u32 dar, dsisr;
    asm volatile("mfdar %0" : "=r"(dar));
    asm volatile("mfdsisr %0" : "=r"(dsisr));
    // Demand paging first: inside a guest window every fault is a page that
    // is not mapped yet, whoever made the access (C code or JIT).
    if (dsisr & 0x48000000)
    {
        // Integer only (FPU off in here): the profiler's time base read is.
        FP_T0(t0);
        const int paged = guest_fault(dar, dsisr);
        FP_ACC(mem, t0);
        if (paged)
        {
            s_page_faults++;
            FP_BUMP(n_mem);
            return 0;
        }
    }
    if (!g_wii_fastmem_active)
        return 1;
    return rec_fastmem_patch(pc);
}

// ============================================================================
// Setup
// ============================================================================

static void enable_translation(void)
{
    const u32 level = IRQ_Disable();
    DCFlushRange(s_htab, HTAB_SIZE);
    asm volatile("sync");
    asm volatile("mtspr 25, %0" :: "r"(s_htab_phys | HTAB_MASK)); // SDR1
    for (u32 i = 0; i < 8; i++)
        asm volatile("mtsrin %0, %1" :: "r"(VSID_BASE + i), "r"(i << 28));
    asm volatile("sync; isync");
    for (u32 i = 0; i < 64; i++)
        asm volatile("tlbie %0" :: "r"(i << 12));
    asm volatile("sync; isync");
    IRQ_Restore(level);
}

static __attribute__((noinline)) bool self_test(void)
{
    volatile u32* ram = (volatile u32*)(WII_GUEST_RAM_EA + 0x10);
    volatile u32* mirror = (volatile u32*)(WII_GUEST_RAM_EA + RAM_SIZE + 0x10);
    volatile u32* aram = (volatile u32*)(WII_GUEST_ARAM_EA + 0x7FFFF0);
    bool ok = *ram == 0 && *aram == 0;           // zero-page reads
    *ram = 0xA55A1234;
    *aram = 0x5AA54321;
    ok &= *ram == 0xA55A1234 && *mirror == 0xA55A1234 && *aram == 0x5AA54321;
    *ram = 0;
    *aram = 0;
    return ok;
}

// The shared zero page is mapped read-only (PP=3) so the first store to a
// page that was only read faults and gets its own frame. If the MMU does not
// enforce PP (an emulator might not), that store lands in the zero page and
// every unwritten page in every region sees it. Probe: map two pages through
// reads, write the first, read the second.
static __attribute__((noinline)) bool zero_page_protected(void)
{
    const u32 ea_a = WII_GUEST_RAM_EA + 0x1FF0000;
    const u32 ea_b = WII_GUEST_RAM_EA + 0x1FF1000;
    volatile u32* a = (volatile u32*)ea_a;
    volatile u32* b = (volatile u32*)ea_b;
    const u32 before = *a + *b;                  // both now map the zero page
    *a = 0x5A5AA5A5;
    const bool isolated = *b == 0;
    *a = 0;
    if (isolated)
        return before == 0;
    // Stores went into the zero page, here and in self_test(): every
    // translation made so far may point at it, writable in effect (RAM page
    // 0 among them, where the boot later writes the exception stubs). Start
    // the page table over and clean the page; backed pages fault back in.
    const u32 level = IRQ_Disable();
    memset(s_htab, 0, HTAB_SIZE);
    DCFlushRange(s_htab, HTAB_SIZE);
    asm volatile("sync" ::: "memory");
    for (u32 i = 0; i < 64; i++)
        asm volatile("tlbie %0" :: "r"(i << 12));
    asm volatile("sync; isync" ::: "memory");
    clear_page(s_zero_page);
    IRQ_Restore(level);
    return false;
}

// Whether a store sets the change bit of its PTE again once it is cleared
// (what the ELAN GPU reads rely on to see writes). Probed on a RAM page next
// to zero_page_protected's.
static __attribute__((noinline)) bool change_bit_tracked(void)
{
    const u32 ea = WII_GUEST_RAM_EA + 0x1FF2000;
    volatile u32* p = (volatile u32*)ea;
    *p = 0;                                      // backs and maps the page, C set
    const u32 level = IRQ_Disable();
    u32* pte = pte_find(ea);
    bool set = false;
    if (pte)
    {
        pte[1] &= ~0x80u;
        flush_pte(pte);
        tlb_invalidate(ea);
        *p = 0;
        asm volatile("sync" ::: "memory");
        asm volatile("dcbf 0,%0; sync" :: "r"(pte) : "memory");
        set = (pte[1] & 0x80) != 0;
    }
    IRQ_Restore(level);
    return set;
}

extern "C" int WiiGuestMem_Init(u32 mem1_keep, u32 mem2_keep)
{
    if (s_guest_ready)
        return 1;
    const u32 level = IRQ_Disable();

    // The page table, from MEM1 if it has room (it is walked on every TLB
    // miss), aligned to its size.
    u8* m1_lo = (u8*)SYS_GetArena1Lo();
    u8* m1_hi = (u8*)SYS_GetArena1Hi();
    u8* htab = (u8*)(((u32)m1_hi - HTAB_SIZE) & ~(u32)(HTAB_SIZE - 1));
    if (htab >= m1_lo + mem1_keep)
        SYS_SetArena1Hi(htab);
    else
    {
        u8* m2_hi = (u8*)SYS_GetArena2Hi();
        htab = (u8*)(((u32)m2_hi - HTAB_SIZE) & ~(u32)(HTAB_SIZE - 1));
        SYS_SetArena2Hi(htab);
    }
    s_htab = htab;
    s_htab_phys = MEM_VIRTUAL_TO_PHYSICAL(htab);
    memset(s_htab, 0, HTAB_SIZE);

    // Page frames: everything above the kept headroom of each arena.
    m1_lo = (u8*)SYS_GetArena1Lo();
    m1_hi = (u8*)SYS_GetArena1Hi();
    if (m1_hi - m1_lo > (s32)mem1_keep)
    {
        u8* start = m1_lo + mem1_keep;
        pool_add(start, m1_hi);
        SYS_SetArena1Hi(start);
    }
    u8* m2_lo = (u8*)SYS_GetArena2Lo();
    u8* m2_hi = (u8*)SYS_GetArena2Hi();
    if (m2_hi - m2_lo > (s32)mem2_keep)
    {
        u8* start = m2_lo + mem2_keep;
        pool_add(start, m2_hi);
        SYS_SetArena2Hi(start);
    }
    if (s_free_count < 1024)
    {
        IRQ_Restore(level);
        wii_boot_log("[mem] page pool too small: %u KB\n", s_free_count * 4);
        return 0;
    }
    s_zero_page = s_free[--s_free_count];
    clear_page(s_zero_page);
    IRQ_Restore(level);

    enable_translation();
    PPCExcptSetHandler(PPC_EXCPT_DSI, __wii_fastmem_dsi_handler);
    s_guest_ready = true;
    // PPCExcptSetHandler is a plain store into libogc's table: without a
    // barrier the compiler moved it after the self-test's first (volatile)
    // guest read, so that fault reached the panic handler instead of ours.
    asm volatile("sync" ::: "memory");

    if (!self_test())
    {
        wii_boot_log("[mem] MMU self-test FAILED\n");
        s_guest_ready = false;
        return 0;
    }
    s_zero_page_ro = zero_page_protected();
    if (!s_zero_page_ro)
        wii_boot_log("[mem] MMU page protection not enforced (emulator): unwritten pages are backed on first read\n");
    s_c_tracking = change_bit_tracked();
    if (!s_c_tracking)
        wii_boot_log("[mem] MMU change bit not tracked (emulator): ELAN lists stay on the CPU-fed GPU path\n");
    wii_boot_log("[mem] paged guest memory: pool %u KB, page table at %p\n", s_pool_pages * 4, s_htab);
    return 1;
}

extern "C" void WiiGuestMem_Stats(WiiGuestMemStats* out)
{
    out->pool_kb = s_pool_pages * 4;
    out->free_kb = s_free_count * 4;
    out->ram_kb  = s_regions[0].backed * 4;
    out->vram_kb = s_regions[1].backed * 4;
    out->elan_kb = s_regions[2].backed * 4;
    out->aram_kb = s_regions[3].backed * 4;
    out->reclaims = s_reclaims;
    out->reclaimed_kb = s_reclaimed * 4;
    out->alloc_fails = s_alloc_fails;
    out->last_fail_ea = s_last_fail_ea;
    u32 comp = 0;
    for (u32 r = 0; r < 4; r++)
        comp += s_regions[r].compressed;
    out->comp_kb = comp * 4;
    out->store_kb = s_store_frames * 4;
    out->compressions = s_compressions;
    out->decompressions = s_decompressions;
    out->corrupt = s_corrupt;
    out->dsi = s_dsi_count;
    out->page_faults = s_page_faults;
}

void WiiFastmem_Init(void)
{
    // The dynarec's direct accesses only need the paged windows to exist.
    if (!get_fastmem_preset() || !s_guest_ready)
    {
        printf("[fastmem] %s\n", s_guest_ready ? "preset off" : "guest memory not paged - inactive");
        return;
    }
    g_wii_fastmem_active = 1;
    printf("[fastmem] active on the paged guest windows\n");
}
