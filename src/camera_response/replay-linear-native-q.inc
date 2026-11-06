/* OPTIONAL CAUSAL EXPERIMENT, not an accuracy/physics-equivalence claim.
 * Keep native original linear arithmetic/projection/cap/teleport/setter region.
 * Existing angular path remains entirely unchanged, including authored90zero.
 * Source inputs are the already-executed SDK getter observations, NOT raw10 or
 * position20 substitutes. Latest VRR mean is not asserted native16-substepmean.
 * BB_REPLAY_LINEAR_NATIVE_Q=1. PacketQ source variant: unavailable/multiQ skip explicitly.
 */
#define RLQ_Q_US INT64_C(33333)
#define RLQ_CAPACITY 32
typedef struct {
 int64_t time;uintptr_t clock,provider;
 float current_pos[4],current_vel[4];_Alignas(16)unsigned char actor_bytes[0xa0];
} RlqInput;
typedef struct {uintptr_t actor,vehicle;int adapted,initialized;int64_t last_q;uint64_t packet_epoch;RlqInput previous;} RlqState;
typedef struct {
 int active,due;uintptr_t frame,actor,vehicle,r12,r14;int64_t time,q;
 float dt,current_pos[4],current_vel[4];_Alignas(16)unsigned char actor_bytes[0xa0];
} RlqCall;
static RlqState rlq_states[RLQ_CAPACITY];static SRWLOCK rlq_lock=SRWLOCK_INIT;
static _Thread_local RlqCall rlq_call;
static __attribute__((used)) uintptr_t rlq_original_entry,rlq_original_exit;
static int rlq_finite(const void*p,unsigned n){for(unsigned i=0;i<n;++i){float f;memcpy(&f,(const char*)p+4*i,4);if(!isfinite(f))return 0;}return 1;}
static int replay_linear_q_requested(void){const char*p=camera_fix_getenv("BB_REPLAY_LINEAR_NATIVE_Q");return p&&!strcmp(p,"1");}
static void replay_linear_q_discontinuity(void){lsq_runtime_forget(0,0);AcquireSRWLockExclusive(&rlq_lock);memset(rlq_states,0,sizeof rlq_states);ReleaseSRWLockExclusive(&rlq_lock);}
static uintptr_t rlq_body_state(uintptr_t vehicle){uintptr_t manager=0,owner=0,body=0;uint32_t handle=0;
 if(!camera_pose_read(vehicle+0x70,&manager,8)||!camera_pose_read(vehicle+0x18,&handle,4))return 0;
 int32_t index=(int32_t)(handle+UINT32_C(0xff0f3f01));
 if(!camera_pose_read(manager+(int64_t)index*0xb8+0x38,&owner,8)||!camera_pose_read(owner+8,&body,8))return 0;return body+0x2ad0;
}
/* Called only by the already-installed actual641220 observer, after original.
 * No new guest setter hook, getter, guessed discontinuity threshold or writes. */
