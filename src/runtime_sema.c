/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include "../gpu/shim/bbport_diagnostics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    int32_t need, result;
    int done;
    pthread_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    Waiter *first;
    struct Sema *next;
} Sema;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Sema *registry;
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out;
static Sema *find(uint32_t id) {
    for (Sema *s=registry;s;s=s->next) if (s->id==id) return s;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
/* Waits use this clock for their deadlines. winpthreads condition variables accept only
 * CLOCK_REALTIME (pthread_condattr_setclock(CLOCK_MONOTONIC) fails with EINVAL). */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    pthread_mutex_lock(&lock);
    if (next_id==UINT32_MAX) { pthread_mutex_unlock(&lock); free(s); return ERR(28); }
    s->id=next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    s->next=registry; registry=s; *out=s->id; ++created;
    printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",s->id,attr,initial,maximum);
    pthread_mutex_unlock(&lock);
    return 0;
}
extern __thread int runtime_thread_background;
/* Background library threads (WebAPI, wheel support) that poll a semaphore in vain over and over sleep a little
 * between polls instead of keeping a core busy. */
static void throttle_background(void) {
    if (!runtime_thread_background) return;
    compat_sleep_ns(2000000);
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); ++timed_out; }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        static int trace_sema=-1; if (trace_sema<0) trace_sema=BB_DIAGNOSTIC_ENV("BB_TRACE_SEMA")!=NULL; if (trace_sema) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,s->count); /* deadlock diagnosis */
        Waiter w={.need=need};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        while (!w.done) {
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--s->active && s->deleted) free(s);
    }
    pthread_mutex_unlock(&lock);
    if (result==ERR(16) || (result==ERR(60) && timeout && !*timeout)) throttle_background();
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) {
    runtime_diag_wait_begin("sema",id,__builtin_return_address(0));
    int32_t result=wait_count(id,need,timeout,1);
    runtime_diag_wait_end();
    return result;
}
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            host_check(pthread_cond_signal(&w->event));
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        host_check(pthread_cond_signal(&w->event));
    }
    return count;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t count,int32_t *waiters) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    pthread_mutex_lock(&lock);
    Sema **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Read-only test/diagnostic snapshot, not exposed to guest code. */
unsigned runtime_sema_waiters(uint32_t id) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id); unsigned n=0;
    if (s) for (Waiter *w=s->first;w;w=w->next) ++n;
    pthread_mutex_unlock(&lock);
    return n;
}
/* POSIX semaphores: the guest sem_t holds the registry id in its first four bytes. */
static int32_t sem_posix(int32_t r) { return r ? (int32_t)((uint32_t)r & 0xffff) : 0; }
static ABI int32_t psem_init(uint32_t *sem,int32_t shared,uint32_t value) {
    (void)shared; if (!sem || value>INT32_MAX) return 22;
    return sem_posix(sem_create(sem,"posix",0,(int32_t)value,INT32_MAX,NULL));
}
static ABI int32_t psem_destroy(uint32_t *sem) { return sem ? sem_posix(sem_delete(*sem)) : 22; }
static ABI int32_t psem_wait(uint32_t *sem) { return sem ? sem_posix(sem_wait(*sem,1,NULL)) : 22; }
static ABI int32_t psem_trywait(uint32_t *sem) { return sem ? (sem_poll(*sem,1) ? 35 : 0) : 22; }
static ABI int32_t psem_post(uint32_t *sem) { return sem ? sem_posix(sem_signal(*sem,1)) : 22; }
static ABI int32_t psem_timedwait(uint32_t *sem,const struct timespec *abs) {
    if (!sem || !abs) return 22;
    struct timespec now; clock_gettime(CLOCK_REALTIME,&now);
    int64_t us=((int64_t)abs->tv_sec-now.tv_sec)*1000000+((int64_t)abs->tv_nsec-now.tv_nsec)/1000;
    uint32_t timeout=us<0 ? 0 : us>UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    int32_t r=sem_wait(*sem,1,&timeout);
    return r ? 60 : 0; /* ETIMEDOUT */
}
uintptr_t runtime_sema_resolve(const char *name) {
    if (nid_match(name, "pDuPEf3m4fI#")) return (uintptr_t)psem_init;
    if (nid_match(name, "cDW233RAwWo#")) return (uintptr_t)psem_destroy;
    if (nid_match(name, "YCV5dGGBcCo#")) return (uintptr_t)psem_wait;
    if (nid_match(name, "WBWzsRifCEA#")) return (uintptr_t)psem_trywait;
    if (nid_match(name, "IKP8typ0QUk#")) return (uintptr_t)psem_post;
    if (nid_match(name, "w5IHyvahg-o#")) return (uintptr_t)psem_timedwait;
    if (nid_match(name, "188x57JYp0g#p#J")) return (uintptr_t)sem_create;
    if (nid_match(name, "Zxa0VhQVTsk#p#J")) return (uintptr_t)sem_wait;
    if (nid_match(name, "4czppHBiriw#p#J")) return (uintptr_t)sem_signal;
    if (nid_match(name, "12wOHk8ywb0#p#J")) return (uintptr_t)sem_poll;
    if (nid_match(name, "4DM06U2BNEY#p#J")) return (uintptr_t)sem_cancel;
    if (nid_match(name, "R1Jvn8bSCW8#p#J")) return (uintptr_t)sem_delete;
    return 0;
}
void runtime_sema_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    pthread_mutex_unlock(&lock);
}

