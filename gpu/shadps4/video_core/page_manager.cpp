#include "../../shim/bbport_diagnostics.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include "bbport_toggles.h"
#include "bbport_frametrace.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#include "common/decoder.h"
#include <immintrin.h>
#endif

extern "C" int runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

#ifdef _WIN64
namespace {
// bbport: BB_FAULT_EMULATE=1. Guest read faults in DriveClub mostly read CPU data that shares a 4 KB page with a
// few GPU-written bytes (the top site reads a field ~250 bytes from 16 GPU-written bytes): the page is protected
// for those bytes, and the read waited for a download of them. A plain load (mov, movzx, movsx, movsxd, SSE
// moves) of bytes the GPU did not write is done here instead: the value comes from the unprotected backing view,
// the register is set and the instruction skipped; the page stays protected for the GPU-written bytes.
// A page the guest keeps touching is better downloaded once (which unprotects it) than emulated on every access:
// up to BB_FAULT_EMULATE_PAGE (default 128) emulated faults per page and frame on this thread, then the usual path.
bool PageEmulationBudgetLeft(VAddr addr) {
    static const u32 budget = [] {
        const char* env = std::getenv("BB_FAULT_EMULATE_PAGE");
        return env ? u32(std::strtoul(env, nullptr, 10)) : 128u;
    }();
    struct Entry {
        u64 page;
        u64 frame;
        u32 count;
    };
    thread_local std::array<Entry, 64> table{};
    // A hard total per frame and thread that table collisions cannot reset.
    thread_local u64 total_frame = ~0ull;
    thread_local u32 total = 0;
    const u64 page = addr >> 12;
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    if (frame != total_frame) {
        total_frame = frame;
        total = 0;
    }
    if (++total > budget * 32) {
        return false;
    }
    auto& e = table[(page ^ (page >> 6)) & 63];
    if (e.page != page || e.frame != frame) {
        e = {page, frame, 0};
    }
    return ++e.count <= budget;
}

// The bytes an emulated instruction reads, from guest memory when the GPU wrote none of them. False: the usual path.
bool ReadOperand(Vulkan::Rasterizer* rasterizer, VAddr ea, void* out, u32 size) {
    if (!rasterizer->IsRangeGpuWritten(ea, size)) {
        return runtime_memory_read_backing(ea, out, size) != 0;
    }
    if constexpr (BB_DIAGNOSTICS) BbStats::fault_emulate_gpu.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool FaultEmulation() {
    static const bool on = [] {
        const char* env = std::getenv("BB_FAULT_EMULATE");
        const bool enabled = !(env && env[0] == '0'); // default on; BB_FAULT_EMULATE=0 restores the downloads
        if (enabled) {
            Common::Decoder::Instance(); // created here, once: the singleton itself has no lock
        }
        return enabled;
    }();
    return on;
}

DWORD64* Gpr(CONTEXT* c, ZydisRegister r) {
    switch (r) {
    case ZYDIS_REGISTER_RAX: return &c->Rax;
    case ZYDIS_REGISTER_RBX: return &c->Rbx;
    case ZYDIS_REGISTER_RCX: return &c->Rcx;
    case ZYDIS_REGISTER_RDX: return &c->Rdx;
    case ZYDIS_REGISTER_RSI: return &c->Rsi;
    case ZYDIS_REGISTER_RDI: return &c->Rdi;
    case ZYDIS_REGISTER_RBP: return &c->Rbp;
    case ZYDIS_REGISTER_RSP: return &c->Rsp;
    case ZYDIS_REGISTER_R8: return &c->R8;
    case ZYDIS_REGISTER_R9: return &c->R9;
    case ZYDIS_REGISTER_R10: return &c->R10;
    case ZYDIS_REGISTER_R11: return &c->R11;
    case ZYDIS_REGISTER_R12: return &c->R12;
    case ZYDIS_REGISTER_R13: return &c->R13;
    case ZYDIS_REGISTER_R14: return &c->R14;
    case ZYDIS_REGISTER_R15: return &c->R15;
    default: return nullptr;
    }
}

enum class Load { None, Gpr, GprZero, GprSign, XmmLow, XmmFull, XmmLane };

// Legacy SSE, or VEX with 128-bit vectors (the PS4 code uses AVX moves).
bool Encodable(const ZydisDecodedInstruction& inst) {
    return inst.encoding == ZYDIS_INSTRUCTION_ENCODING_LEGACY ||
           (inst.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX && inst.avx.vector_length == 128);
}

// The upper halves of the YMM registers in the exception context (VEX loads zero them), or null.
M128A* YmmUpper(CONTEXT* ctx) {
    if ((ctx->ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE) {
        return nullptr;
    }
    DWORD length = 0;
    auto* upper = static_cast<M128A*>(LocateXStateFeature(ctx, XSTATE_AVX, &length));
    return upper && length >= 16 * sizeof(M128A) ? upper : nullptr;
}

Load Classify(const ZydisDecodedInstruction& inst) {
    switch (inst.mnemonic) {
    case ZYDIS_MNEMONIC_MOV: return Load::Gpr;
    case ZYDIS_MNEMONIC_MOVZX: return Load::GprZero;
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD: return Load::GprSign;
    case ZYDIS_MNEMONIC_MOVSS:
    case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_MOVD:
    case ZYDIS_MNEMONIC_MOVQ:
    case ZYDIS_MNEMONIC_VMOVSS:
    case ZYDIS_MNEMONIC_VMOVSD:
    case ZYDIS_MNEMONIC_VMOVD:
    case ZYDIS_MNEMONIC_VMOVQ: return Load::XmmLow;
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVDQU:
    case ZYDIS_MNEMONIC_MOVDQA:
    case ZYDIS_MNEMONIC_VMOVUPS:
    case ZYDIS_MNEMONIC_VMOVAPS:
    case ZYDIS_MNEMONIC_VMOVUPD:
    case ZYDIS_MNEMONIC_VMOVAPD:
    case ZYDIS_MNEMONIC_VMOVDQU:
    case ZYDIS_MNEMONIC_VMOVDQA: return Load::XmmFull;
    case ZYDIS_MNEMONIC_EXTRACTPS:
    case ZYDIS_MNEMONIC_VEXTRACTPS: return Load::XmmLane;
    default: return Load::None;
    }
}

constexpr ZydisRegister kGprs[] = {ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RBX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX,
                                   ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_RDI, ZYDIS_REGISTER_RBP, ZYDIS_REGISTER_RSP,
                                   ZYDIS_REGISTER_R8,  ZYDIS_REGISTER_R9,  ZYDIS_REGISTER_R10, ZYDIS_REGISTER_R11,
                                   ZYDIS_REGISTER_R12, ZYDIS_REGISTER_R13, ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R15};


// Compares reading memory: cmp/test with a register or an immediate, and the scalar float compares. The host runs
// the same instruction on the loaded value and the flags it sets go to the guest context.
template <typename T>
u64 HostCmpFlags(T dst, T src, bool test) {
    u64 flags;
    if (test) {
        asm volatile("test %[s], %[d]\n\tpushfq\n\tpopq %[f]" : [f] "=r"(flags) : [d] "r"(dst), [s] "r"(src) : "cc");
    } else {
        asm volatile("cmp %[s], %[d]\n\tpushfq\n\tpopq %[f]" : [f] "=r"(flags) : [d] "r"(dst), [s] "r"(src) : "cc");
    }
    return flags;
}

// add/sub/and/or/xor dst, src on the host: the result in dst and the flags the instruction leaves.
template <typename T>
bool HostAlu(ZydisMnemonic m, T& d, T s, u64& flags) {
    switch (m) {
    case ZYDIS_MNEMONIC_ADD:
        asm volatile("add %[s], %[d]\n\tpushfq\n\tpopq %[f]" : [d] "+r"(d), [f] "=r"(flags) : [s] "r"(s) : "cc");
        return true;
    case ZYDIS_MNEMONIC_SUB:
        asm volatile("sub %[s], %[d]\n\tpushfq\n\tpopq %[f]" : [d] "+r"(d), [f] "=r"(flags) : [s] "r"(s) : "cc");
        return true;
    case ZYDIS_MNEMONIC_AND:
        asm volatile("and %[s], %[d]\n\tpushfq\n\tpopq %[f]" : [d] "+r"(d), [f] "=r"(flags) : [s] "r"(s) : "cc");
        return true;
    case ZYDIS_MNEMONIC_OR:
        asm volatile("or %[s], %[d]\n\tpushfq\n\tpopq %[f]" : [d] "+r"(d), [f] "=r"(flags) : [s] "r"(s) : "cc");
        return true;
    case ZYDIS_MNEMONIC_XOR:
        asm volatile("xor %[s], %[d]\n\tpushfq\n\tpopq %[f]" : [d] "+r"(d), [f] "=r"(flags) : [s] "r"(s) : "cc");
        return true;
    default:
        return false;
    }
}

bool ReadGpr(CONTEXT* ctx, ZydisRegister r, u32 width, u64& value) {
    const bool high = r >= ZYDIS_REGISTER_AH && r <= ZYDIS_REGISTER_BH;
    DWORD64* reg = Gpr(ctx, ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r));
    if (!reg) {
        return false;
    }
    value = high ? (*reg >> 8) & 0xff : *reg;
    if (width < 64) {
        value &= (1ull << width) - 1;
    }
    return true;
}

bool EmulateNonGpuCompare(CONTEXT* ctx, const ZydisDecodedInstruction& inst, const ZydisDecodedOperand* ops,
                          VAddr fault, Vulkan::Rasterizer* rasterizer) {
    const auto m = inst.mnemonic;
    const bool is_cmp = m == ZYDIS_MNEMONIC_CMP, is_test = m == ZYDIS_MNEMONIC_TEST;
    const bool is_fcmp = m == ZYDIS_MNEMONIC_UCOMISS || m == ZYDIS_MNEMONIC_COMISS ||
                         m == ZYDIS_MNEMONIC_UCOMISD || m == ZYDIS_MNEMONIC_COMISD ||
                         m == ZYDIS_MNEMONIC_VUCOMISS || m == ZYDIS_MNEMONIC_VCOMISS ||
                         m == ZYDIS_MNEMONIC_VUCOMISD || m == ZYDIS_MNEMONIC_VCOMISD;
    if ((!is_cmp && !is_test && !is_fcmp) || !Encodable(inst) || inst.operand_count_visible != 2 ||
        (inst.attributes & (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE |
                            ZYDIS_ATTRIB_HAS_REPNE))) {
        return false;
    }
    const int mem = ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? 0 : ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY ? 1 : -1;
    if (mem < 0 || ops[mem].mem.segment == ZYDIS_REGISTER_FS || ops[mem].mem.segment == ZYDIS_REGISTER_GS) {
        return false;
    }
    const auto& memop = ops[mem];
    const auto& other = ops[1 - mem];
    ZydisRegisterContext regs{};
    for (const ZydisRegister r : kGprs) {
        regs.values[r] = *Gpr(ctx, r);
    }
    ZyanU64 ea = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&inst, &memop, ctx->Rip, &regs, &ea))) {
        return false;
    }
    const u32 size = memop.size / 8;
    if (size == 0 || size > 8 || fault < ea || fault >= ea + size) {
        return false;
    }
    u64 loaded = 0;
    if (!ReadOperand(rasterizer, ea, &loaded, size)) {
        return false;
    }
    u64 flags = 0;
    constexpr u64 kArith = 0x8D5; // CF PF AF ZF SF OF
    if (is_fcmp) {
        // ucomis*/comis* xmm, m: the register is the first operand.
        if (mem != 1 || other.type != ZYDIS_OPERAND_TYPE_REGISTER || other.reg.value < ZYDIS_REGISTER_XMM0 ||
            other.reg.value > ZYDIS_REGISTER_XMM15) {
            return false;
        }
        const auto* x = reinterpret_cast<const u8*>(&ctx->Xmm0 + (other.reg.value - ZYDIS_REGISTER_XMM0));
        const bool dbl = m == ZYDIS_MNEMONIC_UCOMISD || m == ZYDIS_MNEMONIC_COMISD || m == ZYDIS_MNEMONIC_VUCOMISD ||
                         m == ZYDIS_MNEMONIC_VCOMISD;
        const bool ordered = m == ZYDIS_MNEMONIC_COMISS || m == ZYDIS_MNEMONIC_COMISD ||
                             m == ZYDIS_MNEMONIC_VCOMISS || m == ZYDIS_MNEMONIC_VCOMISD;
        if (size != (dbl ? 8u : 4u) || (ctx->MxCsr & 0x1F80u) != 0x1F80u) {
            return false; // an unmasked FP exception would be raised in the handler, not at the guest instruction
        }
        const u32 host_csr = _mm_getcsr(); // the guest's DAZ/FTZ apply to the compare
        _mm_setcsr(ctx->MxCsr & ~0x3Fu);
        if (dbl) {
            double a, b;
            std::memcpy(&a, x, 8);
            std::memcpy(&b, &loaded, 8);
            if (ordered) {
                asm volatile("comisd %[b], %[a]\n\tpushfq\n\tpopq %[f]" : [f] "=r"(flags) : [a] "x"(a), [b] "x"(b) : "cc");
            } else {
                asm volatile("ucomisd %[b], %[a]\n\tpushfq\n\tpopq %[f]" : [f] "=r"(flags) : [a] "x"(a), [b] "x"(b) : "cc");
            }
        } else {
            float a, b;
            std::memcpy(&a, x, 4);
            std::memcpy(&b, &loaded, 4);
            if (ordered) {
                asm volatile("comiss %[b], %[a]\n\tpushfq\n\tpopq %[f]" : [f] "=r"(flags) : [a] "x"(a), [b] "x"(b) : "cc");
            } else {
                asm volatile("ucomiss %[b], %[a]\n\tpushfq\n\tpopq %[f]" : [f] "=r"(flags) : [a] "x"(a), [b] "x"(b) : "cc");
            }
        }
        ctx->MxCsr |= _mm_getcsr() & 0x3Fu;
        _mm_setcsr(host_csr);
    } else {
        const u32 width = size * 8;
        u64 value = 0;
        if (other.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            value = other.imm.value.u; // sign-extended to the operand size by the decoder
        } else if (other.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            if (!ReadGpr(ctx, other.reg.value, width, value)) {
                return false;
            }
        } else {
            return false;
        }
        // Intel order: cmp dst, src computes dst - src; dst is operand 0.
        const u64 dst = mem == 0 ? loaded : value;
        const u64 src = mem == 0 ? value : loaded;
        switch (width) {
        case 8: flags = HostCmpFlags<u8>(u8(dst), u8(src), is_test); break;
        case 16: flags = HostCmpFlags<u16>(u16(dst), u16(src), is_test); break;
        case 32: flags = HostCmpFlags<u32>(u32(dst), u32(src), is_test); break;
        case 64: flags = HostCmpFlags<u64>(dst, src, is_test); break;
        default: return false;
        }
    }
    ctx->EFlags = DWORD((ctx->EFlags & ~kArith) | (flags & kArith));
    ctx->Rip += inst.length;
    return true;
}


