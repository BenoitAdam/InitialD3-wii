// SHIL executor (Windows runner only, `--shil-exec 1`): runs the SH-4 the way
// the Wii's recompiler does, block by block from the same decoder
// (rec_v2/decoder.cpp), but executing each SHIL op in C instead of PowerPC:
// the canonical bodies (shil_canonical.h) through the ngen_CC_* calling
// convention below, the few ops a back end implements itself (moves, memory,
// jumps, interpreter fallback) by hand, and the block exits as ngen_End emits
// them. What it checks is the front end: a decoder change (JIT FPU BANKS, for
// one) must leave the game's run unchanged, which two runs with and without it
// show through the record digests. It is not the Wii back end: pinned
// registers, fastmem and the PowerPC code are not modelled.
#include "types.h"

#if HOST_OS != OS_WII

#include "dc/sh4/rec_v2/decoder.h"
#include "dc/sh4/rec_v2/shil.h"
#include "dc/sh4/rec_v2/ngen.h"
#include "dc/sh4/rec_v2/rec_config.h"
#include "dc/sh4/sh4_registers.h"
#include "dc/sh4/sh4_opcode_list.h"
#include "dc/sh4/intc.h"
#include "dc/mem/sh4_mem.h"

#include <type_traits>
#include <unordered_map>
#include <vector>

u32* Sh4_int_GetRegisterPtr(Sh4RegType reg);
namespace {

// ---- Operands ------------------------------------------------------------------
u32* regPtr(const shil_param& p)
{
    return Sh4_int_GetRegisterPtr((Sh4RegType)p._reg);
}

u32 getU32(const shil_param& p)
{
    if (p.is_null())
        return 0;
    return p.is_imm() ? p._imm : *regPtr(p);
}

float getF32(const shil_param& p)
{
    if (p.is_imm())
    {
        float f;
        memcpy(&f, &p._imm, 4);
        return f;
    }
    float f;
    memcpy(&f, regPtr(p), 4);
    return f;
}

// ---- The canonical calling convention (ngen.h) ------------------------------
// The canonical bodies push their arguments last to first, name the function,
// then the parameters its result goes to. The call is made when the first of
// those comes (its type says what the function returns), or at the end.
struct CcArg
{
    CanonicalParamType t;
    shil_param* p;
};
CcArg s_args[8];
int s_nargs = 0;
void* s_fn = nullptr;
bool s_called = false;
union CcResult
{
    u32 u;
    float f;
    u64 q;
};
CcResult s_rv;

template <typename R, typename... A>
CcResult callAs(void* fn, A... a)
{
    CcResult res;       // not `r`: sh4_registers.h makes it the SH-4's registers
    res.q = 0;
    if constexpr (std::is_same<R, void>::value)
        ((void (*)(A...))fn)(a...);
    else if constexpr (std::is_same<R, u32>::value)
        res.u = ((u32(*)(A...))fn)(a...);
    else if constexpr (std::is_same<R, float>::value)
        res.f = ((float (*)(A...))fn)(a...);
    else
        res.q = ((u64(*)(A...))fn)(a...);
    return res;
}

// The C arguments in order: s_args[n - 1] first.
template <typename R, typename... A>
CcResult dispatch(void* fn, int i, A... a)
{
    if (i < 0)
        return callAs<R>(fn, a...);
    if constexpr (sizeof...(A) < 4)
    {
        const CcArg& c = s_args[i];
        switch (c.t)
        {
        case CPT_f32: return dispatch<R>(fn, i - 1, a..., getF32(*c.p));
        case CPT_ptr: return dispatch<R>(fn, i - 1, a..., (void*)regPtr(*c.p));
        default:      return dispatch<R>(fn, i - 1, a..., getU32(*c.p));
        }
    }
    die("shil_exec: canonical call with more than 4 arguments");
    return CcResult();
}

void ccCall(CanonicalParamType rv)
{
    if (s_called)
        return;
    s_called = true;
    switch (rv)
    {
    case CPT_u32rv:  s_rv = dispatch<u32>(s_fn, s_nargs - 1); break;
    case CPT_f32rv:  s_rv = dispatch<float>(s_fn, s_nargs - 1); break;
    case CPT_u64rvL:
    case CPT_u64rvH: s_rv = dispatch<u64>(s_fn, s_nargs - 1); break;
    default:         s_rv = dispatch<void>(s_fn, s_nargs - 1); break;
    }
}

// ---- Blocks ----------------------------------------------------------------------
struct Cached
{
    DecodedBlock blk;
    std::vector<u16> code;
};
std::unordered_map<u64, Cached> s_blocks;

u64 blockKey(u32 pc)
{
    return (u64)pc << 8 | (u64)(fpscr.PR << 2 | fpscr.SZ << 1 | (fpscr.RM & 1));
}

Cached& blockAt(u32 pc)
{
    Cached& c = s_blocks[blockKey(pc)];
    bool ok = !c.code.empty();
    for (size_t i = 0; ok && i < c.code.size(); i++)
        ok = ReadMem16(pc + (u32)i * 2) == c.code[i];
    if (!ok)
    {
        // As the recompiler: rdv_CompileBlock decodes with the FPSCR of the moment.
        DecodedBlock* d = dec_DecodeBlock(pc, fpscr, SH4_TIMESLICE / 2);
        c.blk = *d;
        dec_Cleanup();
        c.code.resize(c.blk.sh4_code_size / 2 ? c.blk.sh4_code_size / 2 : 1);
        for (size_t i = 0; i < c.code.size(); i++)
            c.code[i] = (u16)ReadMem16(pc + (u32)i * 2);
    }
    return c;
}

u32 address(const shil_opcode& o)
{
    return getU32(o.rs1) + (o.rs3.is_null() ? 0 : getU32(o.rs3));
}

} // namespace

