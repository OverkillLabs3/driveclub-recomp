/* Lock to horizon and camera shake (the launcher's Driving tab), DriveClub 1.28, Windows x86-64 only.

   BB_LOCK_HORIZON=1: the cockpit (driver's head) camera keeps the horizon level while the car rolls and pitches, as
   Assetto Corsa's option does; =2: the bonnet, bumper, dash and chase cameras too. BB_CAMERA_SHAKE=<0..100>: percent
   of the game's camera shake (100 as made).

   Cockpit: in the driver's head camera update (guest +0xb916d0, `this` in r15), at +0xb92420, +0x270..+0x2a0 hold the
   camera's world matrix the renderer draws from (just written by +0xb93510) and +0x2d0..+0x300 the car body's: rows of
   four floats, the x, y and z axis and the position, in metres; the axes point left, up and forward; world up is +y.
   Nothing later in the function writes +0x270 again.

   Other views: every vehicle camera goes through the base update +0xcc8820 (`this` in r12), which copies the camera
   class's matrix to +0x10..+0x4f (same rows) at +0xcc8a57, then lets the shake effects (+0x70 count, +0x78 array) work
   on that copy, which the main view reads. The class is told by its vtable: ATTACHED_CAMERA (bonnet, bumper, dash)
   image+0x1654250, CHASE_CAMERA image+0x1654ef0; the cockpit's DRIVER_HEAD_CAMERA image+0x1654e40 is left to the hook
   above. The car body's matrix is the first target's (`**(this+0x148)`, a vehicle when its +8 is 1) at +0xd0, refreshed
   earlier in the same call.

   A camera fixed to the car (cockpit, bonnet, bumper, dash) keeps its turn relative to the car (the head looking into
   corners, its sway on the game's springs, the view down the road) and has it put on the car levelled: its heading
   only, without roll and pitch. A chase camera follows the car's roll only partly (its ROLL_PERCENTAGE), so its own
   tilt is taken out instead; it still looks down at the car. Positions stay. The game's camera shake comes after both.

   What still tips a fixed camera then is the head swaying on the game's springs (its tilt and nod relative to the car).
   For motion sickness its tilt and nod are smoothed over horizon_smooth_s (0.25 s; BB_LOCK_HORIZON_SMOOTH in ms, 0 off):
   fast wobbles and jolts go, slow leaning stays. The heading is never smoothed, so the view does not lag the car's
   turns. Each camera (other cars have them too) keeps its own state.

   Camera shake: after the levelling, the base update applies each of the camera's shake effects to its matrix
   (+0xcc8bf1: `effects[i]->vtable[+0x10](effect, this+0x10)`, i in rbx). That call is wrapped: the effect runs as the
   game has it, and the matrix it leaves is moved back towards the one before it by the percentage. The effects' own
   state is not touched. */
