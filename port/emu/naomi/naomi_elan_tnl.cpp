// NAOMI 2 ELAN transform and lighting, done on the CPU.
//
// Polygon lists (ICH) carry model-space vertices. The ELAN transforms them
// with the current instance matrix, lights them with the current material
// and lights, projects them and hands ordinary polygons to the CLX2. This
// file does the same and feeds the result to the PVR's TA as standard
// polygon parameters (packed-colour header, 32-byte vertices), so whatever
// renders the TA's output - the Windows soft renderer or the Wii's GX
// renderer - draws NAOMI 2 geometry with no special path.
//
// The maths follow the behaviour Flycast established for the hardware
// (its GPU shader computes the same terms per vertex):
//   view  X = tm00 x + tm10 y + tm20 z + tm30   (Y, Z likewise, columns 1, 2)
//   screen x = fx X / Z + tx,  y = fy Y / Z + ty,  TA z = 1 / Z
// Lighting is evaluated in the space Flycast uses (view X and Z negated),
// with the light directions as the game supplies them.
//
// Not done: modifier volumes in NAOMI 2 form, two-volume (shadow) polygons
// beyond volume 0, bump mapping, far-plane culling.
#include "types.h"
#include "plugins/plugin_manager.h"
#include "naomi.h"
#include "naomi_elan.h"
#include "naomi_elan_gx.h"

extern int g_trans_sort_preset;   // renderer presets (launch page)
extern int g_autosort_preset;

#include <cmath>
#include <cstring>
#include <vector>
#if !defined(GEKKO)
#include <unordered_map>
#include <unordered_set>
#endif

#if !defined(BUILD_NAOMI2)
#error "naomi_elan_tnl.cpp is only part of the NAOMI 2 build"
#endif

namespace {

struct Vec3
{
    float x, y, z;
};

inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

inline float asFloat(u32 w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
}

inline u32 asWord(float f)
{
    u32 w;
    memcpy(&w, &f, 4);
    return w;
}

// The T&L runs per vertex for ~60K vertices a frame in a race, and the Wii's
// Broadway has no square root instruction (libm's is software) and a slow
// pow(): these are the fast forms. x > 0.
inline float rsqrt(float x)
{
#if defined(GEKKO)
    float y;
    asm("frsqrte %0,%1" : "=f"(y) : "f"(x));      // ~5-bit estimate
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);           // ~20 bits
    return y;
#else
    return 1.f / std::sqrt(x);
#endif
}

inline Vec3 normalize(Vec3 v)
{
    const float l2 = dot(v, v);
    return l2 > 0.f ? v * rsqrt(l2) : v;
}

// x^g for x in (0, 1], g > 0, through 2-term log2 / exp2 approximations
// (~0.5% error: invisible on a specular highlight, ~10x cheaper than pow).
inline float fastPow(float x, float g)
{
    const u32 i = asWord(x);
    const float m = asFloat((i & 0x007FFFFFu) | 0x3F800000u);    // mantissa in [1, 2)
    const float lg = (float)((int)(i >> 23) - 127) + (-0.34484843f * m + 2.02466578f) * m - 1.67487759f;
    float y = g * lg;
    if (y < -126.f)
        return 0.f;
    if (y > 0.f)
        y = 0.f;
    int k = (int)y;                               // truncates towards 0: floor for y <= 0
    if ((float)k > y)
        k--;
    const float f = y - (float)k;                 // [0, 1)
    const float p = 1.f + f * (0.6565f + 0.3435f * f);
    return asFloat(asWord(p) + ((u32)k << 23));
}

void unpackColor(u32 argb, float* out)
{
    out[0] = ((argb >> 16) & 0xFF) / 255.f;
    out[1] = ((argb >> 8) & 0xFF) / 255.f;
    out[2] = (argb & 0xFF) / 255.f;
    out[3] = (argb >> 24) / 255.f;
}

u32 packColor(const float* c)
{
    return (u32)(clamp01(c[3]) * 255.f + 0.5f) << 24 | (u32)(clamp01(c[0]) * 255.f + 0.5f) << 16 |
           (u32)(clamp01(c[1]) * 255.f + 0.5f) << 8 | (u32)(clamp01(c[2]) * 255.f + 0.5f);
}

// 12-bit light direction: 8 high bits in the light word, 4 low bits in the PCW.
float lightDir(u32 hi8, u32 lo4)
{
    return (float)(((int)(s8)hi8 << 4) | (int)lo4) / 2047.f;
}

float half(u32 v16)
{
    return asFloat(v16 << 16);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Light
{
    bool defined = false;
    bool parallel = true;
    float color[3] = {};
    Vec3 dir = {};              // towards the light (parallel and spot)
    Vec3 dirN = {};             // normalize(dir), set with the light
    Vec3 pos = {};
    u32 routing = 0, dmode = 0, smode = 0;
    bool distMode = false;
    bool distAtten = false, angleAtten = false;   // distA/B, angleA/B not identity
    float distA = 1, distB = 0, angleA = 1, angleB = 0;
};

struct LightModel
{
    bool set = false;
    bool ambientBaseMaterial[2] = {}, ambientOffsetMaterial[2] = {};
    bool baseOver = false;
    u16 diffuseMask[2] = {}, specularMask[2] = {};
    float ambientBase[2][4] = {}, ambientOffset[2][4] = {};
};

struct Material
{
    bool set = false;
    u32 select = 0;             // d0 s0 a0 f0 d1 s1 a1 f1 v1uv0 b0 b1 e0 e1
    float gloss[2] = {1, 1};
    float diffuse[2][4] = {}, specular[2][4] = {};
};

struct Matrix
{
    bool set = false;
    float tm[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};   // tm00 tm01 tm02 tm10 ... tm32
    float lm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};             // lm00 lm10 lm20 lm01 ... lm22
    float nearZ = 0.001f, farZ = 100000.f;
    float envU = 0, envV = 0;
};