// Integer arithmetic reading memory into a register: add/sub/and/or/xor r, m (DriveClub: sub rax, [rbx+0x20] at
// 0x85f3b2, a std::vector's begin pointer on a page with GPU-written bytes, ~80 waits/s for a download in races).
bool EmulateNonGpuIntAlu(CONTEXT* ctx, const ZydisDecodedInstruction& inst, const ZydisDecodedOperand* ops,
                         VAddr fault, Vulkan::Rasterizer* rasterizer) {
    const auto m = inst.mnemonic;
    if ((m != ZYDIS_MNEMONIC_ADD && m != ZYDIS_MNEMONIC_SUB && m != ZYDIS_MNEMONIC_AND && m != ZYDIS_MNEMONIC_OR &&
         m != ZYDIS_MNEMONIC_XOR) ||
        inst.encoding != ZYDIS_INSTRUCTION_ENCODING_LEGACY || inst.operand_count_visible != 2 ||
        (inst.attributes & (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE |
                            ZYDIS_ATTRIB_HAS_REPNE))) {
        return false;
    }
    const ZydisDecodedOperand& dst = ops[0];
    const ZydisDecodedOperand& src = ops[1];
    if (dst.type != ZYDIS_OPERAND_TYPE_REGISTER || src.type != ZYDIS_OPERAND_TYPE_MEMORY ||
        src.mem.segment == ZYDIS_REGISTER_FS || src.mem.segment == ZYDIS_REGISTER_GS ||
        (dst.reg.value >= ZYDIS_REGISTER_AH && dst.reg.value <= ZYDIS_REGISTER_BH)) {
        return false;
    }
    DWORD64* reg = Gpr(ctx, ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, dst.reg.value));
    if (!reg) {
        return false;
    }
    ZydisRegisterContext regs{};
    for (const ZydisRegister r : kGprs) {
        regs.values[r] = *Gpr(ctx, r);
    }
    ZyanU64 ea = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&inst, &src, ctx->Rip, &regs, &ea))) {
        return false;
    }
    const u32 size = src.size / 8;
    const u32 width = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, dst.reg.value);
    if (size == 0 || size > 8 || width != size * 8 || fault < ea || fault >= ea + size) {
        return false;
    }
    u64 loaded = 0;
    if (!ReadOperand(rasterizer, ea, &loaded, size)) {
        return false;
    }
    u64 flags = 0;
    u64 result = *reg;
    bool ok = false;
    switch (width) {
    case 8: { u8 d = u8(result); ok = HostAlu<u8>(m, d, u8(loaded), flags); result = (result & ~0xffull) | d; break; }
    case 16: { u16 d = u16(result); ok = HostAlu<u16>(m, d, u16(loaded), flags); result = (result & ~0xffffull) | d; break; }
    case 32: { u32 d = u32(result); ok = HostAlu<u32>(m, d, u32(loaded), flags); result = d; break; } // zero-extends
    case 64: { u64 d = result; ok = HostAlu<u64>(m, d, loaded, flags); result = d; break; }
    default: return false;
    }
    if (!ok) {
        return false;
    }
    constexpr u64 kArith = 0x8D5; // CF PF AF ZF SF OF
    *reg = result;
    ctx->EFlags = DWORD((ctx->EFlags & ~kArith) | (flags & kArith));
    ctx->Rip += inst.length;
    return true;
}

// Float arithmetic reading memory: (v)add/sub/mul/div/min/max ss/sd/ps/pd xmm, m. The host runs the same
// operation under the guest thread's MXCSR (rounding, flush-to-zero, denormals-are-zero); the sticky exception
// flags it raises go to the guest MXCSR.
enum class ArithOp { Add, Sub, Mul, Div, Min, Max };
struct ArithForm {
    ArithOp op;
    u8 bytes;  // memory operand size: 4 ss, 8 sd, 16 ps/pd
    bool dbl;
    bool vex;
};

bool ClassifyArith(ZydisMnemonic m, ArithForm& f) {
    struct Entry {
        ZydisMnemonic legacy, vex;
        ArithOp op;
        u8 bytes;
        bool dbl;
    };
    static constexpr Entry table[] = {
        {ZYDIS_MNEMONIC_ADDSS, ZYDIS_MNEMONIC_VADDSS, ArithOp::Add, 4, false},
        {ZYDIS_MNEMONIC_SUBSS, ZYDIS_MNEMONIC_VSUBSS, ArithOp::Sub, 4, false},
        {ZYDIS_MNEMONIC_MULSS, ZYDIS_MNEMONIC_VMULSS, ArithOp::Mul, 4, false},
        {ZYDIS_MNEMONIC_DIVSS, ZYDIS_MNEMONIC_VDIVSS, ArithOp::Div, 4, false},
        {ZYDIS_MNEMONIC_MINSS, ZYDIS_MNEMONIC_VMINSS, ArithOp::Min, 4, false},
        {ZYDIS_MNEMONIC_MAXSS, ZYDIS_MNEMONIC_VMAXSS, ArithOp::Max, 4, false},
        {ZYDIS_MNEMONIC_ADDSD, ZYDIS_MNEMONIC_VADDSD, ArithOp::Add, 8, true},
        {ZYDIS_MNEMONIC_SUBSD, ZYDIS_MNEMONIC_VSUBSD, ArithOp::Sub, 8, true},
        {ZYDIS_MNEMONIC_MULSD, ZYDIS_MNEMONIC_VMULSD, ArithOp::Mul, 8, true},
        {ZYDIS_MNEMONIC_DIVSD, ZYDIS_MNEMONIC_VDIVSD, ArithOp::Div, 8, true},
        {ZYDIS_MNEMONIC_MINSD, ZYDIS_MNEMONIC_VMINSD, ArithOp::Min, 8, true},
        {ZYDIS_MNEMONIC_MAXSD, ZYDIS_MNEMONIC_VMAXSD, ArithOp::Max, 8, true},
        {ZYDIS_MNEMONIC_ADDPS, ZYDIS_MNEMONIC_VADDPS, ArithOp::Add, 16, false},
        {ZYDIS_MNEMONIC_SUBPS, ZYDIS_MNEMONIC_VSUBPS, ArithOp::Sub, 16, false},
        {ZYDIS_MNEMONIC_MULPS, ZYDIS_MNEMONIC_VMULPS, ArithOp::Mul, 16, false},
        {ZYDIS_MNEMONIC_DIVPS, ZYDIS_MNEMONIC_VDIVPS, ArithOp::Div, 16, false},
        {ZYDIS_MNEMONIC_MINPS, ZYDIS_MNEMONIC_VMINPS, ArithOp::Min, 16, false},
        {ZYDIS_MNEMONIC_MAXPS, ZYDIS_MNEMONIC_VMAXPS, ArithOp::Max, 16, false},
        {ZYDIS_MNEMONIC_ADDPD, ZYDIS_MNEMONIC_VADDPD, ArithOp::Add, 16, true},
        {ZYDIS_MNEMONIC_SUBPD, ZYDIS_MNEMONIC_VSUBPD, ArithOp::Sub, 16, true},
        {ZYDIS_MNEMONIC_MULPD, ZYDIS_MNEMONIC_VMULPD, ArithOp::Mul, 16, true},
        {ZYDIS_MNEMONIC_DIVPD, ZYDIS_MNEMONIC_VDIVPD, ArithOp::Div, 16, true},
        {ZYDIS_MNEMONIC_MINPD, ZYDIS_MNEMONIC_VMINPD, ArithOp::Min, 16, true},
        {ZYDIS_MNEMONIC_MAXPD, ZYDIS_MNEMONIC_VMAXPD, ArithOp::Max, 16, true},
    };
    for (const auto& e : table) {
        if (m == e.legacy || m == e.vex) {
            f = {e.op, e.bytes, e.dbl, m == e.vex};
            return true;
        }
    }
    return false;
}

template <typename V, typename AddF, typename SubF, typename MulF, typename DivF, typename MinF, typename MaxF>
V RunArith(ArithOp op, V a, V b, AddF add, SubF sub, MulF mul, DivF div, MinF min, MaxF max) {
    switch (op) {
    case ArithOp::Add: return add(a, b);
    case ArithOp::Sub: return sub(a, b);
    case ArithOp::Mul: return mul(a, b);
    case ArithOp::Div: return div(a, b);
    case ArithOp::Min: return min(a, b);
    case ArithOp::Max: return max(a, b);
    }
    return a;
}

bool EmulateNonGpuArith(CONTEXT* ctx, const ZydisDecodedInstruction& inst, const ZydisDecodedOperand* ops,
                        VAddr fault, Vulkan::Rasterizer* rasterizer) {
    ArithForm f;
    if (!ClassifyArith(inst.mnemonic, f) || !Encodable(inst) ||
        (inst.attributes & (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE |
                            ZYDIS_ATTRIB_HAS_REPNE))) {
        return false;
    }
    const u32 visible = f.vex ? 3 : 2;
    if (inst.operand_count_visible != visible) {
        return false;
    }
    const ZydisDecodedOperand& dst = ops[0];
    const ZydisDecodedOperand& src1 = f.vex ? ops[1] : ops[0];
    const ZydisDecodedOperand& mem = ops[visible - 1];
    const auto is_xmm = [](const ZydisDecodedOperand& o) {
        return o.type == ZYDIS_OPERAND_TYPE_REGISTER && o.reg.value >= ZYDIS_REGISTER_XMM0 &&
               o.reg.value <= ZYDIS_REGISTER_XMM15;
    };
    if (!is_xmm(dst) || !is_xmm(src1) || mem.type != ZYDIS_OPERAND_TYPE_MEMORY ||
        mem.mem.segment == ZYDIS_REGISTER_FS || mem.mem.segment == ZYDIS_REGISTER_GS || mem.size / 8 != f.bytes) {
        return false;
    }
    M128A* const ymm_upper = f.vex ? YmmUpper(ctx) : nullptr;
    if ((f.vex && !ymm_upper) || (ctx->MxCsr & 0x1F80u) != 0x1F80u) {
        return false; // unmasked FP exceptions: the original instruction raises them after a download
    }
    ZydisRegisterContext regs{};
    for (const ZydisRegister r : kGprs) {
        regs.values[r] = *Gpr(ctx, r);
    }
    ZyanU64 ea = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&inst, &mem, ctx->Rip, &regs, &ea))) {
        return false;
    }
    if (fault < ea || fault >= ea + f.bytes) {
        return false;
    }
    alignas(16) u8 loaded[16]{};
    if (!ReadOperand(rasterizer, ea, loaded, f.bytes)) {
        return false;
    }
    alignas(16) u8 a_bytes[16];
    std::memcpy(a_bytes, &ctx->Xmm0 + (src1.reg.value - ZYDIS_REGISTER_XMM0), 16);
    alignas(16) u8 out[16];
    const u32 host_csr = _mm_getcsr();
    _mm_setcsr(ctx->MxCsr & ~0x3Fu);
    if (f.dbl) {
        const __m128d a = _mm_load_pd(reinterpret_cast<const double*>(a_bytes));
        const __m128d b = _mm_load_pd(reinterpret_cast<const double*>(loaded));
        __m128d r;
        if (f.bytes == 16) {
            r = RunArith(f.op, a, b, [](auto x, auto y) { return _mm_add_pd(x, y); }, [](auto x, auto y) { return _mm_sub_pd(x, y); }, [](auto x, auto y) { return _mm_mul_pd(x, y); }, [](auto x, auto y) { return _mm_div_pd(x, y); }, [](auto x, auto y) { return _mm_min_pd(x, y); }, [](auto x, auto y) { return _mm_max_pd(x, y); });
        } else {
            r = RunArith(f.op, a, b, [](auto x, auto y) { return _mm_add_sd(x, y); }, [](auto x, auto y) { return _mm_sub_sd(x, y); }, [](auto x, auto y) { return _mm_mul_sd(x, y); }, [](auto x, auto y) { return _mm_div_sd(x, y); }, [](auto x, auto y) { return _mm_min_sd(x, y); }, [](auto x, auto y) { return _mm_max_sd(x, y); });
        }
        _mm_store_pd(reinterpret_cast<double*>(out), r);
    } else {
        const __m128 a = _mm_load_ps(reinterpret_cast<const float*>(a_bytes));
        const __m128 b = _mm_load_ps(reinterpret_cast<const float*>(loaded));
        __m128 r;
        if (f.bytes == 16) {
            r = RunArith(f.op, a, b, [](auto x, auto y) { return _mm_add_ps(x, y); }, [](auto x, auto y) { return _mm_sub_ps(x, y); }, [](auto x, auto y) { return _mm_mul_ps(x, y); }, [](auto x, auto y) { return _mm_div_ps(x, y); }, [](auto x, auto y) { return _mm_min_ps(x, y); }, [](auto x, auto y) { return _mm_max_ps(x, y); });
        } else {
            r = RunArith(f.op, a, b, [](auto x, auto y) { return _mm_add_ss(x, y); }, [](auto x, auto y) { return _mm_sub_ss(x, y); }, [](auto x, auto y) { return _mm_mul_ss(x, y); }, [](auto x, auto y) { return _mm_div_ss(x, y); }, [](auto x, auto y) { return _mm_min_ss(x, y); }, [](auto x, auto y) { return _mm_max_ss(x, y); });
        }
        _mm_store_ps(reinterpret_cast<float*>(out), r);
    }
    const u32 raised = _mm_getcsr() & 0x3Fu;
    _mm_setcsr(host_csr);
    ctx->MxCsr |= raised;
    // Scalar forms keep the upper lanes of the first source (dst for legacy SSE, src1 for VEX) via the _ss/_sd
    // intrinsics; VEX.128 forms zero bits 255:128.
    const u32 index = dst.reg.value - ZYDIS_REGISTER_XMM0;
    std::memcpy(&ctx->Xmm0 + index, out, 16);
    if (f.vex) {
        std::memset(ymm_upper + index, 0, sizeof(M128A));
    }
    ctx->Rip += inst.length;
    return true;
}

