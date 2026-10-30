// GPU (GX) transform & lighting for NAOMI 2 ELAN polygon lists - see
// naomi_elan_gx.h. Wii only; the host build keeps the CPU path.
#include "naomi_elan_gx.h"
#include "naomi_elan.h"
#include "plugs/drkPvr/frame_prof.h"

int g_elan_gx_preset = 0;
int g_elan_fetch_preset = 1;
int g_elan_point_preset = 0;
int g_elan_box_preset = 0;

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

struct FetchRec
{
    u32 off, count, vsize;
    bool color;
};

struct CheckTotals
{
    u64 vertices, lists, fetched, built, written, color, other, patchVertices;
    u64 liveWords;          // ring words of the programs drawn (each once a render)
    u64 lightSets;          // light sets the Wii's arena would hold
    u32 lightSetsMax;       // ...in its fullest render
    u32 draws, mismatches, overruns;
    u32 boxAsked, boxes, boxStale;  // lists recorded with 3D LIST CULL on, boxes found, boxes not holding the records
};

std::vector<FetchRec> s_fetchRecs;
std::vector<const FetchList*> s_drawnPrograms;  // this render's
CheckTotals s_check = {};
u32 s_checkRenders = 0;
u64 s_recordDigest = 14695981039346656037ull;   // recordDigest()

// This render's light sets, shared as the Wii's elan_gx_record shares them:
// same key as the last list, or a copy among the last kDedupDepth (4) sets.
std::vector<ElanGxLightSet> s_renderSets;
u32 s_lastSetKey = 0;

void lightSetNote(const ElanGxRecord& r)
{
    if (r.lightKey && r.lightKey == s_lastSetKey)
        return;
    s_lastSetKey = r.lightKey;
    const size_t n = s_renderSets.size();
    for (size_t k = 0; k < 4 && k < n; k++)
        if (!memcmp(&s_renderSets[n - 1 - k], &r.lights, sizeof(r.lights)))
            return;
    s_renderSets.push_back(r.lights);
}

void fetchNote(const ElanGxRecord& r)
{
    const u8* ram = naomi_ElanRam();
    const u32 off = (u32)((const u8*)r.vtx - ram);
    if (!ram || off >= naomi_ElanRamSize())
        return;
    s_fetchRecs.push_back({off, r.count, r.vsize, r.colorFromVertex});
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
    s_lastSetKey = 0;
    takeWrites();                               // up to the draw, for pages not checked yet this render

    static std::vector<Tri3> fromProgram, fromCpu;
    for (const FetchRec& r : s_fetchRecs)
    {
        s_check.vertices += r.count;
        s_check.lists++;
        if (r.color)
        {
            s_check.color += r.count;
            continue;
        }
        const u32* vtx = (const u32*)(ram + r.off);
        const u32 patchesBefore = s_fetchCounts.patchRecords;
        FetchOutcome how;
        const FetchList* e = fetchProgram(vtx, r.off, r.count, r.vsize, how);
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
    }
    s_fetchRecs.clear();
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
        printf("[run]   GPU fetch per drawn render: %.0f vertices in %.0f lists | from programs %.0f (%.0f%%), "
               "built now %.0f (%.0f%%), pages written since the last render %.0f (%.0f%%), colour lists %.0f, "
               "other CPU-fed %.0f | patched records %.0f | programs drawn take %.0f of the ring's %u KB | "
               "light sets %.1f (most %u)\n",
               s_check.vertices / d, s_check.lists / d, s_check.fetched / d, 100.0 * s_check.fetched / v,
               s_check.built / d, 100.0 * s_check.built / v, s_check.written / d, 100.0 * s_check.written / v,
               s_check.color / d, s_check.other / d, s_check.patchVertices / d, s_check.liveWords * 2 / 1024.0 / d,
               s_progWords * 2 / 1024, s_check.lightSets / d, s_check.lightSetsMax);
        printf("[run]   GPU fetch check: %u draws compared with the CPU draw, %u MISMATCHES, %u ring overruns | "
               "programs so far: %u built, %u patches (%.1f records each), %u ring passes, %u too big, %u aborted, "
               "%u stale caught, %u table restarts\n",
               s_check.draws, s_check.mismatches, s_check.overruns, c.builds, c.patches,
               c.patches ? (double)c.patchRecords / c.patches : 0.0, c.wraps, c.tooBig, c.aborted, c.staleCaught,
               c.resets);
        if (s_check.boxAsked)
            printf("[run]   3D list boxes: %u lists asked, %u had a box (%.0f%%), every box checked against the "
                   "list's records: %u STALE\n",
                   s_check.boxAsked, s_check.boxes, 100.0 * s_check.boxes / s_check.boxAsked, s_check.boxStale);
        printf("[run]   record digest at render %u: %016llX\n", s_render, (unsigned long long)s_recordDigest);
        s_check = {};
        s_checkRenders = 0;
    }
}

