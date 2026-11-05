// ---------------------------------------------------------------------------
// FRAME_PROF - per-frame tail profiler. See frame_prof.h for what it is for and
// why a once-a-second average cannot answer the same question.
// ---------------------------------------------------------------------------
#include "frame_prof.h"
#include "Renderer_if.h"   // RenderTicks / TaPerf / ArmTicks / AicaTicks / SndWaitTicks
#include "drkPvr.h"        // spg_PaceFrameSec
#include "dc/sh4/rec_v2/ccall_census.h"   // g_ccall: C call-outs per frame (JIT CCALLS)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Output goes out one whole line at a time, already formatted, as printf("%s"):
// Dolphin intercepts printf (HLE) and formats it itself, piece by piece, and
// mangles float arguments that way; a single string argument survives.
static char s_fp_line[1024];
static size_t s_fp_len = 0;

static void fp_out(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(s_fp_line + s_fp_len, sizeof(s_fp_line) - s_fp_len, fmt, ap);
  va_end(ap);
  if (n > 0)
    s_fp_len += (size_t)n < sizeof(s_fp_line) - s_fp_len ? (size_t)n : sizeof(s_fp_line) - s_fp_len - 1;
  if (s_fp_len > 0 && (s_fp_line[s_fp_len - 1] == 0x0A || s_fp_len >= sizeof(s_fp_line) - 1))
  {
    printf("%s", s_fp_line);
    s_fp_len = 0;
    s_fp_line[0] = 0;
  }
}


#if HOST_OS == OS_WII
#include <ogc/video.h>
#include <ogc/gx.h>
#endif

// The preset lives here rather than in wii/main.cpp so that plugs/ and dc/ can
// reference it without pulling in a symbol from wii/ - same arrangement as
// g_ta_profile_preset in Renderer_if.cpp.
extern "C" { int g_frame_prof_preset = 0; }

// No initializer on purpose: static storage zero-initializes, and a field list
// here would silently fall out of step the next time a bucket is added.
FrameProfAcc g_fp;

// -- Buckets ---------------------------------------------------------------
// Disjoint by construction: sh4 is the residual, and the two things that
// happen INSIDE DoRender() (tex, and the GP waits DoRender itself takes) are
// subtracted out of rend when the sample is built, so the ten columns sum to
// the frame time and their percentages are real shares.
enum { FPB_SH4, FPB_REND, FPB_TEX, FPB_GXW, FPB_TA, FPB_ARM,
       FPB_AICA, FPB_SND, FPB_JIT, FPB_DISC, FPB_MEM, FPB_PAD, FPB_CART, FP_NB };

// NAOMI 2 has no GD-ROM: its "disc" bucket times the ELAN instead (command
// walk + T&L + the TA decode of its output, naomi_elan.cpp).
static const char *const kFpName[FP_NB] =
#if defined(BUILD_NAOMI2)
  { "sh4", "rend", "tex", "gxw", "ta", "arm", "aica", "snd", "jit", "elan", "mem", "pad", "cart" };
#else
  { "sh4", "rend", "tex", "gxw", "ta", "arm", "aica", "snd", "jit", "disc", "mem", "pad", "cart" };
#endif

struct FpSample
{
  u32 seq;          // monotonic emulated-frame number, for the tail listing
  u32 total;        // whole frame, pacing sleep and this profiler excluded
  u32 b[FP_NB];
  u16 n_tex, n_jit, n_clear, n_disc, n_wrap, n_pad, n_mem;
  u32 n_cc;         // C call-outs from SH-4 code (JIT CCALLS on; 0 otherwise)
  u8  vi;           // VI fields that elapsed during this frame (255 = clamped)
  u8  drawn;        // 0 when frameskip dropped the render
  u8  clear_reasons; // OR of FP_CLR_* (frame_prof.h) for every clear this frame
};

// One short tag per FP_CLR_* bit, in bit order. Turns a clear_reasons mask
// into e.g. "flush+blkchk" instead of a number nobody can read at a glance.
static const char *const kClrTag[] =
  { "minfree", "flush", "bootaddr", "retry", "blkchk", "reset", "ici" };

static void fp_clr_str(char *out, size_t outsz, u8 mask)
{
  out[0] = '\0';
  bool first = true;
  for (int i = 0; i < (int)(sizeof(kClrTag) / sizeof(kClrTag[0])); i++)
  {
    if (!(mask & (1u << i))) continue;
    size_t len = strlen(out);
    if (len >= outsz - 1) break;
    snprintf(out + len, outsz - len, "%s%s", first ? "" : "+", kClrTag[i]);
    first = false;
  }
}

// ~8.5 s of history at 60 Hz. Deep enough that p99 is the 5th-worst frame of a
// real sample rather than the worst of sixty, which is what a 1-second window
// would give and is not a percentile in any useful sense.
#define FP_RING       512
#define FP_DUMP_EVERY 2   // stats-block ticks (~1 s each) between dumps
#define FP_TAIL_LIST  8   // frames listed individually in mode 2