// bbport: BB_FAULT_EMULATE_LANES (default on, =0 off). Lane selects and bitwise ops reading memory: blendps/pd,
// pblendw, vpblendd, shufps, insertps and and/andn/or/xor (ps, pd, integer), legacy SSE and VEX.128. None raises an FP
// exception. At 4K, eboot FUN_00611de0 (per-view setup; 0x61203d: vblendps keeps the w of a vec4 it rewrites) reads
// CPU bytes of a page whose other 768 bytes cs c6edad8b writes; not emulated, each read waited for the end of the
// GPU frame (~12 ms, ~21/s: the 4K alternation).
enum class LaneOp { Blend16, Blend32, Blend64, Shuf32, Insert32, And, AndN, Or, Xor };
struct LaneForm {
    LaneOp op;
    u8 bytes; // memory operand size
    bool imm;
    bool vex;
};

bool ClassifyLane(ZydisMnemonic m, LaneForm& f) {
    struct Entry {
        ZydisMnemonic legacy, vex;
        LaneOp op;
        u8 bytes;
        bool imm;
    };
    static constexpr Entry table[] = {
        {ZYDIS_MNEMONIC_BLENDPS, ZYDIS_MNEMONIC_VBLENDPS, LaneOp::Blend32, 16, true},
        {ZYDIS_MNEMONIC_BLENDPD, ZYDIS_MNEMONIC_VBLENDPD, LaneOp::Blend64, 16, true},
        {ZYDIS_MNEMONIC_PBLENDW, ZYDIS_MNEMONIC_VPBLENDW, LaneOp::Blend16, 16, true},
        {ZYDIS_MNEMONIC_INVALID, ZYDIS_MNEMONIC_VPBLENDD, LaneOp::Blend32, 16, true},
        {ZYDIS_MNEMONIC_SHUFPS, ZYDIS_MNEMONIC_VSHUFPS, LaneOp::Shuf32, 16, true},
        {ZYDIS_MNEMONIC_INSERTPS, ZYDIS_MNEMONIC_VINSERTPS, LaneOp::Insert32, 4, true},
        {ZYDIS_MNEMONIC_ANDPS, ZYDIS_MNEMONIC_VANDPS, LaneOp::And, 16, false},
        {ZYDIS_MNEMONIC_ANDPD, ZYDIS_MNEMONIC_VANDPD, LaneOp::And, 16, false},
        {ZYDIS_MNEMONIC_PAND, ZYDIS_MNEMONIC_VPAND, LaneOp::And, 16, false},
        {ZYDIS_MNEMONIC_ANDNPS, ZYDIS_MNEMONIC_VANDNPS, LaneOp::AndN, 16, false},
        {ZYDIS_MNEMONIC_ANDNPD, ZYDIS_MNEMONIC_VANDNPD, LaneOp::AndN, 16, false},
        {ZYDIS_MNEMONIC_PANDN, ZYDIS_MNEMONIC_VPANDN, LaneOp::AndN, 16, false},
        {ZYDIS_MNEMONIC_ORPS, ZYDIS_MNEMONIC_VORPS, LaneOp::Or, 16, false},
        {ZYDIS_MNEMONIC_ORPD, ZYDIS_MNEMONIC_VORPD, LaneOp::Or, 16, false},
        {ZYDIS_MNEMONIC_POR, ZYDIS_MNEMONIC_VPOR, LaneOp::Or, 16, false},
        {ZYDIS_MNEMONIC_XORPS, ZYDIS_MNEMONIC_VXORPS, LaneOp::Xor, 16, false},
        {ZYDIS_MNEMONIC_XORPD, ZYDIS_MNEMONIC_VXORPD, LaneOp::Xor, 16, false},
        {ZYDIS_MNEMONIC_PXOR, ZYDIS_MNEMONIC_VPXOR, LaneOp::Xor, 16, false},
    };
    for (const auto& e : table) {
        if ((e.legacy != ZYDIS_MNEMONIC_INVALID && m == e.legacy) || m == e.vex) {
            f = {e.op, e.bytes, e.imm, m == e.vex};
            return true;
        }
    }
    return false;
}

bool LaneEmulation() {
    static const bool on = [] {
        const char* env = std::getenv("BB_FAULT_EMULATE_LANES");
        return !(env && env[0] == '0');
    }();
    return on;
}

bool EmulateNonGpuLane(CONTEXT* ctx, const ZydisDecodedInstruction& inst, const ZydisDecodedOperand* ops,
                       VAddr fault, Vulkan::Rasterizer* rasterizer) {
    LaneForm f;
    if (!LaneEmulation() || !ClassifyLane(inst.mnemonic, f) || !Encodable(inst) ||
        (inst.attributes & (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE |
                            ZYDIS_ATTRIB_HAS_REPNE))) {
        return false;
    }
    const u32 regs_visible = f.vex ? 3 : 2; // dst, (VEX: src1), memory
    if (inst.operand_count_visible != regs_visible + (f.imm ? 1 : 0)) {
        return false;
    }
    const ZydisDecodedOperand& dst = ops[0];
    const ZydisDecodedOperand& src1 = f.vex ? ops[1] : ops[0];
    const ZydisDecodedOperand& mem = ops[regs_visible - 1];
    const auto is_xmm = [](const ZydisDecodedOperand& o) {
        return o.type == ZYDIS_OPERAND_TYPE_REGISTER && o.reg.value >= ZYDIS_REGISTER_XMM0 &&
               o.reg.value <= ZYDIS_REGISTER_XMM15;
    };
    if (!is_xmm(dst) || !is_xmm(src1) || mem.type != ZYDIS_OPERAND_TYPE_MEMORY ||
        mem.mem.segment == ZYDIS_REGISTER_FS || mem.mem.segment == ZYDIS_REGISTER_GS || mem.size / 8 != f.bytes ||
        (f.imm && ops[regs_visible].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)) {
        return false;
    }
    M128A* const ymm_upper = f.vex ? YmmUpper(ctx) : nullptr;
    if (f.vex && !ymm_upper) {
        return false;
    }
    ZydisRegisterContext regs{};
    for (const ZydisRegister r : kGprs) {
        regs.values[r] = *Gpr(ctx, r);
    }
    ZyanU64 ea = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&inst, &mem, ctx->Rip, &regs, &ea))) {
        return false;
    }
    if (fault < ea || fault >= ea + f.bytes) {
        return false;
    }
    alignas(16) u8 b[16]{};
    if (!ReadOperand(rasterizer, ea, b, f.bytes)) {
        return false;
    }
    alignas(16) u8 a[16];
    std::memcpy(a, &ctx->Xmm0 + (src1.reg.value - ZYDIS_REGISTER_XMM0), 16);
    const u32 imm = f.imm ? u32(ops[regs_visible].imm.value.u & 0xff) : 0;
    alignas(16) u8 out[16];
    std::memcpy(out, a, 16);
    const auto lanes = [&](u32 width, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            if (imm & (1u << i)) {
                std::memcpy(out + i * width, b + i * width, width);
            }
        }
    };
    switch (f.op) {
    case LaneOp::Blend16: lanes(2, 8); break;
    case LaneOp::Blend32: lanes(4, 4); break;
    case LaneOp::Blend64: lanes(8, 2); break;
    case LaneOp::Shuf32:
        std::memcpy(out + 0, a + 4 * (imm & 3), 4);
        std::memcpy(out + 4, a + 4 * ((imm >> 2) & 3), 4);
        std::memcpy(out + 8, b + 4 * ((imm >> 4) & 3), 4);
        std::memcpy(out + 12, b + 4 * ((imm >> 6) & 3), 4);
        break;
    case LaneOp::Insert32: // memory form: the 32-bit operand goes to lane imm[5:4], then imm[3:0] zeroes lanes
        std::memcpy(out + 4 * ((imm >> 4) & 3), b, 4);
        for (u32 i = 0; i < 4; ++i) {
            if (imm & (1u << i)) {
                std::memset(out + 4 * i, 0, 4);
            }
        }
        break;
    case LaneOp::And:
    case LaneOp::AndN:
    case LaneOp::Or:
    case LaneOp::Xor:
        for (u32 i = 0; i < 16; ++i) {
            out[i] = f.op == LaneOp::And    ? u8(a[i] & b[i])
                     : f.op == LaneOp::AndN ? u8(~a[i] & b[i])
                     : f.op == LaneOp::Or   ? u8(a[i] | b[i])
                                            : u8(a[i] ^ b[i]);
        }
        break;
    }
    const u32 index = dst.reg.value - ZYDIS_REGISTER_XMM0;
    std::memcpy(&ctx->Xmm0 + index, out, 16);
    if (f.vex) {
        std::memset(ymm_upper + index, 0, sizeof(M128A)); // VEX.128 zeroes bits 255:128
    }
    ctx->Rip += inst.length;
    return true;
}

bool EmulateNonGpuLoad(void* context, VAddr fault, Vulkan::Rasterizer* rasterizer) {
    CONTEXT* ctx = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord;
    ZydisDecodedInstruction inst;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
            inst, ops, reinterpret_cast<void*>(ctx->Rip), 15))) {
        return false;
    }
    const Load kind = Classify(inst);
    if (kind == Load::None) {
        return EmulateNonGpuCompare(ctx, inst, ops, fault, rasterizer) ||
               EmulateNonGpuArith(ctx, inst, ops, fault, rasterizer) ||
               EmulateNonGpuIntAlu(ctx, inst, ops, fault, rasterizer) ||
               EmulateNonGpuLane(ctx, inst, ops, fault, rasterizer);
    }
    if (kind == Load::XmmLane || !Encodable(inst) || inst.operand_count_visible != 2 ||
        (inst.attributes & (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE |
                            ZYDIS_ATTRIB_HAS_REPNE))) {
        return false;
    }
    const bool vex = inst.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX;
    M128A* const ymm_upper = vex ? YmmUpper(ctx) : nullptr;
    if (vex && !ymm_upper) {
        return false;
    }
    const ZydisDecodedOperand& dst = ops[0];
    const ZydisDecodedOperand& src = ops[1];
    if (dst.type != ZYDIS_OPERAND_TYPE_REGISTER || src.type != ZYDIS_OPERAND_TYPE_MEMORY ||
        src.mem.segment == ZYDIS_REGISTER_FS || src.mem.segment == ZYDIS_REGISTER_GS) {
        return false;
    }
    const bool to_xmm = dst.reg.value >= ZYDIS_REGISTER_XMM0 && dst.reg.value <= ZYDIS_REGISTER_XMM15;
    if ((kind == Load::XmmLow || kind == Load::XmmFull) != to_xmm) {
        return false;
    }
    ZydisRegisterContext regs{};
    for (const ZydisRegister r : kGprs) {
        regs.values[r] = *Gpr(ctx, r);
    }
    ZyanU64 ea = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&inst, &src, ctx->Rip, &regs, &ea))) {
        return false;
    }
    const u32 size = src.size / 8;
    if (size == 0 || size > 16 || (size > 8 && !to_xmm) || fault < ea || fault >= ea + size) {
        return false;
    }
    alignas(16) u8 data[16]{};
    if (!ReadOperand(rasterizer, ea, data, size)) {
        return false;
    }
    if (to_xmm) {
        // Loads into the low part of an XMM register zero the rest (legacy SSE load forms).
        const u32 index = dst.reg.value - ZYDIS_REGISTER_XMM0;
        std::memcpy(&ctx->Xmm0 + index, data, 16);
        if (vex) {
            std::memset(ymm_upper + index, 0, sizeof(M128A)); // VEX.128 loads zero bits 255:128
        }
    } else {
        if (dst.reg.value >= ZYDIS_REGISTER_AH && dst.reg.value <= ZYDIS_REGISTER_BH) {
            return false;
        }
        const ZydisRegister full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, dst.reg.value);
        DWORD64* reg = Gpr(ctx, full);
        if (!reg) {
            return false;
        }
        u64 v = 0;
        std::memcpy(&v, data, size);
        if (kind == Load::GprSign && size < 8) {
            const u32 shift = 64 - size * 8;
            v = u64(s64(v << shift) >> shift);
        }
        switch (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, dst.reg.value)) {
        case 64: *reg = v; break;
        case 32: *reg = u32(v); break; // 32-bit destinations zero the upper half
        case 16: *reg = (*reg & ~0xffffull) | (v & 0xffff); break;
        case 8: *reg = (*reg & ~0xffull) | (v & 0xff); break;
        default: return false;
        }
    }
    ctx->Rip += inst.length;
    return true;
}

