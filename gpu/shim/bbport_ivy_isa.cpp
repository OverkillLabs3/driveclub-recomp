// SPDX-License-Identifier: GPL-2.0-or-later
// Ivy Bridge / pre-Haswell AVX1 compatibility layer for DriveClub Recomp.
//
// Lowers PS4 Jaguar instructions missing on Ivy Bridge:
//   - LZCNT (F3 0F BD) : silent BSR corruption on Ivy -> statically rewritten (or UD2-trapped if 4B)
//   - TZCNT (F3 0F BC) : silent BSF corruption on Ivy -> statically rewritten (or UD2-trapped if 4B)
//   - MOVBE (0F 38 F0/F1) : #UD on Ivy -> statically rewritten + VEH fallback
//   - BMI1 VEX (ANDN, BEXTR, BLSI, BLSMSK, BLSR) : #UD on Ivy (always >= 5B) -> statically rewritten + VEH fallback
#include "bbport_ivy_isa.h"

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <Zydis/Zydis.h>
#include <xbyak/xbyak.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace {

constexpr uint64_t kRedZone = 128;
constexpr size_t kArenaSize = 16 * 1024 * 1024; // 16 MiB trampoline pool
constexpr uint64_t kEflagsCf = 1ull << 0;
constexpr uint64_t kEflagsZf = 1ull << 6;
constexpr uint64_t kEflagsSf = 1ull << 7;
constexpr uint64_t kEflagsOf = 1ull << 11;

enum MnemonicSlot : size_t {
    kSlotLzcnt = 0,
    kSlotTzcnt,
    kSlotMovbe,
    kSlotAndn,
    kSlotBextr,
    kSlotBlsi,
    kSlotBlsmsk,
    kSlotBlsr,
    kSlotCount
};

const char* kSlotNames[kSlotCount] = {
    "LZCNT", "TZCNT", "MOVBE", "ANDN", "BEXTR", "BLSI", "BLSMSK", "BLSR",
};

struct TrappedSite {
    uintptr_t rip;
    uint8_t orig_len;
    uint8_t orig_bytes[15];
    uint64_t hits; // updated atomically inside VEH
};

struct IvyState {
    bool active = false;
    bool need_bmi1 = false;
    bool need_movbe = false;
    bool need_lzcnt = false;
    bool stats_enabled = false;

    uintptr_t image_base = 0;
    uint64_t image_size = 0;

    uint8_t* arena = nullptr;
    size_t arena_size = 0;
    size_t arena_used = 0;

    ZydisDecoder decoder{};
    bool decoder_ready = false;

    // Sorted by `rip` for lock-free O(log N) lookup inside vectored_fault().
    std::vector<TrappedSite> trapped_sites;

    uint64_t rewritten_trampoline[kSlotCount]{};
    uint64_t rewritten_ud2[kSlotCount]{};
    uint64_t bextr_const_fast = 0;
    std::atomic<uint64_t> veh_hits[kSlotCount]{};
    std::atomic<uint64_t> veh_total{0};
};

IvyState g_ivy;

void Cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    uint32_t ra, rb, rc, rd;
    __asm__ volatile("cpuid" : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd) : "a"(leaf), "c"(subleaf));
    *a = ra;
    *b = rb;
    *c = rc;
    *d = rd;
}

void DetectFeatures(bool* need_bmi1, bool* need_movbe, bool* need_lzcnt) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    Cpuid(0, 0, &a, &b, &c, &d);
    const uint32_t max_basic = a;

    bool has_movbe = false;
    bool has_bmi1 = false;
    bool has_lzcnt = false;

    if (max_basic >= 1) {
        Cpuid(1, 0, &a, &b, &c, &d);
        has_movbe = ((c >> 22) & 1u) != 0;
    }
    if (max_basic >= 7) {
        Cpuid(7, 0, &a, &b, &c, &d);
        has_bmi1 = ((b >> 3) & 1u) != 0;
    }
    Cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000001u) {
        Cpuid(0x80000001u, 0, &a, &b, &c, &d);
        has_lzcnt = ((c >> 5) & 1u) != 0;
    }

    *need_bmi1 = !has_bmi1;
    *need_movbe = !has_movbe;
    *need_lzcnt = !has_lzcnt;
}

int ClassifyMnemonic(ZydisMnemonic m, bool need_bmi1, bool need_movbe, bool need_lzcnt) {
    switch (m) {
    case ZYDIS_MNEMONIC_LZCNT:
        return need_lzcnt ? int(kSlotLzcnt) : -1;
    case ZYDIS_MNEMONIC_TZCNT:
        return need_bmi1 ? int(kSlotTzcnt) : -1;
    case ZYDIS_MNEMONIC_MOVBE:
        return need_movbe ? int(kSlotMovbe) : -1;
    case ZYDIS_MNEMONIC_ANDN:
        return need_bmi1 ? int(kSlotAndn) : -1;
    case ZYDIS_MNEMONIC_BEXTR:
        return need_bmi1 ? int(kSlotBextr) : -1;
    case ZYDIS_MNEMONIC_BLSI:
        return need_bmi1 ? int(kSlotBlsi) : -1;
    case ZYDIS_MNEMONIC_BLSMSK:
        return need_bmi1 ? int(kSlotBlsmsk) : -1;
    case ZYDIS_MNEMONIC_BLSR:
        return need_bmi1 ? int(kSlotBlsr) : -1;
    default:
        return -1;
    }
}

bool IsSilentWithoutUd(ZydisMnemonic m) {
    return m == ZYDIS_MNEMONIC_LZCNT || m == ZYDIS_MNEMONIC_TZCNT;
}

// Map any Zydis GPR to its canonical x86-64 register index 0..15, or -1.
int ZydisRegToIdx(ZydisRegister r) {
    const ZydisRegister full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
    switch (full) {
    case ZYDIS_REGISTER_RAX: return 0;
    case ZYDIS_REGISTER_RCX: return 1;
    case ZYDIS_REGISTER_RDX: return 2;
    case ZYDIS_REGISTER_RBX: return 3;
    case ZYDIS_REGISTER_RSP: return 4;
    case ZYDIS_REGISTER_RBP: return 5;
    case ZYDIS_REGISTER_RSI: return 6;
    case ZYDIS_REGISTER_RDI: return 7;
    case ZYDIS_REGISTER_R8:  return 8;
    case ZYDIS_REGISTER_R9:  return 9;
    case ZYDIS_REGISTER_R10: return 10;
    case ZYDIS_REGISTER_R11: return 11;
    case ZYDIS_REGISTER_R12: return 12;
    case ZYDIS_REGISTER_R13: return 13;
    case ZYDIS_REGISTER_R14: return 14;
    case ZYDIS_REGISTER_R15: return 15;
    default: return -1;
    }
}

bool IsGpr64(ZydisRegister r) {
    return r >= ZYDIS_REGISTER_RAX && r <= ZYDIS_REGISTER_R15;
}

DWORD64* ContextGprPtr(CONTEXT* c, int idx) {
    switch (idx) {
    case 0:  return &c->Rax;
    case 1:  return &c->Rcx;
    case 2:  return &c->Rdx;
    case 3:  return &c->Rbx;
    case 4:  return &c->Rsp;
    case 5:  return &c->Rbp;
    case 6:  return &c->Rsi;
    case 7:  return &c->Rdi;
    case 8:  return &c->R8;
    case 9:  return &c->R9;
    case 10: return &c->R10;
    case 11: return &c->R11;
    case 12: return &c->R12;
    case 13: return &c->R13;
    case 14: return &c->R14;
    case 15: return &c->R15;
    default: return nullptr;
    }
}

uint64_t WidthMask(uint32_t bits) {
    if (bits == 64) return ~UINT64_C(0);
    return (UINT64_C(1) << bits) - 1;
}

uint64_t ReadRegValue(const CONTEXT* c, ZydisRegister r, uint32_t bits) {
    const int idx = ZydisRegToIdx(r);
    if (idx < 0) return 0;
    const DWORD64* p = ContextGprPtr(const_cast<CONTEXT*>(c), idx);
    return (*p) & WidthMask(bits);
}

void WriteRegValue(CONTEXT* c, ZydisRegister r, uint32_t bits, uint64_t val) {
    const int idx = ZydisRegToIdx(r);
    if (idx < 0) return;
    DWORD64* p = ContextGprPtr(c, idx);
    if (bits == 64) {
        *p = val;
    } else if (bits == 32) {
        // 32-bit GPR writes zero-extend to 64 bits on x86-64.
        *p = static_cast<uint32_t>(val);
    } else if (bits == 16) {
        // 16-bit GPR writes preserve upper 48 bits.
        *p = (*p & ~UINT64_C(0xFFFF)) | (val & 0xFFFFu);
    }
}

uint64_t ByteSwap(uint64_t v, uint32_t bits) {
    if (bits == 16) return ((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8);
    if (bits == 32) return __builtin_bswap32(static_cast<uint32_t>(v));
    return __builtin_bswap64(v);
}

uint32_t CountLz(uint64_t v, uint32_t bits) {
    v &= WidthMask(bits);
    if (v == 0) return bits;
    uint32_t lz64 = static_cast<uint32_t>(__builtin_clzll(v));
    return lz64 - (64u - bits);
}

uint32_t CountTz(uint64_t v, uint32_t bits) {
    v &= WidthMask(bits);
    if (v == 0) return bits;
    return static_cast<uint32_t>(__builtin_ctzll(v));
}

bool CalcMemEa(const CONTEXT* c, const ZydisDecodedInstruction& inst, const ZydisDecodedOperand& op,
               uintptr_t rip, uint64_t* out_ea) {
    if (op.mem.segment == ZYDIS_REGISTER_FS || op.mem.segment == ZYDIS_REGISTER_GS) {
        return false;
    }
    static constexpr ZydisRegister kGprs[16] = {
        ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX, ZYDIS_REGISTER_RBX,
        ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_RBP, ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_RDI,
        ZYDIS_REGISTER_R8,  ZYDIS_REGISTER_R9,  ZYDIS_REGISTER_R10, ZYDIS_REGISTER_R11,
        ZYDIS_REGISTER_R12, ZYDIS_REGISTER_R13, ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R15,
    };
    ZydisRegisterContext ctx{};
    for (int i = 0; i < 16; ++i) {
        ctx.values[kGprs[i]] = *ContextGprPtr(const_cast<CONTEXT*>(c), i);
    }
    ZyanU64 ea = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&inst, &op, rip, &ctx, &ea))) {
        return false;
    }
    *out_ea = ea;
    return true;
}

bool ReadOperandValue(const CONTEXT* c, const ZydisDecodedInstruction& inst, const ZydisDecodedOperand& op,
                      uintptr_t rip, uint32_t bits, uint64_t* out_val) {
    if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        *out_val = ReadRegValue(c, op.reg.value, bits);
        return true;
    }
    if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        uint64_t ea = 0;
        if (!CalcMemEa(c, inst, op, rip, &ea)) return false;
        uint64_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(ea), bits / 8);
        *out_val = v & WidthMask(bits);
        return true;
    }
    return false;
}

