/* macOS implementations behind compat_macos.h. See there for why each one exists. */
#define BB_LOW_HEAP_IMPLEMENTATION 1
#include "compat_macos.h"
#ifdef __APPLE__
#include <stdio.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <os/lock.h>
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
/* build-macos.sh force-includes compat_macos.h before the define above: here the names are the
 * system allocator's, which bb_low_free and bb_low_realloc pass foreign pointers on to. */
#undef malloc
#undef calloc
#undef realloc
#undef aligned_alloc
#undef posix_memalign
#undef free
#undef strdup
#undef strndup

/* ---- address space ---- */
static int reservation_ok;
/* Reserves [start, end); 0 when all of it was free. */
static int reserve(uintptr_t start, uintptr_t end) {
    const size_t size = (size_t)(end - start);
    void *p = mmap((void *)start, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == (void *)start) return 0;
    if (p != MAP_FAILED) munmap(p, size);
    /* Something is already mapped there: take what is free piece by piece (16 MiB steps). */
    const size_t step = (size_t)16 << 20;
    uintptr_t first_taken = 0;
    for (uintptr_t at = start; at < end; at += step) {
        void *q = mmap((void *)at, step, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (q != (void *)at) {
            if (q != MAP_FAILED) munmap(q, step);
            if (!first_taken) first_taken = at;
        }
    }
    fprintf(stderr, "Runtime (macOS): addresses from 0x%llx in [0x%llx, 0x%llx) were in use at startup; "
                    "guest mappings there will fail\n",
            (unsigned long long)first_taken, (unsigned long long)start, (unsigned long long)end);
    return -1;
}
__attribute__((constructor(101))) static void reserve_guest_range(void) {
    reserve(BB_MAC_LOW_HEAP, BB_MAC_LOW_END);
    reserve(BB_MAC_USER_MIN, BB_MAC_USER_MAX);
    reservation_ok = 1;
}
int bb_mac_reserved(uintptr_t address, size_t size) {
    return reservation_ok &&
           ((address >= BB_MAC_LOW_HEAP && address + size <= BB_MAC_LOW_END) ||
            (address >= BB_MAC_USER_MIN && address + size <= BB_MAC_USER_MAX));
}
int bb_mac_unreserve_release(void *address, size_t size) {
    if (!bb_mac_reserved((uintptr_t)address, size)) return munmap(address, size);
    return mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED ? -1 : 0;
}

/* ---- memfd: an unlinked POSIX shared memory object ---- */
int memfd_create(const char *name, unsigned flags) {
    char path[64];
    for (int attempt = 0; attempt < 64; ++attempt) {
        snprintf(path, sizeof(path), "/bb-%d-%u", (int)getpid(), arc4random());
        int fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(path);
            if (flags & MFD_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
            (void)name;
            return fd;
        }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

/* ---- threads ---- */
static int before(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}
int bb_mac_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline) {
    for (long nap = 1000;; nap = nap < 500000 ? nap * 2 : nap) {
        int e = pthread_mutex_trylock(mutex);
        if (e != EBUSY) return e;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (!before(&now, deadline)) return ETIMEDOUT;
        struct timespec t = {0, nap};
        nanosleep(&t, NULL);
    }
}
void bb_mac_signal_other_threads(int sig) {
    thread_act_array_t list;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return;
    mach_port_t self = pthread_mach_thread_np(pthread_self());
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        pthread_t t = list[i] != self ? pthread_from_mach_thread_np(list[i]) : NULL;
        if (t) { pthread_kill(t, sig); usleep(20000); }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(*list));
}

/* ---- time ---- */
int bb_mac_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain) {
    struct timespec wait = *request;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        clock_gettime(clock, &now);
        if (!before(&now, request)) return 0;
        wait.tv_sec = request->tv_sec - now.tv_sec;
        wait.tv_nsec = request->tv_nsec - now.tv_nsec;
        if (wait.tv_nsec < 0) { wait.tv_nsec += 1000000000L; --wait.tv_sec; }
        remain = NULL;
    }
    return nanosleep(&wait, remain) ? errno : 0;
}