// Lazily allocated, so the preset costs no MEM1 while off - the JIT HOTBLOCKS
// table is allocated the same way and for the same reason.
static FpSample *s_ring   = 0;
static u32      *s_sorted = 0;
static bool      s_alloc_failed = false;

static u32 s_head  = 0;   // next slot to write
static u32 s_count = 0;   // valid samples, saturating at FP_RING
static u32 s_seq   = 0;

// Span bookkeeping. s_resume is the time base reading taken at the BOTTOM of
// the vblank handler; the next vblank closes the span against it.
static u32  s_resume = 0;
static bool s_have_resume = false;

// Previous values of the shared accumulators, so this profiler can take
// per-frame deltas of counters that SPG.cpp zeroes once a second. A zeroing is
// detected as cur < last, and then cur IS the delta: the reset happens later in
// the same vblank handler that sampled them, so nothing is lost between the two.
static u32 s_last_rend = 0, s_last_ta = 0, s_last_arm = 0;
static u32 s_last_aica = 0, s_last_snd = 0;
static u32 s_last_cc = 0;

// All C call-outs counted so far (the census only counts while JIT CCALLS is on).
static u32 fp_cc_total()
{
  u32 t = 0;
  for (int i = 0; i < CC_COUNT; i++) t += g_ccall[i];
  return t;
}

#if HOST_OS == OS_WII
// VI field counter. The callback runs in interrupt context, so it does exactly
// one store; any previously installed user callback is chained. Hooked on first
// use and never removed - it is one increment per field.
static volatile u32 s_vi_ticks = 0;
static VIRetraceCallback s_vi_prev = 0;
static bool s_vi_hooked = false;

static void fp_vi_cb(u32 cnt)
{
  s_vi_ticks = s_vi_ticks + 1;   // not ++: -Wvolatile deprecates it
  if (s_vi_prev) s_vi_prev(cnt);
}
#endif
static u32 s_vi_last = 0;

#if HOST_OS == OS_WII
#define FP_MS(t) ((double)ticks_to_microsecs((u64)(u32)(t)) / 1000.0)
#else
#define FP_MS(t) ((void)(t), 0.0)
#endif

// ---------------------------------------------------------------------------
// Real-hardware view: the [HW] lines (see frame_prof.h). Wii only; the hooks
// are empty elsewhere.
// ---------------------------------------------------------------------------
u32 g_fp_elan_ticks = 0;
u32 g_fp_elan_verts = 0;
u32 g_fp_elan_fetch = 0;
u32 g_fp_elan_gap = 0;
u32 g_fp_elan_patch = 0;
u32 g_fp_elan_build = 0;
u32 g_fp_elan_wrote = 0;
u32 g_fp_elan_resets = 0;
u32 g_fp_elan_progs = 0;
u32 g_fp_elan_patches = 0;
u32 g_fp_elan_wraps = 0;
u32 g_fp_elan_bounds = 0;
u32 g_fp_elan_gaps = 0;
u32 g_fp_elan_items = 0;
u32 g_fp_elan_progtb = 0;
u32 g_fp_elan_strips = 0;
u32 g_fp_elan_pgchk = 0;
u32 g_fp_elan_pgdirty = 0;
u32 g_fp_elan_pgro = 0;
u32 g_fp_elan_pgtb = 0;
u32 g_fp_elan_pgset = 0;
u32 g_fp_elan_pgfault = 0;

#if HOST_OS == OS_WII
// Event set being counted (index into kPmcSel), -1 = counters not programmed.
static int s_pmc_set = -1;

struct FpPmc { u32 c[4]; };

// PMC1..PMC4 = SPR 953, 954, 957, 958 (supervisor; libogc runs supervisor).
static INLINE void fp_pmc_read(FpPmc &p)
{
  asm volatile("mfspr %0,953" : "=r"(p.c[0]));
  asm volatile("mfspr %0,954" : "=r"(p.c[1]));
  asm volatile("mfspr %0,957" : "=r"(p.c[2]));
  asm volatile("mfspr %0,958" : "=r"(p.c[3]));
}

// 750/750CL performance monitor event encodings, PMC1..PMC4:
//   set A: 1 processor cycles, 2 instructions completed, 15 cycles generated
//          by L1 load misses, 6 cycles of DTLB table searches
//   set B: 7 accesses that hit the L2, 5 L1 instruction cache misses,
//          7 L2 data misses, 8 mispredicted branches
static const u32 kPmcSel[2][4] = { { 1, 2, 15, 6 }, { 7, 5, 7, 8 } };

