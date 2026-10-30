// GPU vertex fetch: a list's strips as a small program of index draws over
// the records where they lie (Wii: GX indexed arrays based at the records'
// physical frames, the CPU sending only indices). Shared by the Wii draw and
// the Windows runner, which checks every program it builds against the CPU
// draw (elan_gx_census_render). Included inside an anonymous namespace after
// elan_strips; the includer first defines
//   u32  fetchPagePhys(u32 page, int* written)  frame of ELAN page `page` (0:
//        not resident); *written: written or moved since its last call
//   u32  fetchRingPhys(const void* p)            physical address in the ring
//   void fetchFlush(const void* p, u32 bytes)    CPU writes -> memory the GPU reads
// and allocates s_pages, s_lists and s_prog (s_progWords words, 32-byte aligned).
//
// A program stays valid while none of its list's pages is written or moved
// (checked once a render). The GPU needs a triangle's three records in one run
// of physically adjacent frames, and the pager hands out 4 KB frames: a
// program switches array bases between runs, and the triangles over a gap
// between two runs (at most 3 of a strip, 5 records) get their records copied
// into the program itself, a patch the GPU reads there. Strips with fan
// vertices over a gap are CPU-fed.
//
// After its ops (from a 4-byte boundary) each program keeps its list's
// bounding box: min x y z, max x y z of the records' positions (placeholders
// aside), 6 floats. Same life as the program, and its checksum covers it: the
// ELAN side culls and sizes the list's depth range from it (cachedBox).
//
// Program ops (u16):
//   kOpSeg, pa >> 16, pa & 0xFFFF       array base: vertex b's record is at
//                                       physical pa; the indices that follow
//                                       count from b
//   kOpPatch, n, [pad,] n words         array base: these copied records
//                                       (4-byte aligned in the ring)
//   kOpDirect, i, j                     the strips of vertices [i, j), CPU-fed
//   n (3..kMaxRun), s                   strip of the indices s, s+1, ..., s+n-1
//   kOpRunOdd | n, s                    the same with s sent twice first: the
//                                       part of a strip from an odd triangle
//                                       on (keeps the triangles' winding)
//   kOpList | n, n indices              strip of those indices
constexpr u16 kOpSeg = 0xFFFF, kOpPatch = 0xFFFE, kOpDirect = 0xFFFD;
constexpr u16 kOpList = 0x4000, kOpRunOdd = 0x8000;
constexpr u32 kMaxRun = 0x3FFF;

constexpr u32 kElanBytes = 32u << 20;
constexpr u32 kElanPages = kElanBytes >> 12;
constexpr u32 kFetchSlots = 4096;              // list table, power of two
constexpr u32 kFetchMaxLists = 3072;           // then it starts over
// Program ring: a race's programs and patches take ~215 KB when every page
// boundary is a gap (Windows check, IDAS3_FETCH_SCATTER=100), less as frames
// come adjacent; the room to spare keeps new lists from pushing out ones
// still in use.
constexpr u32 kProgWordsMax = 128 * 1024, kProgWordsMin = 64 * 1024;
constexpr u32 kListPages = 512;                // pages a fetched list may span
constexpr u32 kBoxWords = 12;                  // the box after a program, in u16 units
constexpr u32 kSumWindow = 32;                 // a cached program's checksum: one render in this many

struct PageState
{
    u32 pa;                 // frame at the last check, 0 = not resident
    u32 checked;            // render of the last check
    u32 written;            // render a write (or a new frame) was last seen at
};

struct FetchList
{
    u32 key;                // ELAN offset of the records + 1, 0 = free slot
    u32 gen;                // ring pass its program was written in
    u32 built;              // render it was built at
    u32 prog, len;          // its program in s_prog
    u32 sum;                // progSum of the program when built
    u16 count;
    u8 vsize;
    u8 valid;               // a program was built (and not given up since)
};

