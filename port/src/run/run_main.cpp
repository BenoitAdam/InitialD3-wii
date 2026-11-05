// Headless NAOMI 2 runner (Windows bring-up tool).
//
//   idas3wii_run --chd <gds-0033.chd> [--frames N] [--log file]
//
// Opens the game exactly like the launcher (CHD + PIC + DIMM decryption),
// hands the decrypted image to the NAOMI 2 core, boots BIOS-free and runs N
// emulated frames with no video/audio output, printing what the game does.
// windows.h first: the core's sh4_registers.h defines one-letter macros
// (r, sr, pr...) that break the Windows headers.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#include "core/game_session.h"
#include "core/bios_image.h"
#include "core/log.h"
#include "platform/platform.h"

#include "naomi/naomi.h"
#include "naomi/naomi_frontend.h"
#include "naomi/naomi_elan.h"
#include "host/host_hooks.h"
#include "dc/sh4/sh4_registers.h"
#include "dc/sh4/sh4_opcode_list.h"
#include "dc/mem/sb.h"
#include "dc/mem/sh4_mem.h"
#include "dc/aica/aica_if.h"
#include "dc/pvr/pvr_if.h"

extern bool Arm7Enabled;
extern u32 g_dbg_ch2_count[8], g_dbg_ch2_bytes[8];
extern u32 g_dbg_sqw_count[8];
u32 arm_GetNextPC();
u32 arm_GetReg(int n);

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>


using namespace idas3;

#if defined(_WIN32)
// Bring-up aid: on a host crash, print the fault, a symbolised host stack and
// the guest PC instead of dying silently.
static LONG WINAPI crashHandler(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    printf("\n[crash] exception %08lX at %p", rec->ExceptionCode, rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        printf(" (%s %p)", rec->ExceptionInformation[0] ? "write" : "read", (void*)rec->ExceptionInformation[1]);
    printf("\n[crash] guest pc %08X pr %08X\n", next_pc, pr);

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; i++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
            break;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line = {sizeof(line)};
        DWORD ldisp = 0;
        const bool hasSym = SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym) != FALSE;
        const bool hasLine = SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &ldisp, &line) != FALSE;
        printf("[crash]   #%02d %s+0x%llx  %s:%lu\n", i, hasSym ? sym->Name : "?", (unsigned long long)disp,
               hasLine ? line.FileName : "", hasLine ? line.LineNumber : 0);
    }
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// --watch <addr>: report every change of a guest word, with the PCs that led
// to it (debug hook in the Windows interpreter).
extern void (*g_sh4_step_hook)(u32 pc, u32 op);
extern s32 g_sh4_cpu_ratio_override;   // --cpi: interpreter cycles per instruction
static u32 s_watchAddr;
static u32 s_watchLast;
static u32 s_watchHits;
static u32 s_pcRing[64];
static u32 s_srRing[64];
static u32 s_pcPos;

// --trace-pc <addr>: dump the registers each time the guest reaches addr.
static u32 s_tracePcs[8];
static u32 s_tracePcCount;
static u32 s_traceHits[8];   // per address: a tight loop must not use up the others' lines
static u32 s_traceFrom;      // --trace-from <addr>: arm the trace when reached
static u32 s_traceAtFrame;   // --trace-at <frame>: arm the trace at that frame
static bool s_traceArmed = true;

static void watchHook(u32 pc, u32)
{
    s_srRing[s_pcPos & 63] = sr.GetFull();
    s_pcRing[s_pcPos++ & 63] = pc;
    if (!s_traceArmed && pc == s_traceFrom)
        s_traceArmed = true;
    bool traced = false;
    for (u32 i = 0; i < s_tracePcCount; i++)
        if (pc == s_tracePcs[i] && s_traceArmed && s_traceHits[i] < 40)
        {
            s_traceHits[i]++;
            traced = true;
        }
    if (traced)
    {
        printf("[trace] pc %08X pr %08X ssr %08X spc %08X sr %08X\n        r0-7  %08X %08X %08X %08X %08X %08X %08X %08X\n"
               "        r8-15 %08X %08X %08X %08X %08X %08X %08X %08X\n",
               pc, pr, ssr, spc, sr.GetFull(), r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11],
               r[12], r[13], r[14], r[15]);
        for (u32 i = 0; i < 64; i++)
            printf("%s%08X/%X", (i % 8) ? " " : "\n        ", s_pcRing[(s_pcPos + i) & 63],
                   s_srRing[(s_pcPos + i) & 63] & 0xF3);
        printf("\n");
    }
    if (!s_watchAddr)
        return;
    const u32 v = ReadMem32(s_watchAddr);
    if (v == s_watchLast)
        return;
    printf("[watch] %08X: %08X -> %08X  (now at pc %08X pr %08X r15 %08X)\n      recent pcs:", s_watchAddr, s_watchLast, v,
           pc, pr, r[15]);
    for (u32 i = 0; i < 16; i++)
        printf(" %08X", s_pcRing[(s_pcPos + 48 + i) & 63]);
    printf("\n");
    s_watchLast = v;
    if (++s_watchHits >= 40)
    {
        printf("[watch] limit reached, watch disabled\n");
        g_sh4_step_hook = nullptr;
    }
}

// --area1-census: count the SH-4 stores to VRAM's 32-bit interface (area 1)
// by instruction address and width; every 600 frames the busiest are printed
// with their opcode, then the counts restart.
extern void (*g_area1_write_hook)(u32 addr, u32 size);
static void logLine(const char* text);
static std::map<u64, u32> s_area1;      // pc << 8 | size -> stores
static u32 s_area1Min = ~0u, s_area1Max = 0;

static void area1Hook(u32 addr, u32 size)
{
    const u32 pc = Sh4cntx.pc - 2;
    s_area1[(u64)pc << 8 | size]++;
    const u32 a = addr & 0x01FFFFFF;
    if (a < s_area1Min)
        s_area1Min = a;
    if (a > s_area1Max)
        s_area1Max = a;
}

static void area1Print(u32 frame)
{
    std::vector<std::pair<u32, u64>> top;
    u64 total = 0;
    for (const auto& e : s_area1)
    {
        top.push_back({e.second, e.first});
        total += e.second;
    }
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    char line[256];
    snprintf(line, sizeof(line), "[run]   area-1 stores, frames %u-%u: %.1f a frame, addresses %08X-%08X\n", frame - 600,
             frame, total / 600.0, s_area1Min, s_area1Max);
    logLine(line);
    for (size_t k = 0; k < top.size() && k < 6; k++)
    {
        const u32 pc = (u32)(top[k].second >> 8), size = (u32)(top[k].second & 0xFF);
        const u16 op = *(const u16*)&mem_b.data[pc & (RAM_SIZE - 1) & ~1u];
        snprintf(line, sizeof(line), "[run]     pc %08X op %04X: %u-bit, %.1f a frame\n", pc, op, size * 8,
                 top[k].first / 600.0);
        logLine(line);
    }
    s_area1.clear();
    s_area1Min = ~0u;
    s_area1Max = 0;
}

// --histo-from <frame>: from that frame on, count executed PCs (64-byte
// buckets) and print the hottest code at exit.
static u32 s_histoFrom;
static std::vector<u32> s_histo;     // indexed by (pc & 0x01FFFFFF) >> 6

static void histoHook(u32 pc, u32 op)
{
    const u32 bucket = (pc & 0x01FFFFFF) >> 6;
    if (bucket < s_histo.size())
        s_histo[bucket]++;
    if (s_watchAddr || s_tracePcCount)
        watchHook(pc, op);
}

