// GPU (GX) transform & lighting for NAOMI 2 ELAN polygon lists - see
// naomi_elan_gx.h. Wii only; the host build keeps the CPU path.
#include "naomi_elan_gx.h"
#include "naomi_elan.h"
#include "plugs/drkPvr/frame_prof.h"

int g_elan_gx_preset = 0;
int g_elan_fetch_preset = 1;
int g_elan_dl_preset = 0;
int g_elan_colour_fetch_preset = 0;
int g_elan_point_preset = 0;
int g_elan_box_preset = 0;
int g_headlight_preset = 0;
int g_light_sum_preset = 1;
extern "C" {
int g_draw_memo_preset = 0;     // DRAW MEMO (launch page)
}

#include <math.h>

// ---- HEADLIGHTS = SMOOTH: the projected cone (Wii draw and Windows check) ---
float elan_beam_tan(const ElanGxBeam& b)
{
    // The cone term's zero, c0 = -p / q, plus a 15% margin: the texture's
    // outer ring is then dark, which GX's clamp repeats outside it.
    const float c0 = b.coneQ > 0.f ? -b.coneP / b.coneQ : 0.f;
    const float c = c0 > 0.05f ? (c0 < 0.9999f ? c0 : 0.9999f) : 0.05f;
    return sqrtf(1.f - c * c) / c * 1.15f;
}

void elan_beam_texture(const ElanGxBeam& b, u8* texels)
{
    // Texel i's centre is s = (i + 0.5) / N, at x / z = (s - 0.5) * 2 tan.
    const float t = elan_beam_tan(b), step = 2.f * t / kElanBeamTex;
    for (u32 j = 0; j < kElanBeamTex; j++)
        for (u32 i = 0; i < kElanBeamTex; i++)
        {
            const float x = ((float)i + 0.5f) * step - t, y = ((float)j + 0.5f) * step - t;
            const float c = 1.f / sqrtf(1.f + x * x + y * y);
            float v = b.coneP + b.coneQ * c;
            v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
            const bool edge = i == 0 || j == 0 || i == kElanBeamTex - 1 || j == kElanBeamTex - 1;
            texels[j * kElanBeamTex + i] = edge ? 0 : (u8)(v * 255.f + 0.5f);
        }
}

void elan_beam_texmtx(const ElanGxXform& x, const ElanGxBeam& b, float m[3][4])
{
    // Two unit vectors across the axis (the cone is round: which two is free).
    const float* a = b.axis;
    float h[3] = {0.f, 1.f, 0.f};
    if (fabsf(a[1]) > 0.9f)
    {
        h[0] = 1.f;
        h[1] = 0.f;
    }
    float u[3] = {h[1] * a[2] - h[2] * a[1], h[2] * a[0] - h[0] * a[2], h[0] * a[1] - h[1] * a[0]};
    const float ul = 1.f / sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& c : u)
        c *= ul;
    const float v[3] = {a[1] * u[2] - a[2] * u[1], a[2] * u[0] - a[0] * u[2], a[0] * u[1] - a[1] * u[0]};
    // Lighting space to (s t q): s = 0.5 + k x / z with x, z the point's
    // offsets from the lamp across and along the axis, k = 0.5 / tan.
    const float k = 0.5f / elan_beam_tan(b);
    float B[3][4];
    for (int c = 0; c < 3; c++)
    {
        B[0][c] = k * u[c] + 0.5f * a[c];
        B[1][c] = k * v[c] + 0.5f * a[c];
        B[2][c] = a[c];
    }
    for (int r = 0; r < 3; r++)
        B[r][3] = -(B[r][0] * b.pos[0] + B[r][1] * b.pos[1] + B[r][2] * b.pos[2]);
    // Times the position matrix: model space to lighting space (-X, Y, -Z),
    // as the GPU path's drawItem loads it.
    const float* t = x.tm;
    const float mv[3][4] = {{-t[0], -t[3], -t[6], -t[9]}, {t[1], t[4], t[7], t[10]}, {-t[2], -t[5], -t[8], -t[11]}};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            m[r][c] = B[r][0] * mv[0][c] + B[r][1] * mv[1][c] + B[r][2] * mv[2][c] + (c == 3 ? B[r][3] : 0.f);
}

void elan_beam_mask_texture(u8* texels)
{
    // Black left half, white right half: sampled bilinear, 0 up to the centre
    // of the last black texel and 1 from the first white one.
    for (u32 j = 0; j < kElanBeamMaskH; j++)
        for (u32 i = 0; i < kElanBeamMaskW; i++)
            texels[j * kElanBeamMaskW + i] = i < kElanBeamMaskW / 2 ? 0 : 255;
}

void elan_beam_maskmtx(const ElanGxXform& x, const ElanGxBeam& b, float m[2][4])
{
    // s = (W/2 - 0.5 + z / mask) / W, z the distance along the axis in front of
    // the lamp: the black texel's centre at z = 0, the white one's at z = mask.
    // Behind the lamp s goes below 0 and clamps black (|s| stays small: GX
    // texture coordinates have a limited range). t: the middle row.
    const float w = (float)kElanBeamMaskW, k = 1.f / (b.mask * w);
    const float B[4] = {k * b.axis[0], k * b.axis[1], k * b.axis[2],
                        (w * 0.5f - 0.5f) / w - k * (b.axis[0] * b.pos[0] + b.axis[1] * b.pos[1] + b.axis[2] * b.pos[2])};
    const float* t = x.tm;
    const float mv[3][4] = {{-t[0], -t[3], -t[6], -t[9]}, {t[1], t[4], t[7], t[10]}, {-t[2], -t[5], -t[8], -t[11]}};
    for (int c = 0; c < 4; c++)
    {
        m[0][c] = B[0] * mv[0][c] + B[1] * mv[1][c] + B[2] * mv[2][c] + (c == 3 ? B[3] : 0.f);
        m[1][c] = c == 3 ? 0.5f : 0.f;
    }
}

// ---- Strip structure of an ICH list (Wii draw and Windows census) ----------
// Vertex records: word 0 header (bit 31 ends the strip, bits 29-30 == 2 make
// a fan vertex), words 1-3 the position; unused slots hold -inf positions.
namespace elan_strips {

inline bool nonFinite(u32 w)
{
    return (w & 0x7F800000u) == 0x7F800000u;
}

inline bool placeholder(const u32* v)
{
    return nonFinite(v[1]) || nonFinite(v[2]) || nonFinite(v[3]);
}

// Vertices [first, end) as one strip of indices, strips and fans joined by
// degenerate triangles (the CPU path's strip assembly rules). Returns the
// index count, at most cap.
inline u32 joinedIndices(const u32* vtx, u32 words, u32 first, u32 end, u16* out, u32 cap)
{
    u32 m = 0;
    bool start = true;
    u16 fanCenter = 0, fanLast = 0;
    auto push = [&](u16 i) {
        if (m < cap)
            out[m++] = i;
    };
    for (u32 i = first; i < end; i++)
    {
        const u32* v = vtx + i * words;
        if (placeholder(v))
        {
            start = true;                       // placeholder vertex: restart after it
            continue;
        }
        const u32 header = v[0];
        if (start)
        {
            fanCenter = (u16)i;
            if (m)
            {
                const u16 last = out[m - 1];
                push(last);
                push((u16)i);
                if (m & 1)
                    push((u16)i);
            }
            start = false;
        }
        else if ((header >> 29 & 3) == 2)       // fan vertex
        {
            push(fanLast);
            push(fanCenter);
            if (m & 1)
                push(fanCenter);
            push(fanCenter);
            push(fanLast);
        }
        push((u16)i);
        fanLast = (u16)i;
        if (header >> 31)
            start = true;
    }
    return m;
}

// The strip starting at vertex `first` (not a placeholder) ends before the
// returned index: after a vertex with the end bit, or before a placeholder.
// `fan`: it has fan vertices (then only joinedIndices draws it right).
inline u32 stripEnd(const u32* vtx, u32 words, u32 first, u32 count, bool& fan)
{
    fan = false;
    const u32* v = vtx + first * words;
    for (u32 j = first + 1;; j++)
    {
        // This walk is the first touch of the list's records: ask for the
        // cache lines ahead (dcbt; real hardware stalls on every miss,
        // Dolphin doesn't model them). 8 vertices: 4 was only ~30
        // instructions ahead of a miss that costs a few hundred cycles. A
        // record longer than a line (float normals) gets its second line too.
#if defined(__GNUC__)
        __builtin_prefetch(v + 8 * words);
        if (words > 8)
            __builtin_prefetch(v + 8 * words + 8);
#endif
        if (v[0] >> 31 || j >= count)
            return j;
        v += words;
        if (placeholder(v))
            return j;
        if ((v[0] >> 29 & 3) == 2)
            fan = true;
    }
}

} // namespace elan_strips

#if !defined(GEKKO)

// No GX here. The Windows runner's --gpu-tnl still takes the lists (the
// renderer opens their TA list and drops them), so its ELAN timers show
// what is left for the CPU when the Wii's GPU path is on.
bool elan_gx_enabled()
{
    return g_elan_gx_preset != 0;
}

#include <algorithm>
#include <vector>

#include "host/host_hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Census of the strips: how many indices the joined strip has against the
// vertex count, and a check that drawing every strip on its own (the Wii's
// draw) yields exactly the joined strip's triangles.
namespace {

struct Tri
{
    u16 a, b, c;
    bool operator<(const Tri& o) const
    {
        return a != o.a ? a < o.a : b != o.b ? b < o.b : c < o.c;
    }
    bool operator==(const Tri& o) const { return a == o.a && b == o.b && c == o.c; }
};

// The visible triangles of a strip of indices, rotated to start at their
// smallest index (orientation kept).
void stripTriangles(const u16* s, u32 n, std::vector<Tri>& out)
{
    for (u32 k = 0; k + 2 < n; k++)
    {
        u16 a = s[k], b = s[k + 1], c = s[k + 2];
        if (k & 1)
            std::swap(a, b);
        if (a == b || b == c || a == c)
            continue;
        while (a > b || a > c)
        {
            const u16 t = a;
            a = b;
            b = c;
            c = t;
        }
        out.push_back({a, b, c});
    }
}

} // namespace

static void stripCensus(const ElanGxRecord& r)
{
    using namespace elan_strips;
    static std::vector<u16> joined(65536), run(65536);
    static std::vector<Tri> t1, t2;
    NaomiElanStats& st = elan_Stats();
    const u32 words = r.vsize / 4;
    const u32 m = joinedIndices(r.vtx, words, 0, r.count, joined.data(), 65535);
    t1.clear();
    t2.clear();
    stripTriangles(joined.data(), m, t1);
    for (u32 i = 0; i < r.count;)
    {
        if (placeholder(r.vtx + i * words))
        {
            i++;
            continue;
        }
        bool fan;
        const u32 j = stripEnd(r.vtx, words, i, r.count, fan);
        if (!fan)
        {
            for (u32 k = 0; k < j - i; k++)
                run[k] = (u16)(i + k);
            stripTriangles(run.data(), j - i, t2);
        }
        else
            stripTriangles(run.data(), joinedIndices(r.vtx, words, i, j, run.data(), 65535), t2);
        i = j;
    }
    std::sort(t1.begin(), t1.end());
    std::sort(t2.begin(), t2.end());
    st.gpuTriangles += (u32)t1.size();
    if (t1 != t2)
        st.gpuTriangleMismatches++;
}

static void stripCensusCounts(const ElanGxRecord& r)
{
    NaomiElanStats& st = elan_Stats();
    const u32 words = r.vsize / 4;
    u32 m = 0;
    bool start = true, fanInStrip = false;
    for (u32 i = 0; i < r.count; i++)
    {
        const u32* v = r.vtx + i * words;
        auto inf = [](u32 w) { return (w & 0x7F800000u) == 0x7F800000u; };
        if (inf(v[1]) || inf(v[2]) || inf(v[3]))
        {
            st.gpuPlaceholders++;
            start = true;
            continue;
        }
        const u32 header = v[0];
        if (start)
        {
            if (fanInStrip)
                st.gpuStripsWithFans++;
            fanInStrip = false;
            st.gpuStrips++;
            if (m)
            {
                m += 2;
                if (m & 1)
                    m++;
            }
            start = false;
        }
        else if ((header >> 29 & 3) == 2)
        {
            st.gpuFanVertices++;
            fanInStrip = true;
            m += 2;
            if (m & 1)
                m++;
            m += 2;
        }
        m++;
        if (header >> 31)
            start = true;
    }
    if (fanInStrip)
        st.gpuStripsWithFans++;
    st.gpuIndices += m;
}