PageState* s_pages = nullptr;
FetchList* s_lists = nullptr;
u16* s_prog = nullptr;
u32 s_progWords = 0, s_progHead = 0, s_progGen = 1, s_nLists = 0;
u32 s_render = 1;                              // renders counted by the includer
bool s_fetchPatched = false;                   // patch data written (the includer clears it)
bool s_fetchNoPatches = false;                 // strips over a gap: CPU-fed instead
u16 s_runHead[kListPages];                     // list being built: first page of each page's run
u16 s_join[kMaxRun];                           // a fan strip's joined indices

struct FetchCounts
{
    u32 builds, patches, patchRecords, aborted, tooBig, wraps, staleCaught, resets;
    u32 bounds, gaps;       // page boundaries inside lists built, and those between frames apart
};
FetchCounts s_fetchCounts = {};
// The first cached program found changed under its list (a ring bookkeeping
// bug; caught by its checksum and rebuilt): for the includer's log.
struct StaleInfo
{
    u32 key, count, gen, prog, len, head, ringGen;
};
StaleInfo s_staleFirst = {};

// Checks ELAN pages [first, last] (once a render each). False when one is not
// resident or was written since the previous render; `newest` = the latest
// render a write to them was seen at.
bool pagesSettled(u32 first, u32 last, u32& newest)
{
    bool settled = true;
    newest = 0;
    for (u32 p = first; p <= last; p++)
    {
        PageState& s = s_pages[p];
        if (s.checked != s_render)
        {
            int written;
            const u32 pa = fetchPagePhys(p, &written);
            if (written || pa != s.pa)
                s.written = s_render;
            s.pa = pa;
            s.checked = s_render;
        }
        if (!s.pa || s.written == s_render)
            settled = false;
        if (s.written > newest)
            newest = s.written;
    }
    return settled;
}

inline u32 listHash(u32 key, u32 count)
{
    return ((key >> 2) * 2654435761u ^ count * 40503u) >> 20;
}

FetchList* findList(u32 key, u32 count, u32 vsize, bool& found)
{
    const u32 h = listHash(key, count);
    for (u32 k = 0; k < kFetchSlots; k++)
    {
        FetchList& e = s_lists[(h + k) & (kFetchSlots - 1)];
        if (!e.key || (e.key == key && e.count == count && e.vsize == vsize))
        {
            found = e.key != 0;
            return &e;
        }
    }
    found = false;
    return nullptr;                             // cannot happen: kFetchMaxLists < kFetchSlots
}

// Written in this ring pass, or in the previous one past the head.
inline bool programValid(const FetchList& e)
{
    return e.valid && (e.gen == s_progGen || (e.gen + 1 == s_progGen && e.prog >= s_progHead));
}

inline u32 progSum(const u16* p, u32 n)
{
    u32 s = n;
    for (u32 k = 0; k < n; k++)
        s = (s << 5 | s >> 27) + p[k];
    return s;
}

// Where a program's box lies, and the words its program and box take.
inline u32 boxPos(u32 prog, u32 len)
{
    return prog + ((len + 1) & ~1u);
}
inline u32 progSpan(const FetchList& e)
{
    return boxPos(e.prog, e.len) + kBoxWords - e.prog;
}

// One list's program, written at the ring's head.
struct ProgramBuilder
{
    const u32* vtx;
    u32 off, vs, words;
    u16* out;
    u32 m;                  // words written
    int curRun;             // run of the array base set last, -1: none (or a patch)
    u32 curBase;            // ...and its base vertex
    bool fail;

    bool reserve(u32 n)
    {
        if (s_progHead + m + n > s_progWords)
            fail = true;
        return !fail;
    }

    // The run (first page, relative) holding vertex v's whole record, -1 when
    // the record spans two runs.
    int run(u32 v) const
    {
        const u32 a = (off & 0xFFF) + v * vs;
        const u32 h = s_runHead[a >> 12];
        return h == s_runHead[(a + vs - 1) >> 12] ? (int)h : -1;
    }

    u32 phys(u32 v) const
    {
        const u32 a = off + v * vs;
        return s_pages[a >> 12].pa + (a & 0xFFF);
    }

