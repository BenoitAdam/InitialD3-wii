// Host functions the core expects from its platform layer. nullDC4Wii keeps
// them in its wii/main.cpp (its menu program); this port has its own
// launcher, so the few the core calls live here. Wii build only - the
// Windows equivalents are in host/host_win.cpp.
#include "types.h"

#if HOST_OS == OS_WII

#include <cstdarg>
#include <cstdio>
#include <ogc/lwp_watchdog.h>

#include "naomi/naomi.h"

double os_GetSeconds()
{
    // Time base deltas only: the absolute value is too large for a double
    // to keep frame-level precision (see nullDC4Wii's main.cpp).
    static u64 t0 = 0;
    const u64 now = gettime();
    if (t0 == 0)
        t0 = now;
    return (double)ticks_to_microsecs(now - t0) / 1000000.0;
}

int os_msgbox(const char* text, unsigned int)
{
    naomi_Log("[core] %s\n", text);
    return 0;
}

// Boot diagnostics from the memory and fastmem setup: same sink as the rest
// of the core's NAOMI logging (the launcher's log file and screen).
extern "C" void wii_boot_log(const char* fmt, ...)
{
    char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    naomi_Log("%s", buffer);
}

// gxRend switches the boot log off once its own logging starts; the port's
// log sink stays the same, so nothing to do.
extern "C" void wii_boot_log_stop() {}

// Per-game texture layering hacks from nullDC4Wii's game_presets (indexed
// slots, 0 = unset). None apply to Initial D3.
extern "C" int get_layer_back_tex_n(int) { return 0; }
extern "C" int get_layer_front_tex_n(int) { return 0; }

#endif // HOST_OS == OS_WII