static void fp_pmc_program(int set)
{
  // MMCR0 (SPR 952): PMC1SELECT = bits 19-25 (<< 6), PMC2SELECT = bits 26-31;
  // DIS, DP, DU, DMS, DMR and ENINT clear = count in every mode, no interrupt.
  // MMCR1 (SPR 956): PMC3SELECT = bits 0-4 (<< 27), PMC4SELECT = bits 5-9 (<< 22).
  const u32 mmcr0 = kPmcSel[set][0] << 6 | kPmcSel[set][1];
  const u32 mmcr1 = kPmcSel[set][2] << 27 | kPmcSel[set][3] << 22;
  asm volatile("mtspr 952,%0" : : "r"(0x80000000u));   // MMCR0.DIS while switching
  asm volatile("mtspr 956,%0" : : "r"(mmcr1));
  asm volatile("mtspr 953,%0" : : "r"(0u));
  asm volatile("mtspr 954,%0" : : "r"(0u));
  asm volatile("mtspr 957,%0" : : "r"(0u));
  asm volatile("mtspr 958,%0" : : "r"(0u));
  asm volatile("mtspr 952,%0" : : "r"(mmcr0));
  s_pmc_set = set;
}

// One dump window's worth.
struct FpHwWin
{
  u64 frame[4], rend[4];   // counter deltas: whole frames, displayed 3D renders
  u64 frameTb, rendTb;     // the same spans in time base ticks
  u32 frames, renders;
  u64 elanTb, elanVerts;   // ELAN GPU lists drawn inside those renders
  u64 elanFetch, elanBuild, elanWrote;   // ...their vertices by GPU fetch outcome
  u64 elanGap, elanPatch;
  u64 elanItems, elanProgTb, elanStrips;   // lists drawn; time and strips in cached programs
  u32 overflows;           // GX FIFO overflows inside them
  u32 gpIdle;              // renders after which the GP was already idle
  u32 gpWaited;            // the others that got a draw-done reading...
  u64 gpBacklogTb;         // ...and how long after the CPU it came
};
static FpHwWin s_hw;
static FpPmc s_pmc_frame0, s_pmc_rend0;
static bool s_frame_open = false, s_rend_open = false;
static u32 s_rend_t0 = 0, s_ovf0 = 0, s_elan_t0 = 0, s_elan_v0 = 0;
static u32 s_elan_f0 = 0, s_elan_b0 = 0, s_elan_w0 = 0, s_elan_g0 = 0, s_elan_p0 = 0, s_elan_resets0 = 0;
static u32 s_elan_progs0 = 0, s_elan_patches0 = 0, s_elan_wraps0 = 0, s_elan_bounds0 = 0, s_elan_gaps0 = 0;
static u32 s_elan_i0 = 0, s_elan_pt0 = 0, s_elan_s0 = 0;
static u32 s_pg_chk0 = 0, s_pg_dirty0 = 0, s_pg_ro0 = 0, s_pg_tb0 = 0, s_pg_set0 = 0, s_pg_fault0 = 0;

// Draw-done probe: the interrupt stamps every draw-done; a render that left
// the GP busy takes the first stamp after its own end.
static volatile u32 s_gp_last_done = 0;
static volatile u32 s_gp_done_n = 0;
static bool s_gp_waiting = false;
static u32 s_gp_cpu_end = 0, s_gp_n_at_end = 0;

void frame_prof_gp_done()
{
  s_gp_last_done = FP_TICKS32();
  s_gp_done_n = s_gp_done_n + 1;
}

static void fp_gp_poll()
{
  if (s_gp_waiting && s_gp_done_n != s_gp_n_at_end)
  {
    const u32 d = s_gp_last_done - s_gp_cpu_end;
    if ((s32)d >= 0)
    {
      s_hw.gpBacklogTb += d;
      s_hw.gpWaited++;
    }
    s_gp_waiting = false;
  }
}

void frame_prof_render_begin()
{
  if (!FRAME_PROF_ON() || s_pmc_set < 0)
    return;
  fp_gp_poll();
  s_gp_waiting = false;   // a previous reading that never came is dropped
  s_elan_t0 = g_fp_elan_ticks;
  s_elan_v0 = g_fp_elan_verts;
  s_elan_f0 = g_fp_elan_fetch;
  s_elan_b0 = g_fp_elan_build;
  s_elan_w0 = g_fp_elan_wrote;
  s_elan_g0 = g_fp_elan_gap;
  s_elan_p0 = g_fp_elan_patch;
  s_elan_i0 = g_fp_elan_items;
  s_elan_pt0 = g_fp_elan_progtb;
  s_elan_s0 = g_fp_elan_strips;
  s_ovf0 = GX_GetOverflowCount();
  s_rend_t0 = FP_TICKS32();
  fp_pmc_read(s_pmc_rend0);
  s_rend_open = true;
}

