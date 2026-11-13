/*
 * SH4 Interpreter — Dreamcast / Wii port
 *
 * Runtime accuracy preset (get_accuracy_int_preset()):
 *   0 = Fast     — timeslice 1792, loose peripheral polling
 *   1 = Balanced — timeslice  896, moderate peripheral polling
 *   2 = Accurate — timeslice  448, original peripheral polling (default)
 *
 * All timing-sensitive values (timeslice, CPU_RATIO, cascade thresholds)
 * are selected at the start of Sh4_int_Run() so a preset change takes
 * effect on the next Run() call without any restart required.
 */

#include "types.h"

#include "sh4_interpreter.h"
#include "dc/sh4/rec_v2/ccall_census.h"
#include "sh4_opcode_list.h"
#include "sh4_registers.h"
#include "sh4_if.h"
#include "dc/pvr/pvr_if.h"
#include "dc/aica/aica_if.h"
#include "plugs/vbaARM/arm_aica.h"  // ARM7 CPU tick (vbaARM)
#include "dmac.h"
#include "dc/mem/sb.h"   // SB_C2DSTAT/SB_C2DLEN/SB_C2DST for the [DMA] probe
#include "dc/gdrom/gdrom_if.h"
#include "dc/maple/maple_if.h"
#include "intc.h"
#include "tmu.h"
#include "dc/mem/sh4_mem.h"
#include "ccn.h"
#include "sh4_sched.h"   // SCHED preset: unified cycle-deadline event scheduler
#include "scif.h"
#if defined(BUILD_NAOMI2)
#include "naomi/naomi_elan.h"
#include "naomi/naomi.h"
#endif

// SCHED preset accessor (wii/main.cpp). When on, migrated completion/IRQ events
// fire through sh4_sched (deadline-ordered) instead of at their own cascade tier.
extern "C" int get_sched_preset();
// #include <gccore.h>  // Uncomment for Wii VIDEO_WaitVSync() frame limiter
#include <time.h>
#include <float.h>

// -------------------------------------------------------------------------
// Opcode field helpers
// -------------------------------------------------------------------------
#define GetN(op) (((op) >> 8) & 0xf)
#define GetM(op) (((op) >> 4) & 0xf)

// -------------------------------------------------------------------------
// Run flag — volatile: may be cleared from an interrupt/other thread on Wii
// -------------------------------------------------------------------------
volatile bool sh4_int_bCpuRun = false;

#if HOST_OS != OS_WII
// Debug hook (Initial D3 port, host builds only): called before every
// interpreted instruction when set. Used by the bring-up runner's --watch.
void (*g_sh4_step_hook)(u32 pc, u32 op) = nullptr;
// Called before an instruction is fetched: may change next_pc (runner's --reflection).
void (*g_sh4_pc_hook)() = nullptr;
// Instructions a pc hook stepped over (the runner's GAME COPIES): charged as if run.
u32 g_sh4_hook_cycles = 0;
// Cycles charged per interpreted instruction when > 0 (runner's --cpi).
s32 g_sh4_cpu_ratio_override = 0;
#endif

// -------------------------------------------------------------------------
// Sin / cos lookup table
//   [0x00000 .. 0x0FFFF] — full circle (sin values)
//   [0x10000 .. 0x13FFF] — wraparound copy to avoid a branch on cos lookup
// -------------------------------------------------------------------------
f32 sin_table[0x10000 + 0x4000];

// -------------------------------------------------------------------------
// Active timing parameters — set at the top of Sh4_int_Run() from preset
// -------------------------------------------------------------------------
// SH-4 TIMESLICE (launch page): 1 = with the FAST preset, twice its timeslice
// and the tiers half as many slices apart (see ApplyAccuracyPreset).
extern "C" {
int g_sh4_wide_slice_preset = 0;
}

static s32 s_timeslice     = SH4_TIMESLICE_ACCURATE;
static s32 s_cpu_ratio     = SH4_CPU_RATIO_ACCURATE;
static u32 s_medium_period = SH4_MEDIUM_ACCURATE;
static u32 s_slow_period   = SH4_SLOW_ACCURATE;
static u32 s_vslow_period  = SH4_VSLOW_ACCURATE;