static void printHisto()
{
    std::vector<std::pair<u32, u32>> hot;
    u64 total = 0;
    for (u32 i = 0; i < s_histo.size(); i++)
        if (s_histo[i])
        {
            hot.push_back({s_histo[i], i});
            total += s_histo[i];
        }
    std::sort(hot.begin(), hot.end(), [](auto& a, auto& b) { return a.first > b.first; });
    printf("[histo] %llu instructions sampled\n", (unsigned long long)total);
    // Every bucket, to compare two parts of the game.
    if (const char* path = getenv("IDAS3_HISTO_FILE"))
        if (FILE* f = fopen(path, "w"))
        {
            for (const auto& h : hot)
                fprintf(f, "%08X %u\n", 0x0C000000 | (h.second << 6), h.first);
            fclose(f);
        }
    u64 running = 0;
    for (size_t i = 0; i < hot.size() && i < 64; i++)
    {
        running += hot[i].first;
        printf("[histo]   %08X-%08X  %5.1f%%  (%5.1f%% so far)\n", 0x0C000000 | (hot[i].second << 6),
               0x0C000000 | ((hot[i].second << 6) + 63), 100.0 * hot[i].first / (double)total,
               100.0 * running / (double)total);
    }
}

// --arm-histo-from <frame>: from that frame on, the ARM7 sound CPU's ticks by
// 64-byte range of its code (the Windows interpreter, no idle skip), printed at
// exit: what its 1.3 ms a race frame on a Wii is spent on.
extern void (*g_arm_histo_hook)(u32 pc, u32 ticks);
static u32 s_armHistoFrom;
static std::vector<u64> s_armHisto;     // by (pc & 0x7FFFFF) >> 6
static u64 s_armHistoTicks;

static void armHistoHook(u32 pc, u32 ticks)
{
    const u32 bucket = (pc & 0x7FFFFF) >> 6;
    if (bucket < s_armHisto.size())
        s_armHisto[bucket] += ticks;
    s_armHistoTicks += ticks;
}

static void printArmHisto(u32 frames)
{
    std::vector<std::pair<u64, u32>> hot;
    for (u32 i = 0; i < s_armHisto.size(); i++)
        if (s_armHisto[i])
            hot.push_back({s_armHisto[i], i});
    std::sort(hot.begin(), hot.end(), [](auto& a, auto& b) { return a.first > b.first; });
    printf("[armhisto] %llu ticks over %u frames (%.0f a frame)\n", (unsigned long long)s_armHistoTicks, frames,
           frames ? s_armHistoTicks / (double)frames : 0.0);
    u64 running = 0;
    for (size_t i = 0; i < hot.size() && i < 40; i++)
    {
        running += hot[i].first;
        printf("[armhisto]   %06X-%06X  %5.1f%%  (%5.1f%% so far)\n", hot[i].second << 6, (hot[i].second << 6) + 63,
               100.0 * hot[i].first / (double)s_armHistoTicks, 100.0 * running / (double)s_armHistoTicks);
    }
}

// --reflection N: the launcher's REFLECTIONS, one reflection map build in N
// (default 1 here, the game's own behaviour; the launcher's is 4).
extern int g_reflection_rate_preset;
extern "C" int g_incar_reflect_preset;
extern "C" u32 g_reflection_incar_skips;
extern void (*g_sh4_pc_hook)();

static u32 s_reflectionStarts;          // builds started (row 0), made or not

static void reflectionHook()
{
    if ((next_pc & 0x1FFFFFFF) == 0x0C048E86)
        if (const u32 out = naomi_ReflectionExit(next_pc))
        {
            s_reflectionStarts += r[4] == 0;
            if (naomi_ReflectionSkip(r[4]))
                next_pc = out;
        }
}

// env IDAS3_SF_PROBE=LO:HI[:FROM] (hex range, decimal frame): each call into [LO, HI) from outside it, its
// arguments and result tried against native double arithmetic, to name the
// game's software floating-point routines. Printed at exit.
static u32 s_probeLo, s_probeHi, s_probePrev, s_probeFrom;
struct ProbeFrame
{
    u32 entry, ret, sp, a[4];
};
enum
{
    kHypDiv, kHypMul, kHypAdd, kHypSub, kHypRsub, kHypNeg, kHypI2D, kHypU2D, kHypD2I, kHypF2D, kHypD2F, kHypCmp,
    kHypCount
};
static const char* const kHypNames[kHypCount] = {"a/b", "a*b", "a+b", "a-b", "b-a", "-a", "(double)int",
                                                 "(double)uint", "(int)a", "(double)float", "(float)a", "cmp"};
struct ProbeStats
{
    u64 calls = 0;
    u32 hits[2][kHypCount] = {};      // [word order: r(n) low, r(n) high][hypothesis]
    u32 example[6] = {};
};
static std::vector<ProbeFrame> s_probeStack;
static std::map<u32, ProbeStats> s_probe;

