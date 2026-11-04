// SOFT FLOAT. Initial D does its double-precision arithmetic in software:
// GCC soft-float routines, SH-4 integer code that never touches the FPU. A race
// makes ~23 divisions and ~23 multiplications a frame, ~40K SH-4 instructions,
// most of them the division's bit-by-bit loop (61 rounds a call). Called at a
// routine's entry by the recompiler (wii_driver.cpp ngen_Compile) and by the
// Windows runner, the routine runs here instead, on integers, step for step as
// the game's machine code does it: 61-bit fractions (the implicit bit at 60, 8
// rounding bits under the 52), the same quotient loop, the same 128-bit
// product, the same round-to-nearest-even with a sticky bit. Written from that
// machine code, not from any library source. Operands or results it does not
// model (NaN, infinity, denormals, overflow, division by zero) go back to the
// game's own code, which then runs as before. The routines, the unpack/pack
// helpers they call and their constants are fingerprinted: other code is never
// touched.
#include "types.h"
#include "dc/mem/sh4_mem.h"
#include "naomi.h"

extern "C" {
int g_soft_float_preset = 1;            // launch page SOFT FLOAT: 1 = run them here
alignas(8) u32 g_soft_float_result[2];  // r0 (low word), r1 (high word)
}

namespace {

const u64 kOne = 1ull << 60;   // a fraction's implicit bit

struct Number
{
    u32 sign;
    s32 exp;
    u64 frac;   // [kOne, 2 * kOne) once normalised
};

// The game's unpack (0C2225A0): 1 = a normal number, 0 = zero, -1 = a
// denormal, an infinity or a NaN (left to the game's code).
int unpack(u32 lo, u32 hi, Number& n)
{
    n.sign = hi >> 31;
    const u32 e = (hi >> 20) & 0x7FF;
    const u64 f = (u64)(hi & 0xFFFFF) << 32 | lo;
    if (e == 0)
        return f == 0 ? 0 : -1;
    if (e == 0x7FF)
        return -1;
    n.exp = (s32)e - 1023;
    n.frac = f << 8 | kOne;
    return 1;
}

u32 zero(u32 sign)
{
    g_soft_float_result[0] = 0;
    g_soft_float_result[1] = sign << 31;
    return 1;
}

// The game's pack (0C222440) of a normal number: its 8 rounding bits go to the
// nearest, ties to even. A result under the normal range is truncated there and
// one over it is an infinity: both are left to the game's code.
u32 pack(u32 sign, s32 exp, u64 f)
{
    if (exp < -1022 || exp > 1023)
        return 0;
    u32 e = (u32)(exp + 1023);
    if ((f & 0xFF) != 0x80)
        f += 0x7F;
    else if (f & 0x100)
        f += 0x80;
    if (f >= kOne << 1)
    {
        f >>= 1;
        e++;
    }
    if (e >= 0x7FF)
        return 0;
    f >>= 8;
    g_soft_float_result[0] = (u32)f;
    g_soft_float_result[1] = sign << 31 | e << 20 | ((u32)(f >> 32) & 0xFFFFF);
    return 1;
}

// Both routines fold what lies under the 8 rounding bits into them the same
// way: exactly half way up and odd, or half way up with more below, rounds up
// here; pack then finds no half way left.
u64 sticky(u64 f, bool below)
{
    if ((f & 0xFF) == 0x80 && ((f & 0x100) || below))
        f += 0x80;
    return f;
}

// Each routine with what it calls, as the game has them: start, end, FNV-1a
// of its 16-bit words.
struct Span
{
    u32 start, end, hash;
};
const Span kPackUnpack = {0x0C222440, 0x0C2226A4, 0x9FE45C96};
const Span kConstants = {0x0C2EEB88, 0x0C2EEBE8, 0x76C3FAD5};
const Span kMul = {0x0C222A00, 0x0C222CE4, 0xE22C0CDA};
const Span kDiv = {0x0C222D00, 0x0C222E68, 0x68430859};

bool matches(const Span& s)
{
    u32 h = 0x811C9DC5;
    for (u32 a = s.start; a < s.end; a += 2)
        h = (h ^ ReadMem16(a)) * 0x01000193;
    return h == s.hash;
}

} // namespace

// __divdf3 (0C222D00): a = r4 (low word), r5; b = r6, r7.
extern "C" u32 naomi_SoftFloatDiv(u32 r4, u32 r5, u32 r6, u32 r7)
{
    Number a = {}, b = {};
    const int ka = unpack(r4, r5, a);
    const int kb = unpack(r6, r7, b);
    if (ka < 0 || kb <= 0)
        return 0;
    const u32 sign = a.sign ^ b.sign;
    if (ka == 0)
        return zero(sign);
    s32 exp = a.exp - b.exp;
    u64 num = a.frac;
    const u64 den = b.frac;
    if (num < den)
    {
        num <<= 1;
        exp--;
    }
    // One quotient bit a round, 1 << 60 first: the quotient lands in [kOne, 2 kOne).
    u64 q = 0;
    for (int i = 0; i < 61; i++)
    {
        q <<= 1;
        if (num >= den)
        {
            num -= den;
            q |= 1;
        }
        num <<= 1;
    }
    return pack(sign, exp, sticky(q, num != 0));
}

// __muldf3 (0C222A00): same registers.
extern "C" u32 naomi_SoftFloatMul(u32 r4, u32 r5, u32 r6, u32 r7)
{
    Number a = {}, b = {};
    const int ka = unpack(r4, r5, a);
    const int kb = unpack(r6, r7, b);
    if (ka < 0 || kb < 0)
        return 0;
    const u32 sign = a.sign ^ b.sign;
    if (ka == 0 || kb == 0)
        return zero(sign);
    // The 122-bit product of the two fractions, from 32-bit halves.
    const u64 al = (u32)a.frac, ah = a.frac >> 32, bl = (u32)b.frac, bh = b.frac >> 32;
    const u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const u64 mid = (ll >> 32) + (u32)lh + (u32)hl;
    u64 lo = mid << 32 | (u32)ll;
    u64 hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    // Its high half sits in [2^56, 2^58): bring the leading bit up to 60.
    s32 exp = a.exp + b.exp + 4;
    while (hi < kOne)
    {
        hi = hi << 1 | lo >> 63;
        lo <<= 1;
        exp--;
    }
    return pack(sign, exp, sticky(hi, lo != 0));
}

extern "C" NaomiSoftFloatFn naomi_SoftFloatEntry(u32 pc)
{
    const u32 p = pc & 0x1FFFFFFF;
    if (!g_soft_float_preset || (p != kDiv.start && p != kMul.start))
        return nullptr;
    const Span& s = p == kDiv.start ? kDiv : kMul;
    if (!matches(s) || !matches(kPackUnpack) || !matches(kConstants))
        return nullptr;
    return p == kDiv.start ? naomi_SoftFloatDiv : naomi_SoftFloatMul;
}
