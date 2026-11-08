/* The CPU that runs the game's x86-64 code: the host CPU on x86-64 (src/runtime_cpu.c), FEXCore's
 * JIT elsewhere (cpu/, libbbcpu.so; from the bbport ARM64 port). Guest and host share one address
 * space either way; what differs is how control crosses between them, which goes through these calls:
 *  - host functions handed to the guest (imports, patch thunks) are wrapped by guest_cpu_host_function;
 *  - guest functions called by the host (thread entries, callbacks) go through guest_cpu_call;
 *  - guest register state at a signal is read with guest_cpu_signal_regs. */
#ifndef BB_GUEST_CPU_H
#define BB_GUEST_CPU_H
#include <stddef.h>
#include <stdint.h>
#ifndef _WIN32
#include <signal.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#define GUEST_CPU_NATIVE 1
#endif
#ifdef __cplusplus
extern "C" {
#endif
#ifndef _WIN32
#pragma GCC visibility push(default)
#endif

/* Guest registers in x86 encoding order. */
enum {
    GUEST_RAX, GUEST_RCX, GUEST_RDX, GUEST_RBX, GUEST_RSP, GUEST_RBP, GUEST_RSI, GUEST_RDI,
    GUEST_R8, GUEST_R9, GUEST_R10, GUEST_R11, GUEST_R12, GUEST_R13, GUEST_R14, GUEST_R15,
    GUEST_GPRS
};
typedef struct { uint64_t gpr[GUEST_GPRS]; uint64_t rip; } GuestRegs;

/* Once, before any other call. `map` allocates memory the guest may hold pointers to (below
 * 1 TiB: the game packs pointers into 40 bits), as runtime_low_map. */
void guest_cpu_init(void *(*map)(size_t size, int prot));
/* Guest code lives in [base, base + size) (the image, import traps, patch thunks). */
void guest_cpu_code(uintptr_t base, size_t size);
/* The host changed guest code in [address, address + size) after the game started running
 * (a hook installed late): translations of it are dropped. No-op on x86-64 hosts. */
void guest_cpu_code_changed(uintptr_t address, size_t size);
/* The guest-callable address of host function `fn` (SysV ABI on the guest side, at most 14
 * integer arguments and 8 vector arguments; no structures by value, no va_list). */
uintptr_t guest_cpu_host_function(void *fn);
/* Calls guest function `fn` with `count` integer arguments; returns RAX. On this thread's current
 * guest stack when the host was called by guest code, else on the stack set by
 * guest_cpu_thread_stack (or one allocated for the thread). */
uint64_t guest_cpu_call(uintptr_t fn, unsigned count, const uint64_t *args);
/* As guest_cpu_call, and the low 64 bits of XMM0..XMM(xmm_count-1) set from `xmm` (a float
 * argument is its bit pattern in the low 32 bits). Not available on x86-64 hosts: the caller
 * calls the function directly there. */
uint64_t guest_cpu_call_xmm(uintptr_t fn, unsigned count, const uint64_t *args, unsigned xmm_count,
                            const uint64_t *xmm);
/* The guest stack of this host thread for calls not nested in guest code: [top - size, top). */
void guest_cpu_thread_stack(void *top, size_t size);
/* Guest GS base (the TCB) of this host thread. */
void guest_cpu_set_gs(void *base);
/* This thread left guest code with longjmp (pthread_exit): nested calls are over. */
void guest_cpu_abandon(void);
/* This host thread is ending: its guest CPU state is released. */
void guest_cpu_thread_end(void);
/* The return address of the guest call that entered the current host function, for diagnostics.
 * `native` is __builtin_return_address(0) of that function (the answer on x86-64 hosts). */
uintptr_t guest_cpu_return_address(uintptr_t native);
/* The guest stack pointer of the innermost guest call into the host on this thread, or 0. */
uintptr_t guest_cpu_stack_pointer(void);
/* The guest registers at a signal. GUEST_CPU_IN_GUEST: the signal interrupted guest code, `regs`
 * as there (always the answer on x86-64 hosts, where host code is not told apart).
 * GUEST_CPU_IN_HOST_CALL: host code called by the guest, `regs` as at that call (rip: the return
 * address into the guest). 0: no guest code below; `regs` holds the host PC and stack pointer. */
enum { GUEST_CPU_IN_GUEST=1, GUEST_CPU_IN_HOST_CALL=2 };
int guest_cpu_signal_regs(const void *ucontext, GuestRegs *regs);
/* The host program counter at a signal. */
uintptr_t guest_cpu_host_pc(const void *ucontext);
#ifndef _WIN32
/* Faults raised by the guest CPU itself (JIT bookkeeping): handled and to be resumed when 1. */
int guest_cpu_handle_fault(int sig, siginfo_t *info, void *ucontext);
#endif
/* Replaces the guest instruction at `address` (at least `size` >= 2 bytes) with a call of
 * `hook`, which sees and edits the registers and must set rip. RCX and R11 hold garbage when the
 * hook runs, so the original code must not read them there. Not available on x86-64 hosts: returns 0. */
typedef void (*GuestHook)(GuestRegs *regs);
int guest_cpu_hook(uintptr_t address, size_t size, GuestHook hook);
/* The guest's time stamp counter (RDTSC) and its frequency in Hz. */
uint64_t guest_cpu_tsc(void);
uint64_t guest_cpu_tsc_frequency(void);

#ifndef _WIN32
#pragma GCC visibility pop
#endif
#ifdef __cplusplus
}
#endif
#endif
