// NAOMI 2 ELAN command processor.
//
// The SH-4 drives the ELAN with 32-byte commands, either written to the
// command FIFO (area 2, 0x09000000) or stored in ELAN RAM as display lists
// that a FIFO "link" command points at. A command stream mixes two kinds of
// 32-byte parameters, told apart by bit 27 of the first word:
//   0  ordinary PowerVR TA parameters (polygon/sprite headers, vertices, end
//      of list): the ELAN forwards them to the CLX2 tile accelerators, here
//      the PVR's TA input (libPvr_TaDMA), so 2D drawing works unchanged.
//   1  NAOMI 2 commands, kind in bits 8-11:
//        3 projection     4 instance matrix / light model / light
//        5 material (GMP) 7 polygon list (ICH, untransformed vertices)
//        8 model (call a list in ELAN RAM)    E register wait
//        F link: follow a list in ELAN RAM, or DMA a texture into VRAM
//
// Formats are those documented by Flycast's NAOMI 2 work (elan_struct.h);
// this is an independent implementation. State commands and polygon lists
// are handed to the transform & lighting code (naomi_elan_tnl.cpp).
#include "types.h"
#include "dc/asic/asic.h"
#include "dc/mem/sh4_mem.h"
#include "dc/sh4/dmac.h"
#include "plugins/plugin_manager.h"
#include "naomi.h"
#include "naomi_elan.h"
#include "naomi_elan_gx.h"

#include <cmath>
#include <cstring>

#if !defined(BUILD_NAOMI2)
#error "naomi_elan_cmd.cpp is only part of the NAOMI 2 build"
#endif

extern "C" {
int g_elan_model_skip_preset = 1;
int g_mirror_preset = 1;
int g_elan_prefetch_preset = 1;
int g_elan_model_prefetch_preset = 1;
int g_cache_hints_preset = 0;        // CACHE HINTS (the launcher's default is ON)
}

#if HOST_OS != OS_WII
// Runner (env IDAS3_MODELS, run_main.cpp): the ELAN offset of every model the
// game calls, while on.
bool g_elan_model_census_on = false;
u32 g_elan_model_census[4096];
u32 g_elan_model_census_count = 0;
u32 g_elan_link_census[4096];        // lists followed (N2 link targets), depth << 28 | offset
u32 g_elan_link_census_count = 0;
#define MODEL_CENSUS(off)                                                                                      \
    do                                                                                                         \
    {                                                                                                          \
        if (g_elan_model_census_on && g_elan_model_census_count < 4096)                                        \
            g_elan_model_census[g_elan_model_census_count++] = (off);                                          \
    } while (0)
#define LINK_CENSUS(off, depth)                                                                                \
    do                                                                                                         \
    {                                                                                                          \
        if (g_elan_model_census_on && g_elan_link_census_count < 4096)                                         \
            g_elan_link_census[g_elan_link_census_count++] = (u32)(depth) << 28 | (off);                       \
    } while (0)
#else
#define MODEL_CENSUS(off) do { } while (0)
#define LINK_CENSUS(off, depth) do { } while (0)
#endif

