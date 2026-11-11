// Hooks between the forked core and the port's host side (Windows build).
#pragma once

#include "types.h"

struct HostRenderStats
{
    u32 vblanks;
    u32 renders;
    u32 skipped;        // renders dropped by frame skipping
    u32 lists;
    u32 strips;
    u32 polyParams;
    u32 vertices;
    u32 verticesAtLastRender;
    u32 sprites;
    u32 modVols;
    // GPU T&L stand-in (--gpu-tnl): the depth range its lists asked for.
    float frameNear, frameFar;  // this frame so far
    u32 depthFrames, nearPinned, nearUnder1;   // renders; near at the 0.01 plane; near < 1
    float nearMin, nearMax, farMax;
};
extern HostRenderStats g_host_render;

// Called by the renderer at every emulated VBlank (the runner stops the CPU
// from here after the requested number of frames).
void host_on_vblank();

// Guest memory blocks for the Windows build: zeroed, 64 KB aligned and
// write-watched, so the runner can report which 4 KB pages the game has
// written (what the Wii's demand pager would have to back).
void* host_GuestAlloc(u32 size);
u32 host_GuestWrittenPages(const void* base, u32 size);
// Pages of a block (offsets >> 12, at most `cap`) written since the previous
// call, which resets the watch; host_GuestWrittenPages still counts them.
u32 host_GuestTakeWritten(const void* base, u32 size, u32* pages, u32 cap);

// Software rasteriser (host/soft_renderer.cpp): when enabled, every `every`
// emulated frames the next render is drawn and saved as
// <dir>/frame_<vblank>.png (640x480). 0 disables it.
void host_SetSnapshots(const char* dir, u32 every);
// Draws and saves the next render regardless of the period.
void host_RequestSnapshot();

// Texture cache census (host/texcache_census.cpp, env IDAS3_TEXCACHE): gxRend's
// cache rules replayed on every textured header of a drawn render.
void texcCensusBind(u32 pcw, u32 tsp, u32 tcw);
void texcCensusRender(bool dropped);
void texcCensusExit();
