/* DriveClub 1.28 replay seeking. */
#ifdef _WIN32
static const unsigned char replay_seek_counter_expected[]={
    0x41,0x38,0xc5,0x74,0x61,0x31,0xc0,0xc5,0xfa,0x10,0x4d,0xd4,0xc5,0xf8,0x2e,0xca,0xb9,0x00,0x00,0x00,
    0x00,0x74,0x0a,0x41,0x8b,0x8c,0x24,0x30,0x01,0x00,0x00,0xff,0xc9,0xc5,0xf0,0x57,0xc9,0xc5,0xf8,0x2e,
    0xc1,0x41,0x89,0x8c,0x24,0x30,0x01,0x00,0x00,0x74,0x0a,0x41,0x8b,0x84,0x24,0x34,0x01,0x00,0x00,0xff,
    0xc8,0x41,0x89,0x84,0x24,0x34,0x01,0x00,0x00,0x41,0x8b,0x8c,0x24,0x38,0x01,0x00,0x00,0xff,0xc1,0x41,
    0x89,0x8c,0x24,0x38,0x01,0x00,0x00,0xb8,0xcd,0xcc,0xcc,0xcc,0x48,0x0f,0xaf,0xc1,0x48,0xc1,0xe8,0x22,
    0xeb,0x0e,0x41,0xc7,0x84,0x24,0x38,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x31,0xc0,
};
static uint32_t replay_seek_count(void *self,unsigned forward,unsigned back,unsigned ticks) {
    uint32_t counters[3];memcpy(counters,(unsigned char *)self+0x130,sizeof counters);
    if(forward!=back) {
        counters[0]=forward ? counters[0]-ticks : 0;
        counters[1]=back ? counters[1]-ticks : 0;
        counters[2]+=ticks;
    } else counters[2]=0;
    memcpy((unsigned char *)self+0x130,counters,sizeof counters);
    return counters[2]/5;
}
typedef struct { void *self;double credit;unsigned direction; } ReplaySeekClock;
static __attribute__((used,noinline)) ABI uint32_t replay_seek_timers(void *self,unsigned forward,unsigned back) {
    static _Thread_local ReplaySeekClock clocks[16];
    static _Thread_local unsigned next;
    ReplaySeekClock *clock=NULL;
    for(unsigned i=0;i<16;++i) {
        if(!clocks[i].self)clocks[i].self=self;
        if(clocks[i].self==self){clock=&clocks[i];break;}
    }
    if(!clock){clock=&clocks[next++%16];*clock=(ReplaySeekClock){.self=self};}
    typedef float (ABI *RealDelta)(void);
    float dt=((RealDelta)(runtime_image_base+0x79690))();
    unsigned direction=forward!=back ? (forward ? 1 : 2) : 0;
    unsigned ticks=1;
    if(!direction || direction!=clock->direction)clock->credit=0;
    else if(!(dt>0))ticks=0;
    else {
        /* Keep the native one-update limit on slow frames without losing fractional time. */
        clock->credit+=dt>1.0/30.0 ? 1.0/30.0 : dt;
        ticks=(unsigned)((clock->credit+0.000001)/(1.0/30.0));
        clock->credit-=ticks*(1.0/30.0);
        if(clock->credit<0)clock->credit=0;
    }
    clock->direction=direction;
    return replay_seek_count(self,forward,back,ticks);
}
static __attribute__((used)) uintptr_t replay_seek_resume;
static __attribute__((naked,used)) ABI void replay_seek_bridge(void) {
    __asm__ volatile("movq %r12,%rdi\n movzbl %r13b,%esi\n movzbl %al,%edx\n"
                     "callq replay_seek_timers\n jmp *replay_seek_resume(%rip)\n");
}
typedef uintptr_t (ABI *ReplaySeekSetTime)(void *,int64_t);
static ReplaySeekSetTime replay_seek_set_time_original;
typedef void (ABI *ReplaySeekUpdate)(void *,void *,void *,void *);
static ReplaySeekUpdate replay_seek_update_original;
static _Thread_local void *replay_seek_active;
static ABI void replay_seek_update_trace(void *self,void *input,void *a,void *b) {
    void *previous=replay_seek_active;replay_seek_active=self;
    replay_seek_update_original(self,input,a,b);
    replay_seek_active=previous;
}
static ABI uintptr_t replay_seek_set_time_trace(void *self,int64_t target) {
    int64_t before,after,total;
    memcpy(&before,(unsigned char *)self+0xb0,8);
    uintptr_t result=replay_seek_set_time_original(self,target);
    memcpy(&after,(unsigned char *)self+0xb0,8);
    memcpy(&total,(unsigned char *)self+0xb8,8);
    uint32_t held=0,frame=0;int64_t dt=0;
    if(replay_seek_active) {
        memcpy(&held,(unsigned char *)replay_seek_active+0x138,4);
        uintptr_t root,manager,clock;
        memcpy(&root,image+0x1911100,8);memcpy(&manager,(void *)(root+0x970),8);memcpy(&clock,(void *)(manager+0x1e0),8);
        memcpy(&frame,(void *)(clock+0x218),4);memcpy(&dt,(void *)(clock+0x20),8);
    }
    printf("ReplaySeekTrace: wall_ms=%llu reader=%p before=%lld target=%lld after=%lld total=%lld caller=+0x%llx held=%u frame=%u dt_us=%lld\n",
        GetTickCount64(),self,(long long)before,(long long)target,(long long)after,(long long)total,
        (unsigned long long)((uintptr_t)__builtin_return_address(0)-runtime_image_base),held,frame,(long long)dt);
    return result;
}
static void replay_seek_install(void) {
    const size_t offset=0x10bed4;
    static const unsigned char delta[]={0x48,0x8d,0x05,0x69,0x7a,0x89,0x01,0x48,0x8b,0x00};
    if(runtime_image_size<0x1911108 ||
       memcmp(image+offset,replay_seek_counter_expected,sizeof replay_seek_counter_expected) ||
       memcmp(image+0x79690,delta,sizeof delta)) {
        puts("Replay seek: unsupported 1.28 signature; unchanged");return;
    }
    const char *trace=BB_DIAGNOSTIC_ENV("BB_REPLAY_SEEK_TRACE");
    if(trace && trace[0]=='1') {
        static const unsigned char prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50};
        replay_seek_set_time_original=(ReplaySeekSetTime)camera_rate_hook(0x762340,prefix,sizeof prefix,
            (uintptr_t)replay_seek_set_time_trace);
        replay_seek_update_original=(ReplaySeekUpdate)camera_rate_hook(0x10be80,prefix,sizeof prefix,
            (uintptr_t)replay_seek_update_trace);
        printf("ReplaySeekTrace: native playhead telemetry %s\n",replay_seek_set_time_original?"on":"unavailable");
    }
    const char *enabled=getenv("BB_REPLAY_SEEK_FIX");
    if(enabled && atoi(enabled)==0)return;
    /* The delta-time patch bypasses the game's native fixed-frame timing mode. */
    static const unsigned char variable_dt[]={0xb8,0x01,0x00,0x00,0x00};
    if(memcmp(image+0xd84800,variable_dt,sizeof variable_dt)) {
        puts("Replay seek: native frame timing unchanged");return;
    }
    replay_seek_resume=(uintptr_t)image+offset+sizeof replay_seek_counter_expected;
    const unsigned char jump[]={0xff,0x25,0,0,0,0};
    uintptr_t target=(uintptr_t)replay_seek_bridge;
    memcpy(image+offset,jump,sizeof jump);memcpy(image+offset+6,&target,8);
    memset(image+offset+14,0x90,sizeof replay_seek_counter_expected-14);
    FlushInstructionCache(GetCurrentProcess(),image+offset,sizeof replay_seek_counter_expected);
    puts("Replay seek: 30 Hz reference hold/repeat timing enabled");
}
#else
static void replay_seek_install(void) {}
#endif