static double pairDouble(u32 a, u32 b, int order)
{
    const u64 bits = order ? ((u64)a << 32 | b) : ((u64)b << 32 | a);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static bool sameDouble(double x, u32 lo, u32 hi, int order)
{
    const double y = pairDouble(lo, hi, order);
    return memcmp(&x, &y, 8) == 0 || (x != x && y != y);
}

static void probeResult(const ProbeFrame& f)
{
    ProbeStats& s = s_probe[f.entry];
    if (s.calls++ == 0)
    {
        memcpy(s.example, f.a, 16);
        s.example[4] = r[0];
        s.example[5] = r[1];
    }
    for (int o = 0; o < 2; o++)
    {
        const double a = pairDouble(f.a[0], f.a[1], o), b = pairDouble(f.a[2], f.a[3], o);
        u32* h = s.hits[o];
        h[kHypDiv] += sameDouble(a / b, r[0], r[1], o);
        h[kHypMul] += sameDouble(a * b, r[0], r[1], o);
        h[kHypAdd] += sameDouble(a + b, r[0], r[1], o);
        h[kHypSub] += sameDouble(a - b, r[0], r[1], o);
        h[kHypRsub] += sameDouble(b - a, r[0], r[1], o);
        h[kHypNeg] += sameDouble(-a, r[0], r[1], o);
        h[kHypI2D] += sameDouble((double)(s32)f.a[0], r[0], r[1], o);
        h[kHypU2D] += sameDouble((double)f.a[0], r[0], r[1], o);
        h[kHypD2I] += (a == a && a > -2147483649.0 && a < 2147483648.0 && (s32)r[0] == (s32)a);
        float fa;
        memcpy(&fa, &f.a[0], 4);
        h[kHypF2D] += sameDouble((double)fa, r[0], r[1], o);
        const float fl = (float)a;
        u32 frb;
        memcpy(&frb, &fl, 4);
        h[kHypD2F] += frb == r[0];
        const s32 cmp = (s32)r[0];
        h[kHypCmp] += a == a && b == b && ((a < b) == (cmp < 0)) && ((a == b) == (cmp == 0)) && ((a > b) == (cmp > 0));
    }
}

static void probeStep()
{
    if (g_host_render.vblanks < s_probeFrom)
        return;
    const u32 pc = next_pc & 0x1FFFFFFF;
    while (!s_probeStack.empty() && pc == (s_probeStack.back().ret & 0x1FFFFFFF) && r[15] == s_probeStack.back().sp)
    {
        probeResult(s_probeStack.back());
        s_probeStack.pop_back();
    }
    const bool in = pc >= s_probeLo && pc < s_probeHi;
    const bool wasIn = s_probePrev >= s_probeLo && s_probePrev < s_probeHi;
    const u32 ret = pr & 0x1FFFFFFF;
    if (in && !wasIn && !(ret >= s_probeLo && ret < s_probeHi) && s_probeStack.size() < 64)
        s_probeStack.push_back({pc, pr, r[15], {r[4], r[5], r[6], r[7]}});
    s_probePrev = pc;
}

static void printProbe(u32 frames)
{
    std::vector<std::pair<u64, u32>> order;
    for (const auto& e : s_probe)
        order.push_back({e.second.calls, e.first});
    std::sort(order.rbegin(), order.rend());
    printf("[probe] calls into %08X-%08X over %u frames\n", s_probeLo, s_probeHi, frames);
    for (const auto& o : order)
    {
        const ProbeStats& s = s_probe[o.second];
        std::string hyp;
        for (int w = 0; w < 2; w++)
            for (int k = 0; k < kHypCount; k++)
                if (s.hits[w][k] * 100ull >= s.calls * 99ull)
                    hyp += std::string(" ") + kHypNames[k] + (w ? "[r(n) high]" : "[r(n) low]") + " " +
                           std::to_string(s.hits[w][k]);
        printf("[probe]   %08X  %8.1f calls a frame  matches:%s | e.g. r4-r7 %08X %08X %08X %08X -> r0 r1 %08X %08X\n",
               o.second, frames ? (double)s.calls / frames : 0.0, hyp.empty() ? " none" : hyp.c_str(), s.example[0],
               s.example[1], s.example[2], s.example[3], s.example[4], s.example[5]);
    }
}

// --soft-float 0/1/2: the launcher's SOFT FLOAT (default 0 here). 1 = the game's software
// double divide and multiply run natively (naomi_softfloat.cpp); 2 = the game's code runs
// and each result is checked against the native one. --soft-float-test N: N random operand
// pairs against the host's own doubles, no game needed.
static int s_softFloat;
struct SoftFloatCall
{
    u32 ret, sp, kind, a[4], native[2];
};
static std::vector<SoftFloatCall> s_sfStack;
static u64 s_sfCalls[2], s_sfNative[2], s_sfChecked[2], s_sfWrong[2];
static const char* const kSfNames[2] = {"divide", "multiply"};

static void softFloatStep()
{
    const u32 pc = next_pc & 0x1FFFFFFF;
    while (!s_sfStack.empty() && pc == (s_sfStack.back().ret & 0x1FFFFFFF) && r[15] == s_sfStack.back().sp)
    {
        const SoftFloatCall& call = s_sfStack.back();
        s_sfChecked[call.kind]++;
        if ((r[0] != call.native[0] || r[1] != call.native[1]) && s_sfWrong[call.kind]++ < 16)
            printf("[run] SOFT FLOAT %s MISMATCH %08X%08X, %08X%08X: game %08X%08X, native %08X%08X\n",
                   kSfNames[call.kind], call.a[1], call.a[0], call.a[3], call.a[2], r[1], r[0], call.native[1],
                   call.native[0]);
        s_sfStack.pop_back();
    }
    const NaomiSoftFloatFn fn = pc == 0x0C222D00 || pc == 0x0C222A00 ? naomi_SoftFloatEntry(pc) : nullptr;
    if (!fn)
        return;
    const u32 kind = fn == naomi_SoftFloatMul;
    s_sfCalls[kind]++;
    if (!fn(r[4], r[5], r[6], r[7]))
        return;
    s_sfNative[kind]++;
    if (s_softFloat == 1)
    {
        r[0] = g_soft_float_result[0];
        r[1] = g_soft_float_result[1];
        next_pc = pr;
    }
    else if (s_sfStack.size() < 64)
        s_sfStack.push_back({pr, r[15], kind, {r[4], r[5], r[6], r[7]}, {g_soft_float_result[0], g_soft_float_result[1]}});
}

static int softFloatTest(u32 pairs)
{
    u64 seed = 0x9E3779B97F4A7C15ull;
    auto next = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    // Exponents that keep results normal; one operand in two has its middle fraction bits
    // clear, for exact products and ties; one in 64 is zero.
    auto make = [&next]() {
        const u64 bits = next();
        u64 frac = bits & 0xFFFFFFFFFFFFFull;
        if (bits >> 63)
            frac &= 0xFFC00000003FFull;
        const u64 e = (bits & 0x3F0000) ? 600 + next() % 800 : 0;
        return (next() & 1) << 63 | e << 52 | (e ? frac : 0);
    };
    const NaomiSoftFloatFn fns[2] = {naomi_SoftFloatDiv, naomi_SoftFloatMul};
    u64 wrong[2] = {}, left[2] = {};
    for (u32 i = 0; i < pairs; i++)
    {
        const u64 a = make(), b = make();
        double da, db;
        memcpy(&da, &a, 8);
        memcpy(&db, &b, 8);
        const double want[2] = {da / db, da * db};
        for (int k = 0; k < 2; k++)
        {
            if (!fns[k]((u32)a, (u32)(a >> 32), (u32)b, (u32)(b >> 32)))
            {
                left[k]++;
                continue;
            }
            const u64 got = (u64)g_soft_float_result[1] << 32 | g_soft_float_result[0];
            u64 w;
            memcpy(&w, &want[k], 8);
            if (got != w && wrong[k]++ < 8)
                printf("[run] SOFT FLOAT %s WRONG %016llX, %016llX: host %016llX, native %016llX\n", kSfNames[k],
                       (unsigned long long)a, (unsigned long long)b, (unsigned long long)w, (unsigned long long)got);
        }
    }
    for (int k = 0; k < 2; k++)
        printf("[run] SOFT FLOAT self-test %s: %u pairs, %llu wrong, %llu left to the game\n", kSfNames[k], pairs,
               (unsigned long long)wrong[k], (unsigned long long)left[k]);
    return wrong[0] || wrong[1];
}

static bool s_reflectionHook;

static void pcHook()
{
    if (s_reflectionHook)
        reflectionHook();
    if (s_probeHi)
        probeStep();
    if (s_softFloat)
        softFloatStep();
}

// --work-from F[:T]: from frame F on, a [work] line per frame with its SH-4
// instructions outside the wait-for-frame loop: the game's real work, what is
// left on a Wii after the idle skip. With T, frames over T instructions and
// the others get a PC histogram each, to see what a heavy frame does more of.
static u32 s_workFrom;
static u32 s_workSpike;
static u32 s_workTotal;              // this frame, outside the wait loop
static u32 s_workRanges[2];          // of it: 0C048E80 routine, 0C20A2A0 copy loop
static u32 s_workFrames[2];          // normal frames, heavy frames
static std::vector<u32> s_workHisto[3];   // this frame, normal frames, heavy frames
static const ChdGdrom* s_workGdrom;  // hunks decoded, for the [work] line
static u32 s_workHunks;

static void workHook(u32 pc, u32 op)
{
    const u32 a = pc & 0x1FFFFFFF;
    if ((a >= 0x0C20C840 && a < 0x0C20C880) || (a >= 0x0C211880 && a < 0x0C2118C0))
        return;
    s_workTotal++;
    s_workRanges[0] += a >= 0x0C048E80 && a < 0x0C049240;
    s_workRanges[1] += a >= 0x0C20A2A0 && a < 0x0C20A2B0;
    if (s_workSpike)
    {
        const u32 bucket = (pc & 0x01FFFFFF) >> 6;
        if (bucket < s_workHisto[0].size())
            s_workHisto[0][bucket]++;
    }
    if (s_watchAddr || s_tracePcCount)
        watchHook(pc, op);
}

static void workFrame(u32 frame)
{
    const u32 hunks = s_workGdrom ? s_workGdrom->hunksDecoded() : 0;
    printf("[work] f%u %u instructions, routine %u, copy %u, hunks %u\n", frame, s_workTotal, s_workRanges[0],
           s_workRanges[1], hunks - s_workHunks);
    s_workHunks = hunks;
    if (s_workSpike)
    {
        const u32 heavy = s_workTotal > s_workSpike;
        s_workFrames[heavy]++;
        for (size_t i = 0; i < s_workHisto[0].size(); i++)
            s_workHisto[1 + heavy][i] += s_workHisto[0][i];
        std::fill(s_workHisto[0].begin(), s_workHisto[0].end(), 0u);
    }
    s_workTotal = s_workRanges[0] = s_workRanges[1] = 0;
}

// Per 64-byte range, instructions a frame in the heavy frames against the
// normal ones, biggest difference first.
static void printWork()
{
    if (!s_workSpike || !s_workFrames[0] || !s_workFrames[1])
        return;
    struct Row
    {
        double normal, heavy;
        u32 bucket;
    };
    std::vector<Row> rows;
    for (u32 i = 0; i < s_workHisto[1].size(); i++)
        if (s_workHisto[1][i] || s_workHisto[2][i])
            rows.push_back({s_workHisto[1][i] / (double)s_workFrames[0], s_workHisto[2][i] / (double)s_workFrames[1], i});
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.heavy - a.normal > b.heavy - b.normal; });
    printf("[work] %u normal frames, %u heavy frames; instructions a frame by range:\n", s_workFrames[0], s_workFrames[1]);
    for (size_t i = 0; i < rows.size() && i < 48; i++)
        printf("[work]   %08X-%08X  normal %8.0f  heavy %8.0f\n", 0x0C000000 | (rows[i].bucket << 6),
               0x0C000000 | ((rows[i].bucket << 6) + 63), rows[i].normal, rows[i].heavy);
}