// Stores: mov m, r / mov m, imm and the SSE stores of the low part or the whole XMM register. The bytes are
// collected here; the GPU thread checks them and does the store (Rasterizer::StoreUntracked).
bool EmulateNonGpuStore(void* context, VAddr fault, Vulkan::Rasterizer* rasterizer) {
    CONTEXT* ctx = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord;
    ZydisDecodedInstruction inst;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
            inst, ops, reinterpret_cast<void*>(ctx->Rip), 15))) {
        if constexpr (BB_DIAGNOSTICS) BbStats::store_why[0].fetch_add(1, std::memory_order_relaxed);
        if (BbStats::enabled) {
            BbStats::NoteStoreSkip(ctx->Rip);
        }
        return false;
    }
    const Load kind = Classify(inst);
    if (kind == Load::None || kind == Load::GprZero || kind == Load::GprSign || !Encodable(inst) ||
        inst.operand_count_visible != (kind == Load::XmmLane ? 3 : 2) ||
        (inst.attributes & (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE |
                            ZYDIS_ATTRIB_HAS_REPNE))) {
        if constexpr (BB_DIAGNOSTICS) BbStats::store_why[0].fetch_add(1, std::memory_order_relaxed);
        if (BbStats::enabled) {
            BbStats::NoteStoreSkip(ctx->Rip);
        }
        return false;
    }
    const ZydisDecodedOperand& dst = ops[0];
    const ZydisDecodedOperand& src = ops[1];
    if (dst.type != ZYDIS_OPERAND_TYPE_MEMORY || dst.mem.segment == ZYDIS_REGISTER_FS ||
        dst.mem.segment == ZYDIS_REGISTER_GS) {
        if constexpr (BB_DIAGNOSTICS) BbStats::store_why[1].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ZydisRegisterContext regs{};
    for (const ZydisRegister r : kGprs) {
        regs.values[r] = *Gpr(ctx, r);
    }
    ZyanU64 ea = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&inst, &dst, ctx->Rip, &regs, &ea))) {
        return false;
    }
    const u32 size = dst.size / 8;
    if (size == 0 || size > 16 || fault < ea || fault >= ea + size) {
        if constexpr (BB_DIAGNOSTICS) BbStats::store_why[2].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    alignas(16) u8 data[16]{};
    if (src.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        if (kind != Load::Gpr || size > 8) {
            return false;
        }
        const u64 v = src.imm.value.u; // sign-extended to the operand size by the decoder
        std::memcpy(data, &v, size);
    } else if (src.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const ZydisRegister r = src.reg.value;
        if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM15) {
            if (kind != Load::XmmLow && kind != Load::XmmFull && kind != Load::XmmLane) {
                return false;
            }
            const auto* xmm = reinterpret_cast<const u8*>(&ctx->Xmm0 + (r - ZYDIS_REGISTER_XMM0));
            if (kind == Load::XmmLane) {
                if (size != 4 || ops[2].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    return false;
                }
                std::memcpy(data, xmm + 4 * (ops[2].imm.value.u & 3), 4);
            } else {
                std::memcpy(data, xmm, size);
            }
        } else {
            if (kind != Load::Gpr || size > 8) {
                return false;
            }
            DWORD64* reg = Gpr(ctx, ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r));
            if (!reg) {
                return false;
            }
            u64 v = *reg;
            if (r >= ZYDIS_REGISTER_AH && r <= ZYDIS_REGISTER_BH) {
                v >>= 8;
            }
            std::memcpy(data, &v, size);
        }
    } else {
        if constexpr (BB_DIAGNOSTICS) BbStats::store_why[3].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!rasterizer->StoreUntracked(ea, data, size)) {
        return false;
    }
    ctx->Rip += inst.length;
    return true;
}
} // namespace
#endif

