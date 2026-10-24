/* 1.28 resource manager: the entry of +0x77e7b0 without its deadlock (#185: the car select froze while scrolling
 * through cars, the music played on). BB_RESOURCE_ENTRY_FIX=0 disables it.
 *
 * The manager (the object at +0x274b110) lets one thread at a time work inside it: +0x104 counts the entries, +0x108
 * is the thread inside, +0x100 the state (0 idle, 1 cleanup pending, 2/3 the same with a thread inside, 4 cleanup
 * running on the thread at +0x110). Four places start the cleanup with a compare-exchange of the state from 1 to 4
 * (+0x77b951, +0x77b5d6, +0x77cf6a, +0x77d258), without the entry lock (+0x200). The entry checks for a running cleanup
 * before taking that lock, then counts itself in and moves the state 0->2 or 1->3 under it. A cleanup that starts
 * between the two finds state 1 and wins, and the entering thread goes in while it runs. When the cleanup then destroys
 * an object whose destructor enters the manager, it waits for that thread to leave (+0x77e876), and that thread, inside,
 * enters again and waits for the cleanup to end (+0x77e82a): both hang for good.
 * Here the entry is the game's, with one more check under the entry lock once the thread has counted itself in: if
 * the state is 4 then, a cleanup on another thread started in that gap, and the thread counts itself out again (the
 * manager is as before it came), waits for the cleanup to end and enters again. The cleanup's own entries (from the
 * destructors it runs) go in as before. */
