// Software renderer for the Windows build of the core (the Wii build uses
// plugs/drkPvr/gxRend.cpp). It implements the same interface - the rend_*
// entry points and the TA VertexDecoder - and keeps the render-done timing
// the GX renderer uses, so the game sees the same hardware behaviour.
//
// It always collects the display lists (cheap); when snapshots are enabled
// it rasterises the frame in software and writes it as a PNG, which is how
// the headless runner shows what the game draws. Coverage is what bring-up
// needs: opaque / punch-through / translucent polygons and sprites, Gouraud
// or flat colour, offset colour, the four texture shading instructions,
// source/destination blending, the PowerVR depth modes, and textures in
// every pixel format (twiddled, strided, VQ, palettes, mipmaps; point
// sampled), user tile clipping, back-face culling (runner --cull). Not
// modelled: modifier volumes, fog, translucent sorting, bump maps, the
// background plane (cleared to grey).
#include "types.h"

#if HOST_OS != OS_WII

#include "plugs/drkPvr/drkPvr.h"
#include "plugs/drkPvr/ta.h"
#include "plugs/drkPvr/regs.h"
#include "plugs/drkPvr/Renderer_if.h"
#include "dc/pvr/pvr_if.h"
#include "host_hooks.h"
#include "naomi/naomi_elan_gx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace TASplitter;

HostRenderStats g_host_render;

extern s32 render_end_pending_cycles;   // SPG.cpp
extern int g_isp_cull_preset;           // presets_generated.cpp

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

// ---------------------------------------------------------------------------
// Captured display lists
// ---------------------------------------------------------------------------
struct Vtx
{
    float x, y, z;          // screen position, z = 1/w
    float u, v;
    float col[4];           // r g b a, 0..1
    float ofs[4];
};

struct State
{
    u32 pcw, isp, tsp, tcw;
    u32 clip;               // user tile clip: mode << 30 | x0 << 24 | y0 << 16 | x1 << 8 | y1 (tiles)
    u32 run = 0;            // IDAS3_STRIP_ORDER's run (s_order index)
};

struct Tri
{
    u32 v[3];
    u32 state;
};

struct Frame
{
    std::vector<Vtx> vtx;
    std::vector<State> states;
    std::vector<Tri> tris[5];   // per list type: 0 opaque, 1 op. mod, 2 trans., 3 tr. mod, 4 punch-through

    void clear()
    {
        vtx.clear();
        states.clear();
        for (auto& t : tris)
            t.clear();
    }
};

Frame s_frame;
u32 s_list;
float s_face[4] = {1, 1, 1, 1};
float s_faceOfs[4];
u32 s_stripCount;
u32 s_stripFirst;
Vtx s_pending;                  // 64-byte vertices arrive in two halves
u32 s_tileClipRect;             // last User Tile Clip parameter, packed like State::clip
u32 s_tileClip;                 // with the current mode in bits 30-31
u32 s_spriteBase, s_spriteOfs;
float s_spriteA[7];

std::string s_snapDir;
u32 s_snapEvery;
u32 s_lastSnap;
bool s_snapRequested;

// Bring-up: recent decoder events, printed with each snapshot.
char s_events[64];
u32 s_eventPos;
inline void event(char c)
{
    s_events[s_eventPos++ & 63] = c;
}

