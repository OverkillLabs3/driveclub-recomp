/* Guest CPU self-test: small x86-64 functions run through the guest CPU (FEXCore on ARM64, out/fex/libbbcpu.so)
 * without the game. Checks the crossings the runtime uses (host calls guest, guest calls host, nested calls,
 * vector arguments, SSE/AVX, locked atomics, RDTSC, several threads) and times the JIT.
 * build.sh builds it as out/dc-cpu-selftest; `run.sh --cpu-selftest` (the AppImage too) runs it, `--quick`
 * without the speed measurements (the package's build check). On x86-64 hosts (src/runtime_cpu.c) the same code
 * runs natively. */
#define _GNU_SOURCE
#include "../src/guest_cpu.h"
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Assembled from (Intel syntax):
 *  f:     lea rax,[rdi+rdi*2]; add rax,rsi; ret                         f(a, b) = 3a + b
 *  g:     sub rsp,8; mov esi,5; movabs rax,HOST; call rax; add rax,1; add rsp,8; ret     g(x) = host(x, 5) + 1
 *  h:     addss xmm0,xmm0; cvttss2si eax,xmm0; ret                      h(float a) = (int)(2a)
 *  loop:  xor eax,eax; 1: add rax,rdi; dec rdi; jnz 1b; ret             loop(n) = 1 + 2 + ... + n
 *  tsc:   rdtsc; shl rdx,32; or rax,rdx; ret
 *  avx:   vbroadcastss ymm0,[rdi]; vaddps ymm0,ymm0,ymm0; vmovups [rsi],ymm0; vzeroupper; ret
 *  atom:  mov eax,1; lock xadd [rdi],rax; ret
 *  hloop: push rbx; push r12; sub rsp,8; mov rbx,rdi; movabs r12,TRIVIAL
 *         2: mov rdi,rbx; call r12; dec rbx; jnz 2b; add rsp,8; pop r12; pop rbx; ret   n calls of trivial() */
static const unsigned char guest_code[] = {
    0x48,0x8d,0x04,0x7f,0x48,0x01,0xf0,0xc3,0x48,0x83,0xec,0x08,0xbe,0x05,0x00,0x00,
    0x00,0x48,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0xff,0xd0,0x48,0x83,0xc0,
    0x01,0x48,0x83,0xc4,0x08,0xc3,0xf3,0x0f,0x58,0xc0,0xf3,0x0f,0x2c,0xc0,0xc3,0x31,
    0xc0,0x48,0x01,0xf8,0x48,0xff,0xcf,0x75,0xf8,0xc3,0x0f,0x31,0x48,0xc1,0xe2,0x20,
    0x48,0x09,0xd0,0xc3,0xc4,0xe2,0x7d,0x18,0x07,0xc5,0xfc,0x58,0xc0,0xc5,0xfc,0x11,
    0x06,0xc5,0xf8,0x77,0xc3,0xb8,0x01,0x00,0x00,0x00,0xf0,0x48,0x0f,0xc1,0x07,0xc3,
    0x53,0x41,0x54,0x48,0x83,0xec,0x08,0x48,0x89,0xfb,0x49,0xbc,0x88,0x77,0x66,0x55,
    0x44,0x33,0x22,0x11,0x48,0x89,0xdf,0x41,0xff,0xd4,0x48,0xff,0xcb,0x75,0xf5,0x48,
    0x83,0xc4,0x08,0x41,0x5c,0x5b,0xc3,
};
enum { F = 0x00, G = 0x08, G_HOST = 0x13, H = 0x26, LOOP = 0x2f, TSC = 0x3a, AVX = 0x44, ATOM = 0x55,
       HLOOP = 0x60, HLOOP_HOST = 0x6c };

