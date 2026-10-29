// Force-included (/FI) in the Windows MSVC build of the forked core only.
// nullDC4Wii is written for GCC (devkitPPC); this maps the GCC extensions it
// uses onto MSVC equivalents so the shared sources stay untouched.
#pragma once

#if defined(_MSC_VER)

#include <intrin.h>

#define __attribute__(x)
#define __builtin_expect(x, y) (x)
#define __builtin_return_address(level) _ReturnAddress()
#define __builtin_prefetch(...) ((void)0)
#define __builtin_unreachable() __assume(0)
#define __builtin_bswap16 _byteswap_ushort
#define __builtin_bswap32 _byteswap_ulong
#define __builtin_bswap64 _byteswap_uint64
#define __restrict__ __restrict
#define __inline__ __inline

static __forceinline int __builtin_ctz(unsigned int x)
{
    unsigned long i;
    _BitScanForward(&i, x);
    return (int)i;
}

static __forceinline int __builtin_clz(unsigned int x)
{
    unsigned long i;
    _BitScanReverse(&i, x);
    return 31 - (int)i;
}

static __forceinline int __builtin_popcount(unsigned int x)
{
    return (int)__popcnt(x);
}

#endif
