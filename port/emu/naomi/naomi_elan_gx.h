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

// HEADLIGHTS = SMOOTH: a car's headlight on a list it crosses the edge of. The
// ELAN lights per vertex, and a headlight's cone goes from full to nothing in
// ~10 degrees, so on a big road polygon each corner is lit or not and the
// polygon shows. Here the cone is a texture projected from the lamp (one
// texgen, sampled per pixel), times colour channel 1 lit per vertex by this
// light without its cone (colour, distance, N.L, and nothing behind the lamp).
struct ElanGxBeam
{
    float pos[3];               // the lamp, lighting space
    float axis[3];              // unit vector, the way it shines
    float color[3];             // GX light colour (<= 1)
    float scale;                // what the colour lost to stay <= 1, carried by the angle term
    float dist[3];              // distance attenuation k0 k1 k2 (as a local light's)
    float coneP, coneQ;         // the ELAN's cone term clamp(p + q c), c the cosine to the axis
    // A list reaching behind the lamp's plane (the road around the car): the
    // projection mirrors there, so the beam is also multiplied by a front mask,
    // 0 behind the plane rising to 1 over this distance in front (0: no mask).
    float mask;
};
constexpr u32 kElanBeamTex = 64;            // the cone texture's side, texels
constexpr u32 kElanBeamMaskW = 16, kElanBeamMaskH = 4;   // the mask texture: a step at its middle
// Tangent of the half angle the cone texture spans (its zero ring with a margin).
float elan_beam_tan(const ElanGxBeam& b);
// The cone texture, kElanBeamTex^2 bytes, row-major (the GX upload tiles it).
void elan_beam_texture(const ElanGxBeam& b, u8* texels);
// The texture matrix: model-space position (the list's vertices) to the cone
// texture's (s t q), s/q and t/q in 0..1 across it.
void elan_beam_texmtx(const ElanGxXform& x, const ElanGxBeam& b, float m[3][4]);
// The mask texture (kElanBeamMaskW x kElanBeamMaskH bytes, row-major) and its
// matrix: model-space position to (s t), s the distance along the axis.
void elan_beam_mask_texture(u8* texels);
void elan_beam_maskmtx(const ElanGxXform& x, const ElanGxBeam& b, float m[2][4]);

struct ElanGxLightSet
{
    bool lit;
    u8 count;                   // <= 8 (GX hardware lights); <= 7 with a beam
    u8 localMask;               // lights that are local (point) lights
    u8 beam;                    // 1: beamLight is drawn as a projected cone (GX light 7)
    float ambient[3];
    ElanGxLight lights[8];
    ElanGxBeam beamLight;
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
    // DRAW MEMO: the list's GPU fetch slot + 1 as found with its box this
    // render (elan_gx_list_box), 0: none. Not part of what is drawn.
    u32 fetchSlot;
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
// *slot: where its program sits (for DRAW MEMO), set only when true.
bool elan_gx_list_box(const u32* vtx, u32 count, u32 vsize, float box[6], u32* slot);
// Launch option DRAW MEMO (naomi_elan_gx.cpp): 1 = the draw takes a list's
// program from the slot found when it was recorded, and its normal matrix's
// scale without libm's sqrtf.
extern "C" int g_draw_memo_preset;
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
// True when the item borrowed TEV stages 1-3 and the texgen count (HEADLIGHTS
// SMOOTH): the renderer then sets its own stages 1+ and their count back.
bool elan_gx_draw(u32 item, const ElanGxFrame& f, bool forceOpaqueAlpha);
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
// Launch option HEADLIGHTS (the cars' headlights at night): 0 ARCADE (per
// vertex, as the ELAN), 1 SOFT (per vertex, a wider and softer cone), 2 SMOOTH
// (GPU path: the cone projected per pixel on lists crossing its edge).
extern int g_headlight_preset;
// Launch option 3D LIST CULL (GPU path, lists with a fetch program's box):
// 0 = off (every list recorded, depth measured as before), 1 = lists behind
// the camera dropped, 2 = also lists beside or above/below the picture, 3 =
// also whole models stepped over before their commands are walked.
extern int g_elan_box_preset;
