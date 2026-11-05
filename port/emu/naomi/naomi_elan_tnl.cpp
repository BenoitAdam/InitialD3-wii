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

int g_depth_plane_preset = 0;     // DEPTH MEASURE, 1 = RACE PLANE (launch page)
extern "C" {
int g_small_cull_preset = 0;      // SMALL 3D, in pixels (launch page)
}
extern int g_trans_sort_preset;   // renderer presets (launch page)
extern int g_autosort_preset;

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>
#if !defined(GEKKO)
#include <cstdio>
#include <cstdlib>
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
    float reachSq = INFINITY;   // squared distance from which distance attenuation is 0
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
            if (d2 >= l.reachSq)
                continue;                                    // attenuated to 0: adds nothing
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

#if !defined(GEKKO)
// Windows preview of NIGHT LIGHTS = GPU (the runner has no GX): point-lit
// lists take the colour GX's channel would give them - material x clamp(
// ambient + sum of attenuation x clamped N.L x light colour), no offset colour.
const ElanGxLightSet* s_gxPreview = nullptr;

void gxLighting(float* base, float* offs, Vec3 P, Vec3 N, const ElanGxLightSet& ls)
{
    float acc[3] = {ls.ambient[0], ls.ambient[1], ls.ambient[2]};
    for (u32 i = 0; i < ls.count; i++)
    {
        const ElanGxLight& g = ls.lights[i];
        Vec3 L = {g.dir[0], g.dir[1], g.dir[2]};
        float attn = 1.f;
        if (ls.localMask >> i & 1)
        {
            const Vec3 toLight = Vec3{g.pos[0], g.pos[1], g.pos[2]} - P;
            const float d2 = dot(toLight, toLight), d = std::sqrt(d2);
            L = d > 0.f ? toLight * (1.f / d) : toLight;
            const float c = std::fmax(0.f, dot(L, {g.dir[0], g.dir[1], g.dir[2]}));
            attn = std::fmax(0.f, g.angle[0] + g.angle[1] * c + g.angle[2] * c * c) /
                   (g.dist[0] + g.dist[1] * d + g.dist[2] * d2);
        }
        const float f = attn * std::fmax(0.f, dot(N, L));
        for (int c = 0; c < 3; c++)
            acc[c] += f * g.color[c];
    }
    for (int c = 0; c < 3; c++)
        base[c] *= clamp01(acc[c]);
    for (int c = 0; c < 4; c++)
        offs[c] = 0.f;
}
#endif

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
#if !defined(GEKKO)
        if (s_gxPreview)
            gxLighting(o.base, o.offs, P, N, *s_gxPreview);
        else
#endif
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
// At most this many vertices per polygon header. Renderers check their
// buffers once per header (nullDC4Wii's GX renderer keeps 4K vertices of
// headroom), so one huge strip must not arrive in a single piece. Even,
// so each continuation starts on an even vertex and keeps its winding.
constexpr u32 kMaxStripVertices = 1024;

// This path's buffers are fixed and lists go through them a piece at a time:
// nothing here allocates once the game runs. On a Wii the C heap has a few
// hundred KB left beside the guest page pool, and these used to be vectors
// sized by the longest list. In a day race the GPU path takes every list and
// they stayed empty; the night attract demo (point lights on everything, so
// nearly every list comes here) grows them to ~1.5 MB, more than the heap has,
// and a failed new is abort() without exceptions.
struct TaWriter
{
    u32 words[8 + 8 * kMaxStripVertices];       // one piece: header + vertices
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

    // Sent as soon as it is built: the buffer holds one piece.
    void piece(const TnlVtx* v, u32 count)
    {
        if (count < 3)
            return;
        elan_Stats().tnlVertices += count;
        memcpy(words, header, sizeof(header));
        u32* o = words + 8;
        const float fx = s.proj[0], tx = s.proj[1], fy = s.proj[2], ty = s.proj[3];
        for (u32 i = 0; i < count; i++, o += 8)
        {
            const float invZ = 1.f / v[i].Z;
            o[0] = 0xE0000000u | (i + 1 == count ? 0x10000000u : 0u);
            o[1] = asWord(fx * v[i].X * invZ + tx);
            o[2] = asWord(fy * v[i].Y * invZ + ty);
            o[3] = asWord(invZ);
            if (textured)
            {
                o[4] = asWord(v[i].u);
                o[5] = asWord(v[i].v);
                o[6] = packColor(v[i].base);
                o[7] = packColor(v[i].offs);
            }
            else
            {
                o[4] = 0;
                o[5] = 0;
                o[6] = packColor(v[i].base);
                o[7] = 0;
            }
        }
        ELAN_TIME_BEGIN(tt);
        libPvr_TaDMA(words, count + 1);
        ELAN_TIME_END(taNs, tt);
        elan_Stats().taBlocks += count + 1;
    }
};

TaWriter s_out;

template <u32 N>
struct VtxBuf
{
    TnlVtx v[N];
    u32 n;
    void push(const TnlVtx& t) { v[n++] = t; }
};

// A piece of a list's linked strip (at most kMaxStripVertices, plus what one
// input vertex adds), and the same piece near-plane clipped.
VtxBuf<kMaxStripVertices + 8> s_strip;
constexpr u32 kClippedCap = kMaxStripVertices + 16;
VtxBuf<kClippedCap> s_clipped;

// Appends strip `v` to `out`, joined by degenerate triangles. The first real
// vertex lands on an even index so its triangles keep their winding. One
// polygon header per list instead of one per piece keeps the renderer's
// strip/param tables small (the GX renderer has 8K of each per frame).
template <u32 N>
void linkStrip(VtxBuf<N>& out, const TnlVtx* v, u32 n)
{
    if (out.n)
    {
        const TnlVtx last = out.v[out.n - 1];
        out.push(last);
        out.push(v[0]);
        if (out.n & 1)
            out.push(v[0]);
    }
    memcpy(out.v + out.n, v, n * sizeof(TnlVtx));
    out.n += n;
}

