/* Blocked-thread diagnostic.
 *
 * Every blocking runtime call (semaphore, condition variable, event flag, contended mutex,
 * sleep) records what it is waiting on and the guest address that called it. A watchdog thread
 * writes a snapshot of threads that have waited for a while to <user dir>/threads.txt every few
 * seconds. When the game stalls (a character stuck idle, a scene that never advances), the file
 * says which threads are blocked, on what, and from which guest function: the return addresses
 * are guest image offsets, ready to look up in a disassembler.
 *
 * Cost: a thread-local store and a clock read per blocking call. Nothing is written until a wait
 * has lasted DIAG_MIN_WAIT_MS. */
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DIAG_MIN_WAIT_MS 2000
#define DIAG_PERIOD_MS 3000

typedef struct DiagThread {
    char name[32];
    const char *kind;
    uint64_t arg, since_ns;
    const void *caller;
    int depth;
    struct DiagThread *next;
} DiagThread;

uintptr_t runtime_image_base;
uint64_t runtime_image_size;
volatile int runtime_image_ready; /* the image has its final protection: other threads may read it */
static DiagThread *threads;
static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local DiagThread *self;
static int started;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static DiagThread *me(void) {
    if (!self) {
        self = calloc(1, sizeof(*self));
        if (!self) return NULL;
        snprintf(self->name, sizeof(self->name), "host-thread");
        pthread_mutex_lock(&list_lock);
        self->next = threads;
        threads = self;
        pthread_mutex_unlock(&list_lock);
    }
    return self;
}
void runtime_diag_name(const char *name) {
    DiagThread *t = me();
    if (t && name && *name) snprintf(t->name, sizeof(t->name), "%s", name);
}
void runtime_diag_wait_begin(const char *kind, uint64_t arg, const void *caller) {
    DiagThread *t = me();
    if (!t) return;
    if (t->depth++) return; /* nested (a POSIX wrapper around a native wait): the outer call owns it */
    t->arg = arg;
    t->caller = caller;
    t->since_ns = now_ns();
    __atomic_store_n(&t->kind, kind, __ATOMIC_RELEASE);
}
void runtime_diag_wait_end(void) {
    if (self && self->depth > 0 && --self->depth == 0) __atomic_store_n(&self->kind, NULL, __ATOMIC_RELEASE);
}
static void describe(FILE *f, const void *caller) {
    const uintptr_t c = (uintptr_t)caller;
    if (runtime_image_base && c >= runtime_image_base && c - runtime_image_base < runtime_image_size)
        fprintf(f, "guest+0x%llx", (unsigned long long)(c - runtime_image_base));
    else
        fprintf(f, "%p", caller);
}
/* The same table as threads.txt, for the log (freeze report). */
void runtime_diag_print(void *file) {
    FILE *f = (FILE *)file;
    const uint64_t now = now_ns();
    fprintf(f, "Blocked guest threads (waits of %d ms or more):\n", DIAG_MIN_WAIT_MS);
    pthread_mutex_lock(&list_lock);
    for (DiagThread *t = threads; t; t = t->next) {
        const char *kind = __atomic_load_n(&t->kind, __ATOMIC_ACQUIRE);
        if (!kind) continue;
        const uint64_t waited = (now - t->since_ns) / 1000000u;
        if (waited < DIAG_MIN_WAIT_MS) continue;
        fprintf(f, "  %-24s %-9s id=%llu waited=%llums from ", t->name, kind, (unsigned long long)t->arg,
                (unsigned long long)waited);
        describe(f, t->caller);
        fputc('\n', f);
    }
    pthread_mutex_unlock(&list_lock);
    fflush(f);
}
static void *watchdog(void *unused) {
    (void)unused;
    for (;;) {
        struct timespec nap = {.tv_sec = DIAG_PERIOD_MS / 1000, .tv_nsec = 0};
        nanosleep(&nap, NULL);
        const char *dir = runtime_file_user_dir();
        char path[1024];
        snprintf(path, sizeof(path), "%s/threads.txt", dir && *dir ? dir : ".");
        FILE *f = fopen(path, "w");
        if (!f) continue;
        const uint64_t now = now_ns();
        unsigned long long seconds = (unsigned long long)(now / 1000000000u);
        fprintf(f, "blocked threads (waits of %d ms or more) at t=%llus\n", DIAG_MIN_WAIT_MS, seconds);
        pthread_mutex_lock(&list_lock);
        for (DiagThread *t = threads; t; t = t->next) {
            const char *kind = __atomic_load_n(&t->kind, __ATOMIC_ACQUIRE);
            if (!kind) continue;
            const uint64_t waited = (now - t->since_ns) / 1000000u;
            if (waited < DIAG_MIN_WAIT_MS) continue;
            fprintf(f, "  %-24s %-9s id=%llu waited=%llums from ", t->name, kind,
                    (unsigned long long)t->arg, (unsigned long long)waited);
            describe(f, t->caller);
            fputc('\n', f);
        }
        pthread_mutex_unlock(&list_lock);
        fclose(f);
    }
    return NULL;
}
void runtime_diag_start(void) {
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog, NULL) == 0) pthread_detach(t);
}