// ============================================================================
// Xbyak Trampoline Emission
// ============================================================================

inline Xbyak::Reg64 R64(int idx) { return Xbyak::Reg64(idx); }
inline Xbyak::Reg32 R32(int idx) { return Xbyak::Reg32(idx); }
inline Xbyak::Reg16 R16(int idx) { return Xbyak::Reg16(idx); }

bool FitsRel32(uintptr_t from_end, uintptr_t to) {
    const int64_t d = static_cast<int64_t>(to) - static_cast<int64_t>(from_end);
    return d >= INT32_MIN && d <= INT32_MAX;
}

void EmitJmpRel32(Xbyak::CodeGenerator& c, uintptr_t target) {
    const uintptr_t from_end = reinterpret_cast<uintptr_t>(c.getCurr()) + 5;
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(from_end));
    c.db(0xE9);
    c.dd(static_cast<uint32_t>(rel));
}

void EmitJccRel32(Xbyak::CodeGenerator& c, uint8_t cc, uintptr_t target) {
    const uintptr_t from_end = reinterpret_cast<uintptr_t>(c.getCurr()) + 6;
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(from_end));
    c.db(0x0F);
    c.db(static_cast<uint8_t>(0x80u | (cc & 0x0Fu)));
    c.dd(static_cast<uint32_t>(rel));
}

enum class FuseTailKind { None, RawBytes, Ret, JmpRel, JccRel };

struct FuseTail {
    FuseTailKind kind = FuseTailKind::None;
    const uint8_t* raw = nullptr;
    size_t orig_extra_len = 0;
    uint8_t jcc_cc = 0;
    uintptr_t branch_target = 0;
};

uint16_t OperandRegMask(const ZydisDecodedOperand& op) {
    uint16_t mask = 0;
    if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int idx = ZydisRegToIdx(op.reg.value);
        if (idx >= 0) mask |= static_cast<uint16_t>(1u << idx);
    } else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        const int b = ZydisRegToIdx(op.mem.base);
        const int x = ZydisRegToIdx(op.mem.index);
        if (b >= 0) mask |= static_cast<uint16_t>(1u << b);
        if (x >= 0) mask |= static_cast<uint16_t>(1u << x);
    }
    return mask;
}

int PickScratch(uint16_t& used_mask) {
    // Prefer caller-saved regs (r11, r10, r9, r8, rax, rdx, rcx, rsi, rdi...) though we always save/restore.
    static constexpr int kOrder[15] = {11, 10, 9, 8, 0, 2, 1, 6, 7, 3, 5, 12, 13, 14, 15};
    for (int r : kOrder) {
        if ((used_mask & (1u << r)) == 0) {
            used_mask |= static_cast<uint16_t>(1u << r);
            return r;
        }
    }
    return -1;
}

// Validate that a memory operand can be emitted cleanly by Xbyak in 64-bit mode.
bool MemOperandSupported(const ZydisDecodedOperand& op) {
    if (op.type != ZYDIS_OPERAND_TYPE_MEMORY) return true;
    if (op.mem.segment == ZYDIS_REGISTER_FS || op.mem.segment == ZYDIS_REGISTER_GS) return false;
    if (op.mem.base != ZYDIS_REGISTER_NONE && op.mem.base != ZYDIS_REGISTER_RIP && !IsGpr64(op.mem.base)) {
        return false; // 32-bit address-size override
    }
    if (op.mem.index != ZYDIS_REGISTER_NONE && !IsGpr64(op.mem.index)) {
        return false;
    }
    return true;
}

struct PreparedMem {
    bool is_mem = false;
    bool uses_rip_or_abs = false;
    uint64_t abs_ea = 0;
    Xbyak::RegExp re;
};

bool PrepareMemOperand(const ZydisDecodedInstruction& inst, const ZydisDecodedOperand& op,
                       uintptr_t orig_rip, int64_t rsp_delta, PreparedMem* out) {
    out->is_mem = (op.type == ZYDIS_OPERAND_TYPE_MEMORY);
    if (!out->is_mem) return true;
    if (!MemOperandSupported(op)) return false;

    const int64_t raw_disp = op.mem.disp.has_displacement ? op.mem.disp.value : 0;

    if (op.mem.base == ZYDIS_REGISTER_RIP) {
        out->uses_rip_or_abs = true;
        out->abs_ea = orig_rip + inst.length + static_cast<uint64_t>(raw_disp);
        return true;
    }
    if (op.mem.base == ZYDIS_REGISTER_NONE && op.mem.index == ZYDIS_REGISTER_NONE) {
        out->uses_rip_or_abs = true;
        out->abs_ea = static_cast<uint64_t>(raw_disp);
        return true;
    }

    int64_t disp = raw_disp;
    Xbyak::RegExp re;
    if (op.mem.base != ZYDIS_REGISTER_NONE) {
        const int b = ZydisRegToIdx(op.mem.base);
        if (b < 0) return false;
        if (b == 4) disp += rsp_delta; // RSP shifted down by rsp_delta
        re = re + R64(b);
    }
    if (op.mem.index != ZYDIS_REGISTER_NONE) {
        const int x = ZydisRegToIdx(op.mem.index);
        if (x < 0) return false;
        const uint8_t scale = op.mem.scale ? op.mem.scale : 1;
        re = re + R64(x) * scale;
    }
    if (disp < INT32_MIN || disp > INT32_MAX) return false;
    if (disp != 0) {
        re = re + static_cast<int32_t>(disp);
    }
    out->uses_rip_or_abs = false;
    out->re = re;
    return true;
}

Xbyak::Address SizedAddr(const PreparedMem& pm, int addr_reg, uint32_t bits) {
    const Xbyak::RegExp expr = pm.uses_rip_or_abs ? Xbyak::RegExp(R64(addr_reg)) : pm.re;
    if (bits == 16) return Xbyak::util::word[expr];
    if (bits == 32) return Xbyak::util::dword[expr];
    return Xbyak::util::qword[expr];
}

void EmitLoadFromOp(Xbyak::CodeGenerator& c, int dst_idx, const ZydisDecodedOperand& op,
                    const PreparedMem& pm, int addr_reg, uint32_t bits) {
    if (!pm.is_mem) {
        const int src_idx = ZydisRegToIdx(op.reg.value);
        if (dst_idx == src_idx && bits != 32) return;
        if (bits == 16) c.mov(R16(dst_idx), R16(src_idx));
        else if (bits == 32) c.mov(R32(dst_idx), R32(src_idx));
        else c.mov(R64(dst_idx), R64(src_idx));
        return;
    }
    if (pm.uses_rip_or_abs) {
        c.mov(R64(addr_reg), pm.abs_ea);
    }
    const Xbyak::Address addr = SizedAddr(pm, addr_reg, bits);
    if (bits == 16) c.mov(R16(dst_idx), addr);
    else if (bits == 32) c.mov(R32(dst_idx), addr);
    else c.mov(R64(dst_idx), addr);
}

struct LowerOpDesc {
    const ZydisDecodedInstruction* inst;
    const ZydisDecodedOperand* ops;
    uintptr_t orig_rip;
    bool has_bextr_const = false;
    uint32_t bextr_const_ctrl = 0;
};