// Selects all timing parameters from the current accuracy preset.
// Called once at the start of each Run() so changes take effect immediately.
static void ApplyAccuracyPreset()
{
	const int preset = get_accuracy_int_preset();

	if (preset == 0)  // Fast
	{
		s_timeslice     = SH4_TIMESLICE_FAST;
		s_cpu_ratio     = SH4_CPU_RATIO_FAST;
		s_medium_period = SH4_MEDIUM_FAST;
		s_slow_period   = SH4_SLOW_FAST;
		s_vslow_period  = SH4_VSLOW_FAST;
		// SH-4 TIMESLICE = 3584: half the UpdateSystem calls (~1870 a frame,
		// each a C call from the recompiled code and a dispatch after it). The
		// tiers come half as many slices apart, so sound, DMA, the drive and
		// Maple keep their cadence in cycles (7168 / 14336 / 28672); TMU, the
		// SPG and pending interrupts are seen every 3584 cycles (18 us).
		if (g_sh4_wide_slice_preset)
		{
			s_timeslice     = SH4_TIMESLICE_FAST * 2;
			s_medium_period = SH4_MEDIUM_FAST / 2;
			s_slow_period   = SH4_SLOW_FAST / 2;
			s_vslow_period  = SH4_VSLOW_FAST / 2;
		}
		printf("Sh4: accuracy preset = FAST (timeslice %d)\n", s_timeslice);
	}
	else if (preset == 1)  // Balanced
	{
		s_timeslice     = SH4_TIMESLICE_BALANCED;
		s_cpu_ratio     = SH4_CPU_RATIO_BALANCED;
		s_medium_period = SH4_MEDIUM_BALANCED;
		s_slow_period   = SH4_SLOW_BALANCED;
		s_vslow_period  = SH4_VSLOW_BALANCED;
		printf("Sh4: accuracy preset = BALANCED (timeslice %d)\n", s_timeslice);
	}
	else  // Accurate (default)
	{
		s_timeslice     = SH4_TIMESLICE_ACCURATE;
		s_cpu_ratio     = SH4_CPU_RATIO_ACCURATE;
		s_medium_period = SH4_MEDIUM_ACCURATE;
		s_slow_period   = SH4_SLOW_ACCURATE;
		s_vslow_period  = SH4_VSLOW_ACCURATE;
		printf("Sh4: accuracy preset = ACCURATE (timeslice %d)\n", s_timeslice);
	}
#if HOST_OS != OS_WII
	// Host builds can afford a realistic cycles-per-instruction charge (the
	// SH-4 retires ~1-2 instructions per cycle; 8 is a Wii speed trade-off).
	if (g_sh4_cpu_ratio_override > 0)
	{
		s_cpu_ratio = g_sh4_cpu_ratio_override;
		printf("Sh4: cycles per instruction = %d\n", s_cpu_ratio);
	}
#endif
}

// Public wrappers so the recompiler can latch the same preset values.
// The JIT mainloop is emitted ONCE (first run) with the timeslice baked in,
// so the preset must be applied before ngen_mainloop() emits it — see
// recSh4_Run() in rec_v2/driver.cpp.
void Sh4_ApplyAccuracyPreset()
{
	ApplyAccuracyPreset();
}

s32 sh4_GetTimeslice()
{
	return s_timeslice;
}

