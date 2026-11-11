// Texture cache census for the Windows runner (env IDAS3_TEXCACHE=1).
//
// The Wii's texture cache lives in gxRend.cpp, which the runner does not run.
// This file replays gxRend's cache rules on the bind stream the runner sees
// (every textured TA polygon header and every GPU-drawn ELAN list, the same
// headers gxRend calls SetTextureParams for, in a drawn render) and counts,
// per policy, what the Wii would do: decodes, the bytes and texels decoded,
// binds that would show another texture's pixels, wraps of the arena.
//
// Policies (gxRend TEXTURE CACHE / VQ AS CMPR):
//   NORMAL        preset 2: sized bump arena (GX_TEX_CACHE_BYTES) + address map.
//                 Its sentinel lands past the small mip levels, so a mipmapped
//                 non-VQ texture never hits (gxRend.cpp, "SENTINEL ADDRESS").
//   NORMAL+CMPR   the same with VQ 565 decoded to CMPR (4 bits a texel).
//   PLUS(old)     VERY FAST+ as built until 2026-11-11 on NAOMI 2: no overflow
//                 arena, so every texture goes to its address-derived slot,
//                 (tex_addr * 2) masked to the lower 4 MB of the 8 MB buffer.
//   PLUS(old)+CMPR the owner's setting.
//   PLUS(new)     the VERY FAST+ rewrite: NORMAL's arena with the sentinel at the
//                 address the check reads, plus a per-render memo (a bind seen
//                 earlier in the render skips the lookup and the VRAM read).
//   PLUS(new)+CMPR
//
// "Content changed" is modelled by the validity word the Wii reads (first word
// at the texture's address, or the VQ codebook ^ index fingerprint) compared
// with its value at the decode; the Wii's sentinel also catches a write of the
// same value, which this cannot see.
#include "types.h"

#if HOST_OS != OS_WII

#include "plugs/drkPvr/drkPvr.h"
#include "plugs/drkPvr/cmpr_encode.h"
#include "dc/pvr/pvr_if.h"
#include "host_hooks.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

constexpr u32 kArenaBytes = 8u * 1024u * 1024u;   // GX_TEX_CACHE_BYTES on NAOMI 2
constexpr u32 kDescBytes = 64;                     // sizeof(TextureCacheDesc)
constexpr u32 kGranule = 32;

struct Bind
{
    u32 tcw, tsp;
};
std::vector<Bind> s_binds;   // this render's binds, in submission order

int s_on = -1;
u32 s_period = 60;           // drawn renders a line

inline u32 rd32(u32 addr)
{
    return *(const u32*)&vram.data[addr & VRAM_MASK & ~3u];
}

// What one bind asks for, as SetTextureParams computes it.
struct Tex
{
    u32 addr, w, h, eh, pf;
    bool vq, mip, scan, stride;
    u32 key;            // shape key (tex_shape_key, JOJO_FIX off)
    u32 word;           // the validity word the Wii reads
    u32 dec16;          // decode bytes with VQ as 16 bpp
    u32 decCmpr;        // with VQ 565 as CMPR
    bool vqCmpr;        // VQ 565: what VQ AS CMPR changes
};

Tex describe(const Bind& b)
{
    Tex t;
    t.addr = (b.tcw & 0x1FFFFF) << 3;
    t.pf = (b.tcw >> 27) & 7;
    t.vq = (b.tcw >> 30) & 1;
    t.mip = (b.tcw >> 31) & 1;
    t.scan = ((b.tcw >> 26) & 1) && t.pf != 5 && t.pf != 6;
    t.stride = t.scan && ((b.tcw >> 25) & 1);
    t.w = 8u << ((b.tsp >> 3) & 7);
    t.h = 8u << (b.tsp & 7);
    t.eh = t.mip ? t.w : t.h;
    t.key = (b.tcw & ~0x1FFFFFu) ^ (b.tsp & 0x3Fu);
    if (t.vq)
    {
        // VQMipPoint: 2048 + the index map's offset to the full-size level.
        static const u32 kVqMip[8] = {0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556};
        const u32 idxAddr = t.addr + 2048 + (t.mip ? kVqMip[(b.tsp >> 3) & 7] : 0);
        t.word = rd32(t.addr) ^ rd32(idxAddr);
    }
    else
        t.word = rd32(t.addr);
    t.vqCmpr = t.vq && t.pf == 1 && !t.scan;
    t.dec16 = t.w * t.eh * 2;
    t.decCmpr = t.vqCmpr ? t.w * t.eh / 2 : t.dec16;
    return t;
}

