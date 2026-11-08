/* OPTIONAL SDK setter baseline experiment. Include AFTER parent RLQ module.
 * Requires original render getter snapshot already saved in RlqCall.current_vel.
 * Original Q projected/capped C0 is untouched. Normal addition uses mean_render.
 * Teleport does not visit this site. Native30 and original fallback are no-ops.
 * No getter, physical history/reset, RNG, TTL or owner-cache operation.
 */
static __attribute__((used)) uintptr_t rlmr_original_add;
static unsigned char *rlmr_pending_add;
static int replay_linear_mean_rebase_requested(void){
 const char*p=camera_fix_getenv("BB_REPLAY_LINEAR_MEAN_REBASE");return p&&!strcmp(p,"1");
}
static __attribute__((used,noinline)) ABI void rlmr_before_add(void*frame,void*actor,void*vehicle){
 RlqCall*c=&rlq_call;
 if(c->active&&c->due&&c->frame==(uintptr_t)frame&&c->vehicle==(uintptr_t)vehicle&&
    (uintptr_t)actor==(uintptr_t)c->actor_bytes){
  lsq_capture_before_add(frame,c); /* M_Q+C_Q BEFORE physical mean restore */
  memcpy((char*)frame-0x50,c->current_vel,16);
 }
#ifdef RLMR_CONTEXT_SELFTEST
 rlmr_context_test_clobber();
#endif
}
static __attribute__((naked,used)) ABI void rlmr_add_bridge(void){
 __asm__ volatile(
 "pushfq\n pushq %rax\n pushq %rcx\n pushq %rdx\n pushq %rbx\n pushq %rbp\n pushq %rsi\n pushq %rdi\n pushq %r8\n pushq %r9\n pushq %r10\n pushq %r11\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n subq $1024,%rsp\n fxsave64 0(%rsp)\n"
 "vmovdqu %ymm0,512(%rsp)\n vmovdqu %ymm1,544(%rsp)\n vmovdqu %ymm2,576(%rsp)\n vmovdqu %ymm3,608(%rsp)\n vmovdqu %ymm4,640(%rsp)\n vmovdqu %ymm5,672(%rsp)\n vmovdqu %ymm6,704(%rsp)\n vmovdqu %ymm7,736(%rsp)\n vmovdqu %ymm8,768(%rsp)\n vmovdqu %ymm9,800(%rsp)\n vmovdqu %ymm10,832(%rsp)\n vmovdqu %ymm11,864(%rsp)\n vmovdqu %ymm12,896(%rsp)\n vmovdqu %ymm13,928(%rsp)\n vmovdqu %ymm14,960(%rsp)\n vmovdqu %ymm15,992(%rsp)\n"
 "movq %rbp,%rdi\n movq %r12,%rsi\n movq %rbx,%rdx\n callq rlmr_before_add\n fxrstor64 0(%rsp)\n"
 "vmovdqu 512(%rsp),%ymm0\n vmovdqu 544(%rsp),%ymm1\n vmovdqu 576(%rsp),%ymm2\n vmovdqu 608(%rsp),%ymm3\n vmovdqu 640(%rsp),%ymm4\n vmovdqu 672(%rsp),%ymm5\n vmovdqu 704(%rsp),%ymm6\n vmovdqu 736(%rsp),%ymm7\n vmovdqu 768(%rsp),%ymm8\n vmovdqu 800(%rsp),%ymm9\n vmovdqu 832(%rsp),%ymm10\n vmovdqu 864(%rsp),%ymm11\n vmovdqu 896(%rsp),%ymm12\n vmovdqu 928(%rsp),%ymm13\n vmovdqu 960(%rsp),%ymm14\n vmovdqu 992(%rsp),%ymm15\n"
 "addq $1024,%rsp\n popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %r11\n popq %r10\n popq %r9\n popq %r8\n popq %rdi\n popq %rsi\n popq %rbp\n popq %rbx\n popq %rdx\n popq %rcx\n popq %rax\n popfq\n jmpq *rlmr_original_add(%rip)\n");
}
static const unsigned char rlmr_add_expected[]={
 0xc5,0xf8,0x28,0x45,0xb0,0xc5,0xf8,0x29,0x45,0xa0,
 0xc5,0xf8,0x28,0x85,0x40,0xff,0xff,0xff};
/* Called by parent installer AFTER its untouched whole-region hash succeeds,
 * BEFORE any parent or addon site is changed. All allocation is also precommit.
 * Rechecking whole middle here prevents unsafe standalone prepare calls.
 */
static int replay_linear_mean_rebase_prepare(void){
 if(!replay_linear_mean_rebase_requested())return 1;
 if(!replay_linear_q_requested()||runtime_image_size<0x123ef4||
    memcmp(image+0x123eb7,rlmr_add_expected,sizeof rlmr_add_expected)||
    rlq_hash(image+0x123cd5,0x123ee3-0x123cd5)!=UINT64_C(0x1bc235377d23c741)){
  puts("ReplayLinearMeanRebase: unsupported/conflicting original region; no linear hooks");return 0;
 }
 if(rlmr_pending_add)return 0;
 unsigned char*code=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
 if(!code)return 0;
 memcpy(code,rlmr_add_expected,sizeof rlmr_add_expected);
 nf32_emit_jump(code+sizeof rlmr_add_expected,(uintptr_t)image+0x123ec9);
 FlushInstructionCache(GetCurrentProcess(),code,sizeof rlmr_add_expected+14);
 rlmr_pending_add=code;return 1;
}
static void replay_linear_mean_rebase_commit(void){
 if(!replay_linear_mean_rebase_requested())return;
 /* Parent calls only after successful preflight/allocation; no fallible work
  * or late signature check is allowed between first parent write and commit. */
 rlmr_original_add=(uintptr_t)rlmr_pending_add;
 nf32_emit_jump(image+0x123eb7,(uintptr_t)rlmr_add_bridge);
 memset(image+0x123eb7+14,0x90,sizeof rlmr_add_expected-14);
 FlushInstructionCache(GetCurrentProcess(),image+0x123eb7,sizeof rlmr_add_expected);
 puts("ReplayLinearMeanRebase: OPTIONAL original Q delta + saved original render SDK mean; teleport/native30 unchanged");
}