struct State
{
    float proj[4] = {579.411194f, -320.f, -579.411194f, -240.f};   // fx tx fy ty
    Matrix matrix;
    Material material;
    LightModel lightModel;
    Light lights[16];
    // Set by the model command for the duration of its list.
    u32 modelTsp = 0;
    bool cullReversed = false;
    bool shadow = false;
    // Lights used by each volume (diffuse or specular mask and defined),
    // rebuilt when a light or the light model changes.
    bool lightsDirty = true;
    u8 activeCount[2] = {};
    u8 active[2][16] = {};
    bool anySpecular[2] = {};
};

State s;

// Change counters, outside State (tnl_ResetState rebuilds it): matrix and
// projection; lights, light model and material. recordForGpu keeps its last
// copies of those while they stay the same.
u32 s_xformGen = 1, s_lightGen = 1;

// ---------------------------------------------------------------------------
// Vertices
// ---------------------------------------------------------------------------
struct TnlVtx
{
    float X, Y, Z;              // view space
    float u, v;
    float base[4], offs[4];
};

void rebuildActiveLights()
{
    const LightModel& lm = s.lightModel;
    for (int vol = 0; vol < 2; vol++)
    {
        u8 n = 0;
        bool spec = false;
        for (int i = 0; i < 16; i++)
        {
            const bool d = lm.diffuseMask[vol] >> i & 1, sp = lm.specularMask[vol] >> i & 1;
            // A black light adds nothing (every term of lighting() is scaled by
            // its colour): leave it out. Initial D's car select keeps a black
            // spotlight on, which cost every car vertex a point-light evaluation
            // and kept the whole screen off the GPU path (point light).
            const float* c = s.lights[i].color;
            if ((d || sp) && s.lights[i].defined && (c[0] != 0.f || c[1] != 0.f || c[2] != 0.f))
            {
                s.active[vol][n++] = (u8)i;
                spec |= sp;
            }
        }
        s.activeCount[vol] = n;
        s.anySpecular[vol] = spec;
    }
    s.lightsDirty = false;
}

void lighting(float* base, float* offs, int vol, Vec3 P, Vec3 N)
{
    const LightModel& lm = s.lightModel;
    if (!lm.set)
        return;
    if (s.material.set && (s.material.select >> (9 + vol) & 1))
        return;                                   // constant colour
    if (s.lightsDirty)
        rebuildActiveLights();
    float diffuse[3] = {}, specular[3] = {};
    float diffuseAlpha = 0, specularAlpha = 0;
    Vec3 R = {};
    if (s.anySpecular[vol])
    {
        const Vec3 I = normalize(P);
        R = I - N * (2.f * dot(N, I));
    }
    const float gloss = s.material.set ? s.material.gloss[vol] : 1.f;
    elan_Stats().litVertices++;
    elan_Stats().lightEvals += s.activeCount[vol];

    for (u32 k = 0; k < s.activeCount[vol]; k++)
    {
        const int i = s.active[vol][k];
        const bool doDiffuse = lm.diffuseMask[vol] >> i & 1;
        const bool doSpecular = lm.specularMask[vol] >> i & 1;
        const Light& l = s.lights[i];
        float color[3] = {l.color[0], l.color[1], l.color[2]};
        Vec3 L;
        if (l.parallel)
            L = l.dirN;
        else
        {
            const Vec3 toLight = l.pos - P;
            const float d2 = dot(toLight, toLight);
            const float inv = d2 > 0.f ? rsqrt(d2) : 0.f;   // 1 / distance
            L = toLight * inv;
            if (l.distAtten)
            {
                const float d = l.distMode ? d2 * inv : inv;  // distance, or its inverse
                const float a = clamp01(l.distB * d + l.distA);
                for (float& c : color)
                    c *= a;
            }
            if (l.angleAtten)
            {
                const float cosAngle = 1.f - std::fmax(0.f, dot(L, l.dir));
                const float a = clamp01(cosAngle * l.angleB + l.angleA);
                for (float& c : color)
                    c *= a;
            }
        }
        const float sign = (l.routing & 8) ? -2.f : 2.f;
        if (doDiffuse)
        {
            float f = sign;
            if (l.dmode == 0)
                f *= std::fmax(dot(N, L), 0.f);
            else if (l.dmode == 1)
                f *= std::fabs(dot(N, L));
            if (l.routing & 4)
                diffuseAlpha += color[0] * f;
            else
                for (int c = 0; c < 3; c++)
                    ((l.routing & 2) ? specular : diffuse)[c] += color[c] * f * base[c];
        }
        if (doSpecular)
        {
            float f = sign;
            const float lr = dot(L, R);
            const float x = l.smode == 0 ? lr : l.smode == 1 ? std::fabs(lr) : 1.f;
            if (l.smode <= 1)
                f *= x > 0.f ? clamp01(fastPow(x, gloss)) : 0.f;
            if (l.routing & 4)
                specularAlpha += color[0] * f;
            else
                for (int c = 0; c < 3; c++)
                    ((l.routing & 1) ? specular : diffuse)[c] += color[c] * f * offs[c];
        }
    }
    for (int c = 0; c < 3; c++)
    {
        diffuse[c] += lm.ambientBaseMaterial[vol] ? lm.ambientBase[vol][c] * base[c] : lm.ambientBase[vol][c];
        specular[c] += lm.ambientOffsetMaterial[vol] ? lm.ambientOffset[vol][c] * offs[c] : lm.ambientOffset[vol][c];
        base[c] = diffuse[c];
        offs[c] = specular[c];
    }
    base[3] += diffuseAlpha;
    offs[3] += specularAlpha;
    if (lm.baseOver)
        for (int c = 0; c < 4; c++)
            offs[c] += std::fmax(base[c] - 1.f, 0.f);
    for (int c = 0; c < 4; c++)
    {
        base[c] = clamp01(base[c]);
        offs[c] = clamp01(offs[c]);
    }
}