#ifdef _WIN32
#include <math.h>
static __attribute__((used)) uintptr_t horizon_lock_resume, horizon_cameras_resume;
static int horizon_mode;               /* 0 off, 1 cockpit, 2 all views */
static float horizon_smooth_s=0.25f;
static void horizon_normalize(float v[3]) {
    const float n=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    if (n>1e-6f) { v[0]/=n; v[1]/=n; v[2]/=n; }
}
/* x = hand * (y cross z) for these rows: +1 or -1 */
static float horizon_hand(float r[3][3]) {
    return (r[1][1]*r[2][2]-r[1][2]*r[2][1])*r[0][0]+(r[1][2]*r[2][0]-r[1][0]*r[2][2])*r[0][1]+
           (r[1][0]*r[2][1]-r[1][1]*r[2][0])*r[0][2]<0.0f ? -1.0f : 1.0f;
}
/* Rows for forward f (unit) with tilt `roll` about it (0: up as near world up as it can be), handedness hand. */
static void horizon_rows(const float f[3], float roll, float hand, float out[3][3]) {
    float u0[3]={-f[1]*f[0],1.0f-f[1]*f[1],-f[1]*f[2]};
    horizon_normalize(u0);
    const float l0[3]={hand*(u0[1]*f[2]-u0[2]*f[1]),hand*(u0[2]*f[0]-u0[0]*f[2]),hand*(u0[0]*f[1]-u0[1]*f[0])};
    const float c=cosf(roll), s=sinf(roll);
    const float up[3]={c*u0[0]+s*l0[0],c*u0[1]+s*l0[1],c*u0[2]+s*l0[2]};
    for (int k=0;k<3;++k) { out[2][k]=f[k]; out[1][k]=up[k]; }
    out[0][0]=hand*(up[1]*f[2]-up[2]*f[1]);
    out[0][1]=hand*(up[2]*f[0]-up[0]*f[2]);
    out[0][2]=hand*(up[0]*f[1]-up[1]*f[0]);
}
/* A fixed camera's rows with their nod (pitch) and tilt (roll) smoothed; `key`: the camera. */
static void horizon_smooth(const void *key, float out[3][3], float hand) {
    static struct { const void *key; LONGLONG last; float pitch, roll; } state[64];
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const float *f=out[2];
    if (fabsf(f[1])>0.95f) return;   /* looking (nearly) straight up or down: no tilt to speak of */
    float level[3][3];
    horizon_rows(f,0.0f,hand,level);
    const float pitch=asinf(f[1]);
    const float roll=atan2f(out[1][0]*level[0][0]+out[1][1]*level[0][1]+out[1][2]*level[0][2],
                            out[1][0]*level[1][0]+out[1][1]*level[1][1]+out[1][2]*level[1][2]);
    int slot=-1, oldest=0;
    for (int i=0;i<64 && slot<0;++i) {
        if (state[i].key==key) slot=i;
        else if (state[i].last<state[oldest].last) oldest=i;
    }
    if (slot<0) { slot=oldest; state[slot].key=key; state[slot].last=0; }
    const float dt=state[slot].last ? (float)(now.QuadPart-state[slot].last)/(float)frequency.QuadPart : 1.0f;
    state[slot].last=now.QuadPart;
    if (dt>0.2f) { state[slot].pitch=pitch; state[slot].roll=roll; return; }   /* first call, or after a pause */
    const float k=1.0f-expf(-dt/horizon_smooth_s);
    state[slot].pitch+=(pitch-state[slot].pitch)*k;
    state[slot].roll+=(roll-state[slot].roll)*k;
    /* rebuilt from the heading as it is and the smoothed nod and tilt */
    const float horizontal=sqrtf(f[0]*f[0]+f[2]*f[2]), cp=cosf(state[slot].pitch);
    const float fw[3]={f[0]/horizontal*cp,sinf(state[slot].pitch),f[2]/horizontal*cp};
    horizon_rows(fw,state[slot].roll,hand,out);
}
/* A camera fixed to the car: rows cam (stride 4) put on the car (rows, stride 4) levelled. */
static void horizon_level(float *cam, const float *car, const void *key, const char *what) {
    float row[3][3];   /* the car's axes, unit length */
    for (int i=0;i<3;++i) { for (int k=0;k<3;++k) row[i][k]=car[i*4+k]; horizon_normalize(row[i]); }
    /* The car levelled: its forward axis on the ground plane, up straight up, left from those two with the car's own
       handedness. A car pointing (nearly) straight up or down has no heading: left as it is. */
    float level[3][3]={{0,0,0},{0,1,0},{row[2][0],0,row[2][2]}};
    if (level[2][0]*level[2][0]+level[2][2]*level[2][2]<0.01f) return;
    horizon_normalize(level[2]);
    const float hand=horizon_hand(row);
    level[0][0]=hand*(level[1][1]*level[2][2]-level[1][2]*level[2][1]);
    level[0][1]=hand*(level[1][2]*level[2][0]-level[1][0]*level[2][2]);
    level[0][2]=hand*(level[1][0]*level[2][1]-level[1][1]*level[2][0]);
    /* the camera's rows in the car's axes, then on the levelled car */
    float rel[3][3], out[3][3];
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) rel[i][j]=cam[i*4]*row[j][0]+cam[i*4+1]*row[j][1]+cam[i*4+2]*row[j][2];
    for (int i=0;i<3;++i) for (int k=0;k<3;++k) out[i][k]=rel[i][0]*level[0][k]+rel[i][1]*level[1][k]+rel[i][2]*level[2][k];
    if (horizon_smooth_s>0.0f) horizon_smooth(key,out,hand);
    for (int i=0;i<3;++i) for (int k=0;k<3;++k) cam[i*4+k]=out[i][k];
    static const char *logged[4];
    for (int i=0;i<4;++i) {
        if (logged[i]==what) break;
        if (!logged[i]) {
            logged[i]=what;
            printf("Runtime: lock to horizon: first %s camera, car up axis %.3f %.3f %.3f (world up is +y)\n",what,row[1][0],row[1][1],row[1][2]);
            break;
        }
    }
}
static __attribute__((used,noinline)) ABI void horizon_lock_compute(float *camera) {
    horizon_level(camera+0x270/4,camera+0x2d0/4,camera,"cockpit");
}
/* VR: where the bonnet, bumper, dash and chase cameras were just now (other cars have them too), so the
   headset can tell the main view is one of them (gpu/shim/bbport_vr.cpp, the scene camera hook). */