inline float f16(u16 x)
{
    const u32 bits = (u32)x << 16;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

inline void unpack(u32 argb, float* out)
{
    out[0] = ((argb >> 16) & 0xFF) / 255.f;
    out[1] = ((argb >> 8) & 0xFF) / 255.f;
    out[2] = (argb & 0xFF) / 255.f;
    out[3] = (argb >> 24) / 255.f;
}

inline void intensity(float i, const float* face, float* out)
{
    out[0] = face[0] * i;
    out[1] = face[1] * i;
    out[2] = face[2] * i;
    out[3] = face[3];
}

// Env IDAS3_STRIP_ORDER=FROM:TO: each render in those vblanks printed as runs
// of polygon headers in submission order (list type; E a GPU-drawn ELAN list,
// P a TA polygon, S a TA sprite), with their vertices, screen box, 1/W range
// and first texture: where the 3D scene ends and the HUD starts (MOTION FX).
struct OrderRun
{
    char kind;
    u32 list, headers, vtx;
    float x0, y0, x1, y1, z0, z1;
    u32 tcw;
};
std::vector<OrderRun> s_order;
char s_orderKind = 'P';

bool orderOn()
{
    static int s_on = -1;
    if (s_on < 0)
        s_on = getenv("IDAS3_STRIP_ORDER") != nullptr;
    return s_on != 0;
}

void orderHeader(char kind, u32 list, u32 tcw, float z0, float z1)
{
    if (!orderOn())
        return;
    if (!s_order.empty() && s_order.back().kind == kind && s_order.back().list == list)
    {
        OrderRun& r = s_order.back();
        r.headers++;
        if (z0 < r.z0)
            r.z0 = z0;
        if (z1 > r.z1)
            r.z1 = z1;
        return;
    }
    s_order.push_back({kind, list, 1, 0, 1e9f, 1e9f, -1e9f, -1e9f, z0, z1, tcw});
}

void orderVertex(const Vtx& v)
{
    if (!orderOn() || s_order.empty())
        return;
    OrderRun& r = s_order.back();
    r.vtx++;
    r.x0 = v.x < r.x0 ? v.x : r.x0;
    r.y0 = v.y < r.y0 ? v.y : r.y0;
    r.x1 = v.x > r.x1 ? v.x : r.x1;
    r.y1 = v.y > r.y1 ? v.y : r.y1;
    r.z0 = v.z < r.z0 ? v.z : r.z0;
    r.z1 = v.z > r.z1 ? v.z : r.z1;
}

void newState(u32 pcw, u32 isp, u32 tsp, u32 tcw)
{
    s_frame.states.push_back({pcw, isp, tsp, tcw, s_tileClip});
    s_stripCount = 0;
    orderHeader(s_orderKind, s_list, tcw, 1e9f, -1e9f);
    if (orderOn())
        s_frame.states.back().run = (u32)s_order.size() - 1;
}

// IDAS3_STRIP_ORDER with snapshots: each run drawn alone as well (-1: all).
int s_onlyRun = -1;

void addVertex(const Vtx& v, bool polygon = true)
{
    const u32 index = (u32)s_frame.vtx.size();
    s_frame.vtx.push_back(v);
    orderVertex(v);
    if (s_stripCount == 0)
        s_stripFirst = index;
    s_stripCount++;
    if (s_stripCount >= 3 && !s_frame.states.empty() && s_list < 5)
    {
        const u32 k = s_stripCount - 3;
        Tri t;
        t.state = (u32)s_frame.states.size() - 1;
        if (k & 1)
            t.v[0] = index - 1, t.v[1] = index - 2, t.v[2] = index;
        else
            t.v[0] = index - 2, t.v[1] = index - 1, t.v[2] = index;
        s_frame.tris[s_list].push_back(t);
    }
    if (polygon)
        g_host_render.vertices++;
}

Vtx position(const f32* xyz)
{
    Vtx v = {};
    v.x = xyz[0];
    v.y = xyz[1];
    v.z = xyz[2];
    v.col[0] = v.col[1] = v.col[2] = v.col[3] = 1.f;
    return v;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
u32 twiddle(u32 x, u32 y, u32 w, u32 h)
{
    u32 r = 0, bit = 0;
    for (u32 sw = w >> 1, sh = h >> 1; sw || sh;)
    {
        if (sh)
        {
            r |= (y & 1) << bit++;
            y >>= 1;
            sh >>= 1;
        }
        if (sw)
        {
            r |= (x & 1) << bit++;
            x >>= 1;
            sw >>= 1;
        }
    }
    return r;
}

inline u8 vram8(u32 addr)
{
    return vram.data[addr & VRAM_MASK];
}

inline u16 vram16(u32 addr)
{
    return *(u16*)&vram.data[addr & VRAM_MASK & ~1u];
}

u32 expand16(u16 p, u32 format)
{
    u32 a, r, g, b;
    switch (format)
    {
    case 1:     // RGB565
        a = 255;
        r = (p >> 11) & 31; r = r << 3 | r >> 2;
        g = (p >> 5) & 63;  g = g << 2 | g >> 4;
        b = p & 31;         b = b << 3 | b >> 2;
        break;
    case 2:     // ARGB4444
        a = ((p >> 12) & 15) * 17;
        r = ((p >> 8) & 15) * 17;
        g = ((p >> 4) & 15) * 17;
        b = (p & 15) * 17;
        break;
    default:    // ARGB1555 (also used for the reserved format)
        a = p & 0x8000 ? 255 : 0;
        r = (p >> 10) & 31; r = r << 3 | r >> 2;
        g = (p >> 5) & 31;  g = g << 3 | g >> 2;
        b = p & 31;         b = b << 3 | b >> 2;
        break;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

u32 palette(u32 index)
{
    const u32 entry = PvrReg(PALETTE_RAM_START_addr + (index & 1023) * 4, u32);
    switch (PvrReg(PAL_RAM_CTRL_addr, u32) & 3)
    {
    case 0: return expand16((u16)entry, 0);
    case 1: return expand16((u16)entry, 1);
    case 2: return expand16((u16)entry, 2);
    default: return entry;      // ARGB8888
    }
}

u32 yuvToArgb(int y, int u, int v)
{
    auto clamp = [](int c) { return c < 0 ? 0 : c > 255 ? 255 : c; };
    const int r = clamp(y + (int)(1.375f * (v - 128)));
    const int g = clamp(y - (int)(0.34375f * (u - 128) + 0.6875f * (v - 128)));
    const int b = clamp(y + (int)(1.71875f * (u - 128)));
    return 0xFF000000u | (u32)r << 16 | (u32)g << 8 | (u32)b;
}

// Offset in texels of the largest mipmap level, for a texture 2^n wide.
u32 mipOffset(u32 n, bool vq)
{
    u32 sum = 0;
    if (vq)
    {
        for (u32 k = 0; k + 1 < n; k++)
            sum += 1u << (2 * k);
        return 1 + sum;
    }
    for (u32 k = 0; k < n; k++)
        sum += 1u << (2 * k);
    return 3 + sum;
}

struct Texture
{
    u32 w, h;
    std::vector<u32> argb;
};

Texture decode(u32 tcw, u32 tsp)
{
    Texture t;
    t.w = 8u << ((tsp >> 3) & 7);
    t.h = 8u << (tsp & 7);
    t.argb.resize(t.w * t.h);

    const u32 format = (tcw >> 27) & 7;
    const bool vq = (tcw >> 30) & 1;
    const bool mip = (tcw >> 31) & 1;
    const bool linear = (tcw >> 26) & 1 && format != 5 && format != 6;
    const bool strided = linear && ((tcw >> 25) & 1);
    u32 addr = (tcw & 0x1FFFFF) << 3;
    u32 log2w = 0;
    while ((1u << log2w) < t.w)
        log2w++;

    if (vq)
    {
        const u32 book = addr;
        u32 indices = addr + 2048 + (mip ? mipOffset(log2w, true) : 0);
        for (u32 y = 0; y < t.h; y++)
            for (u32 x = 0; x < t.w; x++)
            {
                const u32 idx = vram8(indices + twiddle(x >> 1, y >> 1, t.w >> 1, t.h >> 1));
                const u16 p = vram16(book + (idx * 4 + ((x & 1) << 1 | (y & 1))) * 2);
                t.argb[y * t.w + x] = expand16(p, format);
            }
        return t;
    }

    if (format == 5 || format == 6)
    {
        const u32 pal = (tcw >> 21) & 0x3F;
        const u32 base = mip ? mipOffset(log2w, false) : 0;
        for (u32 y = 0; y < t.h; y++)
            for (u32 x = 0; x < t.w; x++)
            {
                const u32 texel = base + twiddle(x, y, t.w, t.h);
                u32 index;
                if (format == 5)
                {
                    const u8 b = vram8(addr + (texel >> 1));
                    index = (pal << 4) | ((texel & 1) ? b >> 4 : b & 15);
                }
                else
                    index = ((pal >> 4) << 8) | vram8(addr + texel);
                t.argb[y * t.w + x] = palette(index);
            }
        return t;
    }

    u32 stride = t.w;
    if (strided)
        stride = (PvrReg(TEXT_CONTROL_addr, u32) & 31) * 32;
    if (mip && !linear)
        addr += mipOffset(log2w, false) * 2;
    for (u32 y = 0; y < t.h; y++)
        for (u32 x = 0; x < t.w; x++)
        {
            const u32 texel = linear ? y * stride + x : twiddle(x, y, t.w, t.h);
            u32 c;
            if (format == 3)            // YUV422: pixel pairs share U and V
            {
                const u32 pair = linear ? y * stride + (x & ~1u) : twiddle(x & ~1u, y, t.w, t.h);
                const u16 p0 = vram16(addr + pair * 2), p1 = vram16(addr + (pair + (linear ? 1 : 2)) * 2);
                const int yv = (x & 1) ? p1 >> 8 : p0 >> 8;
                c = yuvToArgb(yv, p0 & 0xFF, p1 & 0xFF);
            }
            else if (format == 4)       // bump map: show as grey
                c = 0xFF808080;
            else
                c = expand16(vram16(addr + texel * 2), format);
            t.argb[y * t.w + x] = c;
        }
    return t;
}

std::unordered_map<u64, Texture> s_textures;

const Texture& texture(u32 tcw, u32 tsp)
{
    const u64 key = (u64)tcw << 32 | (tsp & 0x3F) | (u64)(PvrReg(PAL_RAM_CTRL_addr, u32) & 3) << 6;
    auto it = s_textures.find(key);
    if (it != s_textures.end())
        return it->second;
    return s_textures.emplace(key, decode(tcw, tsp)).first->second;
}

// ---------------------------------------------------------------------------
// Rasteriser
// ---------------------------------------------------------------------------
u32 s_color[kWidth * kHeight];
float s_depth[kWidth * kHeight];

// Env IDAS3_GX_DEPTH=NEAR:FAR[:RULES][;NEAR:FAR[:RULES]...] (up to 8): each
// snapshot is also drawn with the Wii's depth buffer (1/W mapped onto 24-bit
// integers between those planes, like gxRend's projection; NEAR 0 = exact
// 1/W), and _diffK images mark the pixels that differ from the first one; the
// lists that win those pixels are counted. RULES, joined by '+':
//   geq  GEQUAL for every list (gxRend with ISP DEPTH FUNC = OFF)
//   isp  opaque/punch-through: the polygon's depth mode, translucent GEQUAL
//   fly  Flycast: opaque the polygon's mode, punch-through and translucent
//        GEQUAL, translucent without depth writes (auto-sort)
//   nzw  translucent without depth writes
//   bK   translucent depth K steps nearer (a Z offset)
//   rK   compared with configuration K instead of the first
//   jK   every depth K steps nearer (K < 1: moves where Z rounds)
//   cull / cullx  the polygons' cull modes (cullx: the other winding), like
//        runner --cull 2 / 1 (BACKFACE CULL)
//   none of geq/isp/fly: every list uses its polygon's depth mode
// A configuration and the same with j0.5 against it show which pixels depend
// on Z rounding, i.e. flicker when the camera moves.
struct GxDepth
{
    bool quantize = false, geq = false, isp = false, fly = false, nzw = false;
    int cull = 0;           // as g_isp_cull_preset
    float bias = 0.f, jitter = 0.f;
    u32 ref = 0;
    float p5 = 0.f, p6 = 0.f;
};
GxDepth s_gxCfg[8];
u32 s_gxCount = 0;
const GxDepth* s_gx = nullptr;  // the current rasterise() emulates this Z buffer
u32 s_owner[kWidth * kHeight];  // list << 24 | state of the last write, + 1

void gxDepthInit()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    const char* e = getenv("IDAS3_GX_DEPTH");
    while (e && *e && s_gxCount < 8)
    {
        float n = -1.f, f = 0.f;
        char rules[32] = {};
        if (sscanf(e, "%f:%f:%31[a-z0-9.+]", &n, &f, rules) < 2 || n < 0.f || f <= n)
            break;
        GxDepth& g = s_gxCfg[s_gxCount++];
        g.quantize = n > 0.f;
        for (char* r = strtok(rules, "+"); r; r = strtok(nullptr, "+"))
        {
            g.geq |= !strcmp(r, "geq");
            g.isp |= !strcmp(r, "isp");
            g.fly |= !strcmp(r, "fly");
            g.nzw |= !strcmp(r, "nzw");
            if (r[0] == 'b')
                g.bias = (float)atof(r + 1);
            if (r[0] == 'r' && (u32)atoi(r + 1) < s_gxCount - 1)
                g.ref = (u32)atoi(r + 1);
            if (r[0] == 'j')
                g.jitter = (float)atof(r + 1);
            if (!strcmp(r, "cull"))
                g.cull = 2;
            if (!strcmp(r, "cullx"))
                g.cull = 1;
        }
        if (g.quantize)
        {
            g.p6 = -1.f / (1.f / f - 1.f / n);
            g.p5 = g.p6 / n;
        }
        printf("[gxdepth] #%u: near %g far %g%s%s%s%s%s bias %g jitter %g cull %d, against #%u\n", s_gxCount - 1, n,
               f, g.quantize ? "" : " (exact 1/W)", g.geq ? " geq" : "", g.isp ? " isp" : "", g.fly ? " fly" : "",
               g.nzw ? " nzw" : "", g.bias, g.jitter, g.cull, g.ref);
        e = strchr(e, ';');
        e = e ? e + 1 : nullptr;
    }
}

// 1/W -> the 24-bit Z the Wii would store (float math like the XF's).
inline float gxZ(float invW, float bias)
{
    if (!s_gx || !s_gx->quantize)
        return invW;
    const float W = 1.f / invW;
    const float zs = ((s_gx->p5 * -W + s_gx->p6) / W) * 16777215.f + 16777215.f + bias + s_gx->jitter;
    return zs < 0.f ? 0.f : zs > 16777215.f ? 16777215.f : zs;
}

bool depthPass(u32 mode, float z, float stored)
{
    switch (mode)
    {
    case 0: return false;
    case 1: return z < stored;
    case 2: return z == stored;
    case 3: return z <= stored;
    case 4: return z > stored;
    case 5: return z != stored;
    case 6: return z >= stored;
    default: return true;
    }
}

float blendFactor(u32 instr, const float* src, const float* dst, int c, bool forSource)
{
    const float* other = forSource ? dst : src;
    switch (instr)
    {
    case 0: return 0.f;
    case 1: return 1.f;
    case 2: return other[c];
    case 3: return 1.f - other[c];
    case 4: return src[3];
    case 5: return 1.f - src[3];
    case 6: return dst[3];
    default: return 1.f - dst[3];
    }
}

inline float clamp01(float v)
{
    return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
}

u32 wrap(int c, u32 size, bool clamp, bool flip)
{
    if (clamp)
        return c < 0 ? 0 : c >= (int)size ? size - 1 : (u32)c;
    if (flip)
    {
        const u32 period = (u32)c & (2 * size - 1);
        return period < size ? period : 2 * size - 1 - period;
    }
    return (u32)c & (size - 1);
}

void drawTriangle(const Vtx& a, const Vtx& b, const Vtx& c, const State& st, u32 list)
{
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    // Games send placeholder triangles at infinity; the PVR draws nothing.
    if (!std::isfinite(area) || std::fabs(area) < 1e-6f || std::fabs(a.x) > 1e6f || std::fabs(b.x) > 1e6f ||
        std::fabs(c.x) > 1e6f || std::fabs(a.y) > 1e6f || std::fabs(b.y) > 1e6f || std::fabs(c.y) > 1e6f)
        return;
    // BACKFACE CULL (g_isp_cull_preset, runner --cull): ISP cull mode 2 drops
    // a negative area (counter-clockwise on screen), 3 a positive one; preset
    // 1 swaps them, as gxRend's ISP_CULL 1 does against 2.
    const int cull = s_gx ? s_gx->cull : g_isp_cull_preset;
    if (cull)
    {
        const u32 cullMode = (st.isp >> 27) & 3;
        const float signedArea = cull == 2 ? area : -area;
        if ((cullMode == 2 && signedArea < 0.f) || (cullMode == 3 && signedArea > 0.f))
            return;
    }
    int minX = (int)std::floor(std::fmin(a.x, std::fmin(b.x, c.x)));
    int maxX = (int)std::ceil(std::fmax(a.x, std::fmax(b.x, c.x)));
    int minY = (int)std::floor(std::fmin(a.y, std::fmin(b.y, c.y)));
    int maxY = (int)std::ceil(std::fmax(a.y, std::fmax(b.y, c.y)));
    minX = minX < 0 ? 0 : minX;
    minY = minY < 0 ? 0 : minY;
    maxX = maxX > kWidth - 1 ? kWidth - 1 : maxX;
    maxY = maxY > kHeight - 1 ? kHeight - 1 : maxY;
    // User tile clip: 2 = draw inside the rectangle only, 3 = outside only.
    const u32 clipMode = st.clip >> 30;
    const int cx0 = (st.clip >> 24 & 63) * 32, cy0 = (st.clip >> 16 & 31) * 32;
    const int cx1 = ((st.clip >> 8 & 63) + 1) * 32 - 1, cy1 = ((st.clip & 31) + 1) * 32 - 1;
    if (clipMode == 2)
    {
        minX = minX < cx0 ? cx0 : minX;
        minY = minY < cy0 ? cy0 : minY;
        maxX = maxX > cx1 ? cx1 : maxX;
        maxY = maxY > cy1 ? cy1 : maxY;
    }
    if (minX > maxX || minY > maxY)
        return;

    const bool textured = (st.pcw >> 3) & 1;
    const bool offset = (st.pcw >> 2) & 1;
    const bool gouraud = (st.pcw >> 1) & 1;
    u32 depthMode = st.isp >> 29;
    bool zWrite = ((st.isp >> 26) & 1) == 0;
    float bias = 0.f;
    if (s_gx)
    {
        if (s_gx->geq || ((s_gx->isp || s_gx->fly) && list == 2) || (s_gx->fly && list == 4))
            depthMode = 6;
        if (list == 2)
        {
            zWrite &= !s_gx->fly && !s_gx->nzw;
            bias = s_gx->bias;
        }
    }
    const bool quantize = s_gx && s_gx->quantize;
    const float za = gxZ(a.z, bias), zb = gxZ(b.z, bias), zc = gxZ(c.z, bias);
    const u32 owner = (list << 24 | (u32)(&st - s_frame.states.data())) + 1;
    const u32 srcInstr = st.tsp >> 29, dstInstr = (st.tsp >> 26) & 7;
    const bool useAlpha = (st.tsp >> 20) & 1, ignoreTexA = (st.tsp >> 19) & 1;
    const bool flipU = (st.tsp >> 18) & 1, flipV = (st.tsp >> 17) & 1;
    const bool clampU = (st.tsp >> 16) & 1, clampV = (st.tsp >> 15) & 1;
    const u32 shading = (st.tsp >> 6) & 3;
    const float ptRef = (PvrReg(PT_ALPHA_REF_addr, u32) & 0xFF) / 255.f;
    const Texture* tex = textured ? &texture(st.tcw, st.tsp) : nullptr;

    const float inv = 1.f / area;
    for (int y = minY; y <= maxY; y++)
    {
        const float py = y + 0.5f;
        for (int x = minX; x <= maxX; x++)
        {
            if (clipMode == 3 && x >= cx0 && x <= cx1 && y >= cy0 && y <= cy1)
                continue;
            const float px = x + 0.5f;
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
            float w2 = 1.f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            float z = w0 * za + w1 * zb + w2 * zc;
            if (quantize)
                z = std::floor(z);
            const int pixel = y * kWidth + x;
            if (!depthPass(depthMode, z, s_depth[pixel]))
                continue;

            // Perspective-correct weights (z is 1/w).
            float pw0 = w0 * a.z, pw1 = w1 * b.z, pw2 = w2 * c.z;
            const float sum = pw0 + pw1 + pw2;
            if (sum > 0.f)
            {
                pw0 /= sum;
                pw1 /= sum;
                pw2 /= sum;
            }
            else
                pw0 = w0, pw1 = w1, pw2 = w2;

            float col[4], ofs[4];
            for (int i = 0; i < 4; i++)
            {
                col[i] = gouraud ? pw0 * a.col[i] + pw1 * b.col[i] + pw2 * c.col[i] : c.col[i];
                ofs[i] = gouraud ? pw0 * a.ofs[i] + pw1 * b.ofs[i] + pw2 * c.ofs[i] : c.ofs[i];
            }
            if (!useAlpha)
                col[3] = 1.f;

            float out[4];
            if (tex)
            {
                const float u = pw0 * a.u + pw1 * b.u + pw2 * c.u;
                const float v = pw0 * a.v + pw1 * b.v + pw2 * c.v;
                const u32 tx = wrap((int)std::floor(u * tex->w), tex->w, clampU, flipU);
                const u32 ty = wrap((int)std::floor(v * tex->h), tex->h, clampV, flipV);
                float t[4];
                unpack(tex->argb[ty * tex->w + tx], t);
                if (ignoreTexA)
                    t[3] = 1.f;
                switch (shading)
                {
                case 0:     // decal
                    out[0] = t[0], out[1] = t[1], out[2] = t[2], out[3] = t[3];
                    break;
                case 1:     // modulate
                    out[0] = t[0] * col[0], out[1] = t[1] * col[1], out[2] = t[2] * col[2], out[3] = t[3];
                    break;
                case 2:     // decal alpha
                    for (int i = 0; i < 3; i++)
                        out[i] = t[i] * t[3] + col[i] * (1.f - t[3]);
                    out[3] = col[3];
                    break;
                default:    // modulate alpha
                    for (int i = 0; i < 4; i++)
                        out[i] = t[i] * col[i];
                    break;
                }
            }
            else
                out[0] = col[0], out[1] = col[1], out[2] = col[2], out[3] = col[3];
            if (offset)
                for (int i = 0; i < 3; i++)
                    out[i] = clamp01(out[i] + ofs[i]);

            if (list == 4 && out[3] < ptRef)
                continue;               // punch-through alpha test

            u32& dstPixel = s_color[pixel];
            if (list == 2)
            {
                float dst[4];
                unpack(dstPixel, dst);
                float res[4];
                for (int i = 0; i < 4; i++)
                    res[i] = clamp01(out[i] * blendFactor(srcInstr, out, dst, i, true) +
                                     dst[i] * blendFactor(dstInstr, out, dst, i, false));
                out[0] = res[0], out[1] = res[1], out[2] = res[2], out[3] = res[3];
            }
            dstPixel = (u32)(clamp01(out[3]) * 255.f + 0.5f) << 24 | (u32)(clamp01(out[0]) * 255.f + 0.5f) << 16 |
                       (u32)(clamp01(out[1]) * 255.f + 0.5f) << 8 | (u32)(clamp01(out[2]) * 255.f + 0.5f);
            s_owner[pixel] = owner;
            if (zWrite)
                s_depth[pixel] = z;
        }
    }
}

void rasterise()
{
    for (u32& c : s_color)
        c = 0xFF202020;
    for (float& d : s_depth)
        d = 0.f;
    memset(s_owner, 0, sizeof(s_owner));
    s_textures.clear();
    static const u32 order[3] = {0, 4, 2};
    for (u32 list : order)
        for (const Tri& t : s_frame.tris[list])
            if (s_onlyRun < 0 || s_frame.states[t.state].run == (u32)s_onlyRun)
                drawTriangle(s_frame.vtx[t.v[0]], s_frame.vtx[t.v[1]], s_frame.vtx[t.v[2]], s_frame.states[t.state],
                             list);
}

// ---------------------------------------------------------------------------
// PNG output (stored deflate blocks: no compressor needed)
// ---------------------------------------------------------------------------
u32 crc32(const u8* data, size_t n, u32 crc = 0)
{
    static u32 table[256];
    if (!table[1])
        for (u32 i = 0; i < 256; i++)
        {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<u8>& out, u32 v)
{
    out.push_back((u8)(v >> 24));
    out.push_back((u8)(v >> 16));
    out.push_back((u8)(v >> 8));
    out.push_back((u8)v);
}

void chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data)
{
    put32(out, (u32)data.size());
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(&out[start], out.size() - start));
}

bool writePng(const char* path)
{
    std::vector<u8> raw;
    raw.reserve((kWidth * 3 + 1) * kHeight);
    for (int y = 0; y < kHeight; y++)
    {
        raw.push_back(0);
        for (int x = 0; x < kWidth; x++)
        {
            const u32 c = s_color[y * kWidth + x];
            raw.push_back((u8)(c >> 16));
            raw.push_back((u8)(c >> 8));
            raw.push_back((u8)c);
        }
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 s1 = 1, s2 = 0;
    for (u8 b : raw)
    {
        s1 = (s1 + b) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    for (size_t pos = 0; pos < raw.size();)
    {
        const size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back((u8)n);
        z.push_back((u8)(n >> 8));
        z.push_back((u8)~n);
        z.push_back((u8)(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    put32(z, s2 << 16 | s1);

    std::vector<u8> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<u8> ihdr;
    put32(ihdr, kWidth);
    put32(ihdr, kHeight);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// IDAS3_GX_DEPTH report (see GxDepth)
// ---------------------------------------------------------------------------
// How the frame's polygons cull, in draw order: what BACKFACE CULL costs in
// GX cull mode changes.
void gxCullCensus(u32 frame)
{
    u32 modes[4] = {}, changes = 0, last = 0;
    static const u32 order[3] = {0, 4, 2};
    for (u32 list : order)
    {
        u32 prevState = ~0u;
        for (const Tri& t : s_frame.tris[list])
        {
            if (t.state == prevState)
                continue;
            prevState = t.state;
            const u32 m = (s_frame.states[t.state].isp >> 27) & 3;
            modes[m]++;
            const u32 gx = m >= 2 ? m : 0;
            changes += gx != last;
            last = gx;
        }
    }
    printf("[gxdepth] frame %u cull modes (polygon headers): 0:%u 1:%u 2:%u 3:%u, GX cull changes %u\n", frame,
           modes[0], modes[1], modes[2], modes[3], changes);
}

void describeOwner(u32 owner, char* out, size_t n)
{
    if (!owner)
    {
        snprintf(out, n, "background");
        return;
    }
    const State& st = s_frame.states[(owner - 1) & 0xFFFFFF];
    snprintf(out, n, "list %u st %u dm %u cull %u zw %u blend %u/%u tex %06X", (owner - 1) >> 24,
             (owner - 1) & 0xFFFFFF, st.isp >> 29, (st.isp >> 27) & 3, ((st.isp >> 26) & 1) ^ 1, st.tsp >> 29,
             (st.tsp >> 26) & 7, (st.tcw & 0x1FFFFF) << 3);
}

// Env IDAS3_GX_DEPTH_STACK: every triangle under pixel px, in draw order.
void printPixelStack(int px)
{
    const float sx = px % kWidth + 0.5f, sy = px / kWidth + 0.5f;
    printf("[gxstack] pixel %d,%d\n", px % kWidth, px / kWidth);
    static const u32 order[3] = {0, 4, 2};
    for (u32 list : order)
        for (size_t t = 0; t < s_frame.tris[list].size(); t++)
        {
            const Tri& tr = s_frame.tris[list][t];
            const Vtx& p = s_frame.vtx[tr.v[0]];
            const Vtx& q = s_frame.vtx[tr.v[1]];
            const Vtx& r = s_frame.vtx[tr.v[2]];
            const float area = (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
            if (!std::isfinite(area) || std::fabs(area) < 1e-6f)
                continue;
            const float w0 = ((q.x - sx) * (r.y - sy) - (q.y - sy) * (r.x - sx)) / area;
            const float w1 = ((r.x - sx) * (p.y - sy) - (r.y - sy) * (p.x - sx)) / area;
            const float w2 = 1.f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            const State& st = s_frame.states[tr.state];
            printf("[gxstack]  list %u tri %zu st %u dm %u cull %u tex %06X 1/W %.9g | W (%.6f %.6f %.6f) xy (%.3f %.3f) "
                   "(%.3f %.3f) (%.3f %.3f)\n",
                   list, t, tr.state, st.isp >> 29, (st.isp >> 27) & 3, (st.tcw & 0x1FFFFF) << 3,
                   w0 * p.z + w1 * q.z + w2 * r.z, 1.f / p.z, 1.f / q.z, 1.f / r.z, p.x, p.y, q.x, q.y, r.x, r.y);
        }
}

// The snapshot just rasterised, drawn again with each configuration; s_color
// holds the plain snapshot again on return.
void gxDepthReport(u32 frame)
{
    gxDepthInit();
    if (!s_gxCount)
        return;
    static u32 plain[kWidth * kHeight];
    static std::vector<u32> results[8], owners[8];
    static std::vector<float> refDepth;
    memcpy(plain, s_color, sizeof(plain));
    gxCullCensus(frame);
    for (u32 k = 0; k < s_gxCount; k++)
    {
        s_gx = &s_gxCfg[k];
        rasterise();
        s_gx = nullptr;
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%05u_gx%u.png", s_snapDir.c_str(), frame, k);
        writePng(path);
        results[k].assign(s_color, s_color + kWidth * kHeight);
        owners[k].assign(s_owner, s_owner + kWidth * kHeight);
        if (k == 0)
        {
            refDepth.assign(s_depth, s_depth + kWidth * kHeight);  // 1/W when #0 is exact
            continue;
        }
        const u32* ref = results[s_gxCfg[k].ref].data();
        const u32* refOwner = owners[s_gxCfg[k].ref].data();
        std::unordered_map<u64, u32> pairs;    // reference's winner, this one's -> pixels
        std::unordered_map<u64, double> pairW;
        u32 differ = 0;
        for (int i = 0; i < kWidth * kHeight; i++)
        {
            const bool d = s_color[i] != ref[i];
            if (d)
            {
                const u64 key = (u64)refOwner[i] << 32 | s_owner[i];
                differ++;
                pairs[key]++;
                pairW[key] += refDepth[i] > 0.f ? 1.0 / refDepth[i] : 0.0;
            }
            s_color[i] = d ? 0xFFFF00FFu : (ref[i] >> 1 & 0x7F7F7F) | 0xFF000000u;
        }
        snprintf(path, sizeof(path), "%s/frame_%05u_diff%u.png", s_snapDir.c_str(), frame, k);
        writePng(path);
        printf("[gxdepth] frame %u #%u against #%u: %u pixels differ\n", frame, k, s_gxCfg[k].ref, differ);
        std::vector<std::pair<u32, u64>> top;
        for (const auto& p : pairs)
            top.push_back({p.second, p.first});
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t j = 0; j < top.size() && j < 8; j++)
        {
            char a[96], b[96];
            describeOwner((u32)(top[j].second >> 32), a, sizeof(a));
            describeOwner((u32)top[j].second, b, sizeof(b));
            printf("[gxdepth]   %6u px, W %.1f: ref %s | this %s\n", top[j].first, pairW[top[j].second] / top[j].first,
                   a, b);
        }
        if (k == 1 && !top.empty() && getenv("IDAS3_GX_DEPTH_STACK"))
            for (int i = 0; i < kWidth * kHeight; i++)
                if (s_color[i] == 0xFFFF00FFu && ((u64)refOwner[i] << 32 | s_owner[i]) == top[0].second)
                {
                    printPixelStack(i);
                    break;
                }
    }
    memcpy(s_color, plain, sizeof(plain));
}

} // namespace

void host_SetSnapshots(const char* dir, u32 every)
{
    s_snapDir = dir ? dir : "";
    s_snapEvery = every;
}

void host_RequestSnapshot()
{
    s_snapRequested = true;
}

// ---------------------------------------------------------------------------
// TA display-list sink
// ---------------------------------------------------------------------------
struct VertexDecoder
{
    static void StartList(u32 listType)
    {
        g_host_render.lists++;
        s_list = listType;
        event((char)('0' + listType));
    }
    static void EndList(u32) { event('e'); }

    static void StartPolyStrip() { s_stripCount = 0; }
    static void EndPolyStrip()
    {
        g_host_render.strips++;
        s_stripCount = 0;
    }

    static void fastcall AppendPolyParam0(TA_PolyParam0* p)
    {
        g_host_render.polyParams++;
        newState(p->pcw.full, p->isp.full, p->tsp.full, p->tcw.full);
    }
    static void fastcall AppendPolyParam1(TA_PolyParam1* p)
    {
        g_host_render.polyParams++;
        newState(p->pcw.full, p->isp.full, p->tsp.full, p->tcw.full);
        s_face[0] = p->FaceColorR, s_face[1] = p->FaceColorG, s_face[2] = p->FaceColorB, s_face[3] = p->FaceColorA;
    }
    static void fastcall AppendPolyParam2A(TA_PolyParam2A* p)
    {
        g_host_render.polyParams++;
        newState(p->pcw.full, p->isp.full, p->tsp.full, p->tcw.full);
    }
    static void fastcall AppendPolyParam2B(TA_PolyParam2B* p)
    {
        s_face[0] = p->FaceColorR, s_face[1] = p->FaceColorG, s_face[2] = p->FaceColorB, s_face[3] = p->FaceColorA;
        s_faceOfs[0] = p->FaceOffsetR, s_faceOfs[1] = p->FaceOffsetG, s_faceOfs[2] = p->FaceOffsetB;
        s_faceOfs[3] = p->FaceOffsetA;
    }
    static void fastcall AppendPolyParam3(TA_PolyParam3* p)
    {
        g_host_render.polyParams++;
        newState(p->pcw.full, p->isp.full, p->tsp.full, p->tcw.full);
    }
    static void fastcall AppendPolyParam4A(TA_PolyParam4A* p)
    {
        g_host_render.polyParams++;
        newState(p->pcw.full, p->isp.full, p->tsp.full, p->tcw.full);
    }
    static void fastcall AppendPolyParam4B(TA_PolyParam4B* p)
    {
        s_face[0] = p->FaceColor0R, s_face[1] = p->FaceColor0G, s_face[2] = p->FaceColor0B, s_face[3] = p->FaceColor0A;
    }

    // Non-textured
    static void AppendPolyVertex0(TA_Vertex0* p)
    {
        Vtx v = position(p->xyz);
        unpack(p->BaseCol, v.col);
        addVertex(v);
    }
    static void AppendPolyVertex1(TA_Vertex1* p)
    {
        Vtx v = position(p->xyz);
        v.col[0] = p->BaseR, v.col[1] = p->BaseG, v.col[2] = p->BaseB, v.col[3] = p->BaseA;
        addVertex(v);
    }
    static void AppendPolyVertex2(TA_Vertex2* p)
    {
        Vtx v = position(p->xyz);
        intensity(p->BaseInt, s_face, v.col);
        addVertex(v);
    }
    // Textured
    static void AppendPolyVertex3(TA_Vertex3* p)
    {
        Vtx v = position(p->xyz);
        v.u = p->u, v.v = p->v;
        unpack(p->BaseCol, v.col);
        unpack(p->OffsCol, v.ofs);
        addVertex(v);
    }
    static void AppendPolyVertex4(TA_Vertex4* p)
    {
        Vtx v = position(p->xyz);
        v.u = f16(p->u), v.v = f16(p->v);
        unpack(p->BaseCol, v.col);
        unpack(p->OffsCol, v.ofs);
        addVertex(v);
    }
    static void AppendPolyVertex5A(TA_Vertex5A* p)
    {
        s_pending = position(p->xyz);
        s_pending.u = p->u, s_pending.v = p->v;
    }
    static void AppendPolyVertex5B(TA_Vertex5B* p)
    {
        s_pending.col[0] = p->BaseR, s_pending.col[1] = p->BaseG, s_pending.col[2] = p->BaseB, s_pending.col[3] = p->BaseA;
        s_pending.ofs[0] = p->OffsR, s_pending.ofs[1] = p->OffsG, s_pending.ofs[2] = p->OffsB, s_pending.ofs[3] = p->OffsA;
        addVertex(s_pending);
    }
    static void AppendPolyVertex6A(TA_Vertex6A* p)
    {
        s_pending = position(p->xyz);
        s_pending.u = f16(p->u), s_pending.v = f16(p->v);
    }
    static void AppendPolyVertex6B(TA_Vertex6B* p) { AppendPolyVertex5B((TA_Vertex5B*)p); }
    static void AppendPolyVertex7(TA_Vertex7* p)
    {
        Vtx v = position(p->xyz);
        v.u = p->u, v.v = p->v;
        intensity(p->BaseInt, s_face, v.col);
        intensity(p->OffsInt, s_faceOfs, v.ofs);
        addVertex(v);
    }
    static void AppendPolyVertex8(TA_Vertex8* p)
    {
        Vtx v = position(p->xyz);
        v.u = f16(p->u), v.v = f16(p->v);
        intensity(p->BaseInt, s_face, v.col);
        intensity(p->OffsInt, s_faceOfs, v.ofs);
        addVertex(v);
    }
    // Two volumes: volume 0 only
    static void AppendPolyVertex9(TA_Vertex9* p)
    {
        Vtx v = position(p->xyz);
        unpack(p->BaseCol0, v.col);
        addVertex(v);
    }
    static void AppendPolyVertex10(TA_Vertex10* p)
    {
        Vtx v = position(p->xyz);
        intensity(p->BaseInt0, s_face, v.col);
        addVertex(v);
    }
    static void AppendPolyVertex11A(TA_Vertex11A* p)
    {
        s_pending = position(p->xyz);
        s_pending.u = p->u0, s_pending.v = p->v0;
        unpack(p->BaseCol0, s_pending.col);
        unpack(p->OffsCol0, s_pending.ofs);
    }
    static void AppendPolyVertex11B(TA_Vertex11B*) { addVertex(s_pending); }
    static void AppendPolyVertex12A(TA_Vertex12A* p)
    {
        s_pending = position(p->xyz);
        s_pending.u = f16(p->u0), s_pending.v = f16(p->v0);
        unpack(p->BaseCol0, s_pending.col);
        unpack(p->OffsCol0, s_pending.ofs);
    }
    static void AppendPolyVertex12B(TA_Vertex12B*) { addVertex(s_pending); }
    static void AppendPolyVertex13A(TA_Vertex13A* p)
    {
        s_pending = position(p->xyz);
        s_pending.u = p->u0, s_pending.v = p->v0;
        intensity(p->BaseInt0, s_face, s_pending.col);
        intensity(p->OffsInt0, s_faceOfs, s_pending.ofs);
    }
    static void AppendPolyVertex13B(TA_Vertex13B*) { addVertex(s_pending); }
    static void AppendPolyVertex14A(TA_Vertex14A* p)
    {
        s_pending = position(p->xyz);
        s_pending.u = f16(p->u0), s_pending.v = f16(p->v0);
        intensity(p->BaseInt0, s_face, s_pending.col);
        intensity(p->OffsInt0, s_faceOfs, s_pending.ofs);
    }
    static void AppendPolyVertex14B(TA_Vertex14B*) { addVertex(s_pending); }

    // Sprites: A B C given, D completes the parallelogram.
    static void AppendSpriteParam(TA_SpriteParam* p)
    {
        g_host_render.sprites++;
        s_orderKind = 'S';
        newState(p->pcw.full, p->isp.full, p->tsp.full, p->tcw.full);
        s_orderKind = 'P';
        s_spriteBase = p->BaseCol;
        s_spriteOfs = p->OffsCol;
    }
    static void AppendSpriteVertexA(TA_Sprite1A* p)
    {
        s_spriteA[0] = p->x0, s_spriteA[1] = p->y0, s_spriteA[2] = p->z0;
        s_spriteA[3] = p->x1, s_spriteA[4] = p->y1, s_spriteA[5] = p->z1;
        s_spriteA[6] = p->x2;
    }
    static void AppendSpriteVertexB(TA_Sprite1B* p)
    {
        Vtx q[4] = {};
        const float xyz[4][3] = {
            {s_spriteA[0], s_spriteA[1], s_spriteA[2]},
            {s_spriteA[3], s_spriteA[4], s_spriteA[5]},
            {s_spriteA[6], p->y2, p->z2},
            {p->x3, p->y3, s_spriteA[2] + p->z2 - s_spriteA[5]},
        };
        const float uv[3][2] = {{f16(p->u0), f16(p->v0)}, {f16(p->u1), f16(p->v1)}, {f16(p->u2), f16(p->v2)}};
        for (int i = 0; i < 4; i++)
        {
            q[i].x = xyz[i][0], q[i].y = xyz[i][1], q[i].z = xyz[i][2];
            unpack(s_spriteBase, q[i].col);
            unpack(s_spriteOfs, q[i].ofs);
            if (i < 3)
                q[i].u = uv[i][0], q[i].v = uv[i][1];
        }
        q[3].u = q[0].u + q[2].u - q[1].u;
        q[3].v = q[0].v + q[2].v - q[1].v;
        // A B C then A C D (two independent triangles)
        s_stripCount = 0;
        addVertex(q[0], false);
        addVertex(q[1], false);
        addVertex(q[2], false);
        s_stripCount = 0;
        addVertex(q[0], false);
        addVertex(q[2], false);
        addVertex(q[3], false);
        s_stripCount = 0;
    }

    static void AppendModVolParam(TA_ModVolParam*) {}
    static void StartModVol(TA_ModVolParam*) { g_host_render.modVols++; }
    static void ModVolStripEnd() {}
    static void AppendModVolVertexA(TA_ModVolA*) {}
    static void AppendModVolVertexB(TA_ModVolB*) {}

    static void SetTileClip(u32 xmin, u32 ymin, u32 xmax, u32 ymax)
    {
        s_tileClipRect = xmin << 24 | ymin << 16 | xmax << 8 | ymax;
        s_tileClip = (s_tileClip & 0xC0000000u) | s_tileClipRect;
    }
    static void TileClipMode(u32 mode) { s_tileClip = mode << 30 | s_tileClipRect; }

    static void ListCont() {}
    static void ListInit()
    {
        event('I');
        s_frame.clear();
        s_order.clear();
    }
    static void SoftReset()
    {
        // Resets the TA's input state only: what it already wrote to the
        // parameter buffers (our capture) is still rendered (games reset the
        // TA between finishing the lists and STARTRENDER).
        event('S');
        s_stripCount = 0;
    }
};

FifoSplitter<VertexDecoder> TileAccel;

extern "C" void rend_SetTileClipMode(u32 mode)
{
    VertexDecoder::TileClipMode(mode);
}

// NAOMI 2 GPU T&L stand-in (naomi_elan_gx.cpp, runner --gpu-tnl): nothing is
// drawn, but the TA list the polygon belongs to is opened like gxRend does,
// so list ends raise the right interrupts.
extern "C" bool rend_AddElanItem(u32 pcw, u32, u32, u32, u32, float nearW, float farW)
{
    if (TileAccel.CurrentList == ListType_None)
        TileAccel.ta_list_start(pcw >> 24 & 7);
    orderHeader('E', pcw >> 24 & 7, 0, farW > 0.f ? 1.f / farW : 0.f, nearW > 0.f ? 1.f / nearW : 0.f);
    HostRenderStats& h = g_host_render;
    if (h.frameFar == 0.f || nearW < h.frameNear)
        h.frameNear = nearW;
    if (farW > h.frameFar)
        h.frameFar = farW;
    return true;
}

// Once per render: what the frame's depth range would have been.
static void noteGpuDepthRange()
{
    HostRenderStats& h = g_host_render;
    if (h.frameFar == 0.f)
        return;
    if (h.depthFrames == 0 || h.frameNear < h.nearMin)
        h.nearMin = h.frameNear;
    if (h.depthFrames == 0 || h.frameNear > h.nearMax)
        h.nearMax = h.frameNear;
    if (h.frameFar > h.farMax)
        h.farMax = h.frameFar;
    h.depthFrames++;
    h.nearPinned += h.frameNear <= 0.0101f;
    h.nearUnder1 += h.frameNear < 1.f;
    h.frameNear = h.frameFar = 0.f;
}

// ---------------------------------------------------------------------------
// rend_* entry points (see plugs/drkPvr/gxRend.h)
// ---------------------------------------------------------------------------
bool InitRenderer() { return true; }
void TermRenderer() {}
void ResetRenderer(bool) {}
bool ThreadStart() { return true; }
void ThreadEnd() {}

void VBlank()
{
    FrameCount++;
    g_host_render.vblanks++;
    host_on_vblank();
}

// Frame skipping, with gxRend.cpp's contract: the NAOMI 2 ELAN asks for the
// verdict of the frame being built (so a dropped frame also skips its T&L) and
// StartRender consumes the same verdict. Fixed modes only (FRAMESKIP 1 = 1 of
// 2, 2 = 2 of 3); the AUTO modes need the Wii's frame pacer.
extern "C" int get_frameskip_preset();
static bool s_skipValid = false, s_skip = false;
static u32 s_skipCounter = 0;

static bool decideSkip()
{
    const int mode = get_frameskip_preset();
    if (mode != 1 && mode != 2)
        return false;
    s_skipCounter = (s_skipCounter + 1) % (mode + 1);
    return s_skipCounter != 0;
}

// 2 PLAYERS (naomi_2p.cpp): a pair of views takes one frameskip verdict, as
// on the Wii (gxRend.cpp).
extern "C" int naomi_2pHalf();
extern "C" int naomi_2pNextHalf();
extern "C" bool naomi_2pSkip(int half, bool (*decide)());

extern "C" bool rend_NextFrameSkipped()
{
    if (!s_skipValid)
    {
        s_skip = naomi_2pSkip(naomi_2pNextHalf(), decideSkip);
        s_skipValid = true;
    }
    return s_skip;
}

extern "C" bool rend_FrameKnownSkipped()
{
    return s_skipValid && s_skip;
}

extern "C" {
u32 g_rend_dropped_renders = 0;              // renders the frameskip dropped (profiler)
}

static bool takeSkipVerdict()
{
    bool skip;
    if (s_skipValid)
    {
        s_skipValid = false;
        skip = s_skip;
    }
    else
        skip = naomi_2pSkip(naomi_2pHalf(), decideSkip);
    g_rend_dropped_renders += skip;
    return skip;
}

// IDAS3_RENDER_TRACE=FROM:TO prints every RENDER_START between those vblanks
// with the framebuffer target, to see how a frame is split into passes.
static void traceRender()
{
    static int s_from = -1, s_to = -1;
    if (s_from == -1)
    {
        s_from = s_to = -2;
        if (const char* e = getenv("IDAS3_RENDER_TRACE"))
            sscanf(e, "%d:%d", &s_from, &s_to);
    }
    const int f = (int)g_host_render.vblanks;
    if (f < s_from || f > s_to)
        return;
    printf("[rtrace] vbl %d r %u sof1 %08X sof2 %08X wctrl %08X stride %u xclip %u-%u yclip %u-%u region %08X "
           "param %08X rsof1 %08X vtx %u\n",
           f, g_host_render.renders, FB_W_SOF1, FB_W_SOF2, FB_W_CTRL, FB_W_LINESTRIDE, FB_X_CLIP.min, FB_X_CLIP.max,
           FB_Y_CLIP.min, FB_Y_CLIP.max, REGION_BASE, PARAM_BASE, FB_R_SOF1,
           g_host_render.vertices - g_host_render.verticesAtLastRender);
}

// 2 PLAYERS (naomi_2p.cpp): each render is one car's view, squeezed into its
// half of the picture as on the Wii (gxRend split2p_*). A snapshot due during
// split screen rasterises the render it falls on and the next one, the other
// car's, and writes the two side by side.
extern "C" void naomi_2pDrawn(int half);
static std::vector<u32> s_split;   // the side-by-side picture
static u32 s_splitHalves;          // halves put in since the snapshot was due (bit per half)
static u32 s_splitFrame;           // vblank of the snapshot

static void splitPut(int half)
{
    if (s_split.empty())
        s_split.assign(kWidth * kHeight, 0xFF202020);
    const int w = kWidth / 2;
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < w; x++)
        {
            // Two source pixels per destination pixel, averaged.
            const u32 a = s_color[y * kWidth + 2 * x], b = s_color[y * kWidth + 2 * x + 1];
            const u32 c = ((a >> 1) & 0x7F7F7F7F) + ((b >> 1) & 0x7F7F7F7F);
            s_split[y * kWidth + half * w + x] = c | 0xFF000000;
        }
    s_splitHalves |= 1u << half;
}

static void splitSnapshotRender(bool due)
{
    const int half = naomi_2pHalf();
    if (half < 0)
    {
        s_splitHalves = 0;
        return;
    }
    naomi_2pDrawn(half);
    if (!due && !s_splitHalves)
        return;
    if (!s_splitHalves)
        s_splitFrame = g_host_render.vblanks;
    rasterise();
    splitPut(half);
    if (s_splitHalves != 3)
        return;
    s_splitHalves = 0;
    memcpy(s_color, s_split.data(), sizeof(u32) * kWidth * kHeight);
    char path[512];
    snprintf(path, sizeof(path), "%s/split_%05u.png", s_snapDir.c_str(), s_splitFrame);
    if (writePng(path))
        printf("[snap] %s (2 PLAYERS, views of frames %u and %u)\n", path, s_splitFrame, g_host_render.vblanks);
}

void StartRender()
{
    event('R');
    traceRender();
    g_host_render.renders++;
    {
        // env IDAS3_TIMELINE (run_main.cpp): the renders among the accesses.
        static u32 tlFrom = 1, tlTo = 0;
        static bool tlRead = false;
        if (!tlRead)
        {
            tlRead = true;
            if (const char* tl = getenv("IDAS3_TIMELINE"))
                sscanf(tl, "%u:%u", &tlFrom, &tlTo);
        }
        extern u32 g_elan_exec_seq;
        if (g_host_render.vblanks >= tlFrom && g_host_render.vblanks <= tlTo)
            printf("[tl] f%u RENDER %u, half %d, elan %u\n", g_host_render.vblanks, g_host_render.renders, naomi_2pHalf(),
                   g_elan_exec_seq);
    }
    noteGpuDepthRange();
    if (orderOn())
    {
        static int s_from = -1, s_to = -1;
        if (s_from == -1)
            sscanf(getenv("IDAS3_STRIP_ORDER"), "%d:%d", &s_from, &s_to);
        const int f = (int)g_host_render.vblanks;
        if (f >= s_from && f <= s_to)
        {
            printf("[order] vbl %d render %u%s: %u runs\n", f, g_host_render.renders,
                   rend_FrameKnownSkipped() ? " (dropped)" : "", (u32)s_order.size());
            for (const OrderRun& r : s_order)
                printf("[order]   L%u %c %4u headers %5u vtx  x %4.0f..%4.0f y %4.0f..%4.0f  1/W %.4g..%.4g  tex %08X\n",
                       r.list, r.kind, r.headers, r.vtx, r.x0, r.x1, r.y0, r.y1, r.z0, r.z1, r.tcw);
        }
    }
    if (takeSkipVerdict())
    {
        // Same as gxRend: render-done still fires on schedule, nothing is drawn.
        g_host_render.skipped++;
        g_host_render.verticesAtLastRender = g_host_render.vertices;
        render_end_pending_cycles = 50000;
        s_frame.clear();
        return;
    }
    const u32 vtx = g_host_render.vertices - g_host_render.verticesAtLastRender;
    g_host_render.verticesAtLastRender = g_host_render.vertices;
    VertexCount += vtx;
    elan_gx_census_render();
    // Same render-done timing as gxRend.cpp's default (non RENDER_DELAY) path.
    render_end_pending_cycles = (s32)(vtx * 15);
    if (render_end_pending_cycles < 50000)
        render_end_pending_cycles = 50000;

    // env IDAS3_SNAP_FROM=F: periodic snapshots only from frame F on.
    static const u32 snapFrom = getenv("IDAS3_SNAP_FROM") ? (u32)strtoul(getenv("IDAS3_SNAP_FROM"), nullptr, 0) : 0;
    const bool periodic = s_snapEvery && g_host_render.vblanks >= snapFrom && g_host_render.vblanks - s_lastSnap >= s_snapEvery;
    if (naomi_2pHalf() >= 0)
    {
        if (periodic)
            s_lastSnap = g_host_render.vblanks;
        if (!s_snapDir.empty())
            splitSnapshotRender(periodic || s_snapRequested);
        else
            naomi_2pDrawn(naomi_2pHalf());
        s_snapRequested = false;
        return;
    }
    if (!s_snapDir.empty() && (periodic || s_snapRequested))
    {
        s_lastSnap = g_host_render.vblanks;
        s_snapRequested = false;
        rasterise();
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%05u.png", s_snapDir.c_str(), g_host_render.vblanks);
        gxDepthReport(g_host_render.vblanks);
        if (writePng(path))
            printf("[snap] %s (%u tris op, %u pt, %u tr; %u states, %u vtx | totals: lists %u sprites %u params %u)\n",
                   path, (u32)s_frame.tris[0].size(), (u32)s_frame.tris[4].size(), (u32)s_frame.tris[2].size(),
                   (u32)s_frame.states.size(), (u32)s_frame.vtx.size(), g_host_render.lists, g_host_render.sprites,
                   g_host_render.polyParams);
        if (orderOn())
        {
            // IDAS3_STRIP_ORDER: every run with vertices drawn alone.
            for (u32 r = 0; r < s_order.size(); r++)
            {
                if (s_order[r].vtx < 3)
                    continue;
                s_onlyRun = (int)r;
                rasterise();
                // The run's nearest drawn pixel and how many pixels it covers.
                float zMax = 0.f;
                u32 covered = 0, nearPx = 0;
                for (int i = 0; i < kWidth * kHeight; i++)
                    if (s_depth[i] > 0.f)
                    {
                        covered++;
                        nearPx += s_depth[i] > 4.f;   // nearer than W = 0.25
                        zMax = s_depth[i] > zMax ? s_depth[i] : zMax;
                    }
                printf("[order]   run %u: %u pixels, nearest 1/W %.4g, %u pixels nearer than W 0.25\n", r, covered,
                       zMax, nearPx);
                snprintf(path, sizeof(path), "%s/frame_%05u_run%02u_L%u%c.png", s_snapDir.c_str(),
                         g_host_render.vblanks, r, s_order[r].list, s_order[r].kind);
                writePng(path);
            }
            s_onlyRun = -1;
        }
        char ev[65];
        for (u32 i = 0; i < 64; i++)
            ev[i] = s_events[(s_eventPos + i) & 63] ? s_events[(s_eventPos + i) & 63] : '.';
        ev[64] = 0;
        printf("[snap] events: %s\n", ev);
    }
}

void EndRender() {}
void ListCont() { TileAccel.ListCont(); }
void ListInit() { TileAccel.ListInit(); }
void SoftReset() { TileAccel.SoftReset(); }
void SetFpsText(char*) {}
void SetFbScale(float, float) {}
void VramLockedWrite(vram_block*) {}

#endif // HOST_OS != OS_WII