/* Memory the guest sees stays below 1 TiB, as with runtime_low_map. */
static pthread_mutex_t low_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t low_next = UINT64_C(0x0800000000);
static void *low_map(size_t size, int prot) {
    size = (size + 0xfff) & ~(size_t)0xfff;
    void *p = NULL;
    pthread_mutex_lock(&low_lock);
    while (!p && low_next + size < (UINT64_C(1) << 40)) {
        void *want = (void *)low_next;
        void *got = mmap(want, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        low_next += size + 0x1000;
        if (got == want) p = got;
        else if (got != MAP_FAILED) munmap(got, size); /* kernels before 4.17 ignore the flag */
    }
    pthread_mutex_unlock(&low_lock);
    return p;
}

static void fault(int sig, siginfo_t *info, void *context) {
    if (guest_cpu_handle_fault(sig, info, context)) return;
    GuestRegs regs;
    const int where = guest_cpu_signal_regs(context, &regs);
    char line[256];
    snprintf(line, sizeof(line), "cpu-selftest: signal %d, address %p, %s, guest rip 0x%" PRIx64 ", host pc %p\n", sig,
             info->si_addr, where == GUEST_CPU_IN_GUEST ? "in guest code" : where == GUEST_CPU_IN_HOST_CALL ? "in a host call" : "in host code",
             regs.rip, (void *)guest_cpu_host_pc(context));
    ssize_t written = write(2, line, strlen(line));
    (void)written;
    _exit(3);
}

static uintptr_t base;
static int failures;
static void check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) printf("  %-48s ok\n", what);
    else { printf("  %-48s FAIL: %" PRIu64 ", expected %" PRIu64 "\n", what, got, want); ++failures; }
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static uint64_t call(unsigned offset, unsigned count, uint64_t a0, uint64_t a1) {
    const uint64_t args[2] = {a0, a1};
    return guest_cpu_call(base + offset, count, args);
}
static void own_stack(void) {
    const size_t size = 1 << 20;
    unsigned char *stack = low_map(size, PROT_READ | PROT_WRITE);
    if (!stack) { fprintf(stderr, "cpu-selftest: no low memory for a guest stack\n"); exit(2); }
    guest_cpu_thread_stack(stack + size - 64, size - 64);
}

/* Called by guest code: calls guest code again (as the runtime's callbacks do inside imports). */
static uint64_t host(uint64_t x, uint64_t y) { return x * 10 + y + call(F, 2, 1, 2); }
static uint64_t trivial(uint64_t x) { return x; }

static uint64_t *shared_counter;
enum { THREADS = 4, THREAD_LOOP = 20000000, THREAD_ATOMS = 20000 };
static void *worker(void *arg) {
    const uint64_t n = THREAD_LOOP + (uintptr_t)arg;
    own_stack();
    uint64_t ok = call(LOOP, 1, n, 0) == n * (n + 1) / 2;
    for (int i = 0; i < THREAD_ATOMS; ++i) call(ATOM, 1, (uintptr_t)shared_counter, 0);
    ok &= call(G, 1, 3, 0) == 41; /* host and nested guest calls on this thread too */
    guest_cpu_thread_end();
    return (void *)(uintptr_t)ok;
}