inline u32 roundUp(u32 x, u32 a)
{
    return (x + a - 1) & ~(a - 1);
}

struct Counts
{
    u64 binds = 0, lookups = 0, decodes = 0, decBytes = 0, cmprTexels = 0, texels = 0;
    u64 corrupt = 0;      // binds that would show pixels or a descriptor another decode wrote over
    u64 clobberLive = 0;  // decodes writing over a texture bound earlier in the same render
    u64 wraps = 0;
    u64 renders = 0;
};

class Policy
{
public:
    Policy(const char* name, bool cmpr) : m_name(name), m_cmpr(cmpr) {}
    virtual ~Policy() = default;
    virtual void render(const std::vector<Bind>& binds) = 0;
    const char* m_name;
    bool m_cmpr;
    Counts m_block, m_total;

protected:
    void decoded(const Tex& t, u32 bytes)
    {
        m_block.decodes++;
        m_block.decBytes += bytes;
        m_block.texels += (u64)t.w * t.eh;
        if (m_cmpr && t.vqCmpr)
            m_block.cmprTexels += (u64)t.w * t.eh;
    }
};

// NORMAL (and the new VERY FAST+): persistent sized bump arena + address map.
class Arena : public Policy
{
public:
    Arena(const char* name, bool cmpr, bool fixedSentinel, bool memo)
        : Policy(name, cmpr), m_fixed(fixedSentinel), m_memo(memo)
    {
    }

    void render(const std::vector<Bind>& binds) override
    {
        m_block.renders++;
        m_seen.clear();
        for (const Bind& b : binds)
        {
            m_block.binds++;
            const u64 bk = (u64)b.tcw << 32 | (b.tsp & 0x3F);
            if (m_memo)
            {
                auto s = m_seen.find(bk);
                if (s != m_seen.end())
                    continue;
                m_seen.emplace(bk, 1);
            }
            m_block.lookups++;
            const Tex t = describe(b);
            const u32 need = kDescBytes + roundUp(m_cmpr ? t.decCmpr : t.dec16, 32);
            auto it = m_map.find(t.addr);
            Entry* e = nullptr;
            if (it != m_map.end())
            {
                e = &it->second;
                if (e->slot < need)
                {
                    alloc(t.addr, need);
                    e = &m_map[t.addr];
                    e->valid = false;
                }
            }
            else
            {
                alloc(t.addr, need);
                e = &m_map[t.addr];
                e->valid = false;
            }
            bool ok = e->valid && e->vq == t.vq && e->word == t.word;
            if (ok && !t.vq)
                ok = e->key == t.key && (m_fixed || !t.mip);
            if (!ok)
            {
                decoded(t, need - kDescBytes);
                e->valid = true;
                e->vq = t.vq;
                e->word = t.word;
                e->key = t.key;
            }
        }
    }

private:
    struct Entry
    {
        u32 slot = 0;
        bool valid = false, vq = false;
        u32 word = 0, key = 0;
    };
    void alloc(u32 addr, u32 need)
    {
        if (m_off + need > kArenaBytes)
        {
            m_off = 0;
            m_map.clear();
            m_block.wraps++;
        }
        Entry e;
        e.slot = need;
        m_map[addr] = e;
        m_off += need;
    }
    bool m_fixed, m_memo;
    u32 m_off = 0;
    std::unordered_map<u32, Entry> m_map;
    std::unordered_map<u64, int> m_seen;
};

// VERY FAST+ as built for NAOMI 2 until now: every texture in its skimp slot.
class Slots : public Policy
{
public:
    Slots(const char* name, bool cmpr) : Policy(name, cmpr), m_owner(kArenaBytes / kGranule, 0)
    {
        m_dec.push_back({});   // serial 0 = never written
    }

