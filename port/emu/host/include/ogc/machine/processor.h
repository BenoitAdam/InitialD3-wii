#pragma once
#include "../lwp_watchdog.h"
// Lower 32 bits of the (emulated) timebase, like mftb on Broadway.
static inline uint32_t PPCMftb(void) { return (uint32_t)host_gettime(); }
#define mftb() PPCMftb()