// bbport: the rasterizer for bbgpu_guest_read/write (Impl is private to PageManager).
Vulkan::Rasterizer* g_memops_rasterizer = nullptr;

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers : 7;
        // At the moment only buffer cache can request read watchers.
        // And buffers cannot overlap, thus only 1 can exist per page.
        u8 num_read_watchers : 1;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <s32 delta, bool is_read>
        u8 AddDelta() {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;

    Impl() = default;
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        // bbport: the draw recording thread handles its faults inline too (vk_draw_pipe.h).
        const auto is_gpu_thread = rasterizer->IsGpuSideThread();
        if (is_gpu_thread) {
            if constexpr (BB_DIAGNOSTICS) BbStats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
        }
        if (Common::IsWriteError(context)) {
            if (is_gpu_thread && BbStats::enabled) {
                BbStats::NoteGpuReadFault(Common::FaultInstructionPointer(context) | (1ull << 63));
            }
            BbStats::Timer timer{BbStats::t_write_faults};
            // The readback log names the faulting instruction for writes too (it kept the last read's before).
            BbTrace::fault_rip = Common::FaultInstructionPointer(context);
#ifdef _WIN64
            // Only faults on pages holding GPU data: CPU write tracking faults stay on the cheap path.
            if (!is_gpu_thread && FaultEmulation() && rasterizer->IsPageGpuModified(addr) &&
                PageEmulationBudgetLeft(addr)) {
                if (EmulateNonGpuStore(context, addr, rasterizer)) {
                    if constexpr (BB_DIAGNOSTICS) BbStats::store_emulated.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                if constexpr (BB_DIAGNOSTICS) BbStats::store_emulate_declined.fetch_add(1, std::memory_order_relaxed);
            }
#endif
            return rasterizer->OnWriteFault(addr, is_gpu_thread);
        } else {
            if constexpr (BB_DIAGNOSTICS) BbStats::read_faults.fetch_add(1, std::memory_order_relaxed);
            if (is_gpu_thread && BbStats::enabled) {
                BbStats::NoteGpuReadFault(Common::FaultInstructionPointer(context));
            }
            BbStats::Timer timer{BbStats::t_read_faults};
            BbTrace::fault_rip = Common::FaultInstructionPointer(context);
#ifdef _WIN64
            BbTrace::fault_ret = *reinterpret_cast<const u64*>(((EXCEPTION_POINTERS*)context)->ContextRecord->Rsp);
            // Only read faults from GPU read protection (a page holding GPU data).
            if (!is_gpu_thread && FaultEmulation() && rasterizer->IsPageGpuModified(addr) &&
                PageEmulationBudgetLeft(addr)) {
                if (EmulateNonGpuLoad(context, addr, rasterizer)) {
                    if constexpr (BB_DIAGNOSTICS) BbStats::fault_emulated.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                if constexpr (BB_DIAGNOSTICS) BbStats::fault_emulate_declined.fetch_add(1, std::memory_order_relaxed);
                // Declined loads are listed with the store skips (bit 63 set marks a load).
                if (BbStats::enabled) {
                    BbStats::NoteStoreSkip(Common::FaultInstructionPointer(context) | (1ull << 63));
                }
            }
#endif
            if (BbTrace::enabled) {
                const s64 start = BbTrace::NowNs();
                const bool handled = rasterizer->ReadMemory(addr, 8, is_gpu_thread);
                BbTrace::NoteReadFault(start, u64(BbTrace::NowNs() - start));
                return handled;
            }
            return rasterizer->ReadMemory(addr, 8, is_gpu_thread);
        }
        return false;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

#ifdef __linux__
// bbport: write tracking with userfaultfd write-protection instead of mprotect. mprotect takes
// the address space lock for writing and splits mappings; while guest threads fault on
// per-frame buffers and the GPU thread re-protects uploaded pages, every page fault in the
// process (copy threads included) waits for it. UFFDIO_WRITEPROTECT changes page table bits
// under the lock for reading. Read protection (readbacks) still uses mprotect: userfaultfd
// write-protection cannot deny reads. BB_UFFD=1.
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;
    std::mutex read_revoked_mutex;
    std::unordered_set<u64> read_revoked_pages; ///< 4 KiB pages denied reads with mprotect
    std::atomic<size_t> num_read_revoked{0};

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        g_memops_rasterizer = rasterizer_; // bbgpu_guest_read/write
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Guest memory is a shared memfd mapping: write-protection there needs the shmem
        // feature, and unpopulated pages must be protectable too.
        uffdio_api api{};
        api.api = UFFD_API;
        api.features =
            UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            close(uffd);
            throw std::runtime_error("uffdio_api");
        }

        // Read faults (readbacks) still arrive as signals.
        Core::Signals::Instance()->RegisterAccessViolationHandler(
            GuestFaultSignalHandler, std::numeric_limits<u32>::min());

        ufd_thread = std::jthread([this](std::stop_token token) { UffdHandler(token); });
        std::printf("GPU: memory tracking with userfaultfd write-protection\n");
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        uffdio_register reg{};
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio register {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range{};
        range.start = address;
        range.len = size;
        ioctl(uffd, UFFDIO_UNREGISTER, &range);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        auto& address_space = Core::Memory::Instance()->GetAddressSpace();
        const bool allow_read = True(perms & Core::MemoryPermission::Read);
        const bool allow_write = True(perms & Core::MemoryPermission::Write);
        const u64 first = address >> 12, last = (address + size + 4095) >> 12;
        if (!allow_read) {
            // Readbacks: deny all access with mprotect.
            address_space.Protect(address, size, Core::MemoryPermission::None);
            std::scoped_lock lk{read_revoked_mutex};
            for (u64 page = first; page < last; ++page) {
                read_revoked_pages.insert(page);
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            return;
        }
        if (num_read_revoked.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lk{read_revoked_mutex};
            bool restore = false;
            for (u64 page = first; page < last; ++page) {
                restore |= read_revoked_pages.erase(page) != 0;
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            if (restore) {
                address_space.Protect(address, size, Core::MemoryPermission::ReadWrite);
            }
        }
        uffdio_writeprotect wp{};
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio writeprotect {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("bb:Uffd");
        while (!token.stop_requested()) {
            pollfd pollfd{};
            pollfd.fd = uffd;
            pollfd.events = POLLIN;
            // Short timeout so the stop request is seen.
            const int pollres = poll(&pollfd, 1, 100);
            if (pollres <= 0 || !(pollfd.revents & POLLIN)) {
                continue;
            }
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret != sizeof(msg)) {
                continue;
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            {
                BbStats::Timer timer{BbStats::t_write_faults};
                rasterizer->OnWriteFault(addr, rasterizer->IsGpuSideThreadId(ptid));
            }
            // Protect() clears with DONTWAKE (it may run for pages nobody waits on).
            uffdio_range wake{};
            wake.start = addr & ~u64(PM_PAGE_SIZE - 1);
            wake.len = PM_PAGE_SIZE;
            ioctl(uffd, UFFDIO_WAKE, &wake);
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        g_memops_rasterizer = rasterizer_; // bbgpu_guest_read/write

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __linux__
    if (std::getenv("BB_UFFD") && std::getenv("BB_UFFD")[0] == '1') {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

template <bool track>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, false>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore

// bbport: BB_GUEST_MEMOPS (default on, =0 off). The guest's memcpy/memset/memmove/memcmp are the host CRT's
// (src/runtime.c). On a page that also holds GPU-written bytes, the CRT's wide stores and loads (AVX, rep stos)
// fault and cannot be emulated, so every such call waited for a download of the page's GPU data even when it
// touched none of those bytes (DriveClub: memsets of ~90 KB at 0x4005730020 and copies next to GPU-written
// counters, ~40% of the main thread's waits in day races). Ranges with no GPU-written byte are done here through
// the unprotected backing view, stores queued for the GPU copy like emulated stores (Rasterizer::StoreUntracked).
// 0 from these: not handled (no GPU page involved, or GPU-written bytes): the caller runs the CRT function.
namespace {
bool GuestMemops() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GUEST_MEMOPS");
        // Default on: +4% uncapped at 720p render on an RTX 3070 Laptop; neutral at a 60 FPS cap (the waits moved
        // to the trueSKY pages).
        return !(env && env[0] == '0');
    }();
    return on;
}
bool AnyGpuPage(Vulkan::Rasterizer* r, VAddr addr, u64 size) {
    for (VAddr page = Common::AlignDown(addr, 4096); page < addr + size; page += 4096) {
        if (r->IsPageGpuModified(page)) {
            return true;
        }
    }
    return false;
}
} // namespace

extern "C" int bbgpu_guest_read(void* out, uintptr_t addr, uint64_t size) {
    auto* r = VideoCore::g_memops_rasterizer;
    if (!r || !GuestMemops() || size == 0 || r->IsGpuSideThread() || !AnyGpuPage(r, addr, size) ||
        r->IsRangeGpuWritten(addr, size)) {
        return 0;
    }
    return runtime_memory_read_backing(addr, out, size) ? 1 : 0;
}

extern "C" int bbgpu_guest_write(uintptr_t addr, const void* data, uint64_t size) {
    auto* r = VideoCore::g_memops_rasterizer;
    if (!r || !GuestMemops() || size == 0 || r->IsGpuSideThread() || !AnyGpuPage(r, addr, size) ||
        r->IsRangeGpuWritten(addr, size)) {
        return 0;
    }
    const auto* src = static_cast<const u8*>(data);
    VAddr at = addr;
    u64 left = size;
    while (left) {
        const u64 n = std::min<u64>(left, Common::AlignDown(at, 4096) + 4096 - at);
        if (r->IsPageGpuModified(at)) {
            if (!r->StoreUntracked(at, src, u32(n))) {
                // Not eligible after all (a download in flight, ...): the rest takes the usual write faults.
                std::memcpy(reinterpret_cast<void*>(at), src, left);
                return 1;
            }
        } else {
            std::memcpy(reinterpret_cast<void*>(at), src, n); // no GPU data on this page: at most a tracking fault
        }
        at += n;
        src += n;
        left -= n;
    }
    return 1;
}

// 1 when a guest memory operation over the range may need the backing path (memops on, a page with GPU data).
extern "C" int bbgpu_guest_gpu_pages(uintptr_t addr, uint64_t size) {
    auto* r = VideoCore::g_memops_rasterizer;
    return r && GuestMemops() && size != 0 && !r->IsGpuSideThread() && AnyGpuPage(r, addr, size);
}

// bbport: BB_EXPOSURE_STALE=1 (opt-in; src/probe.c retargets the calls): DriveClub's auto-exposure (eboot
// FUN_00702750) asks the readback getter for its two luminance histograms in latency mode and copies each one (1 KB)
// with memcpy at 0x702ad2 / 0x702afc. The game expects them 2-3 frames old (it reads in latency mode), but
// here the GPU runs ~3 submissions behind the recording, so each copy waited for the GPU on the main thread. While a
// histogram still holds GPU-written bytes not delivered, the copy returns the last histogram delivered from that
// address and a helper thread fetches the new one (it takes the fault and the wait); otherwise it is a plain copy.
// Exposure never sees missing data: the first copy from an address always waits.
namespace {
struct ExposureShadow {
    uintptr_t src = 0;
    u64 size = 0;
    bool valid = false;
    bool fetching = false;
    std::chrono::steady_clock::time_point delivered{}; ///< when `data` was read from the GPU or guest memory
    std::array<u8, 4096> data{};
};
// A shadow older than this is not served (the address may belong to another object by now: the real read runs).
constexpr auto kShadowMaxAge = std::chrono::milliseconds(250);
bool Fresh(const ExposureShadow& s) {
    return s.valid && std::chrono::steady_clock::now() - s.delivered < kShadowMaxAge;
}
std::mutex g_exposure_mutex;
std::condition_variable g_exposure_cv;
std::array<ExposureShadow, 16> g_exposure{};
std::deque<size_t> g_exposure_queue;
u64 g_exposure_stale = 0, g_exposure_plain = 0, g_exposure_fetched = 0;

// Under g_exposure_mutex. Null when the table is full (then the copy is plain).
ExposureShadow* ExposureSlot(uintptr_t src, u64 size) {
    for (auto& s : g_exposure) {
        if (s.src == src && s.size == size) {
            return &s;
        }
    }
    for (auto& s : g_exposure) {
        if (!s.src) {
            s.src = src;
            s.size = size;
            return &s;
        }
    }
    return nullptr;
}

void ExposureFetcher() {
    BbTrace::lazy_readback = true; // its downloads ride the next submission
    std::array<u8, 4096> buf;
    std::unique_lock lk{g_exposure_mutex};
    for (;;) {
        g_exposure_cv.wait(lk, [] { return !g_exposure_queue.empty(); });
        auto& slot = g_exposure[g_exposure_queue.front()];
        g_exposure_queue.pop_front();
        const uintptr_t src = slot.src;
        const u64 size = slot.size;
        lk.unlock();
        // A snapshot of the GPU's bytes, waited for here; guest memory when the GPU no longer holds the range. Not
        // a guest read: a read fault would deliver the window and, when the GPU keeps rewriting it, end in a
        // synchronous download on the GPU command thread (every ~8 frames with BB_CLOUD_STALE).
        auto* r = VideoCore::g_memops_rasterizer;
        bool got = r && r->SnapshotGpuRange(src, size, buf.data());
        // A failed snapshot also means its bounded GPU wait may have timed out. Backing bytes
        // are authoritative only after no undelivered GPU bytes remain. Otherwise keep the
        // previous delivery time: publishing stale backing as fresh would defeat the age limit.
        if (!got && (!r || !r->IsRangeGpuWritten(src, size))) {
            got = runtime_memory_read_backing(src, buf.data(), size) != 0;
        }
        lk.lock();
        if (got) {
            std::memcpy(slot.data.data(), buf.data(), size);
            slot.valid = true;
            slot.delivered = std::chrono::steady_clock::now();
        }
        slot.fetching = false;
        ++g_exposure_fetched;
    }
}

PS4_SYSV_ABI void* ExposureCopy(void* dst, const void* src, u64 size) {
    auto* r = VideoCore::g_memops_rasterizer;
    const auto at = reinterpret_cast<uintptr_t>(src);
    const bool tracked = size <= 4096;
    if (r && tracked && r->IsRangeGpuWritten(at, size)) {
        std::scoped_lock lk{g_exposure_mutex};
        auto* slot = ExposureSlot(at, size);
        if (slot && Fresh(*slot)) {
            if (!slot->fetching) {
                slot->fetching = true;
                g_exposure_queue.push_back(size_t(slot - g_exposure.data()));
                g_exposure_cv.notify_one();
            }
            std::memcpy(dst, slot->data.data(), size);
            if ((++g_exposure_stale & 4095) == 0) {
                std::printf("Exposure histograms: %llu served from the last delivered copy, %llu plain, %llu fetched\n",
                            (unsigned long long)g_exposure_stale, (unsigned long long)g_exposure_plain,
                            (unsigned long long)g_exposure_fetched);
            }
            return dst;
        }
    }
    std::memcpy(dst, src, size); // delivered, or the first copy from this address: the real data
    if (tracked) {
        std::scoped_lock lk{g_exposure_mutex};
        if (auto* slot = ExposureSlot(at, size); slot && !slot->fetching) {
            std::memcpy(slot->data.data(), dst, size);
            slot->valid = true;
            slot->delivered = std::chrono::steady_clock::now();
        }
        ++g_exposure_plain;
    }
    return dst;
}

// The exposure value (8 bytes, eboot 0x702b0d: the getter for obj+0x10, then vmovss from the pointer it returns at
// 0x702b29 / 0x702b2d): the same, by returning a pointer to the last delivered value. Main thread only.
using ReadbackGetter = PS4_SYSV_ABI bool (*)(void* obj, u32 frame, void** out, u32* count);
ReadbackGetter g_real_getter = nullptr;
alignas(16) thread_local u8 g_served_value[16]; // read by the caller after return: per thread

PS4_SYSV_ABI bool ExposureValueGet(void* obj, u32 frame, void** out, u32* count) {
    if (!g_real_getter(obj, frame, out, count)) {
        return false;
    }
    auto* r = VideoCore::g_memops_rasterizer;
    const auto at = reinterpret_cast<uintptr_t>(*out);
    constexpr u64 size = 8;
    if (!r || !at) {
        return true;
    }
    if (r->IsRangeGpuWritten(at, size)) {
        std::scoped_lock lk{g_exposure_mutex};
        auto* slot = ExposureSlot(at, size);
        if (!slot) {
            return true;
        }
        if (!slot->fetching) {
            slot->fetching = true;
            g_exposure_queue.push_back(size_t(slot - g_exposure.data()));
            g_exposure_cv.notify_one();
        }
        if (Fresh(*slot)) {
            std::memcpy(g_served_value, slot->data.data(), size);
            *out = g_served_value;
            ++g_exposure_stale;
        }
        return true; // not valid yet: the game reads the real value (and waits) this once
    }
    u8 value[size];
    std::memcpy(value, reinterpret_cast<const void*>(at), size);
    std::scoped_lock lk{g_exposure_mutex};
    if (auto* slot = ExposureSlot(at, size); slot && !slot->fetching) {
        std::memcpy(slot->data.data(), value, size);
        slot->valid = true;
        slot->delivered = std::chrono::steady_clock::now();
    }
    ++g_exposure_plain;
    return true;
}

std::once_flag g_fetcher_started;
void StartFetcher() {
    std::call_once(g_fetcher_started, [] { std::thread(ExposureFetcher).detach(); });
}

// Under g_exposure_mutex: queues a fetch of the slot unless one is queued or running.
void QueueFetch(ExposureShadow* slot) {
    if (!slot->fetching) {
        slot->fetching = true;
        g_exposure_queue.push_back(size_t(slot - g_exposure.data()));
        g_exposure_cv.notify_one();
    }
}

// BB_CLOUD_STALE=1 (opt-in, see bbgpu_cloud_hook; src/probe.c retargets eboot 0x60265e): trueSKY's cloud sample (FUN_00601df0) asks the
// getter for the previous frame's result (a 16-byte point and a GDS count) and keeps its old point when told "not
// ready" (the last completed copy is safe for it: it feeds the lightning state machine).
// The getter's count read waited for the GPU (~4 ms, the main thread's largest wait once exposure stopped waiting).
// The slot is picked exactly as the getter does (labels included); while its count or point holds GPU bytes not
// delivered, the last delivered pair is returned and the helper thread fetches the new one.
uintptr_t g_image_base = 0;
alignas(16) thread_local u8 g_served_cloud[16];
u64 g_cloud_stale = 0, g_cloud_plain = 0;

struct GetterPick {
    bool ok = false;
    uintptr_t data = 0, count = 0;
    u32 slot = 0, want = 0; ///< the slot and the generation the getter tests its label against
};
// Mirrors eboot FUN_00758980 without reading the count.
GetterPick PickGetterSlot(const u8* obj, u32 frame) {
    const u32 flags = *reinterpret_cast<const u32*>(obj + 0x10);
    u32 slot = 0, want = frame;
    if (flags & 0x10) {
        const u32 cur = *reinterpret_cast<const u32*>(obj + 0x80);
        if (frame != 0) {
            slot = cur;
            if (*reinterpret_cast<const u32*>(obj + 0x48 + slot * 0x30) != frame) {
                return {};
            }
        } else {
            const u32 owner = *reinterpret_cast<const u32*>(obj + 0x84);
            const u32 gen = *reinterpret_cast<const u32*>(obj + 0x88);
            const bool mine = owner == *reinterpret_cast<const u32*>(g_image_base + 0x2749f58);
            const u32 lag = mine ? 3 : 2;
            if (gen <= lag) {
                return {};
            }
            slot = mine ? 1 - cur : cur;
            want = gen - lag;
        }
    }
    // obj+0x40 holds a pointer to the label's pointer (758a11-758a19: two loads, then the compare).
    const u32* label = **reinterpret_cast<u32** const*>(obj + 0x40 + slot * 0x30);
    if (*label != want) {
        return {};
    }
    return {true, *reinterpret_cast<const uintptr_t*>(obj + 0x20 + slot * 0x30),
            (flags & 0x20) ? *reinterpret_cast<const uintptr_t*>(obj + 0x28 + slot * 0x30) : 0, slot, want};
}

PS4_SYSV_ABI bool CloudGet(void* obj, u32 frame, void** out, u32* count) {
    auto* r = VideoCore::g_memops_rasterizer;
    const GetterPick pick = r ? PickGetterSlot(static_cast<const u8*>(obj), frame) : GetterPick{};
    if (!pick.ok || !pick.data || !pick.count) {
        return g_real_getter(obj, frame, out, count); // not ready (or not a counted buffer): as before
    }
    constexpr u64 size = 16;
    if (r->IsRangeGpuWritten(pick.count, 4) || r->IsRangeGpuWritten(pick.data, size)) {
        std::scoped_lock lk{g_exposure_mutex};
        auto* c = ExposureSlot(pick.count, 4);
        auto* d = ExposureSlot(pick.data, size);
        if (c && d) {
            QueueFetch(c);
            QueueFetch(d);
            if (Fresh(*c) && Fresh(*d)) {
                std::memcpy(g_served_cloud, d->data.data(), size);
                u32 n;
                std::memcpy(&n, c->data.data(), 4);
                const u32 most = *reinterpret_cast<const u32*>(static_cast<const u8*>(obj) + 0xc);
                *out = g_served_cloud;
                *count = n > most ? most : n;
                ++g_cloud_stale;
                return true;
            }
        }
        // Nothing delivered yet: the real getter (it waits) this once.
    }
    const bool ok = g_real_getter(obj, frame, out, count);
    u8 point[size];
    u32 n;
    std::memcpy(point, reinterpret_cast<const void*>(pick.data), size);
    std::memcpy(&n, reinterpret_cast<const void*>(pick.count), 4);
    std::scoped_lock lk{g_exposure_mutex};
    auto* c = ExposureSlot(pick.count, 4);
    auto* d = ExposureSlot(pick.data, size);
    if (c && d && !c->fetching && !d->fetching) {
        std::memcpy(c->data.data(), &n, 4);
        std::memcpy(d->data.data(), point, size);
        c->valid = d->valid = true;
        c->delivered = d->delivered = std::chrono::steady_clock::now();
    }
    if ((++g_cloud_plain & 1023) == 0) {
        std::printf("Cloud sample: %llu served from the last delivered pair, %llu plain\n",
                    (unsigned long long)g_cloud_stale, (unsigned long long)g_cloud_plain);
    }
    return ok;
}
// bbport: BB_EXPOSURE_EXACT / BB_CLOUD_EXACT (default on, =0 off; replace BB_EXPOSURE_STALE / BB_CLOUD_STALE): the
// exposure's three getter calls and the cloud sample's (src/probe.c) get exactly the generation
// the game asked for without waiting when the GPU has done it. The emulator writes a readback label when the command
// is recorded, not when the GPU has run the work before it (liverpool.cpp RunEventWriteEop), so the getter passes and
// the payload (or count) read then waits for the GPU (a fault download recorded into the current submission: it
// waits for work after the producer too). Here every slot's label address is registered with its payload (and GDS
// count); at that label's EOP (in order with the draws) copies of them are recorded and tagged with the value the
// label gets (the generation) and the registration's lifetime id. The slot and the generation the getter tests are
// taken from its inputs before it runs (PickGetterSlot mirrors eboot 0x758980, both latency branches), and the label
// must still hold that generation afterwards (a label only grows). Snapshots of that generation done on the GPU are
// served (host copies, no wait). Not recorded, not done yet, or anything changed while the getter ran: the game's
// own read (it waits if the GPU holds the bytes), as before. Nothing else is ever served: no other generation, no
// "not ready" the getter would not have returned (the real getter decides that).
// Lifetime: a label belongs to one slot of one object; registering it for another object or payload drops the old
// registration, and a registration whose object's issue counter (+0x88) goes back, or whose label gets a lower
// generation at an EOP, gets a new id: snapshots of the old lifetime never match again.
// BB_EXPOSURE_EXACT_VERIFY=1: after each serve the real bytes are read too (they wait). Equal: matched. Different:
// kept until a later snapshot of the same range is done; equal to one with a higher generation, the real read saw a
// newer producer (the exposure's next dispatch is recorded before its getters, eboot 0x702a59): newer. Otherwise
// unexplained (printed). Counted per call site (0, 1 histograms, 2 exposure value, 3 cloud).
struct ExactLabel {
    const void* obj = nullptr;
    VAddr label = 0;
    VAddr payload = 0;
    u32 size = 0;
    VAddr count = 0;  ///< the slot's GDS count (4 bytes), 0 when none
    u64 id = 0;       ///< lifetime identity carried by its snapshots
    u32 last_gen = 0; ///< the object's issue counter (+0x88) at the last getter call
    u32 last_tag = 0; ///< the last generation an EOP wrote to the label
    bool tagged = false;
    bool flush = false; ///< end the submission after its snapshot (ExactEopFlush)
};
std::mutex g_exact_mutex;
std::vector<ExactLabel> g_exact_labels; // under g_exact_mutex
u64 g_exact_ids = 0;                    // under g_exact_mutex
std::atomic<u32> g_exact_count{0};
std::atomic<u64> g_exact_recorded{0}, g_exact_epochs{0};
// The exposure (FUN_00702750) and the cloud sample (FUN_00601df0) may run on different guest threads: the stats and
// open mismatches are under g_exact_stats_mutex.
struct ExactStats {
    u64 served = 0, pending = 0, missing = 0, plain = 0, raced = 0, verified = 0, matched = 0, newer = 0,
        unexplained = 0, waited = 0, wait_ns = 0, rewritten = 0, nonempty = 0, deferred = 0, sorting = 0;
};
std::array<ExactStats, 6> g_exact_stats{};
u64 g_exact_other = 0;
struct ExactMismatch {
    u32 site = 0;
    VAddr addr = 0;
    u32 generation = 0;
    u64 id = 0;
    u32 calls = 0;
    std::vector<u8> real;
};
std::vector<ExactMismatch> g_exact_mismatches;
std::mutex g_exact_stats_mutex; // the stats and mismatches above

bool EnvOne(const char* name) {
    const char* env = std::getenv(name);
    return env && env[0] == '1';
}
// BB_EXPOSURE_EXACT=2: the hooks run and log (BB_EXPOSURE_LOG) but never serve: the game's ordinary reads, as the
// reference for temporal comparisons.
// Default on: =0 restores the game's own reads (or BB_EXPOSURE_STALE's last-delivered copies when that is set).
u32 ExposureExactMode() {
    static const u32 mode = [] {
        const char* env = std::getenv("BB_EXPOSURE_EXACT");
        return !env || !env[0] ? 1u : env[0] == '1' || env[0] == '2' ? u32(env[0] - '0') : 0u;
    }();
    return mode;
}
bool ExposureExact() {
    return ExposureExactMode() != 0;
}
bool CloudExact() { // default on, =0 off (then BB_CLOUD_STALE=1 or the game's own wait)
    static const bool on = [] {
        const char* env = std::getenv("BB_CLOUD_EXACT");
        return !(env && env[0] == '0');
    }();
    return on;
}
bool ExactVerify() {
    static const bool on = EnvOne("BB_EXPOSURE_EXACT_VERIFY");
    return on;
}
// BB_EXACT_WAIT=0 off: a snapshot the GPU has not done yet but whose submission has gone is waited for (up to 100 ms)
// instead of the guest's read fault, which copies again behind all current work and waits for that.
u64 ExactWaitNs() {
    static const u64 ns = [] {
        const char* env = std::getenv("BB_EXACT_WAIT");
        return env && env[0] == '0' ? 0ull : 100'000'000ull;
    }();
    return ns;
}

// BB_EXACT_DEFER (default on, =0 off; in-race timing unchanged at a 60 FPS cap, no visible change around late answers
// uncapped): the query results (site 4) and the crowd heights (site 5) are polled every frame, and "not ready" leaves
// the request standing (eboot 0x1051f43 / 0x574d80).
// While the requested generation's snapshot is recorded but the GPU has not done it, they are told "not ready" instead
// of reading the data, which waits for the GPU (the label is written when the command processor reaches the EOP; on a
// PS4 it is written when the GPU gets there). After 16 such answers to one request the game reads and waits as before.
bool ExactDefer() {
    static const bool on = [] {
        const char* env = std::getenv("BB_EXACT_DEFER");
        return !(env && env[0] == '0');
    }();
    return on;
}
// BB_EXACT_EOP_FLUSH (on with BB_EXACT_DEFER, =0 off): the submission ends right after the query's and the crowd's
// snapshot copies, so a snapshot is "done" when the GPU has run the work up to their EOP, not the rest of the batch
// after it. "Not ready" then means what it means on a PS4: the GPU has not reached the EOP.
bool ExactEopFlush() {
    static const bool on = [] {
        const char* env = std::getenv("BB_EXACT_EOP_FLUSH");
        return ExactDefer() && !(env && env[0] == '0');
    }();
    return on;
}
struct ExactDeferral {
    const void* obj = nullptr;
    u32 want = 0, times = 0;
    std::chrono::steady_clock::time_point first{}; ///< the first "not ready"
};
// Per site, one entry per outstanding request (a site can serve several objects in turn: two query objects in batch
// 44): freed when the request is answered; with all four in use the game reads and waits.
std::array<std::array<ExactDeferral, 4>, 6> g_exact_deferrals{};
// Under g_exact_stats_mutex: whether this request may be told "not ready" once more.
bool DeferExact(u32 site, const void* obj, u32 want) {
    auto& list = g_exact_deferrals[site];
    auto it = std::ranges::find_if(list, [&](const ExactDeferral& d) { return d.obj == obj; });
    if (it == list.end()) {
        it = std::ranges::find_if(list, [](const ExactDeferral& d) { return d.obj == nullptr; });
        if (it == list.end()) {
            return false;
        }
        *it = {obj, want, 0, std::chrono::steady_clock::now()};
    } else if (it->want != want) {
        *it = {obj, want, 0, std::chrono::steady_clock::now()};
    }
    if (it->times == 16) {
        return false; // 16 answers given: the game reads (counted as 16)
    }
    ++it->times;
    return true;
}
// Per site: requests answered after 1, 2, 3, 4, 5-15 and 16 "not ready" answers (the game polls once a frame), and
// the time from the first "not ready" to the answer.
std::array<std::array<u64, 6>, 6> g_exact_defer_frames{};
std::array<u64, 6> g_exact_defer_ns{};
// Under g_exact_stats_mutex: the object's request `want` was answered (served, or read by the game): its entry is
// freed, and how long it was put off is counted (the first 24 logged). An entry for an older
// request (a caller that moves on to a new one instead of asking again) is freed without being counted.
void EndDeferral(u32 site, const void* obj, u32 want, char path) {
    for (auto& d : g_exact_deferrals[site]) {
        if (d.obj != obj) {
            continue;
        }
        if (d.times && d.want == want) {
            const u32 n = d.times;
            ++g_exact_defer_frames[site][n <= 4 ? n - 1 : n < 16 ? 4 : 5];
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                                 d.first)
                                .count();
            g_exact_defer_ns[site] += u64(ns);
            static u32 logged = 0;
            if (++logged <= 24) {
                std::printf("Exposure exact site %u: request %u of %p answered (%c) after %u \"not ready\", %.1f ms\n",
                            site, d.want, obj, path, n, ns / 1e6);
            }
        }
        d = {};
    }
}

struct ExactSlots {
    VAddr labels[2]{}, payloads[2]{}, counts[2]{};
};
// The object's two slots (flags 0x10: payload pointer at +0x20 + i * 0x30, count pointer at +0x28, pointer to the
// label pointer at +0x40). Without flag 0x10 the getter uses slot 0 only
// (eboot 0x75898b) and slot 1's fields are not the object's: they are not read.
ExactSlots ReadExactSlots(const u8* o, bool counted) {
    ExactSlots s;
    const u32 slots = (*reinterpret_cast<const u32*>(o + 0x10) & 0x10) ? 2 : 1;
    for (u32 i = 0; i < slots; ++i) {
        s.payloads[i] = *reinterpret_cast<const u64*>(o + 0x20 + i * 0x30);
        s.counts[i] = counted ? *reinterpret_cast<const u64*>(o + 0x28 + i * 0x30) : 0;
        const auto* label_ptr = *reinterpret_cast<const u64* const*>(o + 0x40 + i * 0x30);
        s.labels[i] = label_ptr ? *label_ptr : 0;
    }
    return s;
}

// Under g_exact_mutex: registers both slots (a label now another slot's drops its old registration; a new
// incarnation at the same addresses gets new ids). Returns the id of the selected slot's registration.
u64 RegisterExactSlots(const u8* o, u32 size, const ExactSlots& s, u32 slot, bool flush = false) {
    const u32 gen = *reinterpret_cast<const u32*>(o + 0x88);
    std::erase_if(g_exact_labels, [&](const ExactLabel& e) {
        bool listed = false, label_used = false;
        for (u32 i = 0; i < 2; ++i) {
            label_used |= e.label == s.labels[i];
            listed |= e.label == s.labels[i] && e.payload == s.payloads[i] && e.count == s.counts[i];
        }
        return (label_used || e.obj == o) && !(e.obj == o && listed);
    });
    u64 id = 0;
    for (u32 i = 0; i < 2; ++i) {
        if (!s.labels[i] || !s.payloads[i]) {
            continue;
        }
        auto it = std::ranges::find_if(g_exact_labels, [&](const ExactLabel& e) {
            return e.obj == o && e.label == s.labels[i] && e.payload == s.payloads[i] && e.count == s.counts[i];
        });
        if (it == g_exact_labels.end()) {
            g_exact_labels.push_back({o, s.labels[i], s.payloads[i], size, s.counts[i], ++g_exact_ids, gen, 0, false,
                                      flush});
            it = g_exact_labels.end() - 1;
            std::printf("Exposure exact: slot %u of %p: payload %llx (%u bytes), count %llx, label %llx, id %llu\n",
                        i, (const void*)o, (unsigned long long)s.payloads[i], size, (unsigned long long)s.counts[i],
                        (unsigned long long)s.labels[i], (unsigned long long)it->id);
        } else if (gen < it->last_gen) { // the object was made again at the same addresses
            it->id = ++g_exact_ids;
            it->tagged = false;
            g_exact_epochs.fetch_add(1, std::memory_order_relaxed);
        }
        it->last_gen = gen;
        it->size = size;
        it->flush = flush;
        if (i == slot) {
            id = it->id;
        }
    }
    if (g_exact_labels.size() > 16) {
        g_exact_labels.erase(g_exact_labels.begin(), g_exact_labels.end() - 16);
    }
    g_exact_count.store(u32(g_exact_labels.size()), std::memory_order_release);
    return id;
}

// BB_EXPOSURE_LOG=<file> (diagnostic): one line per exact readback answer (time, object, slot, generation,
// path, snapshot ticks, value). Off unless the variable is set.
FILE* ExposureLog() {
    static FILE* file = [] {
        const char* path = std::getenv("BB_EXPOSURE_LOG");
        FILE* f = path && path[0] ? std::fopen(path, "w") : nullptr;
        if (f) {
            std::fprintf(f, "# E t_ms site obj slot frame gen id path tick0 tick1 value\n");
        }
        return f;
    }();
    return file;
}

// value: the exposure value the game gets (site 2), else 0.
void LogExact(u32 site, const void* obj, u32 slot, u32 generation, u64 id, char path, const u64* ticks,
              u64 value = 0) {
    FILE* f = ExposureLog();
    if (!f) {
        return;
    }
    const auto* o = static_cast<const u8*>(obj);
    static std::mutex m;
    std::scoped_lock lk{m};
    std::fprintf(f, "E %.3f %u %p %u %u %u %llu %c %llu %llu %016llx\n",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
                     .count(),
                 site, obj, slot, *reinterpret_cast<const u32*>(o + 0x88), generation, (unsigned long long)id, path,
                 (unsigned long long)ticks[0], (unsigned long long)ticks[1], (unsigned long long)value);
    static u32 lines = 0;
    if ((++lines & 255) == 0) {
        std::fflush(f);
    }
}

// Under g_exact_stats_mutex: compares a served copy with the real bytes (read here: it waits).
void VerifyExact(u32 site, VAddr addr, const u8* served, u32 size, u32 generation, u64 id) {
    auto& st = g_exact_stats[site];
    ++st.verified;
    ExactMismatch m{site, addr, generation, id, 0, std::vector<u8>(size)};
    std::memcpy(m.real.data(), reinterpret_cast<const void*>(addr), size);
    if (std::memcmp(served, m.real.data(), size) == 0) {
        ++st.matched;
    } else {
        // Acceptance counts raw matches only: every mismatch is printed when it happens; the
        // resolver below only says whether a later snapshot holds the same bytes.
        static u32 printed = 0;
        if (++printed <= 32) {
            std::printf("Exposure exact: MISMATCH site %u addr %llx generation %u id %llu\n", site,
                        (unsigned long long)addr, generation, (unsigned long long)id);
        }
        g_exact_mismatches.push_back(std::move(m));
    }
}

// Under g_exact_stats_mutex: mismatches waiting for a later snapshot of the same range.
void ResolveExactMismatches(Vulkan::Rasterizer& r) {
    static std::vector<u8> later; // as large as the mismatch (the crowd's 8 KB)
    std::erase_if(g_exact_mismatches, [&](ExactMismatch& m) {
        later.resize(std::max(later.size(), m.real.size()));
        for (u32 k = 1; k <= 4; ++k) {
            const u32 next = m.generation + k;
            bool pending = false;
            if (r.ReadTaggedSnapshot(m.addr, u32(m.real.size()), next, m.id, later.data(), &pending, nullptr) &&
                std::memcmp(later.data(), m.real.data(), m.real.size()) == 0) {
                ++g_exact_stats[m.site].newer;
                return true;
            }
        }
        if (++m.calls < 96) {
            return false;
        }
        if (++g_exact_stats[m.site].unexplained <= 16) {
            std::printf("Exposure exact: UNEXPLAINED mismatch site %u addr %llx generation %u id %llu\n", m.site,
                        (unsigned long long)m.addr, m.generation, (unsigned long long)m.id);
        }
        return true;
    });
}

// Under g_exact_stats_mutex.
void PrintExactStats() {
    static u64 calls = 0;
    if ((++calls & 511) != 1) {
        return;
    }
    for (u32 i = 0; i < g_exact_stats.size(); ++i) {
        const auto& t = g_exact_stats[i];
        std::printf("Exposure exact site %u: %llu served (%llu with data; %llu after a %.1f ms average wait for the "
                    "snapshot), "
                    "%llu read by the game (pending %llu, missing %llu), %llu plain, "
                    "%llu raced, %llu deferred, %llu sorting; verified %llu: %llu matched, %llu newer, %llu "
                    "unexplained; %llu copies changed by the caller\n",
                    i, (unsigned long long)t.served, (unsigned long long)t.nonempty, (unsigned long long)t.waited,
                    t.waited ? t.wait_ns / 1e6 / double(t.waited) : 0.0, (unsigned long long)(t.pending + t.missing),
                    (unsigned long long)t.pending, (unsigned long long)t.missing, (unsigned long long)t.plain,
                    (unsigned long long)t.raced, (unsigned long long)t.deferred, (unsigned long long)t.sorting,
                    (unsigned long long)t.verified,
                    (unsigned long long)t.matched,
                    (unsigned long long)t.newer, (unsigned long long)t.unexplained, (unsigned long long)t.rewritten);
    }
    for (u32 i = 3; i <= 5; ++i) {
        const auto& h = g_exact_defer_frames[i];
        if (h[0] + h[1] + h[2] + h[3] + h[4] + h[5]) {
            const u64 n = h[0] + h[1] + h[2] + h[3] + h[4] + h[5];
            std::printf("Exposure exact site %u: deferred requests answered after 1 / 2 / 3 / 4 / 5-15 / 16 \"not "
                        "ready\": %llu / %llu / %llu / %llu / %llu / %llu, %.1f ms on average\n",
                        i, (unsigned long long)h[0], (unsigned long long)h[1], (unsigned long long)h[2],
                        (unsigned long long)h[3], (unsigned long long)h[4], (unsigned long long)h[5],
                        g_exact_defer_ns[i] / 1e6 / double(n));
        }
    }
    std::printf("Exposure exact: %llu snapshots recorded, %llu other reads, %llu lifetimes renewed, %zu mismatches "
                "open\n",
                (unsigned long long)g_exact_recorded.load(std::memory_order_relaxed),
                (unsigned long long)g_exact_other, (unsigned long long)g_exact_epochs.load(std::memory_order_relaxed),
                g_exact_mismatches.size());
}

// The exposure (flags 0x16, latency mode): sites 0, 1 (histograms, 1 KB) and 2 (value, 8 bytes). Site 5
// (BB_CROWD_EXACT, default on, =0 off): the crowd ground heights of eboot FUN_00574d30 (getter call 0x574d79; flags
// 0x10f, one slot, explicit frame, polled every frame until ready), 512 x 16 bytes, read by its loop at 0x574df0 (the
// GPU writes them in FUN_00575f30; the loop's reads waited 4-35 ms, ~40 per race).
template <u32 Site>
PS4_SYSV_ABI bool ExposureExactGet(void* obj, u32 frame, void** out, u32* count) {
    constexpr u32 Size = Site == 2 ? 8 : Site == 5 ? 0x2000 : 0x400;
    constexpr u32 Flags = Site == 5 ? 0x10f : 0x16;
    const auto* o = static_cast<const u8*>(obj);
    auto* r = VideoCore::g_memops_rasterizer;
    const u32 flags = *reinterpret_cast<const u32*>(o + 0x10);
    const int mode = Site == 5 ? 1 : ExposureExactMode();
    if constexpr (Site == 5) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::printf("Exposure exact site 5: object %p flags %#x +0xc %u, frame %u%s\n", obj, flags,
                        *reinterpret_cast<const u32*>(o + 0xc), frame, flags == Flags ? "" : ": not served");
        }
    }
    const bool exposure_obj = r && flags == Flags;
    const GetterPick pick = exposure_obj ? PickGetterSlot(o, frame) : GetterPick{};
    const ExactSlots slots = exposure_obj ? ReadExactSlots(o, false) : ExactSlots{};
    if (!g_real_getter(obj, frame, out, count)) {
        return false;
    }
    const auto at = reinterpret_cast<uintptr_t>(*out);
    if (!pick.ok || pick.data != at || slots.payloads[pick.slot] != at || !slots.labels[pick.slot]) {
        std::scoped_lock lk{g_exact_stats_mutex};
        ++g_exact_other;
        if (Site == 5 && ExactDefer()) {
            EndDeferral(Site, obj, frame, 'O');
        }
        return true;
    }
    const u32 generation = pick.want;
    const bool raced = *reinterpret_cast<const u32*>(slots.labels[pick.slot]) != generation;
    u64 id = 0;
    {
        std::scoped_lock lk{g_exact_mutex};
        id = RegisterExactSlots(o, Size, slots, pick.slot, Site == 5 && ExactEopFlush());
    }
    // Read by the caller right after the return (memcpy at 0x702ad2 / 0x702afc, vmovss at 0x702b29; the crowd's loop
    // at 0x574df0).
    alignas(16) thread_local u8 buf[Size < 16 ? 16 : Size];
    u64 ticks[3]{};
    bool pending = false;
    char path;
    const bool defer = Site == 5 && ExactDefer();
    const bool done = !raced && id &&
                      r->ReadTaggedSnapshot(at, Size, generation, id, buf, &pending, ticks,
                                            mode == 1 && !defer ? ExactWaitNs() : 0);
    std::scoped_lock lk{g_exact_stats_mutex};
    auto& st = g_exact_stats[Site];
    if (ticks[2]) {
        ++st.waited;
        st.wait_ns += ticks[2];
    }
    if (raced) {
        path = 'R';
        ++st.raced;
    } else if (done && mode == 1) {
        path = 'X';
        *out = buf;
        ++st.served;
        ++st.nonempty;
        if (ExactVerify()) {
            VerifyExact(Site, at, buf, Size, generation, id);
        }
    } else if (done) {
        path = 'S'; // BB_EXPOSURE_EXACT=2: would have been served
        ++st.served;
    } else if (defer && pending && DeferExact(Site, obj, generation)) {
        path = 'D'; // "not ready": asked again next frame
        ++st.deferred;
    } else if (r->IsRangeGpuWritten(at, Size)) {
        path = pending ? 'Y' : 'Z'; // the game's read waits, as without the hook
        ++(pending ? st.pending : st.missing);
    } else {
        path = 'P'; // delivered: the game's read does not wait
        ++st.plain;
    }
    if (defer && path != 'D') {
        EndDeferral(Site, obj, generation, path);
    }
    if (!g_exact_mismatches.empty()) {
        ResolveExactMismatches(*r);
    }
    if (ExposureLog()) {
        u64 value = 0;
        if constexpr (Site == 2) {
            std::memcpy(&value, *out, 8); // the game's own read follows at once: it waits the same
        }
        LogExact(Site, obj, pick.slot, generation, id, path, ticks, value);
    }
    PrintExactStats();
    return path != 'D';
}

// The getter's count clamp (eboot 0x758a34: cmp/cmova with the maximum at +0xc, unsigned).
u32 ClampCount(u32 n, u32 most) {
    return n > most ? most : n;
}
enum class CloudServe { RealGetter, NoPoint, Point };
// What the cloud hook may serve: the requested generation's count snapshot, and its point snapshot unless the
// clamped count is 0 (the caller then does not read the point, eboot FUN_00601df0). Anything else: the real getter.
CloudServe DecideCloudServe(bool have_count, u32 clamped, bool have_point) {
    if (!have_count) {
        return CloudServe::RealGetter;
    }
    if (clamped == 0) {
        return CloudServe::NoPoint;
    }
    return have_point ? CloudServe::Point : CloudServe::RealGetter;
}

// The cloud sample (flags 0x36: two slots, GDS count; explicit request), site 3: the getter's count read (eboot
// 0x758a32) is what waits, so the getter itself is mirrored when the count and point snapshots of the requested
// generation are done; otherwise (and for "not ready", decided before any count read) the real getter runs.
// Site 3: the cloud sample (flags 0x36), the 16-byte point the caller copies.
// Site 4 (BB_QUERY_EXACT, default on, =0 off): the query results of eboot FUN_01051f00 (getter call 0x1051f3c), which
// may sort the entries in place (0x1051f5b; such requests keep the real path) and copies count x 32 bytes (memcpy
// 0x1051f6f): the slot's whole payload, the maximum count (+0xc) x 32 bytes, for objects with a count (flags 0x20; one
// slot without 0x10). Its count and payload reads were the main thread's largest GPU waits at 1440p (327 + 279 per
// race, ~5 ms each).
constexpr u32 QueryMaxBytes = 256 * 1024;
// The real getter's answer for a request the hook does not serve; a deferred request's entry is freed when it is
// answered.
template <u32 Site>
bool RealGetterAnswer(void* obj, u32 frame, void** out, u32* count, char path) {
    const bool ready = g_real_getter(obj, frame, out, count);
    if (ready && Site == 4 && ExactDefer()) {
        std::scoped_lock lk{g_exact_stats_mutex};
        EndDeferral(Site, obj, frame, path); // explicit requests: the generation asked for is the frame
    }
    return ready;
}
// sort: site 4 only, FUN_01051f00's argument that makes it sort the results in place (r14b, passed by the thunk in r8):
// such requests keep the real path, so the game sorts its own payload. Site 3 ignores it.
template <u32 Site>
PS4_SYSV_ABI bool CountedExactGet(void* obj, u32 frame, void** out, u32* count, u32 sort) {
    const auto* o = static_cast<const u8*>(obj);
    auto* r = VideoCore::g_memops_rasterizer;
    const u32 flags = *reinterpret_cast<const u32*>(o + 0x10);
    const u32 maximum = *reinterpret_cast<const u32*>(o + 0xc);
    // The maximum is checked before it is multiplied (no wrap to a small size).
    const bool eligible =
        Site == 3 ? flags == 0x36 : (flags & 0x20) && maximum != 0 && maximum <= QueryMaxBytes / 32u;
    const u32 Size = Site == 3 ? 16u : eligible ? maximum * 32u : 0u;
    if constexpr (Site == 4) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::printf("Exposure exact site 4: object %p flags %#x maximum %u entries (%u bytes), frame %u%s\n", obj,
                        flags, maximum, Size, frame, eligible ? "" : ": not served");
        }
    }
    const bool sorting = Site == 4 && (sort & 0xff) != 0;
    if (!r || frame == 0 || !eligible || sorting) {
        {
            std::scoped_lock lk{g_exact_stats_mutex};
            ++(sorting ? g_exact_stats[Site].sorting : g_exact_other);
        }
        return RealGetterAnswer<Site>(obj, frame, out, count, 'O');
    }
    const GetterPick pick = PickGetterSlot(o, frame);
    if (!pick.ok || !pick.data || !pick.count) {
        // "not ready": the real getter returns it without a count read
        return RealGetterAnswer<Site>(obj, frame, out, count, 'O');
    }
    const ExactSlots slots = ReadExactSlots(o, true);
    u64 id = 0;
    {
        std::scoped_lock lk{g_exact_mutex};
        id = RegisterExactSlots(o, Size, slots, pick.slot, Site == 4 && ExactEopFlush());
    }
    thread_local std::vector<u8> buffer; // operator new: 16-byte aligned, as the point's copy needs
    if (buffer.size() < Size) {
        buffer.resize(Size);
    }
    u8* const point = buffer.data();
    // Verify mode: whether the caller changed the copy served last time (FUN_01051f00 sorts it in place when asked),
    // checked before this call overwrites it.
    thread_local std::vector<u8> last_served;
    const bool rewritten = ExactVerify() && !last_served.empty() &&
                           std::memcmp(point, last_served.data(), last_served.size()) != 0;
    last_served.clear();
    u32 n = 0;
    u64 ticks[3]{};
    bool count_pending = false, point_pending = false;
    const bool defer = Site == 4 && ExactDefer();
    const u64 wait_ns = defer ? 0 : ExactWaitNs();
    const bool have_count =
        id && slots.payloads[pick.slot] == pick.data && slots.counts[pick.slot] == pick.count &&
        r->ReadTaggedSnapshot(pick.count, 4, pick.want, id, &n, &count_pending, ticks, wait_ns);
    const u32 clamped = have_count ? ClampCount(n, *reinterpret_cast<const u32*>(o + 0xc)) : 0;
    // The point of the same generation and lifetime only; none needed when the count is 0.
    const bool have_point = have_count && clamped != 0 &&
                            r->ReadTaggedSnapshot(pick.data, Size, pick.want, id, point, &point_pending, nullptr,
                                                  wait_ns);
    const CloudServe how = DecideCloudServe(have_count, clamped, have_point);
    const bool raced = slots.labels[pick.slot] && *reinterpret_cast<const u32*>(slots.labels[pick.slot]) != pick.want;
    char path;
    std::unique_lock lk{g_exact_stats_mutex};
    auto& st = g_exact_stats[Site];
    st.rewritten += rewritten;
    if (ticks[2]) {
        ++st.waited;
        st.wait_ns += ticks[2];
    }
    if (how != CloudServe::RealGetter && !raced) {
        // As the getter: *out is the slot's payload pointer (here the point's copy), *count the clamped count.
        *out = how == CloudServe::Point ? static_cast<void*>(point) : reinterpret_cast<void*>(pick.data);
        *count = clamped;
        path = 'X';
        ++st.served;
        st.nonempty += how == CloudServe::Point;
        if (ExactVerify()) {
            // One request: the raw count and, when served, the point (the real reads wait).
            ++st.verified;
            u32 real_n;
            std::memcpy(&real_n, reinterpret_cast<const void*>(pick.count), 4);
            // The bytes the caller reads: the point (site 3), count x 32 (site 4).
            const u32 compared = Site == 3 ? Size : clamped * 32u;
            const bool same = real_n == n && (how != CloudServe::Point ||
                                              std::memcmp(point, reinterpret_cast<const void*>(pick.data), compared) == 0);
            if (how == CloudServe::Point) {
                last_served.assign(point, point + compared);
            }
            if (same) {
                ++st.matched;
            } else if (++st.unexplained <= 32) {
                std::printf("Exposure exact: MISMATCH site %u count %llx point %llx generation %u id %llu: count %u "
                            "vs real %u\n",
                            Site, (unsigned long long)pick.count, (unsigned long long)pick.data, pick.want,
                            (unsigned long long)id, n, real_n);
            }
        }
    } else if (defer && !raced && (count_pending || point_pending) && DeferExact(Site, obj, pick.want)) {
        path = 'D'; // "not ready": asked again next frame
        ++st.deferred;
    } else {
        const bool waits = r->IsRangeGpuWritten(pick.count, 4) || r->IsRangeGpuWritten(pick.data, Size);
        path = raced ? 'R' : !waits ? 'P' : (count_pending || point_pending) ? 'Y' : 'Z';
        ++(raced ? st.raced : !waits ? st.plain : (count_pending || point_pending) ? st.pending : st.missing);
    }
    if (defer && path == 'X') {
        EndDeferral(Site, obj, pick.want, path);
    }
    if (!g_exact_mismatches.empty()) {
        ResolveExactMismatches(*r);
    }
    LogExact(Site, obj, pick.slot, pick.want, id, path, ticks);
    PrintExactStats();
    lk.unlock();
    if (path == 'X' || path == 'D') {
        return path == 'X';
    }
    return RealGetterAnswer<Site>(obj, frame, out, count, path);
}

// At an EOP: record(payload or count, size, tag, id) for every registration of the label (renewing its id first
// when the generation went back). Separate from the rasterizer for the self-test. Returns whether a registration that
// wants the submission ended there recorded a snapshot.
template <typename Record>
bool ExactEop(VAddr label, u32 tag, bool immediate, Record&& record) {
    if (g_exact_count.load(std::memory_order_acquire) == 0 || !immediate) {
        return false;
    }
    std::array<ExactLabel, 16> hits{};
    u32 n = 0;
    {
        std::scoped_lock lk{g_exact_mutex};
        for (auto& e : g_exact_labels) {
            if (e.label != label || n == hits.size()) {
                continue;
            }
            if (e.tagged && tag < e.last_tag) { // a generation lower than the last: a new lifetime
                e.id = ++g_exact_ids;
                g_exact_epochs.fetch_add(1, std::memory_order_relaxed);
            }
            e.tagged = true;
            e.last_tag = tag;
            hits[n++] = e;
        }
    }
    bool flush = false;
    for (u32 i = 0; i < n; ++i) {
        if (record(hits[i].payload, hits[i].size, tag, hits[i].id)) {
            g_exact_recorded.fetch_add(1, std::memory_order_relaxed);
            flush |= hits[i].flush;
        }
        if (hits[i].count && record(hits[i].count, 4u, tag, hits[i].id)) {
            g_exact_recorded.fetch_add(1, std::memory_order_relaxed);
            flush |= hits[i].flush;
        }
    }
    return flush;
}

// BB_EXPOSURE_EXACT_SELFTEST=1: the slot/generation choice and the lifetime rules on fake
// objects in host memory, before the game starts (no GPU): lag 2 and lag 3, a label not yet written (not ready),
// a lower generation at an EOP and a lower issue counter (new lifetime ids), a label taken over by another object,
// non-immediate EOP data. Prints one line per failed check and a summary; leaves the registry empty.
void ExposureExactSelfTest() {
    constexpr uintptr_t kOwnerGlobal = 0x2749f58;
    std::vector<u8> image(kOwnerGlobal + 8);
    const uintptr_t saved_base = g_image_base;
    g_image_base = reinterpret_cast<uintptr_t>(image.data());
    struct Fake {
        alignas(16) u8 obj[0x100]{};
        alignas(16) u8 payload[2][0x400]{};
        u32 count[2]{};
        alignas(8) u32 label[2]{};
        u64 label_ptr[2]{}; // the slot's +0x40 points here; it holds the label address
        void Init(u32 flags, u32 gen, u32 cur, u32 owner) {
            *reinterpret_cast<u32*>(obj + 0x10) = flags;
            for (u32 i = 0; i < 2; ++i) {
                label_ptr[i] = reinterpret_cast<u64>(&label[i]);
                *reinterpret_cast<u64*>(obj + 0x20 + i * 0x30) = reinterpret_cast<u64>(payload[i]);
                *reinterpret_cast<u64*>(obj + 0x28 + i * 0x30) = reinterpret_cast<u64>(&count[i]);
                *reinterpret_cast<u64*>(obj + 0x40 + i * 0x30) = reinterpret_cast<u64>(&label_ptr[i]);
            }
            *reinterpret_cast<u32*>(obj + 0x80) = cur;
            *reinterpret_cast<u32*>(obj + 0x84) = owner;
            *reinterpret_cast<u32*>(obj + 0x88) = gen;
        }
    };
    auto a = std::make_unique<Fake>(), b = std::make_unique<Fake>();
    u32 checks = 0, failed = 0;
    auto check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) {
            ++failed;
            std::printf("Exposure exact self-test: FAILED %s\n", what);
        }
    };
    struct Rec {
        VAddr addr;
        u32 size, tag;
        u64 id;
    };
    std::vector<Rec> recs;
    auto record = [&](VAddr addr, u32 size, u32 tag, u64 id) {
        recs.push_back({addr, size, tag, id});
        return true;
    };
    auto reg = [&](Fake& f, u32 size, u32 slot) {
        std::scoped_lock lk{g_exact_mutex};
        return RegisterExactSlots(f.obj, size, ReadExactSlots(f.obj, false), slot);
    };
    *reinterpret_cast<u32*>(image.data() + kOwnerGlobal) = 7;

    // Lag 2 (owner not this thread's): the current slot, generation - 2.
    a->Init(0x16, 10, 0, 1);
    a->label[0] = 8;
    GetterPick p = PickGetterSlot(a->obj, 0);
    check(p.ok && p.slot == 0 && p.want == 8 && p.data == reinterpret_cast<uintptr_t>(a->payload[0]), "lag 2 pick");
    // Lag 3 (owner is): the other slot, generation - 3.
    a->Init(0x16, 10, 0, 7);
    a->label[1] = 7;
    p = PickGetterSlot(a->obj, 0);
    check(p.ok && p.slot == 1 && p.want == 7 && p.data == reinterpret_cast<uintptr_t>(a->payload[1]), "lag 3 pick");
    // The label still holds an older generation: not ready (nothing is served; the real getter says so too).
    a->label[1] = 5;
    check(!PickGetterSlot(a->obj, 0).ok, "older label is not ready");

    // Registration and EOPs.
    a->Init(0x16, 10, 0, 1);
    a->label[0] = 8;
    const u64 id0 = reg(*a, 0x400, 0);
    check(id0 != 0, "registered");
    recs.clear();
    ExactEop(reinterpret_cast<VAddr>(&a->label[0]), 8, true, record);
    check(recs.size() == 1 && recs[0].addr == reinterpret_cast<VAddr>(a->payload[0]) && recs[0].size == 0x400 &&
              recs[0].tag == 8 && recs[0].id == id0,
          "EOP records the slot's payload with its generation and id");
    recs.clear();
    ExactEop(reinterpret_cast<VAddr>(&a->label[0]), 9, false, record);
    check(recs.empty(), "non-immediate EOP data records nothing");
    ExactEop(reinterpret_cast<VAddr>(&a->label[0]) + 0x1000, 9, true, record);
    check(recs.empty(), "unregistered label records nothing");
    // A lower generation at the label: a new lifetime id before the snapshot is recorded.
    ExactEop(reinterpret_cast<VAddr>(&a->label[0]), 2, true, record);
    check(recs.size() == 1 && recs[0].id != id0 && recs[0].tag == 2, "lower EOP generation renews the id");
    const u64 id1 = recs.empty() ? 0 : recs[0].id;
    check(reg(*a, 0x400, 0) == id1, "getter sees the renewed id");
    // The object made again at the same addresses: its issue counter goes back.
    *reinterpret_cast<u32*>(a->obj + 0x88) = 3;
    const u64 id2 = reg(*a, 0x400, 0);
    check(id2 != id1 && id2 != id0, "lower issue counter renews the id");
    // Another object takes over slot 0's label with its own payload.
    b->Init(0x16, 4, 0, 1);
    b->label_ptr[0] = reinterpret_cast<u64>(&a->label[0]);
    const u64 idb = reg(*b, 0x400, 0);
    recs.clear();
    ExactEop(reinterpret_cast<VAddr>(&a->label[0]), 4, true, record);
    check(recs.size() == 1 && recs[0].addr == reinterpret_cast<VAddr>(b->payload[0]) && recs[0].id == idb,
          "a label taken over records only the new owner's payload");
    // A counted object (the cloud): payload and count, same tag and id.
    {
        std::scoped_lock lk{g_exact_mutex};
        g_exact_labels.clear();
        g_exact_count.store(0);
    }
    a->Init(0x36, 20, 1, 1);
    *reinterpret_cast<u32*>(a->obj + 0x48 + 1 * 0x30) = 19;
    a->label[1] = 19;
    p = PickGetterSlot(a->obj, 19);
    check(p.ok && p.slot == 1 && p.want == 19 && p.count == reinterpret_cast<uintptr_t>(&a->count[1]),
          "explicit request picks the current slot");
    check(!PickGetterSlot(a->obj, 18).ok, "explicit request for another frame is not ready");
    u64 idc = 0;
    {
        std::scoped_lock lk{g_exact_mutex};
        idc = RegisterExactSlots(a->obj, 16, ReadExactSlots(a->obj, true), 1);
    }
    recs.clear();
    ExactEop(reinterpret_cast<VAddr>(&a->label[1]), 19, true, record);
    check(recs.size() == 2 && recs[0].size == 16 && recs[1].addr == reinterpret_cast<VAddr>(&a->count[1]) &&
              recs[1].size == 4 && recs[0].tag == 19 && recs[1].tag == 19 && recs[0].id == idc && recs[1].id == idc,
          "counted slot records point and count");

    // The snapshot lookup around ReadTaggedSnapshot's wait: done only when the entry found has finished; an entry
    // replaced during the wait by a newer unfinished copy of the same key is pending, a dropped one is missing.
    {
        struct Entry {
            u32 key;
            u64 tick;
        };
        std::vector<Entry> entries{{1, 5}, {2, 7}};
        u64 gpu = 5;
        const auto done_at = [&](u64 t) { return gpu >= t; };
        const auto key1 = [](const Entry& e) { return e.key == 1; };
        bool done = false;
        check(VideoCore::BufferCache::LookupSnapshot(entries, key1, done_at, &done) == &entries[0] && done,
              "lookup: finished copy is done");
        gpu = 4;
        check(VideoCore::BufferCache::LookupSnapshot(entries, key1, done_at, &done) && !done,
              "lookup: unfinished copy is pending");
        gpu = 5;
        entries[0].tick = 9; // replaced while the reader waited for tick 5
        check(VideoCore::BufferCache::LookupSnapshot(entries, key1, done_at, &done) && !done,
              "lookup: replacement after the wait is pending");
        entries.erase(entries.begin());
        check(!VideoCore::BufferCache::LookupSnapshot(entries, key1, done_at, &done) && !done,
              "lookup: dropped entry is missing");
    }
    // Cloud counts: zero, nonzero, clamped (unsigned, as cmova), and what may be served.
    check(ClampCount(0, 8) == 0 && ClampCount(5, 8) == 5 && ClampCount(8, 8) == 8 && ClampCount(20, 8) == 8 &&
              ClampCount(0xffffffffu, 8) == 8,
          "count clamp");
    check(DecideCloudServe(false, 0, false) == CloudServe::RealGetter &&
              DecideCloudServe(false, 5, true) == CloudServe::RealGetter,
          "no count snapshot: the real getter");
    check(DecideCloudServe(true, 0, false) == CloudServe::NoPoint, "count 0: served without a point");
    check(DecideCloudServe(true, 5, true) == CloudServe::Point, "count and point snapshots: served");
    check(DecideCloudServe(true, 5, false) == CloudServe::RealGetter, "count without its point: the real getter");

    {
        std::scoped_lock lk{g_exact_mutex};
        g_exact_labels.clear();
        g_exact_count.store(0);
    }
    g_exact_recorded.store(0);
    g_exact_epochs.store(0);
    g_image_base = saved_base;
    std::printf("Exposure exact self-test: %u of %u checks passed\n", checks - failed, checks);
}
} // namespace

