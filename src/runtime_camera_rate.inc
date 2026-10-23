typedef void (ABI *CameraRateUpdate)(void *, unsigned, void *, float, float);
typedef float (ABI *CameraRateImpulse)(void *, float);
typedef void (ABI *CameraNoiseUpdate)(void *, float);
static CameraRateUpdate camera_rate_update_original;
static CameraRateImpulse camera_rate_impulse_original;
static CameraNoiseUpdate camera_noise_update_original;
static _Thread_local float camera_rate_dt;
static _Thread_local unsigned camera_rate_active;
static _Thread_local unsigned camera_noise_fixed_step;
#include "runtime_camera_patch_spring.inc"
typedef struct { void *self; double credit; ULONGLONG last; } CameraNoiseClock;
static unsigned camera_noise_ticks(double *credit, float dt) {
    if (!(dt>0) || dt>0.25f) return 0;
    const double period=1.0/30.0;
    *credit+=dt;
    unsigned ticks=(unsigned)((*credit+0.000001)/period);
    *credit-=ticks*period;
    if (*credit<0) *credit=0;
    return ticks;
}
static int camera_noise_caller(uintptr_t caller) {
    /* Random mode of the low-frequency effect (including its tail call),
       and six continuous high-frequency noise axes. Event impulses bypass this. */
    return caller==0xce30b8 || caller==0xce30c6 || caller==0xce3d8d ||
           caller==0xce3852 || caller==0xce387d || caller==0xce38aa ||
           caller==0xce38d7 || caller==0xce3904 || caller==0xce3931;
}
static ABI void camera_rate_update(void *self, unsigned index, void *matrix, float dt, float real_dt) {
    float previous=camera_rate_dt;
    unsigned active=camera_rate_active;
    void *previous_dynamics=camera_dynamics_self;
    camera_dynamics_self=camera_dynamics_select(self);
    camera_rate_dt=dt; camera_rate_active=1;
    camera_rate_update_original(self,index,matrix,dt,real_dt);
    camera_reference_input.ready=0;
    camera_rate_dt=previous; camera_rate_active=active;
    camera_dynamics_self=previous_dynamics;
}
static ABI float camera_rate_impulse(void *self, float magnitude) {
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-runtime_image_base;
    if (camera_noise_fixed_step || !camera_rate_active || !camera_noise_caller(caller))
        return camera_rate_impulse_original(self,magnitude);
    static _Thread_local CameraNoiseClock clocks[256];
    CameraNoiseClock *clock=NULL;
    for (unsigned i=0;i<256;++i) {
        if (!clocks[i].self) clocks[i].self=self;
        if (clocks[i].self==self) { clock=&clocks[i]; break; }
    }
    if (!clock) return camera_rate_impulse_original(self,magnitude);
    ULONGLONG now=GetTickCount64();
    if (now-clock->last>250) clock->credit=0;
    clock->last=now;
    unsigned ticks=camera_noise_ticks(&clock->credit,camera_rate_dt);
    float velocity; memcpy(&velocity,(unsigned char *)self+0x10,4);
    for (unsigned i=0;i<ticks;++i)
        velocity=camera_rate_impulse_original(self,magnitude);
    return velocity;
}
typedef struct {
    void *self;
    double credit;
    ULONGLONG last;
    float previous[16], current[16];
} CameraNoisePose;
static ABI void camera_noise_update(void *self, float dt) {
    if (!(dt>0) || dt>0.25f) { camera_noise_update_original(self,dt); return; }
    static _Thread_local CameraNoisePose poses[64];
    CameraNoisePose *p=NULL;
    for (unsigned i=0;i<64;++i) {
        if (!poses[i].self) {
            poses[i].self=self;
            memcpy(poses[i].current,(unsigned char *)self+0x160,64);
            memcpy(poses[i].previous,poses[i].current,64);
        }
        if (poses[i].self==self) { p=&poses[i]; break; }
    }
    if (!p) { camera_noise_update_original(self,dt); return; }
    ULONGLONG now=GetTickCount64();
    if (now-p->last>250) {
        p->credit=0;
        memcpy(p->current,(unsigned char *)self+0x160,64);
        memcpy(p->previous,p->current,64);
    }
    p->last=now;
    unsigned ticks=camera_noise_ticks(&p->credit,dt);
    unsigned previous_fixed_step=camera_noise_fixed_step;
    camera_noise_fixed_step=1;
    for (unsigned i=0;i<ticks;++i) {
        /* Restore the actual simulation output before advancing it; the matrix
           shown between ticks must never feed back into the noise simulation. */
        memcpy((unsigned char *)self+0x160,p->current,64);
        memcpy(p->previous,p->current,64);
        camera_noise_update_original(self,1.0f/30.0f);
        memcpy(p->current,(unsigned char *)self+0x160,64);
    }
    camera_noise_fixed_step=previous_fixed_step;
    float alpha=(float)(p->credit*30.0);
    if (alpha>1) alpha=1;
    float interpolated[16];
    /* This effect contains small shake rotations, not the world camera pose. */
    for (unsigned i=0;i<16;++i)
        interpolated[i]=p->previous[i]+(p->current[i]-p->previous[i])*alpha;
    memcpy((unsigned char *)self+0x160,interpolated,64);
}
static void *camera_rate_hook(size_t offset, const unsigned char *expected, size_t count, uintptr_t target) {
    if (runtime_image_size<offset+count || memcmp(image+offset,expected,count)) return NULL;
    unsigned char *code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!code) return NULL;
    memcpy(code,expected,count);
    /* RAX is scratch at both selected entries, and neither copied span reads it.
       The 12-byte entry jump avoids relocating the impulse's following RIP load. */
    uintptr_t resume=(uintptr_t)image+offset+count;
    const unsigned char jump[2]={0x48,0xb8}, tail[2]={0xff,0xe0};
    memcpy(code+count,jump,2); memcpy(code+count+2,&resume,8); memcpy(code+count+10,tail,2);
    DWORD old;
    if (!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)) { VirtualFree(code,0,MEM_RELEASE); return NULL; }
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    memcpy(image+offset,jump,2); memcpy(image+offset+2,&target,8); memcpy(image+offset+10,tail,2);
    memset(image+offset+12,0x90,count-12);
    FlushInstructionCache(GetCurrentProcess(),image+offset,count);
    return code;
}
