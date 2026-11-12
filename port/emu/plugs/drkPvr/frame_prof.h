#pragma once
// plugin_header.h, not types.h: this header is included from ImgReader.cpp,
// which defines its own verify() macro before anything else and would collide
// with the one types.h defines. plugin_header.h carries config.h (HOST_OS /
// OS_WII) and the u32/u64 typedefs, which is all this header actually needs.
#include "plugins/plugin_header.h"

// ---------------------------------------------------------------------------
// FRAME_PROF - per-frame tail profiler ("where does the p99 frame go?")
//
// Everything else in this emulator measures AVERAGES over one second: the
// stats line's rnd:/ta:/arm:/aica:, the [TEXC] report, [HOT], [CC], [IFB].
// Those answer "what is slow", and they are the right tool for raising the
// steady-state SPEED%. They are the WRONG tool for a stutter, because a
// stutter is by definition a small number of frames: 3 bad frames a second
// that each eat 20 ms extra move a 1-second average by 6%, which is noise
// next to the 60 good frames around them. The spike is real and visible and
// the average says nothing happened.
//
// So this one keeps every frame instead of averaging them. One record per
// EMULATED vblank goes into a ring (the last ~8.5 s at 60 Hz); every couple of
// seconds the dump sorts the frame times, prints p50/p90/p95/p99/max, and then
// does the part that actually matters:
//
//   * the mean per-bucket breakdown of a TYPICAL frame (<= p50), and
//   * the mean per-bucket breakdown of the TAIL frames (>= p95), and
//   * the DIFFERENCE between the two, ranked.
//
// That ranked difference is the flame graph of the stutter. "tail frames spend
// +9.8 ms in tex and +1.2 ms in jit" names the culprit outright, instead of
// leaving you to infer it from an average that never moved.
//
// The buckets are a DISJOINT partition of the frame, so they sum to the frame
// time and the percentages mean something:
//
//   sh4   SH4 emulation - the JIT/interpreter itself, plus anything not
//         broken out below. The residual, so bracket errors land here.
//   rend  DoRender(), net of the two things below that happen inside it
//   tex   texture decode + upload (the cache-miss path in SetTextureParams)
//   gxw   blocking waits on the GP - every GX_DrawDone/GX_WaitDrawDone in
//         gxRend.cpp goes through one wrapper, so this is the whole stall
//   ta    TA list decode           (needs TA PROFILE on; reads 0 otherwise)
//   arm   ARM7 core
//   aica  AICA synthesis, net of the pacing sleep below
//   snd   audio pacing sleep in wii_audio_push_sample()
//   jit   rdv_CompilePC() - decode + ngen_Compile + any cache clear it forces
//   disc  libGDR_ReadSector() - the SD/USB fread behind the emulated GD-ROM
//   pad   host controller reads - maple_DoDma() and the scanline-driven
//         exit-combo poll, i.e. PAD/WPAD_ScanPads and Sixaxis USB traffic
//   cart  NAOMI: cart DMA reads (SD, CHD decompression, DES) and the slices of
//         the frame given to the read-ahead thread (naomi_board.cpp)
//
// Per-frame EVENT counts ride along, because a spike's size is only half the
// story and its cause is usually the count: dec= textures decoded, blk= blocks
// compiled, clr= JIT cache clears, rd= sector reads, wrap= texture-arena
// wraps, pad= host input polls.
//
// vi= is the odd one out and the reason a "tiny stutter" is sometimes NOT in
// any bucket above. The SH4 thread deliberately never VIDEO_WaitVSync()es, so
// presents free-run against the VI. Perfectly even 16.6 ms frames still judder
// if two of them land inside one VI field and the next field repeats. The VI
// post-retrace callback counts real fields between vblanks: a healthy run is
// almost all 1x, and a column of 2x/0x is a present-cadence problem that no
// amount of CPU optimisation will fix.
//
// Cost while OFF: one r13-relative load and a not-taken branch at each bracket,
// and nothing is allocated (the ring is lazily malloc'd when it first turns on,
// like the JIT HOTBLOCKS table). Cost while ON: two time-base reads per
// bracketed event - the busiest is texture decode, and a decode is thousands of
// cycles - plus ~10 reads and a 52-byte store once per emulated frame.
//
// The frame boundary is the emulated vblank in SPG.cpp, and the measured span
// deliberately EXCLUDES the speed limiter's sleep and this profiler's own
// printf: frame_prof_vblank() closes the span at the top of the vblank handler,
// frame_prof_resume() reopens it at the bottom after spg_PaceFrame(). Without
// that the limiter would pad every cheap frame out to exactly one frame period
// and flatten the very distribution we came to look at.
// ---------------------------------------------------------------------------