// Emits the lowered x86-64 sequence for ONE guest instruction into `c` (leaves RSP restored and
// exact architectural result + EFLAGS intact, ready for either another lowered op or return jump).
bool EmitLoweredOp(Xbyak::CodeGenerator& c, const LowerOpDesc& op) {
    const ZydisDecodedInstruction& inst = *op.inst;
    const ZydisDecodedOperand* ops = op.ops;
    const uintptr_t orig_rip = op.orig_rip;
    const bool has_bextr_const = op.has_bextr_const;
    const uint32_t bextr_const_ctrl = op.bextr_const_ctrl;

    // Mask of registers used by the guest instruction (plus RSP=4 always reserved).
    uint16_t used = (1u << 4);
    for (uint8_t i = 0; i < inst.operand_count_visible; ++i) {
        used |= OperandRegMask(ops[i]);
    }

    const auto m = inst.mnemonic;
    if (m == ZYDIS_MNEMONIC_LZCNT || m == ZYDIS_MNEMONIC_TZCNT) {
        if (inst.operand_count_visible < 2 || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
        const uint32_t bits = ops[0].size;
        if (bits != 16 && bits != 32 && bits != 64) return false;
        if (!MemOperandSupported(ops[1])) return false;
        const int dst = ZydisRegToIdx(ops[0].reg.value);
        if (dst < 0) return false;

        uint16_t alloc = used;
        const int tmp = PickScratch(alloc);
        if (tmp < 0) return false;
        const int64_t frame = kRedZone + 8;

        PreparedMem pm{};
        if (!PrepareMemOperand(inst, ops[1], orig_rip, frame, &pm)) return false;

        int addr_reg = -1;
        int64_t actual_frame = frame;
        if (pm.uses_rip_or_abs) {
            addr_reg = PickScratch(alloc);
            if (addr_reg < 0) return false;
            actual_frame = kRedZone + 16;
            if (!PrepareMemOperand(inst, ops[1], orig_rip, actual_frame, &pm)) return false;
        }

        c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp - static_cast<int32_t>(actual_frame)]);
        c.mov(Xbyak::util::qword[Xbyak::util::rsp], R64(tmp));
        if (addr_reg >= 0) {
            c.mov(Xbyak::util::qword[Xbyak::util::rsp + 8], R64(addr_reg));
            c.mov(R64(addr_reg), pm.abs_ea);
        }

        const uint32_t bit_for_cf = (bits == 64) ? 6u : (bits == 32 ? 5u : 4u);
        if (m == ZYDIS_MNEMONIC_LZCNT) {
            const uint32_t zero_pre_xor = (bits * 2u) - 1u;
            const uint32_t xor_imm = bits - 1u;
            if (bits == 16) {
                c.mov(R16(tmp), static_cast<uint16_t>(zero_pre_xor));
                if (pm.is_mem) c.bsr(R16(dst), SizedAddr(pm, addr_reg, 16));
                else c.bsr(R16(dst), R16(ZydisRegToIdx(ops[1].reg.value)));
                c.cmovz(R16(dst), R16(tmp));
                c.xor_(R16(dst), static_cast<uint8_t>(xor_imm));
                c.bt(R16(dst), static_cast<uint8_t>(bit_for_cf));
            } else if (bits == 32) {
                c.mov(R32(tmp), zero_pre_xor);
                if (pm.is_mem) c.bsr(R32(dst), SizedAddr(pm, addr_reg, 32));
                else c.bsr(R32(dst), R32(ZydisRegToIdx(ops[1].reg.value)));
                c.cmovz(R32(dst), R32(tmp));
                c.xor_(R32(dst), static_cast<uint8_t>(xor_imm));
                c.bt(R32(dst), static_cast<uint8_t>(bit_for_cf));
            } else {
                c.mov(R32(tmp), zero_pre_xor);
                if (pm.is_mem) c.bsr(R64(dst), SizedAddr(pm, addr_reg, 64));
                else c.bsr(R64(dst), R64(ZydisRegToIdx(ops[1].reg.value)));
                c.cmovz(R64(dst), R64(tmp));
                c.xor_(R64(dst), static_cast<uint8_t>(xor_imm));
                c.bt(R64(dst), static_cast<uint8_t>(bit_for_cf));
            }
        } else { // TZCNT
            if (bits == 16) {
                c.mov(R16(tmp), static_cast<uint16_t>(bits));
                if (pm.is_mem) c.bsf(R16(dst), SizedAddr(pm, addr_reg, 16));
                else c.bsf(R16(dst), R16(ZydisRegToIdx(ops[1].reg.value)));
                c.cmovz(R16(dst), R16(tmp));
                c.test(R16(dst), R16(dst));
                c.bt(R16(dst), static_cast<uint8_t>(bit_for_cf));
            } else if (bits == 32) {
                c.mov(R32(tmp), bits);
                if (pm.is_mem) c.bsf(R32(dst), SizedAddr(pm, addr_reg, 32));
                else c.bsf(R32(dst), R32(ZydisRegToIdx(ops[1].reg.value)));
                c.cmovz(R32(dst), R32(tmp));
                c.test(R32(dst), R32(dst));
                c.bt(R32(dst), static_cast<uint8_t>(bit_for_cf));
            } else {
                c.mov(R32(tmp), bits);
                if (pm.is_mem) c.bsf(R64(dst), SizedAddr(pm, addr_reg, 64));
                else c.bsf(R64(dst), R64(ZydisRegToIdx(ops[1].reg.value)));
                c.cmovz(R64(dst), R64(tmp));
                c.test(R64(dst), R64(dst));
                c.bt(R64(dst), static_cast<uint8_t>(bit_for_cf));
            }
        }

        if (addr_reg >= 0) c.mov(R64(addr_reg), Xbyak::util::qword[Xbyak::util::rsp + 8]);
        c.mov(R64(tmp), Xbyak::util::qword[Xbyak::util::rsp]);
        c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp + static_cast<int32_t>(actual_frame)]);
        return true;
    }

    if (m == ZYDIS_MNEMONIC_MOVBE) {
        if (inst.operand_count_visible < 2) return false;
        const uint32_t bits = ops[0].size;
        if (bits != 16 && bits != 32 && bits != 64) return false;

        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            const int dst = ZydisRegToIdx(ops[0].reg.value);
            if (dst < 0 || !MemOperandSupported(ops[1])) return false;
            PreparedMem pm{};
            if (!PrepareMemOperand(inst, ops[1], orig_rip, 0, &pm)) return false;

            if (bits == 32 || bits == 64) {
                if (!pm.uses_rip_or_abs) {
                    if (bits == 32) {
                        c.mov(R32(dst), SizedAddr(pm, -1, 32));
                        c.bswap(R32(dst));
                    } else {
                        c.mov(R64(dst), SizedAddr(pm, -1, 64));
                        c.bswap(R64(dst));
                    }
                } else {
                    c.mov(R64(dst), pm.abs_ea);
                    if (bits == 32) {
                        c.mov(R32(dst), Xbyak::util::dword[R64(dst)]);
                        c.bswap(R32(dst));
                    } else {
                        c.mov(R64(dst), Xbyak::util::qword[R64(dst)]);
                        c.bswap(R64(dst));
                    }
                }
            } else {
                const bool save_rax = (dst != 0);
                uint16_t alloc = used | 1u;
                const int addr_reg = pm.uses_rip_or_abs ? PickScratch(alloc) : -1;
                if (pm.uses_rip_or_abs && addr_reg < 0) return false;
                const int slots = (save_rax ? 1 : 0) + (addr_reg >= 0 ? 1 : 0);
                const int64_t frame = slots ? static_cast<int64_t>(kRedZone + 8 * slots) : 0;
                if (!PrepareMemOperand(inst, ops[1], orig_rip, frame, &pm)) return false;

                if (frame) c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp - static_cast<int32_t>(frame)]);
                int off = 0;
                if (save_rax) { c.mov(Xbyak::util::qword[Xbyak::util::rsp + off], Xbyak::util::rax); off += 8; }
                if (addr_reg >= 0) {
                    c.mov(Xbyak::util::qword[Xbyak::util::rsp + off], R64(addr_reg));
                    c.mov(R64(addr_reg), pm.abs_ea);
                }
                c.mov(Xbyak::util::ax, SizedAddr(pm, addr_reg, 16));
                c.xchg(Xbyak::util::al, Xbyak::util::ah);
                if (dst != 0) c.mov(R16(dst), Xbyak::util::ax);
                off = 0;
                if (save_rax) { c.mov(Xbyak::util::rax, Xbyak::util::qword[Xbyak::util::rsp + off]); off += 8; }
                if (addr_reg >= 0) c.mov(R64(addr_reg), Xbyak::util::qword[Xbyak::util::rsp + off]);
                if (frame) c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp + static_cast<int32_t>(frame)]);
            }
            return true;
        }
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const int src = ZydisRegToIdx(ops[1].reg.value);
            if (src < 0 || !MemOperandSupported(ops[0])) return false;
            const uint16_t mem_regs = OperandRegMask(ops[0]);

            PreparedMem pm{};
            if (!PrepareMemOperand(inst, ops[0], orig_rip, 0, &pm)) return false;

            if (bits == 64 && !pm.uses_rip_or_abs && ((mem_regs & (1u << src)) == 0)) {
                c.bswap(R64(src));
                c.mov(SizedAddr(pm, -1, 64), R64(src));
                c.bswap(R64(src));
            } else if (bits == 32 || bits == 64) {
                uint16_t alloc = used;
                const int tmp = PickScratch(alloc);
                const int addr_reg = pm.uses_rip_or_abs ? PickScratch(alloc) : -1;
                if (tmp < 0 || (pm.uses_rip_or_abs && addr_reg < 0)) return false;
                const int64_t frame = static_cast<int64_t>(kRedZone + (addr_reg >= 0 ? 16 : 8));
                if (!PrepareMemOperand(inst, ops[0], orig_rip, frame, &pm)) return false;

                c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp - static_cast<int32_t>(frame)]);
                c.mov(Xbyak::util::qword[Xbyak::util::rsp], R64(tmp));
                if (addr_reg >= 0) {
                    c.mov(Xbyak::util::qword[Xbyak::util::rsp + 8], R64(addr_reg));
                    c.mov(R64(addr_reg), pm.abs_ea);
                }
                if (bits == 32) {
                    c.mov(R32(tmp), R32(src));
                    c.bswap(R32(tmp));
                    c.mov(SizedAddr(pm, addr_reg, 32), R32(tmp));
                } else {
                    c.mov(R64(tmp), R64(src));
                    c.bswap(R64(tmp));
                    c.mov(SizedAddr(pm, addr_reg, 64), R64(tmp));
                }
                if (addr_reg >= 0) c.mov(R64(addr_reg), Xbyak::util::qword[Xbyak::util::rsp + 8]);
                c.mov(R64(tmp), Xbyak::util::qword[Xbyak::util::rsp]);
                c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp + static_cast<int32_t>(frame)]);
            } else {
                uint16_t alloc = used | 1u;
                const int addr_reg = pm.uses_rip_or_abs ? PickScratch(alloc) : -1;
                const bool mem_uses_rax = (mem_regs & 1u) != 0;
                const int ea_reg = (addr_reg >= 0) ? addr_reg : (mem_uses_rax ? PickScratch(alloc) : -1);
                if ((pm.uses_rip_or_abs || mem_uses_rax) && ea_reg < 0) return false;

                const int64_t frame = static_cast<int64_t>(kRedZone + (ea_reg >= 0 ? 16 : 8));
                if (!PrepareMemOperand(inst, ops[0], orig_rip, frame, &pm)) return false;

                c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp - static_cast<int32_t>(frame)]);
                c.mov(Xbyak::util::qword[Xbyak::util::rsp], Xbyak::util::rax);
                if (ea_reg >= 0) {
                    c.mov(Xbyak::util::qword[Xbyak::util::rsp + 8], R64(ea_reg));
                    if (pm.uses_rip_or_abs) c.mov(R64(ea_reg), pm.abs_ea);
                    else c.lea(R64(ea_reg), Xbyak::util::ptr[pm.re]);
                }
                if (src != 0) c.mov(Xbyak::util::ax, R16(src));
                c.xchg(Xbyak::util::al, Xbyak::util::ah);
                if (ea_reg >= 0) c.mov(Xbyak::util::word[R64(ea_reg)], Xbyak::util::ax);
                else c.mov(SizedAddr(pm, -1, 16), Xbyak::util::ax);
                if (ea_reg >= 0) c.mov(R64(ea_reg), Xbyak::util::qword[Xbyak::util::rsp + 8]);
                c.mov(Xbyak::util::rax, Xbyak::util::qword[Xbyak::util::rsp]);
                c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp + static_cast<int32_t>(frame)]);
            }
            return true;
        }
        return false;
    }

    if (m == ZYDIS_MNEMONIC_ANDN) {
        if (inst.operand_count_visible < 3 || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return false;
        }
        const uint32_t bits = ops[0].size;
        if (bits != 32 && bits != 64) return false;
        if (!MemOperandSupported(ops[2])) return false;
        const int dst = ZydisRegToIdx(ops[0].reg.value);
        const int src1 = ZydisRegToIdx(ops[1].reg.value);
        if (dst < 0 || src1 < 0) return false;
        const uint16_t src2_regs = OperandRegMask(ops[2]);

        PreparedMem pm{};
        if (!PrepareMemOperand(inst, ops[2], orig_rip, 0, &pm)) return false;

        if (((src2_regs & (1u << dst)) == 0) && !pm.uses_rip_or_abs) {
            if (bits == 32) {
                c.mov(R32(dst), R32(src1));
                c.not_(R32(dst));
                if (pm.is_mem) c.and_(R32(dst), SizedAddr(pm, -1, 32));
                else c.and_(R32(dst), R32(ZydisRegToIdx(ops[2].reg.value)));
            } else {
                if (dst != src1) c.mov(R64(dst), R64(src1));
                c.not_(R64(dst));
                if (pm.is_mem) c.and_(R64(dst), SizedAddr(pm, -1, 64));
                else c.and_(R64(dst), R64(ZydisRegToIdx(ops[2].reg.value)));
            }
        } else {
            uint16_t alloc = used;
            const int tmp = PickScratch(alloc);
            const int addr_reg = pm.uses_rip_or_abs ? PickScratch(alloc) : -1;
            if (tmp < 0 || (pm.uses_rip_or_abs && addr_reg < 0)) return false;
            const int64_t frame = static_cast<int64_t>(kRedZone + (addr_reg >= 0 ? 16 : 8));
            if (!PrepareMemOperand(inst, ops[2], orig_rip, frame, &pm)) return false;

            c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp - static_cast<int32_t>(frame)]);
            c.mov(Xbyak::util::qword[Xbyak::util::rsp], R64(tmp));
            if (addr_reg >= 0) {
                c.mov(Xbyak::util::qword[Xbyak::util::rsp + 8], R64(addr_reg));
                c.mov(R64(addr_reg), pm.abs_ea);
            }
            if (bits == 32) {
                c.mov(R32(tmp), R32(src1));
                c.not_(R32(tmp));
                if (pm.is_mem) c.and_(R32(tmp), SizedAddr(pm, addr_reg, 32));
                else c.and_(R32(tmp), R32(ZydisRegToIdx(ops[2].reg.value)));
                c.mov(R32(dst), R32(tmp));
            } else {
                c.mov(R64(tmp), R64(src1));
                c.not_(R64(tmp));
                if (pm.is_mem) c.and_(R64(tmp), SizedAddr(pm, addr_reg, 64));
                else c.and_(R64(tmp), R64(ZydisRegToIdx(ops[2].reg.value)));
                c.mov(R64(dst), R64(tmp));
            }
            if (addr_reg >= 0) c.mov(R64(addr_reg), Xbyak::util::qword[Xbyak::util::rsp + 8]);
            c.mov(R64(tmp), Xbyak::util::qword[Xbyak::util::rsp]);
            c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp + static_cast<int32_t>(frame)]);
        }
        return true;
    }

    if (m == ZYDIS_MNEMONIC_BLSR || m == ZYDIS_MNEMONIC_BLSI || m == ZYDIS_MNEMONIC_BLSMSK) {
        if (inst.operand_count_visible < 2 || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
        const uint32_t bits = ops[0].size;
        if (bits != 32 && bits != 64) return false;
        if (!MemOperandSupported(ops[1])) return false;
        const int dst = ZydisRegToIdx(ops[0].reg.value);
        if (dst < 0) return false;

        uint16_t alloc = used;
        const int tmp = PickScratch(alloc);
        if (tmp < 0) return false;

        PreparedMem pm{};
        if (!PrepareMemOperand(inst, ops[1], orig_rip, 0, &pm)) return false;
        const int addr_reg = pm.uses_rip_or_abs ? PickScratch(alloc) : -1;
        if (pm.uses_rip_or_abs && addr_reg < 0) return false;

        const int64_t frame = static_cast<int64_t>(kRedZone + (addr_reg >= 0 ? 16 : 8));
        if (!PrepareMemOperand(inst, ops[1], orig_rip, frame, &pm)) return false;

        c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp - static_cast<int32_t>(frame)]);
        c.mov(Xbyak::util::qword[Xbyak::util::rsp], R64(tmp));
        if (addr_reg >= 0) c.mov(Xbyak::util::qword[Xbyak::util::rsp + 8], R64(addr_reg));

        EmitLoadFromOp(c, tmp, ops[1], pm, addr_reg, bits);

        Xbyak::Label lbl_zero, lbl_done;
        if (m == ZYDIS_MNEMONIC_BLSR) {
            if (bits == 32) {
                c.test(R32(tmp), R32(tmp));
                c.jz(lbl_zero);
                c.lea(R32(dst), Xbyak::util::ptr[R64(tmp) - 1]);
                c.and_(R32(dst), R32(tmp));
                c.jmp(lbl_done);
                c.L(lbl_zero);
                c.xor_(R32(dst), R32(dst));
                c.stc();
                c.L(lbl_done);
            } else {
                c.test(R64(tmp), R64(tmp));
                c.jz(lbl_zero);
                c.lea(R64(dst), Xbyak::util::ptr[R64(tmp) - 1]);
                c.and_(R64(dst), R64(tmp));
                c.jmp(lbl_done);
                c.L(lbl_zero);
                c.xor_(R32(dst), R32(dst));
                c.stc();
                c.L(lbl_done);
            }
        } else if (m == ZYDIS_MNEMONIC_BLSI) {
            if (bits == 32) {
                c.mov(R32(dst), R32(tmp));
                c.neg(R32(dst));
                c.and_(R32(dst), R32(tmp));
                c.jz(lbl_done);
                c.stc();
                c.L(lbl_done);
            } else {
                c.mov(R64(dst), R64(tmp));
                c.neg(R64(dst));
                c.and_(R64(dst), R64(tmp));
                c.jz(lbl_done);
                c.stc();
                c.L(lbl_done);
            }
        } else {
            if (bits == 32) {
                c.test(R32(tmp), R32(tmp));
                c.jz(lbl_zero);
                c.lea(R32(dst), Xbyak::util::ptr[R64(tmp) - 1]);
                c.xor_(R32(dst), R32(tmp));
                c.jmp(lbl_done);
                c.L(lbl_zero);
                c.mov(R32(dst), 0xFFFFFFFFu);
                c.or_(R32(dst), R32(dst));
                c.stc();
                c.L(lbl_done);
            } else {
                c.test(R64(tmp), R64(tmp));
                c.jz(lbl_zero);
                c.lea(R64(dst), Xbyak::util::ptr[R64(tmp) - 1]);
                c.xor_(R64(dst), R64(tmp));
                c.jmp(lbl_done);
                c.L(lbl_zero);
                c.mov(R64(dst), -1);
                c.or_(R64(dst), R64(dst));
                c.stc();
                c.L(lbl_done);
            }
        }

        if (addr_reg >= 0) c.mov(R64(addr_reg), Xbyak::util::qword[Xbyak::util::rsp + 8]);
        c.mov(R64(tmp), Xbyak::util::qword[Xbyak::util::rsp]);
        c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp + static_cast<int32_t>(frame)]);
        return true;
    }

    if (m == ZYDIS_MNEMONIC_BEXTR) {
        if (inst.operand_count_visible < 3 || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            ops[2].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return false;
        }
        const uint32_t bits = ops[0].size;
        if (bits != 32 && bits != 64) return false;
        if (!MemOperandSupported(ops[1])) return false;
        const int dst = ZydisRegToIdx(ops[0].reg.value);
        const int src2 = ZydisRegToIdx(ops[2].reg.value);
        if (dst < 0 || src2 < 0) return false;

        PreparedMem fast_pm{};
        if (has_bextr_const && PrepareMemOperand(inst, ops[1], orig_rip, 0, &fast_pm) && !fast_pm.uses_rip_or_abs) {
            const uint32_t start = bextr_const_ctrl & 0xFFu;
            const uint32_t len = (bextr_const_ctrl >> 8) & 0xFFu;
            if (start >= bits || len == 0) {
                c.xor_(R32(dst), R32(dst));
            } else {
                EmitLoadFromOp(c, dst, ops[1], fast_pm, -1, bits);
                if (bits == 32) {
                    if (start > 0) c.shr(R32(dst), static_cast<uint8_t>(start));
                    if (len < 32) c.and_(R32(dst), (1u << len) - 1u);
                    else c.test(R32(dst), R32(dst));
                } else {
                    if (start > 0) c.shr(R64(dst), static_cast<uint8_t>(start));
                    if (len < 31) {
                        c.and_(R64(dst), static_cast<uint32_t>((1u << len) - 1u));
                    } else if (len < 64) {
                        const uint8_t trim = static_cast<uint8_t>(64u - len);
                        c.shl(R64(dst), trim);
                        c.shr(R64(dst), trim);
                        c.test(R64(dst), R64(dst));
                    } else {
                        c.test(R64(dst), R64(dst));
                    }
                }
            }
            ++g_ivy.bextr_const_fast;
            return true;
        }

        uint16_t alloc = used | (1u << 1);
        const int tmp0 = PickScratch(alloc);
        const int tmp1 = PickScratch(alloc);
        if (tmp0 < 0 || tmp1 < 0) return false;

        const int64_t frame = kRedZone + 24;
        PreparedMem pm{};
        if (!PrepareMemOperand(inst, ops[1], orig_rip, frame, &pm)) return false;

        c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp - static_cast<int32_t>(frame)]);
        c.mov(Xbyak::util::qword[Xbyak::util::rsp + 0], Xbyak::util::rcx);
        c.mov(Xbyak::util::qword[Xbyak::util::rsp + 8], R64(tmp0));
        c.mov(Xbyak::util::qword[Xbyak::util::rsp + 16], R64(tmp1));

        EmitLoadFromOp(c, tmp0, ops[1], pm, tmp1, bits);
        c.mov(R32(tmp1), R32(src2));

        Xbyak::Label lbl_zero, lbl_store;
        c.movzx(Xbyak::util::ecx, R16(tmp1));
        c.and_(Xbyak::util::ecx, 0xFF);
        c.cmp(Xbyak::util::ecx, bits);
        c.jae(lbl_zero);
        if (bits == 32) c.shr(R32(tmp0), Xbyak::util::cl);
        else c.shr(R64(tmp0), Xbyak::util::cl);

        c.shr(R32(tmp1), 8);
        c.and_(R32(tmp1), 0xFF);
        c.test(R32(tmp1), R32(tmp1));
        c.jz(lbl_zero);
        c.cmp(R32(tmp1), bits);
        c.jae(lbl_store);

        c.mov(Xbyak::util::ecx, R32(tmp1));
        if (bits == 32) {
            c.mov(R32(tmp1), 1);
            c.shl(R32(tmp1), Xbyak::util::cl);
            c.dec(R32(tmp1));
            c.and_(R32(tmp0), R32(tmp1));
        } else {
            c.mov(R64(tmp1), 1);
            c.shl(R64(tmp1), Xbyak::util::cl);
            c.dec(R64(tmp1));
            c.and_(R64(tmp0), R64(tmp1));
        }
        c.jmp(lbl_store);

        c.L(lbl_zero);
        c.xor_(R32(tmp0), R32(tmp0));

        c.L(lbl_store);
        c.mov(Xbyak::util::rcx, Xbyak::util::qword[Xbyak::util::rsp + 0]);
        if (bits == 32) {
            c.mov(R32(dst), R32(tmp0));
            c.test(R32(dst), R32(dst));
        } else {
            c.mov(R64(dst), R64(tmp0));
            c.test(R64(dst), R64(dst));
        }
        c.mov(R64(tmp0), Xbyak::util::qword[Xbyak::util::rsp + 8]);
        c.mov(R64(tmp1), Xbyak::util::qword[Xbyak::util::rsp + 16]);
        c.lea(Xbyak::util::rsp, Xbyak::util::ptr[Xbyak::util::rsp + static_cast<int32_t>(frame)]);
        return true;
    }

    return false;
}