static void replay_linear_q_body_reset(uintptr_t state){if(!state)return;lsq_runtime_forget(0,state);AcquireSRWLockExclusive(&rlq_lock);for(unsigned i=0;i<RLQ_CAPACITY;++i){RlqState*s=rlq_states+i;if(s->actor&&rlq_body_state(s->vehicle)==state)s->initialized=0;}ReleaseSRWLockExclusive(&rlq_lock);}
static int rlq_clock(uintptr_t*clock,int64_t*time,int64_t*us){uintptr_t p=0;return camera_pose_read((uintptr_t)image+0x1911100,&p,8)&&camera_pose_read(p+0x970,&p,8)&&camera_pose_read(p+0x1e0,clock,8)&&camera_pose_read(*clock+0x58,time,8)&&camera_pose_read(*clock+0x60,us,8);}
static RlqState*rlq_find(uintptr_t actor,uintptr_t vehicle){RlqState*empty=NULL;for(unsigned i=0;i<RLQ_CAPACITY;++i){RlqState*s=rlq_states+i;if(s->actor==actor)return s;if(!s->actor&&!empty)empty=s;}if(empty){memset(empty,0,sizeof *empty);empty->actor=actor;empty->vehicle=vehicle;}return empty;}
static void rlq_record32(void*frame,void*actor,void*vehicle,const RlqInput*input,float dt,float gain){
 (void)frame;(void)actor;(void)vehicle;(void)input;(void)dt;(void)gain;
}
static void rlq_status(unsigned status,void*actor,uintptr_t vehicle,float dt,int64_t time,int64_t q,int64_t us){lsq_parent_status(status,actor,vehicle,dt,time,q,us);float data[24]={(float)status};memcpy(data+1,&time,8);memcpy(data+3,&q,8);memcpy(data+5,&us,8);camera_accuracy_log(36,actor,vehicle,0x123c8b,dt,data,7);}
#include "replay-linear-packet-q-enter.inc"
static __attribute__((used,noinline)) ABI uintptr_t rlq_leave(void*frame,unsigned char*context){
 RlqCall*c=&rlq_call;if(c->active&&c->frame==(uintptr_t)frame){
  lsq_complete_before_restore(frame,c); /* after native setter, before restoring private snapshots */
  /* Native teleport branch stores precisely targetVelocity pointer in C0.
   * Do not interpolate the next bracket across its real pose discontinuity. */
  uintptr_t stored=0;memcpy(&stored,(char*)frame-0xc0,8);uintptr_t wanted=c->due?(uintptr_t)c->actor_bytes+0x40:c->actor+0x40;
  if(stored==wanted){AcquireSRWLockExclusive(&rlq_lock);RlqState*s=rlq_find(c->actor,c->vehicle);if(s)s->initialized=0;ReleaseSRWLockExclusive(&rlq_lock);}
  if(c->due){memcpy((char*)frame-0xb0,&c->dt,4);memcpy((char*)frame-0xc0,c->current_pos,16);memcpy((char*)frame-0x50,c->current_vel,16);memcpy(context+1064,&c->r12,8);memcpy(context+1048,&c->r14,8);}
  float data[24]={(float)c->due};memcpy(data+1,&c->q,8);memcpy(data+3,&c->time,8);camera_accuracy_log(38,(void*)c->actor,c->vehicle,0x123ee3,c->dt,data,5);memset(c,0,sizeof *c);
 }return rlq_original_exit;
}
/* Same full integer/AVX256/x87 save contract as tested native32 observer.
 * Destination lives in private stack slot outside saved state; no register is
 * borrowed after restoration. Native mid-function RSP is16-aligned. */
#define RLQ_SAVE "pushfq\n pushq %rax\n pushq %rcx\n pushq %rdx\n pushq %rbx\n pushq %rbp\n pushq %rsi\n pushq %rdi\n pushq %r8\n pushq %r9\n pushq %r10\n pushq %r11\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n subq $1040,%rsp\n fxsave64 0(%rsp)\n" \
 "vmovdqu %ymm0,512(%rsp)\n vmovdqu %ymm1,544(%rsp)\n vmovdqu %ymm2,576(%rsp)\n vmovdqu %ymm3,608(%rsp)\n vmovdqu %ymm4,640(%rsp)\n vmovdqu %ymm5,672(%rsp)\n vmovdqu %ymm6,704(%rsp)\n vmovdqu %ymm7,736(%rsp)\n vmovdqu %ymm8,768(%rsp)\n vmovdqu %ymm9,800(%rsp)\n vmovdqu %ymm10,832(%rsp)\n vmovdqu %ymm11,864(%rsp)\n vmovdqu %ymm12,896(%rsp)\n vmovdqu %ymm13,928(%rsp)\n vmovdqu %ymm14,960(%rsp)\n vmovdqu %ymm15,992(%rsp)\n"