/* ---- misc ---- */
ssize_t bb_mac_getrandom(void *buffer, size_t length, unsigned flags) {
    (void)flags;
    arc4random_buf(buffer, length);
    return (ssize_t)length;
}
ssize_t process_vm_readv(pid_t pid, const struct iovec *local, unsigned long nlocal,
                         const struct iovec *remote, unsigned long nremote, unsigned long flags) {
    (void)pid; (void)flags;
    if (nlocal != 1 || nremote != 1 || local[0].iov_len < remote[0].iov_len) { errno = EINVAL; return -1; }
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)remote[0].iov_base,
                               remote[0].iov_len, (mach_vm_address_t)(uintptr_t)local[0].iov_base, &got) != KERN_SUCCESS) {
        errno = EFAULT;
        return -1;
    }
    return (ssize_t)got;
}
void bb_mac_close_from(int lowest) {
    struct rlimit limit;
    int top = getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < 65536 ? (int)limit.rlim_cur : 65536;
    for (int fd = lowest; fd < top; ++fd) close(fd);
}

/* ---- the runtime's heap below 1 TiB ----
 * 64 KiB pages over [BB_MAC_LOW_HEAP, BB_MAC_LOW_HEAP_END). A page holds blocks of one
 * power-of-two class (16 B to 32 KiB, so each block is aligned to its size), or is part of a
 * run of pages for one larger allocation. No headers: the page table says what a pointer is. */
#define HEAP_PAGE_SHIFT 16
#define HEAP_PAGE ((size_t)1 << HEAP_PAGE_SHIFT)
#define HEAP_PAGES ((size_t)((BB_MAC_LOW_HEAP_END - BB_MAC_LOW_HEAP) >> HEAP_PAGE_SHIFT))
#define CLASSES 12 /* 16 << 0 ... 16 << 11 = 32 KiB */
enum { PAGE_UNUSED = 0, PAGE_RUN_HEAD = 0x40, PAGE_RUN_TAIL = 0x80 };
static uint8_t page_kind[HEAP_PAGES];   /* 1 + class, or a run marker */
static uint32_t run_pages[HEAP_PAGES];  /* at a run head: its length in pages */
typedef struct FreeRun { struct FreeRun *next; size_t pages; } FreeRun;
static void *free_blocks[CLASSES];
static FreeRun *free_runs;
static size_t next_page;
static os_unfair_lock heap_lock = OS_UNFAIR_LOCK_INIT;

