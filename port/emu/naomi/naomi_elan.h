// NAOMI 2 ELAN (area 2) - see naomi_elan.cpp and naomi_elan_cmd.cpp.
#pragma once

#include "types.h"

struct NaomiElanStats
{
    u32 regReads;
    u32 regWrites;
    u32 commands;       // FIFO commands executed
    u32 lists;          // display lists followed (links, models)
    u32 taBlocks;       // 32-byte TA parameters passed to the PVR
    u32 n2Commands;     // NAOMI 2 geometry/state commands
    u32 ichLists;       // NAOMI 2 polygon lists (ICH)
    u32 ichVertices;
    u32 tnlVertices;    // TA vertices the T&L emitted for them (strips, clipping)
    u32 litVertices;    // vertices that went through lighting
    u32 lightEvals;     // lights evaluated for them (sum over vertices)
    u32 ichSkipped;     // polygon lists not transformed: their frame is skipped
    u32 stateSkipped;   // matrix / light / material commands not decoded, same reason
    u32 ichHeldBack;    // lists of a drawn frame dropped: state they need was skipped
    // Model lists (N2 MODEL calls) on dropped frames, naomi_elan_cmd.cpp.
    u32 modelsWalked;       // model lists walked (each one's summary refreshed)
    u32 modelsSkipped;      // ...stepped over on a dropped frame, from the summary
    u32 modelListsSkipped;  // polygon lists inside those, not walked
    u32 modelsChanged;      // walked again with different commands (same address)
    u32 modelsFlipped;      // ...of which went from no side effects to some, or back
    u32 gpuVertices;    // vertices handed untransformed to the GPU path (naomi_elan_gx)
    // 3D LIST CULL (GPU path): lists recorded with their fetch program's box,
    // lists (and vertices) dropped by it behind the camera or out of the
    // picture, and vertices whose depth was measured one by one instead.
    u32 boxLists, boxCulledBehind, boxCulledSide, boxCulledVertices, depthMeasured;
    u32 modelsCulled, modelListsCulled;     // ...and whole models stepped over on drawn frames
    u32 modelCheckStale;                    // host: ...that walking them shows were wrongly summarised
    // Host census of those lists' strips (what the GPU path's index builder makes of them).
    u32 gpuIndices, gpuStrips, gpuFanVertices, gpuStripsWithFans, gpuPlaceholders;
    u32 gpuTriangles, gpuTriangleMismatches;    // ...and the per-strip draw checked against the joined strip
    // Host census of ICH vertex data (tnl_DataCensusRender), in vertices.
    u32 dataTotal, dataChangedBeforeRender, dataSameAddress, dataSameContent;
    // Host census, vertices: 0 opaque, 1 translucent, 2 punch-through,
    // 3 env-mapped, 4 lit with specular, 5 lit by a point light, 6 bump,
    // 7 opaque with none of 3-6 (what a first GX T&L path can take).
    u32 census[8];
    u32 texDmaBytes;    // texture DMAs into VRAM
    u32 waits;          // register waits (list-end interrupts)
    u32 errors;         // malformed commands skipped
    // Time spent: ns on the host, time base ticks on the Wii (elan_TimeMs()).
    u64 execNs;         // all ELAN command execution
    u64 tnlNs;          // ...of which transform & lighting (tnl_DrawList)
    u64 taNs;           // ...of which feeding the TA (inside and outside T&L)
    u64 convNs;         // ...of which the per-vertex loop (transform, lighting, strip assembly)
    u64 gpuNs;          // ...of which recording lists for the GPU path
};

