// GPU (GX) transform & lighting for NAOMI 2 ELAN polygon lists - Wii only,
// prototype ("3D T&L: GPU" on the launch page).
//
// The software path (naomi_elan_tnl.cpp) transforms and lights every vertex
// on the CPU and feeds the result to the PVR's TA, which nullDC4Wii's GX
// renderer then decodes again: ~150K vertices a race frame, far too much
// for Broadway. Here an eligible list is instead recorded as it arrives
// (vertex pointer + snapshots of matrix, projection, lights, material) and
// inserted into the renderer's strip stream as a zero-vertex strip carrying
// the same PVR polygon parameters the CPU path would send. When gxRend draws
// that strip, it applies the polygon state exactly as for any other strip
// (texture, TEV shading, blend, depth...) and calls elan_gx_draw(), which
// streams the untransformed vertices with the instance matrix as the GX
// position matrix, GX hardware lights, and a projection that maps onto
// gxRend's own (same screen position and depth mapping as the TA path).
//
// Scope: opaque, translucent and punch-through lists, parallel lights,
// point lights as GX local lights (NIGHT LIGHTS = GPU, approximate),
// diffuse lighting (the PVR offset/specular colour is off by default in
// gxRend anyway), environment maps (texgen from the normal), the five plain
// vertex layouts. Everything else keeps the CPU path.
#pragma once

#include "types.h"

struct ElanGxXform
{
    float tm[12];               // instance matrix, tm00 tm01 tm02 tm10 ... tm32
    float lm[9];                // lighting (normal) matrix, row major
    float proj[4];              // fx tx fy ty
    float env[2];               // environment map offsets (envU, envV)
};

struct ElanGxLight
{
    float dir[3];               // unit vector towards the light (lighting space); local: spot axis
    float color[3];             // already x2 (the ELAN's light scale), clamped
    // Local (point) lights only, drawn with GX_AF_SPOT: position in lighting
    // space, angle (a0 a1 a2) and distance (k0 k1 k2) attenuation.
    float pos[3];
    float angle[3], dist[3];
};

struct ElanGxLightSet
{
    bool lit;
    u8 count;                   // <= 8 (GX hardware lights)
    u8 localMask;               // lights that are local (point) lights
    float ambient[3];
    ElanGxLight lights[8];
};

struct ElanGxRecord
{
    const u32* vtx;             // ICH vertex records (guest ELAN RAM, read at draw time)
    u32 count, vsize, layout;
    u32 pcw, isp, tsp, tcw;     // the PVR polygon parameters the CPU path would send
    bool colorFromVertex;       // layouts 0x042 / 0x04A
    bool envMap;                // UVs from the normal (environment map), not the vertex
    float base[4];              // material colour otherwise (RGBA 0..1)
    float nearW, farW;          // view depth range of its vertices in front of the camera
    // Change counters of the state behind xform / lights (0 = unknown): equal
    // keys mean equal contents, so the copies are shared without comparing.
    u32 xformKey, lightKey;
    ElanGxXform xform;
    ElanGxLightSet lights;
};

// Whether the GPU path is on (launch option) and has room this frame.
bool elan_gx_enabled();
// Records one list into the renderer's stream; false = use the CPU path.
bool elan_gx_record(const ElanGxRecord& r);
// The list's bounding box in model space (min x y z, max x y z; empty when
// min > max), kept with its GPU fetch program while its pages stay unwritten
// (naomi_elan_fetch.inc). False: none now. Asked when the list is recorded.
bool elan_gx_list_box(const u32* vtx, u32 count, u32 vsize, float box[6]);
// The renderer's canvas (screen coordinates of the ELAN's projection), from
// its last 3D render; 0 before the first one.
void elan_gx_canvas(float& width, float& height);
// The render the ELAN is recording, and whether ELAN RAM [p, p + bytes) was
// left unwritten since render `since` (checks its pages for this render).
u32 elan_gx_render();
bool elan_gx_unwritten_since(const void* p, u32 bytes, u32 since);
#if !defined(GEKKO)
void elan_gx_box_check(const u32* vtx, u32 count, u32 vsize, const float box[6]);   // the runner's stale box check
#endif

#if !defined(GEKKO)
// Windows runner: once per drawn render (soft_renderer.cpp StartRender), the
// census of what the Wii's GPU-fetch draw would cover.
void elan_gx_census_render();
#endif

#if defined(GEKKO)
// ---- gxRend.cpp side --------------------------------------------------------
struct ElanGxFrame
{
    float width, height;        // dc_width / dc_height (canvas)
    float p5, p6;               // depth mapping of the frame
    float proj[4][4];           // gxRend's projection, restored after each item
    bool clr1;                  // gxRend's vertex format carries CLR1 this frame
};
void elan_gx_frame_reset();     // the renderer restarts its lists (new frame)
void elan_gx_draw(u32 item, const ElanGxFrame& f, bool forceOpaqueAlpha);
// Puts gxRend's GX state back after a run of elan_gx_draw() calls (no-op
// otherwise). The renderer calls it before it draws anything itself.
void elan_gx_restore(const ElanGxFrame& f);
#endif

// Implemented by the renderer: appends a zero-vertex strip with these PVR
// polygon parameters, marked as ELAN item `item`, and widens the frame's
// tracked depth range to [nearW, farW]. False when full.
extern "C" bool rend_AddElanItem(u32 pcw, u32 isp, u32 tsp, u32 tcw, u32 item, float nearW, float farW);

// Launch option (game_runner.cpp): 0 = CPU T&L, 1 = GPU T&L prototype.
extern int g_elan_gx_preset;
// Launch option: 1 = the GPU fetches unchanged lists' vertices itself (Wii).
extern int g_elan_fetch_preset;
// Launch option DISPLAY LISTS: 1 = a cached list's index draws are written
// once as a GX display list the GPU reads itself (naomi_elan_fetch.inc), in a
// ring of this size that the guest page pool gives up (_vmem.cpp).
extern int g_elan_dl_preset;
constexpr u32 kElanDlRingBytes = 1024u * 1024u;
// Launch option COLOUR LISTS: 1 = vertex-coloured lists go through the GPU
// fetch too (as RGB8) when their vertex alpha is opaque or ignored.
extern int g_elan_colour_fetch_preset;
// Launch option NIGHT LIGHTS: 0 = lists lit by point lights stay on the CPU
// (exact), 1 = the GPU path lights them with GX local lights (approximate).
extern int g_elan_point_preset;
// Launch option 3D LIST CULL (GPU path, lists with a fetch program's box):
// 0 = off (every list recorded, depth measured as before), 1 = lists behind
// the camera dropped, 2 = also lists beside or above/below the picture, 3 =
// also whole models stepped over before their commands are walked.
extern int g_elan_box_preset;