void frame_prof_render_end()
{
  if (!s_rend_open)
    return;
  s_rend_open = false;
  FpPmc p;
  fp_pmc_read(p);
  const u32 t = FP_TICKS32();
  for (int k = 0; k < 4; k++)
    s_hw.rend[k] += p.c[k] - s_pmc_rend0.c[k];
  s_hw.rendTb += t - s_rend_t0;
  s_hw.renders++;
  s_hw.overflows += GX_GetOverflowCount() - s_ovf0;
  s_hw.elanTb += g_fp_elan_ticks - s_elan_t0;
  s_hw.elanVerts += g_fp_elan_verts - s_elan_v0;
  s_hw.elanFetch += g_fp_elan_fetch - s_elan_f0;
  s_hw.elanBuild += g_fp_elan_build - s_elan_b0;
  s_hw.elanWrote += g_fp_elan_wrote - s_elan_w0;
  s_hw.elanGap += g_fp_elan_gap - s_elan_g0;
  s_hw.elanPatch += g_fp_elan_patch - s_elan_p0;
  s_hw.elanItems += g_fp_elan_items - s_elan_i0;
  s_hw.elanProgTb += g_fp_elan_progtb - s_elan_pt0;
  s_hw.elanStrips += g_fp_elan_strips - s_elan_s0;

  // Async render queued the draw-done last: is the GP through it already?
  const u32 n0 = s_gp_done_n;
  u8 overhi, underlow, readIdle, cmdIdle, brkpt;
  GX_GetGPStatus(&overhi, &underlow, &readIdle, &cmdIdle, &brkpt);
  if (readIdle && cmdIdle)
    s_hw.gpIdle++;
  else
  {
    s_gp_waiting = true;
    s_gp_cpu_end = t;
    s_gp_n_at_end = n0;
  }
}

static double fp_tb_ms(u64 t)
{
  return (double)t / (double)TB_TIMER_CLOCK;   // TB_TIMER_CLOCK = ticks per ms
}

static double fp_ratio(u64 a, u64 b)
{
  return b ? (double)a / (double)b : 0.0;
}