#ifdef _WIN32
typedef int64_t (ABI *ResourceSelf)(void);
typedef int (ABI *ResourceMutex)(void *);
typedef int (ABI *ResourceSleep)(unsigned);
static ResourceSelf resource_self;
static ResourceMutex resource_lock, resource_unlock;
static ResourceSleep resource_sleep;
static int64_t resource_cleaner(unsigned char *m) {
    void *mutex=*(void **)(m+0x208);
    resource_lock(mutex);
    const int64_t thread=__atomic_load_n((int64_t *)(m+0x110),__ATOMIC_ACQUIRE);
    resource_unlock(mutex);
    return thread;
}
/* r13 (the manager) in rdi; returns with the thread counted in, as +0x77e8ce expects */
static ABI void resource_enter(unsigned char *m) {
    int *state=(int *)(m+0x100), *count=(int *)(m+0x104);
    int64_t *owner=(int64_t *)(m+0x108);
    void *entry=*(void **)(m+0x200);
    const int64_t self=resource_self();
    for (;;) {
        /* A thread already inside never waits for a cleanup: that cleanup may be waiting for it to leave. (With the
         * check below none starts while a thread is inside; this keeps the freeze away should one still do.) */
        const int inside=__atomic_load_n(count,__ATOMIC_ACQUIRE)!=0 && __atomic_load_n(owner,__ATOMIC_ACQUIRE)==self;
        if (!inside && __atomic_load_n(state,__ATOMIC_ACQUIRE)==4 && resource_cleaner(m)!=self)
            while (__atomic_load_n(state,__ATOMIC_ACQUIRE)==4) resource_sleep(1);
        resource_lock(entry);
        if (__atomic_load_n(count,__ATOMIC_ACQUIRE)!=0 && self!=__atomic_load_n(owner,__ATOMIC_ACQUIRE)) {
            do {
                resource_unlock(entry); resource_sleep(1); resource_lock(entry);
            } while (__atomic_load_n(owner,__ATOMIC_ACQUIRE)!=0);
            __atomic_store_n(owner,self,__ATOMIC_RELEASE);
        }
        const int was=__atomic_fetch_add(count,1,__ATOMIC_SEQ_CST);
        if (was==0) {
            int from=0; __atomic_compare_exchange_n(state,&from,2,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
            from=1; __atomic_compare_exchange_n(state,&from,3,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
            __atomic_store_n(owner,self,__ATOMIC_RELEASE);
            /* the fix: a cleanup on another thread started between the check above and this count */
            if (__atomic_load_n(state,__ATOMIC_SEQ_CST)==4 && resource_cleaner(m)!=self) {
                __atomic_fetch_sub(count,1,__ATOMIC_SEQ_CST);
                __atomic_store_n(owner,0,__ATOMIC_RELEASE);
                resource_unlock(entry);
                static unsigned n; if (n++<8) printf("ResourceEntry: a cleanup started as a thread entered; it waits for it\n");
                continue;
            }
        }
        resource_unlock(entry);
        return;
    }
}
static void resource_entry_install(void) {
    const char *enabled=getenv("BB_RESOURCE_ENTRY_FIX");
    if (enabled && atoi(enabled)==0) return;
    /* +0x77e7d5..+0x77e8ce: lea rbx,[r13+0x100], then the game's entry up to the store of the owner */
    static const unsigned char expected[]={
        0x49,0x8d,0x9d,0x00,0x01,0x00,0x00,0x41,0x8b,0x85,0x00,0x01,0x00,0x00,0x83,0xf8,0x04,0x75,0x49,0xe8,0x93,0xff,
        0x02,0x00,0x48,0x89,0x5d,0xa8,0x49,0x89,0xc7,0x49,0x8b,0x9d,0x08,0x02,0x00,0x00,0x48,0x89,0xdf,0xe8,0xed,0x1a,
        0xac,0x00,0x4d,0x8b,0xa5,0x10,0x01,0x00,0x00,0x48,0x89,0xdf,0xe8,0xbe,0x1a,0xac,0x00,0x4d,0x39,0xe7,0x48,0x8b,
        0x5d,0xa8,0x74,0x16,0x0f,0x1f,0x44,0x00,0x00,0xbf,0x01,0x00,0x00,0x00,0xe8,0x36,0x60,0xee,0xff,0x8b,0x03,0x83,
        0xf8,0x04,0x74,0xef,0x49,0x8b,0xbd,0x00,0x02,0x00,0x00,0xe8,0xb3,0x1a,0xac,0x00,0x41,0x83,0xbd,0x04,0x01,0x00,
        0x00,0x00,0x74,0x4c,0xe8,0x34,0xff,0x02,0x00,0x49,0x89,0xc7,0x4d,0x3b,0xbd,0x08,0x01,0x00,0x00,0x74,0x3b,0x0f,
        0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x49,0x8b,0xbd,0x00,0x02,0x00,0x00,0xe8,0x64,0x1a,0xac,0x00,0xbf,0x01,0x00,
        0x00,0x00,0xe8,0xea,0x5f,0xee,0xff,0x49,0x8b,0xbd,0x00,0x02,0x00,0x00,0xe8,0x6e,0x1a,0xac,0x00,0x49,0x83,0xbd,
        0x08,0x01,0x00,0x00,0x00,0x75,0xd4,0x4d,0x89,0xbd,0x08,0x01,0x00,0x00,0x4d,0x8d,0xa5,0x04,0x01,0x00,0x00,0xb8,
        0x01,0x00,0x00,0x00,0xf0,0x41,0x0f,0xc1,0x04,0x24,0x85,0xc0,0x75,0x25,0xb9,0x02,0x00,0x00,0x00,0x31,0xc0,0xf0,
        0x0f,0xb1,0x0b,0xb9,0x03,0x00,0x00,0x00,0xb8,0x01,0x00,0x00,0x00,0xf0,0x0f,0xb1,0x0b,0xe8,0xb9,0xfe,0x02,0x00,
        0x49,0x89,0x85,0x08,0x01,0x00,0x00};
    const size_t at=0x77e7d5, patch=0x77e7dc, resume=0x77e8ce;
    if (runtime_image_size<0x1240600 || memcmp(image+at,expected,sizeof expected)) {
        puts("ResourceEntry: unsupported signature; fix skipped"); return;
    }
    resource_self=(ResourceSelf)(image+0x7ae780);       /* the game's current-thread call */
    resource_lock=(ResourceMutex)(image+0x12402f0);     /* the mutex lock and unlock the entry uses */
    resource_unlock=(ResourceMutex)(image+0x12402d0);
    resource_sleep=(ResourceSleep)(image+0x664860);     /* its usleep */
    /* mov rdi,r13; movabs rax,resource_enter; call rax; lea r12,[r13+0x104]; jmp +0x77e8ce; the rest int3 */
    unsigned char *c=image+patch;
    memset(c,0xcc,resume-patch);
    const unsigned char head[]={0x4c,0x89,0xef,0x48,0xb8};
    memcpy(c,head,sizeof head); c+=sizeof head;
    const uintptr_t fn=(uintptr_t)resource_enter; memcpy(c,&fn,8); c+=8;
    const unsigned char tail[]={0xff,0xd0,0x4d,0x8d,0xa5,0x04,0x01,0x00,0x00,0xe9};
    memcpy(c,tail,sizeof tail); c+=sizeof tail;
    const int32_t rel=(int32_t)((image+resume)-(c+4)); memcpy(c,&rel,4);
    FlushInstructionCache(GetCurrentProcess(),image+patch,resume-patch);
    puts("ResourceEntry: resource manager entry waits out a cleanup that starts as it enters (car select freeze)");
}
#else
static void resource_entry_install(void) {}
#endif