    void render(const std::vector<Bind>& binds) override
    {
        m_block.renders++;
        m_render++;
        for (const Bind& b : binds)
        {
            m_block.binds++;
            m_block.lookups++;
            const Tex t = describe(b);
            // skimp_slot(): the pixels at (addr * 2) in the lower half, the
            // descriptor in the 64 bytes before them.
            u32 off = (t.addr * 2) & (kArenaBytes / 2 - 1);
            off = roundUp(off, 32);
            if (off < 64)
                off = 64;
            const u32 desc = off - kDescBytes;
            const u32 bytes = m_cmpr ? t.decCmpr : t.dec16;
            // The descriptor's address field sits in its second granule, the
            // GXTexObj in the first.
            const u32 dAddr = m_owner[desc / kGranule + 1];
            const u32 dObj = m_owner[desc / kGranule];
            Dec& d = m_dec[dAddr];
            // PLUS: sentinel (or VQ fingerprint) + address, no shape key.
            if (dAddr && d.addr == t.addr && d.word == t.word)
            {
                // A hit on a slot whose descriptor or pixels another decode
                // wrote over, or that holds this address at another size.
                if (dObj != dAddr || d.clobbered || d.key != t.key)
                    m_block.corrupt++;
                d.boundIn = m_render;
                continue;
            }
            decoded(t, bytes);
            const u32 serial = (u32)m_dec.size();
            Dec nd;
            nd.addr = t.addr;
            nd.word = t.word;
            nd.key = t.key;
            nd.boundIn = m_render;
            m_dec.push_back(nd);
            const u32 g0 = desc / kGranule;
            const u32 g1 = (off + roundUp(bytes, 32)) / kGranule;
            bool hitLive = false;
            // Past the 8 MB buffer gxRend writes on into whatever follows it;
            // only the buffer is modelled.
            for (u32 g = g0; g < g1 && g < m_owner.size(); g++)
            {
                const u32 prev = m_owner[g];
                if (prev && prev != serial)
                {
                    Dec& p = m_dec[prev];
                    // The GPU may not have drawn that one yet.
                    if (p.boundIn == m_render)
                        hitLive = true;
                    p.clobbered = true;
                }
                m_owner[g] = serial;
            }
            m_block.clobberLive += hitLive;
        }
    }

private:
    struct Dec
    {
        u32 addr = 0, word = 0, key = 0;
        bool clobbered = false;
        u32 boundIn = 0;   // the render it was last bound in
    };
    std::vector<u32> m_owner;   // decode serial owning each 32-byte granule
    std::vector<Dec> m_dec;
    u32 m_render = 0;
};

std::vector<Policy*> s_policies;

// VQ AS CMPR on the game's own VQ 565 textures: each one decoded, encoded by
// both encoders (cmpr_encode.h), decoded as GX shows it and compared.
struct CmprCheck
{
    u64 blocks = 0, texels = 0, threeColour = 0, transparent = 0, over32 = 0;
    double sse = 0, secs = 0;
};
// [k][e]: k 0 the VQ 565 textures, 1 the plain twiddled 565 ones (no game
// here has to have VQ 565: Initial D 3 has none, so the encoders are also
// checked on its other RGB565 art); e 0 old (luma), 1 new (PCA).
CmprCheck s_cmpr[2][2];
double s_vq16Secs = 0;
u32 s_checked[2];

u32 twiddleIndex(u32 x, u32 y, u32 w, u32 h)
{
    u32 r = 0, sh = 0;
    w >>= 1;
    h >>= 1;
    while (w || h)
    {
        if (h)
        {
            r |= (y & 1) << sh++;
            y >>= 1;
            h >>= 1;
        }
        if (w)
        {
            r |= (x & 1) << sh++;
            x >>= 1;
            w >>= 1;
        }
    }
    return r;
}