static struct { float position[3]; ULONGLONG time_ms; } vehicle_cameras[16];
static unsigned vehicle_camera_next;
int runtime_vehicle_camera_at(const float *position) {
    const ULONGLONG now=GetTickCount64();
    for (unsigned i=0;i<16;++i) {
        const float *p=vehicle_cameras[i].position;
        const float dx=p[0]-position[0], dy=p[1]-position[1], dz=p[2]-position[2];
        if (now-vehicle_cameras[i].time_ms<=300 && dx*dx+dy*dy+dz*dz<0.15f*0.15f) return 1;
    }
    return 0;
}
/* The other views, from the base camera update: `this` with its matrix at +0x10 (BB_LOCK_HORIZON=2 levels them). */
static __attribute__((used,noinline)) ABI void horizon_cameras_compute(unsigned char *camera) {
    uintptr_t vtable; memcpy(&vtable,camera,8);
    vtable-=(uintptr_t)image;
    const int attached=vtable==0x1654250, chase=vtable==0x1654ef0;
    if (!attached && !chase) return;
    float *cam=(float *)(camera+0x10);
    {
        const unsigned slot=vehicle_camera_next++%16;
        memcpy(vehicle_cameras[slot].position,cam+12,12);
        vehicle_cameras[slot].time_ms=GetTickCount64();
    }
    if (horizon_mode!=2) return;
    if (chase) {   /* its own tilt out; its view down at the car stays */
        float rows[3][3];
        for (int i=0;i<3;++i) for (int k=0;k<3;++k) rows[i][k]=cam[i*4+k];
        float f[3]={rows[2][0],rows[2][1],rows[2][2]};
        horizon_normalize(f);
        if (fabsf(f[1])>0.95f) return;
        float out[3][3];
        horizon_rows(f,0.0f,horizon_hand(rows),out);
        for (int i=0;i<3;++i) for (int k=0;k<3;++k) cam[i*4+k]=out[i][k];
        return;
    }
    uintptr_t begin, end, target;
    memcpy(&begin,camera+0x148,8); memcpy(&end,camera+0x150,8);
    if (begin<0x10000 || begin==end) return;
    memcpy(&target,(void *)begin,8);
    if (target<0x10000) return;
    uint32_t kind; memcpy(&kind,(void *)(target+8),4);
    if (kind!=1) return;   /* not a vehicle */
    horizon_level(cam,(const float *)(target+0xd0),camera,"bonnet/bumper");
}
/* Every register the game may still use and the flags are kept around the call. */
#define HORIZON_SAVE \
    "pushfq\n" "pushq %rax\n" "pushq %rcx\n" "pushq %rdx\n" "pushq %rsi\n" "pushq %rdi\n" "pushq %r8\n" "pushq %r9\n" \
    "pushq %r10\n" "pushq %r11\n" "subq $256, %rsp\n" \
    "vmovdqu %xmm0, 0(%rsp)\n" "vmovdqu %xmm1, 16(%rsp)\n" "vmovdqu %xmm2, 32(%rsp)\n" "vmovdqu %xmm3, 48(%rsp)\n" \
    "vmovdqu %xmm4, 64(%rsp)\n" "vmovdqu %xmm5, 80(%rsp)\n" "vmovdqu %xmm6, 96(%rsp)\n" "vmovdqu %xmm7, 112(%rsp)\n" \
    "vmovdqu %xmm8, 128(%rsp)\n" "vmovdqu %xmm9, 144(%rsp)\n" "vmovdqu %xmm10, 160(%rsp)\n" "vmovdqu %xmm11, 176(%rsp)\n" \
    "vmovdqu %xmm12, 192(%rsp)\n" "vmovdqu %xmm13, 208(%rsp)\n" "vmovdqu %xmm14, 224(%rsp)\n" "vmovdqu %xmm15, 240(%rsp)\n"
#define HORIZON_RESTORE \
    "vmovdqu 0(%rsp), %xmm0\n" "vmovdqu 16(%rsp), %xmm1\n" "vmovdqu 32(%rsp), %xmm2\n" "vmovdqu 48(%rsp), %xmm3\n" \
    "vmovdqu 64(%rsp), %xmm4\n" "vmovdqu 80(%rsp), %xmm5\n" "vmovdqu 96(%rsp), %xmm6\n" "vmovdqu 112(%rsp), %xmm7\n" \
    "vmovdqu 128(%rsp), %xmm8\n" "vmovdqu 144(%rsp), %xmm9\n" "vmovdqu 160(%rsp), %xmm10\n" "vmovdqu 176(%rsp), %xmm11\n" \
    "vmovdqu 192(%rsp), %xmm12\n" "vmovdqu 208(%rsp), %xmm13\n" "vmovdqu 224(%rsp), %xmm14\n" "vmovdqu 240(%rsp), %xmm15\n" \
    "addq $256, %rsp\n" \
    "popq %r11\n" "popq %r10\n" "popq %r9\n" "popq %r8\n" "popq %rdi\n" "popq %rsi\n" "popq %rdx\n" "popq %rcx\n" \
    "popq %rax\n" "popfq\n"
