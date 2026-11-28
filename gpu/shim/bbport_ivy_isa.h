// SPDX-License-Identifier: GPL-2.0-or-later
// Ivy Bridge (and pre-Haswell AVX1) compatibility layer for DriveClub Recomp.
//
// PS4's AMD Jaguar CPU supports AVX1, F16C, POPCNT, SSE4a, BMI1, LZCNT, and MOVBE.
// Intel Ivy Bridge has AVX1, F16C, and POPCNT, and probe.c already handles SSE4a.
// The remaining delta is:
//   - MOVBE, ANDN, BEXTR, BLSI, BLSMSK, BLSR  -> raise #UD (0xC000001D) on Ivy Bridge
//   - LZCNT (F3 0F BD)                        -> NO #UD! Silently executes as BSR (wrong for all values)
//   - TZCNT (F3 0F BC)                        -> NO #UD! Silently executes as BSF (wrong for 0 and CF)
//
// This module provides:
//   1. Load-time CFG-guided static binary rewriting using Zydis + Xbyak into a +-2 GiB
//      trampoline arena (with SysV 128-byte red-zone preservation on Windows).
//   2. UD2-trap fallback for 4-byte LZCNT/TZCNT encodings that cannot fit a 5-byte E9 rel32
//      without risking overwriting a branch target at rip+4.
//   3. A Zydis-based Vectored Exception Handler (#UD) emulator covering MOVBE, all VEX BMI1 ops,
//      and UD2-trapped 4-byte LZCNT/TZCNT sites, with per-mnemonic hit counters.
#ifndef BBPORT_IVY_ISA_H
#define BBPORT_IVY_ISA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t address;
    uint64_t size;
    uint64_t flags; /* bit 0: executable (PF_X) */
} BbIvySegment;

/* Rewrites guest executable segments for hosts lacking BMI1, MOVBE, or LZCNT.
 * Must run after all runtime byte-signature hooks are installed and before segments are protected RX.
 * `roots` is an array of known guest code offsets (entry, module inits, function bindings, code relocs).
 * Returns the number of rewritten sites (trampolines + UD2 traps). No-op returning 0 on Haswell+/Ryzen
 * unless forced with BB_IVY_ISA=1. */
uint64_t bbgpu_ivy_rewrite_guest(unsigned char *image, uint64_t image_size,
                                 const BbIvySegment *segments, uint64_t segment_count,
                                 const uint64_t *roots, uint64_t root_count);

/* Called from vectored_fault() on EXCEPTION_ILLEGAL_INSTRUCTION (Windows).
 * Emulates MOVBE, VEX BMI1, and UD2-trapped 4-byte LZCNT/TZCNT sites in guest code.
 * Returns 1 if the instruction was emulated and execution should continue, 0 otherwise. */
int bbgpu_ivy_handle_ud(void *exception_pointers);

/* Returns 1 if `rip` lies inside the Ivy Bridge trampoline arena (for crash diagnostics). */
int bbgpu_ivy_in_trampoline(uintptr_t rip);

/* Prints rewrite and VEH emulation statistics (if active or BB_IVY_STATS=1). */
void bbgpu_ivy_report(void);

#ifdef __cplusplus
}
#endif

#endif /* BBPORT_IVY_ISA_H */