static void fp_hw_dump()
{
  if (s_pmc_set < 0)
    return;
  const FpHwWin &w = s_hw;
  if (w.renders)
  {
    const double r = (double)w.renders;
    const u32 waited = w.gpWaited;
    fp_out("[HW] 3D renders shown: %u in %u frames | CPU %.2f ms each, of it ELAN GPU lists %.2f ms"
           " (%.0f vertices, %.0f ns each) | GX FIFO full %.1f times a render"
           " | GP idle when the CPU was done: %u of %u, else still drawing %.2f ms (%u readings)\n",
           w.renders, w.frames, fp_tb_ms(w.rendTb) / r, fp_tb_ms(w.elanTb) / r,
           (double)w.elanVerts / r,
           w.elanVerts ? fp_tb_ms(w.elanTb) * 1e6 / (double)w.elanVerts : 0.0,
           (double)w.overflows / r, w.gpIdle, w.renders,
           waited ? fp_tb_ms(w.gpBacklogTb) / (double)waited : 0.0, waited);
    if (w.elanFetch | w.elanBuild | w.elanWrote)
    {
      const u64 rest = w.elanVerts - w.elanFetch - w.elanBuild - w.elanWrote;
      fp_out("[HW] ELAN vertices a render: %.0f from cached lists (GPU-read, %.0f of them from patches, but %.0f"
             " CPU-fed over frames apart), %.0f in lists cached now, %.0f CPU-fed (pages written since the last"
             " render), %.0f CPU-fed (colour, other) | list table restarts %u | programs built %u (page gaps %u"
             " of %u boundaries), patches %u, ring passes %u\n",
             (double)w.elanFetch / r, (double)w.elanPatch / r, (double)w.elanGap / r, (double)w.elanBuild / r,
             (double)w.elanWrote / r, (double)rest / r, g_fp_elan_resets - s_elan_resets0,
             g_fp_elan_progs - s_elan_progs0, g_fp_elan_gaps - s_elan_gaps0, g_fp_elan_bounds - s_elan_bounds0,
             g_fp_elan_patches - s_elan_patches0, g_fp_elan_wraps - s_elan_wraps0);
      s_elan_resets0 = g_fp_elan_resets;
      s_elan_progs0 = g_fp_elan_progs;
      s_elan_patches0 = g_fp_elan_patches;
      s_elan_wraps0 = g_fp_elan_wraps;
      s_elan_bounds0 = g_fp_elan_bounds;
      s_elan_gaps0 = g_fp_elan_gaps;
    }
    if (w.elanItems)
    {
      // Where the ELAN draw time goes: sending cached programs' indices
      // (what GX display lists would take off the CPU) against everything
      // else per list (matrices, lights, colours, program lookup and page
      // checks, CPU-fed vertices).
      const double progMs = fp_tb_ms(w.elanProgTb) / r;
      fp_out("[HW] ELAN draw a render: %.0f lists | cached programs %.2f ms (%.0f strips, %.0f vertices)"
             " | the rest %.2f ms (%.1f us a list: state, lookups, CPU-fed vertices)\n",
             (double)w.elanItems / r, progMs, (double)w.elanStrips / r, (double)w.elanFetch / r,
             fp_tb_ms(w.elanTb) / r - progMs,
             (fp_tb_ms(w.elanTb) - fp_tb_ms(w.elanProgTb)) * 1000.0 / (double)w.elanItems);
    }
    // The GPU fetch's page checks, done when the ELAN records its lists (so in
    // `elan`, not in the render above): full ones against PAGE GUARD's flags.
    const u32 pgChk = g_fp_elan_pgchk - s_pg_chk0, pgRo = g_fp_elan_pgro - s_pg_ro0;
    if (pgChk | pgRo)
    {
      const double pgMs = fp_tb_ms(g_fp_elan_pgtb - s_pg_tb0);
      fp_out("[HW] ELAN page checks a render: %.0f, %.3f ms (%.0f ns each) | full %.0f (%.0f written),"
             " read-only %.0f | made read-only %u, guards dropped %u\n",
             (double)(pgChk + pgRo) / r, pgMs / r, pgMs * 1e6 / (double)(pgChk + pgRo), (double)pgChk / r,
             (double)(g_fp_elan_pgdirty - s_pg_dirty0) / r, (double)pgRo / r, g_fp_elan_pgset - s_pg_set0,
             g_fp_elan_pgfault - s_pg_fault0);
    }
    s_pg_chk0 = g_fp_elan_pgchk;
    s_pg_dirty0 = g_fp_elan_pgdirty;
    s_pg_ro0 = g_fp_elan_pgro;
    s_pg_tb0 = g_fp_elan_pgtb;
    s_pg_set0 = g_fp_elan_pgset;
    s_pg_fault0 = g_fp_elan_pgfault;
  }
  else
    fp_out("[HW] 3D renders shown: 0 in %u frames\n", w.frames);

  u64 rest[4];
  for (int k = 0; k < 4; k++)
    rest[k] = w.frame[k] > w.rend[k] ? w.frame[k] - w.rend[k] : 0;
  const u64 restTb = w.frameTb > w.rendTb ? w.frameTb - w.rendTb : 0;

  if (s_pmc_set == 0)
  {
    // Cycles per second over the measured spans: ~729 M on a Wii when the
    // counters work (Dolphin leaves most of them at 0).
    const double fsec = fp_tb_ms(w.frameTb) / 1000.0;
    fp_out("[HW] CPU counters A, %u frames (%.0f M cycles/s): whole frame IPC %.2f, L1 load-miss cycles %.0f%%,"
           " DTLB walk cycles %.0f%% | 3D renders: IPC %.2f, load-miss %.0f%%, DTLB %.0f%%"
           " | rest of the frame: IPC %.2f, load-miss %.0f%%, DTLB %.0f%%\n",
           w.frames, fsec > 0.0 ? (double)w.frame[0] / fsec / 1e6 : 0.0,
           fp_ratio(w.frame[1], w.frame[0]), 100.0 * fp_ratio(w.frame[2], w.frame[0]),
           100.0 * fp_ratio(w.frame[3], w.frame[0]),
           fp_ratio(w.rend[1], w.rend[0]), 100.0 * fp_ratio(w.rend[2], w.rend[0]),
           100.0 * fp_ratio(w.rend[3], w.rend[0]),
           fp_ratio(rest[1], rest[0]), 100.0 * fp_ratio(rest[2], rest[0]),
           100.0 * fp_ratio(rest[3], rest[0]));
  }
  else
  {
    // Per millisecond of each span, from the time base (PMC1 counts L2 hits here).
    const double rms = fp_tb_ms(w.rendTb), xms = fp_tb_ms(restTb);
    fp_out("[HW] CPU counters B, %u frames, per ms: rest of the frame: L2 hits %.0f, L2 data misses %.0f,"
           " L1 instruction misses %.0f, mispredicted branches %.0f | 3D renders: L2 hits %.0f,"
           " L2 data misses %.0f (%.2f per ELAN vertex), L1 instruction misses %.0f, mispredicted branches %.0f\n",
           w.frames,
           xms > 0 ? rest[0] / xms : 0.0, xms > 0 ? rest[2] / xms : 0.0,
           xms > 0 ? rest[1] / xms : 0.0, xms > 0 ? rest[3] / xms : 0.0,
           rms > 0 ? w.rend[0] / rms : 0.0, rms > 0 ? w.rend[2] / rms : 0.0,
           fp_ratio(w.rend[2], w.elanVerts),
           rms > 0 ? w.rend[1] / rms : 0.0, rms > 0 ? w.rend[3] / rms : 0.0);
  }

  // Next window counts the other set. Called between vblank and resume, so
  // no frame or render span is open.
  memset(&s_hw, 0, sizeof(s_hw));
  fp_pmc_program(s_pmc_set ^ 1);
}
#else
void frame_prof_render_begin() {}
void frame_prof_render_end() {}
void frame_prof_gp_done() {}
#endif

static bool fp_alloc()
{
  if (s_ring)         return true;
  if (s_alloc_failed) return false;

  s_ring   = (FpSample*)malloc(sizeof(FpSample) * FP_RING);
  s_sorted = (u32*)malloc(sizeof(u32) * FP_RING);
  if (!s_ring || !s_sorted)
  {
    free(s_ring);   s_ring   = 0;
    free(s_sorted); s_sorted = 0;
    s_alloc_failed = true;
    fp_out("[FPROF] out of memory (%u KB), disabled\n",
           (u32)((sizeof(FpSample) + 4) * FP_RING / 1024));
    fflush(stdout);
    return false;
  }
  memset(s_ring, 0, sizeof(FpSample) * FP_RING);
  s_head = s_count = 0;
  return true;
}

