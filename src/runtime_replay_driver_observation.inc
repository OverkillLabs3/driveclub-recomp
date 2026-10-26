static __attribute__((used)) uintptr_t replay_driver_observation_resume;
static __attribute__((used,noinline)) ABI void replay_driver_observation_compute(void *frame,void *self,void *vehicle) {
    float dt,cur[6],prev[6],derivative[6];
    memcpy(&dt,(char *)frame-0xf0,4);
    memcpy(cur,(char *)frame-0x40,12);memcpy(cur+3,(char *)frame-0x50,12);
    memcpy(prev,(char *)self+0x130,12);memcpy(prev+3,(char *)self+0x140,12);
    int filtered=replay_recorded_read(replay_recorded_state(vehicle),dt,derivative);
    float delta[2][4]={{0}},inverse[4]={1.0f/dt,0,0,0};
    for(unsigned axis=0;axis<3;++axis) for(unsigned group=0;group<2;++group) {
        unsigned k=group*3+axis;
        delta[group][axis]=filtered?derivative[k]*dt:cur[k]-prev[k];
    }
    /* Preserve native fourth lanes in the fallback, although only XYZ is used. */
    for(unsigned group=0;group<2;++group) {
        float a,c;memcpy(&a,(char *)frame-(group?0x50:0x40)+12,4);
        memcpy(&c,(char *)self+0x130+16*group+12,4);delta[group][3]=a-c;
    }
    memcpy((char *)frame-0x110,delta[0],16);memcpy((char *)frame-0x120,delta[1],16);
    memcpy((char *)frame-0x130,inverse,16);
}
static __attribute__((naked,used)) ABI void replay_driver_observation_bridge(void) {
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
        "movq %r15, %rsi\n"
        "movq %r14, %rdx\n"
        "callq replay_driver_observation_compute\n"
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
        "jmpq *replay_driver_observation_resume(%rip)\n"
    );
}
static int replay_driver_observation_patch(void) {
    static const unsigned char driver_expected[]={0xc4,0xc1,0x70,0x5c,0x8f,0x30,0x1,0x0,0x0,0xc5,0xf8,0x29,0x8d,0xf0,0xfe,0xff,0xff,0xc5,0xfa,0x10,0xd,0x91,0xfc,0x7b,0x0,0xc5,0xf2,0x5e,0xca,0xc5,0xf8,0x29,0x8d,0xd0,0xfe,0xff,0xff,0xc4,0xc1,0x78,0x5c,0x87,0x40,0x1,0x0,0x0,0xc5,0xf8,0x29,0x85,0xe0,0xfe,0xff,0xff};
    if(runtime_image_size<0xb91998 || memcmp(image+0xb91962,driver_expected,sizeof driver_expected)) return 0;
    uintptr_t target;const unsigned char jump[2]={0x48,0xb8},tail[2]={0xff,0xe0};
    replay_driver_observation_resume=(uintptr_t)image+0xb91998;
    target=(uintptr_t)replay_driver_observation_bridge;
    memcpy(image+0xb91962,jump,2);memcpy(image+0xb91964,&target,8);memcpy(image+0xb9196c,tail,2);
    memset(image+0xb9196e,0x90,42);
    FlushInstructionCache(GetCurrentProcess(),image+0xb91962,54);
    return 1;
}
