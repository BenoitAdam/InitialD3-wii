// Host implementations behind host/include (Windows build only).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#include "include/ogc/lwp_watchdog.h"
#include "include/unistd.h"

extern "C" uint64_t host_gettime(void)
{
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t ticks = (uint64_t)c.QuadPart, f = (uint64_t)freq.QuadPart;
    const uint64_t perSec = (uint64_t)TB_TIMER_CLOCK * 1000ull;
    return ticks / f * perSec + ticks % f * perSec / f;
}

extern "C" int usleep(useconds_t usec)
{
    Sleep((DWORD)((usec + 999) / 1000));
    return 0;
}
#endif
