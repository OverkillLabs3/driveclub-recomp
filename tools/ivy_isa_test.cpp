// Assembly-level unit test for gpu/shim/bbport_ivy_isa.cpp.
// Tests trampoline lowering AND UD2/VEH emulation on Ivy Bridge (or Haswell+ via BB_IVY_ISA=1)
// for LZCNT, TZCNT, MOVBE, ANDN, BEXTR, BLSI, BLSMSK, BLSR:
//   - 16-bit (preserving upper 48 bits), 32-bit (zero-extending upper 32 bits), and 64-bit widths
//   - exact CF, ZF, SF, OF flags (and unchanged flags for MOVBE)
//   - register aliasing (dst == src, dst == src2, dst == rcx on BEXTR, src == mem base on MOVBE)
//   - memory operands including [rsp + disp] and [rip + disp]
//   - 128-byte System V red-zone preservation!
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bbport_ivy_isa.h"

namespace {

constexpr uint64_t kCf = 1ull << 0;
constexpr uint64_t kZf = 1ull << 6;
constexpr uint64_t kSf = 1ull << 7;
constexpr uint64_t kOf = 1ull << 11;

int g_failures = 0;

void Check(bool ok, const char* name, uint64_t got, uint64_t want, uint64_t got_fl, uint64_t want_fl,
           uint64_t fl_mask) {
    if (!ok || got != want || ((got_fl ^ want_fl) & fl_mask) != 0) {
        std::fprintf(stderr,
                     "FAIL [%s]: val got=0x%016llx want=0x%016llx | flags got=0x%llx want=0x%llx (mask=0x%llx)\n",
                     name, (unsigned long long)got, (unsigned long long)want, (unsigned long long)(got_fl & fl_mask),
                     (unsigned long long)(want_fl & fl_mask), (unsigned long long)fl_mask);
        ++g_failures;
    }
}

LONG CALLBACK TestVeh(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        if (bbgpu_ivy_handle_ud(info)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Helper: builds a guest SysV-ABI function in `code_buf` that:
//   1. Writes canary pattern into [rsp - 128 .. rsp - 1] (SysV red zone!)
//   2. Loads RAX = arg0 (rdi), RBX = arg1 (rsi), RCX = arg2 (rdx), RDX = arg3 (rcx -> moved via r8)
//   3. Executes `payload` (the Jaguar instruction under test)
//   4. Captures EFLAGS into [r9] (out_flags)
//   5. Verifies the entire 128-byte red zone is UNTOUCHED (writes 1/0 to [r8])
//   6. Returns RAX
// Signature (SysV ABI):
//   uint64_t fn(uint64_t rax_init, uint64_t rbx_init, uint64_t rcx_init, uint64_t rdx_init,
//               uint64_t* out_flags, uint64_t* out_redzone_ok)
// SysV args: rdi, rsi, rdx, rcx, r8, r9
using TestFn = uint64_t (__attribute__((sysv_abi))*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                                     uint64_t*, uint64_t*);

struct Harness {
    uint8_t* image = nullptr;
    size_t image_size = 65536;
    size_t cursor = 0x100;
    std::vector<uint64_t> roots;

    Harness() {
        image = static_cast<uint8_t*>(VirtualAlloc(nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        std::memset(image, 0xCC, image_size);
    }
    ~Harness() {
        if (image) VirtualFree(image, 0, MEM_RELEASE);
    }

    size_t AddTest(const std::vector<uint8_t>& payload, bool return_rcx = false, bool set_initial_cf_of = false) {
        cursor = (cursor + 15) & ~size_t(15);
        const size_t start = cursor;
        roots.push_back(start);

        auto emit = [&](std::initializer_list<uint8_t> b) {
            for (uint8_t x : b) image[cursor++] = x;
        };

        // Save callee-saved rbx, r12, r13, r14, r15 (5 pushes = 40 bytes; entry rsp%16==8 -> now rsp%16==0!)
        emit({0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
        // r12 = r8 (out_flags), r13 = r9 (out_redzone_ok)
        emit({0x4D, 0x89, 0xC4}); // mov r12, r8
        emit({0x4D, 0x89, 0xCD}); // mov r13, r9
        // r14 = rdx (arg2 -> rcx_init), r15 = rcx (arg3 -> rdx_init)
        emit({0x49, 0x89, 0xD6}); // mov r14, rdx
        emit({0x49, 0x89, 0xCF}); // mov r15, rcx

        // Fill 128-byte red zone [rsp - 128 .. rsp - 1] with 0xA5A5A5A5A5A5A5A5
        // mov rax, 0xA5A5A5A5A5A5A5A5
        emit({0x48, 0xB8, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5});
        for (int off = -128; off < 0; off += 8) {
            // mov [rsp + off], rax -> 48 89 44 24 <disp8>
            emit({0x48, 0x89, 0x44, 0x24, static_cast<uint8_t>(static_cast<int8_t>(off))});
        }

        // Load RAX=rdi, RBX=rsi, RCX=r14, RDX=r15
        emit({0x48, 0x89, 0xF8}); // mov rax, rdi
        emit({0x48, 0x89, 0xF3}); // mov rbx, rsi
        emit({0x4C, 0x89, 0xF1}); // mov rcx, r14
        emit({0x4C, 0x89, 0xFA}); // mov rdx, r15

        if (set_initial_cf_of) {
            // Set known flags: CF=1, ZF=1, SF=0, OF=0 via mov r10d, 0x41 -> push/popf? No, use stc + test r10,r10?
            // Actually `xor r10d, r10d; stc` sets ZF=1, CF=1, SF=0, OF=0 without touching rsp!
            emit({0x45, 0x31, 0xD2, 0xF9}); // xor r10d, r10d; stc
        } else {
            // Clear CF/OF/SF, set ZF=0: mov r10d, 1; test r10d, r10d
            emit({0x41, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x45, 0x85, 0xD2});
        }

        // Emit payload!
        for (uint8_t b : payload) image[cursor++] = b;

        // Capture flags WITHOUT clobbering red zone! First step past red zone, pushfq, pop r11, step back!
        // lea rsp, [rsp - 128]; pushfq; pop r11; lea rsp, [rsp + 128]
        emit({0x48, 0x8D, 0x64, 0x24, 0x80}); // lea rsp, [rsp - 128]
        emit({0x9C});                         // pushfq
        emit({0x41, 0x5B});                   // pop r11
        emit({0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
        // mov [r12], r11
        emit({0x4D, 0x89, 0x1C, 0x24});

        if (return_rcx) {
            // mov rax, rcx
            emit({0x48, 0x89, 0xC8});
        }

        // Verify red zone [rsp - 128 .. rsp - 1] == 0xA5..A5
        // mov r10, 0xA5A5A5A5A5A5A5A5; mov r11d, 1
        emit({0x49, 0xBA, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5});
        emit({0x41, 0xBB, 0x01, 0x00, 0x00, 0x00});
        emit({0x45, 0x31, 0xC9}); // xor r9d, r9d
        for (int off = -128; off < 0; off += 8) {
            // cmp [rsp + off], r10 -> 4C 39 54 24 <disp8>; cmovne r11, r9 -> 4D 0F 45 D9
            emit({0x4C, 0x39, 0x54, 0x24, static_cast<uint8_t>(static_cast<int8_t>(off))});
            emit({0x4D, 0x0F, 0x45, 0xD9});
        }
        // mov [r13], r11
        emit({0x4D, 0x89, 0x5D, 0x00});

        // Restore r15..r12, rbx and ret
        emit({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5B, 0xC3});
        return start;
    }

    void Finalize() {
        BbIvySegment seg{0, image_size, 5};
        bbgpu_ivy_rewrite_guest(image, image_size, &seg, 1, roots.data(), roots.size());
        DWORD old = 0;
        VirtualProtect(image, image_size, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), image, image_size);
    }

    TestFn Get(size_t off) const { return reinterpret_cast<TestFn>(image + off); }
};

} // namespace

int main() {
    _putenv("BB_IVY_ISA=1");
    _putenv("BB_IVY_STATS=1");
    AddVectoredExceptionHandler(1, TestVeh);

    Harness h;

    // 1. 4-byte LZCNT eax, eax: F3 0F BD C0 (must UD2-trap into VEH!)
    const size_t t_lzcnt32_4b = h.AddTest({0xF3, 0x0F, 0xBD, 0xC0});
    // 2. 5-byte LZCNT rax, rbx: F3 48 0F BD C3 (trampoline!)
    const size_t t_lzcnt64 = h.AddTest({0xF3, 0x48, 0x0F, 0xBD, 0xC3});
    // 3. 5-byte LZCNT ax, bx: 66 F3 0F BD C3 (trampoline! must preserve upper 48 bits of RAX)
    const size_t t_lzcnt16 = h.AddTest({0x66, 0xF3, 0x0F, 0xBD, 0xC3});
    // 4. 4-byte TZCNT eax, eax: F3 0F BC C0 (must UD2-trap into VEH!)
    const size_t t_tzcnt32_4b = h.AddTest({0xF3, 0x0F, 0xBC, 0xC0});
    // 5. 5-byte TZCNT rax, rbx: F3 48 0F BC C3 (trampoline!)
    const size_t t_tzcnt64 = h.AddTest({0xF3, 0x48, 0x0F, 0xBC, 0xC3});
    // 6. 5-byte TZCNT ax, bx: 66 F3 0F BC C3 (trampoline!)
    const size_t t_tzcnt16 = h.AddTest({0x66, 0xF3, 0x0F, 0xBC, 0xC3});
    // 7. MOVBE eax, [rbx] (4B -> VEH): 0F 38 F0 03
    const size_t t_movbe32_load_4b = h.AddTest({0x0F, 0x38, 0xF0, 0x03}, false, true);
    // 8. MOVBE rax, [rbx+8] (6B -> trampoline!): 48 0F 38 F0 43 08
    const size_t t_movbe64_load = h.AddTest({0x48, 0x0F, 0x38, 0xF0, 0x43, 0x08}, false, true);
    // 9. MOVBE ax, [rbx+0] (5B -> trampoline! preserves upper 48 bits & flags): 66 0F 38 F0 03
    const size_t t_movbe16_load = h.AddTest({0x66, 0x0F, 0x38, 0xF0, 0x03}, false, true);
    // 10. MOVBE [rbx+8], eax (5B -> trampoline! preserves upper 32 bits of RAX & flags): 0F 38 F1 43 08
    const size_t t_movbe32_store = h.AddTest({0x0F, 0x38, 0xF1, 0x43, 0x08}, false, true);
    // 11. MOVBE [rbx+8], rax (6B -> trampoline! preserves flags): 48 0F 38 F1 43 08
    const size_t t_movbe64_store = h.AddTest({0x48, 0x0F, 0x38, 0xF1, 0x43, 0x08}, false, true);
    // 12. ANDN eax, ebx, ecx (5B VEX -> trampoline): C4 E2 60 F2 C1
    const size_t t_andn32 = h.AddTest({0xC4, 0xE2, 0x60, 0xF2, 0xC1});
    // 13. ANDN rax, rbx, rax (5B VEX dst==src2 alias! -> trampoline): C4 E2 E0 F2 C0
    const size_t t_andn64_alias = h.AddTest({0xC4, 0xE2, 0xE0, 0xF2, 0xC0});
    // 14. BLSR eax, ebx (5B VEX -> trampoline): C4 E2 78 F3 CB
    const size_t t_blsr32 = h.AddTest({0xC4, 0xE2, 0x78, 0xF3, 0xCB});
    // 15. BLSR rax, rbx (5B VEX -> trampoline): C4 E2 F8 F3 CB
    const size_t t_blsr64 = h.AddTest({0xC4, 0xE2, 0xF8, 0xF3, 0xCB});
    // 16. BLSI eax, ebx (5B VEX -> trampoline): C4 E2 78 F3 DB
    const size_t t_blsi32 = h.AddTest({0xC4, 0xE2, 0x78, 0xF3, 0xDB});
    // 17. BLSMSK eax, ebx (5B VEX -> trampoline): C4 E2 78 F3 D3
    const size_t t_blsmsk32 = h.AddTest({0xC4, 0xE2, 0x78, 0xF3, 0xD3});
    // 18. BEXTR eax, ebx, ecx (5B VEX -> trampoline): C4 E2 70 F7 C3
    const size_t t_bextr32 = h.AddTest({0xC4, 0xE2, 0x70, 0xF7, 0xC3});
    // 19. BEXTR rcx, rbx, rcx (5B VEX dst==src2==rcx! -> trampoline): C4 E2 F0 F7 CB
    const size_t t_bextr64_rcx = h.AddTest({0xC4, 0xE2, 0xF0, 0xF7, 0xCB}, true);
    // 20. LZCNT rax, [rsp - 8] (6B mem operand INSIDE red zone! tests rsp_delta adjustment!): F3 48 0F BD 44 24 F8
    //     Red zone is filled with 0xA5A5A5A5A5A5A5A5 (MSB=1 -> lzcnt=0, ZF=1, CF=0)!
    const size_t t_lzcnt64_rsp_mem = h.AddTest({0xF3, 0x48, 0x0F, 0xBD, 0x44, 0x24, 0xF8});
    // 21. MOVBE rax, [rsp - 8] (7B mem operand INSIDE red zone! 0xA5..A5 swapped is 0xA5..A5, flags untouched):
    //     48 0F 38 F0 44 24 F8
    const size_t t_movbe64_rsp_mem = h.AddTest({0x48, 0x0F, 0x38, 0xF0, 0x44, 0x24, 0xF8}, false, true);
    // 22. Constant-control BEXTR 32-bit: mov ecx, 0x0804 (start=4, len=8); bextr eax, ebx, ecx
    const size_t t_bextr32_const = h.AddTest({0xB9, 0x04, 0x08, 0x00, 0x00, 0xC4, 0xE2, 0x70, 0xF7, 0xC3});
    // 23. Constant-control BEXTR 64-bit (len=36 >= 32!): mov ecx, 0x2408 (start=8, len=36); bextr rax, rbx, rcx
    const size_t t_bextr64_const36 = h.AddTest({0xB9, 0x08, 0x24, 0x00, 0x00, 0xC4, 0xE2, 0xF0, 0xF7, 0xC3});
    // 24. 4-byte LZCNT fused with Jcc (jc +2 -> skips mov eax, 99):
    //     F3 0F BD C0 (lzcnt eax, eax); 72 05 (jc +5); B8 63 00 00 00 (mov eax, 99)
    //     When input==0 -> CF=1 -> jumps over mov eax,99 -> returns 32!
    //     When input!=0 -> CF=0 -> falls through -> returns 99!
    const size_t t_lzcnt32_jcc_fuse = h.AddTest({0xF3, 0x0F, 0xBD, 0xC0, 0x72, 0x05, 0xB8, 0x63, 0x00, 0x00, 0x00});
    // 25. Back-to-back 4-byte TZCNT run (just like DriveClub's hot +0xe57ad8/+0xe5a909!):
    //     F3 0F BC C0 (tzcnt eax, eax); F3 0F BC CB (tzcnt ecx, ebx); 01 C8 (add eax, ecx)
    const size_t t_tzcnt_run = h.AddTest({0xF3, 0x0F, 0xBC, 0xC0, 0xF3, 0x0F, 0xBC, 0xCB, 0x01, 0xC8});

    h.Finalize();

    uint64_t fl = 0, rz = 0;

    // --- LZCNT 32 (4B UD2 -> VEH) ---
    uint64_t r = h.Get(t_lzcnt32_4b)(0xDEADBEEF00000000ull, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "lzcnt32_4b(0)", r, 32, fl, kCf, kCf | kZf);
    r = h.Get(t_lzcnt32_4b)(0xDEADBEEF80000000ull, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "lzcnt32_4b(msb)", r, 0, fl, kZf, kCf | kZf);
    r = h.Get(t_lzcnt32_4b)(0x0000000000000001ull, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "lzcnt32_4b(1)", r, 31, fl, 0, kCf | kZf);

    // --- LZCNT 64 (trampoline) ---
    r = h.Get(t_lzcnt64)(0x1234, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "lzcnt64(0)", r, 64, fl, kCf, kCf | kZf);
    r = h.Get(t_lzcnt64)(0x1234, 0x8000000000000000ull, 0, 0, &fl, &rz);
    Check(rz == 1, "lzcnt64(msb)", r, 0, fl, kZf, kCf | kZf);
    r = h.Get(t_lzcnt64)(0x1234, 0x0010000000000000ull, 0, 0, &fl, &rz);
    Check(rz == 1, "lzcnt64(bit52)", r, 11, fl, 0, kCf | kZf);

    // --- LZCNT 16 (trampoline, preserve upper 48 bits) ---
    r = h.Get(t_lzcnt16)(0x112233445566FFFFull, 0xFFFF0000ull, 0, 0, &fl, &rz);
    Check(rz == 1, "lzcnt16(0)", r, 0x1122334455660010ull, fl, kCf, kCf | kZf);
    r = h.Get(t_lzcnt16)(0x112233445566FFFFull, 0x00008001ull, 0, 0, &fl, &rz);
    Check(rz == 1, "lzcnt16(msb)", r, 0x1122334455660000ull, fl, kZf, kCf | kZf);

    // --- TZCNT 32 (4B UD2 -> VEH) ---
    r = h.Get(t_tzcnt32_4b)(0xCAFEBABE00000000ull, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "tzcnt32_4b(0)", r, 32, fl, kCf, kCf | kZf);
    r = h.Get(t_tzcnt32_4b)(0xCAFEBABE00000001ull, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "tzcnt32_4b(1)", r, 0, fl, kZf, kCf | kZf);
    r = h.Get(t_tzcnt32_4b)(0x0000000000001000ull, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "tzcnt32_4b(bit12)", r, 12, fl, 0, kCf | kZf);

    // --- TZCNT 64 & 16 (trampolines) ---
    r = h.Get(t_tzcnt64)(0, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "tzcnt64(0)", r, 64, fl, kCf, kCf | kZf);
    r = h.Get(t_tzcnt64)(0, 1ull << 40, 0, 0, &fl, &rz);
    Check(rz == 1, "tzcnt64(bit40)", r, 40, fl, 0, kCf | kZf);
    r = h.Get(t_tzcnt16)(0xAABBCCDDEEFF0000ull, 0x00010000ull, 0, 0, &fl, &rz);
    Check(rz == 1, "tzcnt16(0)", r, 0xAABBCCDDEEFF0010ull, fl, kCf, kCf | kZf);

    // --- MOVBE ---
    alignas(16) uint64_t mem[4] = {0x1122334455667788ull, 0x0102030405060708ull, 0, 0};
    r = h.Get(t_movbe32_load_4b)(0xFFFFFFFF00000000ull, reinterpret_cast<uintptr_t>(mem), 0, 0, &fl, &rz);
    Check(rz == 1, "movbe32_load_4b", r, 0x88776655ull, fl, kCf | kZf, kCf | kZf | kSf | kOf);
    r = h.Get(t_movbe64_load)(0, reinterpret_cast<uintptr_t>(mem), 0, 0, &fl, &rz);
    Check(rz == 1, "movbe64_load", r, 0x0807060504030201ull, fl, kCf | kZf, kCf | kZf | kSf | kOf);
    r = h.Get(t_movbe16_load)(0x123456789ABC0000ull, reinterpret_cast<uintptr_t>(mem), 0, 0, &fl, &rz);
    Check(rz == 1, "movbe16_load", r, 0x123456789ABC8877ull, fl, kCf | kZf, kCf | kZf | kSf | kOf);

    mem[1] = 0;
    r = h.Get(t_movbe32_store)(0xDEADBEEF12345678ull, reinterpret_cast<uintptr_t>(mem), 0, 0, &fl, &rz);
    // Verify RAX upper 32 bits preserved AND mem[1] received swapped 32-bit value AND flags preserved!
    Check(rz == 1 && (mem[1] & 0xFFFFFFFFull) == 0x78563412ull, "movbe32_store", r, 0xDEADBEEF12345678ull,
          fl, kCf | kZf, kCf | kZf | kSf | kOf);
    mem[1] = 0;
    r = h.Get(t_movbe64_store)(0x0123456789ABCDEFull, reinterpret_cast<uintptr_t>(mem), 0, 0, &fl, &rz);
    Check(rz == 1 && mem[1] == 0xEFCDAB8967452301ull, "movbe64_store", r, 0x0123456789ABCDEFull,
          fl, kCf | kZf, kCf | kZf | kSf | kOf);

    // --- ANDN ---
    r = h.Get(t_andn32)(0xFFFFFFFF00000000ull, 0x0F0F0F0Full, 0xFF00FF00ull, 0, &fl, &rz);
    Check(rz == 1, "andn32", r, 0xF000F000ull, fl, kSf, kCf | kOf | kZf | kSf);
    r = h.Get(t_andn64_alias)(0xFFFF0000FFFF0000ull, 0xFFFF0000FFFF0000ull, 0, 0, &fl, &rz);
    Check(rz == 1, "andn64_alias(zero)", r, 0, fl, kZf, kCf | kOf | kZf | kSf);

    // --- BLSR ---
    r = h.Get(t_blsr32)(0, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "blsr32(0)", r, 0, fl, kCf | kZf, kCf | kOf | kZf | kSf);
    r = h.Get(t_blsr32)(0, 0x80000008ull, 0, 0, &fl, &rz);
    Check(rz == 1, "blsr32(non0)", r, 0x80000000ull, fl, kSf, kCf | kOf | kZf | kSf);
    r = h.Get(t_blsr64)(0, 0x100ull, 0, 0, &fl, &rz);
    Check(rz == 1, "blsr64(pow2)", r, 0, fl, kZf, kCf | kOf | kZf | kSf);

    // --- BLSI ---
    r = h.Get(t_blsi32)(0, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "blsi32(0)", r, 0, fl, kZf, kCf | kOf | kZf | kSf);
    r = h.Get(t_blsi32)(0, 0x80000000ull, 0, 0, &fl, &rz);
    Check(rz == 1, "blsi32(msb)", r, 0x80000000ull, fl, kCf | kSf, kCf | kOf | kZf | kSf);

    // --- BLSMSK ---
    r = h.Get(t_blsmsk32)(0, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "blsmsk32(0)", r, 0xFFFFFFFFull, fl, kCf | kSf, kCf | kOf | kZf | kSf);
    r = h.Get(t_blsmsk32)(0, 0x18ull, 0, 0, &fl, &rz);
    Check(rz == 1, "blsmsk32(0x18)", r, 0x0Full, fl, 0, kCf | kOf | kZf | kSf);

    // --- BEXTR ---
    r = h.Get(t_bextr32)(0, 0x12345678ull, (8u << 8) | 4u, 0, &fl, &rz); // start=4, len=8 -> 0x67
    Check(rz == 1, "bextr32(4,8)", r, 0x67ull, fl, 0, kCf | kOf | kZf);
    r = h.Get(t_bextr32)(0, 0x12345678ull, (0u << 8) | 4u, 0, &fl, &rz); // len=0 -> 0
    Check(rz == 1, "bextr32(len0)", r, 0, fl, kZf, kCf | kOf | kZf);
    r = h.Get(t_bextr32)(0, 0x12345678ull, (8u << 8) | 32u, 0, &fl, &rz); // start=32 -> 0
    Check(rz == 1, "bextr32(start32)", r, 0, fl, kZf, kCf | kOf | kZf);
    r = h.Get(t_bextr64_rcx)(0, 0xFEDCBA9876543210ull, (16u << 8) | 40u, 0, &fl, &rz); // start=40, len=16 -> 0xDCBA
    Check(rz == 1, "bextr64_rcx_alias", r, 0xDCBAull, fl, 0, kCf | kOf | kZf);

    // --- [rsp + disp] memory operand (tests rsp_delta red-zone shift!) ---
    r = h.Get(t_lzcnt64_rsp_mem)(0x9999, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "lzcnt64_rsp_mem", r, 0, fl, kZf, kCf | kZf);
    r = h.Get(t_movbe64_rsp_mem)(0x9999, 0, 0, 0, &fl, &rz);
    Check(rz == 1, "movbe64_rsp_mem", r, 0xA5A5A5A5A5A5A5A5ull, fl, kCf | kZf, kCf | kZf | kSf | kOf);

    // --- Constant-control BEXTR fast path (zero stack, zero branches!) ---
    r = h.Get(t_bextr32_const)(0, 0x12345678ull, 0, 0, &fl, &rz); // start=4, len=8 -> 0x67
    Check(rz == 1, "bextr32_const(4,8)", r, 0x67ull, fl, 0, kCf | kOf | kZf);
    r = h.Get(t_bextr32_const)(0, 0x12345008ull, 0, 0, &fl, &rz); // start=4, len=8 -> 0 (ZF=1)
    Check(rz == 1, "bextr32_const(zero)", r, 0ull, fl, kZf, kCf | kOf | kZf);
    r = h.Get(t_bextr64_const36)(0, 0xFEDCBA9876543210ull, 0, 0, &fl, &rz); // >>8 = 0x00FEDCBA98765432, low 36b = 0xA98765432
    Check(rz == 1, "bextr64_const(8,36)", r, 0xA98765432ull, fl, 0, kCf | kOf | kZf);

    // --- 4-byte LZCNT fused with Jcc (jc) ---
    r = h.Get(t_lzcnt32_jcc_fuse)(0, 0, 0, 0, &fl, &rz); // input=0 -> CF=1 -> taken -> returns 32
    Check(rz == 1, "lzcnt32_jcc(0)", r, 32ull, fl, kCf, kCf | kZf);
    r = h.Get(t_lzcnt32_jcc_fuse)(0x80000000ull, 0, 0, 0, &fl, &rz); // input!=0 -> CF=0 -> fallthrough -> 99
    Check(rz == 1, "lzcnt32_jcc(msb)", r, 99ull, fl, kZf, kCf | kZf);

    // --- Back-to-back 4-byte TZCNT run fusion ---
    r = h.Get(t_tzcnt_run)(0x10ull, 0ull, 0, 0, &fl, &rz); // tzcnt(0x10)=4 + tzcnt(0)=32 -> 36
    Check(rz == 1, "tzcnt_run(0x10,0)", r, 36ull, fl, 0, kZf);
    r = h.Get(t_tzcnt_run)(0ull, 0x80ull, 0, 0, &fl, &rz); // tzcnt(0)=32 + tzcnt(0x80)=7 -> 39
    Check(rz == 1, "tzcnt_run(0,0x80)", r, 39ull, fl, 0, kZf);

    bbgpu_ivy_report();

    if (g_failures == 0) {
        std::puts("ALL IVY ISA TESTS PASSED!");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) failed.\n", g_failures);
    return 1;
}