// Monotonic accumulator delta that survives SPG.cpp's once-a-second zeroing.
static INLINE u32 fp_delta(u32 cur, u32 *last)
{
  u32 d = (cur < *last) ? cur : (cur - *last);
  *last = cur;
  return d;
}

// ---------------------------------------------------------------------------
// Close the frame. Called at the TOP of the emulated vblank handler, before
// the stats block and before spg_PaceFrame(), so that neither the speed
// limiter's sleep nor the log write lands inside a measurement.
// ---------------------------------------------------------------------------
void frame_prof_vblank()
{
  if (!FRAME_PROF_ON())
  {
    s_have_resume = false;   // a later enable starts a clean span
    return;
  }
  if (!fp_alloc())
    return;

#if HOST_OS == OS_WII
  if (!s_vi_hooked)
  {
    s_vi_prev   = VIDEO_SetPostRetraceCallback(fp_vi_cb);
    s_vi_hooked = true;
    s_vi_last   = s_vi_ticks;
  }
#endif

  const u32 now = FP_TICKS32();

#if HOST_OS == OS_WII
  if (s_frame_open)
  {
    FpPmc p;
    fp_pmc_read(p);
    for (int k = 0; k < 4; k++)
      s_hw.frame[k] += p.c[k] - s_pmc_frame0.c[k];
    s_hw.frameTb += now - s_resume;
    s_hw.frames++;
    s_frame_open = false;
  }
  fp_gp_poll();
#endif

  // Deltas of the shared accumulators. Taken unconditionally so that `last`
  // tracks them even on a frame that is thrown away below.
  const u32 d_rend = fp_delta((u32)RenderTicks,  &s_last_rend);
  const u32 d_ta   = fp_delta((u32)TaPerf.ticks, &s_last_ta);
  const u32 d_arm  = fp_delta((u32)ArmTicks,     &s_last_arm);
  const u32 d_aica = fp_delta((u32)AicaTicks,    &s_last_aica);
  const u32 d_snd  = fp_delta((u32)SndWaitTicks, &s_last_snd);
  const u32 d_cc   = fp_delta(fp_cc_total(),     &s_last_cc);

#if HOST_OS == OS_WII
  const u32 vi_now = s_vi_ticks;
#else
  const u32 vi_now = 0;
#endif
  const u32 d_vi = vi_now - s_vi_last;
  s_vi_last = vi_now;

  if (!s_have_resume)
  {
    // First frame after the preset was switched on: there is no opening
    // timestamp to measure against, and the accumulator deltas above cover an
    // unknown span. Drop it rather than log a fabricated outlier.
    memset(&g_fp, 0, sizeof(g_fp));
    return;
  }

  const u32 total = now - s_resume;

  FpSample *s = &s_ring[s_head];
  s->seq   = s_seq++;
  s->total = total;

  // AicaTicks brackets the synthesis loop INCLUDING the pacing sleep inside it,
  // so net that out the way the stats line does; and DoRender's own GP waits
  // and texture decodes come out of rend so the columns stay disjoint.
  const u32 aica_net = (d_aica > d_snd) ? (d_aica - d_snd) : 0;
  const u32 rend_in  = g_fp.tex + g_fp.gxw_rend;
  const u32 rend_net = (d_rend > rend_in) ? (d_rend - rend_in) : 0;

  s->b[FPB_REND] = rend_net;
  s->b[FPB_TEX]  = g_fp.tex;
  s->b[FPB_GXW]  = g_fp.gxw;
  s->b[FPB_TA]   = d_ta;
  s->b[FPB_ARM]  = d_arm;
  s->b[FPB_AICA] = aica_net;
  s->b[FPB_SND]  = d_snd;
  s->b[FPB_JIT]  = g_fp.jit;
  s->b[FPB_DISC] = g_fp.disc;
  s->b[FPB_MEM]  = g_fp.mem;
  s->b[FPB_PAD]  = g_fp.pad;
  s->b[FPB_CART] = g_fp.cart;

  // sh4 is the residual, so a bracket that double-counts shows up as a
  // shrinking sh4 rather than as a frame time that does not add up. Clamped at
  // zero: with TA PROFILE off, ta reads 0 and its time is simply part of sh4.
  u32 acc = 0;
  for (int i = 1; i < FP_NB; i++) acc += s->b[i];
  s->b[FPB_SH4] = (total > acc) ? (total - acc) : 0;

  s->n_tex   = (u16)(g_fp.n_tex   > 65535u ? 65535u : g_fp.n_tex);
  s->n_jit   = (u16)(g_fp.n_jit   > 65535u ? 65535u : g_fp.n_jit);
  s->n_clear = (u16)(g_fp.n_clear > 65535u ? 65535u : g_fp.n_clear);
  s->n_disc  = (u16)(g_fp.n_disc  > 65535u ? 65535u : g_fp.n_disc);
  s->n_wrap  = (u16)(g_fp.n_wrap  > 65535u ? 65535u : g_fp.n_wrap);
  s->n_pad   = (u16)(g_fp.n_pad   > 65535u ? 65535u : g_fp.n_pad);
  s->n_mem   = (u16)(g_fp.n_mem   > 65535u ? 65535u : g_fp.n_mem);
  s->n_cc    = d_cc;
  s->vi      = (u8)(d_vi > 255u ? 255u : d_vi);
  s->drawn   = (u8)(d_rend != 0);
  s->clear_reasons = (u8)g_fp.clear_reasons;

  memset(&g_fp, 0, sizeof(g_fp));

  s_head = (s_head + 1) % FP_RING;
  if (s_count < FP_RING) s_count++;
}

