typedef void (ABI *CameraRateUpdate)(void *, unsigned, void *, float, float);
typedef float (ABI *CameraRateImpulse)(void *, float);
typedef void (ABI *CameraNoiseUpdate)(void *, float);
static CameraRateUpdate camera_rate_update_original;

static CameraNoiseUpdate camera_noise_update_original;
static _Thread_local float camera_rate_dt;
static _Thread_local unsigned camera_rate_active;
static _Thread_local unsigned camera_noise_fixed_step;
#include "runtime_camera_patch_spring.inc"
typedef struct { void *self; double credit; ULONGLONG last; } CameraNoiseClock;


static ABI void camera_rate_update(void *self, unsigned index, void *matrix, float dt, float real_dt) {
    float previous=camera_rate_dt;
    unsigned active=camera_rate_active;
    void *previous_dynamics=camera_dynamics_self;
    camera_dynamics_self=camera_dynamics_select(self);
    camera_rate_dt=dt; camera_rate_active=1;
    camera_rate_update_original(self,index,matrix,dt,real_dt);
    if(camera_accuracy_trace_active()){
    float camera_tape[24]={0};unsigned kind=0,frame=0;
    camera_pose_read((uintptr_t)self+0x10,camera_tape,64);camera_pose_read((uintptr_t)self+0x50,camera_tape+16,24);
    camera_pose_read((uintptr_t)self+0xc,&kind,4);camera_pose_read((uintptr_t)self+0xe8,&frame,4);
    camera_tape[22]=(float)kind;camera_tape[23]=real_dt;
    camera_accuracy_log(6,self,index,frame,dt,camera_tape,24);
    }
    camera_reference_input.ready=0;
    camera_rate_dt=previous; camera_rate_active=active;
    camera_dynamics_self=previous_dynamics;
}

#include "runtime_camera_noise_reference.inc"
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

#include "runtime_camera_head_angular_tape.inc"
#include "runtime_camera_head_kernel_tape.inc"