static int ours(const void *p) {
    return (uintptr_t)p >= BB_MAC_LOW_HEAP && (uintptr_t)p < BB_MAC_LOW_HEAP_END;
}
static size_t page_of(const void *p) { return ((uintptr_t)p - BB_MAC_LOW_HEAP) >> HEAP_PAGE_SHIFT; }
static void *page_address(size_t page) { return (void *)(uintptr_t)(BB_MAC_LOW_HEAP + (page << HEAP_PAGE_SHIFT)); }
static int class_of(size_t size) {
    int c = 0;
    while (c < CLASSES && ((size_t)16 << c) < size) ++c;
    return c;
}
/* Pages [page, page+count) made read-write (they start as reserved PROT_NONE). */
static int commit(size_t page, size_t count) {
    return mprotect(page_address(page), count << HEAP_PAGE_SHIFT, PROT_READ | PROT_WRITE) ? -1 : 0;
}
static long take_pages(size_t count) { /* caller holds the lock */
    for (FreeRun **at = &free_runs; *at; at = &(*at)->next) {
        FreeRun *r = *at;
        if (r->pages < count) continue;
        size_t page = page_of(r);
        if (r->pages == count) *at = r->next;
        else {
            FreeRun *rest = page_address(page + count);
            rest->next = r->next; rest->pages = r->pages - count;
            *at = rest;
        }
        return (long)page;
    }
    if (!reservation_ok || next_page + count > HEAP_PAGES) return -1;
    size_t page = next_page;
    if (commit(page, count)) return -1;
    next_page += count;
    return (long)page;
}
static void *heap_alloc(size_t size, size_t alignment) {
    if (!size) size = 1;
    if (alignment > size) size = alignment;
    int c = class_of(size);
    void *p = NULL;
    os_unfair_lock_lock(&heap_lock);
    if (c < CLASSES) {
        if (!free_blocks[c]) {
            long page = take_pages(1);
            if (page >= 0) {
                page_kind[page] = (uint8_t)(1 + c);
                size_t block = (size_t)16 << c;
                char *base = page_address((size_t)page);
                for (size_t off = HEAP_PAGE; off >= block; off -= block) {
                    void **b = (void **)(base + off - block);
                    *b = free_blocks[c]; free_blocks[c] = b;
                }
            }
        }
        if ((p = free_blocks[c])) free_blocks[c] = *(void **)p;
    } else if (alignment <= HEAP_PAGE) {
        size_t count = (size + HEAP_PAGE - 1) >> HEAP_PAGE_SHIFT;
        long page = take_pages(count);
        if (page >= 0) {
            page_kind[page] = PAGE_RUN_HEAD; run_pages[page] = (uint32_t)count;
            for (size_t i = 1; i < count; ++i) page_kind[page + i] = PAGE_RUN_TAIL;
            p = page_address((size_t)page);
        }
    }
    os_unfair_lock_unlock(&heap_lock);
    if (!p) errno = ENOMEM;
    return p;
}
static size_t heap_size(const void *p) {
    uint8_t kind = page_kind[page_of(p)];
    if (kind == PAGE_RUN_HEAD) return (size_t)run_pages[page_of(p)] << HEAP_PAGE_SHIFT;
    return (size_t)16 << (kind - 1);
}
void bb_low_free(void *p) {
    if (!p) return;
    if (!ours(p)) { free(p); return; }
    os_unfair_lock_lock(&heap_lock);
    size_t page = page_of(p);
    uint8_t kind = page_kind[page];
    if (kind == PAGE_RUN_HEAD) {
        size_t count = run_pages[page];
        for (size_t i = 0; i < count; ++i) page_kind[page + i] = PAGE_UNUSED;
        if (count > 1) madvise(page_address(page + 1), (count - 1) << HEAP_PAGE_SHIFT, MADV_FREE);
        FreeRun *r = p; r->pages = count; r->next = free_runs; free_runs = r;
    } else if (kind >= 1 && kind <= CLASSES) {
        *(void **)p = free_blocks[kind - 1]; free_blocks[kind - 1] = p;
    } else {
        os_unfair_lock_unlock(&heap_lock);
        fprintf(stderr, "STOP: free of %p, not a runtime heap block\n", p);
        abort();
    }
    os_unfair_lock_unlock(&heap_lock);
}
void *bb_low_malloc(size_t size) { return heap_alloc(size, 16); }
void *bb_low_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) { errno = ENOMEM; return NULL; }
    void *p = heap_alloc(count * size, 16);
    if (p) memset(p, 0, count * size);
    return p;
}
void *bb_low_realloc(void *p, size_t size) {
    if (!p) return bb_low_malloc(size);
    if (!size) { bb_low_free(p); return NULL; }
    size_t old = ours(p) ? heap_size(p) : malloc_size(p);
    if (ours(p) && size <= old && (old <= 16 || size > old / 2)) return p;
    void *q = heap_alloc(size, 16);
    if (!q) return NULL;
    memcpy(q, p, old < size ? old : size);
    bb_low_free(p);
    return q;
}
void *bb_low_aligned_alloc(size_t alignment, size_t size) {
    if (!alignment || (alignment & (alignment - 1)) || alignment > HEAP_PAGE) { errno = EINVAL; return NULL; }
    return heap_alloc(size, alignment);
}
int bb_low_posix_memalign(void **out, size_t alignment, size_t size) {
    void *p = bb_low_aligned_alloc(alignment < sizeof(void *) ? sizeof(void *) : alignment, size);
    if (!p) return errno;
    *out = p;
    return 0;
}
char *bb_low_strdup(const char *text) {
    size_t n = strlen(text) + 1;
    char *p = bb_low_malloc(n);
    if (p) memcpy(p, text, n);
    return p;
}
char *bb_low_strndup(const char *text, size_t length) {
    size_t n = strnlen(text, length);
    char *p = bb_low_malloc(n + 1);
    if (p) { memcpy(p, text, n); p[n] = 0; }
    return p;
}
#endif
