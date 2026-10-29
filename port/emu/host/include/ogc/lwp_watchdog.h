// Host stand-in for libogc's timebase: a 60.75 MHz counter (TB_TIMER_CLOCK
// ticks per millisecond), derived from QueryPerformanceCounter.
#pragma once
#include <stdint.h>

#define TB_BUS_CLOCK 243000000u
#define TB_TIMER_CLOCK (TB_BUS_CLOCK / 4000)

#ifdef __cplusplus
extern "C" {
#endif
uint64_t host_gettime(void);
#ifdef __cplusplus
}
#endif

static inline uint64_t gettime(void) { return host_gettime(); }

#define ticks_to_secs(ticks) (((uint64_t)(ticks)) / (TB_TIMER_CLOCK * 1000ull))
#define ticks_to_millisecs(ticks) (((uint64_t)(ticks)) / (TB_TIMER_CLOCK))
#define ticks_to_microsecs(ticks) ((((uint64_t)(ticks)) * 8) / (TB_TIMER_CLOCK / 125))
#define ticks_to_nanosecs(ticks) ((((uint64_t)(ticks)) * 8000) / (TB_TIMER_CLOCK / 125))
#define secs_to_ticks(sec) (((uint64_t)(sec)) * (TB_TIMER_CLOCK * 1000ull))
#define millisecs_to_ticks(msec) (((uint64_t)(msec)) * (TB_TIMER_CLOCK))
#define microsecs_to_ticks(usec) ((((uint64_t)(usec)) * (TB_TIMER_CLOCK / 125)) / 8)
#define diff_ticks(tick0, tick1) (((uint64_t)(tick1)) - ((uint64_t)(tick0)))