// ---- GPU-fetch check ---------------------------------------------------------
// The Wii's fetch programs (naomi_elan_fetch.inc) built here every drawn
// render for the lists the Wii would fetch, over a simulated layout of
// physical frames: pages mostly in order, every third one elsewhere, a few
// moved now and then (the pager). Each program is drawn into triangles read
// through that layout and compared with the CPU draw of its list. Pages
// written come from the host's write watch on ELAN RAM, taken before a page
// is checked whenever the ELAN has run since (the game writes between its
// command runs): what the Wii's page change bits would show at that check.
namespace {

constexpr u32 kSimFrames = 1u << 16;            // 256 MB of simulated physical memory
constexpr u32 kSimRingPa = 0x40000000u;         // the program ring's simulated physical address
constexpr u32 kRingGuard = 256;                 // words past the ring checked after each build

std::vector<u32> s_simFrame;                    // per ELAN page: its frame, 0 = none yet
std::vector<int> s_frameOwner;                  // per frame: its ELAN page, -1 = free
std::vector<u32> s_pageWritten, s_pageChecked;  // per ELAN page: take a write was seen at, take of the last check
u32 s_take = 1;                                 // write-watch takes so far (orders writes and checks)
u32 s_takeExec = ~0u;                           // g_elan_exec_seq at the last take
u32 s_simNext = 16, s_rng = 0x1234567u;
u32 s_scatterPct = 33;                          // pages put in a random frame (IDAS3_FETCH_SCATTER)

u32 rnd()
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

// A new frame for ELAN page `page`: mostly the next free one, every third
// time (s_scatterPct) a random one, so runs of adjacent frames break.
void simAssign(u32 page)
{
    u32 f = s_simNext;
    if (rnd() % 100 < s_scatterPct || f >= kSimFrames)
        f = 16 + rnd() % (kSimFrames - 16);
    while (s_frameOwner[f] >= 0)
        f = f + 1 < kSimFrames ? f + 1 : 16;
    s_simNext = f + 1;
    if (s_simFrame[page])
        s_frameOwner[s_simFrame[page]] = -1;
    s_simFrame[page] = f;
    s_frameOwner[f] = (int)page;
}

u32 fetchPagePhys(u32 page, int* written);
u32 fetchRingPhys(const void* p);

void fetchFlush(const void*, u32)
{
}

// DISPLAY LISTS check: the entries called this render since the GPU was last
// waited for (offset, bytes), which no entry written may overlap.
std::vector<std::pair<u32, u32>> s_dlQueued;
u32 s_dlHazards = 0;

void fetchDlSync()
{
    s_dlQueued.clear();
}

#include "naomi_elan_fetch.inc"

const u8* s_ram = nullptr;

// The pages written since the last take.
void takeWrites()
{
    static u32 pages[8192];
    const u32 n = host_GuestTakeWritten(s_ram, (u32)s_pageWritten.size() << 12, pages, 8192);
    s_take++;
    for (u32 k = 0; k < n; k++)
        s_pageWritten[pages[k]] = s_take;
    s_takeExec = g_elan_exec_seq;
}

u32 fetchPagePhys(u32 page, int* written)
{
    if (s_takeExec != g_elan_exec_seq)
        takeWrites();
    bool moved = false;
    if (!s_simFrame[page])
    {
        simAssign(page);
        moved = true;
    }
    *written = moved || s_pageWritten[page] > s_pageChecked[page];
    s_pageChecked[page] = s_take;
    return s_simFrame[page] << 12;
}

u32 fetchRingPhys(const void* p)
{
    return kSimRingPa + (u32)((const u8*)p - (const u8*)s_prog);
}

u8 physByte(u32 pa)
{
    if (pa >= kSimRingPa)
    {
        const u32 o = pa - kSimRingPa;
        return o < s_progWords * 2 ? ((const u8*)s_prog)[o] : 0xCD;
    }
    const u32 f = pa >> 12;
    const int page = f < kSimFrames ? s_frameOwner[f] : -1;
    return page < 0 ? 0xCD : s_ram[(u32)page << 12 | (pa & 0xFFF)];
}

// A vertex as the GPU would read it: its record's bytes, hashed.
u64 physRecord(u32 pa, u32 vs)
{
    u64 h = 14695981039346656037ull;
    for (u32 k = 0; k < vs; k++)
        h = (h ^ physByte(pa + k)) * 1099511628211ull;
    return h;
}

u64 guestRecord(const u32* rec, u32 vs)
{
    const u8* b = (const u8*)rec;
    u64 h = 14695981039346656037ull;
    for (u32 k = 0; k < vs; k++)
        h = (h ^ b[k]) * 1099511628211ull;
    return h;
}

struct Tri3
{
    u64 a, b, c;
    bool operator<(const Tri3& o) const
    {
        return a != o.a ? a < o.a : b != o.b ? b < o.b : c < o.c;
    }
    bool operator==(const Tri3& o) const { return a == o.a && b == o.b && c == o.c; }
};

// A GX strip's visible triangles, rotated to start at their smallest vertex
// (orientation kept).
void addStrip(const std::vector<u64>& v, std::vector<Tri3>& out)
{
    for (size_t k = 0; k + 2 < v.size(); k++)
    {
        u64 a = v[k], b = v[k + 1], c = v[k + 2];
        if (k & 1)
            std::swap(a, b);
        if (a == b || b == c || a == c)
            continue;
        while (a > b || a > c)
        {
            const u64 t = a;
            a = b;
            b = c;
            c = t;
        }
        out.push_back({a, b, c});
    }
}

// The CPU draw of vertices [first, end) (the Wii's drawStrips).
void cpuTriangles(const u32* vtx, u32 vs, u32 first, u32 end, std::vector<Tri3>& out)
{
    using namespace elan_strips;
    static std::vector<u16> idx(65536);
    static std::vector<u64> v;
    const u32 words = vs / 4;
    for (u32 i = first; i < end;)
    {
        if (placeholder(vtx + i * words))
        {
            i++;
            continue;
        }
        bool fan;
        const u32 j = stripEnd(vtx, words, i, end, fan);
        v.clear();
        if (fan)
        {
            const u32 m = joinedIndices(vtx, words, i, j, idx.data(), 65535);
            if (m >= 3)
                for (u32 k = 0; k < m; k++)
                    v.push_back(guestRecord(vtx + idx[k] * words, vs));
        }
        else if (j - i >= 3)
            for (u32 k = i; k < j; k++)
                v.push_back(guestRecord(vtx + k * words, vs));
        addStrip(v, out);
        i = j;
    }
}

// runProgram's other side: the triangles the GPU would draw.
struct CheckSink
{
    const u32* vtx;
    u32 vs, basePa;
    std::vector<Tri3>* tris;
    std::vector<u64> v;

    u64 at(u32 k) const { return physRecord(basePa + k * vs, vs); }
    void base(u32 pa, bool) { basePa = pa; }
    void run(u32 s, u32 n, bool odd)
    {
        v.clear();
        if (odd)
            v.push_back(at(s));
        for (u32 k = s; k < s + n; k++)
            v.push_back(at(k));
        addStrip(v, *tris);
    }
    void list(const u16* p, u32 n)
    {
        v.clear();
        for (u32 k = 0; k < n; k++)
            v.push_back(at(p[k]));
        addStrip(v, *tris);
    }
    void direct(u32 i, u32 j) { cpuTriangles(vtx, vs, i, j, *tris); }
};

// The triangles a display-list entry draws, its call bytes read as the GPU
// would (strips on GX_VTXFMT2 and NOPs only, every attribute's index the
// same, calls on 32 bytes inside the ring). False: bytes the GPU must not see.
bool dlTriangles(const u8* entry, const u32* vtx, u32 vs, u32 attrs, std::vector<Tri3>& out)
{
    const u32* h = (const u32*)entry;
    const u32* s = h + kDlHeader / 4;
    const u32 at = (u32)(entry - s_dl), size = h[3];
    if ((at & 31) || at + size > s_dlBytes || ((kDlHeader + 8 * h[0] + 31) & ~31u) > size)
        return false;
    u32 basePa = 0;
    std::vector<u64> v;
    for (u32 k = 0; k < h[0]; k++, s += 2)
    {
        const u32 a = s[0], b = s[1];
        if (a & kDlCall)
        {
            const u32 off = a & kDlArg;
            if ((off & 31) || (b & 31) || !b || off + b > size)
                return false;
            const u8* p = entry + off;
            const u8* const end = p + b;
            while (p < end)
            {
                const u8 op = *p++;
                if (op == 0)
                    continue;                   // GX_NOP
                if (op != kGxStrip || end - p < 2)
                    return false;
                const u32 n = (u32)p[0] << 8 | p[1];
                p += 2;
                if ((u32)(end - p) < n * attrs * 2)
                    return false;
                v.clear();
                for (u32 i = 0; i < n; i++)
                {
                    const u32 idx = (u32)p[0] << 8 | p[1];
                    for (u32 c = 1; c < attrs; c++)
                        if (((u32)p[2 * c] << 8 | p[2 * c + 1]) != idx)
                            return false;
                    p += 2 * attrs;
                    v.push_back(physRecord(basePa + idx * vs, vs));
                }
                addStrip(v, out);
            }
        }
        else if (a & kDlBase)
            basePa = b;
        else if (a & kDlDirect)
            cpuTriangles(vtx, vs, a & kDlArg, b, out);
        else
            return false;
    }
    return true;
}

struct FetchRec
{
    u32 off, count, vsize;
    bool color;
    bool alphaOff;          // colour list with TSP UseAlpha 0: gxRend forces its vertex alpha opaque
    u8 colorWord;
    u8 attrs;               // attributes the GPU fetches (the Wii's fmtKey): position, normal, colour, UV
    u32 slot;               // DRAW MEMO: the slot found with its box when recorded (+1), 0: none
};

struct CheckTotals
{
    u64 vertices, lists, fetched, built, written, color, other, patchVertices;
    u64 liveWords;          // ring words of the programs drawn (each once a render)
    u64 lightSets;          // light sets the Wii's arena would hold
    u32 lightSetsMax;       // ...in its fullest render
    u32 draws, mismatches, overruns;
    u32 boxAsked, boxes, boxStale;  // lists recorded with 3D LIST CULL on, boxes found, boxes not holding the records
    u64 colorFetched;               // COLOUR LISTS: colour list vertices drawn from programs
    u32 dlDraws, dlMismatches;      // DISPLAY LISTS: draws from an entry, and those not drawing the CPU's triangles
    u64 dlLiveBytes;                // ring bytes of the entries drawn (each once a render)
};

std::vector<FetchRec> s_fetchRecs;
std::vector<const FetchList*> s_drawnPrograms;  // this render's
CheckTotals s_check = {};
u32 s_checkRenders = 0;
// HEADLIGHTS SMOOTH's cone check (beamCheck): lists with a beam, their
// vertices, those in front of the lamp, and the projected cone's error there.
struct BeamCheck
{
    u64 lists, vertices, front, over;
    float maxErr;
    u64 masked, maskVertices, maskOver;     // ...and the front mask's, on lists reaching behind the lamp
    float maskMaxErr;
};
BeamCheck s_beamCheck = {};
u64 s_recordDigest = 14695981039346656037ull;   // recordDigest()

// This render's light sets, shared as the Wii's elan_gx_record shares them:
// same key as the last list, or a copy among the last kDedupDepth (4) sets.
std::vector<ElanGxLightSet> s_renderSets;
std::vector<u32> s_renderSetKeys;               // each set's latest key, as the Wii's s_lsetKeys
u32 s_lastSetKey = 0, s_lastSetIndex = 0;
// The Wii's draw loads a lit item's set into the XF lights when the item
// before it (gxRend draws each list type in recorded order) had another set.
u32 s_lastDrawnSet[8];
u64 s_setLoads = 0, s_setCompares = 0;
u32 s_setKeyStale = 0;                          // sets found by key whose contents differ (must stay 0)

void lightSetNote(const ElanGxRecord& r)
{
    u32 index;
    if (r.lightKey && r.lightKey == s_lastSetKey)
        index = s_lastSetIndex;
    else
    {
        s_lastSetKey = r.lightKey;
        const size_t n = s_renderSets.size();
        index = (u32)n;
        for (size_t k = 0; r.lightKey && k < 16 && k < n; k++)
            if (s_renderSetKeys[n - 1 - k] == r.lightKey)
            {
                index = (u32)(n - 1 - k);
                // A key names one set's contents: check that it does.
                if (memcmp(&s_renderSets[index], &r.lights, sizeof(r.lights)))
                    s_setKeyStale++;
                break;
            }
        for (size_t k = 0; index == n && k < 4 && k < n; k++)
        {
            s_setCompares++;
            if (!memcmp(&s_renderSets[n - 1 - k], &r.lights, sizeof(r.lights)))
                index = (u32)(n - 1 - k);
        }
        if (index == n)
        {
            s_renderSets.push_back(r.lights);
            s_renderSetKeys.push_back(0);
        }
        s_renderSetKeys[index] = r.lightKey;
        s_lastSetIndex = index;
    }
    const u32 type = r.pcw >> 24 & 7;
    if (r.lights.lit && s_lastDrawnSet[type] != index)
    {
        s_setLoads++;
        s_lastDrawnSet[type] = index;
    }
}

void fetchNote(const ElanGxRecord& r)
{
    const u8* ram = naomi_ElanRam();
    const u32 off = (u32)((const u8*)r.vtx - ram);
    if (!ram || off >= naomi_ElanRamSize())
        return;
    // As the Wii's drawItem: normals when lit or environment-mapped, UVs when
    // the layout has them and the list is not environment-mapped.
    const bool env = r.envMap, uv = !env && (r.layout == 0x00A || r.layout == 0x00E || r.layout == 0x04A);
    const bool color = r.colorFromVertex != 0;
    const u8 attrs = (u8)(1 + (r.lights.lit || env ? 1 : 0) + (color ? 1 : 0) + (uv ? 1 : 0));
    s_fetchRecs.push_back(
        {off, r.count, r.vsize, color, !(r.tsp >> 20 & 1), (u8)(r.layout == 0x042 ? 4 : 6), attrs, r.fetchSlot});
}

// DRAW MEMO, as the Wii's fetchDraw: the slot found when the list was recorded,
// else the full lookup. Every memo taken is checked against that lookup (the
// slot it would find, its pages, its program's age): `s_memoWrong` must stay 0.
u32 s_memoTaken = 0, s_memoWrong = 0;
const FetchList* drawLookup(const u32* vtx, const FetchRec& r, FetchOutcome& how)
{
    if (g_draw_memo_preset)
        if (FetchList* e = fetchMemo(r.slot, r.off, r.count, r.vsize, how))
        {
            s_memoTaken++;
            bool found;
            const FetchList* f = findList(r.off + 1, r.count, r.vsize, found);
            u32 newest;
            const bool settled = pagesSettled(r.off >> 12, (r.off + r.count * r.vsize - 1) >> 12, newest);
            if (f != e || !found || !settled || newest > e->built)
                s_memoWrong++;
            return e;
        }
    return fetchProgram(vtx, r.off, r.count, r.vsize, how);
}

bool fetchCheckInit(u32 size)
{
    s_ram = naomi_ElanRam();
    if (s_prog)
        return true;
    s_pages = (PageState*)calloc(kElanPages, sizeof(PageState));
    s_lists = (FetchList*)calloc(kFetchSlots, sizeof(FetchList));
    s_progWords = kProgWordsMax;
    s_prog = (u16*)malloc((s_progWords + kRingGuard) * sizeof(u16));
    if (!s_pages || !s_lists || !s_prog)
        return false;
    for (u32 k = 0; k < kRingGuard; k++)
        s_prog[s_progWords + k] = 0xA5A5;
    if (g_elan_dl_preset)
    {
        // The Wii's ring size unless IDAS3_DL_RING_KB says otherwise; 32-byte
        // aligned like the Wii's memalign, guard bytes after it.
        s_dlBytes = kElanDlRingBytes;
        if (const char* kb = getenv("IDAS3_DL_RING_KB"))
            s_dlBytes = (u32)atoi(kb) << 10;
        static std::vector<u64> ring;
        ring.assign((s_dlBytes + kRingGuard) / 8 + 4, 0xA5A5A5A5A5A5A5A5ull);
        s_dl = (u8*)(((uintptr_t)ring.data() + 31) & ~(uintptr_t)31);
    }
    s_fetchNoPatches = g_elan_fetch_preset == 2;
    if (const char* sc = getenv("IDAS3_FETCH_SCATTER"))
        s_scatterPct = (u32)atoi(sc);
    s_simFrame.assign(kElanPages, 0);
    s_frameOwner.assign(kSimFrames, -1);
    s_pageWritten.assign(size >> 12, 0);
    s_pageChecked.assign(size >> 12, 0);
    return true;
}

} // namespace