// -------------------------------------------------------------------------
// Sh4_int_Run
// -------------------------------------------------------------------------
void Sh4_int_Run()
{
	// Latch timing parameters for this run session
	ApplyAccuracyPreset();

	sh4_int_bCpuRun = true;

	s32 l = s_timeslice;

	do
	{
		// Inner loop: dispatch one timeslice worth of opcodes
		do
		{
#if HOST_OS != OS_WII
			if (g_sh4_pc_hook)
			{
				g_sh4_pc_hook();
				if (g_sh4_hook_cycles)
				{
					l -= (s32)(g_sh4_hook_cycles * s_cpu_ratio);
					g_sh4_hook_cycles = 0;
				}
			}
#endif
			const u32 op = ReadMem16(next_pc);
#if HOST_OS != OS_WII
			if (g_sh4_step_hook)
				g_sh4_step_hook(next_pc, op);
#endif
#if defined(BUILD_NAOMI2)
			const u32 at = next_pc;
#endif
			next_pc += 2;

			OpPtr[op](op);
#if defined(BUILD_NAOMI2)
			// BIOS ROM is never run by the game: catch the jump that gets there.
			if (!(next_pc & 0x1FE00000) && (at & 0x1FE00000))
				naomi_ReportBiosEntry(next_pc, at);
#endif
			l -= s_cpu_ratio;
		} while (l > 0);

		l += s_timeslice;

		// Peripheral update cascade (TMU, PVR, AICA, DMA, GDRom, Maple …)
		UpdateSystem();

		// ----------------------------------------------------------------
		// Wii frame limiter — uncomment to lock to 60 Hz vsync.
		// VIDEO_WaitVSync() must sit OUTSIDE the inner opcode loop
		// (already the case here) so we yield rather than spin-wait.
		//
		// #include <gccore.h>
		// VIDEO_WaitVSync();
		// ----------------------------------------------------------------

	} while (sh4_int_bCpuRun);

	sh4_int_bCpuRun = false;
}

// -------------------------------------------------------------------------
// Sh4_int_Stop
// -------------------------------------------------------------------------
void Sh4_int_Stop()
{
	if (sh4_int_bCpuRun)
		sh4_int_bCpuRun = false;
}

// -------------------------------------------------------------------------
// Sh4_int_Step  (debugger)
// -------------------------------------------------------------------------
void Sh4_int_Step()
{
	if (sh4_int_bCpuRun)
	{
		printf("Sh4 Is running, can't step\n");
		return;
	}

	const u32 op = ReadMem16(next_pc);
	next_pc += 2;
	ExecuteOpcode(op);
}

// -------------------------------------------------------------------------
// Sh4_int_Skip  (debugger)
// -------------------------------------------------------------------------
void Sh4_int_Skip()
{
	if (sh4_int_bCpuRun)
	{
		printf("Sh4 Is running, can't Skip\n");
		return;
	}

	next_pc += 2;
}

// -------------------------------------------------------------------------
// Sh4_int_Reset
// -------------------------------------------------------------------------
void Sh4_int_Reset(bool /*Manual*/)
{
	if (sh4_int_bCpuRun)
	{
		printf("Sh4 Is running, can't Reset\n");
		return;
	}

	next_pc = 0xA0000000u;

	// Integer registers
	memset(r,      0, sizeof(r));
	memset(r_bank, 0, sizeof(r_bank));

	// Control / system registers
	gbr = ssr = spc = sgr = dbr = vbr = 0;
	mach = macl = pr = fpul = 0;

	sr.SetFull(0x700000F0u);
	old_sr = sr;
	UpdateSR();

	// FPU registers
	// NOTE: xf[] (shadow FP bank) intentionally NOT cleared here.
	// The BIOS initializes xf[] before game code runs. Clearing it
	// corrupts the matrix/vector state and causes a black frame on boot.
	memset(fr, 0, sizeof(fr));

	fpscr.full = 0x00040001u;
	old_fpscr  = fpscr;
	UpdateFPSCR();

	// SCHED preset: drop any in-flight deadline requests and zero the scheduler
	// clock so a reset never fires a stale completion into fresh state. Harmless
	// (and cheap) when the preset is off — the list is empty until events
	// register, and nothing ticks it.
	sh4_sched_reset();

	printf("Sh4 Reset\n");
}