#if HOST_OS == OS_WII
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
// Single mftb of the low word, same trick and same wrap-exactness argument as
// PERF_TICKS32() in Renderer_if.h: only deltas are ever computed from it, and
// unsigned 32-bit subtraction is exact modulo 2^32. The time base runs at
// bus/4 = 60.75 MHz, so a bracket would have to last 70 s to be ambiguous.
#define FP_TICKS32() ((u32)PPCMftb())
#else
#define FP_TICKS32() ((u32)0)
#endif

// 0 = off, 1 = summary (percentiles + typical/tail/excess), 2 = summary plus a
// one-line dump of each individual tail frame. Defined in frame_prof.cpp, not
// in wii/main.cpp, so plugs/ and dc/ link without depending on a symbol from
// wii/ - same reasoning as g_ta_profile_preset in Renderer_if.cpp.
extern "C" int g_frame_prof_preset;

#define FRAME_PROF_ON() (__builtin_expect(g_frame_prof_preset != 0, 0))

// WHY a JIT cache clear fired, as a bitmask so a frame that hits more than one
// reason (rare, but n_clear can exceed 1) still reports every one of them
// rather than losing all but the last. Bit order matches dc/sh4/rec_v2/
// driver.cpp's six recSh4_ClearCache() call sites (5 on Wii; the 65536-byte
// non-Wii floor check is its own build and cannot fire here).
//
// Built because a clr=1 next to a jit storm showed up on real hardware with
// NONE of the five call sites' printf lines anywhere in the log, even though
// every one of them prints before clearing and the run continued for many
// more per-second fflush()es afterward -- so the line was not merely
// unflushed. Cheap and un-lose-able because it lives in the same gated
// accumulator as everything else here, instead of a printf that has to win a
// race with the ARM7/render threads' own stdout traffic to survive.
#define FP_CLR_MINFREE   (1u << 0)  // emit_FreeSpace() < CACHE_MIN_FREE
#define FP_CLR_FLUSH     (1u << 1)  // LastAddr >= CACHE_FLUSH_THRESHOLD (92%)
#define FP_CLR_BOOTADDR  (1u << 2)  // JIT_SBP: IP.BIN / 1ST_READ.BIN transition
#define FP_CLR_RETRY     (1u << 3)  // first compile attempt failed, clean retry
#define FP_CLR_BLOCKCHK  (1u << 4)  // rdv_BlockCheckFail: guest overwrote code
#define FP_CLR_RESET     (1u << 5)  // recSh4_Reset() via ResetDC()
// The one that was invisible. dc/sh4/ccn.cpp reaches recSh4_ClearCache through
// the sh4_cpu.ResetCache function POINTER, so it matches no grep for
// "recSh4_ClearCache(" and prints nothing (its printf is commented out on
// purpose -- "games invalidate the I-cache several times a second"). A guest
// CCR.ICI write therefore threw away the whole 6 MB code cache with no trace
// in the log at all. That is what a bare clr=1[] was.
#define FP_CLR_ICI       (1u << 6)  // guest CCR.ICI: SH4 I-cache invalidate

// Live accumulators for the frame in progress. Reset by frame_prof_vblank()
// once it has folded them into the ring.
struct FrameProfAcc
{
  u32 tex;       // ticks decoding/uploading textures   (inside rend)
  u32 jit;       // ticks in rdv_CompilePC()
  u32 disc;      // ticks in libGDR_ReadSector()
  u32 pad;       // ticks reading host controllers
  u32 mem;       // ticks paging guest memory in and out (NAOMI 2: faults, the vblank compression tick)
  u32 cart;      // ticks in cart reads and read-ahead slices (NAOMI)
  u32 gxw;       // ticks blocked on the GP, all call sites
  u32 gxw_rend;  // ...of which were taken inside DoRender(), so `rend` can be
                 // reported net of them without losing the ones taken outside
  u32 n_tex;     // textures decoded
  u32 n_jit;     // blocks compiled
  u32 n_clear;   // JIT cache clears (each one costs a recompile storm after it)
  u32 n_disc;    // libGDR_ReadSector() calls
  u32 n_wrap;    // texture arena wraps (a wrap re-decodes the whole working set)
  u32 n_pad;     // host input polls
  u32 n_mem;     // guest pages faulted in or compressed
  u32 clear_reasons; // OR of FP_CLR_* for every clear this frame, see above
};
extern FrameProfAcc g_fp;