void elan_gx_census_render()
{
    u8* ram = naomi_ElanRam();
    const u32 size = naomi_ElanRamSize();
    if (!ram || !g_elan_gx_preset || size > kElanBytes || !fetchCheckInit(size))
        return;
    s_check.lightSets += s_renderSets.size();
    if (s_renderSets.size() > s_check.lightSetsMax)
        s_check.lightSetsMax = (u32)s_renderSets.size();
    s_renderSets.clear();
    s_renderSetKeys.clear();
    s_lastSetKey = 0;
    for (u32& k : s_lastDrawnSet)
        k = 0xFFFFFFFFu;
    takeWrites();                               // up to the draw, for pages not checked yet this render
    // gxRend waits for the previous render's GPU work before it draws.
    dlRenderStart();
    s_dlQueued.clear();
    static std::vector<std::pair<u32, u32>> drawnEntries;   // (offset, bytes) when called

    static std::vector<Tri3> fromProgram, fromCpu, fromDl;
    for (const FetchRec& r : s_fetchRecs)
    {
        s_check.vertices += r.count;
        s_check.lists++;
        if (r.color && !g_elan_colour_fetch_preset)
        {
            s_check.color += r.count;
            continue;
        }
        const u32* vtx = (const u32*)(ram + r.off);
        const u32 patchesBefore = s_fetchCounts.patchRecords;
        FetchOutcome how;
        const FetchList* e = drawLookup(vtx, r, how);
        if (how == kFetchBuilt)
            for (u32 k = 0; k < kRingGuard; k++)
                if (s_prog[s_progWords + k] != 0xA5A5)
                {
                    s_check.overruns++;
                    s_prog[s_progWords + k] = 0xA5A5;
                }
        s_check.patchVertices += s_fetchCounts.patchRecords - patchesBefore;
        if (!e)
        {
            (how == kFetchWritten ? s_check.written : s_check.other) += r.count;
            continue;
        }
        // COLOUR LISTS = GPU FETCH, as the Wii's fetchDraw (whose opaque rule,
        // gxRend's, also covers some UseAlpha 1 lists by texture format).
        if (r.color && !r.alphaOff && !alphaOpaque(const_cast<FetchList&>(*e), vtx, r.colorWord))
        {
            s_check.color += r.count;
            continue;
        }
        if (r.color)
            s_check.colorFetched += r.count;
        (how == kFetchBuilt ? s_check.built : s_check.fetched) += r.count;
        s_drawnPrograms.push_back(e);
        fromProgram.clear();
        fromCpu.clear();
        CheckSink sink = {vtx, r.vsize, 0, &fromProgram, {}};
        runProgram(s_prog + e->prog, s_prog + e->prog + e->len, sink);
        cpuTriangles(vtx, r.vsize, 0, r.count, fromCpu);
        std::sort(fromProgram.begin(), fromProgram.end());
        std::sort(fromCpu.begin(), fromCpu.end());
        s_check.draws++;
        if (fromProgram != fromCpu && ++s_check.mismatches <= 8)
            printf("[run]   GPU fetch check MISMATCH: list at ELAN +%X, %u vertices of %u bytes, %s program of %u "
                   "words: %u triangles against %u drawn by the CPU\n",
                   r.off, r.count, r.vsize, how == kFetchBuilt ? "new" : "cached", e->len,
                   (u32)fromProgram.size(), (u32)fromCpu.size());
        if (!s_dl)
            continue;
        // DISPLAY LISTS: the entry the Wii would call, its bytes read back as
        // the GPU would, against the CPU draw; and no entry written over one
        // still queued (called this render since the last wait).
        FetchList& fe = const_cast<FetchList&>(*e);
        const u32 builds = s_dlCounts.builds;
        const u8* entry = dlEntry(fe, r.attrs);
        if (!entry)
            continue;
        const u32 at = (u32)(entry - s_dl), size = ((const u32*)entry)[3];
        if (s_dlCounts.builds != builds)
        {
            for (const auto& q : s_dlQueued)
                if (at < q.first + q.second && q.first < at + size)
                    s_dlHazards++;
            for (u32 k = 0; k < kRingGuard; k++)
                if (s_dl[s_dlBytes + k] != 0xA5)
                {
                    s_check.overruns++;
                    s_dl[s_dlBytes + k] = 0xA5;
                }
        }
        dlNoteCall(fe);
        s_dlQueued.push_back({at, size});
        drawnEntries.push_back({at, size});
        fromDl.clear();
        const bool readable = dlTriangles(entry, vtx, r.vsize, r.attrs, fromDl);
        std::sort(fromDl.begin(), fromDl.end());
        s_check.dlDraws++;
        if ((!readable || fromDl != fromCpu) && ++s_check.dlMismatches <= 8)
            printf("[run]   DISPLAY LISTS check MISMATCH: list at ELAN +%X, %u vertices, %u attributes, entry at %u of "
                   "%u bytes: %s, %u triangles against %u drawn by the CPU\n",
                   r.off, r.count, r.attrs, at, size, readable ? "readable" : "BAD BYTES", (u32)fromDl.size(),
                   (u32)fromCpu.size());
    }
    s_fetchRecs.clear();
    std::sort(drawnEntries.begin(), drawnEntries.end());
    drawnEntries.erase(std::unique(drawnEntries.begin(), drawnEntries.end()), drawnEntries.end());
    for (const auto& d : drawnEntries)
        s_check.dlLiveBytes += d.second;
    drawnEntries.clear();
    std::sort(s_drawnPrograms.begin(), s_drawnPrograms.end());
    s_drawnPrograms.erase(std::unique(s_drawnPrograms.begin(), s_drawnPrograms.end()), s_drawnPrograms.end());
    for (const FetchList* e : s_drawnPrograms)
        s_check.liveWords += progSpan(*e);
    s_drawnPrograms.clear();
    // The pager moves pages while the game runs, i.e. after the draw: a page
    // checked when its list was recorded stays put until that list is drawn
    // (on the Wii the check holds the pager off it for a while).
    if (s_render % 16 == 0)
        for (u32 tries = 0, moved = 0; tries < 64 && moved < 2; tries++)
        {
            const u32 p = rnd() % (size >> 12);
            if (s_simFrame[p])
            {
                simAssign(p);
                moved++;
            }
        }
    s_render++;
    if (++s_checkRenders == 64)
    {
        const FetchCounts& c = s_fetchCounts;
        const double d = 64.0, v = s_check.vertices ? (double)s_check.vertices : 1.0;
        static u32 s_pageChecks0 = 0;
        printf("[run]   GPU fetch page checks per drawn render: %.0f\n", (c.pageChecks - s_pageChecks0) / d);
        s_pageChecks0 = c.pageChecks;
        printf("[run]   GPU fetch per drawn render: %.0f vertices in %.0f lists | from programs %.0f (%.0f%%), "
               "built now %.0f (%.0f%%), pages written since the last render %.0f (%.0f%%), colour lists %.0f "
               "(%.0f more fetched), other CPU-fed %.0f | patched records %.0f | programs drawn take %.0f of the "
               "ring's %u KB | light sets %.1f (most %u), loaded at draw %.1f, compared when recorded %.1f (found "
               "by key with other contents: %u STALE so far)\n",
               s_check.vertices / d, s_check.lists / d, s_check.fetched / d, 100.0 * s_check.fetched / v,
               s_check.built / d, 100.0 * s_check.built / v, s_check.written / d, 100.0 * s_check.written / v,
               s_check.color / d, s_check.colorFetched / d, s_check.other / d, s_check.patchVertices / d, s_check.liveWords * 2 / 1024.0 / d,
               s_progWords * 2 / 1024, s_check.lightSets / d, s_check.lightSetsMax, s_setLoads / d,
               s_setCompares / d, s_setKeyStale);
        s_setLoads = s_setCompares = 0;
        printf("[run]   GPU fetch check: %u draws compared with the CPU draw, %u MISMATCHES, %u ring overruns | "
               "programs so far: %u built, %u patches (%.1f records each), %u ring passes, %u too big, %u aborted, "
               "%u stale caught, %u table restarts\n",
               s_check.draws, s_check.mismatches, s_check.overruns, c.builds, c.patches,
               c.patches ? (double)c.patchRecords / c.patches : 0.0, c.wraps, c.tooBig, c.aborted, c.staleCaught,
               c.resets);
        if (s_dl)
        {
            const DlCounts& dc = s_dlCounts;
            printf("[run]   DISPLAY LISTS per drawn render: %.0f lists drawn from an entry, entries drawn take %.0f KB of "
                   "the ring's %u KB | %u draws checked, %u MISMATCHES, %u HAZARDS (entries written over queued ones) "
                   "| so far: %u entries written (%u KB), %u ring passes, %u GPU waits, %u too big\n",
                   s_check.dlDraws / d, s_check.dlLiveBytes / 1024.0 / d, s_dlBytes >> 10, s_check.dlDraws,
                   s_check.dlMismatches, s_dlHazards, dc.builds, dc.bytes >> 10, dc.wraps, dc.syncs, dc.tooBig);
        }
        if (s_check.boxAsked)
            printf("[run]   3D list boxes: %u lists asked, %u had a box (%.0f%%), every box checked against the "
                   "list's records: %u STALE\n",
                   s_check.boxAsked, s_check.boxes, 100.0 * s_check.boxes / s_check.boxAsked, s_check.boxStale);
        if (g_draw_memo_preset)
            printf("[run]   DRAW MEMO per drawn render: %.0f lists drawn from the slot found when recorded, each "
                   "checked against the full lookup: %u WRONG so far\n",
                   s_memoTaken / d, s_memoWrong);
        s_memoTaken = 0;
        if (s_beamCheck.lists)
            printf("[run]   HEADLIGHTS SMOOTH per drawn render: %.1f lists with a beam (%.1f masked) | cone check: %llu "
                   "vertices, %llu in front of the lamp, the projected cone against the ELAN's: max error %.3f, %llu "
                   "over 0.1 | mask check: %llu vertices, max error %.3f, %llu over 0.1\n",
                   s_beamCheck.lists / d, s_beamCheck.masked / d, (unsigned long long)s_beamCheck.vertices,
                   (unsigned long long)s_beamCheck.front, s_beamCheck.maxErr, (unsigned long long)s_beamCheck.over,
                   (unsigned long long)s_beamCheck.maskVertices, s_beamCheck.maskMaxErr,
                   (unsigned long long)s_beamCheck.maskOver);
        s_beamCheck = {};
        printf("[run]   record digest at render %u: %016llX\n", s_render, (unsigned long long)s_recordDigest);
        s_check = {};
        s_checkRenders = 0;
    }
}

bool elan_gx_list_box(const u32* vtx, u32 count, u32 vsize, float box[6], u32* slot)
{
    u8* ram = naomi_ElanRam();
    const u32 size = naomi_ElanRamSize();
    if (!ram || !g_elan_gx_preset || size > kElanBytes || !fetchCheckInit(size))
        return false;
    const u32 off = (u32)((const u8*)vtx - ram);
    if (off >= size)
        return false;
    s_check.boxAsked++;
    if (!cachedBox(off, count, vsize, box, slot))
        return false;
    elan_gx_box_check(vtx, count, vsize, box);
    return true;
}