// Converts one ICH vertex. `layout` is the list's vertex format flags.
TnlVtx convert(const u32* v, u32 layout, bool textured, bool envMap, bool& lit)
{
    const Matrix& m = s.matrix;
    const float x = asFloat(v[1]), y = asFloat(v[2]), z = asFloat(v[3]);
    TnlVtx o;
    o.X = m.tm[0] * x + m.tm[3] * y + m.tm[6] * z + m.tm[9];
    o.Y = m.tm[1] * x + m.tm[4] * y + m.tm[7] * z + m.tm[10];
    o.Z = m.tm[2] * x + m.tm[5] * y + m.tm[8] * z + m.tm[11];

    Vec3 n;
    if (layout == 0x00E)
        n = {asFloat(v[4]), asFloat(v[5]), asFloat(v[6])};
    else
        n = {(s8)(v[0] & 0xFF) / 127.f, (s8)((v[0] >> 8) & 0xFF) / 127.f, (s8)((v[0] >> 16) & 0xFF) / 127.f};
    const Vec3 N = normalize({m.lm[0] * n.x + m.lm[1] * n.y + m.lm[2] * n.z,
                              m.lm[3] * n.x + m.lm[4] * n.y + m.lm[5] * n.z,
                              m.lm[6] * n.x + m.lm[7] * n.y + m.lm[8] * n.z});

    for (int c = 0; c < 4; c++)
    {
        o.base[c] = 1.f;
        o.offs[c] = 0.f;
    }
    o.u = o.v = 0.f;
    switch (layout)
    {
    case 0x00A: o.u = asFloat(v[4]), o.v = asFloat(v[5]); break;
    case 0x00E: o.u = asFloat(v[8]), o.v = asFloat(v[9]); break;
    case 0x042: unpackColor(v[4], o.base); break;
    case 0x04A: o.u = asFloat(v[4]), o.v = asFloat(v[5]); unpackColor(v[6], o.base); break;
    case 0x10A: o.u = asFloat(v[4]), o.v = asFloat(v[5]); break;
    default: break;
    }
    if (s.material.set)
    {
        if (s.material.select & 1)
            memcpy(o.base, s.material.diffuse[0], sizeof(o.base));
        if (s.material.select & 2)
            memcpy(o.offs, s.material.specular[0], sizeof(o.offs));
    }

    const Vec3 P = {-o.X, o.Y, -o.Z};             // Flycast's view space
    if (s.lightModel.set && !(s.material.set && (s.material.select >> 9 & 1)))
    {
        lighting(o.base, o.offs, 0, P, N);
        lit = true;
        if (!textured)
            for (int c = 0; c < 4; c++)
                o.base[c] = clamp01(o.base[c] + o.offs[c]);
    }
    if (envMap)
    {
        o.u = clamp01(m.envU + N.x / 2.f + 0.5f);
        o.v = clamp01(m.envV + N.y / 2.f + 0.5f);
    }
    return o;
}

TnlVtx lerp(const TnlVtx& a, const TnlVtx& b, float t)
{
    TnlVtx o;
    o.X = a.X + (b.X - a.X) * t;
    o.Y = a.Y + (b.Y - a.Y) * t;
    o.Z = a.Z + (b.Z - a.Z) * t;
    o.u = a.u + (b.u - a.u) * t;
    o.v = a.v + (b.v - a.v) * t;
    for (int c = 0; c < 4; c++)
    {
        o.base[c] = a.base[c] + (b.base[c] - a.base[c]) * t;
        o.offs[c] = a.offs[c] + (b.offs[c] - a.offs[c]) * t;
    }
    return o;
}

// ---------------------------------------------------------------------------
// TA output
// ---------------------------------------------------------------------------
struct TaWriter
{
    std::vector<u32> words;
    u32 header[8];
    bool textured = false;

    void begin(u32 pcw, u32 isp, u32 tsp, u32 tcw)
    {
        header[0] = pcw;
        header[1] = isp;
        header[2] = tsp;
        header[3] = tcw;
        header[4] = header[5] = header[6] = header[7] = 0;
        textured = (pcw >> 3) & 1;
    }

    // At most this many vertices per polygon header. Renderers check their
    // buffers once per header (nullDC4Wii's GX renderer keeps 4K vertices of
    // headroom), so one huge strip must not arrive in a single piece. Even,
    // so each continuation starts on an even vertex and keeps its winding.
    static constexpr u32 kMaxStripVertices = 1024;

    void strip(const TnlVtx* v, u32 count)
    {
        while (count > kMaxStripVertices)
        {
            piece(v, kMaxStripVertices);
            v += kMaxStripVertices - 2;            // the next piece repeats two vertices
            count -= kMaxStripVertices - 2;
        }
        piece(v, count);
    }