// --path-after-elan: once the ELAN has received its first command, print
// every branch target executed for the first time, until the first cart DMA
// (the code path the game takes between those two events).
static std::vector<bool> s_pathSeen;
static u32 s_pathPrev;
static u32 s_pathLines;
static bool s_pathArmed;

static void pathHook(u32 pc, u32)
{
    if (!s_pathArmed)
    {
        if (naomi_ElanStats().commands == 0)
            return;
        s_pathArmed = true;
        printf("[path] armed at pc %08X\n", pc);
    }
    if (naomi_Stats().cartDmaCount > 0 || s_pathLines >= 2000)
    {
        printf("[path] end at pc %08X (%u targets)\n", pc, s_pathLines);
        g_sh4_step_hook = nullptr;
        return;
    }
    const u32 slot = (pc & 0x1FFFFFFF) >> 1;
    if (pc != s_pathPrev + 2 && slot < s_pathSeen.size() && !s_pathSeen[slot])
    {
        s_pathSeen[slot] = true;
        s_pathLines++;
        printf("[path] %08X <- %08X  pr %08X r0 %08X r4 %08X r8 %08X r9 %08X\n", pc, s_pathPrev, pr, r[0], r[4], r[8],
               r[9]);
    }
    s_pathPrev = pc;
}

// Memory census: non-zero 4 KB pages per guest region (what a demand-paged
// Wii build would have to back), printed every 600 frames.
static u32 touchedPages(const u8* base, u32 size)
{
    u32 pages = 0;
    for (u32 off = 0; off < size; off += 4096)
    {
        const u64* p = (const u64*)(base + off);
        for (u32 i = 0; i < 4096 / 8; i++)
            if (p[i])
            {
                pages++;
                break;
            }
    }
    return pages;
}

static void memoryCensus(u32 frame)
{
    const double mb = 4096.0 / (1024 * 1024);
    const u32 w[4] = {host_GuestWrittenPages(mem_b.data, RAM_SIZE), host_GuestWrittenPages(vram.data, VRAM_SIZE),
                      host_GuestWrittenPages(aica_ram.data, ARAM_SIZE),
                      host_GuestWrittenPages(naomi_ElanRam(), naomi_ElanRamSize())};
    printf("[mem] frame %5u  non-zero / written:  ram %5.1f / %5.1f MB  vram %5.1f / %5.1f  aram %4.1f / %4.1f  "
           "elan %5.1f / %5.1f  total written %5.1f MB\n",
           frame, touchedPages(mem_b.data, RAM_SIZE) * mb, w[0] * mb, touchedPages(vram.data, VRAM_SIZE) * mb,
           w[1] * mb, touchedPages(aica_ram.data, ARAM_SIZE) * mb, w[2] * mb,
           touchedPages(naomi_ElanRam(), naomi_ElanRamSize()) * mb, w[3] * mb, (w[0] + w[1] + w[2] + w[3]) * mb);
}

static u32 s_frameLimit = 600;
static u64 s_startUs = 0;
extern u32 g_ccn_ici_count;   // emu/dc/sh4/ccn.cpp
extern "C" u32 g_sfx_hits[2];   // plugs/nullAICA/sgc_if.cpp, SOUND LEVELS

// --press F:name[,F:name...]: hold a control for 10 frames from frame F
// (coin, start, view, up, down = shifter, left, right = steering, gas,
// brake, test, service), to reach menus without a pad.
struct Press
{
    u32 frame;
    std::string name;
};
static std::vector<Press> s_presses;

static void pollPresses(NaomiInputs& in)
{
    naomi_DefaultInputs(in);
    const u32 f = g_host_render.vblanks;
    for (const Press& p : s_presses)
    {
        if (f < p.frame || f >= p.frame + 10)
            continue;
        const std::string& n = p.name;
        if (n == "coin")
            in.coin[0] = true;
        else if (n == "start")
            in.players[0] |= NAOMI_JVS_START;
        else if (n == "view")
            in.players[0] |= NAOMI_JVS_DOWN;
        else if (n == "up")
            in.players[1] |= NAOMI_JVS_UP;
        else if (n == "down")
            in.players[1] |= NAOMI_JVS_DOWN;
        else if (n == "left")
            in.analog[0] = 0x2000;
        else if (n == "right")
            in.analog[0] = 0xE000;
        else if (n == "gas")
            in.analog[1] = 0xFFFF;
        else if (n == "brake")
            in.analog[2] = 0xFFFF;
        else if (n == "test")
            in.test = true;
        else if (n == "service")
            in.players[0] |= NAOMI_JVS_SERVICE;
    }
}

static void logLine(const char* text)
{
    fputs(text, stdout);
    fflush(stdout);
    log::write("%s", text);
}