bool elan_gx_list_box(const u32* vtx, u32 count, u32 vsize, float box[6])
{
    u8* ram = naomi_ElanRam();
    const u32 size = naomi_ElanRamSize();
    if (!ram || !g_elan_gx_preset || size > kElanBytes || !fetchCheckInit(size))
        return false;
    const u32 off = (u32)((const u8*)vtx - ram);
    if (off >= size)
        return false;
    s_check.boxAsked++;
    if (!cachedBox(off, count, vsize, box))
        return false;
    s_check.boxes++;
    // Built from the records at an earlier render, a box is only good while
    // they are unchanged: every record must still lie inside it.
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
                       off, count, i, p[0], p[1], p[2], box[0], box[1], box[2], box[3], box[4], box[5]);
            break;
        }
    }
    return true;
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
    return rend_AddElanItem(r.pcw, r.isp, r.tsp, r.tcw, 0, r.nearW, r.farW);
}

#else

#include <gccore.h>
#include <malloc.h>
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
}

} // namespace

bool elan_gx_enabled()
{
    return g_elan_gx_preset && allocate() && s_nItems < s_capItems;
}

void elan_gx_frame_reset()
{
    s_render++;
    s_vtxCacheStale = true;
    s_nItems = s_nXforms = s_nLsets = 0;
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
    const int xr = r.xformKey && r.xformKey == s_lastXformKey ? (int)s_lastXi : findRecent(s_xforms, s_nXforms, r.xform);
    const int lr = r.lightKey && r.lightKey == s_lastLightKey ? (int)s_lastLi : findRecent(s_lsets, s_nLsets, r.lights);
    if ((xr < 0 && s_nXforms >= s_capXforms) || (lr < 0 && s_nLsets >= s_capLsets))
        return false;
    u32 xi, li;
    if (xr >= 0)
        xi = (u32)xr;
    else
    {
        s_xforms[s_nXforms] = r.xform;
        xi = s_nXforms++;
    }
    if (lr >= 0)
        li = (u32)lr;
    else
    {
        s_lsets[s_nLsets] = r.lights;
        li = s_nLsets++;
    }
    s_lastXformKey = r.xformKey;
    s_lastXi = xi;
    s_lastLightKey = r.lightKey;
    s_lastLi = li;
    if (!rend_AddElanItem(r.pcw, r.isp, r.tsp, r.tcw, s_nItems, r.nearW, r.farW))
        return false;
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
    bool envTexgen;             // TEXCOORD0 generated from the normal
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
// below) instead of taking them from the FIFO. Never with colour.
constexpr u32 kFmtFetch = 16;

void setFormat(u32 key)
{
    const u8 src = (key & kFmtFetch) ? GX_INDEX16 : GX_DIRECT;
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
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
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
    struct mallinfo mi = mallinfo();
    printf("[elan] GPU vertex fetch: on%s (%u KB of tables, %u KB of them for programs; heap %u KB free inside "
           "malloc, MEM2 arena %u KB left)\n",
           s_fetchNoPatches ? ", no patches" : "",
           (u32)((kElanPages * sizeof(PageState) + kFetchSlots * sizeof(FetchList) + s_progWords * 2) >> 10),
           s_progWords * 2 >> 10, (u32)mi.fordblks >> 10,
           (u32)((u8*)SYS_GetArena2Hi() - (u8*)SYS_GetArena2Lo()) >> 10);
}

void setArrays(const Item& it, u32 pa, u32 fmtKey)
{
    // GX_SetArray takes a cached virtual address and keeps its physical part.
    const u8 stride = it.vsize;
    GX_SetArray(GX_VA_POS, (void*)(0x80000000u | (pa + 4)), stride);
    if (fmtKey & 8)                             // float normal: words 4-6; S8: header bytes z y x
        GX_SetArray(GX_VA_NRM, (void*)(0x80000000u | (pa + ((fmtKey & 4) ? 16 : 1))), stride);
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
    else
    {
        wgPipe->U32 = k << 16 | k;
        wgPipe->U16 = (u16)k;
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
    const FetchList* e = fetchProgram(it.vtx, (u32)it.vtx - WII_GUEST_ELAN_EA, it.count, it.vsize, how);
    if (how == kFetchWritten)
        g_fp_elan_wrote += it.count;
    noteFetchEvents();
    if (!e)
        return false;
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
    const u32 attrs = 1 + ((fmtKey >> 3) & 1) + (fmtKey & 1);
    if (attrs == 1)
        drawProgram<1>(it, *e, fmtKey, alphaOr);
    else if (attrs == 2)
        drawProgram<2>(it, *e, fmtKey, alphaOr);
    else
        drawProgram<3>(it, *e, fmtKey, alphaOr);
    return true;
}

// gxRend's canvas, as the last item drawn saw it.
float s_canvasW = 0.f, s_canvasH = 0.f;

} // namespace

bool elan_gx_list_box(const u32* vtx, u32 count, u32 vsize, float box[6])
{
    return s_fetchOn && cachedBox((u32)vtx - WII_GUEST_ELAN_EA, count, vsize, box);
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

static void drawItem(u32 index, const ElanGxFrame& f, bool forceOpaqueAlpha);

static void prefetchAhead(u32 index);

void elan_gx_draw(u32 index, const ElanGxFrame& f, bool forceOpaqueAlpha)
{
    // PROFILER: [HW] line, the ELAN share of a render on the CPU.
    FP_T0(t);
    prefetchAhead(index);
    s_canvasW = f.width;
    s_canvasH = f.height;
    drawItem(index, f, forceOpaqueAlpha);
    if (FRAME_PROF_ON())
    {
        g_fp_elan_ticks += FP_TICKS32() - t;
        if (index < s_nItems)
        {
            g_fp_elan_verts += s_items[index].count;
            g_fp_elan_items++;
        }
    }
}

// The renderer draws the items mostly in recorded order, and each costs a few
// cache misses in our own tables before a vertex moves: the item, its
// matrices, its list slot, its program, its page state. They are asked for
// one to three items ahead. (These tables are BAT-mapped, where dcbt works;
// on the paged guest memory it is dropped at every TLB miss.)
static void prefetchAhead(u32 index)
{
#if defined(__GNUC__)
    if (index + 3 < s_nItems)
        __builtin_prefetch(&s_items[index + 3]);
    if (index + 2 < s_nItems && s_fetchOn)
    {
        const Item& n2 = s_items[index + 2];
        const u32 off = (u32)n2.vtx - WII_GUEST_ELAN_EA;
        __builtin_prefetch(&s_lists[listHash(off + 1, n2.count)]);
        if (off < kElanBytes)
            __builtin_prefetch(&s_pages[off >> 12]);
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
            const FetchList& e = s_lists[listHash(key, n1.count)];
            if (e.key == key && e.count == n1.count)
            {
                __builtin_prefetch(s_prog + e.prog);
                __builtin_prefetch(s_prog + e.prog + 16);
            }
        }
    }
#else
    (void)index;
#endif
}

static void drawItem(u32 index, const ElanGxFrame& f, bool forceOpaqueAlpha)
{
    if (index >= s_nItems)
        return;
    const Item& it = s_items[index];
    const ElanGxXform& x = s_xforms[it.xform];
    const ElanGxLightSet& ls = s_lsets[it.lights];
    if (it.count < 3)
        return;

    if (!s_run.active)
    {
        // First item of a run: everything is gxRend's.
        s_run.active = true;
        s_run.format = s_run.chan = 0xFFFFFFFFu;
        s_run.envTexgen = false;
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
        const float s = (sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]) + sqrtf(l[3] * l[3] + l[4] * l[4] + l[5] * l[5]) +
                         sqrtf(l[6] * l[6] + l[7] * l[7] + l[8] * l[8])) / 3.f;
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
        const u32 mat = rgba(it.base, forceOpaqueAlpha ? (u8)255 : it.base[3]);
        if (!s_run.matValid || mat != s_run.matColor)
        {
            GX_SetChanMatColor(GX_COLOR0A0, toColor(mat));
            s_run.matColor = mat;
            s_run.matValid = true;
        }
    }
    if (ls.lit)
    {
        const u8 amb[3] = {toByte(ls.ambient[0]), toByte(ls.ambient[1]), toByte(ls.ambient[2])};
        const u32 a = rgba(amb, 255);
        if (!s_run.ambValid || a != s_run.ambColor)
        {
            GX_SetChanAmbColor(GX_COLOR0A0, toColor(a));
            s_run.ambColor = a;
            s_run.ambValid = true;
        }
    }

    const u32 fmtKey = (hasNormal ? 8u : 0u) | (hasNormal && floatNormal ? 4u : 0u) | (hasColor ? 2u : 0u) |
                       (hasUV ? 1u : 0u);
    const u32 alphaOr = forceOpaqueAlpha ? 0xFF000000u : 0u;
    if (!hasColor && s_fetchOn && fetchDraw(it, fmtKey, alphaOr))
        return;
    useFormat(fmtKey);
    kDraw[fmtKey](it, 0, it.count, alphaOr, kEmit[fmtKey]);
}

#endif