    void piece(const TnlVtx* v, u32 count)
    {
        if (count < 3)
            return;
        elan_Stats().tnlVertices += count;
        words.insert(words.end(), header, header + 8);
        const float fx = s.proj[0], tx = s.proj[1], fy = s.proj[2], ty = s.proj[3];
        for (u32 i = 0; i < count; i++)
        {
            const float invZ = 1.f / v[i].Z;
            const u32 pcw = 0xE0000000u | (i + 1 == count ? 0x10000000u : 0u);
            words.push_back(pcw);
            words.push_back(asWord(fx * v[i].X * invZ + tx));
            words.push_back(asWord(fy * v[i].Y * invZ + ty));
            words.push_back(asWord(invZ));
            if (textured)
            {
                words.push_back(asWord(v[i].u));
                words.push_back(asWord(v[i].v));
                words.push_back(packColor(v[i].base));
                words.push_back(packColor(v[i].offs));
            }
            else
            {
                words.push_back(0);
                words.push_back(0);
                words.push_back(packColor(v[i].base));
                words.push_back(0);
            }
        }
    }

    void flush()
    {
        if (!words.empty())
        {
            ELAN_TIME_BEGIN(tt);
            libPvr_TaDMA(words.data(), (u32)words.size() / 8);
            ELAN_TIME_END(taNs, tt);
            elan_Stats().taBlocks += (u32)words.size() / 8;
            words.clear();
        }
    }
};

TaWriter s_out;
std::vector<TnlVtx> s_strip;
std::vector<TnlVtx> s_clipped;

// Appends strip `v` to `out`, joined by degenerate triangles. The first real
// vertex lands on an even index so its triangles keep their winding. One
// polygon header per list instead of one per piece keeps the renderer's
// strip/param tables small (the GX renderer has 8K of each per frame).
void linkStrip(std::vector<TnlVtx>& out, const TnlVtx* v, u32 n)
{
    if (!out.empty())
    {
        out.push_back(out.back());
        out.push_back(v[0]);
        if (out.size() & 1)
            out.push_back(v[0]);
    }
    out.insert(out.end(), v, v + n);
}

// Triangle (a, b, c) clipped against Z >= nearZ, appended to s_clipped.
void clippedTriangle(const TnlVtx& a, const TnlVtx& b, const TnlVtx& c, float nearZ)
{
    const TnlVtx* in[3] = {&a, &b, &c};
    TnlVtx poly[4];
    u32 n = 0;
    for (int i = 0; i < 3; i++)
    {
        const TnlVtx& p = *in[i];
        const TnlVtx& q = *in[(i + 1) % 3];
        const bool pIn = p.Z >= nearZ, qIn = q.Z >= nearZ;
        if (pIn)
            poly[n++] = p;
        if (pIn != qIn && n < 4)
            poly[n++] = lerp(p, q, (nearZ - p.Z) / (q.Z - p.Z));
    }
    if (n == 3)
        linkStrip(s_clipped, poly, 3);
    else if (n == 4)
    {
        const TnlVtx quad[4] = {poly[0], poly[1], poly[3], poly[2]};
        linkStrip(s_clipped, quad, 4);
    }
}

// GPU lists' depth measurement (recordForGpu), per frame.
struct DepthPin
{
    bool pinned;            // a list of this frame reached its near plane
    bool pinnedLastFrame;
    bool windowHit;         // pinned: a list measured in this window reached its near plane
    float pinPlane;         // nearest near plane reached this frame
    float pinPlaneLastFrame;
    u32 frames;             // renders that recorded GPU lists
    u32 lists;              // GPU lists recorded this render (dropped ones too)
    u32 windowRenders;      // pinned: renders in the current window (kDepthWindow)
};
DepthPin s_depth = {false, false, false, 1e30f, 1e30f, 0, 0, 0};

// While pinned, each list is still measured one render in this many.
constexpr u32 kDepthWindow = 32;