// Bracket helpers. Written as macros rather than a scoped RAII object so the
// OFF path is provably a load/branch with no object to construct and nothing
// live across the measured call.
#define FP_T0(v)          u32 v = FRAME_PROF_ON() ? FP_TICKS32() : 0u
#define FP_ACC(field, v)  do { if (FRAME_PROF_ON()) g_fp.field += FP_TICKS32() - (v); } while (0)
#define FP_BUMP(field)    do { if (FRAME_PROF_ON()) g_fp.field++; } while (0)
#define FP_CLR_TAG(bit)   do { if (FRAME_PROF_ON()) g_fp.clear_reasons |= (bit); } while (0)

// Called from the emulated-vblank handler in SPG.cpp.
void frame_prof_vblank();          // close the frame, fold it into the ring
void frame_prof_resume();          // reopen the span after the stats + pacing
void frame_prof_dump(double secs); // no-op unless the preset is on

// ---------------------------------------------------------------------------
// Real-hardware view ([HW] lines, Wii, preset on). Two things the time base
// alone cannot tell, and Dolphin does not model at all:
//
//  * The 750CL performance counters (MMCR0/MMCR1, PMC1-4) over the whole frame
//    and over the 3D renders alone. Set A: cycles, instructions, cycles of L1
//    load misses, cycles of DTLB table walks -> IPC and where the stalls are.
//    Set B: L2 hits, L1 instruction misses, L2 data misses, mispredicted
//    branches. The two sets alternate from one dump to the next.
//
//  * The 3D render probe: CPU time of each displayed render, the part spent
//    feeding ELAN GPU lists (and their vertices), the GX FIFO overflows inside
//    it (each one suspends the CPU until the GP has drained 112 KB: the GP was
//    the slower side), and whether the GP was already idle when the CPU was
//    done - if not, how long it still drew (async render).
// ---------------------------------------------------------------------------
void frame_prof_render_begin();    // around the displayed-frame DoRender()
void frame_prof_render_end();
void frame_prof_gp_done();         // the GX draw-done interrupt
// Around each NAOMI 2 ELAN command run (naomi_elan.cpp): the walk and the
// recording of its lists get counters of their own, inside "rest of the frame".
void frame_prof_elan_begin();
void frame_prof_elan_end();
// PC SAMPLER (LOGS page): the heap its tables take when it is on, which _vmem.cpp
// keeps out of the guest page pool. Its [PCS] lines come with the [HW] dumps.
extern "C" int g_pc_sampler_preset;
u32 frame_prof_heap_bytes();
extern u32 g_fp_elan_ticks;       // ELAN GPU lists drawn (time base ticks)
extern u32 g_fp_elan_verts;        // ...and their vertices, of which (naomi_elan_gx.cpp GPU fetch):
extern u32 g_fp_elan_fetch;        //   read by the GPU from a list's cached program
extern u32 g_fp_elan_gap;          //     (of those, CPU-fed strips over frames that are not adjacent)
extern u32 g_fp_elan_patch;        //     (of those, read from copies of such strips: patches)
extern u32 g_fp_elan_build;        //   in lists whose program was built (CPU walk)
extern u32 g_fp_elan_wrote;        //   CPU-fed: a page written since the last render, or not resident
extern u32 g_fp_elan_resets;       // list table full: started over
extern u32 g_fp_elan_progs;        // programs built, patches written, ring passes (cumulative)
extern u32 g_fp_elan_patches;
extern u32 g_fp_elan_wraps;
extern u32 g_fp_elan_bounds;       // page boundaries in the lists built, of which gaps
extern u32 g_fp_elan_gaps;         //   (frames not adjacent: patches)
extern u32 g_fp_elan_items;        // ELAN GPU lists drawn, and of g_fp_elan_ticks the time spent
extern u32 g_fp_elan_progtb;       //   sending cached programs' indices (the rest: list setup, CPU-fed)
extern u32 g_fp_elan_strips;       //   in those many strips
extern u32 g_fp_elan_pgchk;        // ELAN page checks for the GPU fetch (wii_fastmem.cpp), full ones,
extern u32 g_fp_elan_pgdirty;      //   of those the pages found written,
extern u32 g_fp_elan_pgro;         //   checks of read-only pages (PAGE GUARD), a flag each,
extern u32 g_fp_elan_pgtb;         //   time base ticks in both;
extern u32 g_fp_elan_pgset;        //   pages made read-only, and guards dropped (a store faulted, or
extern u32 g_fp_elan_pgfault;      //   the page was mapped anew). All cumulative.
extern u32 g_fp_elan_dlcalls;      // DISPLAY LISTS: lists drawn from a display-list entry; entries
extern u32 g_fp_elan_dlbuilds;     //   written and their KB, ring passes, GPU waits before writing
extern u32 g_fp_elan_dlkb;         //   over one still queued and their time base ticks (all
extern u32 g_fp_elan_dlsyncs;      //   cumulative)
extern u32 g_fp_elan_dlwraps;
extern u32 g_fp_elan_dlwaittb;