    void seg(int r, u32 lo, u32 hi)
    {
        if (curRun == r && lo >= curBase && hi - curBase <= 0xFFFE)
            return;
        if (!reserve(3))
            return;
        const u32 pa = phys(lo);
        out[m++] = kOpSeg;
        out[m++] = (u16)(pa >> 16);
        out[m++] = (u16)pa;
        curRun = r;
        curBase = lo;
    }

    void runOp(u32 s, u32 n, bool odd)
    {
        if (!reserve(2))
            return;
        out[m++] = (u16)((odd ? kOpRunOdd : 0) | n);
        out[m++] = (u16)s;
    }

    void direct(u32 i, u32 j)
    {
        if (!reserve(3))
            return;
        out[m++] = kOpDirect;
        out[m++] = (u16)i;
        out[m++] = (u16)j;
    }

    // n vertices, all in run r: idx[0..n), or s, s+1, ... when idx is null.
    void piece(int r, const u16* idx, u32 s, u32 n, bool odd)
    {
        u32 lo = s, hi = s + n - 1;
        if (idx)
        {
            lo = hi = idx[0];
            for (u32 k = 1; k < n; k++)
            {
                lo = idx[k] < lo ? idx[k] : lo;
                hi = idx[k] > hi ? idx[k] : hi;
            }
        }
        seg(r, lo, hi);
        if (!idx)
        {
            runOp(s - curBase, n, odd);
            return;
        }
        const u32 len = n + (odd ? 1 : 0);
        if (!reserve(1 + len))
            return;
        out[m++] = (u16)(kOpList | len);
        if (odd)
            out[m++] = (u16)(idx[0] - curBase);
        for (u32 k = 0; k < n; k++)
            out[m++] = (u16)(idx[k] - curBase);
    }

    // Vertices s .. s+n-1 copied into the program (4-byte aligned: s_prog
    // is) and drawn from there.
    void patch(u32 s, u32 n, bool odd)
    {
        const u32 dataWords = n * vs / 2;       // u16 units
        const u32 dataPos = (s_progHead + m + 2 + 1) & ~1u;
        if (dataWords > 0xFFFF || dataPos + dataWords + 2 > s_progWords)
        {
            fail = true;
            return;
        }
        out[m++] = kOpPatch;
        out[m++] = (u16)dataWords;
        if (dataPos != s_progHead + m)
            out[m] = 0;                         // padding
        u32* const dst = (u32*)(s_prog + dataPos);
        const u32* src = vtx + s * words;
        for (u32 k = 0; k < n * words; k++)
            dst[k] = src[k];
        fetchFlush(dst, n * vs);
        m = dataPos + dataWords - s_progHead;
        runOp(0, n, odd);
        curRun = -1;
        s_fetchPatched = true;
        s_fetchCounts.patches++;
        s_fetchCounts.patchRecords += n;
    }

    // Strip triangle q of vertices v(q), v(q+1), v(q+2): the run holding all
    // three, or -1.
    int triRun(u32 a, u32 b, u32 c) const
    {
        const int r = run(a);
        return r >= 0 && run(b) == r && run(c) == r ? r : -1;
    }

    // The strip of vertices [i, j) (s_join[0..k) when fan, else i, i+1, ...).
    void strip(u32 i, u32 j, bool fan, u32 k)
    {
        const u16* idx = fan ? s_join : nullptr;
        const u32 n = fan ? k : j - i;
        const int r = run(i);
        if (r >= 0 && run(j - 1) == r)
        {
            piece(r, idx, i, n, false);         // one run: the usual case
            return;
        }
        if (fan || s_fetchNoPatches)
        {
            direct(i, j);
            return;
        }
        // Over gaps: pieces of consecutive triangles in one run, patches for
        // those whose records lie in two.
        for (u32 q = 0; q + 2 < n && !fail;)
        {
            const u32 q0 = q;
            const int t = triRun(i + q, i + q + 1, i + q + 2);
            do
                q++;
            while (q + 2 < n && triRun(i + q, i + q + 1, i + q + 2) == t);
            if (t >= 0)
                piece(t, nullptr, i + q0, q - q0 + 2, (q0 & 1) != 0);
            else
                patch(i + q0, q - q0 + 2, (q0 & 1) != 0);
        }
    }
};

