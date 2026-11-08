/* Read-only original Replay linear feedback observer. The original74native
 * bytes execute ONCE in a gateway (only two RIP constant loads expanded).
 * No gain normalization, extra native getters, source/RNG/clock writes.
 * Native guest integer/flags/AVX256/x87/MXCSR state preserved before math. */
static __attribute__((used)) uintptr_t nf32_gateway;
static __attribute__((noinline,used)) ABI void nf32_observe(void*frame,void*actor,void*vehicle){
 (void)frame;(void)actor;(void)vehicle;
}
static __attribute__((naked,used)) ABI void nf32_bridge(void){
 __asm__ volatile(
  "pushfq\n pushq %rax\n pushq %rcx\n pushq %rdx\n pushq %rbx\n pushq %rbp\n pushq %rsi\n pushq %rdi\n"
  "pushq %r8\n pushq %r9\n pushq %r10\n pushq %r11\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n subq $1024,%rsp\n"
  "fxsave64 0(%rsp)\n"
  "vmovdqu %ymm0,512(%rsp)\n vmovdqu %ymm1,544(%rsp)\n vmovdqu %ymm2,576(%rsp)\n vmovdqu %ymm3,608(%rsp)\n"
  "vmovdqu %ymm4,640(%rsp)\n vmovdqu %ymm5,672(%rsp)\n vmovdqu %ymm6,704(%rsp)\n vmovdqu %ymm7,736(%rsp)\n"
  "vmovdqu %ymm8,768(%rsp)\n vmovdqu %ymm9,800(%rsp)\n vmovdqu %ymm10,832(%rsp)\n vmovdqu %ymm11,864(%rsp)\n"
  "vmovdqu %ymm12,896(%rsp)\n vmovdqu %ymm13,928(%rsp)\n vmovdqu %ymm14,960(%rsp)\n vmovdqu %ymm15,992(%rsp)\n"
  "movq %rbp,%rdi\n movq %r12,%rsi\n movq %rbx,%rdx\n callq nf32_observe\n"
  "fxrstor64 0(%rsp)\n"
  "vmovdqu 512(%rsp),%ymm0\n vmovdqu 544(%rsp),%ymm1\n vmovdqu 576(%rsp),%ymm2\n vmovdqu 608(%rsp),%ymm3\n"
  "vmovdqu 640(%rsp),%ymm4\n vmovdqu 672(%rsp),%ymm5\n vmovdqu 704(%rsp),%ymm6\n vmovdqu 736(%rsp),%ymm7\n"
  "vmovdqu 768(%rsp),%ymm8\n vmovdqu 800(%rsp),%ymm9\n vmovdqu 832(%rsp),%ymm10\n vmovdqu 864(%rsp),%ymm11\n"
  "vmovdqu 896(%rsp),%ymm12\n vmovdqu 928(%rsp),%ymm13\n vmovdqu 960(%rsp),%ymm14\n vmovdqu 992(%rsp),%ymm15\n"
  "addq $1024,%rsp\n popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %r11\n popq %r10\n popq %r9\n popq %r8\n"
  "popq %rdi\n popq %rsi\n popq %rbp\n popq %rbx\n popq %rdx\n popq %rcx\n popq %rax\n popfq\n jmpq *nf32_gateway(%rip)\n");
}
static const unsigned char nf32_expected[]={0xc5,0xfa,0x10,0x15,0xdd,0x9c,0x18,0x1,0xc5,0xea,0x5e,0x95,0x50,0xff,0xff,0xff,0xc5,0xf0,0x5c,0x9d,0x40,0xff,0xff,0xff,0xc5,0xf8,0x29,0x9d,0x30,0xff,0xff,0xff,0xc5,0xf8,0x28,0xd,0x1d,0x9c,0x18,0x1,0xc5,0xe0,0x59,0xd9,0xc5,0xf9,0x70,0xd2,0x0,0xc5,0xe0,0x59,0xd2,0xc5,0xf8,0x5c,0x45,0xb0,0xc5,0xf8,0x59,0xc1,0xc5,0xf8,0x58,0xc2,0xc5,0xf8,0x29,0x85,0x40,0xff,0xff,0xff};
static void nf32_emit_jump(unsigned char*out,uintptr_t destination){
 static const unsigned char jump[]={0xff,0x25,0,0,0,0};memcpy(out,jump,6);memcpy(out+6,&destination,8);
}
static unsigned nf32_emit_constant(unsigned char*out,uintptr_t address,int scalar){
 static const unsigned char before[]={0x41,0x53,0x49,0xbb};
 static const unsigned char load_scalar[]={0xc4,0xc1,0x7a,0x10,0x13}; /* vmovss xmm2,[r11] */
 static const unsigned char load_vector[]={0xc4,0xc1,0x78,0x28,0x0b}; /* vmovaps xmm1,[r11] */
 static const unsigned char after[]={0x41,0x5b};
 memcpy(out,before,4);memcpy(out+4,&address,8);memcpy(out+12,scalar?load_scalar:load_vector,5);memcpy(out+17,after,2);return 19;
}
static uintptr_t nf32_make_gateway(uintptr_t resume){
 const size_t offset=0x123c8b;int32_t d0,d1;memcpy(&d0,nf32_expected+4,4);memcpy(&d1,nf32_expected+36,4);
 uintptr_t scalar=(uintptr_t)image+offset+8+d0,vector=(uintptr_t)image+offset+40+d1;
 unsigned char*code=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);if(!code)return 0;unsigned n=0;
 n+=nf32_emit_constant(code+n,scalar,1);memcpy(code+n,nf32_expected+8,24);n+=24;
 n+=nf32_emit_constant(code+n,vector,0);memcpy(code+n,nf32_expected+40,sizeof nf32_expected-40);n+=sizeof nf32_expected-40;
 nf32_emit_jump(code+n,resume);n+=14;FlushInstructionCache(GetCurrentProcess(),code,n);return(uintptr_t)code;
}
static __attribute__((unused)) int native_feedback32_install(void){
 const char*requested=camera_fix_getenv("BB_REPLAY_FEEDBACK_NATIVE_TRACE");if(!requested||strcmp(requested,"1"))return 0;
 const size_t offset=0x123c8b;if(runtime_image_size<offset+sizeof nf32_expected||memcmp(image+offset,nf32_expected,sizeof nf32_expected)){
  puts("NativeFeedback32: original signature unavailable (patched feedback route?); no hook");return 0;
 }
 nf32_gateway=nf32_make_gateway((uintptr_t)image+offset+sizeof nf32_expected);if(!nf32_gateway)return 0;
 nf32_emit_jump(image+offset,(uintptr_t)nf32_bridge);memset(image+offset+14,0x90,sizeof nf32_expected-14);FlushInstructionCache(GetCurrentProcess(),image+offset,sizeof nf32_expected);
 puts("NativeFeedback32: read-only original linear feedback inputs; native74bytes once, GPR/flags/YMM/x87/MXCSR preserved");return 1;
}