// The list's state as an ElanGxRecord for the GPU path, or false when the
// GX hardware cannot reproduce its lighting (point lights, unusual routing,
// more than 8 lights, a layout without a plain vertex format).
bool recordForGpu(const u32* w, u32 vertexSize, u32 count, u32 layout, u32 pcw, u32 isp, u32 tsp, bool envMap)
{
    if (layout != 0x002 && layout != 0x00A && layout != 0x00E && layout != 0x042 && layout != 0x04A)
        return false;
    static ElanGxRecord r;
    r.vtx = w + 8;
    r.count = count;
    r.vsize = vertexSize;
    r.layout = layout;
    r.pcw = pcw;
    r.isp = isp;
    r.tsp = tsp;
    r.tcw = w[3];
    const bool matBase = s.material.set && (s.material.select & 1);
    r.colorFromVertex = (layout == 0x042 || layout == 0x04A) && !matBase;
    for (int c = 0; c < 4; c++)
        r.base[c] = matBase ? s.material.diffuse[0][c] : 1.f;
    r.envMap = envMap;
    const u32 xformKey = s_xformGen * 2u + (envMap ? 1u : 0u);
    if (r.xformKey != xformKey)
    {
        memcpy(r.xform.tm, s.matrix.tm, sizeof(r.xform.tm));
        memcpy(r.xform.lm, s.matrix.lm, sizeof(r.xform.lm));
        memcpy(r.xform.proj, s.proj, sizeof(r.xform.proj));
        r.xform.env[0] = envMap ? s.matrix.envU : 0.f;   // 0 otherwise: keeps the matrix copies shared
        r.xform.env[1] = envMap ? s.matrix.envV : 0.f;
        r.xformKey = xformKey;
    }

    // The list's view depth range (one dot product per vertex): the renderer
    // fits its depth range to the scene, and only the CPU sees these depths.
    // GX has a 24-bit Z buffer where the PVR's was float, so a guessed range
    // costs visible precision (z-fighting). Nothing in front: nothing to draw.
    //
    // In a race some list always reaches the matrix's near plane (the road
    // under the camera), and the frame's range can then go no lower: after
    // such a frame, lists whose own near plane is no nearer than the one
    // reached use their planes as they are. Each is still measured one render
    // in kDepthWindow, a different slice of the lists each render (by their
    // index), and the pin drops at the end of a window in which none of them
    // reached its plane. Measuring every list in the same render instead cost
    // that render ~11 ms on a Wii (115K vertices, a cache miss each). Never clips
    // anything (no list's range starts nearer than its nearZ); lists with a
    // nearer plane (or none: 0.001) are still measured, so they don't drag
    // the frame's near plane, i.e. its depth precision, down for nothing. The
    // menus, whose nearest model sits metres away, keep measuring.
    const float nearZ = s.matrix.nearZ > 0.f ? s.matrix.nearZ : 0.001f;
    // Every list takes an index, dropped ones too: otherwise each list after a
    // measured one found behind the camera got the same index and was measured
    // as well, i.e. the whole stretch of track behind the camera every render.
    const u32 index = s_depth.lists++;
    if (s_depth.pinnedLastFrame && nearZ >= s_depth.pinPlaneLastFrame &&
        (index + s_depth.frames) % kDepthWindow != 0)
    {
        r.nearW = nearZ;
        r.farW = s.matrix.farZ > nearZ && s.matrix.farZ < 100000.f ? s.matrix.farZ : 100000.f;
        s_depth.pinned = true;                       // still assumed, until the next measured frame
    }
    else
    {
        const float* t = s.matrix.tm;
        float minZ = 1e30f, maxZ = 0.f;
        const u32* v = w + 8;
        for (u32 i = 0; i < count; i++, v += vertexSize / 4)
        {
#if defined(__GNUC__)
            __builtin_prefetch(v + 8 * (vertexSize / 4));   // dcbt: misses cost far more than this loop
#endif
            const float Z = t[2] * asFloat(v[1]) + t[5] * asFloat(v[2]) + t[8] * asFloat(v[3]) + t[11];
            if (Z > 0.f && Z < 1e30f)          // in front, and not a placeholder (-inf) vertex
            {
                if (Z < minZ)
                    minZ = Z;
                if (Z > maxZ)
                    maxZ = Z;
            }
        }
        if (maxZ < nearZ)
            return true;                             // entirely behind the near plane
        r.nearW = minZ > nearZ ? minZ : nearZ;
        r.farW = maxZ;
        if (r.nearW <= nearZ * 1.5f)
            s_depth.pinned = s_depth.windowHit = true;
    }
    if (r.nearW <= nearZ * 1.5f && nearZ < s_depth.pinPlane)
        s_depth.pinPlane = nearZ;

    if (r.lightKey != s_lightGen)
    {
        r.lightKey = 0;                              // until built: a refused set is rebuilt next time
        memset(&r.lights, 0, sizeof(r.lights));
        const LightModel& lm = s.lightModel;
        r.lights.lit = lm.set && !(s.material.set && (s.material.select >> 9 & 1));
        if (r.lights.lit)
        {
            if (s.lightsDirty)
                rebuildActiveLights();
            for (u32 k = 0; k < s.activeCount[0]; k++)
            {
                const int i = s.active[0][k];
                const Light& l = s.lights[i];
                if (!l.parallel || (l.routing & 0xE))
                    return false;                        // point light, or routed to offset/alpha/negative
                if (!(lm.diffuseMask[0] >> i & 1))
                    continue;                            // specular only: no offset colour on this path
                if (r.lights.count == 8)
                    return false;
                ElanGxLight& g = r.lights.lights[r.lights.count++];
                g.dir[0] = l.dirN.x;
                g.dir[1] = l.dirN.y;
                g.dir[2] = l.dirN.z;
                for (int c = 0; c < 3; c++)
                    g.color[c] = clamp01(2.f * l.color[c]);   // the ELAN's x2 light scale
            }
            for (int c = 0; c < 3; c++)
                r.lights.ambient[c] = clamp01(lm.ambientBase[0][c]);
        }
        r.lightKey = s_lightGen;
    }
    return elan_gx_record(r);
}

} // namespace

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Frame skipping: a dropped frame's lists are never transformed
// (tnl_DrawList), so its matrices, lights and materials need not be decoded
// either - a matrix is 160 bytes, four cache misses past its header on a
// Wii, and a race frame sets hundreds. State does not outlive the register
// wait that ends a list section (tnl_ResetState), so nothing a drawn frame
// uses is lost. Should the verdict ever change inside a section (it is held
// from the first question to the render), the drawn lists after it would
// see state that was skipped: those are dropped too (ichHeldBack), never
// drawn with stale matrices.
static bool s_stateSkippedSinceReset = false;

static bool skipStateCommand()
{
    if (!rend_NextFrameSkipped())
        return false;
    s_stateSkippedSinceReset = true;
    elan_Stats().stateSkipped++;
    return true;
}

void tnl_SetProjection(const u32* w)
{
    s_xformGen++;
    s.proj[0] = asFloat(w[2]);
    s.proj[1] = asFloat(w[3]);
    s.proj[2] = asFloat(w[4]);
    s.proj[3] = asFloat(w[5]);
}

void tnl_SetMatrix(const u32* w)
{
    s_xformGen++;
    if (skipStateCommand())
        return;
    Matrix& m = s.matrix;
    m.set = true;
    m.envU = asFloat(w[9]);
    for (int i = 0; i < 9; i++)
        m.lm[i] = asFloat(w[10 + i]);
    m.envV = asFloat(w[19]);
    m.nearZ = asFloat(w[25]);
    for (int i = 0; i < 12; i++)
        m.tm[i] = asFloat(w[26 + i]);
    m.farZ = asFloat(w[38]);
#if !defined(GEKKO)
    {
        // Host census: the near/far planes games put in their instance matrices.
        static float seen[16][2];
        static u32 nSeen = 0;
        bool known = false;
        for (u32 i = 0; i < nSeen && !known; i++)
            known = seen[i][0] == m.nearZ && seen[i][1] == m.farZ;
        if (!known && nSeen < 16)
        {
            seen[nSeen][0] = m.nearZ;
            seen[nSeen][1] = m.farZ;
            nSeen++;
            naomi_Log("[elan] instance matrix planes #%u: near %g far %g\n", nSeen, m.nearZ, m.farZ);
        }
    }
#endif
}