// -------------------------------------------------------------------------
// GenerateSinCos
// -------------------------------------------------------------------------
void GenerateSinCos()
{
	printf("Generating sincos tables ...\n");

	char* path = GetEmuPath("data/fsca-table.bin");
	FILE* tbl  = fopen(path, "rb");
	free(path);

	if (tbl)
	{
		fread(sin_table, sizeof(f32), 0x8000, tbl);
		fclose(tbl);

		// Endian-swap the loaded half (no-op on big-endian / Wii)
		for (int i = 0; i < 0x8000; i++)
			(u32&)sin_table[i] = host_to_le<4>((u32&)sin_table[i]);
	}
	else
	{
		// Initial D3 port: built-in copy of data/fsca-table.bin
		// (naomi/fsca_table.cpp), already in host order.
		extern const u32 g_fsca_table[0x8000];
		for (int i = 0; i < 0x8000; i++)
			(u32&)sin_table[i] = g_fsca_table[i];
	}

	// Mirror: second half of the unit circle (sin is antisymmetric)
	sin_table[0x8000] = 0.0f;                    // sin(π) == 0 exactly
	for (int i = 0x8001; i < 0x10000; i++)
		sin_table[i] = -sin_table[i - 0x8000];

	// Overlap region for cos: copy [0..0x3FFF] → [0x10000..0x13FFF]
	for (int i = 0x10000; i < 0x14000; i++)
		sin_table[i] = sin_table[i & 0xFFFF];
}

// -------------------------------------------------------------------------
// Init / Term
// -------------------------------------------------------------------------
void Sh4_int_Init()
{
	BuildOpcodeTables();
	GenerateSinCos();
}

void Sh4_int_Term()
{
	Sh4_int_Stop();
	printf("Sh4 Term\n");
}

bool Sh4_int_IsCpuRunning()
{
	return sh4_int_bCpuRun;
}

// -------------------------------------------------------------------------
// Delay-slot helpers
// -------------------------------------------------------------------------
void ExecuteDelayslot()
{
	const u32 op = IReadMem16(next_pc);
	next_pc += 2;
	if (op != 0)
		ExecuteOpcode(op);
}

void ExecuteDelayslot_RTE()
{
	// Restore the whole SR from SSR, T included (rec_v2's rte writes the full
	// SR too). Copying only the status bits left T as the exception handler
	// had it, so an interrupt between a compare and its branch flipped the
	// branch (Initial D3's sound driver upload stopped early).
	// UpdateSR() is called by the RTE opcode handler after this returns.
	sr.SetFull(ssr);
	ExecuteDelayslot();
}

// -------------------------------------------------------------------------
// Update cascade
//
//  UpdateSystem()   — every s_timeslice SH4 cycles  (called from run loop)
//  MediumUpdate()   — every s_medium_period calls   (AICA, DMA)
//  SlowUpdate()     — every s_slow_period calls     (GDRom)
//  VerySlowUpdate() — every s_vslow_period calls    (Maple, RTC)
//
//  The period masks use the latched s_* values so they scale automatically
//  with the active accuracy preset.
// -------------------------------------------------------------------------
s32 rtc_cycles  = 0;
u32 update_cnt  = 0;
u32 gcp_timer   = 0;

void FASTCALL VerySlowUpdate()
{
	gcp_timer++;
	rtc_cycles -= (s_timeslice * (s32)s_vslow_period);
	if (rtc_cycles <= 0)
	{
		rtc_cycles += SH4_CLOCK_EFF; // effective clock: 1 RTC second per emulated second, stays synced to video under underclock
		settings.dreamcast.RTC++;
	}
	maple_Update(s_timeslice * s_vslow_period);
}

void FASTCALL SlowUpdate()
{
	// GDROM_DELAY: this tier cadence IS the drive clock. Credit the elapsed
	// cycles before the transfer, so the budget it spends matches emulated time.
	gdrom_CreditCycles(s_timeslice * s_slow_period);
	UpdateGDRom();

	if (!(update_cnt & (s_vslow_period - 1)))
		VerySlowUpdate();
}

