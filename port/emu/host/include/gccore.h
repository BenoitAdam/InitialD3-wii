// Host stand-in for <gccore.h>: just the pieces the shared core touches.
#pragma once
#include "ogc/lwp_watchdog.h"
#include "ogc/machine/processor.h"
#include "ogc/video.h"
#include "ogc/cache.h"
#include "ogc/system.h"
#include "wiiuse/wpad.h"
static inline int PAD_ScanPads(void) { return 0; }
