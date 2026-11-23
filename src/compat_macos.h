/* macOS (x86_64 under Rosetta 2): the Linux runtime's few Linux-only calls, mapped to Darwin.
 * build-macos.sh force-includes this header (-include) into every runtime C file, before
 * anything else, so the Linux sources stay as they are wherever the POSIX call is the same.
 *
 * The runtime's own heap is also moved low here: the game packs host pointers it is handed
 * (thread handles, mutexes, TLS blocks) into 40-bit fields. glibc's non-PIE brk heap is below
 * 1 TiB on Linux; macOS's malloc zones are at 0x6000'0000'0000 and above. malloc and friends
 * in the runtime sources go to a small allocator over a reserved range below 32 GiB
 * (compat_macos.c); pointers from anywhere else are passed on to the system allocator. */
#ifndef BB_COMPAT_MACOS_H
#define BB_COMPAT_MACOS_H
#if defined(__APPLE__) && !defined(__cplusplus)

#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
/* <ucontext.h> wants it for the (deprecated) ucontext routines; the signal handlers only read
 * the context they are handed. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/uio.h>

/* ---- address space ----
 * Two ranges are reserved PROT_NONE before main (compat_macos.c), so that the host's own
 * mappings, which macOS places low and growing, never land where the guest's go:
 *   [BB_MAC_LOW_HEAP, BB_MAC_LOW_END)   the runtime heap, then runtime_low_map (LOW_MIN..LOW_MAX)
 *   [BB_MAC_USER_MIN, BB_MAC_USER_MAX)  the PS4 user range (runtime_memory.c USER_MIN..USER_MAX)
 * The kernel holds [0x0fc0000000, 0x7000000000), between the two, in every process (max
 * protection none; native arm64 processes too). */
#define BB_MAC_LOW_HEAP       UINT64_C(0x0400000000)  /* runtime heap: 16 GiB */
#define BB_MAC_LOW_HEAP_END   UINT64_C(0x0800000000)  /* = runtime_memory.c LOW_MIN */
#define BB_MAC_LOW_END        UINT64_C(0x0fc0000000)  /* = runtime_memory.c LOW_MAX */
#define BB_MAC_USER_MIN       UINT64_C(0x7000000000)
#define BB_MAC_USER_MAX       UINT64_C(0xfc00000000)
/* The game maps one subsystem's memory at fixed addresses from 0x1000000000, the bottom of the
 * PS4 app area, inside the macOS kernel's hole. probe.c patch_app_area moves it here, and
 * runtime_memory.c find_free keeps this range for it. */
#define BB_MAC_APP_AREA       UINT64_C(0xb000000000)
#define BB_MAC_APP_AREA_END   UINT64_C(0xc000000000)
#define BB_PS4_APP_AREA       UINT64_C(0x1000000000)
int bb_mac_reserved(uintptr_t address, size_t size);
/* Gives [address, address+size) back to the reservation (instead of munmap). */
int bb_mac_unreserve_release(void *address, size_t size);

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
/* Used by runtime_low_map only, always inside the reservation: MAP_FIXED there replaces
 * nothing but our own PROT_NONE pages. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif

/* ---- memfd ---- */
#define MFD_CLOEXEC 1u
int memfd_create(const char *name, unsigned flags);
#define FALLOC_FL_KEEP_SIZE  1
#define FALLOC_FL_PUNCH_HOLE 2

/* ---- threads ---- */
static inline pid_t bb_mac_gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (pid_t)id;
}
#define gettid bb_mac_gettid
/* Darwin names only the calling thread. */
static inline int bb_mac_setname(pthread_t thread, const char *name) {
    return pthread_equal(thread, pthread_self()) ? pthread_setname_np(name) : 0;
}
#define pthread_setname_np(thread, name) bb_mac_setname(thread, name)
int bb_mac_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline);
#define pthread_mutex_timedlock bb_mac_mutex_timedlock
/* Sends `sig` to every thread of the process but the caller (watchdog dumps). */
void bb_mac_signal_other_threads(int sig);

/* Runs `body` on a second thread and serves bb_mac_run_on_main jobs on this, the first one,
 * which Cocoa requires; exits with what `body` returns. */
int bb_mac_main(int (*body)(int, char **), int argc, char **argv);
void bb_mac_run_on_main(void (*fn)(void *), void *arg);

/* ---- time ---- */
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
int bb_mac_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain);
#define clock_nanosleep bb_mac_clock_nanosleep

/* ---- names Darwin spells differently ---- */
#include <sys/stat.h>
#include <sys/resource.h>
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
/* Darwin has no per-thread rusage: the process's figures stand in (diagnostics only). */
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD RUSAGE_SELF
#endif
/* Darwin condition variables time out against CLOCK_REALTIME only; runtime_sema.c takes its
 * deadlines from that clock on macOS, as on Windows. */
static inline int bb_mac_condattr_setclock(pthread_condattr_t *attr, clockid_t clock) {
    (void)attr;
    return clock == CLOCK_REALTIME ? 0 : EINVAL;
}
#define pthread_condattr_setclock bb_mac_condattr_setclock

/* ---- misc ---- */
ssize_t bb_mac_getrandom(void *buffer, size_t length, unsigned flags);
#define getrandom bb_mac_getrandom
ssize_t process_vm_readv(pid_t pid, const struct iovec *local, unsigned long nlocal,
                         const struct iovec *remote, unsigned long nremote, unsigned long flags);
void bb_mac_close_from(int lowest);
#define M_ARENA_MAX 0
#define M_MMAP_THRESHOLD 0
static inline int mallopt(int param, int value) { (void)param; (void)value; return 1; }

/* ---- the runtime's heap below 1 TiB ---- */
void *bb_low_malloc(size_t size);
void *bb_low_calloc(size_t count, size_t size);
void *bb_low_realloc(void *pointer, size_t size);
void *bb_low_aligned_alloc(size_t alignment, size_t size);
int bb_low_posix_memalign(void **out, size_t alignment, size_t size);
void bb_low_free(void *pointer);
char *bb_low_strdup(const char *text);
char *bb_low_strndup(const char *text, size_t length);
/* Function-like, so __attribute__((malloc)) in other headers is left alone. The runtime never
 * takes the address of these functions. */
#ifndef BB_LOW_HEAP_IMPLEMENTATION
#define malloc(size) bb_low_malloc(size)
#define calloc(count, size) bb_low_calloc(count, size)
#define realloc(pointer, size) bb_low_realloc(pointer, size)
#define aligned_alloc(alignment, size) bb_low_aligned_alloc(alignment, size)
#define posix_memalign(out, alignment, size) bb_low_posix_memalign(out, alignment, size)
#define free(pointer) bb_low_free(pointer)
#define strdup(text) bb_low_strdup(text)
#define strndup(text, length) bb_low_strndup(text, length)
#endif

#endif
#endif