void checkCmpr(const Tex& t, u32 tsp)
{
    if (t.w < 4 || t.eh < 4)
        return;
    const int kind = t.vq ? 0 : 1;
    std::vector<u16> px((size_t)t.w * t.eh);
    if (t.vq)
    {
        static const u32 kVqMip[8] = {0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556};
        const u32 idxBase = t.addr + 2048 + (t.mip ? kVqMip[(tsp >> 3) & 7] : 0);
        const auto q0 = std::chrono::steady_clock::now();
        for (u32 y = 0; y < t.eh; y++)
            for (u32 x = 0; x < t.w; x++)
            {
                const u32 idx = vram.data[(idxBase + twiddleIndex(x >> 1, y >> 1, t.w >> 1, t.eh >> 1)) & VRAM_MASK];
                px[(size_t)y * t.w + x] =
                    *(const u16*)&vram.data[(t.addr + (idx * 4 + ((x & 1) << 1 | (y & 1))) * 2) & VRAM_MASK & ~1u];
            }
        s_vq16Secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - q0).count();
    }
    else
    {
        // Twiddled RGB565; a mipmapped one from its full-size level.
        u32 base = t.addr;
        if (t.mip)
        {
            u32 sum = 0, n = 0;
            while ((1u << n) < t.w)
                n++;
            for (u32 k = 0; k < n; k++)
                sum += 1u << (2 * k);
            base += (3 + sum) * 2;
        }
        for (u32 y = 0; y < t.eh; y++)
            for (u32 x = 0; x < t.w; x++)
                px[(size_t)y * t.w + x] =
                    *(const u16*)&vram.data[(base + twiddleIndex(x, y, t.w, t.eh) * 2) & VRAM_MASK & ~1u];
    }
    for (int e = 0; e < 2; e++)
    {
        CmprCheck& c = s_cmpr[kind][e];
        std::vector<u8> out((size_t)t.w * t.eh / 2);
        const auto t0 = std::chrono::steady_clock::now();
        u8* o = out.data();
        for (u32 by = 0; by < t.eh; by += 4)
            for (u32 bx = 0; bx < t.w; bx += 4, o += 8)
            {
                u16 block[16];
                for (u32 i = 0; i < 16; i++)
                    block[i] = px[(size_t)(by + i / 4) * t.w + bx + i % 4];
                if (e)
                    cmpr_encode_block_pca(block, o);
                else
                    cmpr_encode_block_luma(block, o);
            }
        c.secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        o = out.data();
        for (u32 by = 0; by < t.eh; by += 4)
            for (u32 bx = 0; bx < t.w; bx += 4, o += 8)
            {
                u32 shown[16];
                cmpr_decode_block(o, shown);
                const u16 c0 = (u16)(o[0] << 8 | o[1]), c1 = (u16)(o[2] << 8 | o[3]);
                c.blocks++;
                c.threeColour += c0 <= c1;
                for (u32 i = 0; i < 16; i++)
                {
                    s32 want[3];
                    cmpr_expand565(px[(size_t)(by + i / 4) * t.w + bx + i % 4], want);
                    c.texels++;
                    if (!(shown[i] >> 24))
                    {
                        c.transparent++;
                        c.over32++;
                        c.sse += (double)want[0] * want[0] + (double)want[1] * want[1] + (double)want[2] * want[2];
                        continue;
                    }
                    const s32 d0 = (s32)((shown[i] >> 16) & 255) - want[0];
                    const s32 d1 = (s32)((shown[i] >> 8) & 255) - want[1];
                    const s32 d2 = (s32)(shown[i] & 255) - want[2];
                    c.sse += (double)d0 * d0 + (double)d1 * d1 + (double)d2 * d2;
                    c.over32 += abs(d0) > 32 || abs(d1) > 32 || abs(d2) > 32;
                }
            }
    }
    s_checked[kind]++;
}

// One-off census of the textures themselves.
struct Seen
{
    u32 binds;
    Tex t;
};
std::unordered_map<u64, Seen> s_seen;   // by (tcw, tsp)
u64 s_drawn = 0;

void init()
{
    const char* e = getenv("IDAS3_TEXCACHE");
    s_on = e && *e && *e != '0';
    if (!s_on)
        return;
    if (atoi(e) > 1)
        s_period = (u32)atoi(e);
    s_policies.push_back(new Arena("NORMAL", false, false, false));
    s_policies.push_back(new Arena("NORMAL+CMPR", true, false, false));
    s_policies.push_back(new Slots("PLUS(old)", false));
    s_policies.push_back(new Slots("PLUS(old)+CMPR", true));
    s_policies.push_back(new Arena("PLUS(new)", false, true, true));
    s_policies.push_back(new Arena("PLUS(new)+CMPR", true, true, true));
}

void printBlock(const char* tag, u32 frame, const Counts& c, const char* name)
{
    const double r = c.renders ? (double)c.renders : 1.0;
    printf("[texc] %s f%u %-15s %4.0f binds %4.0f lookups %6.2f decodes %7.1f KB (%6.0f texels, %6.0f as CMPR) a render; "
           "%llu corrupt binds, %llu live clobbers, %llu wraps\n",
           tag, frame, name, c.binds / r, c.lookups / r, c.decodes / r, c.decBytes / r / 1024.0, c.texels / r,
           c.cmprTexels / r, (unsigned long long)c.corrupt, (unsigned long long)c.clobberLive,
           (unsigned long long)c.wraps);
}