int main(int argc, char **argv) {
    const int quick = argc > 1 && !strcmp(argv[1], "--quick");
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (sysconf(_SC_PAGESIZE) != 4096)
        printf("cpu-selftest: this kernel uses %ld-byte pages; the runtime needs 4 KB pages\n", sysconf(_SC_PAGESIZE));
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    guest_cpu_init(low_map);
    unsigned char *code = low_map(4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    unsigned char *data = low_map(4096, PROT_READ | PROT_WRITE);
    if (!code || !data) { fprintf(stderr, "cpu-selftest: no memory below 1 TiB\n"); return 2; }
    memcpy(code, guest_code, sizeof(guest_code));
    const uint64_t host_address = guest_cpu_host_function((void *)host);
    const uint64_t trivial_address = guest_cpu_host_function((void *)trivial);
    memcpy(code + G_HOST, &host_address, 8);
    memcpy(code + HLOOP_HOST, &trivial_address, 8);
    base = (uintptr_t)code;
    guest_cpu_code(base, 4096);
    __builtin___clear_cache((char *)code, (char *)code + sizeof(guest_code));
    own_stack();

    printf("cpu-selftest: crossings\n");
    check("host calls guest: f(4, 3) = 15", call(F, 2, 4, 3), 15);
    check("guest calls host, host calls guest: g(4) = 51", call(G, 1, 4, 0), 51);
#ifdef GUEST_CPU_NATIVE
    check("float argument (SSE): h(1.5) = 3", (uint64_t)((int (*)(float))(base + H))(1.5f), 3);
#else
    {
        const float a = 1.5f;
        uint64_t xmm = 0;
        memcpy(&xmm, &a, sizeof(a));
        check("float argument (SSE): h(1.5) = 3", (uint32_t)guest_cpu_call_xmm(base + H, 0, NULL, 1, &xmm), 3);
    }
#endif
    {
        float *in = (float *)data, *out = (float *)(data + 64);
        *in = 1.25f;
        call(AVX, 2, (uintptr_t)in, (uintptr_t)out);
        unsigned good = 0;
        for (int i = 0; i < 8; ++i) good += out[i] == 2.5f;
        check("AVX (256-bit): 8 lanes of 1.25 + 1.25", good, 8);
    }
    shared_counter = (uint64_t *)(data + 256);
    *shared_counter = 41;
    check("lock xadd returns the old value", call(ATOM, 1, (uintptr_t)shared_counter, 0), 41);
    check("lock xadd stored the new value", *shared_counter, 42);
    {
        const uint64_t frequency = guest_cpu_tsc_frequency();
        const double t0 = now();
        const uint64_t c0 = call(TSC, 0, 0, 0);
        usleep(200000);
        const uint64_t c1 = call(TSC, 0, 0, 0);
        const double rate = (double)(c1 - c0) / (now() - t0);
        printf("  RDTSC: %.1f MHz measured, %.1f MHz reported\n", rate / 1e6, (double)frequency / 1e6);
        check("RDTSC rate within 5% of the reported one", rate > frequency * 0.95 && rate < frequency * 1.05, 1);
    }

    printf("cpu-selftest: %d threads (loop, locked atomics, host calls)\n", THREADS);
    {
        pthread_t threads[THREADS];
        *shared_counter = 0;
        for (uintptr_t i = 0; i < THREADS; ++i) pthread_create(&threads[i], NULL, worker, (void *)i);
        uint64_t good = 0;
        for (int i = 0; i < THREADS; ++i) {
            void *result;
            pthread_join(threads[i], &result);
            good += (uintptr_t)result;
        }
        check("every thread's results", good, THREADS);
        check("no atomic increment lost", *shared_counter, (uint64_t)THREADS * THREAD_ATOMS);
    }

    if (!quick) {
        printf("cpu-selftest: speed\n");
        const uint64_t n = 300000000;
        call(LOOP, 1, 1000, 0); /* compiled */
        double t0 = now();
        const uint64_t sum = call(LOOP, 1, n, 0);
        const double guest = now() - t0;
        check("loop(300M)", sum, n * (n + 1) / 2);
        printf("  JIT loop (add, dec, jnz): %.0f M iterations/s", n / guest / 1e6);
#if defined(__aarch64__)
        uint64_t s = 0, k = n;
        t0 = now();
        __asm__ volatile("1: add %0, %0, %1\n subs %1, %1, #1\n b.ne 1b" : "+r"(s), "+r"(k) : : "cc");
        printf(", native ARM64 loop %.0f M/s", n / (now() - t0) / 1e6);
#endif
        printf("\n");
        const uint64_t calls = 2000000;
        call(HLOOP, 1, 1000, 0);
        t0 = now();
        call(HLOOP, 1, calls, 0);
        printf("  guest -> host call (an import): %.0f ns\n", (now() - t0) / calls * 1e9);
        const unsigned host_calls = 200000;
        t0 = now();
        for (unsigned i = 0; i < host_calls; ++i) call(F, 2, i, 1);
        printf("  host -> guest call (a callback): %.0f ns\n", (now() - t0) / host_calls * 1e9);
    }
    guest_cpu_thread_end();
    printf(failures ? "cpu-selftest: %d FAILED\n" : "cpu-selftest: all passed\n", failures);
    return failures ? 1 : 0;
}