void tnl_SetLightModel(const u32* w)
{
    s_lightGen++;
    if (skipStateCommand())
        return;
    LightModel& lm = s.lightModel;
    lm.set = true;
    s.lightsDirty = true;
    lm.ambientBaseMaterial[0] = w[1] >> 5 & 1;
    lm.ambientOffsetMaterial[0] = w[1] >> 6 & 1;
    lm.ambientBaseMaterial[1] = w[1] >> 7 & 1;
    lm.ambientOffsetMaterial[1] = w[1] >> 8 & 1;
    lm.baseOver = w[1] >> 9 & 1;
    lm.diffuseMask[0] = (u16)w[2];
    lm.specularMask[0] = (u16)(w[2] >> 16);
    unpackColor(w[3], lm.ambientBase[0]);
    unpackColor(w[4], lm.ambientOffset[0]);
    lm.diffuseMask[1] = (u16)w[5];
    lm.specularMask[1] = (u16)(w[5] >> 16);
    unpackColor(w[6], lm.ambientBase[1]);
    unpackColor(w[7], lm.ambientOffset[1]);
}

void tnl_SetLight(const u32* w)
{
    s_lightGen++;
    if (skipStateCommand())
        return;
    Light& l = s.lights[w[1] & 15];
    l = Light();
    l.defined = true;
    l.color[0] = (w[1] >> 24) / 255.f;
    l.color[1] = ((w[1] >> 16) & 0xFF) / 255.f;
    l.color[2] = ((w[1] >> 8) & 0xFF) / 255.f;
    // Stored direction points away from the light: negate it.
    l.dir = {-lightDir(w[2] >> 16 & 0xFF, w[0] >> 16 & 15), -lightDir(w[2] >> 8 & 0xFF, w[0] >> 4 & 15),
             -lightDir(w[2] & 0xFF, w[0] & 15)};
    l.routing = w[2] >> 24 & 15;
    l.dirN = normalize(l.dir);
    s.lightsDirty = true;
    if (w[0] >> 20 & 1)                          // parallel light
    {
        l.parallel = true;
        l.dmode = w[2] >> 28 & 3;
        l.smode = 0;
        return;
    }
    l.dmode = w[1] >> 5 & 7;
    l.smode = w[2] >> 28 & 3;
    l.distMode = w[2] >> 31 & 1;
    l.pos = {asFloat(w[3]), asFloat(w[4]), asFloat(w[5])};
    l.distA = half(w[6] & 0xFFFF);
    l.distB = half(w[6] >> 16);
    l.angleA = half(w[7] & 0xFFFF);
    l.angleB = half(w[7] >> 16);
    // A point light with no position and no attenuation acts as a parallel one.
    l.parallel = w[3] == 0 && w[4] == 0 && w[5] == 0 && w[6] == 0 && w[7] == 0;
    l.distAtten = l.distA != 1.f || l.distB != 0.f;
    l.angleAtten = l.angleA != 1.f || l.angleB != 0.f;
#if !defined(GEKKO)
    if (!l.parallel)
    {
        // Host census: the point lights games define (what a GX local light
        // would have to reproduce: attenuation modes, spot terms, routing).
        static u32 seen[16][6];
        static u32 nSeen = 0;
        bool known = false;
        for (u32 i = 0; i < nSeen && !known; i++)
            known = !memcmp(seen[i], w + 1, sizeof(seen[i]));
        if (!known && nSeen < 16)
        {
            memcpy(seen[nSeen++], w + 1, sizeof(seen[0]));
            naomi_Log("[elan] point light #%u (slot %u): colour %.2f %.2f %.2f pos %g %g %g dir %.2f %.2f %.2f | "
                      "dist %s A %g B %g | angle A %g B %g | dmode %u smode %u routing %X\n",
                      nSeen, w[1] & 15, l.color[0], l.color[1], l.color[2], l.pos.x, l.pos.y, l.pos.z, l.dir.x,
                      l.dir.y, l.dir.z, l.distMode ? "linear" : "inverse", l.distA, l.distB, l.angleA, l.angleB,
                      l.dmode, l.smode, l.routing);
        }
    }
#endif
}

void tnl_SetMaterial(const u32* w)
{
    s_lightGen++;
    if (skipStateCommand())
        return;
    Material& m = s.material;
    m.set = true;
    m.select = w[2];
    for (int vol = 0; vol < 2; vol++)
    {
        const u32 frac = (w[1] >> (vol * 8)) & 31, exp = (w[1] >> (vol * 8 + 5)) & 7;
        m.gloss[vol] = std::ldexp(1.f + frac / 32.f, (int)exp - 1);
    }
    for (int c = 0; c < 4; c++)
        m.diffuse[0][c] = m.specular[0][c] = m.diffuse[1][c] = m.specular[1][c] = 0.f;
    if (m.select & 1)
        unpackColor(w[3], m.diffuse[0]);
    if (m.select & 2)
        unpackColor(w[4], m.specular[0]);
    if (m.select & 16)
        unpackColor(w[5], m.diffuse[1]);
    if (m.select & 32)
        unpackColor(w[6], m.specular[1]);
}

void tnl_BeginModel(const u32* w)
{
    s.modelTsp = w[2];
    s.cullReversed = (w[1] >> 27 & 1) == 0;
    s.shadow = w[0] >> 7 & 1;
}