void add(Counts& to, const Counts& c)
{
    to.binds += c.binds;
    to.lookups += c.lookups;
    to.decodes += c.decodes;
    to.decBytes += c.decBytes;
    to.cmprTexels += c.cmprTexels;
    to.texels += c.texels;
    to.corrupt += c.corrupt;
    to.clobberLive += c.clobberLive;
    to.wraps += c.wraps;
    to.renders += c.renders;
}

}   // namespace

void texcCensusBind(u32 pcw, u32 tsp, u32 tcw)
{
    if (s_on < 0)
        init();
    if (!s_on || !((pcw >> 3) & 1))
        return;
    s_binds.push_back({tcw, tsp});
}

void texcCensusRender(bool dropped)
{
    if (s_on < 0)
        init();
    if (!s_on)
        return;
    if (dropped)
    {
        s_binds.clear();
        return;
    }
    for (const Bind& b : s_binds)
    {
        const u64 k = (u64)b.tcw << 32 | (b.tsp & 0x3F);
        auto it = s_seen.find(k);
        if (it == s_seen.end())
        {
            const Tex t = describe(b);
            s_seen.emplace(k, Seen{1, t});
            if (t.vqCmpr || (t.pf == 1 && !t.vq && !t.scan))
                checkCmpr(t, b.tsp);
        }
        else
            it->second.binds++;
    }
    for (Policy* p : s_policies)
        p->render(s_binds);
    s_binds.clear();
    if (++s_drawn % s_period == 0)
        for (Policy* p : s_policies)
        {
            printBlock("   ", g_host_render.vblanks, p->m_block, p->m_name);
            add(p->m_total, p->m_block);
            p->m_block = Counts();
        }
}

void texcCensusExit()
{
    if (s_on <= 0)
        return;
    for (Policy* p : s_policies)
    {
        add(p->m_total, p->m_block);
        printBlock("all", g_host_render.vblanks, p->m_total, p->m_name);
    }
    // The textures: by kind, and the address range they span.
    struct K
    {
        u32 n = 0;
        u64 bytes16 = 0, bytesCmpr = 0, binds = 0;
    } k[4];   // 0 VQ 565, 1 other VQ, 2 mipmapped non-VQ, 3 the rest
    u32 lo = ~0u, hi = 0;
    for (const auto& s : s_seen)
    {
        const Tex& t = s.second.t;
        const int c = t.vqCmpr ? 0 : t.vq ? 1 : t.mip ? 2 : 3;
        k[c].n++;
        k[c].bytes16 += t.dec16;
        k[c].bytesCmpr += t.decCmpr;
        k[c].binds += s.second.binds;
        lo = t.addr < lo ? t.addr : lo;
        hi = t.addr > hi ? t.addr : hi;
    }
    static const char* kNames[4] = {"VQ 565", "VQ other", "mipmapped non-VQ", "other"};
    for (int c = 0; c < 4; c++)
        printf("[texc] textures %-16s %5u, %8.1f KB decoded (%8.1f KB as CMPR), %llu binds\n", kNames[c], k[c].n,
               k[c].bytes16 / 1024.0, k[c].bytesCmpr / 1024.0, (unsigned long long)k[c].binds);
    printf("[texc] texture addresses %06X..%06X, %u distinct (tcw, size) pairs\n", lo, hi, (u32)s_seen.size());
    static const char* kEnc[2] = {"ON (OLD) luma", "ON  PCA"};
    static const char* kKind[2] = {"VQ 565", "plain 565"};
    for (int k = 0; k < 2; k++)
        for (int e = 0; e < 2 && s_checked[k]; e++)
        {
            const CmprCheck& c = s_cmpr[k][e];
            const double mse = c.texels ? c.sse / (3.0 * c.texels) : 0.0;
            printf("[texc] CMPR %-13s on %u %s textures, %llu blocks: %llu (%.2f%%) 3-colour, %llu texels "
                   "transparent, %.2f%% texels off by > 32, RMS %.2f (PSNR %.1f dB), encode %.1f ms",
                   kEnc[e], s_checked[k], kKind[k], (unsigned long long)c.blocks, (unsigned long long)c.threeColour,
                   c.blocks ? 100.0 * c.threeColour / c.blocks : 0.0, (unsigned long long)c.transparent,
                   c.texels ? 100.0 * c.over32 / c.texels : 0.0, sqrt(mse),
                   mse > 0 ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0, c.secs * 1e3);
            if (k == 0)
                printf(" (VQ to 16 bpp %.1f ms)", s_vq16Secs * 1e3);
            printf("\n");
        }
}

#endif   // HOST_OS != OS_WII