// Emits a trampoline into `g_ivy.arena` for a contiguous run of 1..N lowered guest instructions
// (plus optional `tail` fusion for widening a single 4-byte instruction).
uintptr_t EmitTrampolineRun(const LowerOpDesc* run, size_t run_count, size_t run_bytes,
                            const FuseTail& tail = {}) {
    if (!run || run_count == 0) return 0;
    const size_t max_buf = 512 * run_count;
    if (!g_ivy.arena || g_ivy.arena_size - g_ivy.arena_used < max_buf) return 0;

    const size_t aligned = (g_ivy.arena_used + 15u) & ~size_t(15u);
    if (aligned + max_buf > g_ivy.arena_size) return 0;

    uint8_t* stub_ptr = g_ivy.arena + aligned;
    const uintptr_t stub_addr = reinterpret_cast<uintptr_t>(stub_ptr);
    const uintptr_t first_rip = run[0].orig_rip;
    const uintptr_t ret_addr = first_rip + run_bytes + tail.orig_extra_len;
    if (!FitsRel32(first_rip + 5, stub_addr)) return 0;

    try {
        Xbyak::CodeGenerator c(max_buf, stub_ptr);
        for (size_t i = 0; i < run_count; ++i) {
            if (!EmitLoweredOp(c, run[i])) return 0;
        }

        if (tail.kind == FuseTailKind::RawBytes) {
            for (size_t i = 0; i < tail.orig_extra_len; ++i) c.db(tail.raw[i]);
            if (!FitsRel32(reinterpret_cast<uintptr_t>(c.getCurr()) + 5, ret_addr)) return 0;
            EmitJmpRel32(c, ret_addr);
        } else if (tail.kind == FuseTailKind::Ret) {
            c.ret();
        } else if (tail.kind == FuseTailKind::JmpRel) {
            if (!FitsRel32(reinterpret_cast<uintptr_t>(c.getCurr()) + 5, tail.branch_target)) return 0;
            EmitJmpRel32(c, tail.branch_target);
        } else if (tail.kind == FuseTailKind::JccRel) {
            if (!FitsRel32(reinterpret_cast<uintptr_t>(c.getCurr()) + 6, tail.branch_target)) return 0;
            EmitJccRel32(c, tail.jcc_cc, tail.branch_target);
            if (!FitsRel32(reinterpret_cast<uintptr_t>(c.getCurr()) + 5, ret_addr)) return 0;
            EmitJmpRel32(c, ret_addr);
        } else {
            if (!FitsRel32(reinterpret_cast<uintptr_t>(c.getCurr()) + 5, ret_addr)) return 0;
            EmitJmpRel32(c, ret_addr);
        }
        c.ready();

        const size_t emitted = c.getSize();
        if (emitted == 0 || aligned + emitted > g_ivy.arena_size) return 0;
        g_ivy.arena_used = aligned + emitted;
        return stub_addr;
    } catch (...) {
        return 0;
    }
}