void host_on_vblank()
{
    const u32 frame = g_host_render.vblanks;
    if (frame % 60 == 0 || frame == s_frameLimit)
    {
        const NaomiStats& st = naomi_Stats();
        const double secs = (double)(platform::microseconds() - s_startUs) / 1e6;
        char line[400];
        snprintf(line, sizeof(line),
                 "[run] frame %5u  %6.1fs  pc %08X  sr %08X  renders %u  vtx %u  cartDMA %u (%u KB)  pio %u  board r %u w %u  "
                 "dimm %u  elan cmd %u\n",
                 frame, secs, next_pc, sr.GetFull(), g_host_render.renders, g_host_render.vertices,
                 st.cartDmaCount, st.cartDmaBytes / 1024, st.cartPioReads, st.boardRegReads, st.boardRegWrites,
                 st.dimmCommands, naomi_ElanStats().commands);
        logLine(line);
        {
            // Sound samples per real second against emulated frames per real
            // second: SOUND CLOCK = REAL TIME keeps the first at 44100.
            extern u32 g_aica_samples_total;
            static u32 lastSamples = 0, lastFrame = 0;
            static double lastSecs = 0;
            if (secs > lastSecs)
            {
                snprintf(line, sizeof(line), "[run]   sound: %.0f samples per real second, %.1f emulated fps\n",
                         (g_aica_samples_total - lastSamples) / (secs - lastSecs), (frame - lastFrame) / (secs - lastSecs));
                logLine(line);
            }
            lastSamples = g_aica_samples_total;
            lastFrame = frame;
            lastSecs = secs;
        }
        snprintf(line, sizeof(line),
                 "[run]   holly ist nrm %08X ext %08X err %08X | iml6 %08X/%08X/%08X iml4 %08X/%08X/%08X iml2 %08X/%08X/%08X\n",
                 SB_ISTNRM, SB_ISTEXT, SB_ISTERR, SB_IML6NRM, SB_IML6EXT, SB_IML6ERR, SB_IML4NRM, SB_IML4EXT,
                 SB_IML4ERR, SB_IML2NRM, SB_IML2EXT, SB_IML2ERR);
        logLine(line);
        snprintf(line, sizeof(line),
                 "[run]   expevt %03X intevt %03X spc %08X ssr %08X | ipra %04X iprb %04X iprc %04X tstr %02X tcr0 %04X | "
                 "vec224 %08X vec22C %08X vec240 %08X vec2A0 %08X\n",
                 ReadMem32(0xFF000024), ReadMem32(0xFF000028), spc, ssr, ReadMem16(0xFFD00004), ReadMem16(0xFFD00008),
                 ReadMem16(0xFFD0000C), ReadMem8(0xFFD80004), ReadMem16(0xFFD80010), ReadMem32(0x8C000224),
                 ReadMem32(0x8C00022C), ReadMem32(0x8C000240), ReadMem32(0x8C0002A0));
        logLine(line);
        snprintf(line, sizeof(line),
                 "[run]   r0-7  %08X %08X %08X %08X %08X %08X %08X %08X\n"
                 "[run]   r8-15 %08X %08X %08X %08X %08X %08X %08X %08X  pr %08X vbr %08X\n",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14],
                 r[15], pr, vbr);
        logLine(line);
        snprintf(line, sizeof(line), "[run]   arm7 %s pc %08X lr %08X sp %08X | aram[5C] %08X aram[0] %08X\n",
                 Arm7Enabled ? "on " : "off", arm_GetNextPC(), arm_GetReg(14), arm_GetReg(13),
                 *(u32*)&aica_ram.data[0x5C], *(u32*)&aica_ram.data[0]);
        logLine(line);
        const NaomiElanStats& es = naomi_ElanStats();
        const NaomiJvsStats& js = naomi_JvsStats();
        snprintf(line, sizeof(line),
                 "[run]   ch2 dma a2 %u (%u KB) a4 %u (%u KB) | sq a1 %u a2 %u a3 %u a4 %u | elan reg r %u w %u | "
                 "mie %u jvs %u inputs %u unk %u | card %u\n",
                 g_dbg_ch2_count[2], g_dbg_ch2_bytes[2] / 1024, g_dbg_ch2_count[4], g_dbg_ch2_bytes[4] / 1024,
                 g_dbg_sqw_count[1], g_dbg_sqw_count[2], g_dbg_sqw_count[3], g_dbg_sqw_count[4], es.regReads,
                 es.regWrites, js.mapleFrames, js.jvsMessages, js.inputReads, js.unknown, naomi_CardCommands());
        logLine(line);
        snprintf(line, sizeof(line),
                 "[run]   elan lists %u ta %u n2 %u ich %u (%u vtx in, %u out) texdma %u KB waits %u errors %u | "
                 "I-cache invalidates %u\n",
                 es.lists, es.taBlocks, es.n2Commands, es.ichLists, es.ichVertices, es.tnlVertices,
                 es.texDmaBytes / 1024, es.waits, es.errors, g_ccn_ici_count);
        logLine(line);
        // ELAN cost per emulated frame over the last 60 frames (host time).
        static u64 lastExec = 0, lastTnl = 0, lastTa = 0, lastConv = 0;
        static u64 lastGpu = 0;
        snprintf(line, sizeof(line),
                 "[run]   elan time per frame: %.2f ms (T&L %.2f ms incl. TA feed; GPU lists %.2f ms; vertex loop %.2f ms; "
                 "TA feed %.2f ms)\n",
                 (es.execNs - lastExec) / 60e6, (es.tnlNs - lastTnl) / 60e6, (es.gpuNs - lastGpu) / 60e6,
                 (es.convNs - lastConv) / 60e6, (es.taNs - lastTa) / 60e6);
        lastGpu = es.gpuNs;
        lastExec = es.execNs;
        lastTnl = es.tnlNs;
        lastTa = es.taNs;
        lastConv = es.convNs;
        logLine(line);
        // Commands the walk read (headers; summarised models' commands are not read).
        static u32 lastRead = 0, lastReadDropped = 0, lastDropped = 0;
        const u32 dropped = g_rend_dropped_renders - lastDropped;
        const u32 readDropped = es.walkReadDropped - lastReadDropped;
        snprintf(line, sizeof(line), "[run]   elan walk: %u commands read a frame | dropped frames %u, %u read each\n",
                 (es.walkRead - lastRead) / 60, dropped, dropped ? readDropped / dropped : 0u);
        lastRead = es.walkRead;
        lastReadDropped = es.walkReadDropped;
        lastDropped = g_rend_dropped_renders;
        logLine(line);
        static u32 lastIn = 0, lastLit = 0, lastEvals = 0;
        const u32 dIn = es.ichVertices - lastIn, dLit = es.litVertices - lastLit, dEv = es.lightEvals - lastEvals;
        snprintf(line, sizeof(line),
                 "[run]   elan vertices per frame: %u in, %u lit, %.2f lights per lit vertex | %u lists skipped, %u renders "
                 "skipped | state commands skipped %u, drawn lists held back %u\n",
                 dIn / 60, dLit / 60, dLit ? (double)dEv / dLit : 0.0, es.ichSkipped, g_host_render.skipped,
                 es.stateSkipped, es.ichHeldBack);
        logLine(line);
        snprintf(line, sizeof(line),
                 "[run]   elan model lists: %u walked, %u stepped over (%u polygon lists not walked), changed %u, "
                 "side effects changed %u | cart DMA polls %u\n",
                 es.modelsWalked, es.modelsSkipped, es.modelListsSkipped, es.modelsChanged, es.modelsFlipped,
                 naomi_Stats().cartDmaPolls);
        lastIn = es.ichVertices;
        lastLit = es.litVertices;
        lastEvals = es.lightEvals;
        logLine(line);
        {
            static u32 lastV = 0, lastI = 0, lastS = 0, lastF = 0, lastSF = 0, lastP = 0, lastT = 0;
            const u32 dv = es.gpuVertices - lastV, di = es.gpuIndices - lastI, ds = es.gpuStrips - lastS;
            if (dv)
            {
                snprintf(line, sizeof(line),
                         "[run]   GPU strips per frame: %u vertices -> %u indices (%+.0f%%) | %u strips of %.1f vertices, "
                         "%u fan vertices in %u strips, %u placeholders | %u triangles, lists drawn differently "
                         "strip by strip: %u (total)\n",
                         dv / 60, di / 60, 100.0 * ((double)di - dv) / dv, ds / 60, ds ? (double)dv / ds : 0.0,
                         (es.gpuFanVertices - lastF) / 60, (es.gpuStripsWithFans - lastSF) / 60,
                         (es.gpuPlaceholders - lastP) / 60, (es.gpuTriangles - lastT) / 60, es.gpuTriangleMismatches);
                lastT = es.gpuTriangles;
                logLine(line);
            }
            lastV = es.gpuVertices;
            lastI = es.gpuIndices;
            lastS = es.gpuStrips;
            lastF = es.gpuFanVertices;
            lastSF = es.gpuStripsWithFans;
            lastP = es.gpuPlaceholders;
        }
        {
            // IN-CAR REFLECT: where its mirror models are seen and its builds skipped.
            static u32 lastSeen = 0, lastSkips = 0, lastStarts = 0;
            if (g_incar_reflect_preset && (g_elan_mirror_seen != lastSeen || g_reflection_incar_skips != lastSkips))
            {
                snprintf(line, sizeof(line),
                         "[run]   IN-CAR REFLECT per 60 frames: %u mirror models, %u of %u builds not made\n",
                         g_elan_mirror_seen - lastSeen, g_reflection_incar_skips - lastSkips,
                         s_reflectionStarts - lastStarts);
                logLine(line);
            }
            lastSeen = g_elan_mirror_seen;
            lastSkips = g_reflection_incar_skips;
            lastStarts = s_reflectionStarts;
        }
        {
            static NaomiElanStats last = {};
            if (es.boxLists != last.boxLists || es.depthMeasured != last.depthMeasured)
            {
                snprintf(line, sizeof(line),
                         "[run]   3D list cull per frame: %u lists with a box, %u dropped behind the camera, %u out of "
                         "view (%u vertices), whole models %u (%u lists, %u STALE so far) | depth measured on %u vertices\n",
                         (es.boxLists - last.boxLists) / 60, (es.boxCulledBehind - last.boxCulledBehind) / 60,
                         (es.boxCulledSide - last.boxCulledSide) / 60,
                         (es.boxCulledVertices - last.boxCulledVertices) / 60,
                         (es.modelsCulled - last.modelsCulled) / 60, (es.modelListsCulled - last.modelListsCulled) / 60,
                         es.modelCheckStale, (es.depthMeasured - last.depthMeasured) / 60);
                logLine(line);
            }
            if (getenv("IDAS3_FAR_CENSUS"))
            {
                u32 f[8], z[8], zl[8];
                for (int k = 0; k < 8; k++)
                {
                    f[k] = (es.farBins[k] - last.farBins[k]) / 60;
                    z[k] = (es.sizeBins[k] - last.sizeBins[k]) / 60;
                    zl[k] = (es.sizeBinLists[k] - last.sizeBinLists[k]) / 60;
                }
                snprintf(line, sizeof(line),
                         "[run]   drawn boxed vertices by nearest depth <25 %u <50 %u <100 %u <200 %u <400 %u <800 %u "
                         "<1600 %u more %u | by size on screen <2px %u/%u <4 %u/%u <8 %u/%u <16 %u/%u <32 %u/%u "
                         "<64 %u/%u <128 %u/%u more %u/%u (vertices/lists)\n",
                         f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], z[0], zl[0], z[1], zl[1], z[2], zl[2], z[3],
                         zl[3], z[4], zl[4], z[5], zl[5], z[6], zl[6], z[7], zl[7]);
                logLine(line);
            }
            if (es.boxCulledSmall != last.boxCulledSmall)
            {
                snprintf(line, sizeof(line), "[run]   SMALL 3D per frame: %u lists not drawn\n",
                         (es.boxCulledSmall - last.boxCulledSmall) / 60);
                logLine(line);
            }
            if (es.lightSetsBuilt != last.lightSetsBuilt || es.lightSetsReused != last.lightSetsReused)
            {
                snprintf(line, sizeof(line),
                         "[run]   point-lit light sets per frame: %u built, %u the last one again (checked: %u STALE "
                         "so far)\n",
                         (es.lightSetsBuilt - last.lightSetsBuilt) / 60, (es.lightSetsReused - last.lightSetsReused) / 60,
                         es.lightSetsStale);
                logLine(line);
            }
            if (es.mirrorModels != last.mirrorModels)
            {
                snprintf(line, sizeof(line),
                         "[run]   MIRROR OFF per frame: %u models not drawn (%u from their summary), %u lists, %u "
                         "vertices\n",
                         (es.mirrorModels - last.mirrorModels) / 60, (es.mirrorSummarised - last.mirrorSummarised) / 60,
                         (es.mirrorLists - last.mirrorLists) / 60, (es.mirrorVertices - last.mirrorVertices) / 60);
                logLine(line);
            }
            last = es;
        }
        if (g_host_render.depthFrames)
        {
            HostRenderStats& h = g_host_render;
            snprintf(line, sizeof(line),
                     "[run]   GPU depth range: near at the 0.01 plane in %u of %u renders, under 1 in %u | near %g..%g, "
                     "far up to %g\n",
                     h.nearPinned, h.depthFrames, h.nearUnder1, h.nearMin, h.nearMax, h.farMax);
            logLine(line);
            h.depthFrames = h.nearPinned = h.nearUnder1 = 0;
            h.farMax = 0.f;
        }
        {
            static u32 lastT = 0, lastChg = 0, lastAddr = 0, lastCont = 0;
            const u32 t = es.dataTotal - lastT;
            const double pct = t ? 100.0 / t : 0.0;
            snprintf(line, sizeof(line),
                     "[run]   elan vertex data: %u per frame | changed before the render %.1f%% | same as last frame: "
                     "same address %.1f%%, same content anywhere %.1f%%\n",
                     t / 60, (es.dataChangedBeforeRender - lastChg) * pct, (es.dataSameAddress - lastAddr) * pct,
                     (es.dataSameContent - lastCont) * pct);
            lastT = es.dataTotal;
            lastChg = es.dataChangedBeforeRender;
            lastAddr = es.dataSameAddress;
            lastCont = es.dataSameContent;
            logLine(line);
        }
        static u32 lastCensus[8];
        u32 c[8];
        for (int i = 0; i < 8; i++)
        {
            c[i] = (es.census[i] - lastCensus[i]) / 60;
            lastCensus[i] = es.census[i];
        }
        snprintf(line, sizeof(line),
                 "[run]   elan census per frame: opaque %u, translucent %u, punch-through %u | env-map %u, specular %u, "
                 "point light %u, bump %u | GX-ready opaque %u\n",
                 c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
        logLine(line);
    }
    if (frame % 600 == 0)
    {
        memoryCensus(frame);
        if (g_area1_write_hook)
            area1Print(frame);
    }
    if (s_histoFrom && frame == s_histoFrom)
    {
        s_histo.assign(RAM_SIZE >> 6, 0);
        g_sh4_step_hook = histoHook;
    }
    if (s_armHistoFrom && frame == s_armHistoFrom)
    {
        s_armHisto.assign(0x800000 >> 6, 0);
        g_arm_histo_hook = armHistoHook;
    }
    if (s_workFrom && frame >= s_workFrom)
    {
        if (frame == s_workFrom)
        {
            for (auto& h : s_workHisto)
                h.assign(s_workSpike ? RAM_SIZE >> 6 : 0, 0);
            g_sh4_step_hook = workHook;
        }
        else
            workFrame(frame);
    }
    if (s_traceAtFrame && frame == s_traceAtFrame)
        s_traceArmed = true;
    if (frame >= s_frameLimit)
        naomi_Stop();
}