namespace {

// TA parameter types (first word, bits 31-29)
enum
{
    TA_END_OF_LIST = 0,
    TA_USER_CLIP = 1,
    TA_OBJECT_LIST_SET = 2,
    TA_POLYGON = 4,
    TA_SPRITE = 5,
    TA_VERTEX = 7,
};

// NAOMI 2 commands (bits 11-8)
enum
{
    N2_NULL = 0x0,
    N2_PROJECTION = 0x3,
    N2_MATRIX_OR_LIGHT = 0x4,
    N2_GMP = 0x5,
    N2_ICH = 0x7,
    N2_MODEL = 0x8,
    N2_REGISTER_WAIT = 0xE,
    N2_LINK = 0xF,
};

constexpr u32 kMaxDepth = 64;          // display lists calling lists
// Games chain display lists: a buffer's last command is a Link to the next
// buffer (Initial D 3's attract demo goes well past 16 deep). Such a link is
// followed as a jump, not a call; the cap only stops a corrupt circular chain.
constexpr u32 kMaxChain = 65536;
constexpr u32 kDmaCycles = 512;        // texture DMA latency before status bit 0
constexpr u32 kPrefetchAhead = 256;    // ELAN PREFETCH: bytes asked for ahead of the walk

// Where the TA stream stands across commands: the vertex size depends on
// the last polygon header, and a 64-byte parameter may straddle two FIFO
// commands.
struct TaStream
{
    int listType = -1;
    u32 vertexSize = 32;
    bool secondHalfPending = false;
};
TaStream s_ta;

inline u32 word(const u8* p, u32 index)
{
    return ((const u32*)p)[index];
}

// ---- Model lists on dropped frames ------------------------------------------
// A frame the frameskip drops only needs what its walk leaves behind: list-end
// interrupts, texture DMAs, TA parameters (2D), the projection, the TA stream
// state. The model lists a race frame calls (N2 MODEL: cars, track pieces)
// hold none of those - matrices, materials and polygon lists only - yet
// walking them is most of a dropped frame's ELAN time on a Wii: each polygon
// list header is a cache miss, its vertices sit between two of them. So every
// model walked gets a summary, and on a dropped frame a model whose summary
// says "no side effects" is stepped over. Drawn frames walk everything and
// refresh the summaries, so one is at most a few dropped frames old; the
// skeleton hash counts models walked again with other commands at the same
// address (modelsChanged / modelsFlipped), the case this relies on being rare.
//
// Drawn frames (3D LIST CULL = VIEW + MODELS): a model's polygon lists lie
// inline in its block of ELAN RAM and it holds no matrices or lights (the
// per-frame list that calls it does), only materials. So its lists' boxes, from
// their GPU fetch programs, add up to one box under the matrix current at the
// call: wholly out of the picture, the model is stepped over before any of its
// commands is read, and its block's pages stand for all its lists. ~1400 lists
// a race frame cost ~4 us each to walk and record; ~23% are in such models.
struct ModelWalk
{
    u32 skel;           // hash of the command words that decide the walk
    u32 cmds, ich, verts, lists, states;
    bool pure;          // nothing but state commands, polygon lists, pure models
    bool boxOk;         // every list has a box; no matrix or light commands
    float box[6];       // their union (min x y z, max x y z)
    const u8* base;     // the block
    u32 lastGmp;        // offset of its last material + 1, 0: none
    u32 indexed;        // lists that took a depth slice (tnl_ListsIndexed)
    u16 heads[8];       // MODEL PREFETCH: its first command headers, 32-byte units
    u16 slots[8];       // CACHE HINTS: what they are (ModelHeads::slot)
    u32 nHeads;
};

// Two cache lines, the first enough to tell a model that stays visible.
struct alignas(32) ModelEntry
{
    u32 offset, size;
    float box[6];       // valid when boxed
    u32 skel, verts;
    u16 cmds, ich, lists, states;
    s8 typeIn, typeOut; // TA list type open before / after the walk
    u8 pure, used;
    u8 boxed;           // the block unwritten since boxRender: the box holds
    u16 indexed;
    u32 lastGmp, boxRender;
};
constexpr u32 kModelSlots = 2048;
ModelEntry s_models[kModelSlots];
// MODEL PREFETCH: where a model's first command headers were the last time it
// was walked (same slot as its summary; 0xFFFF ends the list). A model's
// headers sit between its vertices, each a cache miss on a Wii; asked for
// together at the model's call, they arrive while the first is handled. Only
// a hint: a block that changed is walked as before, from its own words.
constexpr u32 kModelHeads = 8;
// CACHE HINTS: with each head, what it is: a polygon list's GPU fetch slot + 1
// (naomi_elan_fetch.inc: what recording it reads, its table entry, page state
// and box, can be asked for ahead), kHeadMaterial (a 64-byte material: its
// second line too), or 0. One line a model, the heads with them.
constexpr u16 kHeadMaterial = 0xFFFF;
struct alignas(32) ModelHeads
{
    u16 at[kModelHeads];        // 0xFFFF ends the list
    u16 slot[kModelHeads];
};
ModelHeads s_modelHeads[kModelSlots];
ModelWalk* s_walk = nullptr;        // the innermost model list being walked
int s_dropLists = 0;                // inside a model MIRROR OFF does not draw

inline u32 skelMix(u32 h, u32 w)
{
    return (h ^ w) * 0x01000193u;
}

inline ModelEntry& modelSlot(u32 offset, u32 size)
{
    return s_models[(((offset >> 5) ^ size) * 0x9E3779B1u) >> 21];
}

inline void walkImpure()
{
    if (s_walk)
        s_walk->pure = false;
}

void mergeWalk(ModelWalk& outer, const ModelWalk& inner)
{
    outer.skel = skelMix(outer.skel, inner.skel);
    outer.cmds += inner.cmds;
    outer.ich += inner.ich;
    outer.verts += inner.verts;
    outer.lists += inner.lists + 1;
    outer.states += inner.states;
    outer.pure = outer.pure && inner.pure;
    outer.boxOk = false;                // no box across a nested model
}

// After a list of the model being walked was drawn.
void noteListBox(ModelWalk& w)
{
    float b[6];
    const int state = tnl_ListBox(b);
    if (state == kListNoBox)
        w.boxOk = false;
    else if (state == kListBoxed)
        for (int c = 0; c < 3; c++)
        {
            w.box[c] = b[c] < w.box[c] ? b[c] : w.box[c];
            w.box[3 + c] = b[3 + c] > w.box[3 + c] ? b[3 + c] : w.box[3 + c];
        }
}

void rememberModel(u32 offset, u32 size, const ModelWalk& w, int typeIn, int typeOut)
{
    NaomiElanStats& st = elan_Stats();
    st.modelsWalked++;
    ModelEntry& m = modelSlot(offset, size);
    const bool fits = w.cmds <= 0xFFFF && w.lists <= 0xFFFF && w.states <= 0xFFFF;
    const bool pure = w.pure && fits;
    if (m.used && m.offset == offset && m.size == size && m.skel != w.skel)
    {
        st.modelsChanged++;
        if ((m.pure != 0) != pure)
            st.modelsFlipped++;
    }
    m.offset = offset;
    m.size = size;
    m.skel = w.skel;
    m.verts = w.verts;
    m.cmds = (u16)w.cmds;
    m.ich = (u16)w.ich;
    m.lists = (u16)w.lists;
    m.states = (u16)w.states;
    m.typeIn = (s8)typeIn;
    m.typeOut = (s8)typeOut;
    m.pure = pure;
    m.used = 1;
    ModelHeads& heads = s_modelHeads[&m - s_models];
    for (u32 i = 0; i < kModelHeads; i++)
    {
        heads.at[i] = i < w.nHeads ? w.heads[i] : 0xFFFF;
        heads.slot[i] = i < w.nHeads ? w.slots[i] : 0;
    }
    m.boxed = pure && w.boxOk && w.ich && w.indexed <= 0xFFFF;
    if (m.boxed)
    {
        memcpy(m.box, w.box, sizeof(m.box));
        m.boxRender = elan_gx_render();
        m.lastGmp = w.lastGmp;
        m.indexed = (u16)w.indexed;
    }
}

// On a dropped frame: what walking the model at `offset` would have left
// behind, from its summary. False: walk it.
bool stepOverModel(const u8* data, u32 offset, u32 size)
{
    if (!g_elan_model_skip_preset || s_ta.secondHalfPending || !rend_NextFrameSkipped())
        return false;
    const ModelEntry& m = modelSlot(offset, size);
    if (!m.used || !m.pure || m.offset != offset || m.size != size || m.typeIn != s_ta.listType)
        return false;
    tnl_BeginModel((const u32*)data);
    tnl_EndModel();
    tnl_SkipModelState(m.states);
    s_ta.listType = m.typeOut;
    if (m.ich)
        s_ta.vertexSize = 32;
    NaomiElanStats& st = elan_Stats();
    st.lists += 1u + m.lists;
    st.n2Commands += m.cmds;
    st.ichLists += m.ich;
    st.ichVertices += m.verts;
    st.ichSkipped += m.ich;
    st.modelsSkipped++;
    st.modelListsSkipped += m.ich;
    if (s_walk)
    {
        ModelWalk w = {};
        w.skel = m.skel;
        w.cmds = m.cmds;
        w.ich = m.ich;
        w.verts = m.verts;
        w.lists = m.lists;
        w.states = m.states;
        w.pure = true;
        mergeWalk(*s_walk, w);
    }
    return true;
}

#if !defined(GEKKO)
inline bool isNaomi2(u32 pcw);
u32 ichVertexSize(u32 flags);

// Runner: walks a model about to be stepped over. Its block must hold what its
// summary says: every vertex drawn inside the box, the same last material, and
// nothing a skip would lose.
void checkHiddenModel(const u8* list, u32 listSize, const ModelEntry& m)
{
    u32 lastGmp = 0, ich = 0, verts = 0;
    bool bad = false;
    for (u32 at = 0; at + 32 <= listSize && !bad;)
    {
        const u8* p = list + at;
        const u32 pcw = word(p, 0);
        u32 used = 32;
        if (!isNaomi2(pcw))
            bad = true;
        else
            switch (pcw >> 8 & 0xF)
            {
            case N2_NULL:
                break;
            case N2_REGISTER_WAIT:
                bad = word(p, 1) != 0xFFFFFFFF && word(p, 3) != 0;
                break;
            case N2_GMP:
                used = 64;
                lastGmp = at + 1;
                break;
            case N2_ICH:
            {
                const u32 vsize = ichVertexSize(word(p, 6)), count = word(p, 7);
                used = 32 + vsize * count;
                ich++;
                verts += count;
                const int type = m.typeIn >= 0 ? m.typeIn : (int)(pcw >> 24 & 7);
                if (type & 1)
                    break;                      // modifier volumes are not drawn
                for (u32 i = 0; i < count && !bad; i++)
                {
                    float v[3];
                    memcpy(v, p + 32 + i * vsize + 4, sizeof(v));
                    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
                        continue;
                    for (int c = 0; c < 3; c++)
                        bad = bad || v[c] < m.box[c] || v[c] > m.box[3 + c];
                }
                break;
            }
            default:
                bad = true;
                break;
            }
        at += used;
    }
    if (lastGmp != m.lastGmp || ich != m.ich || verts != m.verts)
        bad = true;
    if (bad && elan_Stats().modelCheckStale++ < 8)
        naomi_Log("[elan] hidden model at ELAN +%X, %u bytes: its block no longer matches its summary\n", m.offset,
                  m.size);
}
#endif

const u8* elanRamPtr(u32 offset, u32& size);
void execute(const u8* data, u32 size, u32 depth);

// What walking a model none of whose lists is drawn leaves behind, from its
// summary: its last material (deferred), the list type, the counts.
void skipFromSummary(const u8* list, const ModelEntry& m, u32 indexed)
{
    tnl_ModelHidden(m.lastGmp ? (const u32*)(list + m.lastGmp - 1) : nullptr, indexed);
    s_ta.listType = m.typeOut;
    if (m.ich)
        s_ta.vertexSize = 32;
    NaomiElanStats& st = elan_Stats();
    st.lists += 1u + m.lists;
    st.n2Commands += m.cmds;
    st.ichLists += m.ich;
    st.ichVertices += m.verts;
    if (s_walk)
    {
        ModelWalk w = {};
        w.skel = m.skel;
        w.cmds = m.cmds;
        w.ich = m.ich;
        w.verts = m.verts;
        w.lists = m.lists;
        w.states = m.states;
        w.pure = true;
        mergeWalk(*s_walk, w);
    }
}

// On a drawn frame: the model is out of the picture and has left its block
// unwritten, so walking it would have dropped each of its lists. False: walk it.
bool hiddenModel(const u8* list, u32 listSize, u32 offset, u32 size)
{
    if (g_elan_box_preset != 3 || s_ta.secondHalfPending || rend_NextFrameSkipped() || s_dropLists)
        return false;
    const ModelEntry& m = modelSlot(offset, size);
    if (m.offset != offset || m.size != size || !tnl_BoxHidden(m.box) || !m.boxed || m.typeIn != s_ta.listType ||
        !elan_gx_unwritten_since(list, listSize, m.boxRender))
        return false;
#if !defined(GEKKO)
    checkHiddenModel(list, listSize, m);
#endif
    skipFromSummary(list, m, m.indexed);
    NaomiElanStats& st = elan_Stats();
    st.boxCulledVertices += m.verts;
    st.modelsCulled++;
    st.modelListsCulled += m.ich;
    return true;
}

// MIRROR OFF, a drawn frame: a model clipped inside the user clip rectangle
// (the in-car view's rear-view mirror: the scene again, from behind) is not
// drawn. Stepped over from its summary when its block is unchanged since it was
// walked (the mirror shows the main view's own models), else walked with its
// lists dropped (s_dropLists), which leaves every state exactly as drawing them
// would. Its lists take no depth slice either way.
bool mirrorModel(const u8* data, u32 offset, u32 size, u32 depth)
{
    if (g_mirror_preset == 1 || s_ta.secondHalfPending || rend_NextFrameSkipped())
        return false;
    u32 listSize = size;
    const u8* list = elanRamPtr(offset, listSize);
    if (!list)
        return false;
    NaomiElanStats& st = elan_Stats();
    st.mirrorModels++;
    const ModelEntry& m = modelSlot(offset, size);
    if (m.used && m.boxed && m.offset == offset && m.size == size && m.typeIn == s_ta.listType &&
        elan_gx_unwritten_since(list, listSize, m.boxRender))
    {
#if !defined(GEKKO)
        checkHiddenModel(list, listSize, m);
#endif
        skipFromSummary(list, m, 0);
        st.mirrorSummarised++;
        st.mirrorLists += m.ich;
        st.mirrorVertices += m.verts;
        return true;
    }
    st.lists++;
    ModelWalk* const outer = s_walk;
    ModelWalk walk = {};
    if (outer)
    {
        walk.skel = 0x811C9DC5u;
        walk.pure = !s_ta.secondHalfPending;
        s_walk = &walk;
    }
    s_dropLists++;
    tnl_BeginModel((const u32*)data);
    execute(list, listSize, depth + 1);
    tnl_EndModel();
    s_dropLists--;
    // Its summary is left as it was: lists not drawn have no box.
    if (outer)
    {
        s_walk = outer;
        mergeWalk(*outer, walk);
    }
    return true;
}

// Counts a parse error and logs the first few with the offending command.
void elanError(const char* what, const u8* data, u32 size)
{
    NaomiElanStats& st = elan_Stats();
    if (st.errors++ >= 24)
        return;
    const u8* ram = naomi_ElanRam();
    const bool inRam = ram && data >= ram && data < ram + naomi_ElanRamSize();
    u32 w[8] = {};
    for (u32 i = 0; i < 8 && i * 4 + 4 <= size; i++)
        w[i] = word(data, i);
    naomi_Log("[elan] error: %s at %s %08X (%u bytes left): %08X %08X %08X %08X %08X %08X %08X %08X\n", what,
              inRam ? "ram" : "fifo", inRam ? (u32)(data - ram) : 0u, size, w[0], w[1], w[2], w[3], w[4], w[5], w[6],
              w[7]);
}

inline bool isNaomi2(u32 pcw)
{
    return (pcw >> 27) & 1;
}

// Polygon header and vertex sizes from the object-control bits
// (bit 0 16-bit UV, 2 offset, 3 texture, 5-4 colour type, 6 two volumes).
u32 polygonHeaderSize(u32 pcw)
{
    const u32 colType = (pcw >> 4) & 3;
    const bool texture = (pcw >> 3) & 1, offset = (pcw >> 2) & 1, volume = (pcw >> 6) & 1;
    if (volume)
        return colType == 2 ? 64 : 32;
    return colType == 2 && texture && offset ? 64 : 32;
}

u32 polygonVertexSize(u32 pcw)
{
    const u32 colType = (pcw >> 4) & 3;
    const bool texture = (pcw >> 3) & 1, volume = (pcw >> 6) & 1;
    if (!texture)
        return 32;
    if (volume)
        return 64;
    return colType == 1 ? 64 : 32;
}

void feedTa(const u8* data, u32 bytes)
{
    if (bytes >= 32)
    {
        ELAN_TIME_BEGIN(t0);
        libPvr_TaDMA((u32*)data, bytes / 32);
        ELAN_TIME_END(taNs, t0);
        elan_Stats().taBlocks += bytes / 32;
    }
}

// Length of the run of TA parameters at the start of `data`; stops at the
// first NAOMI 2 command. May end halfway through a 64-byte parameter, which
// is then flagged so the next command's first 32 bytes complete it.
u32 taRun(const u8* data, u32 size)
{
    u32 i = 0;
    while (i + 32 <= size)
    {
        const u32 pcw = word(data + i, 0);
        if (isNaomi2(pcw))
            break;
        u32 paramSize = 32;
        switch (pcw >> 29)
        {
        case TA_END_OF_LIST:
            s_ta.listType = -1;
            break;
        case TA_USER_CLIP:
        case TA_OBJECT_LIST_SET:
            break;
        case TA_POLYGON:
            if (s_ta.listType < 0)
                s_ta.listType = (pcw >> 24) & 7;
            if (s_ta.listType & 1)              // modifier volume lists
                s_ta.vertexSize = 64;
            else
            {
                s_ta.vertexSize = polygonVertexSize(pcw);
                paramSize = polygonHeaderSize(pcw);
            }
            break;
        case TA_SPRITE:
            if (s_ta.listType < 0)
                s_ta.listType = (pcw >> 24) & 7;
            s_ta.vertexSize = 64;
            break;
        case TA_VERTEX:
            paramSize = s_ta.vertexSize;
            break;
        default:                                // 3, 6: invalid
            elanError("invalid TA parameter type", data + i, size - i);
            break;
        }
        if (i + paramSize > size)
        {
            s_ta.secondHalfPending = true;      // first half only
            return i + 32;
        }
        i += paramSize;
    }
    return i;
}

const u8* elanRamPtr(u32 offset, u32& size)
{
    u8* ram = naomi_ElanRam();
    const u32 ramSize = naomi_ElanRamSize();
    if (!ram || ramSize == 0)
    {
        size = 0;
        return nullptr;
    }
    offset &= ramSize - 1;
    if (size > ramSize - offset)
        size = ramSize - offset;
    return ram + offset;
}

void raiseListEnd(u32 mask)
{
    switch (mask)
    {
    case 0x80:     asic_RaiseInterrupt(holly_OPAQUE); break;
    case 0x100:    asic_RaiseInterrupt(holly_OPAQUEMOD); break;
    case 0x200:    asic_RaiseInterrupt(holly_TRANS); break;
    case 0x400:    asic_RaiseInterrupt(holly_TRANSMOD); break;
    case 0x200000: asic_RaiseInterrupt(holly_PUNCHTHRU); break;
    default:       elanError("register wait: unknown interrupt mask", (const u8*)&mask, 4); break;
    }
}

// Link with bit 31 (source: SH-4 RAM at the DMAC channel 2 source address)
// or bit 29 (source: ELAN RAM): copy into texture memory.
void textureDma(u32 offset, u32 vramAddress, u32 size)
{
    if (size == 0 || size > VRAM_SIZE)
    {
        const u32 info[3] = {offset, vramAddress, size};
        elanError("texture DMA size", (const u8*)info, sizeof(info));
        return;
    }
    const u32 dst = (vramAddress & VRAM_MASK) | 0xA4000000;   // 64-bit texture path
    if (offset & 0x80000000)
    {
        u32* src = (u32*)GetMemPtr(DMAC_SAR[2], size);
        if (src)
            WriteMemBlock_nommu_ptr(dst, src, size);
    }
    else
    {
        const u8* src = elanRamPtr(offset, size);
        if (src)
            WriteMemBlock_nommu_ptr(dst, (u32*)src, size);
    }
    elan_Stats().texDmaBytes += size;
    elan_StartDma(kDmaCycles);
}

u32 ichVertexSize(u32 flags)
{
    switch (flags)
    {
    case 0x002: return 16;      // position
    case 0x00A: return 24;      // position, UV
    case 0x00E: return 40;      // position, normal, UV
    case 0x042: return 24;      // position, colours
    case 0x04A: return 32;      // position, UV, colours
    case 0x10A: return 40;      // position, UV, bump basis
    default: return 0;
    }
}

// MODEL PREFETCH: the cache lines of a model's remembered command headers.
// CACHE HINTS: also a material's second line, and for its polygon lists what
// recording them reads from the GPU fetch tables: their entries and page states
// (asked for one model ahead, by prefetchNextModel), then, at the model's own
// call (`now`, those entries in by then), the boxes after their programs.
void prefetchModel(const u8* list, u32 listSize, u32 offset, u32 size, bool now)
{
#if defined(__GNUC__)
    const ModelEntry& m = modelSlot(offset, size);
    if (!m.used || m.offset != offset || m.size != size)
        return;
    const ModelHeads& heads = s_modelHeads[&m - s_models];
    const bool hints = g_cache_hints_preset != 0;
    for (u32 i = 0; i < kModelHeads && heads.at[i] != 0xFFFF; i++)
    {
        const u32 at = (u32)heads.at[i] << 5;
        if (at >= listSize)
            continue;
        if (g_elan_model_prefetch_preset)
            __builtin_prefetch(list + at);
        if (!hints)
            continue;
        const u16 slot = heads.slot[i];
        if (slot == kHeadMaterial)
        {
            if (at + 32 < listSize)
                __builtin_prefetch(list + at + 32);
        }
        else if (slot)
            elan_gx_prefetch_list(slot, offset + at + 32);
    }
    if (hints && now)
        for (u32 i = 0; i < kModelHeads && heads.at[i] != 0xFFFF; i++)
            if (heads.slot[i] != kHeadMaterial && heads.slot[i])
                elan_gx_prefetch_box(heads.slot[i]);
    if (g_elan_model_prefetch_preset)
        elan_Stats().modelPrefetches++;
#else
    (void)list;
    (void)listSize;
    (void)offset;
    (void)size;
    (void)now;
#endif
}

// ...and of the next model called from the same list, a model's walk ahead:
// the top-level list (matrix, model, matrix, model...) is already in the cache
// (ELAN PREFETCH), so peeking a few commands ahead costs no miss of its own.
void prefetchNextModel(const u8* data, u32 size)
{
    for (u32 n = 0; n < 4 && size >= 32; n++)
    {
        const u32 pcw = word(data, 0);
        if (!isNaomi2(pcw))
            return;
        u32 used = 32;
        switch ((pcw >> 8) & 0xF)
        {
        case N2_MODEL:
        {
            const u32 offset = word(data, 4) & 0x1FFFFFF8, msize = word(data, 6);
            u32 listSize = msize;
            const u8* list = elanRamPtr(offset, listSize);
            if (list)
                prefetchModel(list, listSize, offset, msize, false);
            return;
        }
        case N2_MATRIX_OR_LIGHT:
            if (word(data, 1) == 0xF && word(data, 2) == 0x7F)
                used = 160;
            break;
        case N2_GMP:
            used = 64;
            break;
        case N2_ICH:
        case N2_LINK:
            return;
        default:
            break;
        }
        if (used > size)
            return;
        data += used;
        size -= used;
    }
}

void execute(const u8* data, u32 size, u32 depth)
{
    if (depth > kMaxDepth)
    {
        walkImpure();
        elanError("display lists nested too deep", data, size);
        return;
    }
    NaomiElanStats& st = elan_Stats();
    u32 chain = 0;
    // ELAN PREFETCH: the lists a frame walks outside any model are matrices,
    // materials, lights and model calls back to back, and on a Wii each header
    // is a cache miss the walk waits for (log 25: ~0.5 us a command on a dropped
    // frame). There the lines kPrefetchAhead bytes ahead are asked for. Not
    // inside a model (nor a mirror model MIRROR OFF walks), where polygon
    // lists' vertices lie between the headers.
    const u8* ahead = data;
    while (size >= 32)
    {
#if defined(__GNUC__)
        if (g_elan_prefetch_preset && !s_walk && !s_dropLists)
        {
            const u8* const limit = data + (size < kPrefetchAhead ? size : kPrefetchAhead);
            if (ahead < data)
                ahead = data;
            for (; ahead < limit; ahead += 32)
                __builtin_prefetch(ahead);
        }
#endif
        if (s_ta.secondHalfPending)
        {
            walkImpure();
            s_ta.secondHalfPending = false;
            feedTa(data, 32);
            data += 32;
            size -= 32;
            continue;
        }

        const u32 pcw = word(data, 0);
        st.walkRead++;
        int head = -1;                           // this command's place in the model's heads
        if (s_walk && s_walk->base && s_walk->nHeads < kModelHeads)
        {
            const u32 at = (u32)(data - s_walk->base) >> 5;
            if (data >= s_walk->base && at < 0xFFFF)
            {
                head = (int)s_walk->nHeads;
                s_walk->slots[head] = 0;
                s_walk->heads[s_walk->nHeads++] = (u16)at;
            }
        }
        if (!isNaomi2(pcw))
        {
            walkImpure();
            const u32 n = taRun(data, size);
            if (n == 0)
            {
                elanError("empty TA run", data, size);
                return;
            }
            feedTa(data, n);
            data += n;
            size -= n < size ? n : size;
            continue;
        }

        st.n2Commands++;
        if (s_walk)
        {
            s_walk->cmds++;
            s_walk->skel = skelMix(s_walk->skel, pcw);
        }
        u32 used = 32;
        switch ((pcw >> 8) & 0xF)
        {
        case N2_NULL:
            break;

        case N2_PROJECTION:
            walkImpure();                        // the projection outlives the list
            tnl_SetProjection((const u32*)data);
            break;

        case N2_REGISTER_WAIT:
            if (word(data, 1) != 0xFFFFFFFF && word(data, 3) != 0)
            {
                // Waiting for a list-end interrupt: the frame's lists are
                // done, and the ELAN starts the next one from a clean state.
                walkImpure();
                st.waits++;
                raiseListEnd(word(data, 3));
                tnl_ResetState();
            }
            break;

        case N2_MATRIX_OR_LIGHT:
            if (s_walk)
            {
                s_walk->states++;
                s_walk->boxOk = false;
            }
            if (word(data, 1) == 0xF && word(data, 2) == 0x7F)
            {
                used = 160;                      // instance matrix
                if (used <= size)
                    tnl_SetMatrix((const u32*)data);
            }
            else if (word(data, 1) & 0x10)
                tnl_SetLightModel((const u32*)data);
            else
                tnl_SetLight((const u32*)data);
            break;

        case N2_GMP:
            if (s_walk)
            {
                s_walk->states++;
                s_walk->lastGmp = (u32)(data - s_walk->base) + 1;
                if (head >= 0)
                    s_walk->slots[head] = kHeadMaterial;
            }
            used = 64;
            if (used <= size)
                tnl_SetMaterial((const u32*)data);
            break;

        case N2_ICH:
        {
            const u32 vsize = ichVertexSize(word(data, 6));
            const u32 count = word(data, 7);
            if (vsize == 0)
            {
                walkImpure();
                elanError("ICH list: unknown vertex layout", data, size);
                return;                          // unknown layout: cannot resync
            }
            st.ichLists++;
            st.ichVertices += count;
            if (s_walk)
            {
                s_walk->ich++;
                s_walk->verts += count;
                s_walk->skel = skelMix(skelMix(s_walk->skel, word(data, 6)), count);
            }
            used = 32 + vsize * count;
#if defined(__GNUC__)
            // The next command sits past the vertices: start its cache lines
            // now (on a Wii every command header is a miss; on a skipped frame
            // tnl_DrawList returns at once, so this is most of the walk).
            __builtin_prefetch(data + used);
            __builtin_prefetch(data + used + 32);
#endif
            if (ahead < data + used)
                ahead = data + used;            // ELAN PREFETCH: not the vertices
            if (used <= size && s_dropLists)
            {
                // MIRROR OFF: the list type tnl_DrawList would return, nothing drawn.
                const int type = s_ta.listType >= 0 ? s_ta.listType : (int)(pcw >> 24 & 7);
                if (!(type & 1))
                    s_ta.listType = type;
                s_ta.vertexSize = 32;
                st.mirrorLists++;
                st.mirrorVertices += count;
            }
            else if (used <= size)
            {
                s_ta.listType = tnl_DrawList((const u32*)data, vsize, s_ta.listType);
                s_ta.vertexSize = 32;
                if (s_walk)
                {
                    noteListBox(*s_walk);
                    if (head >= 0)
                        s_walk->slots[head] = (u16)tnl_ListFetchSlot();   // <= 4096: never kHeadMaterial
                }
            }
            break;
        }

        case N2_MODEL:
        {
            const u32 modelOffset = word(data, 4) & 0x1FFFFFF8, modelSize = word(data, 6);
            MODEL_CENSUS(modelOffset);
            if (s_walk)
                s_walk->skel = skelMix(skelMix(s_walk->skel, modelOffset), modelSize);
            // The model's user clip (the in-car view's rear-view mirror is a
            // model clipped to its rectangle). Not undone after the model, like
            // the hardware: the next model sets its own.
            rend_SetTileClipMode(pcw >> 16 & 3);
            if ((pcw >> 16 & 3) == 2)
                g_elan_mirror_seen++;
            if (stepOverModel(data, modelOffset, modelSize))
                break;
            if ((pcw >> 16 & 3) == 2 && mirrorModel(data, modelOffset, modelSize, depth))
                break;
            u32 listSize = modelSize;
            const u8* list = elanRamPtr(modelOffset, listSize);
            if (list && hiddenModel(list, listSize, modelOffset, modelSize))
                break;
            if (list)
            {
                st.lists++;
                ModelWalk walk = {};
                walk.skel = 0x811C9DC5u;
                walk.pure = !s_ta.secondHalfPending;
                walk.boxOk = true;
                for (int c = 0; c < 3; c++)
                {
                    walk.box[c] = 1e30f;
                    walk.box[3 + c] = -1e30f;
                }
                walk.base = list;
                ModelWalk* const outer = s_walk;
                const int typeIn = s_ta.listType;
                const u32 indexed = tnl_ListsIndexed();
                if (g_elan_model_prefetch_preset || g_cache_hints_preset)
                {
                    prefetchModel(list, listSize, modelOffset, modelSize, true);
                    if (!outer && size > 32)
                        prefetchNextModel(data + 32, size - 32);
                }
                s_walk = &walk;
                tnl_BeginModel((const u32*)data);
                execute(list, listSize, depth + 1);
                tnl_EndModel();
                s_walk = outer;
                walk.indexed = tnl_ListsIndexed() - indexed;
                if (s_ta.secondHalfPending)
                    walk.pure = false;
                if (!s_dropLists)            // lists not drawn have no box: keep the summary
                    rememberModel(modelOffset, modelSize, walk, typeIn, s_ta.listType);
                if (outer)
                    mergeWalk(*outer, walk);
            }
            break;
        }

        case N2_LINK:
        {
            walkImpure();
            const u32 offset = word(data, 1);
            u32 listSize = word(data, 3);
            if (offset & 0xA0000000)
                textureDma(offset, word(data, 2), listSize);
            else if (listSize)
            {
                const u8* list = elanRamPtr(offset, listSize);
                LINK_CENSUS(offset & 0x0FFFFFFF, depth > 15 ? 15 : depth);
                if (list)
                {
                    st.lists++;
                    if (size <= 32 && chain < kMaxChain)
                    {
                        // Last command of this list: continue in the next one.
                        chain++;
                        data = list;
                        size = listSize;
                        ahead = data;
                        continue;
                    }
                    if (chain >= kMaxChain)
                    {
                        elanError("display list chain does not end", data, size);
                        return;
                    }
                    execute(list, listSize, depth + 1);
                }
            }
            break;
        }

        default:
            walkImpure();
            elanError("unknown NAOMI 2 command", data, size);
            break;
        }
        if (used > size)
        {
            walkImpure();
            return;
        }
        data += used;
        size -= used;
    }
}

} // namespace

u32 g_elan_exec_seq = 0;
extern "C" {
u32 g_elan_mirror_seen = 0;
}

void elan_Execute(const u32* words, u32 bytes)
{
    g_elan_exec_seq++;
    execute((const u8*)words, bytes, 0);
}

void elan_CmdReset()
{
    s_ta = TaStream();
    tnl_ResetState();
}