uintptr_t EmitTrampoline(const ZydisDecodedInstruction& inst, const ZydisDecodedOperand* ops,
                         uintptr_t orig_rip, const FuseTail& tail = {},
                         bool has_bextr_const = false, uint32_t bextr_const_ctrl = 0) {
    LowerOpDesc desc{&inst, ops, orig_rip, has_bextr_const, bextr_const_ctrl};
    return EmitTrampolineRun(&desc, 1, inst.length, tail);
}

// ============================================================================
// Trampoline Arena Allocation within +-2 GiB of Guest Image
// ============================================================================

uint8_t* AllocNearImage(uintptr_t image_base, uint64_t image_size, size_t want_size) {
    const uintptr_t gran = 65536;
    const uintptr_t max_dist = 0x70000000ull; // stay well inside +-2 GiB

    auto try_range = [&](uintptr_t lo, uintptr_t hi) -> uint8_t* {
        uintptr_t cur = (lo + gran - 1) & ~(gran - 1);
        while (cur + want_size <= hi) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_FREE) {
                const uintptr_t reg_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                const uintptr_t aligned = (cur + gran - 1) & ~(gran - 1);
                if (aligned + want_size <= reg_end && aligned + want_size <= hi) {
                    void* p = VirtualAlloc(reinterpret_cast<void*>(aligned), want_size,
                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                    if (p) return static_cast<uint8_t*>(p);
                }
            }
            const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (next <= cur) break;
            cur = (next + gran - 1) & ~(gran - 1);
        }
        return nullptr;
    };

    // Prefer right after the guest image (keeps everything below 1 TiB too).
    const uintptr_t after = image_base + image_size;
    if (after < UINTPTR_MAX - max_dist) {
        if (uint8_t* p = try_range(after, after + max_dist)) return p;
    }
    // Otherwise search below the guest image.
    const uintptr_t min_addr = 0x10000000ull;
    const uintptr_t lo = (image_base > max_dist + min_addr) ? (image_base - max_dist) : min_addr;
    if (image_base > lo + want_size) {
        if (uint8_t* p = try_range(lo, image_base)) return p;
    }
    return nullptr;
}

// ============================================================================
// CFG-Guided Disassembly & Rewriting
// ============================================================================

struct ExecRange {
    uint64_t start;
    uint64_t end;
};

bool InExecRanges(const std::vector<ExecRange>& ranges, uint64_t off) {
    for (const auto& r : ranges) {
        if (off >= r.start && off < r.end) return true;
    }
    return false;
}

bool IsPrivilegedOrInvalid(const ZydisDecodedInstruction& inst) {
    switch (inst.meta.category) {
    case ZYDIS_CATEGORY_SYSTEM:
    case ZYDIS_CATEGORY_IO:
    case ZYDIS_CATEGORY_IOSTRINGOP:
    case ZYDIS_CATEGORY_INTERRUPT:
    case ZYDIS_CATEGORY_SEGOP:
        return true;
    default:
        break;
    }
    switch (inst.mnemonic) {
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_CLI:
    case ZYDIS_MNEMONIC_STI:
    case ZYDIS_MNEMONIC_IN:
    case ZYDIS_MNEMONIC_OUT:
    case ZYDIS_MNEMONIC_INSB:
    case ZYDIS_MNEMONIC_INSW:
    case ZYDIS_MNEMONIC_INSD:
    case ZYDIS_MNEMONIC_OUTSB:
    case ZYDIS_MNEMONIC_OUTSW:
    case ZYDIS_MNEMONIC_OUTSD:
    case ZYDIS_MNEMONIC_ARPL:
    case ZYDIS_MNEMONIC_BOUND:
    case ZYDIS_MNEMONIC_INTO:
    case ZYDIS_MNEMONIC_IRET:
    case ZYDIS_MNEMONIC_IRETD:
    case ZYDIS_MNEMONIC_IRETQ:
        return true;
    default:
        return false;
    }
}

// Validates that `off` begins a plausible x86-64 basic block (used only for speculative LEA targets).
bool ValidateCodeTarget(const ZydisDecoder& dec, const unsigned char* image,
                        const std::vector<ExecRange>& ranges, uint64_t off) {
    if (!InExecRanges(ranges, off)) return false;
    uint64_t cur = off;
    ZydisDecodedInstruction inst{};
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
    for (int steps = 0; steps < 6; ++steps) {
        if (!InExecRanges(ranges, cur)) return false;
        const size_t avail = std::min<size_t>(15, static_cast<size_t>(ranges.back().end - cur));
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, image + cur, avail, &inst, ops))) return false;
        if (IsPrivilegedOrInvalid(inst)) return false;
        if (inst.mnemonic == ZYDIS_MNEMONIC_INT3 || inst.mnemonic == ZYDIS_MNEMONIC_UD2) return false;
        if (inst.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE || inst.mnemonic == ZYDIS_MNEMONIC_RET) {
            return steps >= 1; // short valid thunk/stub
        }
        cur += inst.length;
    }
    return true;
}

// Automatically locates PS4/ELF `.eh_frame_hdr` tables embedded inside executable segments
// (signature `01 1B 03 3B`: version=1, eh_ptr=sdata4|pcrel, fde_cnt=udata4, tbl=sdata4|datarel)
// and extracts every FDE function entry point. Guarantees 100% function coverage without
// changing the boot-linked.bin format and without ever scanning .rodata!
void DiscoverEhFrameRoots(const unsigned char* image, uint64_t image_size,
                          const std::vector<ExecRange>& exec_ranges,
                          std::vector<uint64_t>* out_roots) {
    auto rd_u32 = [&](uint64_t off) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, image + off, 4);
        return v;
    };
    auto rd_i32 = [&](uint64_t off) -> int32_t {
        int32_t v = 0;
        std::memcpy(&v, image + off, 4);
        return v;
    };

    for (const auto& r : exec_ranges) {
        if (r.end - r.start < 32) continue;
        for (uint64_t off = (r.start + 3u) & ~uint64_t(3u); off + 20 <= r.end; off += 4) {
            if (image[off] != 0x01 || image[off + 1] != 0x1B ||
                image[off + 2] != 0x03 || image[off + 3] != 0x3B) {
                continue;
            }
            const int32_t eh_rel = rd_i32(off + 4);
            const int64_t eh_ptr = static_cast<int64_t>(off + 4) + eh_rel;
            const uint32_t fde_count = rd_u32(off + 8);
            if (fde_count < 8 || fde_count > 300000) continue;
            if (eh_ptr < static_cast<int64_t>(r.start) || eh_ptr >= static_cast<int64_t>(r.end)) continue;
            const uint64_t tbl_start = off + 12;
            const uint64_t tbl_end = tbl_start + static_cast<uint64_t>(fde_count) * 8;
            if (tbl_end > r.end) continue;

            // Validate monotonicity and range of sample entries across the sorted search table.
            bool ok = true;
            int64_t prev_loc = -1;
            const uint32_t check_n = std::min<uint32_t>(fde_count, 16);
            for (uint32_t k = 0; k < check_n; ++k) {
                const uint32_t idx = (k < 8) ? k : (fde_count - (check_n - k));
                const int64_t loc = static_cast<int64_t>(off) + rd_i32(tbl_start + uint64_t(idx) * 8);
                const int64_t fde = static_cast<int64_t>(off) + rd_i32(tbl_start + uint64_t(idx) * 8 + 4);
                if (loc < static_cast<int64_t>(r.start) || loc >= static_cast<int64_t>(off) ||
                    fde < eh_ptr || fde >= static_cast<int64_t>(r.end) ||
                    (k < 8 && loc <= prev_loc)) {
                    ok = false;
                    break;
                }
                if (k < 8) prev_loc = loc;
            }
            if (!ok) continue;

            for (uint32_t i = 0; i < fde_count; ++i) {
                const int64_t loc = static_cast<int64_t>(off) + rd_i32(tbl_start + uint64_t(i) * 8);
                if (loc >= static_cast<int64_t>(r.start) && loc < static_cast<int64_t>(r.end)) {
                    out_roots->push_back(static_cast<uint64_t>(loc));
                }
            }
            // Jump past the validated table.
            off = (tbl_end & ~uint64_t(3u));
        }
    }
    (void)image_size;
}