// CACHE HINTS: cache hints only (no-ops with MSVC), as on the Wii.
void elan_gx_prefetch_list(u32 slot, u32 off)
{
    prefetchListTables(slot, off);
}

void elan_gx_prefetch_box(u32 slot)
{
    prefetchListBox(slot);
}

// Built from the records at an earlier render, a box is only good while they
// are unchanged: every record must still lie inside it.
void elan_gx_box_check(const u32* vtx, u32 count, u32 vsize, const float box[6])
{
    s_check.boxes++;
    using namespace elan_strips;
    const u32 words = vsize / 4;
    for (u32 i = 0; i < count; i++)
    {
        const u32* v = vtx + i * words;
        if (placeholder(v))
            continue;
        float p[3];
        memcpy(p, v + 1, sizeof(p));
        int c = 0;
        while (c < 3 && p[c] >= box[c] && p[c] <= box[3 + c])
            c++;
        if (c < 3)
        {
            if (++s_check.boxStale <= 8)
                printf("[run]   3D list box STALE: list at ELAN +%X, %u vertices: vertex %u (%g %g %g) outside "
                       "(%g %g %g)..(%g %g %g)\n",
                       (u32)((const u8*)vtx - naomi_ElanRam()), count, i, p[0], p[1], p[2], box[0], box[1], box[2],
                       box[3], box[4], box[5]);
            break;
        }
    }
}

void elan_gx_canvas(float& width, float& height)
{
    width = 640.f;                              // the snapshots' canvas
    height = 480.f;
}

u32 elan_gx_render()
{
    return s_render;
}

bool elan_gx_unwritten_since(const void* p, u32 bytes, u32 since)
{
    u8* ram = naomi_ElanRam();
    const u32 size = naomi_ElanRamSize();
    return ram && g_elan_gx_preset && size <= kElanBytes && fetchCheckInit(size) &&
           unwrittenSince((u32)((const u8*)p - ram), bytes, since);
}

// HEADLIGHTS SMOOTH: the Wii's projected cone at each vertex of a list with a
// beam (the texture through the texture matrix, sampled bilinear as GX does)
// against the ELAN's cone term there. A wrong matrix, axis or texture shows as
// large errors; between the vertices, per pixel, is where the two differ.
static float sampleBeam(const u8* tex, float s, float t)
{
    const float n = (float)kElanBeamTex;
    float x = s * n - 0.5f, y = t * n - 0.5f;
    x = x < 0.f ? 0.f : x > n - 1.f ? n - 1.f : x;
    y = y < 0.f ? 0.f : y > n - 1.f ? n - 1.f : y;
    const u32 x0 = (u32)x, y0 = (u32)y;
    const u32 x1 = x0 + 1 < kElanBeamTex ? x0 + 1 : x0, y1 = y0 + 1 < kElanBeamTex ? y0 + 1 : y0;
    const float fx = x - (float)x0, fy = y - (float)y0;
    const float top = tex[y0 * kElanBeamTex + x0] * (1.f - fx) + tex[y0 * kElanBeamTex + x1] * fx;
    const float bottom = tex[y1 * kElanBeamTex + x0] * (1.f - fx) + tex[y1 * kElanBeamTex + x1] * fx;
    return (top * (1.f - fy) + bottom * fy) / 255.f;
}

static void beamCheck(const ElanGxRecord& r)
{
    const ElanGxBeam& b = r.lights.beamLight;
    static float texP = NAN, texQ = NAN;
    static u8 tex[kElanBeamTex * kElanBeamTex];
    if (b.coneP != texP || b.coneQ != texQ)
    {
        elan_beam_texture(b, tex);
        texP = b.coneP;
        texQ = b.coneQ;
    }
    float m[3][4];
    elan_beam_texmtx(r.xform, b, m);
    const float* tm = r.xform.tm;
    s_beamCheck.lists++;
    static u8 maskTex[kElanBeamMaskW * kElanBeamMaskH];
    float mm[2][4] = {};
    if (b.mask > 0.f)
    {
        elan_beam_mask_texture(maskTex);
        elan_beam_maskmtx(r.xform, b, mm);
        s_beamCheck.masked++;
    }
    const u32 words = r.vsize / 4;
    for (u32 i = 0; i < r.count; i++)
    {
        const u32* v = r.vtx + i * words;
        if (elan_strips::placeholder(v))
            continue;
        float p[3];
        memcpy(p, v + 1, sizeof(p));
        float stq[3];
        for (int k = 0; k < 3; k++)
            stq[k] = m[k][0] * p[0] + m[k][1] * p[1] + m[k][2] * p[2] + m[k][3];
        s_beamCheck.vertices++;
        // The ELAN's terms at the vertex, in lighting space (-X, Y, -Z).
        const float P[3] = {-(tm[0] * p[0] + tm[3] * p[1] + tm[6] * p[2] + tm[9]),
                            tm[1] * p[0] + tm[4] * p[1] + tm[7] * p[2] + tm[10],
                            -(tm[2] * p[0] + tm[5] * p[1] + tm[8] * p[2] + tm[11])};
        if (b.mask > 0.f)
        {
            // The mask: 0 behind the lamp's plane, 1 from `mask` in front of it.
            const float z = (P[0] - b.pos[0]) * b.axis[0] + (P[1] - b.pos[1]) * b.axis[1] + (P[2] - b.pos[2]) * b.axis[2];
            float want = z / b.mask;
            want = want < 0.f ? 0.f : want > 1.f ? 1.f : want;
            const float s = mm[0][0] * p[0] + mm[0][1] * p[1] + mm[0][2] * p[2] + mm[0][3];
            const float n = (float)kElanBeamMaskW;
            float tx = s * n - 0.5f;
            tx = tx < 0.f ? 0.f : tx > n - 1.f ? n - 1.f : tx;
            const u32 x0 = (u32)tx, x1 = x0 + 1 < kElanBeamMaskW ? x0 + 1 : x0;
            const float got = (maskTex[x0] * (1.f - (tx - (float)x0)) + maskTex[x1] * (tx - (float)x0)) / 255.f;
            const float err = fabsf(got - want);
            s_beamCheck.maskVertices++;
            s_beamCheck.maskOver += err > 0.1f;
            if (err > s_beamCheck.maskMaxErr)
                s_beamCheck.maskMaxErr = err;
        }
        if (stq[2] <= 0.f)
            continue;                           // behind the lamp: no light there (channel 1, the mask)
        const float L[3] = {b.pos[0] - P[0], b.pos[1] - P[1], b.pos[2] - P[2]};
        const float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (!(d > 0.f))
            continue;
        const float c = -(L[0] * b.axis[0] + L[1] * b.axis[1] + L[2] * b.axis[2]) / d;
        float exact = b.coneP + b.coneQ * (c > 0.f ? c : 0.f);
        exact = exact < 0.f ? 0.f : exact > 1.f ? 1.f : exact;
        const float err = fabsf(sampleBeam(tex, stq[0] / stq[2], stq[1] / stq[2]) - exact);
        s_beamCheck.front++;
        s_beamCheck.over += err > 0.1f;
        if (err > s_beamCheck.maxErr)
            s_beamCheck.maxErr = err;
    }
}

// Digest of everything the renderer is handed (printed every 64 renders): an
// optimisation of the recording side must leave it exactly the same.
static void recordDigest(const ElanGxRecord& r)
{
    const u32 off = (u32)((const u8*)r.vtx - naomi_ElanRam());
    const u32 words[10] = {off, r.count, r.vsize, r.layout, r.pcw, r.isp, r.tsp, r.tcw, r.colorFromVertex, r.envMap};
    auto mix = [](const void* p, size_t n) {
        const u8* b = (const u8*)p;
        for (size_t k = 0; k < n; k++)
            s_recordDigest = (s_recordDigest ^ b[k]) * 1099511628211ull;
    };
    mix(words, sizeof(words));
    mix(r.base, sizeof(r.base));
    mix(&r.nearW, sizeof(r.nearW));
    mix(&r.farW, sizeof(r.farW));
    mix(&r.xform, sizeof(r.xform));
    mix(&r.lights, sizeof(r.lights));
}

bool elan_gx_record(const ElanGxRecord& r)
{
    stripCensusCounts(r);
    stripCensus(r);
    fetchNote(r);
    lightSetNote(r);
    recordDigest(r);
    if (r.lights.beam)
        beamCheck(r);
    return rend_AddElanItem(r.pcw, r.isp, r.tsp, r.tcw, 0, r.nearW, r.farW);
}

#else

#include <gccore.h>
#include <malloc.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wii/wii_fastmem.h"