#define RLQ_RESTORE "movq %rax,1024(%rsp)\n fxrstor64 0(%rsp)\n vmovdqu 512(%rsp),%ymm0\n vmovdqu 544(%rsp),%ymm1\n vmovdqu 576(%rsp),%ymm2\n vmovdqu 608(%rsp),%ymm3\n vmovdqu 640(%rsp),%ymm4\n vmovdqu 672(%rsp),%ymm5\n vmovdqu 704(%rsp),%ymm6\n vmovdqu 736(%rsp),%ymm7\n vmovdqu 768(%rsp),%ymm8\n vmovdqu 800(%rsp),%ymm9\n vmovdqu 832(%rsp),%ymm10\n vmovdqu 864(%rsp),%ymm11\n vmovdqu 896(%rsp),%ymm12\n vmovdqu 928(%rsp),%ymm13\n vmovdqu 960(%rsp),%ymm14\n vmovdqu 992(%rsp),%ymm15\n addq $1040,%rsp\n popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %r11\n popq %r10\n popq %r9\n popq %r8\n popq %rdi\n popq %rsi\n popq %rbp\n popq %rbx\n popq %rdx\n popq %rcx\n popq %rax\n popfq\n jmpq *-144(%rsp)\n"
static __attribute__((naked,used)) ABI void rlq_entry_bridge(void){__asm__ volatile(RLQ_SAVE "movq %rbp,%rdi\n movq %r12,%rsi\n movq %rbx,%rdx\n movq %rsp,%rcx\n callq rlq_enter\n" RLQ_RESTORE);}
static __attribute__((naked,used)) ABI void rlq_exit_bridge(void){__asm__ volatile(RLQ_SAVE "movq %rbp,%rdi\n movq %rsp,%rsi\n callq rlq_leave\n" RLQ_RESTORE);}
#undef RLQ_SAVE
#undef RLQ_RESTORE
static uint64_t rlq_hash(const unsigned char*p,size_t n){uint64_t h=UINT64_C(14695981039346656037);for(size_t i=0;i<n;++i)h=(h^p[i])*UINT64_C(1099511628211);return h;}
static int replay_linear_q_install(void){
 if(!replay_linear_q_requested())return 0;
 static const unsigned char exit_code[]={0x49,0x8b,0x07,0x48,0x8b,0x80,0x70,0x09,0,0,0x4c,0x8b,0xb0,0x70,0x03,0,0};
 if(runtime_image_size<0x123ef4||memcmp(image+0x123c8b,nf32_expected,sizeof nf32_expected)||memcmp(image+0x123ee3,exit_code,sizeof exit_code)||rlq_hash(image+0x123cd5,0x123ee3-0x123cd5)!=UINT64_C(0x1bc235377d23c741)){puts("ReplayLinearNativeQ: unsupported/conflicting whole-region signatures; unavailable");return 0;}
 rlq_original_entry=nf32_make_gateway((uintptr_t)image+0x123cd5);if(!rlq_original_entry)return 0;
 unsigned char*exit=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);if(!exit)return 0;memcpy(exit,exit_code,sizeof exit_code);nf32_emit_jump(exit+sizeof exit_code,(uintptr_t)image+0x123ef4);rlq_original_exit=(uintptr_t)exit;
 if(!replay_linear_mean_rebase_prepare())return 0;
 nf32_emit_jump(image+0x123c8b,(uintptr_t)rlq_entry_bridge);memset(image+0x123c8b+14,0x90,sizeof nf32_expected-14);nf32_emit_jump(image+0x123ee3,(uintptr_t)rlq_exit_bridge);memset(image+0x123ee3+14,0x90,sizeof exit_code-14);
 FlushInstructionCache(GetCurrentProcess(),image+0x123c8b,sizeof nf32_expected);FlushInstructionCache(GetCurrentProcess(),image+0x123ee3,sizeof exit_code);FlushInstructionCache(GetCurrentProcess(),exit,sizeof exit_code+14);
 replay_linear_mean_rebase_commit();
 feedback_branch_scalar_install(); /* MUST follow original whole-region hash and semantic commits */
 puts("ReplayLinearNativeQ: OPTIONAL original packet/native673930 Q targets; original whole linear region; angular unchanged; physical SDK Q sampling EXPERIMENTAL");return 1;
}
