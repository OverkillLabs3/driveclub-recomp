// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Changed for this port. The upstream sources of the vendored tree are listed in
// gpu/VENDOR.txt and in NOTICE.
//
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

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#include "common/decoder.h"
#include <immintrin.h>
extern "C" int runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
#endif

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
// driveclub: BB_FAULT_EMULATE=1. Guest read faults in DriveClub mostly read CPU data that shares a 4 KB page with a
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
    BbStats::fault_emulate_gpu.fetch_add(1, std::memory_order_relaxed);
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
               EmulateNonGpuIntAlu(ctx, inst, ops, fault, rasterizer);
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
        BbStats::store_why[0].fetch_add(1, std::memory_order_relaxed);
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
        BbStats::store_why[0].fetch_add(1, std::memory_order_relaxed);
        if (BbStats::enabled) {
            BbStats::NoteStoreSkip(ctx->Rip);
        }
        return false;
    }
    const ZydisDecodedOperand& dst = ops[0];
    const ZydisDecodedOperand& src = ops[1];
    if (dst.type != ZYDIS_OPERAND_TYPE_MEMORY || dst.mem.segment == ZYDIS_REGISTER_FS ||
        dst.mem.segment == ZYDIS_REGISTER_GS) {
        BbStats::store_why[1].fetch_add(1, std::memory_order_relaxed);
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
        BbStats::store_why[2].fetch_add(1, std::memory_order_relaxed);
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
        BbStats::store_why[3].fetch_add(1, std::memory_order_relaxed);
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
            BbStats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
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
                    BbStats::store_emulated.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                BbStats::store_emulate_declined.fetch_add(1, std::memory_order_relaxed);
            }
#endif
            return rasterizer->OnWriteFault(addr, is_gpu_thread);
        } else {
            BbStats::read_faults.fetch_add(1, std::memory_order_relaxed);
            if (is_gpu_thread && BbStats::enabled) {
                BbStats::NoteGpuReadFault(Common::FaultInstructionPointer(context));
            }
            BbStats::Timer timer{BbStats::t_read_faults};
            BbTrace::fault_rip = Common::FaultInstructionPointer(context);
#ifdef _WIN64
            // Only read faults from GPU read protection (a page holding GPU data).
            if (!is_gpu_thread && FaultEmulation() && rasterizer->IsPageGpuModified(addr) &&
                PageEmulationBudgetLeft(addr)) {
                if (EmulateNonGpuLoad(context, addr, rasterizer)) {
                    BbStats::fault_emulated.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                BbStats::fault_emulate_declined.fetch_add(1, std::memory_order_relaxed);
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