namespace {

// Per frame. Opaque lists alone fit the small set; with translucent and
// punch-through lists too the larger one is tried first (~140 KB more).
constexpr u32 kMaxItems = 6144, kMinItems = 4096;
constexpr u32 kMaxXforms = 1536, kMinXforms = 1024;
// A day race uses ~4 light sets a render, the night attract demo ~33, 51 at
// most (NIGHT LIGHTS = GPU: each list's lights depend on its box). A set is
// ~500 bytes; lists past the last one take the CPU path.
constexpr u32 kMaxLightSets = 64, kMinLightSets = 48;
constexpr u32 kMaxIndices = 65535;              // GX_Begin's count is 16-bit
constexpr u32 kDedupDepth = 4;                  // earlier entries searched for a copy

struct Item
{
    const u32* vtx;
    u16 count;
    u8 vsize;
    u8 colorFromVertex;
    u8 envMap;
    u16 layout;
    u16 xform;
    u16 lights;
    u8 base[4];                                 // RGBA, material colour
    u8 textured;                                // PCW Texture: gxRend's stage 0 samples TEXMAP0
    u16 slot;                                   // DRAW MEMO: its fetch slot + 1 when recorded, 0: none
};

Item* s_items = nullptr;
ElanGxXform* s_xforms = nullptr;
ElanGxLightSet* s_lsets = nullptr;
u16* s_idx = nullptr;
u32 s_nItems = 0, s_nXforms = 0, s_nLsets = 0;
u32 s_capItems = 0, s_capXforms = 0, s_capLsets = 0;
bool s_allocFailed = false;
u32 s_loadedLights = 0xFFFFFFFFu;               // light set now in the XF light registers
// What PNMTX1 holds (gxRend only uses PNMTX0): consecutive lists mostly share
// their matrices, so they are loaded once.
u32 s_loadedPos = 0xFFFFFFFFu;                  // xform index in the position matrix
u32 s_loadedNrm = 0xFFFFFFFFu;                  // xform index * 2 + float normals, normal matrix
// The last xform / light set recorded and the state key it came from.
u32 s_lastXformKey = 0, s_lastXi = 0, s_lastLightKey = 0, s_lastLi = 0;
// Each entry's latest state key (0: none). A key always names the same
// contents (naomi_elan_tnl.cpp: matrix and light change counters, point-lit
// sets' own keys), so a record whose key an earlier entry has is that entry,
// without comparing ~100 (matrix) or ~600 (light set) bytes.
u32 s_xformKeys[kMaxXforms], s_lsetKeys[kMaxLightSets];
// CACHE HINTS: the item, matrix and light-set tables are filled front to back,
// starting over each render. A store into a line that is not in the cache
// first reads it from memory, and none of these lines still is (the game's
// work between two frames is far bigger than the L2): ~1-2K reads a heavy
// frame. The lines past what this render has written hold nothing it needs,
// so they are zeroed in the cache instead (dcbz: no read). Each table's
// front: its first line not zeroed this render. The tables are 32-byte
// aligned and their sizes multiples of 32 (24 x 6144 or 4096, 108 x 1536 or
// 1024, 564 x 64 or 48), so the last line zeroed is still theirs.
static_assert(sizeof(Item) * kMaxItems % 32 == 0 && sizeof(Item) * kMinItems % 32 == 0 &&
                  sizeof(ElanGxXform) * kMaxXforms % 32 == 0 && sizeof(ElanGxXform) * kMinXforms % 32 == 0 &&
                  sizeof(ElanGxLightSet) * kMaxLightSets % 32 == 0 && sizeof(ElanGxLightSet) * kMinLightSets % 32 == 0,
              "CACHE HINTS zeroes whole lines: every table must end on one");
u8* s_frontItems = nullptr;
u8* s_frontXforms = nullptr;
u8* s_frontLsets = nullptr;
void resetFronts()
{
    s_frontItems = (u8*)s_items;
    s_frontXforms = (u8*)s_xforms;
    s_frontLsets = (u8*)s_lsets;
}
// Zeroes the lines from `front` up to the one holding byte end - 1 (and no
// further than `limit`, the table's end).
inline void zeroFront(u8*& front, const void* base, const void* end, const void* limit)
{
    if (front < (const u8*)base)                // never set (tables allocated since): start over
        front = (u8*)base;
    const u8* const stop = (const u8*)end < (const u8*)limit ? (const u8*)end : (const u8*)limit;
    while (front < stop)
    {
        asm volatile("dcbz 0,%0" : : "r"(front) : "memory");
        front += 32;
    }
}
// GPU fetch (naomi_elan_fetch.inc, renders counted by elan_gx_frame_reset):
// an ELAN page's frame and whether it was written since (wii_fastmem.cpp),
// the program ring's physical addresses.
u32 fetchPagePhys(u32 page, int* written)
{
    return WiiGuestMem_ElanGpuPage(page, written);
}
u32 fetchRingPhys(const void* p)
{
    return MEM_VIRTUAL_TO_PHYSICAL(p);
}
void fetchFlush(const void* p, u32 bytes)
{
    DCStoreRange((void*)p, bytes);
}
// Mid-render, as gxRend does before recycling textures: the GPU is through
// every display list called so far.
void fetchDlSync()
{
    FP_T0(t);
    GX_DrawDone();
    if (FRAME_PROF_ON())
        g_fp_elan_dlwaittb += FP_TICKS32() - t;
}
#include "naomi_elan_fetch.inc"
// Whether the GPU's vertex cache was invalidated since the frame started or
// patch data was written into the ring.
bool s_vtxCacheStale = true;
void allocateFetch();

bool allocateArenas(u32 items, u32 xforms, u32 lsets)
{
    s_items = (Item*)memalign(32, sizeof(Item) * items);
    s_xforms = (ElanGxXform*)memalign(32, sizeof(ElanGxXform) * xforms);
    s_lsets = (ElanGxLightSet*)memalign(32, sizeof(ElanGxLightSet) * lsets);
    s_idx = (u16*)memalign(32, sizeof(u16) * kMaxIndices);
    if (s_items && s_xforms && s_lsets && s_idx)
    {
        s_capItems = items;
        s_capXforms = xforms;
        s_capLsets = lsets;
        resetFronts();
        return true;
    }
    free(s_items);
    free(s_xforms);
    free(s_lsets);
    free(s_idx);
    s_items = nullptr;
    s_xforms = nullptr;
    s_lsets = nullptr;
    s_idx = nullptr;
    return false;
}

bool allocate()
{
    if (s_items || s_allocFailed)
        return s_items != nullptr;
    if (!allocateArenas(kMaxItems, kMaxXforms, kMaxLightSets) &&
        !allocateArenas(kMinItems, kMinXforms, kMinLightSets))
        s_allocFailed = true;
    // Once per run: whether the GPU path got its tables (heap is tight).
    if (s_items)
    {
        printf("[elan] GPU T&L tables: %u lists, %u matrices, %u light sets a frame\n", s_capItems, s_capXforms,
               s_capLsets);
        allocateFetch();
    }
    else
        printf("[elan] GPU T&L tables: no memory, 3D stays on the CPU\n");
    return s_items != nullptr;
}

// Index of a copy of `v` among the last kDedupDepth entries of `table`, or -1.
template <class T>
int findRecent(const T* table, u32 n, const T& v)
{
    for (u32 k = 0; k < kDedupDepth && k < n; k++)
        if (!memcmp(&table[n - 1 - k], &v, sizeof(T)))
            return (int)(n - 1 - k);
    return -1;
}

// The same, looking first for an entry with the record's key among the last
// kKeyDepth (a few word compares; nearby lists alternate between a handful of
// matrices and light sets, e.g. point-lit and parallel-lit ones).
constexpr u32 kKeyDepth = 16;
template <class T>
int findShared(const T* table, const u32* keys, u32 n, const T& v, u32 key)
{
    if (key)
        for (u32 k = 0; k < kKeyDepth && k < n; k++)
            if (keys[n - 1 - k] == key)
                return (int)(n - 1 - k);
    return findRecent(table, n, v);
}

inline float asFloat(u32 w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
}

inline u8 toByte(float v)
{
    return v <= 0.f ? 0 : v >= 1.f ? 255 : (u8)(v * 255.f + 0.5f);
}

// sqrt(x) through frsqrte and two Newton steps (~4e-6), 0 for x <= 0.
inline float rootFast(float x)
{
    if (!(x > 0.f))
        return 0.f;
    float y;
    asm("frsqrte %0,%1" : "=f"(y) : "f"(x));
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}


void loadLights(const ElanGxLightSet& ls)
{
    for (u32 i = 0; i < ls.count; i++)
    {
        const ElanGxLight& l = ls.lights[i];
        GXLightObj lo;
        GX_InitLightColor(&lo, (GXColor){toByte(l.color[0]), toByte(l.color[1]), toByte(l.color[2]), 255});
        if (ls.localMask >> i & 1)
        {
            // GX_InitLightDir stores the direction negated: the hardware's
            // spot cosine is then dot(vertex-to-light, l.dir), the ELAN's.
            GX_InitLightPos(&lo, l.pos[0], l.pos[1], l.pos[2]);
            GX_InitLightDir(&lo, -l.dir[0], -l.dir[1], -l.dir[2]);
            GX_InitLightAttn(&lo, l.angle[0], l.angle[1], l.angle[2], l.dist[0], l.dist[1], l.dist[2]);
        }
        else
        {
            // A light far away along its direction acts as a parallel one.
            // Attenuation 1 under GX_AF_SPOT too (a set with local lights).
            GX_InitLightPos(&lo, l.dir[0] * 1.0e8f, l.dir[1] * 1.0e8f, l.dir[2] * 1.0e8f);
            GX_InitLightDir(&lo, 0.f, 0.f, 0.f);
            GX_InitLightAttn(&lo, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f);
        }
        GX_LoadLightObj(&lo, GX_LIGHT0 << i);
    }
    if (ls.beam)
    {
        // HEADLIGHTS SMOOTH, channel 1's only light: no cone (the texture is
        // the cone), angle term scale x c, c = dot(vertex-to-lamp, -axis).
        const ElanGxBeam& b = ls.beamLight;
        GXLightObj lo;
        GX_InitLightColor(&lo, (GXColor){toByte(b.color[0]), toByte(b.color[1]), toByte(b.color[2]), 255});
        GX_InitLightPos(&lo, b.pos[0], b.pos[1], b.pos[2]);
        GX_InitLightDir(&lo, b.axis[0], b.axis[1], b.axis[2]);   // stored negated: -axis, towards the lamp
        GX_InitLightAttn(&lo, 0.f, b.scale, 0.f, b.dist[0], b.dist[1], b.dist[2]);
        GX_LoadLightObj(&lo, GX_LIGHT7);
    }
}

// HEADLIGHTS SMOOTH: the cone textures (I8, tiled for GX), one per cone shape
// (Initial D has one or two). Static: nothing may grow on the heap in a game.
constexpr u32 kBeamTexSlots = 4;
struct BeamTex
{
    float p, q;
    GXTexObj obj;
};
BeamTex s_beamTex[kBeamTexSlots];
u32 s_nBeamTex = 0;
u8 s_beamTexels[kBeamTexSlots][kElanBeamTex * kElanBeamTex] __attribute__((aligned(32)));

const BeamTex* beamTexture(const ElanGxBeam& b)
{
    for (u32 k = 0; k < s_nBeamTex; k++)
        if (s_beamTex[k].p == b.coneP && s_beamTex[k].q == b.coneQ)
            return &s_beamTex[k];
    if (s_nBeamTex == kBeamTexSlots)
        return nullptr;
    BeamTex& t = s_beamTex[s_nBeamTex];
    static u8 linear[kElanBeamTex * kElanBeamTex];
    elan_beam_texture(b, linear);
    // I8 tiles: 8 x 4 texels, 32 bytes, row by row.
    u8* out = s_beamTexels[s_nBeamTex];
    const u32 tilesX = kElanBeamTex / 8;
    for (u32 y = 0; y < kElanBeamTex; y++)
        for (u32 x = 0; x < kElanBeamTex; x++)
            out[((y / 4) * tilesX + x / 8) * 32 + (y % 4) * 8 + x % 8] = linear[y * kElanBeamTex + x];
    DCFlushRange(out, kElanBeamTex * kElanBeamTex);
    GX_InitTexObj(&t.obj, out, kElanBeamTex, kElanBeamTex, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&t.obj, GX_LINEAR, GX_LINEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    t.p = b.coneP;
    t.q = b.coneQ;
    s_nBeamTex++;
    return &t;
}

// The front mask's step texture (I8, two 8 x 4 tiles side by side).
u8 s_maskTexels[kElanBeamMaskW * kElanBeamMaskH] __attribute__((aligned(32)));
GXTexObj s_maskObj;
bool s_maskReady = false;

GXTexObj* beamMaskTexture()
{
    if (!s_maskReady)
    {
        u8 linear[kElanBeamMaskW * kElanBeamMaskH];
        elan_beam_mask_texture(linear);
        const u32 tilesX = kElanBeamMaskW / 8;
        for (u32 y = 0; y < kElanBeamMaskH; y++)
            for (u32 x = 0; x < kElanBeamMaskW; x++)
                s_maskTexels[((y / 4) * tilesX + x / 8) * 32 + (y % 4) * 8 + x % 8] = linear[y * kElanBeamMaskW + x];
        DCFlushRange(s_maskTexels, sizeof(s_maskTexels));
        GX_InitTexObj(&s_maskObj, s_maskTexels, kElanBeamMaskW, kElanBeamMaskH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjLOD(&s_maskObj, GX_LINEAR, GX_LINEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        s_maskReady = true;
    }
    return &s_maskObj;
}

} // namespace

bool elan_gx_enabled()
{
    return g_elan_gx_preset && allocate() && s_nItems < s_capItems;
}

void elan_gx_frame_reset()
{
    s_render++;
    dlRenderStart();                            // gxRend waits for the last render's GPU work first
    s_vtxCacheStale = true;
    s_nItems = s_nXforms = s_nLsets = 0;
    resetFronts();
    s_loadedLights = 0xFFFFFFFFu;
    s_loadedPos = s_loadedNrm = 0xFFFFFFFFu;
    s_lastXformKey = s_lastLightKey = 0;
}

bool elan_gx_record(const ElanGxRecord& r)
{
    if (!s_items || s_nItems >= s_capItems || r.count == 0 || r.count > 0xFFFF)
        return false;
    // Nearby lists mostly share their matrix and lights: keep one copy. Same
    // state key as the last list: same contents, nothing to compare.
    const int xr = r.xformKey && r.xformKey == s_lastXformKey
                       ? (int)s_lastXi
                       : findShared(s_xforms, s_xformKeys, s_nXforms, r.xform, r.xformKey);
    const int lr = r.lightKey && r.lightKey == s_lastLightKey
                       ? (int)s_lastLi
                       : findShared(s_lsets, s_lsetKeys, s_nLsets, r.lights, r.lightKey);
    if ((xr < 0 && s_nXforms >= s_capXforms) || (lr < 0 && s_nLsets >= s_capLsets))
        return false;
    const bool hints = g_cache_hints_preset != 0;
    u32 xi, li;
    if (xr >= 0)
        xi = (u32)xr;
    else
    {
        if (hints)
            zeroFront(s_frontXforms, s_xforms, &s_xforms[s_nXforms + 1], &s_xforms[s_capXforms]);
        s_xforms[s_nXforms] = r.xform;
        xi = s_nXforms++;
    }
    if (lr >= 0)
        li = (u32)lr;
    else
    {
        if (hints)
            zeroFront(s_frontLsets, s_lsets, &s_lsets[s_nLsets + 1], &s_lsets[s_capLsets]);
        s_lsets[s_nLsets] = r.lights;
        li = s_nLsets++;
    }
    s_xformKeys[xi] = r.xformKey;               // the newest key naming these contents
    s_lsetKeys[li] = r.lightKey;
    s_lastXformKey = r.xformKey;
    s_lastXi = xi;
    s_lastLightKey = r.lightKey;
    s_lastLi = li;
    if (!rend_AddElanItem(r.pcw, r.isp, r.tsp, r.tcw, s_nItems, r.nearW, r.farW))
        return false;
    if (hints)
        zeroFront(s_frontItems, s_items, &s_items[s_nItems + 1], &s_items[s_capItems]);
    Item& it = s_items[s_nItems++];
    it.vtx = r.vtx;
    it.count = (u16)r.count;
    it.vsize = (u8)r.vsize;
    it.colorFromVertex = r.colorFromVertex;
    it.envMap = r.envMap;
    it.layout = (u16)r.layout;
    it.xform = (u16)xi;
    it.lights = (u16)li;
    for (int c = 0; c < 4; c++)
        it.base[c] = toByte(r.base[c]);
    it.textured = (u8)(r.pcw >> 3 & 1);
    it.slot = (u16)(r.fetchSlot <= 0xFFFF ? r.fetchSlot : 0);
    return true;
}

namespace {

// ---- GX state of a run of GPU-drawn lists ----------------------------------
// gxRend draws its own strips with POS/CLR0[/CLR1]/TEX0 on VTXFMT0, PNMTX0,
// its projection and an unlit colour channel, and sets those once per frame.
// An ELAN item swaps them for its own; consecutive items then only change what
// differs, and gxRend's state comes back (elan_gx_restore) just before gxRend
// draws anything itself. A race frame is ~1600 items and nearly no TA strips.
struct RunState
{
    bool active;                // ELAN state loaded: gxRend's needs restoring
    float proj[4];              // the item projection loaded
    u32 format;                 // vertex format loaded (fmtKey), ~0 = none
    u32 chan;                   // colour channel set-up loaded (chanKey), ~0 = none
    u32 matColor, ambColor;     // their register colours...
    bool matValid, ambValid;    // ...when loaded since the channel was set up
    u32 ambSet;                 // DRAW MEMO: the light set ambColor came from (with ambValid)
    bool envTexgen;             // TEXCOORD0 generated from the normal
    // HEADLIGHTS SMOOTH: the cone texture in TEXMAP7, whose beam matrices are
    // in TEXMTX3 / TEXMTX4 (xform << 16 | light set, of that render; ~0 =
    // none), the mask texture loaded in TEXMAP3.
    const void* beamTex;
    u32 beamMtxKey, beamMtxRender;
    bool maskLoaded;
};
RunState s_run = {};

inline u32 rgba(const u8* c, u8 a)
{
    return (u32)c[0] << 24 | (u32)c[1] << 16 | (u32)c[2] << 8 | a;
}

inline GXColor toColor(u32 v)
{
    return (GXColor){(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
}

// The vertex loop, one instance per vertex format: no per-vertex tests. The
// FIFO takes the guest words as they are (floats are already big-endian).
template <bool kNormal, bool kFloatNormal, bool kColor, bool kUV>
void emitVertices(const Item& it, u32 n, u32 alphaOr)
{
    const u32 words = it.vsize / 4;
    const u32 colorWord = it.layout == 0x042 ? 4 : 6;
    const u32 uvWord = it.layout == 0x00E ? 8 : 4;
    const u32* const base = it.vtx;
    for (u32 k = 0; k < n; k++)
    {
        const u32* v = base + s_idx[k] * words;
        wgPipe->U32 = v[1];
        wgPipe->U32 = v[2];
        wgPipe->U32 = v[3];
        if (kNormal)
        {
            if (kFloatNormal)
            {
                wgPipe->U32 = v[4];
                wgPipe->U32 = v[5];
                wgPipe->U32 = v[6];
            }
            else
            {
                // x y z are the header's low three bytes, lowest first. Sent
                // as z y x, their order in memory, which is what the GPU
                // reads when it fetches the records itself (GPU fetch below):
                // the normal matrix swaps x and z back for both paths.
                const u32 h = v[0];
                wgPipe->U8 = (u8)(h >> 16);
                wgPipe->U16 = (u16)h;
            }
        }
        if (kColor)
        {
            const u32 c = v[colorWord] | alphaOr;   // ARGB -> RGBA in one write
            wgPipe->U32 = c << 8 | c >> 24;
        }
        if (kUV)
        {
            wgPipe->U32 = v[uvWord];
            wgPipe->U32 = v[uvWord + 1];
        }
    }
}

using EmitFn = void (*)(const Item&, u32, u32);
template <u32 K>
void emitFormat(const Item& it, u32 n, u32 alphaOr)
{
    emitVertices<(K & 8) != 0, (K & 4) != 0, (K & 2) != 0, (K & 1) != 0>(it, n, alphaOr);
}

// The strips of vertices [first, count), each its own GX primitive straight
// from the guest records: no index pass, no joining vertices. A strip with
// fan vertices goes through the joined indices instead (kEmit).
template <bool kNormal, bool kFloatNormal, bool kColor, bool kUV>
void drawStrips(const Item& it, u32 first, u32 count, u32 alphaOr, EmitFn indexed)
{
    using namespace elan_strips;
    const u32 words = it.vsize / 4;
    const u32 colorWord = it.layout == 0x042 ? 4 : 6;
    const u32 uvWord = it.layout == 0x00E ? 8 : 4;
    for (u32 i = first; i < count;)
    {
        if (placeholder(it.vtx + i * words))
        {
            i++;
            continue;
        }
        bool fan;
        const u32 j = stripEnd(it.vtx, words, i, count, fan);
        if (fan)
        {
            const u32 m = joinedIndices(it.vtx, words, i, j, s_idx, kMaxIndices);
            if (m >= 3)
            {
                GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT2, (u16)m);
                indexed(it, m, alphaOr);
                GX_End();
            }
        }
        else if (j - i >= 3)
        {
            GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT2, (u16)(j - i));
            const u32* v = it.vtx + i * words;
            for (u32 k = i; k < j; k++, v += words)
            {
                wgPipe->U32 = v[1];
                wgPipe->U32 = v[2];
                wgPipe->U32 = v[3];
                if (kNormal)
                {
                    if (kFloatNormal)
                    {
                        wgPipe->U32 = v[4];
                        wgPipe->U32 = v[5];
                        wgPipe->U32 = v[6];
                    }
                    else
                    {
                        const u32 h = v[0];
                        wgPipe->U8 = (u8)(h >> 16);
                        wgPipe->U16 = (u16)h;
                    }
                }
                if (kColor)
                {
                    const u32 c = v[colorWord] | alphaOr;
                    wgPipe->U32 = c << 8 | c >> 24;
                }
                if (kUV)
                {
                    wgPipe->U32 = v[uvWord];
                    wgPipe->U32 = v[uvWord + 1];
                }
            }
            GX_End();
        }
        i = j;
    }
}

using DrawFn = void (*)(const Item&, u32, u32, u32, EmitFn);
template <u32 K>
void drawFormat(const Item& it, u32 first, u32 count, u32 alphaOr, EmitFn indexed)
{
    drawStrips<(K & 8) != 0, (K & 4) != 0, (K & 2) != 0, (K & 1) != 0>(it, first, count, alphaOr, indexed);
}
// Indexed by fmtKey: normal 8, float normal 4, colour 2, UV 1.
const EmitFn kEmit[16] = {emitFormat<0>,  emitFormat<1>,  emitFormat<2>,  emitFormat<3>,
                          emitFormat<4>,  emitFormat<5>,  emitFormat<6>,  emitFormat<7>,
                          emitFormat<8>,  emitFormat<9>,  emitFormat<10>, emitFormat<11>,
                          emitFormat<12>, emitFormat<13>, emitFormat<14>, emitFormat<15>};
const DrawFn kDraw[16] = {drawFormat<0>,  drawFormat<1>,  drawFormat<2>,  drawFormat<3>,
                          drawFormat<4>,  drawFormat<5>,  drawFormat<6>,  drawFormat<7>,
                          drawFormat<8>,  drawFormat<9>,  drawFormat<10>, drawFormat<11>,
                          drawFormat<12>, drawFormat<13>, drawFormat<14>, drawFormat<15>};

// fmtKey bit 4: the GPU fetches the attributes (16-bit indices, GPU fetch
// below) instead of taking them from the FIFO. With colour (COLOUR LISTS =
// GPU FETCH) it reads RGB8, alpha 255 (alphaOpaque in naomi_elan_fetch.inc).
constexpr u32 kFmtFetch = 16;

void setFormat(u32 key)
{
    const bool fetch = (key & kFmtFetch) != 0;
    const u8 src = fetch ? GX_INDEX16 : GX_DIRECT;
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, src);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    if (key & 8)
    {
        GX_SetVtxDesc(GX_VA_NRM, src);
        GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_NRM, GX_NRM_XYZ, (key & 4) ? GX_F32 : GX_S8, 0);
    }
    if (key & 2)
    {
        GX_SetVtxDesc(GX_VA_CLR0, src);
        GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, fetch ? GX_CLR_RGB : GX_CLR_RGBA, fetch ? GX_RGB8 : GX_RGBA8, 0);
    }
    if (key & 1)
    {
        GX_SetVtxDesc(GX_VA_TEX0, src);
        GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    }
}

inline void useFormat(u32 key)
{
    if (key != s_run.format)
    {
        setFormat(key);
        s_run.format = key;
    }
}

// ---- GPU fetch: the GPU reads the vertex records itself --------------------
// Streaming the records through the CPU costs a cache miss per vertex on a
// Wii (~220 ns: they are read once a render, from memory): ~25 ms a race
// render, the GPU idle meanwhile. Instead, once a list's strips are known,
// the GPU fetches its records as indexed vertex arrays whose base is the
// records' physical address, and the CPU only writes indices: the list's
// program (naomi_elan_fetch.inc). A list with a page written since the
// previous render is drawn directly (its data is in flux), and so are colour
// lists (the records hold ARGB, the GPU wants RGBA). On the Windows runner
// ~95% of a race render's vertices qualify, and every program built there is
// checked against the CPU draw (elan_gx_census_render).
bool s_fetchOn = false, s_fetchTried = false;

void allocateFetch()
{
    if (s_fetchTried)
        return;
    s_fetchTried = true;
    if (!g_elan_fetch_preset)
    {
        printf("[elan] GPU vertex fetch: off (launch option)\n");
        return;
    }
    if (!WiiGuestMem_ElanGpuReady())
    {
        printf("[elan] GPU vertex fetch: off (page writes cannot be tracked)\n");
        return;
    }
    s_pages = (PageState*)calloc(kElanPages, sizeof(PageState));
    s_lists = (FetchList*)calloc(kFetchSlots, sizeof(FetchList));
    s_progWords = kProgWordsMax;
    s_prog = (u16*)memalign(32, s_progWords * sizeof(u16));
    if (!s_prog)
    {
        s_progWords = kProgWordsMin;
        s_prog = (u16*)memalign(32, s_progWords * sizeof(u16));
    }
    if (!s_pages || !s_lists || !s_prog)
    {
        free(s_pages);
        free(s_lists);
        free(s_prog);
        s_pages = nullptr;
        s_lists = nullptr;
        s_prog = nullptr;
        printf("[elan] GPU vertex fetch: no memory, lists stay CPU-fed\n");
        return;
    }
    s_fetchNoPatches = g_elan_fetch_preset == 2;
    s_fetchOn = true;
    // DISPLAY LISTS: _vmem.cpp left the page pool's heap headroom this much
    // larger for it.
    if (g_elan_dl_preset)
    {
        // A smaller ring when the full one does not fit (diagnostics such as JIT
        // HOTBLOCKS take ~450 KB of heap): a night render's entries take ~350-380
        // KB, so 768 KB still keeps them across renders; 512 KB rebuilds more.
        static const u32 kTry[] = {kElanDlRingBytes, 768u << 10, 512u << 10};
        for (u32 k = 0; k < sizeof(kTry) / sizeof(kTry[0]) && !s_dl; k++)
        {
            s_dl = (u8*)memalign(32, kTry[k]);
            s_dlBytes = s_dl ? kTry[k] : 0;
        }
    }
    struct mallinfo mi = mallinfo();
    printf("[elan] GPU vertex fetch: on%s (%u KB of tables, %u KB of them for programs; heap %u KB free inside "
           "malloc, MEM2 arena %u KB left)\n",
           s_fetchNoPatches ? ", no patches" : "",
           (u32)((kElanPages * sizeof(PageState) + kFetchSlots * sizeof(FetchList) + s_progWords * 2) >> 10),
           s_progWords * 2 >> 10, (u32)mi.fordblks >> 10,
           (u32)((u8*)SYS_GetArena2Hi() - (u8*)SYS_GetArena2Lo()) >> 10);
    if (g_elan_dl_preset)
        printf("[elan] GX display lists: %s (%u KB ring)\n", s_dl ? "on" : "no memory, cached lists send their indices",
               (s_dl ? s_dlBytes : kElanDlRingBytes) >> 10);
}

void setArrays(const Item& it, u32 pa, u32 fmtKey)
{
    // GX_SetArray takes a cached virtual address and keeps its physical part.
    const u8 stride = it.vsize;
    GX_SetArray(GX_VA_POS, (void*)(0x80000000u | (pa + 4)), stride);
    if (fmtKey & 8)                             // float normal: words 4-6; S8: header bytes z y x
        GX_SetArray(GX_VA_NRM, (void*)(0x80000000u | (pa + ((fmtKey & 4) ? 16 : 1))), stride);
    if (fmtKey & 2)                             // ARGB colour word 4 or 6: its R G B bytes
        GX_SetArray(GX_VA_CLR0, (void*)(0x80000000u | (pa + (it.layout == 0x042 ? 16 : 24) + 1)), stride);
    if (fmtKey & 1)
        GX_SetArray(GX_VA_TEX0, (void*)(0x80000000u | (pa + (it.layout == 0x00E ? 32 : 16))), stride);
}

// One vertex: the same index for every fetched attribute, in one or two writes.
template <u32 kAttrs>
inline void putIndex(u32 k)
{
    if (kAttrs == 1)
        wgPipe->U16 = (u16)k;
    else if (kAttrs == 2)
        wgPipe->U32 = k << 16 | k;
    else if (kAttrs == 3)
    {
        wgPipe->U32 = k << 16 | k;
        wgPipe->U16 = (u16)k;
    }
    else
    {
        wgPipe->U32 = k << 16 | k;
        wgPipe->U32 = k << 16 | k;
    }
}

// The GX side of runProgram: index draws over the records.
template <u32 kAttrs>
struct GxSink
{
    const Item& it;
    u32 fmtKey, alphaOr;
    bool inPatch;
    u32 strips;