void VideoCore::ExposureExactAtEop(Vulkan::Rasterizer& rasterizer, VAddr label, u32 tag, bool immediate) {
    if (ExactEop(label, tag, immediate, [&](VAddr addr, u32 size, u32 t, u64 id) {
            return rasterizer.RecordTaggedSnapshot(addr, size, t, id);
        })) {
        rasterizer.FlushAtExactEop();
    }
}

// For src/probe.c: BB_EXPOSURE_EXACT=1, the hook of the exposure getter call `site`, or 0.
extern "C" uintptr_t bbgpu_exposure_exact_hook(uintptr_t real_getter, int site) {
    if (!ExposureExact()) {
        return 0;
    }
    if (site == 0 && EnvOne("BB_EXPOSURE_EXACT_SELFTEST")) {
        ExposureExactSelfTest();
    }
    g_image_base = real_getter - 0x758980; // PickGetterSlot's latency branch reads a global
    g_real_getter = reinterpret_cast<ReadbackGetter>(real_getter);
    return site == 0   ? reinterpret_cast<uintptr_t>(&ExposureExactGet<0>)
           : site == 1 ? reinterpret_cast<uintptr_t>(&ExposureExactGet<1>)
                       : reinterpret_cast<uintptr_t>(&ExposureExactGet<2>);
}