/* +0xb92420: the call, then the two instructions the jump replaced. */
static __attribute__((naked,used)) ABI void horizon_lock_bridge(void) {
    __asm__ volatile(
        HORIZON_SAVE
        "movq %r15, %rdi\n"
        "callq horizon_lock_compute\n"
        HORIZON_RESTORE
        "vmovdqa 0x310(%r15), %xmm0\n"
        "vpshufd $0, %xmm0, %xmm1\n"
        "jmpq *horizon_lock_resume(%rip)\n"
    );
}
/* +0xcc8a57: the matrix's last row stored as there, the call, then the two instructions after it. */
static __attribute__((naked,used)) ABI void horizon_cameras_bridge(void) {
    __asm__ volatile(
        "vmovaps %xmm0, 0x40(%r12)\n"
        HORIZON_SAVE
        "movq %r12, %rdi\n"
        "callq horizon_cameras_compute\n"
        HORIZON_RESTORE
        "movq (%r12), %rax\n"
        "movq %r12, %rdi\n"
        "jmpq *horizon_cameras_resume(%rip)\n"
    );
}
/* jmp [rip] with the bridge's address after it, over exactly 14 bytes of whole instructions: no register used. */
static int horizon_patch(size_t offset, const unsigned char expected[14], uintptr_t bridge, uintptr_t *resume) {
    if (runtime_image_size<offset+14 || memcmp(image+offset,expected,14)) return 0;
    *resume=(uintptr_t)image+offset+14;
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
    memcpy(image+offset,jump,6); memcpy(image+offset+6,&bridge,8);
    FlushInstructionCache(GetCurrentProcess(),image+offset,14);
    return 1;
}

/* ---- camera shake ------------------------------------------------------------------------------------------------ */
static __attribute__((used)) uintptr_t camera_shake_resume;
static float camera_shake=1.0f;   /* BB_CAMERA_SHAKE / 100 */
typedef void (ABI *CameraShakeApply)(void *effect, float *matrix);
/* One shake effect of camera's effect array (+0x78), applied as the game does (its vtable's +0x10, on the camera's
   matrix), then its change scaled: the matrix ends between the one before and the one the effect made. */
static __attribute__((used,noinline)) ABI void camera_shake_apply(unsigned char *camera, uint64_t index, float *matrix) {
    void **effects; memcpy(&effects,camera+0x78,8);
    void *effect=effects[(uint32_t)index];
    void **vtable; memcpy(&vtable,effect,8);
    const CameraShakeApply apply=(CameraShakeApply)vtable[2];
    float before[16]; memcpy(before,matrix,64);
    apply(effect,matrix);
    /* for the log: the largest change of the first 3000 effect calls (about a race's first minute) */
    static float largest; static int calls;
    if (calls<3000) {
        for (int i=0;i<16;++i) largest=fmaxf(largest,fabsf(matrix[i]-before[i]));
        if (++calls==3000) printf("Runtime: camera shake %d%%: the largest change of 3000 shake effect calls was %.4f\n",
                                  (int)lroundf(camera_shake*100.0f),largest);
    }
    for (int i=0;i<16;++i) matrix[i]=before[i]+(matrix[i]-before[i])*camera_shake;
}
/* +0xcc8be2: in place of the loop's effect call (`this` r12, index rbx, the matrix r15 = r12+0x10), back after it. */
static __attribute__((naked,used)) ABI void camera_shake_bridge(void) {
    __asm__ volatile(
        HORIZON_SAVE
        "movq %r12, %rdi\n"
        "movq %rbx, %rsi\n"
        "movq %r15, %rdx\n"
        "callq camera_shake_apply\n"
        HORIZON_RESTORE
        "jmpq *camera_shake_resume(%rip)\n"
    );
}