// Builds list e's program at the ring's head. False: the list is drawn
// directly this time (too many pages, or no room even in an empty ring).
bool buildProgram(const u32* vtx, u32 off, u32 count, u32 vs, FetchList& e)
{
    using namespace elan_strips;
    const u32 first = off >> 12, np = ((off + count * vs - 1) >> 12) - first + 1;
    if (np > kListPages)
    {
        s_fetchCounts.tooBig++;
        return false;
    }
    u32 gaps = 0;
    s_runHead[0] = 0;
    for (u32 k = 1; k < np; k++)
    {
        const bool adjacent = s_pages[first + k].pa == s_pages[first + k - 1].pa + 4096;
        s_runHead[k] = adjacent ? s_runHead[k - 1] : (u16)k;
        gaps += !adjacent;
    }
    s_fetchCounts.bounds += np - 1;
    s_fetchCounts.gaps += gaps;
    // Ops: 7 words a vertex at most; each gap: one patch of 5 records at
    // most, its alignment and the ops around it. Then the box.
    const u32 need = 7 * count + 32 + kBoxWords + gaps * (48 + 3 * vs);
    if (need > s_progWords / 2)
    {
        s_fetchCounts.tooBig++;
        return false;
    }
    if (s_progHead + need > s_progWords)
    {
        s_progHead = 0;                         // older programs past the head stay valid until overwritten
        s_progGen++;
        s_fetchCounts.wraps++;
    }
    ProgramBuilder b = {vtx, off, vs, vs / 4, s_prog + s_progHead, 0, -1, 0, false};
    for (u32 i = 0; i < count && !b.fail;)
    {
        if (placeholder(vtx + i * b.words))
        {
            i++;
            continue;
        }
        bool fan;
        const u32 j = stripEnd(vtx, b.words, i, count, fan);
        if (fan)
        {
            const u32 k = joinedIndices(vtx, b.words, i, j, s_join, kMaxRun);
            if (k >= kMaxRun)
                b.direct(i, j);
            else if (k >= 3)
                b.strip(i, j, true, k);
        }
        else if (j - i >= kMaxRun)
            b.direct(i, j);
        else if (j - i >= 3)
            b.strip(i, j, false, 0);
        i = j;
    }
    const u32 box = boxPos(s_progHead, b.m);
    if (b.fail || box + kBoxWords > s_progWords)
    {
        s_fetchCounts.aborted++;
        return false;
    }
    // The box, from the records just walked (still in the cache).
    float bb[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    for (u32 i = 0; i < count; i++)
    {
        const u32* v = vtx + i * b.words;
        if (placeholder(v))
            continue;
        for (int c = 0; c < 3; c++)
        {
            float f;
            memcpy(&f, &v[1 + c], 4);
            bb[c] = f < bb[c] ? f : bb[c];
            bb[3 + c] = f > bb[3 + c] ? f : bb[3 + c];
        }
    }
    if (b.m & 1)
        b.out[b.m] = 0;                         // padding, under the checksum
    memcpy(s_prog + box, bb, sizeof(bb));
    e.prog = s_progHead;
    e.len = b.m;
    e.gen = s_progGen;
    e.built = s_render;
    e.sum = progSum(b.out, box + kBoxWords - s_progHead);
    e.valid = 1;
    // Next program right after (on 4 bytes, for its box and patches): lining
    // programs up on cache lines took ~15% more of the ring, which the in-car
    // view (the mirror's lists too) then overran, rebuilding live programs.
    s_progHead = box + kBoxWords;
    s_fetchCounts.builds++;
    return true;
}

enum FetchOutcome
{
    kFetchCached,           // drawn from its program
    kFetchBuilt,            // program built now, then drawn from it
    kFetchWritten,          // a page written since the previous render (or not resident): CPU-fed
    kFetchNone              // CPU-fed: not in ELAN RAM, too big, no room
};

// The list's program, or null: draw the list directly. Builds it when there is
// none or its pages were written since.
const FetchList* fetchProgram(const u32* vtx, u32 off, u32 count, u32 vs, FetchOutcome& how)
{
    how = kFetchNone;
    const u32 bytes = count * vs;
    if (!s_prog || !count || off >= kElanBytes || bytes > kElanBytes - off)
        return nullptr;
    u32 newest;
    if (!pagesSettled(off >> 12, (off + bytes - 1) >> 12, newest))
    {
        how = kFetchWritten;
        return nullptr;
    }
    const u32 key = off + 1;
    bool found;
    FetchList* e = findList(key, count, vs, found);
    if (!found && s_nLists >= kFetchMaxLists)
    {
        memset(s_lists, 0, kFetchSlots * sizeof(FetchList));
        s_nLists = 0;
        s_fetchCounts.resets++;
        e = findList(key, count, vs, found);
    }
    if (!e)
        return nullptr;
    if (!found)
    {
        e->key = key;
        e->count = (u16)count;
        e->vsize = (u8)vs;
        e->valid = 0;
        s_nLists++;
    }
    else if (newest <= e->built && programValid(*e))
    {
        // A slice of the lists each render: this safety net has never fired on
        // a Wii, and summing every program read the whole ring (~0.3 ms).
        if (((key >> 4) + s_render) % kSumWindow != 0 || progSum(s_prog + e->prog, progSpan(*e)) == e->sum)
        {
            how = kFetchCached;
            return e;
        }
        if (!s_fetchCounts.staleCaught++)
            s_staleFirst = {key, count, e->gen, e->prog, e->len, s_progHead, s_progGen};
    }
    if (!buildProgram(vtx, off, count, vs, *e))
    {
        e->valid = 0;
        return nullptr;
    }
    how = kFetchBuilt;
    return e;
}

// The ELAN side, when it records a list (before this render's draw): the box
// kept with the list's program, if the program is still good. Checks the
// list's pages for this render, which the draw then does not repeat; a page
// written since the program was built makes the list go without a box (and
// be drawn directly), as at the draw. Never builds anything.
bool cachedBox(u32 off, u32 count, u32 vs, float* box)
{
    const u32 bytes = count * vs;
    if (!s_prog || !count || off >= kElanBytes || bytes > kElanBytes - off)
        return false;
    bool found;
    const FetchList* e = findList(off + 1, count, vs, found);
    if (!found || !programValid(*e))
        return false;
    u32 newest;
    if (!pagesSettled(off >> 12, (off + bytes - 1) >> 12, newest) || newest > e->built)
        return false;
    memcpy(box, s_prog + boxPos(e->prog, e->len), 6 * sizeof(float));
    return true;
}

// Whether ELAN RAM [off, off + bytes) saw no write since render `since`. Checks
// its pages for this render, like cachedBox.
bool unwrittenSince(u32 off, u32 bytes, u32 since)
{
    if (!s_prog || !bytes || off >= kElanBytes || bytes > kElanBytes - off)
        return false;
    u32 newest;
    return pagesSettled(off >> 12, (off + bytes - 1) >> 12, newest) && newest <= since;
}

// Walks a program: sink.base(pa, patch), sink.run(s, n, odd),
// sink.list(indices, n), sink.direct(i, j).
template <class Sink>
inline void runProgram(const u16* p, const u16* end, Sink& sink)
{
    while (p < end)
    {
        const u32 op = *p++;
        if (op >= kOpDirect)
        {
            if (op == kOpDirect)
            {
                sink.direct(p[0], p[1]);
                p += 2;
            }
            else if (op == kOpPatch)
            {
                const u32 n = p[0];
                const u16* data = (const u16*)(((uintptr_t)(p + 1) + 3) & ~(uintptr_t)3);
                sink.base(fetchRingPhys(data), true);
                p = data + n;
            }
            else
            {
                sink.base((u32)p[0] << 16 | p[1], false);
                p += 2;
            }
        }
        else if (op & kOpRunOdd)
        {
            sink.run(p[0], op & kMaxRun, true);
            p++;
        }
        else if (op & kOpList)
        {
            const u32 n = op & kMaxRun;
            sink.list(p, n);
            p += n;
        }
        else
        {
            sink.run(p[0], op, false);
            p++;
        }
    }
}
