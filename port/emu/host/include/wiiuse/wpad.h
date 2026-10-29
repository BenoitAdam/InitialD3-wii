#pragma once
// Host input comes from the port's own JVS layer, not from Wii pads.
static inline int WPAD_ScanPads(void) { return 0; }