bool SafeToFuseFollowup(const ZydisDecodedInstruction& next, const ZydisDecodedOperand* next_ops,
                        bool need_bmi1, bool need_movbe, bool need_lzcnt) {
    if (next.length == 0 || next.length > 15) return false;
    if (IsPrivilegedOrInvalid(next)) return false;
    if (next.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) return false;
    if (next.mnemonic == ZYDIS_MNEMONIC_RET || next.mnemonic == ZYDIS_MNEMONIC_CALL ||
        next.mnemonic == ZYDIS_MNEMONIC_JMP || next.mnemonic == ZYDIS_MNEMONIC_UD2 ||
        next.mnemonic == ZYDIS_MNEMONIC_INT3 || next.mnemonic == ZYDIS_MNEMONIC_SYSCALL) {
        return false;
    }
    // Never fuse another unsupported instruction or SSE4a instruction.
    if (ClassifyMnemonic(next.mnemonic, need_bmi1, need_movbe, need_lzcnt) >= 0) return false;
    if (next.meta.isa_set == ZYDIS_ISA_SET_SSE4A) return false;
    // Do not fuse instructions with RIP-relative operands (raw bytes would compute wrong EA in arena).
    for (uint8_t i = 0; i < next.operand_count; ++i) {
        if (next_ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY && next_ops[i].mem.base == ZYDIS_REGISTER_RIP) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// VEH #UD Emulation
// ============================================================================

bool EmulateInstruction(CONTEXT* c, const ZydisDecodedInstruction& inst,
                        const ZydisDecodedOperand* ops, uintptr_t rip) {
    const auto m = inst.mnemonic;
    const int slot = ClassifyMnemonic(m, true, true, true);
    if (slot < 0) return false;

    if (m == ZYDIS_MNEMONIC_LZCNT || m == ZYDIS_MNEMONIC_TZCNT) {
        if (inst.operand_count_visible < 2 || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
        const uint32_t bits = ops[0].size;
        uint64_t src = 0;
        if (!ReadOperandValue(c, inst, ops[1], rip, bits, &src)) return false;
        const uint32_t res = (m == ZYDIS_MNEMONIC_LZCNT) ? CountLz(src, bits) : CountTz(src, bits);
        WriteRegValue(c, ops[0].reg.value, bits, res);
        uint64_t fl = c->EFlags & ~(kEflagsCf | kEflagsZf);
        if (src == 0) fl |= kEflagsCf;
        if (res == 0) fl |= kEflagsZf;
        c->EFlags = static_cast<DWORD>(fl);
    } else if (m == ZYDIS_MNEMONIC_MOVBE) {
        if (inst.operand_count_visible < 2) return false;
        const uint32_t bits = ops[0].size;
        if (bits != 16 && bits != 32 && bits != 64) return false;
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint64_t v = 0;
            if (!ReadOperandValue(c, inst, ops[1], rip, bits, &v)) return false;
            WriteRegValue(c, ops[0].reg.value, bits, ByteSwap(v, bits));
        } else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint64_t ea = 0;
            if (!CalcMemEa(c, inst, ops[0], rip, &ea)) return false;
            const uint64_t swapped = ByteSwap(ReadRegValue(c, ops[1].reg.value, bits), bits);
            std::memcpy(reinterpret_cast<void*>(ea), &swapped, bits / 8);
        } else {
            return false;
        }
        // MOVBE does not modify EFLAGS.
    } else if (m == ZYDIS_MNEMONIC_ANDN) {
        if (inst.operand_count_visible < 3 || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return false;
        }
        const uint32_t bits = ops[0].size;
        const uint64_t mask = WidthMask(bits);
        const uint64_t s1 = ReadRegValue(c, ops[1].reg.value, bits);
        uint64_t s2 = 0;
        if (!ReadOperandValue(c, inst, ops[2], rip, bits, &s2)) return false;
        const uint64_t res = ((~s1) & s2) & mask;
        WriteRegValue(c, ops[0].reg.value, bits, res);
        uint64_t fl = c->EFlags & ~(kEflagsCf | kEflagsOf | kEflagsZf | kEflagsSf);
        if (res == 0) fl |= kEflagsZf;
        if ((res >> (bits - 1)) & 1u) fl |= kEflagsSf;
        c->EFlags = static_cast<DWORD>(fl);
    } else if (m == ZYDIS_MNEMONIC_BLSR || m == ZYDIS_MNEMONIC_BLSI || m == ZYDIS_MNEMONIC_BLSMSK) {
        if (inst.operand_count_visible < 2 || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
        const uint32_t bits = ops[0].size;
        const uint64_t mask = WidthMask(bits);
        uint64_t src = 0;
        if (!ReadOperandValue(c, inst, ops[1], rip, bits, &src)) return false;
        src &= mask;
        uint64_t res = 0;
        bool cf = false;
        if (m == ZYDIS_MNEMONIC_BLSR) {
            res = (src & (src - 1)) & mask;
            cf = (src == 0);
        } else if (m == ZYDIS_MNEMONIC_BLSI) {
            res = (src & (0 - src)) & mask;
            cf = (src != 0);
        } else {
            res = (src ^ (src - 1)) & mask;
            cf = (src == 0);
        }
        WriteRegValue(c, ops[0].reg.value, bits, res);
        uint64_t fl = c->EFlags & ~(kEflagsCf | kEflagsOf | kEflagsZf | kEflagsSf);
        if (cf) fl |= kEflagsCf;
        if (res == 0) fl |= kEflagsZf;
        if ((res >> (bits - 1)) & 1u) fl |= kEflagsSf;
        c->EFlags = static_cast<DWORD>(fl);
    } else if (m == ZYDIS_MNEMONIC_BEXTR) {
        if (inst.operand_count_visible < 3 || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            ops[2].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return false;
        }
        const uint32_t bits = ops[0].size;
        const uint64_t mask = WidthMask(bits);
        uint64_t src1 = 0;
        if (!ReadOperandValue(c, inst, ops[1], rip, bits, &src1)) return false;
        src1 &= mask;
        const uint64_t ctrl = ReadRegValue(c, ops[2].reg.value, bits);
        const uint32_t start = static_cast<uint32_t>(ctrl & 0xFFu);
        const uint32_t len = static_cast<uint32_t>((ctrl >> 8) & 0xFFu);
        uint64_t res = 0;
        if (start < bits && len > 0) {
            const uint64_t shifted = src1 >> start;
            if (len >= bits) res = shifted & mask;
            else res = shifted & ((UINT64_C(1) << len) - 1);
        }
        WriteRegValue(c, ops[0].reg.value, bits, res);
        uint64_t fl = c->EFlags & ~(kEflagsCf | kEflagsOf | kEflagsZf);
        if (res == 0) fl |= kEflagsZf;
        c->EFlags = static_cast<DWORD>(fl);
    } else {
        return false;
    }

    c->Rip += inst.length;
    g_ivy.veh_hits[slot].fetch_add(1, std::memory_order_relaxed);
    g_ivy.veh_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace

extern "C" {

uint64_t bbgpu_ivy_rewrite_guest(unsigned char* image, uint64_t image_size,
                                 const BbIvySegment* segments, uint64_t segment_count,
                                 const uint64_t* roots, uint64_t root_count) {
    const char* env_enable = std::getenv("BB_IVY_ISA");
    if (env_enable && std::strcmp(env_enable, "0") == 0) return 0;

    bool need_bmi1 = false, need_movbe = false, need_lzcnt = false;
    DetectFeatures(&need_bmi1, &need_movbe, &need_lzcnt);

    const bool forced = env_enable && std::strcmp(env_enable, "1") == 0;
    if (forced) {
        need_bmi1 = need_movbe = need_lzcnt = true;
    }
    if (!need_bmi1 && !need_movbe && !need_lzcnt) return 0;

    const char* env_stats = std::getenv("BB_IVY_STATS");
    g_ivy.stats_enabled = env_stats && std::strcmp(env_stats, "0") != 0;
    g_ivy.active = true;
    g_ivy.need_bmi1 = need_bmi1;
    g_ivy.need_movbe = need_movbe;
    g_ivy.need_lzcnt = need_lzcnt;
    g_ivy.image_base = reinterpret_cast<uintptr_t>(image);
    g_ivy.image_size = image_size;
    g_ivy.trapped_sites.clear();
    g_ivy.bextr_const_fast = 0;
    std::memset(g_ivy.rewritten_trampoline, 0, sizeof(g_ivy.rewritten_trampoline));
    std::memset(g_ivy.rewritten_ud2, 0, sizeof(g_ivy.rewritten_ud2));

    ZydisDecoderInit(&g_ivy.decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    g_ivy.decoder_ready = true;

    const char* env_rewrite = std::getenv("BB_IVY_REWRITE");
    const bool rewrite_ud_ops = !(env_rewrite && std::strcmp(env_rewrite, "0") == 0);
    const bool rewrite_none = env_rewrite && std::strcmp(env_rewrite, "none") == 0;

    std::vector<ExecRange> exec_ranges;
    for (uint64_t s = 0; s < segment_count; ++s) {
        if ((segments[s].flags & 1u) && segments[s].size > 0 && segments[s].address < image_size) {
            const uint64_t end = std::min<uint64_t>(segments[s].address + segments[s].size, image_size);
            exec_ranges.push_back({segments[s].address, end});
        }
    }
    if (exec_ranges.empty()) return 0;
    std::sort(exec_ranges.begin(), exec_ranges.end(),
              [](const ExecRange& a, const ExecRange& b) { return a.start < b.start; });

    if (!rewrite_none) {
        g_ivy.arena = AllocNearImage(g_ivy.image_base, image_size, kArenaSize);
        if (g_ivy.arena) {
            g_ivy.arena_size = kArenaSize;
            g_ivy.arena_used = 0;
        } else {
            std::fprintf(stderr, "IvyISA: warning: could not allocate +-2 GiB trampoline arena; falling back to UD2/VEH\n");
        }
    }

    // CFG-guided recursive disassembly: guarantees we never corrupt .rodata / .eh_frame!
    std::vector<uint8_t> visited((image_size + 7) / 8, 0);
    std::vector<uint8_t> branch_targets((image_size + 7) / 8, 0);
    auto is_visited = [&](uint64_t off) -> bool { return (visited[off >> 3] & (1u << (off & 7))) != 0; };
    auto mark_visited = [&](uint64_t off) { visited[off >> 3] |= static_cast<uint8_t>(1u << (off & 7)); };
    auto is_branch_target = [&](uint64_t off) -> bool {
        return off < image_size && (branch_targets[off >> 3] & (1u << (off & 7))) != 0;
    };
    auto mark_branch_target = [&](uint64_t off) {
        if (off < image_size) branch_targets[off >> 3] |= static_cast<uint8_t>(1u << (off & 7));
    };

    std::deque<uint64_t> queue;
    auto enqueue = [&](uint64_t off) {
        if (off < image_size && InExecRanges(exec_ranges, off)) {
            mark_branch_target(off);
            if (!is_visited(off)) {
                mark_visited(off);
                queue.push_back(off);
            }
        }
    };

    for (uint64_t i = 0; i < root_count; ++i) {
        enqueue(roots[i]);
    }
    std::vector<uint64_t> eh_roots;
    DiscoverEhFrameRoots(image, image_size, exec_ranges, &eh_roots);
    for (uint64_t r : eh_roots) {
        enqueue(r);
    }

    struct CandidateSite {
        uint64_t off;
        uint8_t len;
        int slot;
        bool has_bextr_const;
        uint32_t bextr_const_ctrl;
        uint64_t bextr_const_def_off;
        ZydisDecodedInstruction inst;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    };
    std::vector<CandidateSite> sites;
    std::vector<uint64_t> speculative_leas;

    // Helper: try discovering Clang PIE relative jump tables (int32_t entries relative to table_off).
    auto scan_jump_table = [&](uint64_t tbl_off, uint64_t func_hint) {
        if (tbl_off + 8 > image_size || (tbl_off & 3u) != 0) return;
        // Must have at least 2 valid consecutive entries pointing inside the same exec segment within +-4 MiB.
        std::vector<uint64_t> targets;
        for (size_t idx = 0; idx < 2048 && tbl_off + (idx + 1) * 4 <= image_size; ++idx) {
            int32_t rel = 0;
            std::memcpy(&rel, image + tbl_off + idx * 4, 4);
            const int64_t target = static_cast<int64_t>(tbl_off) + static_cast<int64_t>(rel);
            if (target < 0 || static_cast<uint64_t>(target) >= image_size) break;
            const uint64_t u = static_cast<uint64_t>(target);
            if (!InExecRanges(exec_ranges, u)) break;
            const int64_t dist = static_cast<int64_t>(u) - static_cast<int64_t>(func_hint);
            if (dist < -0x400000LL || dist > 0x400000LL) break;
            targets.push_back(u);
        }
        if (targets.size() >= 2) {
            for (uint64_t t : targets) {
                if (!is_visited(t) && ValidateCodeTarget(g_ivy.decoder, image, exec_ranges, t)) {
                    enqueue(t);
                }
            }
        }
    };

    auto drain_queue = [&]() {
        while (!queue.empty()) {
            uint64_t pc = queue.front();
            queue.pop_front();

            // Remember recent RIP-relative LEA addresses in this basic block for jump-table detection.
            uint64_t recent_lea[4] = {};
            int recent_lea_n = 0;
            // Track intra-basic-block `mov r32/r64, imm` for constant-control BEXTR specialization.
            uint16_t const_known = 0;
            uint32_t const_val[16] = {};
            uint64_t const_def_off[16] = {};

            for (;;) {
                if (!InExecRanges(exec_ranges, pc)) break;
                if (is_branch_target(pc)) const_known = 0;
                const size_t avail = std::min<size_t>(15, static_cast<size_t>(image_size - pc));
                ZydisDecodedInstruction inst{};
                ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&g_ivy.decoder, image + pc, avail, &inst, ops))) break;
                if (IsPrivilegedOrInvalid(inst)) break;

                const int slot = ClassifyMnemonic(inst.mnemonic, need_bmi1, need_movbe, need_lzcnt);
                if (slot >= 0) {
                    const bool is_silent = IsSilentWithoutUd(inst.mnemonic);
                    if (!rewrite_none && (is_silent || rewrite_ud_ops)) {
                        CandidateSite s{};
                        s.off = pc;
                        s.len = inst.length;
                        s.slot = slot;
                        if (inst.mnemonic == ZYDIS_MNEMONIC_BEXTR && inst.operand_count_visible >= 3 &&
                            ops[2].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                            const int r2 = ZydisRegToIdx(ops[2].reg.value);
                            if (r2 >= 0 && ((const_known & (1u << r2)) != 0)) {
                                s.has_bextr_const = true;
                                s.bextr_const_ctrl = const_val[r2];
                                s.bextr_const_def_off = const_def_off[r2];
                            }
                        }
                        s.inst = inst;
                        std::memcpy(s.ops, ops, sizeof(ops));
                        sites.push_back(s);
                    }
                }

                // Update intra-block constant register tracking.
                if (inst.mnemonic == ZYDIS_MNEMONIC_CALL || inst.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
                    const_known = 0;
                } else if (inst.mnemonic == ZYDIS_MNEMONIC_MOV && inst.operand_count_visible >= 2 &&
                           ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                           (ops[0].size == 32 || ops[0].size == 64)) {
                    const int r = ZydisRegToIdx(ops[0].reg.value);
                    if (r >= 0) {
                        const_val[r] = static_cast<uint32_t>(ops[1].imm.value.u);
                        const_def_off[r] = pc;
                        const_known |= static_cast<uint16_t>(1u << r);
                    }
                } else {
                    for (uint8_t oi = 0; oi < inst.operand_count; ++oi) {
                        if (ops[oi].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                            (ops[oi].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
                            const int r = ZydisRegToIdx(ops[oi].reg.value);
                            if (r >= 0) const_known &= static_cast<uint16_t>(~(1u << r));
                        }
                    }
                }

                // Track RIP-relative LEAs (function pointers or switch jump tables).
                if (inst.mnemonic == ZYDIS_MNEMONIC_LEA && inst.operand_count_visible >= 2 &&
                    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[1].mem.base == ZYDIS_REGISTER_RIP &&
                    ops[1].mem.disp.has_displacement) {
                    const int64_t target = static_cast<int64_t>(pc + inst.length) + ops[1].mem.disp.value;
                    if (target >= 0 && static_cast<uint64_t>(target) < image_size) {
                        const uint64_t u = static_cast<uint64_t>(target);
                        if (recent_lea_n < 4) recent_lea[recent_lea_n++] = u;
                        else {
                            std::memmove(recent_lea, recent_lea + 1, 3 * sizeof(uint64_t));
                            recent_lea[3] = u;
                        }
                        if (InExecRanges(exec_ranges, u) && !is_visited(u)) {
                            speculative_leas.push_back(u);
                        }
                    }
                }

                // Follow direct branches and calls.
                if (inst.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE && inst.operand_count_visible >= 1 &&
                    ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[0].imm.is_relative) {
                    ZyanU64 target = 0;
                    if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&inst, &ops[0], pc, &target))) {
                        enqueue(target);
                    }
                }

                // Indirect JMP: check recent LEAs as potential relative switch jump tables.
                if (inst.mnemonic == ZYDIS_MNEMONIC_JMP && inst.operand_count_visible >= 1 &&
                    ops[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    for (int k = 0; k < recent_lea_n; ++k) {
                        scan_jump_table(recent_lea[k], pc);
                    }
                }

                // Stop linear basic-block decode at unconditional terminators.
                if (inst.mnemonic == ZYDIS_MNEMONIC_RET || inst.mnemonic == ZYDIS_MNEMONIC_JMP ||
                    inst.mnemonic == ZYDIS_MNEMONIC_UD2 || inst.mnemonic == ZYDIS_MNEMONIC_INT3) {
                    break;
                }

                const uint64_t next = pc + inst.length;
                if (next >= image_size || !InExecRanges(exec_ranges, next) || is_visited(next)) break;
                mark_visited(next);
                pc = next;
            }
        }
    };

    drain_queue();

    // Expand validated RIP-relative LEA targets that point to real functions in exec segments.
    for (size_t i = 0; i < speculative_leas.size(); ++i) {
        const uint64_t u = speculative_leas[i];
        if (!is_visited(u) && ValidateCodeTarget(g_ivy.decoder, image, exec_ranges, u)) {
            enqueue(u);
            drain_queue();
        }
    }

    // Deduplicate sites by offset (defensive).
    std::sort(sites.begin(), sites.end(),
              [](const CandidateSite& a, const CandidateSite& b) { return a.off < b.off; });
    sites.erase(std::unique(sites.begin(), sites.end(),
                            [](const CandidateSite& a, const CandidateSite& b) { return a.off == b.off; }),
                sites.end());

    auto no_branch_target_in = [&](uint64_t after_off, uint64_t through_off) -> bool {
        if (through_off <= after_off || through_off - after_off > 64) return false;
        for (uint64_t p = after_off + 1; p <= through_off; ++p) {
            if (is_branch_target(p)) return false;
        }
        return true;
    };

    uint64_t total_rewritten = 0;
    uint64_t fused_4b = 0;
    uint64_t fused_runs = 0;
    for (size_t idx = 0; idx < sites.size(); ++idx) {
        const auto& s = sites[idx];
        const uintptr_t orig_rip = g_ivy.image_base + s.off;
        uint8_t* at = image + s.off;

        // First: fuse any contiguous run of 2..8 back-to-back lowered instructions (e.g.
        // `tzcnt ebp, eax; tzcnt r14d, r14d` or 4-way SIMD `tzcnt` chains in eboot.bin!) into ONE
        // trampoline! Eliminates BOTH 4-byte UD2 traps AND redundant trampoline round-trips!
        if (g_ivy.arena && idx + 1 < sites.size() &&
            sites[idx + 1].off == s.off + s.len && !is_branch_target(s.off + s.len)) {
            LowerOpDesc run[8];
            size_t run_n = 0;
            size_t run_bytes = 0;
            uint64_t cur = s.off;
            while (idx + run_n < sites.size() && run_n < 8 && sites[idx + run_n].off == cur) {
                if (run_n > 0 && is_branch_target(cur)) break;
                const auto& si = sites[idx + run_n];
                const bool uc = si.has_bextr_const && no_branch_target_in(si.bextr_const_def_off, si.off);
                run[run_n] = {&si.inst, si.ops, g_ivy.image_base + si.off, uc, si.bextr_const_ctrl};
                cur += si.len;
                run_bytes += si.len;
                ++run_n;
            }
            if (run_n >= 2 && run_bytes >= 5) {
                const uintptr_t stub = EmitTrampolineRun(run, run_n, run_bytes);
                if (stub != 0) {
                    const int32_t rel =
                        static_cast<int32_t>(static_cast<int64_t>(stub) - static_cast<int64_t>(orig_rip + 5));
                    at[0] = 0xE9;
                    std::memcpy(at + 1, &rel, 4);
                    if (run_bytes > 5) std::memset(at + 5, 0x90, run_bytes - 5);
                    for (size_t k = 0; k < run_n; ++k) {
                        ++g_ivy.rewritten_trampoline[sites[idx + k].slot];
                        ++total_rewritten;
                    }
                    ++fused_runs;
                    idx += run_n - 1;
                    continue;
                }
            }
        }

        const bool use_bextr_const =
            s.has_bextr_const && no_branch_target_in(s.bextr_const_def_off, s.off);

        if (s.len >= 5 && g_ivy.arena) {
            const uintptr_t stub =
                EmitTrampoline(s.inst, s.ops, orig_rip, {}, use_bextr_const, s.bextr_const_ctrl);
            if (stub != 0) {
                const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(stub) - static_cast<int64_t>(orig_rip + 5));
                at[0] = 0xE9;
                std::memcpy(at + 1, &rel, 4);
                if (s.len > 5) std::memset(at + 5, 0x90, s.len - 5);
                ++g_ivy.rewritten_trampoline[s.slot];
                ++total_rewritten;
                continue;
            }
        }

        // 4-byte instruction (e.g., `lzcnt ecx, eax`, `tzcnt eax, eax`, `movbe [rsi], eax`):
        // Try safe 2-instruction window fusion when `orig + s.len` is NOT a branch target.
        // Supports plain non-branch/non-RIP-rel follow-ups, plus RET, JMP rel, and Jcc rel!
        if (s.len < 5 && g_ivy.arena && s.off + s.len < image_size && !is_branch_target(s.off + s.len)) {
            const uint64_t next_off = s.off + s.len;
            const size_t avail = std::min<size_t>(15, static_cast<size_t>(image_size - next_off));
            ZydisDecodedInstruction next_inst{};
            ZydisDecodedOperand next_ops[ZYDIS_MAX_OPERAND_COUNT]{};
            if (InExecRanges(exec_ranges, next_off) &&
                ZYAN_SUCCESS(ZydisDecoderDecodeFull(&g_ivy.decoder, image + next_off, avail, &next_inst, next_ops)) &&
                static_cast<size_t>(s.len) + next_inst.length >= 5) {
                FuseTail tail{};
                tail.orig_extra_len = next_inst.length;
                if (SafeToFuseFollowup(next_inst, next_ops, need_bmi1, need_movbe, need_lzcnt)) {
                    tail.kind = FuseTailKind::RawBytes;
                    tail.raw = image + next_off;
                } else if (next_inst.mnemonic == ZYDIS_MNEMONIC_RET && next_inst.length == 1 &&
                           image[next_off] == 0xC3) {
                    tail.kind = FuseTailKind::Ret;
                } else if (next_inst.mnemonic == ZYDIS_MNEMONIC_JMP && next_inst.operand_count_visible >= 1 &&
                           next_ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && next_ops[0].imm.is_relative) {
                    ZyanU64 target = 0;
                    if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&next_inst, &next_ops[0], g_ivy.image_base + next_off, &target))) {
                        tail.kind = FuseTailKind::JmpRel;
                        tail.branch_target = static_cast<uintptr_t>(target);
                    }
                } else if (next_inst.meta.category == ZYDIS_CATEGORY_COND_BR && next_inst.operand_count_visible >= 1 &&
                           next_ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && next_ops[0].imm.is_relative) {
                    // Only standard Jcc (70..7F rel8 or 0F 80..8F rel32; exclude JRCXZ/LOOP).
                    const uint8_t op0 = image[next_off];
                    bool is_jcc = false;
                    uint8_t cc = 0;
                    if (op0 >= 0x70 && op0 <= 0x7F) {
                        is_jcc = true;
                        cc = op0 & 0x0Fu;
                    } else if (op0 == 0x0F && next_inst.length >= 2 &&
                               image[next_off + 1] >= 0x80 && image[next_off + 1] <= 0x8F) {
                        is_jcc = true;
                        cc = image[next_off + 1] & 0x0Fu;
                    }
                    ZyanU64 target = 0;
                    if (is_jcc && ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&next_inst, &next_ops[0],
                                                                        g_ivy.image_base + next_off, &target))) {
                        tail.kind = FuseTailKind::JccRel;
                        tail.jcc_cc = cc;
                        tail.branch_target = static_cast<uintptr_t>(target);
                    }
                }

                if (tail.kind != FuseTailKind::None) {
                    const size_t total_len = static_cast<size_t>(s.len) + next_inst.length;
                    const uintptr_t stub = EmitTrampoline(s.inst, s.ops, orig_rip, tail);
                    if (stub != 0) {
                        const int32_t rel =
                            static_cast<int32_t>(static_cast<int64_t>(stub) - static_cast<int64_t>(orig_rip + 5));
                        at[0] = 0xE9;
                        std::memcpy(at + 1, &rel, 4);
                        if (total_len > 5) std::memset(at + 5, 0x90, total_len - 5);
                        ++g_ivy.rewritten_trampoline[s.slot];
                        ++total_rewritten;
                        ++fused_4b;
                        continue;
                    }
                }
            }
        }

        // Cannot fit a 5-byte E9 rel32 (e.g., 4-byte `lzcnt eax, eax` right before a branch/target):
        // For LZCNT/TZCNT we MUST patch to UD2 (0F 0B) so Ivy Bridge traps into VEH instead of
        // silently executing BSR/BSF! For #UD ops (MOVBE/VEX BMI1), leave original bytes for VEH.
        if (IsSilentWithoutUd(s.inst.mnemonic) && s.len >= 2) {
            TrappedSite ts{};
            ts.rip = orig_rip;
            ts.orig_len = s.len;
            std::memcpy(ts.orig_bytes, at, s.len);
            g_ivy.trapped_sites.push_back(ts);

            at[0] = 0x0F;
            at[1] = 0x0B; // UD2
            if (s.len > 2) std::memset(at + 2, 0x90, s.len - 2);
            ++g_ivy.rewritten_ud2[s.slot];
            ++total_rewritten;
        }
    }

    std::sort(g_ivy.trapped_sites.begin(), g_ivy.trapped_sites.end(),
              [](const TrappedSite& a, const TrappedSite& b) { return a.rip < b.rip; });

    if (g_ivy.arena && g_ivy.arena_used > 0) {
        const size_t protect_sz = (g_ivy.arena_used + 4095u) & ~size_t(4095u);
        DWORD old = 0;
        VirtualProtect(g_ivy.arena, protect_sz, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), g_ivy.arena, protect_sz);
    }

    uint64_t total_tramp = 0, total_ud2 = 0;
    for (size_t i = 0; i < kSlotCount; ++i) {
        total_tramp += g_ivy.rewritten_trampoline[i];
        total_ud2 += g_ivy.rewritten_ud2[i];
    }
    std::printf("IvyISA: active (missing:%s%s%s%s); eh_roots=%zu, %" PRIu64 " trampolines (%" PRIu64
                " fused-4B, %" PRIu64 " fused-runs, %" PRIu64 " const-BEXTR, %zu KB arena), %" PRIu64
                " UD2-trapped 4B\n",
                need_bmi1 ? " BMI1" : "", need_movbe ? " MOVBE" : "", need_lzcnt ? " LZCNT" : "",
                forced ? " [forced]" : "", eh_roots.size(), total_tramp, fused_4b, fused_runs,
                g_ivy.bextr_const_fast, (g_ivy.arena_used + 1023u) / 1024u, total_ud2);
    if (total_rewritten > 0 || g_ivy.stats_enabled) {
        std::printf("IvyISA: breakdown:");
        for (size_t i = 0; i < kSlotCount; ++i) {
            if (g_ivy.rewritten_trampoline[i] || g_ivy.rewritten_ud2[i]) {
                std::printf(" %s=%" PRIu64 "+%" PRIu64 "ud2", kSlotNames[i],
                            g_ivy.rewritten_trampoline[i], g_ivy.rewritten_ud2[i]);
            }
        }
        std::printf("\n");
    }
    return total_rewritten;
}

