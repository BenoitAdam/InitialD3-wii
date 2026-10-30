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

#include <cstring>

#if !defined(BUILD_NAOMI2)
#error "naomi_elan_cmd.cpp is only part of the NAOMI 2 build"
#endif

extern "C" {
int g_elan_model_skip_preset = 1;
}

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
struct ModelWalk
{
    u32 skel;           // hash of the command words that decide the walk
    u32 cmds, ich, verts, lists, states;
    bool pure;          // nothing but state commands, polygon lists, pure models
};

struct ModelEntry
{
    u32 offset, size, skel, verts;
    u16 cmds, ich, lists, states;
    s8 typeIn, typeOut; // TA list type open before / after the walk
    u8 pure, used;
};
constexpr u32 kModelSlots = 2048;
ModelEntry s_models[kModelSlots];
ModelWalk* s_walk = nullptr;        // the innermost model list being walked

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
        const ModelWalk w = {m.skel, m.cmds, m.ich, m.verts, m.lists, m.states, true};
        mergeWalk(*s_walk, w);
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
    while (size >= 32)
    {
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
                s_walk->states++;
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
                s_walk->states++;
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
            if (used <= size)
            {
                s_ta.listType = tnl_DrawList((const u32*)data, vsize, s_ta.listType);
                s_ta.vertexSize = 32;
            }
            break;
        }

        case N2_MODEL:
        {
            const u32 modelOffset = word(data, 4) & 0x1FFFFFF8, modelSize = word(data, 6);
            if (s_walk)
                s_walk->skel = skelMix(skelMix(s_walk->skel, modelOffset), modelSize);
            // The model's user clip (the in-car view's rear-view mirror is a
            // model clipped to its rectangle). Not undone after the model, like
            // the hardware: the next model sets its own.
            rend_SetTileClipMode(pcw >> 16 & 3);
            if (stepOverModel(data, modelOffset, modelSize))
                break;
            u32 listSize = modelSize;
            const u8* list = elanRamPtr(modelOffset, listSize);
            if (list)
            {
                st.lists++;
                ModelWalk walk = {0x811C9DC5u, 0, 0, 0, 0, 0, !s_ta.secondHalfPending};
                ModelWalk* const outer = s_walk;
                const int typeIn = s_ta.listType;
                s_walk = &walk;
                tnl_BeginModel((const u32*)data);
                execute(list, listSize, depth + 1);
                tnl_EndModel();
                s_walk = outer;
                if (s_ta.secondHalfPending)
                    walk.pure = false;
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
                if (list)
                {
                    st.lists++;
                    if (size <= 32 && chain < kMaxChain)
                    {
                        // Last command of this list: continue in the next one.
                        chain++;
                        data = list;
                        size = listSize;
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