    void base(u32 pa, bool patch)
    {
        setArrays(it, pa, fmtKey);
        inPatch = patch;
    }
    void run(u32 s, u32 n, bool odd)
    {
        useFormat(fmtKey | kFmtFetch);
        GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT2, (u16)(n + (odd ? 1 : 0)));
        if (odd)
            putIndex<kAttrs>(s);
        for (u32 k = s; k < s + n; k++)
            putIndex<kAttrs>(k);
        GX_End();
        strips++;
        if (inPatch)
            g_fp_elan_patch += n;
    }
    void list(const u16* p, u32 n)
    {
        useFormat(fmtKey | kFmtFetch);
        GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT2, (u16)n);
        for (u32 k = 0; k < n; k++)
            putIndex<kAttrs>(p[k]);
        GX_End();
        strips++;
    }
    void direct(u32 i, u32 j)
    {
        useFormat(fmtKey);
        kDraw[fmtKey](it, i, j, alphaOr, kEmit[fmtKey]);
        g_fp_elan_gap += j - i;
    }
};

template <u32 kAttrs>
void drawProgram(const Item& it, const FetchList& e, u32 fmtKey, u32 alphaOr)
{
    GxSink<kAttrs> sink = {it, fmtKey, alphaOr, false, 0};
    const u16* p = s_prog + e.prog;
    FP_T0(t);
    runProgram(p, p + e.len, sink);
    if (FRAME_PROF_ON())
    {
        g_fp_elan_progtb += FP_TICKS32() - t;
        g_fp_elan_strips += sink.strips;
    }
}