// For src/probe.c: the hook of the exposure value's getter call; real_getter is eboot 0x758980. 0 unless
// BB_EXPOSURE_STALE=1: the last delivered value is served without matching the generation the getter selected.
extern "C" uintptr_t bbgpu_exposure_value_hook(uintptr_t real_getter) {
    const char* env = std::getenv("BB_EXPOSURE_STALE");
    if (!env || env[0] != '1') { // opt-in: the served value can be older than the generation the game selected
        return 0;
    }
    g_real_getter = reinterpret_cast<ReadbackGetter>(real_getter);
    return reinterpret_cast<uintptr_t>(&ExposureValueGet);
}

// For src/probe.c: the hook of FUN_01051f00's getter call (eboot 0x1051f3c), or 0. BB_QUERY_EXACT (default on,
// =0 off): the requested generation's count and results, recorded at its EOP like the exposure and cloud (CountedExactGet<4>).
extern "C" uintptr_t bbgpu_query_hook(uintptr_t image_base) {
    const char* env = std::getenv("BB_QUERY_EXACT");
    if (env && env[0] == '0') {
        return 0;
    }
    g_image_base = image_base;
    g_real_getter = reinterpret_cast<ReadbackGetter>(image_base + 0x758980);
    return reinterpret_cast<uintptr_t>(&CountedExactGet<4>);
}