// Room for `n` more vertices (and a link) in s_clipped: sends what it holds if
// needed. A piece then starts afresh, at index 0, which keeps every winding.
void clippedRoom(u32 n)
{
    if (s_clipped.n && s_clipped.n + n + 3 > kClippedCap)
    {
        s_out.strip(s_clipped.v, s_clipped.n);
        s_clipped.n = 0;
    }
}

// Triangle (a, b, c) clipped against Z >= nearZ, appended to s_clipped.
void clippedTriangle(const TnlVtx& a, const TnlVtx& b, const TnlVtx& c, float nearZ)
{
    clippedRoom(4);
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

// Sends `n` vertices of a list's linked strip (a piece starting at an even
// index of it). Only triangles that cross the near plane are clipped; runs of
// triangles in front of it stay strips.
void sendStrip(const TnlVtx* v, u32 n, float nearZ)
{
    float minZ = 1e30f;
    for (u32 i = 0; i < n; i++)
        if (v[i].Z < minZ)
            minZ = v[i].Z;
    if (minZ >= nearZ)
    {
        s_out.strip(v, n);
        return;
    }
    s_clipped.n = 0;
    u32 runStart = 0;      // first triangle of the current in-front run
    bool inRun = false;
    auto flushRun = [&](u32 endTri) {     // triangles [runStart, endTri)
        if (!inRun || endTri <= runStart)
            return;
        const TnlVtx* first = v + runStart;
        const u32 count = endTri - runStart + 2;
        clippedRoom(count + 1);
        if (runStart & 1)
        {
            // An odd triangle starts reversed: lead with a duplicate so
            // the run keeps its winding once placed at an even index.
            linkStrip(s_clipped, first, 1);
            memcpy(s_clipped.v + s_clipped.n, first, count * sizeof(TnlVtx));
            s_clipped.n += count;
        }
        else
            linkStrip(s_clipped, first, count);
    };
    for (u32 k = 0; k + 2 < n; k++)
    {
        const TnlVtx& a = v[k];
        const TnlVtx& b = v[k + 1];
        const TnlVtx& c = v[k + 2];
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
    s_out.strip(s_clipped.v, s_clipped.n);
    s_clipped.n = 0;
}

// ---------------------------------------------------------------------------
// Point lights on the GPU path (NIGHT LIGHTS = GPU)
// ---------------------------------------------------------------------------
// Initial D's night scenes light every list with 6-11 point lights (street
// lamps reaching 40 units, the cars' lights 100-200), which kept them all on
// the CPU. GX local lights stand in for them, approximately.

bool gpuLayout(u32 layout)
{
    return layout == 0x002 || layout == 0x00A || layout == 0x00E || layout == 0x042 || layout == 0x04A;
}

// Whether the current state lights volume 0 with a point light.
bool litByPointLights()
{
    if (!s.lightModel.set || (s.material.set && (s.material.select >> 9 & 1)))
        return false;
    if (s.lightsDirty)
        rebuildActiveLights();
    for (u32 k = 0; k < s.activeCount[0]; k++)
        if (!s.lights[s.active[0][k]].parallel)
            return true;
    return false;
}

// A list's vertices in lighting space (-X, Y, -Z).
struct Box
{
    float mn[3], mx[3];
};

float distanceSq(const Box& b, Vec3 p)
{
    const float q[3] = {p.x, p.y, p.z};
    float d2 = 0.f;
    for (int c = 0; c < 3; c++)
    {
        const float e = q[c] < b.mn[c] ? b.mn[c] - q[c] : q[c] > b.mx[c] ? q[c] - b.mx[c] : 0.f;
        d2 += e * e;
    }
    return d2;
}

// The view depth range of a list's vertices in front of the camera and, with
// kBox, the box of all its vertices (placeholders aside) in lighting space.
template <bool kBox>
void measureList(const u32* v, u32 count, u32 vertexSize, const float* t, float& minZ, float& maxZ, Box* box)
{
    minZ = 1e30f;
    maxZ = 0.f;
    if (kBox)
        *box = {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    for (u32 i = 0; i < count; i++, v += vertexSize / 4)
    {
#if defined(__GNUC__)
        __builtin_prefetch(v + 8 * (vertexSize / 4));   // dcbt: misses cost far more than this loop
#endif
        const float x = asFloat(v[1]), y = asFloat(v[2]), z = asFloat(v[3]);
        const float Z = t[2] * x + t[5] * y + t[8] * z + t[11];
        if (Z > 0.f && Z < 1e30f)              // in front, and not a placeholder (-inf) vertex
        {
            if (Z < minZ)
                minZ = Z;
            if (Z > maxZ)
                maxZ = Z;
        }
        if (kBox)
        {
            const float p[3] = {-(t[0] * x + t[3] * y + t[6] * z + t[9]), t[1] * x + t[4] * y + t[7] * z + t[10], -Z};
            if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))
                for (int c = 0; c < 3; c++)
                {
                    box->mn[c] = p[c] < box->mn[c] ? p[c] : box->mn[c];
                    box->mx[c] = p[c] > box->mx[c] ? p[c] : box->mx[c];
                }
        }
    }
}

// A point light as a GX local light (GX_AF_SPOT), `scale` times its colour.
//
// Distance: the ELAN's clamp(B / d + A), A < 0 < B, is 1 near the light and
// exactly 0 from B / -A (the reach); GX's 1 / (k0 + k1 d + k2 d^2) has no such
// edge. A least-squares fit over three times the reach gives k1 = 0,
// k0 = 0.84 and k2 ~ (1.5 + 1.9 / -A) / reach^2: for A = -1, 0.6 at half the
// reach where the ELAN gives 1, and a tail (0.24 at the reach, 0.12 at 1.5x).
// Lights out of reach of a whole list are left out of it (pointLightSet),
// which keeps the tails off most lists.
//
// Angle: clamp((A + B) - B c), c the cosine to the spot axis, is linear, and
// GX clamps it at 0 but not at 1. When it saturates before the axis, a
// parabola with the same zero and its top (1) on the axis stands in (the
// cars' headlights: 0 from 25 degrees, 1 within 15, the parabola 0.87 at 15).
//
// False for the forms Initial D does not use: those lists keep the CPU path.
bool gxPointLight(const Light& l, float scale, ElanGxLight& g)
{
    if (!l.distAtten)
    {
        g.dist[0] = 1.f;
        g.dist[1] = g.dist[2] = 0.f;
    }
    else if (!l.distMode && l.distA < 0.f && l.distB > 0.f)
    {
        const float reach = l.distB / -l.distA;
        g.dist[0] = 0.84f;
        g.dist[1] = 0.f;
        g.dist[2] = (1.5f + 1.9f / -l.distA) / (reach * reach);
    }
    else
        return false;

    const float p = l.angleAtten ? l.angleA + l.angleB : 1.f, q = l.angleAtten ? -l.angleB : 0.f;
    g.angle[1] = g.angle[2] = 0.f;
    if (q == 0.f || (p >= 1.f && p + q >= 1.f))
        g.angle[0] = clamp01(p);                         // constant (street lamps: 4.4 + 7 c, always 1)
    else if (q < 0.f)
        return false;                                    // brighter away from the axis
    else if (p + q <= 1.f)
    {
        g.angle[0] = p;                                  // never above 1: exact
        g.angle[1] = q;
    }
    else
    {
        const float w = 1.f + p / q;                     // 1 - c at the zero, c = -p / q
        const float k = 1.f / (w * w);
        g.angle[0] = 1.f - k;                            // 1 - ((1 - c) / w)^2
        g.angle[1] = 2.f * k;
        g.angle[2] = -k;
    }

    // GX colours stop at 1: what is above goes in the attenuation instead.
    float c[3], top = 1.f;
    for (int i = 0; i < 3; i++)
    {
        c[i] = l.color[i] * scale;
        top = c[i] > top ? c[i] : top;
    }
    for (int i = 0; i < 3; i++)
    {
        g.color[i] = c[i] / top;
        g.angle[i] *= top;
    }
    g.dir[0] = l.dir.x;
    g.dir[1] = l.dir.y;
    g.dir[2] = l.dir.z;
    g.pos[0] = l.pos.x;
    g.pos[1] = l.pos.y;
    g.pos[2] = l.pos.z;
    return true;
}

bool sameLight(const Light& a, const Light& b)
{
    return a.parallel == b.parallel && a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z &&
           a.dir.x == b.dir.x && a.dir.y == b.dir.y && a.dir.z == b.dir.z && a.color[0] == b.color[0] &&
           a.color[1] == b.color[1] && a.color[2] == b.color[2] && a.distA == b.distA && a.distB == b.distB &&
           a.angleA == b.angleA && a.angleB == b.angleB && a.distMode == b.distMode && a.routing == b.routing;
}

// The GX lights of a list lit by point lights, from its box: distSq(p) is a
// lighting-space point's squared distance to it, or less. Lights out of
// reach of the whole box add nothing to it and are left out (exact). Copies
// of one light (the game sends a car's headlight up to 4 times) become one
// light that many times as bright. Past 8 lights, the parallel ones and the
// point lights nearest the box (against their reach) are kept. False when a
// light needs the CPU path.
//
// Done in two steps, since neighbouring lists mostly keep the same lights: the
// pick (which lights, how many copies of each) is a few box distances; the set
// (cleared, converted with several divisions a light, compared) is only built
// when the pick differs from the last one (pointLightRecord).
struct LightPick
{
    u32 gen;                // s_lightGen: every light, the light model and the active list
    u32 n;
    u8 slot[8], copies[8];  // in the set's order

    bool same(const LightPick& o) const
    {
        return gen == o.gen && n == o.n && !memcmp(slot, o.slot, n) && !memcmp(copies, o.copies, n);
    }
};

// Once a light generation: whether a light needs the CPU path (routed to
// offset/alpha/negative), and for each active light the earlier diffuse one
// it is a copy of (sameLight), else itself. Identical lights lie at the same
// distance from any box, so a copy is in reach exactly when its first is.
struct LightPlan
{
    u32 gen;
    bool refuse;
    u8 canon[16];
};
LightPlan s_lightPlan = {};

const LightPlan& lightPlan()
{
    LightPlan& p = s_lightPlan;
    if (p.gen == s_lightGen)
        return p;
    p.gen = s_lightGen;
    p.refuse = false;
    const LightModel& lm = s.lightModel;
    for (u32 k = 0; k < s.activeCount[0]; k++)
    {
        const int i = s.active[0][k];
        const Light& l = s.lights[i];
        p.refuse |= (l.routing & 0xE) != 0;
        p.canon[k] = (u8)k;
        if (!(lm.diffuseMask[0] >> i & 1))
            continue;
        for (u32 c = 0; c < k; c++)
        {
            const int ic = s.active[0][c];
            if ((lm.diffuseMask[0] >> ic & 1) && sameLight(s.lights[ic], l))
            {
                p.canon[k] = (u8)c;
                break;
            }
        }
    }
    return p;
}

template <class DistSq>
bool pickLights(LightPick& out, DistSq distSq)
{
    const LightPlan& plan = lightPlan();
    if (plan.refuse)
        return false;
    const LightModel& lm = s.lightModel;
    struct Pick
    {
        u8 slot, copies;
        float near;                              // squared distance / squared reach; parallel: -1
    };
    Pick pick[16];
    s8 at[16];                                   // an active light's pick, -1: none
    u32 n = 0;
    for (u32 k = 0; k < s.activeCount[0]; k++)
    {
        const int i = s.active[0][k];
        at[k] = -1;
        if (!(lm.diffuseMask[0] >> i & 1))
            continue;                            // specular only: no offset colour on this path
        if (plan.canon[k] != k)
        {
            if (at[plan.canon[k]] >= 0)
                pick[at[plan.canon[k]]].copies++;
            continue;
        }
        const Light& l = s.lights[i];
        float near = -1.f;
        if (!l.parallel)
        {
            const float d2 = distSq(l.pos);
            if (d2 >= l.reachSq)
                continue;
            near = d2 / l.reachSq;
        }
        at[k] = (s8)n;
        pick[n++] = {(u8)i, 1, near};
    }
    if (n > 8)
    {
        // The 8 nearest, then back in slot order (same lights, same set).
        for (u32 a = 1; a < n; a++)
            for (u32 b = a; b > 0 && pick[b].near < pick[b - 1].near; b--)
                std::swap(pick[b], pick[b - 1]);
        n = 8;
        for (u32 a = 1; a < n; a++)
            for (u32 b = a; b > 0 && pick[b].slot < pick[b - 1].slot; b--)
                std::swap(pick[b], pick[b - 1]);
    }
    out.gen = s_lightGen;
    out.n = n;
    for (u32 j = 0; j < n; j++)
    {
        out.slot[j] = pick[j].slot;
        out.copies[j] = pick[j].copies;
    }
    return true;
}

bool buildLightSet(const LightPick& p, ElanGxLightSet& out)
{
    const LightModel& lm = s.lightModel;
    memset(&out, 0, sizeof(out));                // padding too: sets are compared with memcmp
    out.lit = true;
    for (int c = 0; c < 3; c++)
        out.ambient[c] = clamp01(lm.ambientBase[0][c]);
    for (u32 j = 0; j < p.n; j++)
    {
        const Light& l = s.lights[p.slot[j]];
        ElanGxLight& g = out.lights[out.count];
        if (l.parallel)
        {
            g.dir[0] = l.dirN.x;
            g.dir[1] = l.dirN.y;
            g.dir[2] = l.dirN.z;
            for (int c = 0; c < 3; c++)
                g.color[c] = clamp01(2.f * p.copies[j] * l.color[c]);   // the ELAN's x2 light scale
        }
        else
        {
            if (!gxPointLight(l, 2.f * p.copies[j], g))
                return false;
            out.localMask |= (u8)(1u << out.count);
        }
        out.count++;
    }
    return true;
}

template <class DistSq>
bool pointLightSet(ElanGxLightSet& out, DistSq distSq)
{
    LightPick p;
    return pickLights(p, distSq) && buildLightSet(p, out);
}

// The light set of a point-lit list into rec.lights / rec.lightKey: the last
// one built when the pick is the same (copied back only if a parallel-lit list
// overwrote it since). An unchanged set keeps its key, so the renderer shares
// it without comparing; keys with the top bit set never meet a light
// generation. False: CPU path.
// The last set built, its pick and key: one for every caller (a function
// template's statics would be one per DistSq type, with keys that collide).
struct LastLightSet
{
    LightPick pick;                             // gen 0: none yet (s_lightGen starts at 1)
    ElanGxLightSet set;
    u32 key;
};
LastLightSet s_lastLightSet = {};

template <class DistSq>
bool pointLightRecord(ElanGxRecord& rec, DistSq distSq)
{
    LightPick& lastPick = s_lastLightSet.pick;
    ElanGxLightSet& last = s_lastLightSet.set;
    u32& key = s_lastLightSet.key;
    LightPick p;
    if (!pickLights(p, distSq))
    {
        rec.lightKey = 0;
        return false;
    }
    if (p.same(lastPick))
    {
        if (rec.lightKey != key)
        {
            memcpy(&rec.lights, &last, sizeof(last));   // padding too: sets are compared with memcmp
            rec.lightKey = key;
        }
        elan_Stats().lightSetsReused++;
#if !defined(GEKKO)
        // Runner: the set reused must be the one a build would give.
        static ElanGxLightSet check;
        if (!buildLightSet(p, check) || memcmp(&check, &rec.lights, sizeof(check)))
            elan_Stats().lightSetsStale++;
#endif
        return true;
    }
    if (!buildLightSet(p, rec.lights))
    {
        rec.lightKey = 0;
        return false;
    }
    elan_Stats().lightSetsBuilt++;
    if (memcmp(&last, &rec.lights, sizeof(last)))
    {
        memcpy(&last, &rec.lights, sizeof(last));
        key = (key + 1) | 0x80000000u;
    }
    lastPick = p;
    rec.lightKey = key;
    return true;
}

// ---- 3D LIST CULL: a list's model-space box (min x y z, max x y z, kept with
// its GPU fetch program) under the instance matrix, instead of its vertices.

// a x + b y + c z + d over a box, with the box corner giving its largest and
// smallest value already chosen: three multiplications a list.
struct BoxRow
{
    float k[4];
    u8 hi[3], lo[3];

    void set(float a, float b, float c, float d)
    {
        k[0] = a;
        k[1] = b;
        k[2] = c;
        k[3] = d;
        for (int i = 0; i < 3; i++)
        {
            hi[i] = (u8)(k[i] > 0.f ? 3 + i : i);
            lo[i] = (u8)(k[i] > 0.f ? i : 3 + i);
        }
    }
    float top(const float* box) const { return k[3] + k[0] * box[hi[0]] + k[1] * box[hi[1]] + k[2] * box[hi[2]]; }
    float bottom(const float* box) const { return k[3] + k[0] * box[lo[0]] + k[1] * box[lo[1]] + k[2] * box[lo[2]]; }
};

// What the current matrix and projection make of a box, rebuilt when they
// change (a matrix serves ~3 lists). View Z, and the picture's edges. On
// screen x = fx X / Z + tx is within [0, width] (y likewise): for Z > 0 that
// is fx X + tx Z >= 0 and fx X + (tx - width) Z <= 0, planes through the eye,
// written so that the picture is on the side >= 0. A box wholly on the other
// side of one of them has no point in the picture (in front of the camera or
// not), and neither has any triangle between its points. A few pixels of margin.
struct BoxView
{
    u32 gen;                // s_xformGen it was built at
    float width, height;
    BoxRow z, edge[4];
    bool edges;
    float scale;            // pixels per unit at depth 1: the projection's x scale times the matrix's
};
BoxView s_boxView = {};

const BoxView& boxView()
{
    BoxView& v = s_boxView;
    float width, height;
    elan_gx_canvas(width, height);
    if (v.gen == s_xformGen && v.width == width && v.height == height)
        return v;
    v.gen = s_xformGen;
    v.width = width;
    v.height = height;
    v.edges = width > 0.f && height > 0.f;
    const float* t = s.matrix.tm;
    v.z.set(t[2], t[5], t[8], t[11]);
    constexpr float kMargin = 8.f;
    const float fx = s.proj[0], tx = s.proj[1], fy = s.proj[2], ty = s.proj[3];
    {
        // The longest the matrix makes a model-space axis (exact for a rotation
        // with a uniform scale, the instances' kind).
        float m = 0.f;
        for (int c = 0; c < 3; c++)
        {
            const float n = t[3 * c] * t[3 * c] + t[3 * c + 1] * t[3 * c + 1] + t[3 * c + 2] * t[3 * c + 2];
            m = n > m ? n : m;
        }
        v.scale = std::fabs(fx) * std::sqrt(m);
    }
    const struct
    {
        float f, e;     // f X + e Z (or f Y + e Z)
        int row;        // 0: X, 1: Y
    } planes[4] = {{fx, tx + kMargin, 0}, {-fx, width + kMargin - tx, 0}, {fy, ty + kMargin, 1},
                   {-fy, height + kMargin - ty, 1}};
    for (int i = 0; i < 4; i++)
    {
        const auto& p = planes[i];
        v.edge[i].set(p.f * t[p.row] + p.e * t[2], p.f * t[3 + p.row] + p.e * t[5], p.f * t[6 + p.row] + p.e * t[8],
                      p.f * t[9 + p.row] + p.e * t[11]);
    }
    return v;
}

inline bool outOfPicture(const BoxView& v, const float* box)
{
    return v.edges && (v.edge[0].top(box) < 0.f || v.edge[1].top(box) < 0.f || v.edge[2].top(box) < 0.f ||
                       v.edge[3].top(box) < 0.f);
}

// SMALL 3D: the box's diagonal, seen from its nearest depth zlo, spans fewer
// than g_small_cull_preset pixels (an upper bound of the object's size on
// screen: far trees, posts, signs, a distant car).
inline bool tooSmall(const BoxView& v, const float* box, float zlo)
{
    if (!g_small_cull_preset || zlo <= 0.f)
        return false;
    const float dx = box[3] - box[0], dy = box[4] - box[1], dz = box[5] - box[2];
    const float lim = (float)g_small_cull_preset * zlo;
    return v.scale * v.scale * (dx * dx + dy * dy + dz * dz) < lim * lim;
}

// Point lights against the model-space box: the light taken into model
// space, its distance to the box there, times the least the instance matrix
// can shrink a length (exact for a rotation with a uniform scale). Never more
// than the true distance, so a light found out of reach is.
struct ModelBoxDistance
{
    Box box;
    float inv[9], t[3], minScaleSq;

    bool init(const float* b, const float* tm)
    {
        memcpy(&box, b, sizeof(box));
        // View space = A p + t, A's rows (tm0 tm3 tm6), (tm1 tm4 tm7), (tm2 tm5 tm8).
        const float a[9] = {tm[0], tm[3], tm[6], tm[1], tm[4], tm[7], tm[2], tm[5], tm[8]};
        const float c0 = a[4] * a[8] - a[5] * a[7], c1 = a[5] * a[6] - a[3] * a[8], c2 = a[3] * a[7] - a[4] * a[6];
        const float det = a[0] * c0 + a[1] * c1 + a[2] * c2;
        if (!(std::fabs(det) > 1e-12f))
            return false;
        const float r = 1.f / det;
        inv[0] = c0 * r;
        inv[1] = (a[2] * a[7] - a[1] * a[8]) * r;
        inv[2] = (a[1] * a[5] - a[2] * a[4]) * r;
        inv[3] = c1 * r;
        inv[4] = (a[0] * a[8] - a[2] * a[6]) * r;
        inv[5] = (a[2] * a[3] - a[0] * a[5]) * r;
        inv[6] = c2 * r;
        inv[7] = (a[1] * a[6] - a[0] * a[7]) * r;
        inv[8] = (a[0] * a[4] - a[1] * a[3]) * r;
        t[0] = tm[9];
        t[1] = tm[10];
        t[2] = tm[11];
        // Always true: |A d| >= |d| / |A^-1|, |A^-1| <= its Frobenius norm.
        float f2 = 0.f;
        for (float v : inv)
            f2 += v * v;
        minScaleSq = 1.f / f2;
        // Columns (images of the model axes) square to each other within e:
        // |A d|^2 >= (1 - 2e) min |column|^2 |d|^2.
        float n[3];
        for (int j = 0; j < 3; j++)
            n[j] = a[j] * a[j] + a[3 + j] * a[3 + j] + a[6 + j] * a[6 + j];
        bool square = true;
        for (int i = 0; i < 3; i++)
            for (int j = i + 1; j < 3; j++)
            {
                const float d = a[i] * a[j] + a[3 + i] * a[3 + j] + a[6 + i] * a[6 + j];
                square = square && d * d <= 1e-6f * n[i] * n[j];
            }
        if (square)
        {
            const float m = std::fmin(n[0], std::fmin(n[1], n[2])) * 0.998f;
            minScaleSq = m > minScaleSq ? m : minScaleSq;
        }
        return true;
    }

    // p in lighting space (-X, Y, -Z).
    float operator()(Vec3 p) const
    {
        const float v[3] = {-p.x - t[0], p.y - t[1], -p.z - t[2]};
        const Vec3 q = {inv[0] * v[0] + inv[1] * v[1] + inv[2] * v[2], inv[3] * v[0] + inv[4] * v[1] + inv[5] * v[2],
                        inv[6] * v[0] + inv[7] * v[1] + inv[8] * v[2]};
        return distanceSq(box, q) * minScaleSq;
    }
};

// The list being drawn, for its model's box (naomi_elan_cmd.cpp).
float s_listBox[6];
int s_listBoxState = kListNoBox;

inline bool listIsLit()
{
    return s.lightModel.set && !(s.material.set && (s.material.select >> 9 & 1));
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
    float nearest;          // the frame's near plane: least nearW of the lists recorded
    float nearestLastFrame;
};
DepthPin s_depth = {false, false, false, 1e30f, 1e30f, 0, 0, 0, 1e30f, 1e30f};

// While pinned, each list is still measured one render in this many.
constexpr u32 kDepthWindow = 32;

// The near plane of the game's 3D matrices, where a race is pinned.
constexpr float kRacePlane = 0.0099f;

#if !defined(GEKKO)
struct
{
    u32 reach, close;       // unpinned, vertices measured: box reaches its near plane, or may be the nearest
    u32 slice, hidden;      // pinned, the same list's slice; out of the picture, for the pin
    u32 noBoxLit, noBox;    // no box: point-lit, others
    u32 noBoxLists;
} s_nearTrace;
#endif

// A list wholly behind the camera and with no box is dropped before it reaches
// the renderer, so it gets no fetch program, so no box: it was measured again
// every render it had to be (point-lit lists always, any list without a pin).
// Night demo: 160-230 lists, 12-22K vertices a frame while their stretch of
// road stayed behind the camera. The box is kept here instead, under the same
// rule as a program's: good while the list's pages stay unwritten.
struct DroppedList
{
    const u32* vtx;
    u32 count, vertexSize, render;
    float box[6];
};
constexpr u32 kDroppedSets = 256, kDroppedWays = 4;    // a power of two sets
DroppedList s_dropped[kDroppedSets][kDroppedWays];

inline DroppedList* droppedSet(const u32* vtx, u32 count)
{
    return s_dropped[(((u32)(size_t)vtx >> 2) * 2654435761u >> 24 ^ count) & (kDroppedSets - 1)];
}

void keepDroppedBox(const u32* vtx, u32 count, u32 vertexSize)
{
    // The way used longest ago goes: free ones have render 0.
    DroppedList* set = droppedSet(vtx, count);
    DroppedList* oldest = set;
    for (u32 w = 0; w < kDroppedWays; w++)
    {
        if (set[w].vtx == vtx && set[w].count == count)
        {
            oldest = &set[w];
            break;
        }
        if (set[w].render < oldest->render)
            oldest = &set[w];
    }
    DroppedList& d = *oldest;
    d.vtx = vtx;
    d.count = count;
    d.vertexSize = vertexSize;
    d.render = elan_gx_render();
    for (int c = 0; c < 3; c++)
    {
        d.box[c] = 1e30f;
        d.box[3 + c] = -1e30f;
    }
    for (u32 i = 0; i < count; i++, vtx += vertexSize / 4)
    {
        const float p[3] = {asFloat(vtx[1]), asFloat(vtx[2]), asFloat(vtx[3])};
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            continue;                                // a placeholder
        for (int c = 0; c < 3; c++)
        {
            d.box[c] = p[c] < d.box[c] ? p[c] : d.box[c];
            d.box[3 + c] = p[c] > d.box[3 + c] ? p[c] : d.box[3 + c];
        }
    }
}

bool droppedBox(const u32* vtx, u32 count, u32 vertexSize, float* box)
{
    DroppedList* set = droppedSet(vtx, count);
    for (u32 w = 0; w < kDroppedWays; w++)
    {
        const DroppedList& d = set[w];
        if (d.vtx != vtx || d.count != count || d.vertexSize != vertexSize)
            continue;
        if (!elan_gx_unwritten_since(vtx, count * vertexSize, d.render))
            return false;
        memcpy(box, d.box, sizeof(d.box));
#if !defined(GEKKO)
        elan_gx_box_check(vtx, count, vertexSize, box);
#endif
        return true;
    }
    return false;
}

// The list's state as an ElanGxRecord for the GPU path, or false when the
// GX hardware cannot reproduce its lighting (point lights unless NIGHT
// LIGHTS = GPU, unusual routing, more than 8 parallel lights, a layout
// without a plain vertex format).
bool recordForGpu(const u32* w, u32 vertexSize, u32 count, u32 layout, u32 pcw, u32 isp, u32 tsp, bool envMap)
{
    if (!gpuLayout(layout))
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
    //
    // Lists lit by point lights (NIGHT LIGHTS = GPU) are measured every time,
    // in X and Y too: their box decides which lights reach them.
    //
    // 3D LIST CULL: a list with a GPU fetch program has its model-space box
    // (built with the program, good while its pages are unwritten). Its 8
    // corners under the matrix bound every vertex, so a box behind the near
    // plane (or, OUT OF VIEW, beside the picture) is a list that draws nothing:
    // dropped before it costs the renderer anything (in a race ~40% of the
    // lists: the track behind the camera). Pinned, the box's depth range
    // stands in for the planes and a box that cannot reach its near plane
    // needs no measuring: only lists that may reach it keep their slice of
    // the window (which keeps the pin), and point-lit lists take their lights
    // from the box. A list dropped out of the picture that may reach its near
    // plane still counts for the pin as if drawn: in a race that is the road
    // under the camera, below the picture; without it the pin fell as soon as
    // the race camera took over, and every list was measured (Dolphin: 10%
    // speed). Unpinned (menus, replays), a box is looser than its vertices and
    // the frame's near plane is its precision: lists whose box may be the
    // nearest (under twice the last frame's near plane) are measured, the
    // others cannot set it and take their box's range.
    //
    // DEPTH MEASURE = RACE PLANE: a box is looser than its vertices, so in
    // fly-bys and replays many boxes cross the near plane though no vertex does
    // (road lists): measuring them all cost 11-35K vertices a frame (Wii, 5-11
    // ms). It is all or nothing, one such list left unmeasured holds the frame's
    // near plane at its box anyway. So those frames keep the plane as a race
    // does, with the race's precision and its 1-in-32 slices.
    const float nearZ = s.matrix.nearZ > 0.f ? s.matrix.nearZ : 0.001f;
    const bool pointLit = g_elan_point_preset && litByPointLights();
    NaomiElanStats& st = elan_Stats();
    // Every list takes an index, dropped ones too: otherwise each list after a
    // measured one found behind the camera got the same index and was measured
    // as well, i.e. the whole stretch of track behind the camera every render.
    const u32 index = s_depth.lists++;
    const bool pinned = s_depth.pinnedLastFrame && nearZ >= s_depth.pinPlaneLastFrame;
    const bool keepPlane = pinned || (g_depth_plane_preset && nearZ >= kRacePlane);
    const bool due = (index + s_depth.frames) % kDepthWindow == 0;
    float mbox[6];
    const bool boxed = g_elan_box_preset && (elan_gx_list_box(r.vtx, count, vertexSize, mbox) ||
                                             droppedBox(r.vtx, count, vertexSize, mbox));
    bool measure = pointLit || !pinned || due;
    ModelBoxDistance lightDistance;                  // point-lit, not measured: lights against the box
    if (boxed)
    {
        st.boxLists++;
        memcpy(s_listBox, mbox, sizeof(mbox));
        s_listBoxState = kListBoxed;
        const float* t = s.matrix.tm;
        const BoxView& view = boxView();
        const float zlo = view.z.bottom(mbox), zhi = view.z.top(mbox);
        const bool empty = mbox[0] > mbox[3];        // placeholders only
        const bool behind = empty || zhi < nearZ;
        const bool reach = zlo <= nearZ * 1.5f;
        const bool side = !behind && g_elan_box_preset >= 2 && outOfPicture(view, mbox);
        const bool small = !behind && !side && !reach && tooSmall(view, mbox, zlo);
        if (behind || side || small)
        {
            (behind ? st.boxCulledBehind : side ? st.boxCulledSide : st.boxCulledSmall)++;
            st.boxCulledVertices += count;
            if (!behind && reach)
            {
                bool hit = keepPlane && !due;            // assumed, as if drawn
                if (!hit)
                {
                    float minZ, maxZ;
                    measureList<false>(w + 8, count, vertexSize, t, minZ, maxZ, nullptr);
                    st.depthMeasured += count;
#if !defined(GEKKO)
                    s_nearTrace.hidden += count;
#endif
                    hit = maxZ >= nearZ && minZ <= nearZ * 1.5f;
                    if (hit)
                        s_depth.windowHit = true;
                }
                if (hit)
                {
                    s_depth.pinned = true;
                    if (nearZ < s_depth.pinPlane)
                        s_depth.pinPlane = nearZ;
                }
            }
            return true;
        }
        measure = (keepPlane ? reach && due : reach) || (!pinned && zlo < 2.f * s_depth.nearestLastFrame) ||
                  (pointLit && !lightDistance.init(mbox, t));
#if !defined(GEKKO)
        if (measure)
            (pinned ? s_nearTrace.slice : reach ? s_nearTrace.reach : s_nearTrace.close) += count;
#endif
        if (!measure)
        {
            r.nearW = zlo > nearZ ? zlo : nearZ;
            r.farW = zhi;
            if (reach)
                s_depth.pinned = true;               // still assumed, as with the planes
        }
    }
    else if (!measure)
    {
        r.nearW = nearZ;
        r.farW = s.matrix.farZ > nearZ && s.matrix.farZ < 100000.f ? s.matrix.farZ : 100000.f;
        s_depth.pinned = true;                       // still assumed, until the next measured frame
    }
    Box box;
    if (measure)
    {
#if !defined(GEKKO)
        if (!boxed)
        {
            (pointLit ? s_nearTrace.noBoxLit : s_nearTrace.noBox) += count;
            s_nearTrace.noBoxLists++;
        }
#endif
        float minZ, maxZ;
        if (pointLit)
            measureList<true>(w + 8, count, vertexSize, s.matrix.tm, minZ, maxZ, &box);
        else
            measureList<false>(w + 8, count, vertexSize, s.matrix.tm, minZ, maxZ, nullptr);
        st.depthMeasured += count;
        if (maxZ < nearZ)
        {
            if (g_elan_box_preset && !boxed)
                keepDroppedBox(r.vtx, count, vertexSize);
            return true;                             // entirely behind the near plane
        }
        r.nearW = minZ > nearZ ? minZ : nearZ;
        r.farW = maxZ;
        if (r.nearW <= nearZ * 1.5f)
            s_depth.pinned = s_depth.windowHit = true;
    }
    if (r.nearW <= nearZ * 1.5f && nearZ < s_depth.pinPlane)
        s_depth.pinPlane = nearZ;
    if (r.nearW < s_depth.nearest)
        s_depth.nearest = r.nearW;
#if !defined(GEKKO)
    if (boxed)
    {
        const BoxView& view = boxView();
        const float zlo = view.z.bottom(mbox);
        int fb = 0;
        for (float lim = 25.f; fb < 7 && zlo >= lim; lim *= 2.f)
            fb++;
        st.farBins[fb] += count;
        // Diagonal in view units: the matrix may scale, so through the rows.
        const float* t = s.matrix.tm;
        const float d[3] = {mbox[3] - mbox[0], mbox[4] - mbox[1], mbox[5] - mbox[2]};
        float diag2 = 0.f;
        for (int row = 0; row < 3; row++)
        {
            const float v = t[row] * d[0] + t[3 + row] * d[1] + t[6 + row] * d[2];
            diag2 += v * v;
        }
        // On screen, in pixels: the projection's x scale over the depth.
        const float ratio = zlo > nearZ ? std::fabs(s.proj[0]) * std::sqrt(diag2) / zlo : 1e9f;
        int sb = 0;
        for (float lim = 2.f; sb < 7 && ratio >= lim; lim *= 2.f)
            sb++;
        st.sizeBins[sb] += count;
        st.sizeBinLists[sb]++;
    }
#endif

    if (pointLit)
    {
        // Picked for every list (its box), built when the pick changes.
        const bool built = measure ? pointLightRecord(r, [&](Vec3 p) { return distanceSq(box, p); })
                                   : pointLightRecord(r, lightDistance);
        if (!built)
            return false;
    }
    else if (r.lightKey != s_lightGen * 2u + (u32)listIsLit())
    {
        // The material only decides whether a list is lit at all, hence the
        // key: most lists have a material of their own, and a set rebuilt for
        // each was ~900 times a frame 500 bytes cleared, filled and compared.
        r.lightKey = 0;                              // until built: a refused set is rebuilt next time
        memset(&r.lights, 0, sizeof(r.lights));
        const LightModel& lm = s.lightModel;
        r.lights.lit = listIsLit();
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
        r.lightKey = s_lightGen * 2u + (u32)r.lights.lit;
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

// The last material of a model stepped over because it is out of the picture
// (tnl_ModelHidden): decoded only if a list comes before the next material.
static const u32* s_deferredMaterial = nullptr;

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
    // The distance attenuation clamp(B / d + A) (or B * d + A) reaches 0 at a
    // fixed distance and stays there: Initial D's night courses light every
    // vertex with ~9 point lights (street lamps reach 40 units, the car's 100
    // to 200), and most are that far from most vertices.
    if (l.distAtten && !l.distMode && l.distA < 0.f && l.distB > 0.f)
        l.reachSq = (l.distB / l.distA) * (l.distB / l.distA);
    else if (l.distAtten && l.distMode && l.distA > 0.f && l.distB < 0.f)
        l.reachSq = (l.distA / l.distB) * (l.distA / l.distB);
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
    s_deferredMaterial = nullptr;
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

bool tnl_BoxHidden(const float* box)
{
    const float nearZ = s.matrix.nearZ > 0.f ? s.matrix.nearZ : 0.001f;
    const BoxView& view = boxView();
    if (box[0] > box[3] || view.z.top(box) < nearZ)
        return true;
    const float zlo = view.z.bottom(box);
    return zlo > nearZ * 1.5f && (outOfPicture(view, box) || tooSmall(view, box, zlo));
}

void tnl_ModelHidden(const u32* lastMaterial, u32 listsIndexed)
{
    if (lastMaterial)
        s_deferredMaterial = lastMaterial;
    s_depth.lists += listsIndexed;
}

u32 tnl_ListsIndexed()
{
    return s_depth.lists;
}

int tnl_ListBox(float* box)
{
    if (s_listBoxState == kListBoxed)
        memcpy(box, s_listBox, sizeof(s_listBox));
    return s_listBoxState;
}

void tnl_ResetState()
{
    s_xformGen++;
    s_lightGen++;
    s_stateSkippedSinceReset = false;
    s_deferredMaterial = nullptr;
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
#if !defined(GEKKO)
        static const bool nearTrace = getenv("IDAS3_NEAR_TRACE") != nullptr;
        if (nearTrace)
            printf("[near] render %u: pinned %d near %.4f plane %.4f | unpinned measured: reach %u, close %u | slice %u, "
                   "hidden %u, no box lit %u, no box %u (%u lists)\n",
                   s_depth.frames, s_depth.pinnedLastFrame ? 1 : 0, s_depth.nearest, s_depth.pinPlane, s_nearTrace.reach,
                   s_nearTrace.close, s_nearTrace.slice, s_nearTrace.hidden, s_nearTrace.noBoxLit, s_nearTrace.noBox,
                   s_nearTrace.noBoxLists);
        memset(&s_nearTrace, 0, sizeof(s_nearTrace));
#endif
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
        s_depth.nearestLastFrame = s_depth.nearest;
        s_depth.frames++;
    }
    s_depth.pinned = false;
    s_depth.pinPlane = 1e30f;
    s_depth.nearest = 1e30f;
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
    if (s_deferredMaterial)
        tnl_SetMaterial(s_deferredMaterial);
    s_listBoxState = kListNoBox;
    const u32 pcw = w[0];
    const u32 layout = w[6];
    const u32 count = w[7];
    const int list = listType >= 0 ? listType : (int)(pcw >> 24 & 7);
    if (list & 1)
    {
        s_listBoxState = kListNotDrawn;
        return listType;                          // NAOMI 2 modifier volumes: not drawn yet
    }
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
#if !defined(GEKKO)
    // NIGHT LIGHTS = GPU, Windows preview: the point-lit lists the Wii's GPU
    // path would take are lit as GX would (--gpu-tnl drops them instead).
    ElanGxLightSet preview;
    if (g_elan_point_preset && gpuLayout(layout) && (list == 0 || (!g_trans_sort_preset && !g_autosort_preset)) &&
        litByPointLights())
    {
        Box box;
        float minZ, maxZ;
        measureList<true>(w + 8, count, vertexSize, s.matrix.tm, minZ, maxZ, &box);
        if (pointLightSet(preview, [&](Vec3 p) { return distanceSq(box, p); }))
            s_gxPreview = &preview;
    }
#endif

    // Strips and fans, linked into one strip with degenerate triangles, sent
    // a piece at a time.
    s_strip.n = 0;
    const u32* v = w + 8;
    TnlVtx fanCenter = {}, fanLast = {};
    bool stripStart = true;
    bool lit = false;
    const float nearZ = s.matrix.nearZ > 0.f ? s.matrix.nearZ : 0.001f;
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
            if (s_strip.n)
            {
                s_strip.push(fanLast);
                s_strip.push(cur);
                if (s_strip.n & 1)
                    s_strip.push(cur);
            }
            stripStart = false;
        }
        else if ((header >> 29 & 3) == 2)        // fan vertex
        {
            s_strip.push(fanLast);
            s_strip.push(fanCenter);
            if (s_strip.n & 1)
                s_strip.push(fanCenter);
            s_strip.push(fanCenter);
            s_strip.push(fanLast);
        }
        s_strip.push(cur);
        fanLast = cur;
        if (header >> 31)
            stripStart = true;
        if (s_strip.n >= kMaxStripVertices)
        {
            // A full piece: the next one repeats its last two vertices, from an
            // even index (same winding) - the split TaWriter::strip makes.
            sendStrip(s_strip.v, kMaxStripVertices, nearZ);
            s_strip.n -= kMaxStripVertices - 2;
            memmove(s_strip.v, s_strip.v + kMaxStripVertices - 2, s_strip.n * sizeof(TnlVtx));
        }
    }
    ELAN_TIME_END(convNs, tv);
#if !defined(GEKKO)
    s_gxPreview = nullptr;
#endif
    sendStrip(s_strip.v, s_strip.n, nearZ);
    s_strip.n = 0;
    ELAN_TIME_END(tnlNs, t0);
    return list;
}
