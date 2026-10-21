/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Based on Supermedo's bloodborne_pc (https://github.com/Supermedo/bloodborne_pc).
 * Modified for this port.
 */
/* Linux rwlocks behind PS4 pointer-to-handle ABI. The registry protects
   lifetime, static initialization and ownership; waiting happens outside it. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
typedef struct Holder {
    pthread_t thread;
    unsigned readers, writer, pending;
    struct Holder *next;
} Holder;
/* The lock itself is ours, not the host's: FreeBSD's rwlock lets a reader in while a writer is waiting, and the
 * game takes a read lock it already holds (recursive reads). A host lock that queues new readers behind a
 * waiting writer deadlocks there: the writer waits for the reader, the reader for the writer. */
typedef struct Rwlock {
    pthread_mutex_t gate;
    pthread_cond_t changed;
    unsigned active_readers;
    int active_writer;
    Holder *holders;
    unsigned inflight;
    struct Rwlock *next;
} Rwlock;
typedef struct { int64_t seconds, nanoseconds; } GuestTime;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static Rwlock *registry;
static size_t created, reads, writes, unlocks;
static int32_t error(int e) {
    if (!e) return 0;
    unsigned code;
    switch(e) {
    case EPERM: code=1; break;
    case EDEADLK: code=11; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case ETIMEDOUT: code=60; break;
    default: fprintf(stderr,"STOP: unmapped rwlock error %d\n",e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000)|code);
}
/* All helpers ending in _locked require registry_lock. */
static Rwlock *find_locked(Rwlock *handle) {
    for (Rwlock *r=registry; r; r=r->next) if (r==handle) return r;
    return NULL;
}
static int create_locked(Rwlock **out) {
    Rwlock *r=calloc(1,sizeof(*r));
    if (!r) return ENOMEM;
    int e=pthread_mutex_init(&r->gate,NULL);
    if (e) { free(r); return e; }
    e=pthread_cond_init(&r->changed,NULL);
    if (e) { pthread_mutex_destroy(&r->gate); free(r); return e; }
    r->next=registry; registry=r; *out=r; ++created; return 0;
}
static ABI int32_t rw_init(Rwlock **out, void *const *attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return error(EINVAL);
    if (attr) { fputs("STOP: non-default PS4 rwlock attributes are not implemented\n",stderr); exit(21); }
    pthread_mutex_lock(&registry_lock);
    /* POSIX allows uninitialized storage, so never read *out during init. */
    int e=create_locked(out);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static void prune_locked(Rwlock *r, Holder *h) {
    if (h->readers || h->writer || h->pending) return;
    Holder **link=&r->holders;
    while (*link!=h) link=&(*link)->next;
    *link=h->next; free(h);
}
enum Operation { READ, WRITE, TRY_READ, TRY_WRITE, TIMED_READ, TIMED_WRITE };
/* mode 0 waits, 1 only tries, 2 waits until the absolute deadline. A reader waits only for a writer that holds the
 * lock; a writer waits for every holder. */
static int native_acquire(Rwlock *r, int writer, int mode, const struct timespec *deadline) {
    int e=0, waited=0;
    pthread_mutex_lock(&r->gate);
    while (r->active_writer || (writer && r->active_readers)) {
        if (mode==1) { e=EBUSY; break; }
        if (!waited) { waited=1; runtime_diag_wait_begin("rwlock",(uint64_t)(uintptr_t)r,__builtin_return_address(0)); }
        if (mode==2) { e=pthread_cond_timedwait(&r->changed,&r->gate,deadline); if (e==ETIMEDOUT) break; e=0; }
        else pthread_cond_wait(&r->changed,&r->gate);
    }
    if (waited) runtime_diag_wait_end();
    if (!e) { if (writer) r->active_writer=1; else ++r->active_readers; }
    pthread_mutex_unlock(&r->gate);
    return e;
}
static int native_release(Rwlock *r) {
    pthread_mutex_lock(&r->gate);
    if (r->active_writer) r->active_writer=0;
    else if (r->active_readers) --r->active_readers;
    pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->gate);
    return 0;
}
static int32_t acquire(Rwlock **handle, enum Operation op, const GuestTime *time) {
    if (!handle) return error(EINVAL);
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;
    pthread_mutex_lock(&registry_lock);
    int e=0;
    if (!*handle) e=create_locked(handle);
    Rwlock *r=e ? NULL : find_locked(*handle);
    if (!r) { pthread_mutex_unlock(&registry_lock); return error(e ? e : EINVAL); }
    Holder *h=r->holders;
    while (h && !pthread_equal(h->thread,pthread_self())) h=h->next;
    if (h && (h->writer || (writer && h->readers))) {
        pthread_mutex_unlock(&registry_lock); return error(attempt ? EBUSY : EDEADLK);
    }
    if (h && h->readers==UINT_MAX) { pthread_mutex_unlock(&registry_lock); return error(EAGAIN); }
    if (!h) {
        h=calloc(1,sizeof(*h));
        if (!h) { pthread_mutex_unlock(&registry_lock); return error(ENOMEM); }
        h->thread=pthread_self(); h->next=r->holders; r->holders=h;
    }
    ++r->inflight; ++h->pending;
    pthread_mutex_unlock(&registry_lock);
    /* Timed operations first try the lock, matching POSIX timeout validation. */
    if (attempt || timed) e=native_acquire(r,writer,1,NULL);
    else e=native_acquire(r,writer,0,NULL);
    if (timed && e==EBUSY) {
        if (!time || time->nanoseconds<0 || time->nanoseconds>=1000000000) e=EINVAL;
        else {
            struct timespec deadline={.tv_sec=(time_t)time->seconds,.tv_nsec=(long)time->nanoseconds};
            e=native_acquire(r,writer,2,&deadline);
        }
    }
    pthread_mutex_lock(&registry_lock);
    --r->inflight; --h->pending;
    if (!e) {
        if (writer) { h->writer=1; ++writes; }
        else { ++h->readers; ++reads; }
    }
    prune_locked(r,h);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static ABI int32_t rw_read(Rwlock **r) { return acquire(r,READ,NULL); }
static ABI int32_t rw_write(Rwlock **r) { return acquire(r,WRITE,NULL); }
static ABI int32_t rw_tryread(Rwlock **r) { return acquire(r,TRY_READ,NULL); }
static ABI int32_t rw_trywrite(Rwlock **r) { return acquire(r,TRY_WRITE,NULL); }
static ABI int32_t rw_timedread(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_READ,t); }
static ABI int32_t rw_timedwrite(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_WRITE,t); }
static ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    pthread_mutex_lock(&registry_lock);
    Rwlock *r=find_locked(*handle);
    if (!r) { pthread_mutex_unlock(&registry_lock); return error(EINVAL); }
    Holder *h=r->holders;
    while (h && !pthread_equal(h->thread,pthread_self())) h=h->next;
    if (!h || (!h->readers && !h->writer)) { pthread_mutex_unlock(&registry_lock); return error(EPERM); }
    int e=native_release(r);
    if (!e) { if (h->writer) h->writer=0; else --h->readers; ++unlocks; }
    prune_locked(r,h);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    pthread_mutex_lock(&registry_lock);
    if (!*handle) { pthread_mutex_unlock(&registry_lock); return 0; }
    Rwlock *r=find_locked(*handle);
    if (!r) { pthread_mutex_unlock(&registry_lock); return error(EINVAL); }
    if (r->inflight || r->holders) { pthread_mutex_unlock(&registry_lock); return error(EBUSY); }
    int e=pthread_cond_destroy(&r->changed);
    if (!e) e=pthread_mutex_destroy(&r->gate);
    if (!e) {
        Rwlock **link=&registry;
        while (*link!=r) link=&(*link)->next;
        *link=r->next; free(r); *handle=(Rwlock *)(uintptr_t)1;
    }
    pthread_mutex_unlock(&registry_lock); return error(e);
}
/* POSIX variants return positive errno values and ignore attributes. */
static int32_t rw_posix(int32_t r) { return r ? (int32_t)((uint32_t)r & 0xffff) : 0; }
static ABI int32_t prw_init(Rwlock **o,void *attr) { (void)attr; return rw_posix(rw_init(o,NULL,NULL)); }
static ABI int32_t prw_destroy(Rwlock **r) { return rw_posix(rw_destroy(r)); }
static ABI int32_t prw_read(Rwlock **r) { return rw_posix(rw_read(r)); }
static ABI int32_t prw_write(Rwlock **r) { return rw_posix(rw_write(r)); }
static ABI int32_t prw_tryread(Rwlock **r) { return rw_posix(rw_tryread(r)); }
static ABI int32_t prw_trywrite(Rwlock **r) { return rw_posix(rw_trywrite(r)); }
static ABI int32_t prw_unlock(Rwlock **r) { return rw_posix(rw_unlock(r)); }
static ABI int32_t prw_timedread(Rwlock **r,const GuestTime *t) { return rw_posix(rw_timedread(r,t)); }
static ABI int32_t prw_timedwrite(Rwlock **r,const GuestTime *t) { return rw_posix(rw_timedwrite(r,t)); }
uintptr_t runtime_rwlock_resolve(const char *name) {
    if (nid_match(name, "ytQULN-nhL4#")) return (uintptr_t)prw_init;
    if (nid_match(name, "1471ajPzxh0#")) return (uintptr_t)prw_destroy;
    if (nid_match(name, "iGjsr1WAtI0#")) return (uintptr_t)prw_read;
    if (nid_match(name, "sIlRvQqsN2Y#")) return (uintptr_t)prw_write;
    if (nid_match(name, "SFxTMOfuCkE#")) return (uintptr_t)prw_tryread;
    if (nid_match(name, "XhWHn6P5R7U#")) return (uintptr_t)prw_trywrite;
    if (nid_match(name, "EgmLo6EWgso#")) return (uintptr_t)prw_unlock;
    if (nid_match(name, "lb8lnYo-o7k#")) return (uintptr_t)prw_timedread;
    if (nid_match(name, "9zklzAl9CGM#")) return (uintptr_t)prw_timedwrite;
    if (nid_match(name, "6ULAa0fq4jA#p#J")) return (uintptr_t)rw_init;
    if (nid_match(name, "BB+kb08Tl9A#p#J")) return (uintptr_t)rw_destroy;
    if (nid_match(name, "Ox9i0c7L5w0#p#J")) return (uintptr_t)rw_read;
    if (nid_match(name, "mqdNorrB+gI#p#J")) return (uintptr_t)rw_write;
    if (nid_match(name, "XD3mDeybCnk#p#J")) return (uintptr_t)rw_tryread;
    if (nid_match(name, "bIHoZCTomsI#p#J")) return (uintptr_t)rw_trywrite;
    if (nid_match(name, "+L98PIbGttk#p#J")) return (uintptr_t)rw_unlock;
    if (nid_match(name, "iPtZRWICjrM#p#J")) return (uintptr_t)rw_timedread;
    if (nid_match(name, "adh--6nIqTk#p#J")) return (uintptr_t)rw_timedwrite;
    return 0;
}
/* For the freeze report: every lock somebody waits for, with who holds it. */
void runtime_rwlock_dump(void *file) {
    FILE *f=(FILE *)file;
    pthread_mutex_lock(&registry_lock);
    for (Rwlock *r=registry; r; r=r->next) {
        if (!r->inflight) continue;
        fprintf(f,"  rwlock %p: %u waiting; active readers %u, writer %d;",(void *)r,r->inflight,r->active_readers,r->active_writer);
        for (Holder *h=r->holders; h; h=h->next)
            if (h->readers || h->writer) fprintf(f," holder %llx (%u read%s)",(unsigned long long)(uintptr_t)h->thread,h->readers,h->writer ? ", write" : "");
        fputc(10,f);
    }
    pthread_mutex_unlock(&registry_lock);
    fflush(f);
}
void runtime_rwlock_report(void) {
    pthread_mutex_lock(&registry_lock);
    printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu\n",created,reads,writes,unlocks);
    pthread_mutex_unlock(&registry_lock);
}