// Reopen the span. Called at the BOTTOM of the vblank handler, after the stats
// block and after spg_PaceFrame().
void frame_prof_resume()
{
  if (!FRAME_PROF_ON()) return;
  s_resume      = FP_TICKS32();
  s_have_resume = true;
#if HOST_OS == OS_WII
  if (s_pmc_set < 0)
    fp_pmc_program(0);
  fp_pmc_read(s_pmc_frame0);
  s_frame_open = true;
#endif
}

// Shell sort, Ciura gaps. 512 u32 once every couple of seconds, and it runs
// inside the stats block, which is already outside the measured span.
static void fp_sort(u32 *a, u32 n)
{
  static const u32 gaps[] = { 301, 132, 57, 23, 10, 4, 1 };
  for (u32 g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
  {
    const u32 gap = gaps[g];
    if (gap >= n) continue;
    for (u32 i = gap; i < n; i++)
    {
      const u32 v = a[i];
      u32 j = i;
      while (j >= gap && a[j - gap] > v) { a[j] = a[j - gap]; j -= gap; }
      a[j] = v;
    }
  }
}

static INLINE u32 fp_pct(const u32 *sorted, u32 n, double p)
{
  u32 i = (u32)((double)(n - 1) * p + 0.5);
  if (i >= n) i = n - 1;
  return sorted[i];
}

static void fp_print_row(const char *label, const double *ms, int n)
{
  fp_out("[FPROF] %s", label);
  for (int i = 0; i < FP_NB; i++)
    fp_out(" %s=%.2f", kFpName[i], ms[i]);
  fp_out("   (n=%d)\n", n);
}

void frame_prof_dump(double secs)
{
  (void)secs;

  if (!FRAME_PROF_ON() || !s_ring || s_count < 16)
    return;

  static int s_tick = 0;
  if (++s_tick < FP_DUMP_EVERY) return;
  s_tick = 0;

  const u32 n = s_count;

  for (u32 i = 0; i < n; i++) s_sorted[i] = s_ring[i].total;
  fp_sort(s_sorted, n);

  const u32 p50 = fp_pct(s_sorted, n, 0.50);
  const u32 p90 = fp_pct(s_sorted, n, 0.90);
  const u32 p95 = fp_pct(s_sorted, n, 0.95);
  const u32 p99 = fp_pct(s_sorted, n, 0.99);
  const u32 pmx = s_sorted[n - 1];

  // Budget = one emulated video frame. Anything past it is a frame the console
  // could not have delivered on time, whatever the one-second average says.
  const double budget_ms = (spg_PaceFrameSec > 0.0) ? spg_PaceFrameSec * 1000.0 : 16.68;
#if HOST_OS == OS_WII
  const u32 budget_ticks = (u32)PPCUsToTicks((u64)(budget_ms * 1000.0));
#else
  const u32 budget_ticks = 0xFFFFFFFFu;
#endif

  // One pass for the aggregates: typical frames (<= p50), tail frames (>= p95),
  // the over-budget count, the skip rate and the VI cadence histogram.
  double base[FP_NB], tail[FP_NB];
  for (int k = 0; k < FP_NB; k++) { base[k] = 0.0; tail[k] = 0.0; }

  u32 nbase = 0, ntail = 0, nover = 0, ndrawn = 0;
  double cc_base = 0.0, cc_tail = 0.0;
  u32 vi0 = 0, vi1 = 0, vi2 = 0, vi3 = 0;
  const FpSample *worst = &s_ring[0];
  double win_ms = 0.0;

  for (u32 i = 0; i < n; i++)
  {
    const FpSample *s = &s_ring[i];
    win_ms += FP_MS(s->total);
    if (s->total > budget_ticks) nover++;
    if (s->drawn)                ndrawn++;
    if      (s->vi == 0) vi0++;
    else if (s->vi == 1) vi1++;
    else if (s->vi == 2) vi2++;
    else                 vi3++;
    if (s->total > worst->total) worst = s;

    if (s->total <= p50)
    {
      for (int k = 0; k < FP_NB; k++) base[k] += FP_MS(s->b[k]);
      cc_base += s->n_cc;
      nbase++;
    }
    if (s->total >= p95)
    {
      for (int k = 0; k < FP_NB; k++) tail[k] += FP_MS(s->b[k]);
      cc_tail += s->n_cc;
      ntail++;
    }
  }

  if (nbase) for (int k = 0; k < FP_NB; k++) base[k] /= (double)nbase;
  if (ntail) for (int k = 0; k < FP_NB; k++) tail[k] /= (double)ntail;

  fp_out("[FPROF] win=%uf/%.1fs budget=%.2fms | p50=%.2f p90=%.2f p95=%.2f p99=%.2f max=%.2f ms"
         " | over %u (%.1f%%) | drawn %.0f%% | VI 0x:%u 1x:%u 2x:%u 3+:%u\n",
         n, win_ms / 1000.0, budget_ms,
         FP_MS(p50), FP_MS(p90), FP_MS(p95), FP_MS(p99), FP_MS(pmx),
         nover, 100.0 * (double)nover / (double)n,
         100.0 * (double)ndrawn / (double)n,
         vi0, vi1, vi2, vi3);

  fp_print_row("typical(<=p50):", base, (int)nbase);
  fp_print_row("tail   (>=p95):", tail, (int)ntail);
  if (g_jit_ccalls_preset)
    fp_out("[FPROF] C call-outs a frame: typical %.0f, tail %.0f, worst frame %u\n",
           nbase ? cc_base / nbase : 0.0, ntail ? cc_tail / ntail : 0.0, worst->n_cc);

  // The answer line. Rank the per-bucket difference between a tail frame and a
  // typical one: whatever is at the front of this list IS the stutter.
  double exc[FP_NB];
  double exc_sum = 0.0;
  for (int k = 0; k < FP_NB; k++)
  {
    exc[k] = tail[k] - base[k];
    if (exc[k] > 0.0) exc_sum += exc[k];
  }

  fp_out("[FPROF] EXCESS in tail:");
  for (int rank = 0; rank < 5; rank++)
  {
    int best = -1;
    for (int k = 0; k < FP_NB; k++)
      if (exc[k] > 0.005 && (best < 0 || exc[k] > exc[best])) best = k;
    if (best < 0) break;
    fp_out(" %s +%.2fms(%.0f%%)", kFpName[best], exc[best],
           exc_sum > 0.0 ? 100.0 * exc[best] / exc_sum : 0.0);
    exc[best] = -1.0;
  }
  fp_out("  => +%.2fms over a typical frame\n", exc_sum);

  char worst_clr[64];
  fp_clr_str(worst_clr, sizeof(worst_clr), worst->clear_reasons);
  fp_out("[FPROF] worst f%u %.2fms:", worst->seq, FP_MS(worst->total));
  for (int k = 0; k < FP_NB; k++)
    fp_out(" %s=%.2f", kFpName[k], FP_MS(worst->b[k]));
  fp_out(" | dec=%u blk=%u clr=%u[%s] rd=%u wrap=%u pad=%u pg=%u cc=%u vi=%u%s\n",
         (u32)worst->n_tex, (u32)worst->n_jit, (u32)worst->n_clear, worst_clr,
         (u32)worst->n_disc, (u32)worst->n_wrap, (u32)worst->n_pad, (u32)worst->n_mem,
         worst->n_cc, (u32)worst->vi,
         worst->drawn ? "" : " SKIPPED");

  // Mode 2: the individual tail frames. The aggregate above says which bucket
  // grew; this says whether it grew on one catastrophic frame or on a dozen
  // mediocre ones, which is the difference between a load hitch and a steady
  // leak. Walked newest-first so the listing is bounded and recent.
  if (g_frame_prof_preset >= 2)
  {
    int listed = 0;
    for (u32 j = 0; j < n && listed < FP_TAIL_LIST; j++)
    {
      const FpSample *s = &s_ring[(s_head + FP_RING - 1 - j) % FP_RING];
      if (s->total < p95) continue;
      char tail_clr[64];
      fp_clr_str(tail_clr, sizeof(tail_clr), s->clear_reasons);
      fp_out("[FPROF]  f%u %.2fms:", s->seq, FP_MS(s->total));
      for (int k = 0; k < FP_NB; k++)
        if (s->b[k]) fp_out(" %s=%.2f", kFpName[k], FP_MS(s->b[k]));
      fp_out(" | dec=%u blk=%u clr=%u[%s] rd=%u wrap=%u pad=%u pg=%u cc=%u vi=%u%s\n",
             (u32)s->n_tex, (u32)s->n_jit, (u32)s->n_clear, tail_clr,
             (u32)s->n_disc, (u32)s->n_wrap, (u32)s->n_pad, (u32)s->n_mem, s->n_cc, (u32)s->vi,
             s->drawn ? "" : " SKIPPED");
      listed++;
    }
  }

#if HOST_OS == OS_WII
  fp_hw_dump();
#endif

  fflush(stdout); // log is freopen'd to SD; without this the tail is lost
}
