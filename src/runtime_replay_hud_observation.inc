typedef struct {float dt;int prepared;float accel[4];} ReplayHudObservation;
static _Thread_local ReplayHudObservation replay_hud_observation;
static __attribute__((used)) uintptr_t replay_hud_observation_resume1,replay_hud_observation_resume2;
static __attribute__((used,noinline)) ABI void replay_hud_observation_compute(void *state,float *out) {
    if(!replay_hud_observation.prepared) {
        replay_hud_observation.prepared=1;
        camera_pose_read((uintptr_t)state+0xb0,replay_hud_observation.accel,16);
        float derivative[6];
        if(replay_recorded_read((uintptr_t)state,replay_hud_observation.dt,derivative))
            memcpy(replay_hud_observation.accel,derivative,12);
    }
    memcpy(out,replay_hud_observation.accel,16);
}
static __attribute__((naked,used)) ABI void replay_hud_observation_bridge1(void) {
    __asm__ volatile(
        "pushfq\n"
        "pushq %rax\n"
        "pushq %rcx\n"
        "pushq %rdx\n"
        "pushq %rsi\n"
        "pushq %rdi\n"
        "pushq %r8\n"
        "pushq %r9\n"
        "pushq %r10\n"
        "pushq %r11\n"
        "subq $272, %rsp\n"
        "vmovdqu %xmm0, 0(%rsp)\n"
        "vmovdqu %xmm1, 16(%rsp)\n"
        "vmovdqu %xmm2, 32(%rsp)\n"
        "vmovdqu %xmm3, 48(%rsp)\n"
        "vmovdqu %xmm4, 64(%rsp)\n"
        "vmovdqu %xmm5, 80(%rsp)\n"
        "vmovdqu %xmm6, 96(%rsp)\n"
        "vmovdqu %xmm7, 112(%rsp)\n"
        "vmovdqu %xmm8, 128(%rsp)\n"
        "vmovdqu %xmm9, 144(%rsp)\n"
        "vmovdqu %xmm10, 160(%rsp)\n"
        "vmovdqu %xmm11, 176(%rsp)\n"
        "vmovdqu %xmm12, 192(%rsp)\n"
        "vmovdqu %xmm13, 208(%rsp)\n"
        "vmovdqu %xmm14, 224(%rsp)\n"
        "vmovdqu %xmm15, 240(%rsp)\n"
        "movq 0x10(%r14), %rdi\n"
        "leaq 256(%rsp), %rsi\n"
        "callq replay_hud_observation_compute\n"
        "vmovdqu 0(%rsp), %xmm0\n"
        "vmovdqu 16(%rsp), %xmm1\n"
        "vmovdqu 32(%rsp), %xmm2\n"
        "vmovdqu 48(%rsp), %xmm3\n"
        "vmovdqu 64(%rsp), %xmm4\n"
        "vmovdqu 80(%rsp), %xmm5\n"
        "vmovdqu 96(%rsp), %xmm6\n"
        "vmovdqu 112(%rsp), %xmm7\n"
        "vmovdqu 128(%rsp), %xmm8\n"
        "vmovdqu 144(%rsp), %xmm9\n"
        "vmovdqu 160(%rsp), %xmm10\n"
        "vmovdqu 176(%rsp), %xmm11\n"
        "vmovdqu 192(%rsp), %xmm12\n"
        "vmovdqu 208(%rsp), %xmm13\n"
        "vmovdqu 224(%rsp), %xmm14\n"
        "vmovdqu 240(%rsp), %xmm15\n"
        "vmovaps 256(%rsp), %xmm2\n"
        "addq $272, %rsp\n"
        "popq %r11\n"
        "popq %r10\n"
        "popq %r9\n"
        "popq %r8\n"
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rdx\n"
        "popq %rcx\n"
        "popq %rax\n"
        "popfq\n"
        "movq 0x10(%r14), %rax\n"
        "jmpq *replay_hud_observation_resume1(%rip)\n"
    );
}
static __attribute__((naked,used)) ABI void replay_hud_observation_bridge2(void) {
    __asm__ volatile(
        "pushfq\n"
        "pushq %rax\n"
        "pushq %rcx\n"
        "pushq %rdx\n"
        "pushq %rsi\n"
        "pushq %rdi\n"
        "pushq %r8\n"
        "pushq %r9\n"
        "pushq %r10\n"
        "pushq %r11\n"
        "subq $272, %rsp\n"
        "vmovdqu %xmm0, 0(%rsp)\n"
        "vmovdqu %xmm1, 16(%rsp)\n"
        "vmovdqu %xmm2, 32(%rsp)\n"
        "vmovdqu %xmm3, 48(%rsp)\n"
        "vmovdqu %xmm4, 64(%rsp)\n"
        "vmovdqu %xmm5, 80(%rsp)\n"
        "vmovdqu %xmm6, 96(%rsp)\n"
        "vmovdqu %xmm7, 112(%rsp)\n"
        "vmovdqu %xmm8, 128(%rsp)\n"
        "vmovdqu %xmm9, 144(%rsp)\n"
        "vmovdqu %xmm10, 160(%rsp)\n"
        "vmovdqu %xmm11, 176(%rsp)\n"
        "vmovdqu %xmm12, 192(%rsp)\n"
        "vmovdqu %xmm13, 208(%rsp)\n"
        "vmovdqu %xmm14, 224(%rsp)\n"
        "vmovdqu %xmm15, 240(%rsp)\n"
        "movq 0x10(%r14), %rdi\n"
        "leaq 256(%rsp), %rsi\n"
        "callq replay_hud_observation_compute\n"
        "vmovdqu 0(%rsp), %xmm0\n"
        "vmovdqu 16(%rsp), %xmm1\n"
        "vmovdqu 32(%rsp), %xmm2\n"
        "vmovdqu 48(%rsp), %xmm3\n"
        "vmovdqu 64(%rsp), %xmm4\n"
        "vmovdqu 80(%rsp), %xmm5\n"
        "vmovdqu 96(%rsp), %xmm6\n"
        "vmovdqu 112(%rsp), %xmm7\n"
        "vmovdqu 128(%rsp), %xmm8\n"
        "vmovdqu 144(%rsp), %xmm9\n"
        "vmovdqu 160(%rsp), %xmm10\n"
        "vmovdqu 176(%rsp), %xmm11\n"
        "vmovdqu 192(%rsp), %xmm12\n"
        "vmovdqu 208(%rsp), %xmm13\n"
        "vmovdqu 224(%rsp), %xmm14\n"
        "vmovdqu 240(%rsp), %xmm15\n"
        "vmovaps 256(%rsp), %xmm2\n"
        "addq $272, %rsp\n"
        "popq %r11\n"
        "popq %r10\n"
        "popq %r9\n"
        "popq %r8\n"
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rdx\n"
        "popq %rcx\n"
        "popq %rax\n"
        "popfq\n"
        "movq 0x10(%r14), %rax\n"
        "jmpq *replay_hud_observation_resume2(%rip)\n"
    );
}
static int replay_hud_observation_patch(void) {
    static const unsigned char hud1[]={0x49,0x8b,0x46,0x10,0xc5,0xf8,0x28,0x90,0xb0,0x0,0x0,0x0};
    if(runtime_image_size<0x478a5c || memcmp(image+0x478a50,hud1,sizeof hud1)) return 0;
    static const unsigned char hud2[]={0x49,0x8b,0x46,0x10,0xc5,0xf8,0x28,0x90,0xb0,0x0,0x0,0x0};
    if(runtime_image_size<0x478bab || memcmp(image+0x478b9f,hud2,sizeof hud2)) return 0;
    uintptr_t target;const unsigned char jump[2]={0x48,0xb8},tail[2]={0xff,0xe0};
    replay_hud_observation_resume1=(uintptr_t)image+0x478a5c;
    target=(uintptr_t)replay_hud_observation_bridge1;
    memcpy(image+0x478a50,jump,2);memcpy(image+0x478a52,&target,8);memcpy(image+0x478a5a,tail,2);
    FlushInstructionCache(GetCurrentProcess(),image+0x478a50,12);
    replay_hud_observation_resume2=(uintptr_t)image+0x478bab;
    target=(uintptr_t)replay_hud_observation_bridge2;
    memcpy(image+0x478b9f,jump,2);memcpy(image+0x478ba1,&target,8);memcpy(image+0x478ba9,tail,2);
    FlushInstructionCache(GetCurrentProcess(),image+0x478b9f,12);
    return 1;
}

typedef void (ABI *ReplayHudUpdate)(void *,void *,float);
static ReplayHudUpdate replay_hud_update_original;
static ABI void replay_hud_update(void *self,void *subject,float dt) {
    ReplayHudObservation saved=replay_hud_observation;
    replay_hud_observation.dt=dt;replay_hud_observation.prepared=0;
    replay_hud_update_original(self,subject,dt);
    replay_hud_observation=saved;
}
static void replay_hud_install(void) {
    static const unsigned char entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28};
    replay_hud_update_original=(ReplayHudUpdate)camera_rate_hook(0x478a20,entry,sizeof entry,(uintptr_t)replay_hud_update);
    if(replay_hud_update_original)printf("CameraPatch: replay HUD observer %s\n",replay_hud_observation_patch()?"on":"unavailable");
}
