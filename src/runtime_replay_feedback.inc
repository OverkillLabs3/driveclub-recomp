/* Replay position/velocity correction, authored for one update per 1/30 s.
   Keep its native projection, correction cap and teleport path after this block. */
static __attribute__((used)) uintptr_t replay_feedback_resume;
static __attribute__((used,noinline)) ABI void replay_feedback_compute(void *frame,void *actor,void *vehicle) {
    float dt, current_pos[4],current_vel[4],target_pos[4],target_vel[4];
    memcpy(&dt,(char *)frame-0xb0,4);
    memcpy(current_pos,(char *)frame-0xc0,16);
    memcpy(current_vel,(char *)frame-0x50,16);
    memcpy(target_pos,(char *)actor+0x90,16);
    memcpy(target_vel,(char *)actor+0x40,16);
    replay_recorded_publish(vehicle,actor,dt,target_vel);
    const float reference=1.0f/30.0f;
    float inverse=1.0f/dt,gain=0.3f;
    /* Preserve native behavior at 30 FPS and below; only normalize shorter steps.
       0.3 is the original gain, not a newly chosen camera tuning value. */
    if(dt>0 && dt<reference-0.000001f) {
        gain=-expm1f(log1pf(-0.3f)*(dt/reference));
        inverse=30.0f;
    }
    float error[4],correction[4];
    for(unsigned i=0;i<4;++i) {
        error[i]=target_pos[i]-current_pos[i];
        correction[i]=(target_vel[i]-current_vel[i])*gain+(error[i]*gain)*inverse;
    }
    memcpy((char *)frame-0xd0,error,16);
    memcpy((char *)frame-0xc0,correction,16);
}
static __attribute__((used)) uintptr_t replay_rotation_resume;
static __attribute__((used,noinline)) ABI void replay_rotation_compute(void *frame,void *actor) {
    float dt,axes[3][4],target[3][4],current[4],wanted[4];
    memcpy(&dt,(char *)frame-0xb0,4);
    memcpy(axes,(char *)frame-0xf0,sizeof axes);
    memcpy(target,(char *)actor+0x60,sizeof target);
    memcpy(current,(char *)frame-0x40,16);
    memcpy(wanted,(char *)actor+0x50,16);
    float gain=0.3f,rot_gain=0.15f,inverse=1.0f/dt;
    if(dt>0 && dt<1.0f/30.0f-0.000001f) {
        gain=-expm1f(log1pf(-0.3f)*(dt*30.0f));
        rot_gain=0.5f*gain;inverse=30.0f;
    }
    float cross[3][4]={{0}},error[4],correction[4];
    for(unsigned a=0;a<3;++a) {
        float diff[3];for(unsigned i=0;i<3;++i) diff[i]=target[a][i]-axes[a][i];
        for(unsigned i=0;i<3;++i) { unsigned j=(i+1)%3,k=(i+2)%3;cross[a][i]=axes[a][j]*diff[k]-axes[a][k]*diff[j]; }
    }
    for(unsigned i=0;i<4;++i) {
        error[i]=cross[2][i]+(cross[0][i]+cross[1][i]);
        correction[i]=(wanted[i]-current[i])*gain+inverse*(error[i]*rot_gain);
    }
    const float original_flag[4]={1,0,0,0};
    memcpy((char *)frame-0xc0,original_flag,16);
    memcpy((char *)frame-0xb0,correction,16);
}
static __attribute__((naked,used)) ABI void replay_rotation_bridge(void) {
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
        "subq $256, %rsp\n"
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
        "movq %rbp, %rdi\n"
        "movq %r12, %rsi\n"
        "callq replay_rotation_compute\n"
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
        "addq $256, %rsp\n"
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
        "jmpq *replay_rotation_resume(%rip)\n"
    );
}
static __attribute__((naked,used)) ABI void replay_feedback_bridge(void) {
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
        "subq $256, %rsp\n"
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
        "movq %rbp, %rdi\n"
        "movq %r12, %rsi\n"
        "movq %rbx, %rdx\n"
        "callq replay_feedback_compute\n"
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
        "addq $256, %rsp\n"
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
        "jmpq *replay_feedback_resume(%rip)\n"
    );
}
static void replay_feedback_install(void) {
    static const unsigned char expected[]={0xc5,0xfa,0x10,0x15,0xdd,0x9c,0x18,0x1,0xc5,0xea,0x5e,0x95,0x50,0xff,0xff,0xff,0xc5,0xf0,0x5c,0x9d,0x40,0xff,0xff,0xff,0xc5,0xf8,0x29,0x9d,0x30,0xff,0xff,0xff,0xc5,0xf8,0x28,0xd,0x1d,0x9c,0x18,0x1,0xc5,0xe0,0x59,0xd9,0xc5,0xf9,0x70,0xd2,0x0,0xc5,0xe0,0x59,0xd2,0xc5,0xf8,0x5c,0x45,0xb0,0xc5,0xf8,0x59,0xc1,0xc5,0xf8,0x58,0xc2,0xc5,0xf8,0x29,0x85,0x40,0xff,0xff,0xff};
    const size_t offset=0x123c8b;
    if(runtime_image_size<offset+sizeof expected || memcmp(image+offset,expected,sizeof expected)) {
        puts("ReplayFeedback: unsupported signature; skipped");return;
    }
    const int rotation_enabled=1;
    static const unsigned char rotation_expected[]={0xc5,0xfa,0x10,0x25,0x56,0x99,0x18,0x1,0xc5,0xf8,0x29,0xa5,0x40,0xff,0xff,0xff,0xc5,0xda,0x5e,0xa5,0x50,0xff,0xff,0xff,0xc5,0xf0,0x5c,0x8d,0x10,0xff,0xff,0xff,0xc5,0xe8,0x5c,0x95,0x20,0xff,0xff,0xff,0xc5,0xe0,0x5c,0x9d,0x30,0xff,0xff,0xff,0xc5,0xf9,0x70,0xad,0x10,0xff,0xff,0xff,0xc9,0xc5,0xf9,0x70,0xf1,0xc9,0xc5,0xf8,0x28,0x85,0x10,0xff,0xff,0xff,0xc5,0xf8,0x59,0xf6,0xc5,0xd0,0x59,0xc9,0xc5,0xc8,0x5c,0xc9,0xc5,0xf9,0x70,0xc9,0x49,0xc5,0xc0,0x57,0xff,0xc4,0xe3,0x71,0x21,0xcf,0x30,0xc5,0xf9,0x70,0xad,0x20,0xff,0xff,0xff,0xc9,0xc5,0xf9,0x70,0xf2,0xc9,0xc5,0xf8,0x28,0x85,0x20,0xff,0xff,0xff,0xc5,0xf8,0x59,0xf6,0xc5,0xd0,0x59,0xd2,0xc5,0xc8,0x5c,0xd2,0xc5,0xf9,0x70,0xd2,0x49,0xc4,0xe3,0x69,0x21,0xd7,0x30,0xc5,0xf9,0x70,0xad,0x30,0xff,0xff,0xff,0xc9,0xc5,0xf9,0x70,0xf3,0xc9,0xc5,0xf8,0x28,0x85,0x30,0xff,0xff,0xff,0xc5,0xf8,0x59,0xf6,0xc5,0xd0,0x59,0xdb,0xc5,0xc8,0x5c,0xdb,0xc5,0xf9,0x70,0xdb,0x49,0xc4,0xe3,0x61,0x21,0xdf,0x30,0xc5,0xf0,0x58,0xca,0xc5,0xe0,0x58,0xc9,0xc5,0xb8,0x5c,0x45,0xc0,0xc5,0xf0,0x59,0xd,0x3e,0x98,0x18,0x1,0xc5,0xf9,0x70,0xd4,0x0,0xc5,0xe8,0x59,0xc9,0xc5,0xf8,0x59,0x5,0xdd,0x97,0x18,0x1,0xc5,0xf8,0x58,0xc1,0xc5,0xf8,0x29,0x85,0x50,0xff,0xff,0xff};
    if(rotation_enabled && (runtime_image_size<0x1240ff || memcmp(image+0x124012,rotation_expected,sizeof rotation_expected))) {
        puts("ReplayFeedback: unsupported rotation signature; skipped");return;
    }
    replay_feedback_resume=(uintptr_t)image+0x123cd5;
    uintptr_t target=(uintptr_t)replay_feedback_bridge;
    const unsigned char jump[2]={0x48,0xb8},tail[2]={0xff,0xe0};
    memcpy(image+offset,jump,2);memcpy(image+offset+2,&target,8);memcpy(image+offset+10,tail,2);
    memset(image+offset+12,0x90,sizeof expected-12);
    FlushInstructionCache(GetCurrentProcess(),image+offset,sizeof expected);
    if(rotation_enabled) {
        replay_rotation_resume=(uintptr_t)image+0x1240ff;
        target=(uintptr_t)replay_rotation_bridge;
        memcpy(image+0x124012,jump,2);memcpy(image+0x124014,&target,8);memcpy(image+0x12401c,tail,2);
        memset(image+0x12401e,0x90,sizeof rotation_expected-12);
        FlushInstructionCache(GetCurrentProcess(),image+0x124012,sizeof rotation_expected);
    }
    printf("ReplayFeedback: reference-rate linear correction on; rotation %s; original caps and teleport unchanged\n",rotation_enabled?"on":"off");
}