int bbgpu_ivy_handle_ud(void* exception_pointers) {
    if (!g_ivy.active || !g_ivy.decoder_ready || !exception_pointers) return 0;

    auto* info = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    CONTEXT* c = info->ContextRecord;
    const uintptr_t rip = static_cast<uintptr_t>(c->Rip);
    if (rip < g_ivy.image_base || rip - g_ivy.image_base >= g_ivy.image_size) return 0;

    const uint8_t* decode_ptr = reinterpret_cast<const uint8_t*>(rip);
    size_t decode_len = std::min<size_t>(15, static_cast<size_t>(g_ivy.image_size - (rip - g_ivy.image_base)));

    // Check if this is one of our UD2-trapped 4-byte LZCNT/TZCNT sites.
    TrappedSite* trapped = nullptr;
    if (!g_ivy.trapped_sites.empty() && decode_len >= 2 && decode_ptr[0] == 0x0F && decode_ptr[1] == 0x0B) {
        auto it = std::lower_bound(g_ivy.trapped_sites.begin(), g_ivy.trapped_sites.end(), rip,
                                   [](const TrappedSite& s, uintptr_t key) { return s.rip < key; });
        if (it != g_ivy.trapped_sites.end() && it->rip == rip) {
            decode_ptr = it->orig_bytes;
            decode_len = it->orig_len;
            trapped = &(*it);
        }
    }

    ZydisDecodedInstruction inst{};
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&g_ivy.decoder, decode_ptr, decode_len, &inst, ops))) {
        return 0;
    }

    if (!EmulateInstruction(c, inst, ops, rip)) return 0;
    if (trapped) __atomic_fetch_add(&trapped->hits, 1, __ATOMIC_RELAXED);
    return 1;
}