// For src/probe.c: the hook of FUN_00574d30's getter call (eboot 0x574d79), or 0. BB_CROWD_EXACT (default on,
// =0 off): the requested generation's crowd ground heights, recorded at its EOP (ExposureExactGet<5>).
extern "C" uintptr_t bbgpu_crowd_hook(uintptr_t image_base) {
    const char* env = std::getenv("BB_CROWD_EXACT");
    if (env && env[0] == '0') {
        return 0;
    }
    g_image_base = image_base;
    g_real_getter = reinterpret_cast<ReadbackGetter>(image_base + 0x758980);
    return reinterpret_cast<uintptr_t>(&ExposureExactGet<5>);
}

// For src/probe.c: the hook of the cloud sample's getter call (BB_CLOUD_STALE=1), or 0. Opt-in: at 1440p it halves
// frame-time jitter at the same FPS (1.49 -> 0.81-0.84 ms), but the count and the point are fetched
// separately (not one producer generation), and a replayed successful result resets the caller's age timer
// (0x602678) and validity (0x60269f) that the game would let expire. Needs one coherent result
// per generation, or "not ready" while none is delivered, before it can be a default.
extern "C" uintptr_t bbgpu_cloud_hook(uintptr_t image_base) {
    if (CloudExact()) { // BB_CLOUD_EXACT (default on, see ExposureExactAtEop): the requested generation, exactly
        g_image_base = image_base;
        g_real_getter = reinterpret_cast<ReadbackGetter>(image_base + 0x758980);
        return reinterpret_cast<uintptr_t>(&CountedExactGet<3>);
    }
    const char* env = std::getenv("BB_CLOUD_STALE");
    if (!(env && env[0] == '1')) {
        return 0;
    }
    g_image_base = image_base;
    g_real_getter = reinterpret_cast<ReadbackGetter>(image_base + 0x758980);
    StartFetcher();
    return reinterpret_cast<uintptr_t>(&CloudGet);
}

// The hook for src/probe.c, or 0 unless BB_EXPOSURE_STALE=1.
extern "C" uintptr_t bbgpu_exposure_hook(void) {
    const char* env = std::getenv("BB_EXPOSURE_STALE");
    if (!env || env[0] != '1') { // opt-in: the served value can be older than the generation the game selected
        return 0;
    }
    StartFetcher();
    return reinterpret_cast<uintptr_t>(&ExposureCopy);
}
