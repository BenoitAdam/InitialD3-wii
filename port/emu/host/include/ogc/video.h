#pragma once
#include <stdint.h>
typedef void (*VIRetraceCallback)(uint32_t retraceCnt);
// No video interface on the host: callbacks never fire, vsync waits are free.
static inline VIRetraceCallback VIDEO_SetPostRetraceCallback(VIRetraceCallback cb) { (void)cb; return 0; }
static inline VIRetraceCallback VIDEO_SetPreRetraceCallback(VIRetraceCallback cb) { (void)cb; return 0; }
static inline void VIDEO_WaitVSync(void) {}