int bbgpu_ivy_in_trampoline(uintptr_t rip) {
    if (!g_ivy.arena || g_ivy.arena_used == 0) return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_ivy.arena);
    return (rip >= base && rip < base + g_ivy.arena_used) ? 1 : 0;
}

void bbgpu_ivy_report(void) {
    if (!g_ivy.active) return;
    const uint64_t total = g_ivy.veh_total.load(std::memory_order_relaxed);
    if (total == 0 && !g_ivy.stats_enabled) return;
    std::printf("IvyISA: VEH #UD emulated=%" PRIu64 " (", total);
    bool first = true;
    for (size_t i = 0; i < kSlotCount; ++i) {
        const uint64_t n = g_ivy.veh_hits[i].load(std::memory_order_relaxed);
        if (n > 0) {
            std::printf("%s%s=%" PRIu64, first ? "" : " ", kSlotNames[i], n);
            first = false;
        }
    }
    std::printf(")\n");
    for (const auto& ts : g_ivy.trapped_sites) {
        if (ts.hits > 0) {
            std::printf("IvyISA:   UD2 site +0x%" PRIx64 ": %" PRIu64 " hits\n",
                        static_cast<uint64_t>(ts.rip - g_ivy.image_base), ts.hits);
        }
    }
}

} // extern "C"

#else

extern "C" {
uint64_t bbgpu_ivy_rewrite_guest(unsigned char*, uint64_t, const BbIvySegment*, uint64_t,
                                 const uint64_t*, uint64_t) {
    return 0;
}
int bbgpu_ivy_handle_ud(void*) { return 0; }
int bbgpu_ivy_in_trampoline(uintptr_t) { return 0; }
void bbgpu_ivy_report(void) {}
}

#endif