// The same draws from the list's display-list entry (DISPLAY LISTS): the CPU
// sets array bases and calls the GPU-read bytes; CPU-fed strips as before.
void drawEntry(const Item& it, const u8* entry, u32 fmtKey, u32 alphaOr)
{
    FP_T0(t);
    const u32* h = (const u32*)entry;
    const u32* s = h + kDlHeader / 4;
    for (u32 k = 0; k < h[0]; k++, s += 2)
    {
        const u32 a = s[0], b = s[1];
        if (a & kDlCall)
        {
            useFormat(fmtKey | kFmtFetch);
            GX_CallDispList((void*)(entry + (a & kDlArg)), b);   // flushes the GX state first
        }
        else if (a & kDlBase)
            setArrays(it, b, fmtKey);
        else
        {
            const u32 i = a & kDlArg;
            useFormat(fmtKey);
            kDraw[fmtKey](it, i, b, alphaOr, kEmit[fmtKey]);
            g_fp_elan_gap += b - i;
        }
    }
    if (FRAME_PROF_ON())
    {
        g_fp_elan_progtb += FP_TICKS32() - t;
        g_fp_elan_strips += h[1];
        g_fp_elan_patch += h[2];
        g_fp_elan_dlcalls++;
    }
}

// Program events worth a line in the log, once: they say how far the fetch
// path got if the run ends badly.
void noteFetchEvents()
{
    static u32 patches = 0, aborted = 0, stale = 0;
    const FetchCounts& c = s_fetchCounts;
    g_fp_elan_resets = c.resets;
    g_fp_elan_progs = c.builds;
    g_fp_elan_patches = c.patches;
    g_fp_elan_wraps = c.wraps;
    g_fp_elan_bounds = c.bounds;
    g_fp_elan_gaps = c.gaps;
    g_fp_elan_dlbuilds = s_dlCounts.builds;
    g_fp_elan_dlkb = s_dlCounts.bytes >> 10;
    g_fp_elan_dlsyncs = s_dlCounts.syncs;
    g_fp_elan_dlwraps = s_dlCounts.wraps;
    if (c.patches && !patches)
        printf("[elan] GPU fetch: first patch (triangles over a gap between frames, %u records copied)\n",
               c.patchRecords);
    if (c.aborted && !aborted)
        printf("[elan] GPU fetch: a program did not fit in the ring, list CPU-fed (%u programs built so far)\n",
               c.builds);
    if (c.staleCaught && !stale)
    {
        const StaleInfo& s = s_staleFirst;
        printf("[elan] GPU fetch: a cached program changed under its list (ELAN +%X, %u vertices, ring pass %u of "
               "%u, at %u+%u, head %u), rebuilt\n",
               s.key - 1, s.count, s.gen, s.ringGen, s.prog, s.len, s.head);
    }
    patches = c.patches;
    aborted = c.aborted;
    stale = c.staleCaught;
}

// Draws the list from its program with the GPU reading the records. False:
// draw it directly (a page written since the previous render or not resident,
// a list too big for the ring).
bool fetchDraw(const Item& it, u32 fmtKey, u32 alphaOr)
{
    FetchOutcome how;
    const u32 off = (u32)it.vtx - WII_GUEST_ELAN_EA;
    FetchList* e = g_draw_memo_preset ? fetchMemo(it.slot, off, it.count, it.vsize, how) : nullptr;
    if (!e)
        e = const_cast<FetchList*>(fetchProgram(it.vtx, off, it.count, it.vsize, how));
    if (how == kFetchWritten)
        g_fp_elan_wrote += it.count;
    noteFetchEvents();
    if (!e)
        return false;
    // A colour list whose vertex alpha shows stays CPU-fed (it still has its
    // program, so its box).
    if ((fmtKey & 2) && !alphaOr && !alphaOpaque(*e, it.vtx, it.layout == 0x042 ? 4 : 6))
        return false;
    const u32 attrs = 1 + ((fmtKey >> 3) & 1) + ((fmtKey >> 1) & 1) + (fmtKey & 1);
    const u8* entry = dlEntry(*e, attrs);
    if (how == kFetchBuilt)
        g_fp_elan_build += it.count;
    else
        g_fp_elan_fetch += it.count;
    if (s_fetchPatched)
    {
        s_fetchPatched = false;
        s_vtxCacheStale = true;                 // the ring may have held other records there
    }
    if (s_vtxCacheStale)
    {
        GX_InvVtxCache();                       // pages rewritten since may sit at the same addresses
        s_vtxCacheStale = false;
    }
    if (entry)
    {
        dlNoteCall(*e);
        drawEntry(it, entry, fmtKey, alphaOr);
        return true;
    }
    if (attrs == 1)
        drawProgram<1>(it, *e, fmtKey, alphaOr);
    else if (attrs == 2)
        drawProgram<2>(it, *e, fmtKey, alphaOr);
    else if (attrs == 3)
        drawProgram<3>(it, *e, fmtKey, alphaOr);
    else
        drawProgram<4>(it, *e, fmtKey, alphaOr);
    return true;
}

// gxRend's canvas, as the last item drawn saw it.
float s_canvasW = 0.f, s_canvasH = 0.f;

} // namespace

bool elan_gx_list_box(const u32* vtx, u32 count, u32 vsize, float box[6], u32* slot)
{
    return s_fetchOn && cachedBox((u32)vtx - WII_GUEST_ELAN_EA, count, vsize, box, slot);
}

void elan_gx_prefetch_list(u32 slot, u32 off)
{
    if (s_fetchOn)
        prefetchListTables(slot, off);
}

void elan_gx_prefetch_box(u32 slot)
{
    if (s_fetchOn)
        prefetchListBox(slot);
}

void elan_gx_canvas(float& width, float& height)
{
    width = s_canvasW;
    height = s_canvasH;
}

u32 elan_gx_render()
{
    return s_render;
}

bool elan_gx_unwritten_since(const void* p, u32 bytes, u32 since)
{
    return s_fetchOn && unwrittenSince((u32)p - WII_GUEST_ELAN_EA, bytes, since);
}

void elan_gx_restore(const ElanGxFrame& f)
{
    if (!s_run.active)
        return;
    s_run.active = false;
    if (s_run.envTexgen)
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_LoadProjectionMtx((f32(*)[4])f.proj, GX_PERSPECTIVE);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    if (f.clr1)
        GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    if (f.clr1)
        GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
}

static bool drawItem(u32 index, const ElanGxFrame& f, bool forceOpaqueAlpha);

static void prefetchAhead(u32 index);

bool elan_gx_draw(u32 index, const ElanGxFrame& f, bool forceOpaqueAlpha)
{
    // PROFILER: [HW] line, the ELAN share of a render on the CPU.
    FP_T0(t);
    prefetchAhead(index);
    s_canvasW = f.width;
    s_canvasH = f.height;
    const bool borrowed = drawItem(index, f, forceOpaqueAlpha);
    if (FRAME_PROF_ON())
    {
        g_fp_elan_ticks += FP_TICKS32() - t;
        if (index < s_nItems)
        {
            g_fp_elan_verts += s_items[index].count;
            g_fp_elan_items++;
        }
    }
    return borrowed;
}

// The renderer draws the items mostly in recorded order, and each costs a few
// cache misses in our own tables before a vertex moves: the item, its
// matrices, its list slot, its program, its page state. They are asked for
// one to three items ahead. (These tables are BAT-mapped, where dcbt works;
// on the paged guest memory it is dropped at every TLB miss.)
static void prefetchAhead(u32 index)
{
#if defined(__GNUC__)
    // CACHE HINTS: items one line further, and an item's matrix and light set
    // (when they change) two items ahead rather than one: a heavy render takes
    // ~5000 L2 misses, ~8 a list, and one item ahead was too close for them.
    // The light set's lights (up to 15 lines) when the next item loads another set.
    if (g_cache_hints_preset)
    {
        if (index + 5 < s_nItems)
            __builtin_prefetch(&s_items[index + 5]);
        if (index + 2 < s_nItems)
        {
            const Item& n1 = s_items[index + 1];
            const Item& n2 = s_items[index + 2];
            if (n2.xform != n1.xform)
            {
                const u8* x = (const u8*)&s_xforms[n2.xform];
                __builtin_prefetch(x);
                __builtin_prefetch(x + 32);
                __builtin_prefetch(x + 64);
                __builtin_prefetch(x + 96);
            }
            if (n2.lights != n1.lights)
                __builtin_prefetch(&s_lsets[n2.lights]);
        }
        if (index + 1 < s_nItems && s_items[index + 1].lights != s_items[index].lights)
        {
            const ElanGxLightSet& ls = s_lsets[s_items[index + 1].lights];
            if (ls.lit)
            {
                const u8* const p = (const u8*)&ls;
                const u32 bytes = (u32)offsetof(ElanGxLightSet, lights) + ls.count * sizeof(ElanGxLight);
                for (u32 b = 32; b < bytes; b += 32)
                    __builtin_prefetch(p + b);
                __builtin_prefetch(p + bytes - 1);     // sets are not line-aligned (564 bytes)
                if (ls.beam)
                {
                    __builtin_prefetch(&ls.beamLight);
                    __builtin_prefetch((const u8*)&ls.beamLight + sizeof(ElanGxBeam) - 1);
                }
            }
        }
    }
    if (index + 3 < s_nItems)
        __builtin_prefetch(&s_items[index + 3]);
    if (index + 2 < s_nItems && s_fetchOn)
    {
        const Item& n2 = s_items[index + 2];
        const u32 off = (u32)n2.vtx - WII_GUEST_ELAN_EA;
        if (g_draw_memo_preset && n2.slot)
            __builtin_prefetch(&s_lists[n2.slot - 1]);     // DRAW MEMO: its slot, no page state needed
        else
        {
            __builtin_prefetch(&s_lists[listHash(off + 1, n2.count)]);
            if (off < kElanBytes)
                __builtin_prefetch(&s_pages[off >> 12]);
        }
    }
    if (index + 1 < s_nItems)
    {
        const Item& n1 = s_items[index + 1];
        const u8* x = (const u8*)&s_xforms[n1.xform];
        __builtin_prefetch(x);
        __builtin_prefetch(x + 32);
        __builtin_prefetch(x + 64);
        __builtin_prefetch(x + 96);
        if (s_fetchOn)
        {
            const u32 key = (u32)n1.vtx - WII_GUEST_ELAN_EA + 1;
            const FetchList& e = g_draw_memo_preset && n1.slot ? s_lists[n1.slot - 1] : s_lists[listHash(key, n1.count)];
            if (e.key == key && e.count == n1.count)
            {
                if (s_dl && e.dl)
                    __builtin_prefetch(s_dl + (e.dl & ~31u));   // its display-list entry's steps
                else
                {
                    __builtin_prefetch(s_prog + e.prog);
                    __builtin_prefetch(s_prog + e.prog + 16);
                }
            }
        }
    }
#else
    (void)index;
#endif
}