#if !defined(GEKKO)
#include <chrono>
inline u64 elan_NowNs()
{
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline double elan_TimeMs(u64 t)
{
    return t / 1e6;
}
#else
#include <ogc/lwp_watchdog.h>
// The raw time base (one mftb, a few calls per list); converted when printed.
inline u64 elan_NowNs()
{
    return gettime();
}
inline double elan_TimeMs(u64 t)
{
    return t / (TB_TIMER_CLOCK * 1.0);   // TB_TIMER_CLOCK: ticks per millisecond
}
#endif
#define ELAN_TIME_BEGIN(v) const u64 v = elan_NowNs()
#define ELAN_TIME_END(field, v) (elan_Stats().field += elan_NowNs() - (v))

// Allocates ELAN RAM (rounded to a power of two, at most 32 MB; a smaller
// buffer is mirrored over the window). Must run before the memory map.
bool naomi_ElanAlloc(u32 bytes);
void naomi_ElanReset();
// Called from dc/mem/sh4_mem.cpp's area-2 hooks in the NAOMI 2 build.
void naomi_ElanMapInit();
void naomi_ElanMap(u32 base);
// Advances ELAN timing (DMA completion); called from the SH-4 update cascade.
void naomi_ElanUpdate(u32 cycles);

const NaomiElanStats& naomi_ElanStats();
u8* naomi_ElanRam();
u32 naomi_ElanRamSize();
void naomi_ElanLogRecent();      // bring-up: the last 16 FIFO commands

// ---- Internal: shared by naomi_elan.cpp and naomi_elan_cmd.cpp -------------
NaomiElanStats& elan_Stats();

// Implemented by the renderer (gxRend.cpp on the Wii, soft_renderer.cpp on
// the host): whether the frame now being built will be dropped by frame
// skipping. Asking takes the verdict early; StartRender then uses the same one.
extern "C" bool rend_NextFrameSkipped();
// Also the renderer's: a MODEL command's PCW bits 16-17 (user clip: 0 off,
// 2 inside, 3 outside the TA's User Tile Clip rectangle) become the TA's tile
// clip mode for the polygons that follow, as a Group_En polygon header would.
extern "C" void rend_SetTileClipMode(u32 mode);
// Launch option DROPPED 3D: 1 = dropped frames step over model lists without
// side effects (naomi_elan_cmd.cpp), 0 = they walk everything.
extern "C" int g_elan_model_skip_preset;
// Executes a command stream (the 32-byte FIFO entry, or a display list in
// ELAN RAM). `words` are host-order 32-bit values, as both buffers hold them.
void elan_Execute(const u32* words, u32 bytes);
// elan_Execute calls so far: the game only writes ELAN RAM between them.
extern u32 g_elan_exec_seq;
void elan_CmdReset();
// A texture DMA has started; status bit 0 is raised `cycles` later.
void elan_StartDma(u32 cycles);
bool elan_DmaBusy();

// ---- Internal: transform & lighting (naomi_elan_tnl.cpp) --------------------
// Each takes the command's words (host order) as found in the stream.
void tnl_SetProjection(const u32* w);
void tnl_SetMatrix(const u32* w);            // 160-byte instance matrix
void tnl_SetLightModel(const u32* w);
void tnl_SetLight(const u32* w);
void tnl_SetMaterial(const u32* w);          // GMP, 64 bytes
void tnl_BeginModel(const u32* w);
void tnl_EndModel();
void tnl_ResetState();                       // end of frame (register wait)
// A model list stepped over on a dropped frame held this many matrix / light /
// material commands: what skipping them one by one would have left behind.
void tnl_SkipModelState(u32 stateCommands);
// 3D LIST CULL, whole models on a drawn frame (naomi_elan_cmd.cpp). Whether a
// model-space box is out of the picture under the current matrix and cannot
// reach its near plane (it then has no say in the depth pin), and what stepping
// over such a model leaves behind: its last material, its slots in the depth
// slices (tnl_ListsIndexed counts them).
bool tnl_BoxHidden(const float* box);
void tnl_ModelHidden(const u32* lastMaterial, u32 listsIndexed);
u32 tnl_ListsIndexed();
// What tnl_DrawList knew of its list's box: copied to box[6] when kListBoxed.
enum { kListNoBox, kListBoxed, kListNotDrawn };
int tnl_ListBox(float* box);
// Transforms one polygon list and feeds the result to the TA. `listType` is
// the TA's open list (-1 if none); returns the list type now open.
int tnl_DrawList(const u32* w, u32 vertexSize, int listType);
void tnl_OnStartRender();                    // at every STARTRENDER: the frame's lists are in
#if !defined(GEKKO)
void tnl_DataCensusRender();                 // host: at every STARTRENDER
#endif