void FASTCALL MediumUpdate()
{
	UpdateAica(s_timeslice * s_medium_period);
	UpdateArm(s_timeslice * s_medium_period);  // ARM7 tick — same cycle count, arm_aica.cpp divides by arm_sh4_bias (8)
	UpdateDMA();
	UpdateSCIF(s_timeslice * s_medium_period);   // serial line (NAOMI card reader)
#if defined(BUILD_NAOMI2)
	naomi_ElanUpdate(s_timeslice * s_medium_period);   // ELAN DMA completion
	naomi_CheckPc();
#endif

	if (!(update_cnt & (s_slow_period - 1)))
		SlowUpdate();
}

int FASTCALL UpdateSystem()
{
	if (!(update_cnt & (s_medium_period - 1)))
		MediumUpdate();

	update_cnt++;

	UpdateTMU(s_timeslice);
	UpdatePvr(s_timeslice);
	UpdateAicaDma(s_timeslice);     // deferred AICA G2-DMA completion IRQ (DMA_FIX)

	// SCHED preset: advance the deadline scheduler and fire due completion/IRQ
	// events (Stage 2 migrations) in true hardware order. Before UpdateINTC so a
	// fired completion's IRQ is picked up in this same timeslice. No-op when off
	// or when nothing is scheduled.
	if (get_sched_preset())
		sh4_sched_tick(s_timeslice);

	return UpdateINTC();
}

// -------------------------------------------------------------------------
// Split form of UpdateSystem() for the recompiler's static GPR allocation.
//
// The peripheral cascade (Medium/TMU/Pvr) and the interrupt-pending check
// touch NO SH4 GPRs, so the JIT can run them on its hot path WITHOUT flushing
// its pinned registers. UpdateSystem_no_event() does exactly that work and
// returns nonzero only when an interrupt must actually be dispatched.
//
// Only on that (rare) nonzero result does the JIT flush its GPRs, call
// UpdateSystem_handle_event() — which runs the GPR-touching Do_Interrupt /
// register-bank swap — and reload. This keeps the expensive flush/reload off
// the common per-timeslice path.
//
// Invariant: _no_event() must perform every side effect of UpdateSystem()
// EXCEPT the UpdateINTC() interrupt dispatch, and _handle_event() must perform
// exactly that dispatch — so calling _no_event() then (conditionally)
// _handle_event() is behaviourally identical to a single UpdateSystem().
// -------------------------------------------------------------------------
int FASTCALL UpdateSystem_no_event()
{
	CCALL(CC_TIMESLICE);
	if (!(update_cnt & (s_medium_period - 1)))
		MediumUpdate();

	update_cnt++;

	UpdateTMU(s_timeslice);
	UpdatePvr(s_timeslice);
	UpdateAicaDma(s_timeslice);     // deferred AICA G2-DMA completion IRQ (DMA_FIX)

	// SCHED preset: advance the deadline scheduler and fire due completion/IRQ
	// events (Stage 2 migrations) in true hardware order. Before the pending
	// check so a fired completion's IRQ is seen this timeslice. No-op when off.
	if (get_sched_preset())
		sh4_sched_tick(s_timeslice);

	return UpdateINTC_pending();
}

int FASTCALL UpdateSystem_handle_event()
{
	return UpdateINTC();
}

// -------------------------------------------------------------------------
// Cache reset stub (no cache emulation in the interpreter)
// -------------------------------------------------------------------------
void sh4_int_resetcache() { }

// -------------------------------------------------------------------------
// Get_Sh4Interpreter — fills the vtable used by the rest of the emulator
// -------------------------------------------------------------------------
void Get_Sh4Interpreter(sh4_if* rv)
{
	rv->Run          = Sh4_int_Run;
	rv->Stop         = Sh4_int_Stop;
	rv->Step         = Sh4_int_Step;
	rv->Skip         = Sh4_int_Skip;
	rv->Reset        = Sh4_int_Reset;
	rv->Init         = Sh4_int_Init;
	rv->Term         = Sh4_int_Term;
	rv->IsCpuRunning = Sh4_int_IsCpuRunning;
	rv->ResetCache   = sh4_int_resetcache;
}