void tnl_EndModel()
{
    s.modelTsp = 0;
    s.cullReversed = false;
    s.shadow = false;
}

void tnl_SkipModelState(u32 stateCommands)
{
    if (!stateCommands)
        return;
    s_xformGen++;
    s_lightGen++;
    s_stateSkippedSinceReset = true;
    elan_Stats().stateSkipped += stateCommands;
}

void tnl_ResetState()
{
    s_xformGen++;
    s_lightGen++;
    s_stateSkippedSinceReset = false;
    const float proj[4] = {s.proj[0], s.proj[1], s.proj[2], s.proj[3]};
    s = State();
    memcpy(s.proj, proj, sizeof(proj));
}

#if !defined(GEKKO)
// Host census of the ICH vertex data's lifetime (decides what a GPU path may
// do with it): does it change between the ELAN reading it and the render
// (unsafe to read it again at draw time), and does the same data come back
// the next frame (cacheable, at the same address or anywhere)?
namespace {
struct DataRec
{
    const u32* p;
    u32 words, hash, count;
};
std::vector<DataRec> s_frameData;
std::unordered_map<const u32*, u32> s_prevByAddr;
std::unordered_set<u32> s_prevHashes;

u32 hashWords(const u32* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
} // namespace

void tnl_DataCensusRender()
{
    NaomiElanStats& st = elan_Stats();
    std::unordered_map<const u32*, u32> byAddr;
    std::unordered_set<u32> hashes;
    for (const DataRec& r : s_frameData)
    {
        if (hashWords(r.p, r.words) != r.hash)
            st.dataChangedBeforeRender += r.count;
        const auto it = s_prevByAddr.find(r.p);
        if (it != s_prevByAddr.end() && it->second == r.hash)
            st.dataSameAddress += r.count;
        if (s_prevHashes.count(r.hash))
            st.dataSameContent += r.count;
        st.dataTotal += r.count;
        byAddr[r.p] = r.hash;
        hashes.insert(r.hash);
    }
    s_prevByAddr.swap(byAddr);
    s_prevHashes.swap(hashes);
    s_frameData.clear();
}
#endif

void tnl_OnStartRender()
{
    // A render with no GPU list (a frame the frameskip dropped, a 2D screen)
    // says nothing about the depth range: keep the last verdict. Clearing it
    // there made every drawn frame after a skipped one measure every vertex
    // again - with FRAMESKIP AUTO, nearly all of them (~15 ms a drawn frame
    // on a Wii, where each vertex is a cache miss).
    if (s_depth.lists)
    {
        bool pinned = s_depth.pinned;
        if (!s_depth.pinnedLastFrame)
        {
            // Every list was measured: a pin starts its first window now.
            s_depth.windowRenders = 0;
            s_depth.windowHit = false;
        }
        else if (++s_depth.windowRenders == kDepthWindow)
        {
            // A full window, every list measured once: unpin if none reached its plane.
            if (!s_depth.windowHit)
                pinned = false;
            s_depth.windowRenders = 0;
            s_depth.windowHit = false;
        }
        s_depth.pinnedLastFrame = pinned;
        s_depth.pinPlaneLastFrame = s_depth.pinPlane;
        s_depth.frames++;
    }
    s_depth.pinned = false;
    s_depth.pinPlane = 1e30f;
    s_depth.lists = 0;
}

int tnl_DrawList(const u32* w, u32 vertexSize, int listType)
{
#if !defined(GEKKO)
    {
        const u32 words = w[7] * vertexSize / 4;
        s_frameData.push_back({w + 8, words, hashWords(w + 8, words), w[7]});
    }
#endif
    const u32 pcw = w[0];
    const u32 layout = w[6];
    const u32 count = w[7];
    const int list = listType >= 0 ? listType : (int)(pcw >> 24 & 7);
    if (list & 1)
        return listType;                          // NAOMI 2 modifier volumes: not drawn yet
    // The renderer will drop this frame (frame skipping): its geometry is
    // never seen, so don't transform it. Most of a race frame's cost.
    if (rend_NextFrameSkipped())
    {
        elan_Stats().ichSkipped++;
        return list;
    }
    if (s_stateSkippedSinceReset)
    {
        elan_Stats().ichHeldBack++;
        return list;
    }
    ELAN_TIME_BEGIN(t0);

    // Polygon parameters: volume 0 only, packed colour, float UVs.
    const bool envMap = s.material.set && (s.material.select >> 11 & 1);
    bool textured = (pcw >> 3 & 1) || layout == 0x10A || envMap;
    if (layout == 0x002 || layout == 0x042)
        textured = envMap;
    const bool offset = textured && !envMap && ((pcw >> 2 & 1) || layout == 0x10A);
    u32 outPcw = 0x80000000u | (u32)list << 24 | (pcw & 2) | (offset ? 4u : 0u) | (textured ? 8u : 0u) |
                 ((pcw >> 7 & 1) ^ (s.shadow ? 1u : 0u)) << 7;
#if !defined(GEKKO)
    {
        // Census of what a GPU (GX) T&L path has to cover, in vertices.
        NaomiElanStats& st = elan_Stats();
        const bool litList = s.lightModel.set && !(s.material.set && (s.material.select >> 9 & 1));
        if (litList && s.lightsDirty)
            rebuildActiveLights();
        bool point = false;
        if (litList)
            for (u32 k = 0; k < s.activeCount[0]; k++)
                point |= !s.lights[s.active[0][k]].parallel;
        const bool spec = litList && s.anySpecular[0];
        st.census[list == 0 ? 0 : list == 2 ? 1 : 2] += count;
        if (envMap)
        {
            st.census[3] += count;
            static float seen[8][2];
            static u32 nSeen = 0;
            bool known = false;
            for (u32 i = 0; i < nSeen && !known; i++)
                known = seen[i][0] == s.matrix.envU && seen[i][1] == s.matrix.envV;
            if (!known && nSeen < 8)
            {
                seen[nSeen][0] = s.matrix.envU;
                seen[nSeen][1] = s.matrix.envV;
                nSeen++;
                naomi_Log("[elan] env-map offsets #%u: u %g v %g (layout %03X, list %d, lit %d)\n", nSeen,
                          s.matrix.envU, s.matrix.envV, layout, list, (int)litList);
            }
        }
        if (spec)
            st.census[4] += count;
        if (point)
            st.census[5] += count;
        if (layout == 0x10A)
            st.census[6] += count;
        if (list == 0 && layout != 0x10A && !envMap && !spec && !point)
            st.census[7] += count;
    }
#endif
    u32 tsp = w[2] ^ s.modelTsp;
    if (envMap)
        tsp = (tsp | 1u << 20) & ~(1u << 19);    // UseAlpha on, IgnoreTexA off
    const bool projFlip = std::signbit(-s.proj[0]) == std::signbit(s.proj[2]);
    const u32 isp = w[1] ^ (u32)(s.cullReversed != projFlip) << 27;

    // GPU path (prototype, naomi_elan_gx.h): lists the GX hardware can light
    // are handed to the renderer untransformed. Translucent and punch-through
    // ones too, unless a translucent sort is on: the sorts work on the
    // renderer's own triangles, which a GPU-drawn list doesn't have.
    if (elan_gx_enabled() && (list == 0 || (!g_trans_sort_preset && !g_autosort_preset)))
    {
        ELAN_TIME_BEGIN(tg);
        const bool taken = recordForGpu(w, vertexSize, count, layout, outPcw, isp, tsp, envMap);
        ELAN_TIME_END(gpuNs, tg);
        if (taken)
        {
            elan_Stats().gpuVertices += count;
            ELAN_TIME_END(tnlNs, t0);
            return list;
        }
    }

    s_out.begin(outPcw, isp, tsp, w[3]);

    // Strips and fans, linked into one strip with degenerate triangles.
    s_strip.clear();
    const u32* v = w + 8;
    TnlVtx fanCenter = {}, fanLast = {};
    bool stripStart = true;
    bool lit = false;
    float minZ = 1e30f;
    ELAN_TIME_BEGIN(tv);
    for (u32 i = 0; i < count; i++, v += vertexSize / 4)
    {
        const TnlVtx cur = convert(v, layout, textured, envMap, lit);
        const u32 header = v[0];
        if (!std::isfinite(cur.X) || !std::isfinite(cur.Y) || !std::isfinite(cur.Z))
        {
            // Placeholder vertex (the game stores -inf in unused slots): drop
            // it and restart the strip after it. A renderer fed inf/NaN can
            // lose the whole frame (GX) or draw slivers across the screen.
            stripStart = true;
            continue;
        }
        if (stripStart)
        {
            fanCenter = cur;
            if (!s_strip.empty())
            {
                s_strip.push_back(fanLast);
                s_strip.push_back(cur);
                if (s_strip.size() & 1)
                    s_strip.push_back(cur);
            }
            stripStart = false;
        }
        else if ((header >> 29 & 3) == 2)        // fan vertex
        {
            s_strip.push_back(fanLast);
            s_strip.push_back(fanCenter);
            if (s_strip.size() & 1)
                s_strip.push_back(fanCenter);
            s_strip.push_back(fanCenter);
            s_strip.push_back(fanLast);
        }
        s_strip.push_back(cur);
        fanLast = cur;
        if (cur.Z < minZ)
            minZ = cur.Z;
        if (header >> 31)
            stripStart = true;
    }

    ELAN_TIME_END(convNs, tv);
    const float nearZ = s.matrix.nearZ > 0.f ? s.matrix.nearZ : 0.001f;
    if (minZ >= nearZ)
        s_out.strip(s_strip.data(), (u32)s_strip.size());
    else
    {
        // Only triangles that cross the near plane are clipped; runs of
        // triangles in front of it stay strips.
        s_clipped.clear();
        const u32 n = (u32)s_strip.size();
        u32 runStart = 0;      // first triangle of the current in-front run
        bool inRun = false;
        auto flushRun = [&](u32 endTri) {     // triangles [runStart, endTri)
            if (!inRun || endTri <= runStart)
                return;
            const TnlVtx* first = &s_strip[runStart];
            const u32 count = endTri - runStart + 2;
            if (runStart & 1)
            {
                // An odd triangle starts reversed: lead with a duplicate so
                // the run keeps its winding once placed at an even index.
                linkStrip(s_clipped, first, 1);
                s_clipped.insert(s_clipped.end(), first, first + count);
            }
            else
                linkStrip(s_clipped, first, count);
        };
        for (u32 k = 0; k + 2 < n; k++)
        {
            const TnlVtx& a = s_strip[k];
            const TnlVtx& b = s_strip[k + 1];
            const TnlVtx& c = s_strip[k + 2];
            if (a.Z >= nearZ && b.Z >= nearZ && c.Z >= nearZ)
            {
                if (!inRun)
                {
                    inRun = true;
                    runStart = k;
                }
                continue;
            }
            flushRun(k);
            inRun = false;
            if (k & 1)
                clippedTriangle(b, a, c, nearZ);
            else
                clippedTriangle(a, b, c, nearZ);
        }
        if (n >= 3)
            flushRun(n - 2);
        s_out.strip(s_clipped.data(), (u32)s_clipped.size());
    }
    s_out.flush();
    ELAN_TIME_END(tnlNs, t0);
    return list;
}