/* VR (gpu/shim/bbport_vr.cpp) is wanted as BbVr::Wanted() decides: BB_VR (not 0), or else vr.ini next to the game. */
static int horizon_vr_wanted(void) {
    const char *env=getenv("BB_VR");
    if (env && *env) return env[0]!='0';
    wchar_t path[MAX_PATH];
    const DWORD n=GetModuleFileNameW(NULL,path,MAX_PATH);
    if (!n || n>=MAX_PATH) return 0;
    wchar_t *slash=wcsrchr(path,L'\\');
    if (!slash || (size_t)(slash-path)+8>=MAX_PATH) return 0;
    wcscpy(slash+1,L"vr.ini");
    return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;
}

static void horizon_lock_install(void) {
    const char *mode=getenv("BB_LOCK_HORIZON"), *smooth=getenv("BB_LOCK_HORIZON_SMOOTH"), *shake=getenv("BB_CAMERA_SHAKE");
    horizon_mode=mode && (mode[0]=='1' || mode[0]=='2') && !mode[1] ? mode[0]-'0' : 0;
    if (smooth && *smooth) horizon_smooth_s=(float)atoi(smooth)/1000.0f;
    if (shake && *shake) camera_shake=(float)(atoi(shake)<0 ? 0 : atoi(shake)>100 ? 100 : atoi(shake))/100.0f;
    if (camera_shake<0.999f) {
        /* the 18 bytes of the effect call (mov rax,[r12+0x78]; mov rdi,[rax+rbx*8]; mov rax,[rdi]; mov rsi,r15;
           call [rax+0x10]): jmp [rip] to the bridge, then four bytes never reached */
        static const unsigned char call[18]={0x49,0x8b,0x44,0x24,0x78,0x48,0x8b,0x3c,0xd8,0x48,0x8b,0x07,0x4c,0x89,0xfe,0xff,0x50,0x10};
        const int fits=runtime_image_size>=0xcc8be2+sizeof call && !memcmp(image+0xcc8be2,call,sizeof call);
        if (fits) {
            camera_shake_resume=(uintptr_t)image+0xcc8be2+sizeof call;
            const uintptr_t bridge=(uintptr_t)camera_shake_bridge;
            static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
            memcpy(image+0xcc8be2,jump,6); memcpy(image+0xcc8be8,&bridge,8); memset(image+0xcc8bf0,0x90,4);
            FlushInstructionCache(GetCurrentProcess(),image+0xcc8be2,sizeof call);
        }
        printf("Runtime: camera shake %d%%%s\n",(int)lroundf(camera_shake*100.0f),fits ? "" : " (does not fit this game version: as made)");
    }
    if (!horizon_mode && !horizon_vr_wanted()) return;
    static const unsigned char head[14]={0xc4,0xc1,0x79,0x6f,0x87,0x10,0x03,0x00,0x00,   /* vmovdqa xmm0,[r15+0x310] */
                                         0xc5,0xf9,0x70,0xc8,0x00};                     /* vpshufd xmm1,xmm0,0 */
    static const unsigned char cameras[14]={0xc4,0xc1,0x78,0x29,0x44,0x24,0x40,   /* vmovaps [r12+0x40],xmm0 */
                                            0x49,0x8b,0x04,0x24,                  /* mov rax,[r12] */
                                            0x4c,0x89,0xe7};                      /* mov rdi,r12 */
    if (horizon_vr_wanted()) {
        /* VR head tracking hooks the same place (+0xb92420): the cockpit view is the headset's, the other views
           can still keep the horizon level (the base update leaves the driver's head camera alone). */
        const int others=horizon_patch(0xcc8a57,cameras,(uintptr_t)horizon_cameras_bridge,&horizon_cameras_resume);
        if (horizon_mode) printf("Runtime: lock to horizon: the cockpit view is left to VR head tracking%s\n",
                                 others && horizon_mode==2 ? "; the bonnet, bumper, dash and chase views keep the horizon level" : "");
        printf("Runtime: VR: the bonnet, bumper, dash and chase views %s\n",others ? "are known to the headset" : "do not fit this game version");
        return;
    }
    if (!horizon_patch(0xb92420,head,(uintptr_t)horizon_lock_bridge,&horizon_lock_resume)) {
        puts("Runtime: lock to horizon does not fit this game version; off");
        return;
    }
    const int others=horizon_mode==2 && horizon_patch(0xcc8a57,cameras,(uintptr_t)horizon_cameras_bridge,&horizon_cameras_resume);
    printf("Runtime: lock to horizon on: the cockpit view%s keeps the horizon level, smoothing %d ms\n",
           others ? ", bonnet, bumper, dash and chase views" : horizon_mode==2 ? " (the other views do not fit this game version)" : "",
           (int)lroundf(horizon_smooth_s*1000.0f));
}
#else
static void horizon_lock_install(void) {}
int runtime_vehicle_camera_at(const float *position) { (void)position; return 0; }
#endif