// ngen.h's calling convention, executed on the spot (replaces host_win.cpp's stubs).
void ngen_CC_Start(shil_opcode*)
{
    s_nargs = 0;
    s_fn = nullptr;
    s_called = false;
}

void ngen_CC_Param(shil_opcode* op, shil_param* par, CanonicalParamType tp)
{
    (void)op;
    switch (tp)
    {
    case CPT_u32rv:
        ccCall(tp);
        *regPtr(*par) = s_rv.u;
        break;
    case CPT_f32rv:
        ccCall(tp);
        memcpy(regPtr(*par), &s_rv.f, 4);
        break;
    case CPT_u64rvL:
        ccCall(tp);
        *regPtr(*par) = (u32)s_rv.q;
        break;
    case CPT_u64rvH:
        ccCall(tp);
        *regPtr(*par) = (u32)(s_rv.q >> 32);
        break;
    default:
        if (s_nargs >= 8)
            die("shil_exec: too many canonical arguments");
        s_args[s_nargs++] = {tp, par};
        break;
    }
}

void ngen_CC_Call(shil_opcode*, void* function)
{
    s_fn = function;
}

void ngen_CC_Finish(shil_opcode*)
{
    ccCall(CPT_u32);     // a void function: no result parameter came
}

// One block at next_pc; returns the cycles the recompiler would charge for it.
u32 shilexec_Block()
{
    Cached& c = blockAt(next_pc);
    DecodedBlock& b = c.blk;
    u32 djump = 0;
    bool hasJcond = false;
    for (size_t i = 0; i < b.oplist.size(); i++)
    {
        shil_opcode& o = b.oplist[i];
        switch (o.op)
        {
        case shop_mov32:
            *regPtr(o.rd) = getU32(o.rs1);
            break;
        case shop_mov64:
        {
            const u32 lo = *Sh4_int_GetRegisterPtr((Sh4RegType)o.rs1._reg);
            const u32 hi = *Sh4_int_GetRegisterPtr((Sh4RegType)(o.rs1._reg + 1));
            *Sh4_int_GetRegisterPtr((Sh4RegType)o.rd._reg) = lo;
            *Sh4_int_GetRegisterPtr((Sh4RegType)(o.rd._reg + 1)) = hi;
            break;
        }
        case shop_jdyn:
            djump = getU32(o.rs1) + (o.rs2.is_imm() ? o.rs2._imm : 0);
            break;
        case shop_jcond:
            djump = getU32(o.rs1);
            hasJcond = true;
            break;
        case shop_ifb:
            if (o.rs1._imm)
                next_pc = o.rs2._imm;
            OpDesc[o.rs3._imm]->oph(o.rs3._imm);
            break;
        case shop_readm:
        {
            const u32 a = address(o);
            switch (o.flags)
            {
            case 1: *regPtr(o.rd) = (u32)(s32)(s8)ReadMem8(a); break;
            case 2: *regPtr(o.rd) = (u32)(s32)(s16)ReadMem16(a); break;
            case 4: *regPtr(o.rd) = ReadMem32(a); break;
            case 8:
                // As the Wii back end: word[a] -> rd, word[a + 4] -> rd + 1.
                *Sh4_int_GetRegisterPtr((Sh4RegType)o.rd._reg) = ReadMem32(a);
                *Sh4_int_GetRegisterPtr((Sh4RegType)(o.rd._reg + 1)) = ReadMem32(a + 4);
                break;
            default: die("shil_exec: readm size");
            }
            break;
        }
        case shop_writem:
        {
            const u32 a = address(o);
            switch (o.flags)
            {
            case 1: WriteMem8(a, (u8)getU32(o.rs2)); break;
            case 2: WriteMem16(a, (u16)getU32(o.rs2)); break;
            case 4: WriteMem32(a, getU32(o.rs2)); break;
            case 8:
                WriteMem32(a, *Sh4_int_GetRegisterPtr((Sh4RegType)o.rs2._reg));
                WriteMem32(a + 4, *Sh4_int_GetRegisterPtr((Sh4RegType)(o.rs2._reg + 1)));
                break;
            default: die("shil_exec: writem size");
            }
            break;
        }
        default:
            shil_chf[o.op](&o);
            break;
        }
    }
    switch (b.BlockType)
    {
    case BET_Cond_0:
    case BET_Cond_1:
        next_pc = (hasJcond ? djump : sr.T) == (u32)(b.BlockType & 1) ? b.BranchBlock : b.NextBlock;
        break;
    case BET_DynamicCall:
    case BET_DynamicJump:
    case BET_DynamicRet:
        next_pc = djump;
        break;
    case BET_StaticIntr:
    case BET_DynamicIntr:
        next_pc = b.BlockType == BET_StaticIntr ? b.BranchBlock : djump;
        UpdateINTC();
        break;
    default:
        next_pc = b.BranchBlock;
        break;
    }
    return b.cycles ? b.cycles : 1;
}

#endif