// HEADLIGHTS SMOOTH: the item's beam (ElanGxBeam). Channel 1 lights it per
// vertex (GX light 7, no cone), TEXCOORD3 projects the cone texture (TEXMAP7)
// from the lamp, and TEV stages after gxRend's stage 0 add texture x channel
// 1 x cone: stage 1 puts channel 1 x cone in TEVREG2, the mask stage (a list
// reaching behind the lamp: TEXCOORD4 along the axis, the step in TEXMAP3)
// multiplies it, the last stage adds it times the list's texture sampled
// again (untextured: the last of the others adds it straight to the colour).
// The channel's light is clamped at 1 as the ELAN clamps the sum of its
// lights; the ELAN would also clamp the beam with the other lights.
// TEXCOORD3 and TEVREG2 as the FMV program borrows them (gxRend.cpp's resource
// notes: TEXCOORD1-2 are AUTOSORT's). The offset and fog stages these cover add
// nothing to an ELAN list (its channel 1 is black), and gxRend sets them back
// after the item (elan_gx_draw's result).
static void beamStage(u8 stage, u8 coord, u8 map, u8 chan, u8 a, u8 b, u8 c, bool last)
{
    GX_SetTevDirect(stage);
    GX_SetTevOrder(stage, coord, map, chan);
    GX_SetTevColorIn(stage, a, b, c, last ? GX_CC_CPREV : GX_CC_ZERO);
    GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, last ? GX_TEVPREV : GX_TEVREG2);
    GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static bool beamBegin(const Item& it, const ElanGxXform& x, const ElanGxLightSet& ls)
{
    const BeamTex* t = beamTexture(ls.beamLight);
    if (!t)
        return false;
    const bool mask = ls.beamLight.mask > 0.f;
    if (s_run.beamTex != t)
    {
        GX_LoadTexObj(const_cast<GXTexObj*>(&t->obj), GX_TEXMAP7);
        s_run.beamTex = t;
    }
    if (mask && !s_run.maskLoaded)
    {
        GX_LoadTexObj(beamMaskTexture(), GX_TEXMAP3);
        s_run.maskLoaded = true;
    }
    const u32 mtxKey = (u32)it.xform << 16 | it.lights;
    if (s_run.beamMtxKey != mtxKey || s_run.beamMtxRender != s_render)
    {
        float m[3][4];
        elan_beam_texmtx(x, ls.beamLight, m);
        GX_LoadTexMtxImm(m, GX_TEXMTX3, GX_MTX3x4);
        if (mask)
        {
            float mm[3][4] = {};
            elan_beam_maskmtx(x, ls.beamLight, mm);
            GX_LoadTexMtxImm(mm, GX_TEXMTX4, GX_MTX2x4);
        }
        s_run.beamMtxKey = mtxKey;
        s_run.beamMtxRender = s_render;
    }
    GX_SetTexCoordGen(GX_TEXCOORD3, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX3);
    if (mask)
        GX_SetTexCoordGen(GX_TEXCOORD4, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX4);
    GX_SetNumTexGens(mask ? 5 : 4);
    GX_SetNumChans(2);
    GX_SetChanCtrl(GX_COLOR1, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT7, GX_DF_CLAMP, GX_AF_SPOT);
    GX_SetChanCtrl(GX_ALPHA1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetChanAmbColor(GX_COLOR1A1, (GXColor){0, 0, 0, 0});
    GX_SetChanMatColor(GX_COLOR1A1, (GXColor){it.base[0], it.base[1], it.base[2], 0});
    u8 stage = GX_TEVSTAGE1;
    // Channel 1 x cone.
    beamStage(stage++, GX_TEXCOORD3, GX_TEXMAP7, GX_COLOR1A1, GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC,
              !it.textured && !mask);
    if (mask)                                   // ... x the front mask
        beamStage(stage++, GX_TEXCOORD4, GX_TEXMAP3, GX_COLORNULL, GX_CC_ZERO, GX_CC_C2, GX_CC_TEXC, !it.textured);
    if (it.textured)                            // ... x the list's texture, added
        beamStage(stage++, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C2, true);
    GX_SetNumTevStages(stage);                  // GX_TEVSTAGEn == n
    return true;
}

// Back to what the next item and gxRend expect, but the TEV stages (gxRend's).
static void beamEnd(const Item& it, const ElanGxFrame& f)
{
    GX_SetNumTexGens(it.textured ? 1 : 0);
    GX_SetChanMatColor(GX_COLOR1A1, (GXColor){0, 0, 0, 0});
    GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumChans(f.clr1 ? 2 : 1);
}

// LIGHT SUM (ElanGxLightSet::sumScale = S): channel 0 has a white material and
// gives min(1, sum / S); the ELAN's colour is m x min(1, sum), m the list's
// material, so after gxRend's stage 0 (whose colour is replaced; its alpha,
// texture alpha x the material's, stays):
//   TEVREG2 = clamp(S x channel 0);  TEVREG2 x= m;  colour = texture x TEVREG2
// m is in TEVREG1 (only the FMV program uses it, and it loads it again when it
// starts), TEVREG2 is scratch as for the beam. Only register materials (the
// pick leaves vertex-coloured lists at S = 1), not under AUTOSORT.
static void sumStage(u8 stage, u8 coord, u8 map, u8 chan, u8 b, u8 c, u8 d, u8 scale, u8 out)
{
    GX_SetTevDirect(stage);
    GX_SetTevOrder(stage, coord, map, chan);
    GX_SetTevColorIn(stage, GX_CC_ZERO, b, c, d);
    GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, scale, GX_TRUE, out);
    GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void sumBegin(const Item& it, u8 scale)
{
    GX_SetTevColor(GX_TEVREG1, (GXColor){it.base[0], it.base[1], it.base[2], 255});
    u8 stage = GX_TEVSTAGE1;
    sumStage(stage++, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC,
             scale == 4 ? GX_CS_SCALE_4 : GX_CS_SCALE_2, GX_TEVREG2);
    sumStage(stage++, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL, GX_CC_C2, GX_CC_C1, GX_CC_ZERO, GX_CS_SCALE_1,
             GX_TEVREG2);
    if (it.textured)
        sumStage(stage++, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL, GX_CC_C2, GX_CC_TEXC, GX_CC_ZERO, GX_CS_SCALE_1,
                 GX_TEVPREV);
    else
        sumStage(stage++, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C2,
                 GX_CS_SCALE_1, GX_TEVPREV);
    GX_SetNumTevStages(stage);                  // GX_TEVSTAGEn == n
}

static bool drawItem(u32 index, const ElanGxFrame& f, bool forceOpaqueAlpha)
{
    if (index >= s_nItems)
        return false;
    const Item& it = s_items[index];
    const ElanGxXform& x = s_xforms[it.xform];
    const ElanGxLightSet& ls = s_lsets[it.lights];
    if (it.count < 3)
        return false;

    if (!s_run.active)
    {
        // First item of a run: everything is gxRend's.
        s_run.active = true;
        s_run.format = s_run.chan = 0xFFFFFFFFu;
        s_run.envTexgen = false;
        s_run.beamTex = nullptr;
        s_run.beamMtxKey = 0xFFFFFFFFu;
        s_run.maskLoaded = false;
        s_run.proj[0] = s_run.proj[1] = s_run.proj[2] = s_run.proj[3] = NAN;   // never equal
        GX_SetCurrentMtx(GX_PNMTX1);
        if (f.clr1)
        {
            // No offset colour on this path: the offset/fog stage reads black.
            GX_SetChanMatColor(GX_COLOR1A1, (GXColor){0, 0, 0, 0});
            GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
        }
    }

    // Lighting space V = (-X, Y, -Z), X/Y/Z the ELAN view space (the CPU
    // path lights in it too). The TA path would submit (sx*W, sy*W, -W) with
    // sx = fx*X/Z + tx, sy = fy*Y/Z + ty, W = Z against gxRend's projection
    // G = [2/w 0 1 0; 0 -2/h -1 0; 0 0 p5 p6; 0 0 -1 0]. Folding that linear
    // step into G gives, in terms of V, the projection below: same screen
    // position, same depth mapping, and still GX's perspective form.
    if (memcmp(s_run.proj, x.proj, sizeof(s_run.proj)))
    {
        const float fx = x.proj[0], tx = x.proj[1], fy = x.proj[2], ty = x.proj[3];
        Mtx44 P = {{-2.f * fx / f.width, 0.f, 1.f - 2.f * tx / f.width, 0.f},
                   {0.f, -2.f * fy / f.height, 2.f * ty / f.height - 1.f, 0.f},
                   {0.f, 0.f, f.p5, f.p6},
                   {0.f, 0.f, -1.f, 0.f}};
        GX_LoadProjectionMtx(P, GX_PERSPECTIVE);
        memcpy(s_run.proj, x.proj, sizeof(s_run.proj));
    }

    if (s_loadedPos != it.xform)
    {
        const float* t = x.tm;
        Mtx mv = {{-t[0], -t[3], -t[6], -t[9]}, {t[1], t[4], t[7], t[10]}, {-t[2], -t[5], -t[8], -t[11]}};
        GX_LoadPosMtxImm(mv, GX_PNMTX1);
        s_loadedPos = it.xform;
    }

    const bool floatNormal = it.layout == 0x00E;
    const bool env = it.envMap;
    const bool hasUV = !env && (it.layout == 0x00A || it.layout == 0x00E || it.layout == 0x04A);
    const bool hasColor = it.colorFromVertex;
    const bool hasNormal = ls.lit || env;
    const u32 nrmKey = it.xform * 2u + (floatNormal ? 1u : 0u);
    if (env || (ls.lit && s_loadedNrm != nrmKey))
    {
        // GX does not renormalise: fold the matrix scale (and the S8 normal
        // scale, 1/64 on GX against 1/127 on the ELAN) into the normal matrix.
        const float* l = x.lm;
        const float r0 = l[0] * l[0] + l[1] * l[1] + l[2] * l[2], r1 = l[3] * l[3] + l[4] * l[4] + l[5] * l[5],
                    r2 = l[6] * l[6] + l[7] * l[7] + l[8] * l[8];
        // DRAW MEMO: the rows' lengths to ~4e-6 without libm's sqrtf (a 25-step
        // loop on the Wii, 3 of them each time a lit list's matrix changes).
        const float s = g_draw_memo_preset ? (rootFast(r0) + rootFast(r1) + rootFast(r2)) / 3.f
                                           : (sqrtf(r0) + sqrtf(r1) + sqrtf(r2)) / 3.f;
        const float k = (s > 0.f ? 1.f / s : 1.f) * (floatNormal ? 1.f : 64.f / 127.f);
        // S8 normals reach the GPU as z y x (their order in the records):
        // swapping the x and z columns puts them back.
        const u32 cx = floatNormal ? 0 : 2, cz = 2 - cx;
        Mtx nm = {{l[cx] * k, l[1] * k, l[cz] * k, 0.f}, {l[3 + cx] * k, l[4] * k, l[3 + cz] * k, 0.f},
                  {l[6 + cx] * k, l[7] * k, l[6 + cz] * k, 0.f}};
        if (ls.lit && s_loadedNrm != nrmKey)
        {
            GX_LoadNrmMtxImm(nm, GX_PNMTX1);
            s_loadedNrm = nrmKey;
        }
        if (env)
        {
            // The CPU path's u = envU + N.x / 2 + 0.5, v = envV + N.y / 2 + 0.5
            // with N the unit lighting-space normal: a texture matrix applied
            // to the raw vertex normal (GX_TG_NRM is not transformed).
            Mtx tm = {{nm[0][0] * 0.5f, nm[0][1] * 0.5f, nm[0][2] * 0.5f, x.env[0] + 0.5f},
                      {nm[1][0] * 0.5f, nm[1][1] * 0.5f, nm[1][2] * 0.5f, x.env[1] + 0.5f},
                      {0.f, 0.f, 0.f, 1.f}};
            GX_LoadTexMtxImm(tm, GX_TEXMTX2, GX_MTX2x4);
        }
    }
    if (env != s_run.envTexgen)
    {
        if (env)
            GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_NRM, GX_TEXMTX2);
        else
            GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        s_run.envTexgen = env;
    }
    if (ls.lit && s_loadedLights != it.lights)
    {
        loadLights(ls);
        s_loadedLights = it.lights;
    }

    // Colour channel 0: material = vertex colour or the list's base colour,
    // lit by the hardware lights (diffuse, clamped N.L), ambient from the
    // light model. Alpha is never lit. Local lights need the spot/distance
    // attenuation of the channel.
    const u8 matSrc = hasColor ? GX_SRC_VTX : GX_SRC_REG;
    const bool local = ls.lit && ls.localMask;
    // LIGHT SUM: the set was stored 1/sumScale as strong (naomi_elan_tnl.cpp
    // pickLights: only for lists this can draw), so the material goes after.
    const u8 sumScale = ls.lit && ls.sumScale > 1 && !hasColor ? ls.sumScale : (u8)0;
    const u32 chanKey = (ls.lit ? 0x100u | ls.count : 0u) | (hasColor ? 0x200u : 0u) | (local ? 0x400u : 0u);
    if (chanKey != s_run.chan)
    {
        if (ls.lit)
        {
            GX_SetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, matSrc, (u8)((1u << ls.count) - 1), GX_DF_CLAMP,
                           local ? GX_AF_SPOT : GX_AF_NONE);
            GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, matSrc, 0, GX_DF_NONE, GX_AF_NONE);
        }
        else
            GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, matSrc, 0, GX_DF_NONE, GX_AF_NONE);
        s_run.chan = chanKey;
        s_run.matValid = s_run.ambValid = false;
    }
    if (!hasColor)
    {
        static const u8 kWhite[3] = {255, 255, 255};
        const u32 mat = rgba(sumScale ? kWhite : it.base, forceOpaqueAlpha ? (u8)255 : it.base[3]);
        if (!s_run.matValid || mat != s_run.matColor)
        {
            GX_SetChanMatColor(GX_COLOR0A0, toColor(mat));
            s_run.matColor = mat;
            s_run.matValid = true;
        }
    }
    // DRAW MEMO: the same light set as the colour loaded: nothing to convert.
    if (ls.lit && !(g_draw_memo_preset && s_run.ambValid && s_run.ambSet == it.lights))
    {
        const u8 amb[3] = {toByte(ls.ambient[0]), toByte(ls.ambient[1]), toByte(ls.ambient[2])};
        const u32 a = rgba(amb, 255);
        if (!s_run.ambValid || a != s_run.ambColor)
        {
            GX_SetChanAmbColor(GX_COLOR0A0, toColor(a));
            s_run.ambColor = a;
            s_run.ambValid = true;
        }
        s_run.ambSet = it.lights;
    }

    const u32 fmtKey = (hasNormal ? 8u : 0u) | (hasNormal && floatNormal ? 4u : 0u) | (hasColor ? 2u : 0u) |
                       (hasUV ? 1u : 0u);
    const u32 alphaOr = forceOpaqueAlpha ? 0xFF000000u : 0u;
    const bool beam = ls.lit && ls.beam && !hasColor && !env && beamBegin(it, x, ls);
    if (sumScale)                               // never with a beam (pickLights)
    {
        sumBegin(it, sumScale);
        elan_Stats().sumDrawn++;
    }
    if (!(s_fetchOn && (!hasColor || g_elan_colour_fetch_preset) && fetchDraw(it, fmtKey, alphaOr)))
    {
        useFormat(fmtKey);
        kDraw[fmtKey](it, 0, it.count, alphaOr, kEmit[fmtKey]);
    }
    if (beam)
        beamEnd(it, f);
    return beam || sumScale;
}

#endif
