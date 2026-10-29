#pragma once
#include <stdint.h>
// x86 caches are coherent with the CPU's own writes: nothing to flush.
static inline void DCFlushRange(void* p, uint32_t len) { (void)p; (void)len; }
static inline void DCStoreRange(void* p, uint32_t len) { (void)p; (void)len; }
static inline void DCInvalidateRange(void* p, uint32_t len) { (void)p; (void)len; }
static inline void ICInvalidateRange(void* p, uint32_t len) { (void)p; (void)len; }