static const char* argValue(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return nullptr;
}

static bool hasFlag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name))
            return true;
    return false;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // the core prints a lot; keep it all on a crash
#if defined(_WIN32)
    SetUnhandledExceptionFilter(crashHandler);
#endif
    if (const char* t = argValue(argc, argv, "--soft-float-test"))
        return softFloatTest((u32)strtoul(t, nullptr, 0));
    const char* chd = argValue(argc, argv, "--chd");
    if (!chd)
    {
        printf("usage: idas3wii_run --chd <gds-0033.chd> [--frames N] [--log file]\n");
        return 2;
    }
    if (const char* f = argValue(argc, argv, "--frames"))
        s_frameLimit = (u32)strtoul(f, nullptr, 0);
    if (const char* c = argValue(argc, argv, "--cpi"))
        g_sh4_cpu_ratio_override = (s32)strtol(c, nullptr, 0);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--area1-census"))
            g_area1_write_hook = area1Hook;
    if (const char* h = argValue(argc, argv, "--histo-from"))
        s_histoFrom = (u32)strtoul(h, nullptr, 0);
    if (const char* h = argValue(argc, argv, "--arm-histo-from"))
        s_armHistoFrom = (u32)strtoul(h, nullptr, 0);
    if (const char* w = argValue(argc, argv, "--work-from"))
    {
        char* end = nullptr;
        s_workFrom = (u32)strtoul(w, &end, 0);
        if (*end == ':')
            s_workSpike = (u32)strtoul(end + 1, nullptr, 0);
    }
    if (const char* dir = argValue(argc, argv, "--snap-dir"))
    {
        // Software-rendered PNG of the frame every N emulated frames.
        const char* every = argValue(argc, argv, "--snap-every");
        host_SetSnapshots(dir, every ? (u32)strtoul(every, nullptr, 0) : 300);
    }
    log::open(argValue(argc, argv, "--log") ? argValue(argc, argv, "--log") : "idas3wii_run.log");

    static const char* tags[] = {"[ OK ]", "[WARN]", "[FAIL]", "[INFO]"};
    size_t printed = 0;
    GameSession session;
    const bool ok = session.check(chd, [&](const std::vector<CheckLine>& lines, const std::string&) {
        for (; printed < lines.size(); printed++)
            printf("%s %s\n", tags[(int)lines[printed].status], lines[printed].text.c_str());
    });
    if (!ok)
        return 1;

    // BIOS (service library): --bios <naomi.zip | rom.ic27>, else searched for
    // next to the CHD like the launcher does.
    BiosImage bios;
    std::string biosError;
    const char* biosArg = argValue(argc, argv, "--bios");
    const bool haveBios = biosArg ? loadBios(biosArg, bios, biosError)
                                  : findBios({pathDirectory(chd)}, bios, biosError);
    if (!haveBios)
    {
        printf("[FAIL] %s\n", biosError.c_str());
        return 1;
    }
    printf("[ OK ] BIOS service library from %s\n", bios.source.c_str());
    naomi_SetBiosImage(bios.data.data(), (u32)bios.data.size());

    naomi_SetLog(logLine);
    naomi_SetCart(
        [](void* user, u32 offset, u32 length, u8* dst) {
            static const bool trace = getenv("IDAS3_CART_TRACE") != nullptr;
            if (trace)
                printf("[cart] %08X +%X\n", offset, length);
            return static_cast<DimmImage*>(user)->read(offset, length, dst);
        },
        &session.dimm(), session.dimm().size());
    if (const char* a = argValue(argc, argv, "--dimm-ahead"))
        session.dimm().setReadAhead(strtol(a, nullptr, 0) != 0);
    g_reflection_rate_preset = 1;       // the game's own behaviour unless asked
    if (const char* d = argValue(argc, argv, "--depth-plane"))
    {
        extern int g_depth_plane_preset;
        g_depth_plane_preset = atoi(d);
    }
    if (const char* n = argValue(argc, argv, "--reflection"))
    {
        g_reflection_rate_preset = (int)strtol(n, nullptr, 0);
        s_reflectionHook = true;
        g_sh4_pc_hook = pcHook;
    }
    g_soft_float_preset = 0;
    if (const char* s = argValue(argc, argv, "--soft-float"))
    {
        s_softFloat = atoi(s);
        g_soft_float_preset = s_softFloat != 0;
        if (s_softFloat)
            g_sh4_pc_hook = pcHook;
    }
    if (const char* p = getenv("IDAS3_SF_PROBE"))
    {
        sscanf(p, "%x:%x:%u", &s_probeLo, &s_probeHi, &s_probeFrom);
        s_probeLo &= 0x1FFFFFFF;
        s_probeHi &= 0x1FFFFFFF;
        g_sh4_pc_hook = pcHook;
    }
    s_workGdrom = &session.gdrom();
    if (const char* p = argValue(argc, argv, "--press"))
    {
        for (const char* c = p; *c;)
        {
            char* end = nullptr;
            Press press;
            press.frame = (u32)strtoul(c, &end, 10);
            if (!end || *end != ':')
                break;
            c = end + 1;
            while (*c && *c != ',')
                press.name += *c++;
            s_presses.push_back(press);
            if (*c == ',')
                c++;
        }
        naomi_SetInputPoll(pollPresses);
    }
    if (const char* fx = argValue(argc, argv, "--fixes"))
    {
        // The launcher's FIXES page (all ON by default except AICA CA CLAMP):
        // "--fixes off" runs the legacy behaviour for A/B comparisons. Set
        // before naomi_Init, which reads some of them (register reset values).
        extern int g_arm7_str_align_preset, g_adpcm_loop_preset, g_arm7_reset_fix_preset, g_aica_race_preset,
            g_jit_link_guard_preset, g_div32_fix_preset, g_interp_fix_preset, g_spg_fix_preset, g_ta_guard_preset,
            g_inf_vtx_preset;
        const int on = strcmp(fx, "off") != 0;
        for (int* p : {&g_arm7_str_align_preset, &g_adpcm_loop_preset, &g_arm7_reset_fix_preset, &g_aica_race_preset,
                       &g_jit_link_guard_preset, &g_div32_fix_preset, &g_interp_fix_preset, &g_spg_fix_preset,
                       &g_ta_guard_preset, &g_inf_vtx_preset})
            *p = on;
        logLine(on ? "[run] fixes: on\n" : "[run] fixes: off (legacy)\n");
    }

    if (!naomi_Init(false))
    {
        logLine("[run] core initialisation failed\n");
        return 1;
    }
    if (const char* w = argValue(argc, argv, "--watch"))
    {
        s_watchAddr = (u32)strtoul(w, nullptr, 16);
        g_sh4_step_hook = watchHook;
    }
    if (const char* t = argValue(argc, argv, "--trace-from"))
    {
        s_traceFrom = (u32)strtoul(t, nullptr, 16);
        s_traceArmed = false;
    }
    if (const char* t = argValue(argc, argv, "--trace-at"))
    {
        s_traceAtFrame = (u32)strtoul(t, nullptr, 0);
        s_traceArmed = false;
    }
    if (const char* t = argValue(argc, argv, "--trace-pc"))
    {
        for (char* p = (char*)t; *p && s_tracePcCount < 8; p += *p == ',')
            s_tracePcs[s_tracePcCount++] = (u32)strtoul(p, &p, 16);
        g_sh4_step_hook = watchHook;
    }
    if (hasFlag(argc, argv, "--gpu-tnl"))
    {
        // What the Wii's GPU T&L leaves on the CPU: eligible lists are taken
        // (and dropped, there is no GX here) instead of transformed.
        extern int g_elan_gx_preset;
        g_elan_gx_preset = 1;
    }
    if (const char* fs = argValue(argc, argv, "--frameskip"))
    {
        extern int g_frameskip_preset;
        g_frameskip_preset = atoi(fs);
    }
    if (const char* ms = argValue(argc, argv, "--model-skip"))
        g_elan_model_skip_preset = atoi(ms);
    if (const char* pl = argValue(argc, argv, "--point-lights"))
    {
        // NIGHT LIGHTS: 1 = point lights as GX local lights. Without --gpu-tnl
        // the lists stay on the CPU, lit the way GX would (a preview).
        extern int g_elan_point_preset;
        g_elan_point_preset = atoi(pl);
    }
    if (const char* lc = argValue(argc, argv, "--list-cull"))
    {
        // 3D LIST CULL (with --gpu-tnl): 1 = lists behind the camera dropped by
        // their fetch program's box, 2 = also those out of the picture, 3 = also
        // whole models stepped over. Every box used is checked against its
        // list's records ("3D list boxes"), each model stepped over against
        // its block ("STALE so far"). The "record digest" lines of two runs at
        // the same value must match; 3 may differ from 2 (a hidden list that
        // lost its own box is then not drawn at all).
        extern int g_elan_box_preset;
        g_elan_box_preset = atoi(lc);
    }
    if (const char* sm = argValue(argc, argv, "--small"))
    {
        // SMALL 3D (with --gpu-tnl and --list-cull): lists and whole models
        // under this many pixels on screen are not drawn.
        g_small_cull_preset = atoi(sm);
    }
    g_incar_reflect_preset = 0;         // the game's own behaviour unless asked
    if (const char* ir = argValue(argc, argv, "--incar-reflect"))
    {
        // IN-CAR REFLECT: while the in-car view (its rear-view mirror models)
        // is on screen, one reflection map build in N; -1 = none.
        g_incar_reflect_preset = atoi(ir);
        s_reflectionHook = true;
        g_sh4_pc_hook = pcHook;
    }
    if (const char* dl = argValue(argc, argv, "--display-lists"))
    {
        // DISPLAY LISTS (with --gpu-tnl): every cached list's display-list
        // entry is built as on the Wii, its bytes read back as the GPU would
        // and compared with the CPU draw ("DISPLAY LISTS ... MISMATCHES",
        // "HAZARDS": both must stay 0). Env IDAS3_DL_RING_KB: ring size.
        extern int g_elan_dl_preset;
        g_elan_dl_preset = atoi(dl);
    }
    if (const char* cf = argValue(argc, argv, "--colour-fetch"))
    {
        // COLOUR LISTS (with --gpu-tnl): 1 = colour lists get fetch programs
        // (so boxes) and are drawn from them when their vertex alpha is 255 or
        // TSP UseAlpha is off, checked like the others; "(N more fetched)".
        extern int g_elan_colour_fetch_preset;
        g_elan_colour_fetch_preset = atoi(cf);
    }
    if (const char* mi = argValue(argc, argv, "--mirror"))
    {
        // MIRROR: 0 = the in-car view's rear-view mirror (models clipped inside
        // the user clip rectangle) is not drawn.
        g_mirror_preset = atoi(mi);
    }
    if (const char* cu = argValue(argc, argv, "--cull"))
    {
        // BACKFACE CULL: 2 = the polygons' own cull modes (the launcher's ON),
        // 1 = the other winding; the snapshots show it.
        extern int g_isp_cull_preset;
        g_isp_cull_preset = atoi(cu);
    }
    if (hasFlag(argc, argv, "--path-after-elan"))
    {
        s_pathSeen.assign(0x10000000, false);
        g_sh4_step_hook = pathHook;
    }
    s_startUs = platform::microseconds();
    naomi_Run();
    if (!s_histo.empty())
        printHisto();
    if (!s_armHisto.empty())
        printArmHisto(s_frameLimit > s_armHistoFrom ? s_frameLimit - s_armHistoFrom : 0);
    printWork();
    naomi_BoardLogCounters();

    if (const char* d = argValue(argc, argv, "--disasm"))
    {
        // Guest code at the end of the run, for --histo-from's hot ranges:
        // --disasm 0C048E80:0C0491C0[,START:END...]
        for (const char* c = d; *c;)
        {
            char* end = nullptr;
            const u32 from = (u32)strtoul(c, &end, 16);
            if (!end || *end != ':')
                break;
            const u32 to = (u32)strtoul(end + 1, &end, 16);
            printf("[disasm] %08X-%08X\n", from, to);
            for (u32 pc = from & ~1u; pc < to; pc += 2)
            {
                const u16 op = ReadMem16(pc);
                printf("[disasm] %08X %04X  %s\n", pc, op, OpDesc[op]->diss);
            }
            c = *end == ',' ? end + 1 : end;
        }
    }

    if (const char* dump = argValue(argc, argv, "--dump-ram"))
    {
        // Guest RAM as the SH-4 sees it (little-endian bytes on this host).
        if (FILE* f = fopen(dump, "wb"))
        {
            fwrite(mem_b.data, 1, RAM_SIZE, f);
            fclose(f);
            printf("[run] RAM dumped to %s\n", dump);
        }
    }

    if (const char* dump = argValue(argc, argv, "--dump-elan"))
    {
        if (FILE* f = fopen(dump, "wb"))
        {
            fwrite(naomi_ElanRam(), 1, naomi_ElanRamSize(), f);
            fclose(f);
            printf("[run] ELAN RAM dumped to %s\n", dump);
            naomi_ElanLogRecent();
        }
    }
    if (const char* dump = argValue(argc, argv, "--dump-vram"))
    {
        if (FILE* f = fopen(dump, "wb"))
        {
            fwrite(vram.data, 1, VRAM_SIZE, f);
            fclose(f);
            printf("[run] VRAM dumped to %s\n", dump);
        }
    }
    if (const char* dump = argValue(argc, argv, "--dump-aica"))
    {
        if (FILE* f = fopen(dump, "wb"))
        {
            fwrite(aica_ram.data, 1, ARAM_SIZE, f);
            fclose(f);
            printf("[run] AICA RAM dumped to %s\n", dump);
        }
    }

    if (s_probeHi)
        printProbe(g_host_render.vblanks - s_probeFrom);
    {
        // SOUND LEVELS (sgc_if.cpp): key-ons recognised by their sample.
        printf("[run] SOUND LEVELS: %u menu cursor key-ons, %u tire squeal key-ons recognised\n", g_sfx_hits[0],
               g_sfx_hits[1]);
    }
    if (g_incar_reflect_preset)
        printf("[run] IN-CAR REFLECT: %u reflection map builds not made in the in-car view\n",
               g_reflection_incar_skips);
    for (int k = 0; k < 2 && s_softFloat; k++)
        printf("[run] SOFT FLOAT %s: %llu calls (%.1f a frame), %llu native, %llu checked, %llu MISMATCHES\n",
               kSfNames[k], (unsigned long long)s_sfCalls[k],
               g_host_render.vblanks ? (double)s_sfCalls[k] / g_host_render.vblanks : 0.0,
               (unsigned long long)s_sfNative[k], (unsigned long long)s_sfChecked[k], (unsigned long long)s_sfWrong[k]);
    const double secs = (double)(platform::microseconds() - s_startUs) / 1e6;
    char line[256];
    snprintf(line, sizeof(line), "[run] stopped after %u frames in %.1f s (%.1f emulated fps), pc %08X\n",
             g_host_render.vblanks, secs, secs > 0 ? g_host_render.vblanks / secs : 0.0, next_pc);
    logLine(line);
    naomi_Term();
    log::close();
    return 0;
}
