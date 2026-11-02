/* Bounded actual770DD0 packet input/output observation. No extra reader query.
 * Optional private validation calls only original kernel on private0x40 copies.
 * Payload extent is proven by original7717B0 copy08..3F; allocator size UNKNOWN.
 * The six original SysV arguments and live call's complete machine state are
 * preserved. Actual native call executes ONCE, including original return bits. */
#define RPK_CAPACITY 262144u
typedef struct{unsigned char magic[8];uint32_t version,record_size,capacity,count,dropped,failed,reserved0,reserved1;uint64_t epoch,qpc_frequency,reserved;} RpkHeader;
typedef struct{
 uint64_t wall_before,wall_after,sequence,epoch;
 uintptr_t out,left,right,caller,entry_rbp,entry_r12,reader,actor,provider,clock;
 int64_t query,t0,t1,time_before,time_after,clock_dt_before,clock_dt_after,cursor;
 uintptr_t result;
 uint32_t frame_before,frame_after,thread,flags,captured_size,allocation_size_verified,mxcsr_before,mxcsr_after;
 float xmm0_before[4];uint64_t reserved;
 _Alignas(16)unsigned char out_before[64],out_after[64],left_before[64],right_before[64],left_after[64],right_after[64];
} RpkRecord;
_Static_assert(sizeof(RpkHeader)==64,"RPK header64");
_Static_assert(sizeof(RpkRecord)==624,"RPK record624");
static int pqn_enabled;
static int pqn_native_main_return(uintptr_t,uintptr_t);
static void pqn_packet_mark(RpkRecord*);
typedef struct{RpkRecord record;} RpkScope;
static _Thread_local RpkScope rpkt_scopes[8];static _Thread_local unsigned rpkt_depth;
static __attribute__((used)) uintptr_t rpkt_original;
static wchar_t rpkt_path[4096];static HANDLE rpkt_file=INVALID_HANDLE_VALUE;
static SRWLOCK rpkt_file_lock=SRWLOCK_INIT;static RpkHeader rpkt_header;static volatile LONG64 rpkt_sequence;
static __attribute__((naked,used)) ABI void rpkt_post_bridge(void);
enum{RPK_OUT_BEFORE=1,RPK_LEFT_BEFORE=2,RPK_RIGHT_BEFORE=4,RPK_OUT_AFTER=8,RPK_LEFT_AFTER=16,RPK_RIGHT_AFTER=32,RPK_CLOCK_BEFORE=64,RPK_CLOCK_AFTER=128,RPK_CLOCK_STABLE=256,RPK_NATIVE_CALLSITE=512,RPK_REPLAY_CONTEXT=1024,RPK_EXPECTED_VPTR=2048,RPK_LEFT_UNCHANGED=4096,RPK_RIGHT_UNCHANGED=8192};
static int rpl_binding_ready(uintptr_t,uintptr_t,uintptr_t,uint64_t*);
#include "runtime-native-packet-cache.inc"
static uint64_t rpkt_qpc(void){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return(uint64_t)(t.QuadPart/f.QuadPart)*1000000u+(uint64_t)(t.QuadPart%f.QuadPart)*1000000u/(uint64_t)f.QuadPart;}
static int rpkt_clock(uintptr_t*clock,int64_t*time,int64_t*dt,uint32_t*frame){uintptr_t p=0;return camera_pose_read((uintptr_t)image+0x1911100,&p,8)&&camera_pose_read(p+0x970,&p,8)&&camera_pose_read(p+0x1e0,clock,8)&&camera_pose_read(*clock+0x58,time,8)&&camera_pose_read(*clock+0x60,dt,8)&&camera_pose_read(*clock+0x21c,frame,4);}

static void rpkt_write(const RpkRecord*r){
 (void)r;
}
static __attribute__((used,noinline)) ABI void rpkt_before(unsigned char*context){
 unsigned depth=rpkt_depth;if(depth>=8)return;++rpkt_depth;RpkRecord*r=&rpkt_scopes[depth].record;memset(r,0,sizeof*r);
 r->sequence=(uint64_t)InterlockedIncrement64(&rpkt_sequence);r->epoch=1;r->captured_size=64;r->allocation_size_verified=0;r->thread=GetCurrentThreadId();r->wall_before=rpkt_path[0]?rpkt_qpc():0;
 memcpy(&r->out,context+1096,8);memcpy(&r->left,context+1104,8);memcpy(&r->right,context+1128,8);memcpy(&r->query,context+1136,8);memcpy(&r->t0,context+1088,8);memcpy(&r->t1,context+1080,8);memcpy(&r->caller,context+1160,8);memcpy(&r->entry_rbp,context+1112,8);memcpy(&r->entry_r12,context+1056,8);memcpy(&r->mxcsr_before,context+24,4);memcpy(r->xmm0_before,context+512,16);
 if(camera_pose_read(r->out,r->out_before,64))r->flags|=RPK_OUT_BEFORE;if(camera_pose_read(r->left,r->left_before,64))r->flags|=RPK_LEFT_BEFORE;if(camera_pose_read(r->right,r->right_before,64))r->flags|=RPK_RIGHT_BEFORE;
 if(rpkt_clock(&r->clock,&r->time_before,&r->clock_dt_before,&r->frame_before))r->flags|=RPK_CLOCK_BEFORE;
 if(r->caller==(uintptr_t)image+0x76653a){r->flags|=RPK_NATIVE_CALLSITE;r->reader=r->entry_r12;uintptr_t outer=0,returnpc=0;
  if(camera_pose_read(r->entry_rbp,&outer,8)&&camera_pose_read(r->entry_rbp+8,&returnpc,8)&&pqn_native_main_return(returnpc,outer)&&camera_pose_read(r->entry_rbp-0x20,&r->actor,8)&&camera_pose_read(outer-0xe0,&r->provider,8)){r->flags|=RPK_REPLAY_CONTEXT;camera_pose_read(r->provider+0xb0,&r->cursor,8);}
 }
 uintptr_t vt[3]={0};memcpy(vt,r->out_before,8);memcpy(vt+1,r->left_before,8);memcpy(vt+2,r->right_before,8);if(vt[0]==(uintptr_t)image+0x15f0fd0&&vt[1]==vt[0]&&vt[2]==vt[0])r->flags|=RPK_EXPECTED_VPTR;
 pqn_packet_mark(r);rpq_packet_before(r,depth);
 /* Keep the ORIGINAL native function's RSP/frame addresses. A nested CALL
  * would shift native stack-derived scratch outputs. Replace only its return
  * slot until postcapture restores the exact caller address and returns. */
 uintptr_t post=(uintptr_t)rpkt_post_bridge;memcpy(context+1160,&post,8);
}
static __attribute__((used,noinline)) ABI void rpkt_after(unsigned char*context){
 if(!rpkt_depth)return;unsigned depth=--rpkt_depth;if(depth>=8)return;RpkRecord*r=&rpkt_scopes[depth].record;
 memcpy(&r->result,context+1144,8);memcpy(&r->mxcsr_after,context+24,4);memcpy(context+1160,&r->caller,8);
 if(camera_pose_read(r->out,r->out_after,64))r->flags|=RPK_OUT_AFTER;if(camera_pose_read(r->left,r->left_after,64))r->flags|=RPK_LEFT_AFTER;if(camera_pose_read(r->right,r->right_after,64))r->flags|=RPK_RIGHT_AFTER;
 uintptr_t clock=0;if(rpkt_clock(&clock,&r->time_after,&r->clock_dt_after,&r->frame_after))r->flags|=RPK_CLOCK_AFTER;
 if((r->flags&(RPK_CLOCK_BEFORE|RPK_CLOCK_AFTER))==(RPK_CLOCK_BEFORE|RPK_CLOCK_AFTER)&&clock==r->clock&&r->time_before==r->time_after&&r->clock_dt_before==r->clock_dt_after&&r->frame_before==r->frame_after)r->flags|=RPK_CLOCK_STABLE;
 if((r->flags&(RPK_LEFT_BEFORE|RPK_LEFT_AFTER))==(RPK_LEFT_BEFORE|RPK_LEFT_AFTER)&&!memcmp(r->left_before,r->left_after,64))r->flags|=RPK_LEFT_UNCHANGED;
 if((r->flags&(RPK_RIGHT_BEFORE|RPK_RIGHT_AFTER))==(RPK_RIGHT_BEFORE|RPK_RIGHT_AFTER)&&!memcmp(r->right_before,r->right_after,64))r->flags|=RPK_RIGHT_UNCHANGED;
 r->wall_after=rpkt_path[0]?rpkt_qpc():0;rpq_packet_after(r,depth);rpkt_write(r);float data[24]={0};memcpy(data,&r->query,8);memcpy(data+2,&r->t0,8);memcpy(data+4,&r->t1,8);memcpy(data+6,&r->time_before,8);memcpy(data+8,&r->time_after,8);memcpy(data+10,&r->flags,4);memcpy(data+11,&r->sequence,8);memcpy(data+13,&r->reader,8);memcpy(data+15,&r->provider,8);memcpy(data+17,&r->cursor,8);data[19]=64;data[20]=(float)(r->result&255);camera_accuracy_log(41,(void*)r->actor,r->out,r->caller,0,data,21);
}
static __attribute__((naked,used)) ABI void rpkt_bridge(void){__asm__ volatile(
 "pushfq\n"
 "pushq %rax\n"
 "pushq %rcx\n"
 "pushq %rdx\n"
 "pushq %rbx\n"
 "pushq %rbp\n"
 "pushq %rsi\n"
 "pushq %rdi\n"
 "pushq %r8\n"
 "pushq %r9\n"
 "pushq %r10\n"
 "pushq %r11\n"
 "pushq %r12\n"
 "pushq %r13\n"
 "pushq %r14\n"
 "pushq %r15\n"
 "subq $1032,%rsp\n"
 "fxsave64 0(%rsp)\n"
 "vmovdqu %ymm0,512(%rsp)\n"
 "vmovdqu %ymm1,544(%rsp)\n"
 "vmovdqu %ymm2,576(%rsp)\n"
 "vmovdqu %ymm3,608(%rsp)\n"
 "vmovdqu %ymm4,640(%rsp)\n"
 "vmovdqu %ymm5,672(%rsp)\n"
 "vmovdqu %ymm6,704(%rsp)\n"
 "vmovdqu %ymm7,736(%rsp)\n"
 "vmovdqu %ymm8,768(%rsp)\n"
 "vmovdqu %ymm9,800(%rsp)\n"
 "vmovdqu %ymm10,832(%rsp)\n"
 "vmovdqu %ymm11,864(%rsp)\n"
 "vmovdqu %ymm12,896(%rsp)\n"
 "vmovdqu %ymm13,928(%rsp)\n"
 "vmovdqu %ymm14,960(%rsp)\n"
 "vmovdqu %ymm15,992(%rsp)\n"
 "movq %rsp,%rdi\n"
 "callq rpkt_before\n"
 "fxrstor64 0(%rsp)\n"
 "vmovdqu 512(%rsp),%ymm0\n"
 "vmovdqu 544(%rsp),%ymm1\n"
 "vmovdqu 576(%rsp),%ymm2\n"
 "vmovdqu 608(%rsp),%ymm3\n"
 "vmovdqu 640(%rsp),%ymm4\n"
 "vmovdqu 672(%rsp),%ymm5\n"
 "vmovdqu 704(%rsp),%ymm6\n"
 "vmovdqu 736(%rsp),%ymm7\n"
 "vmovdqu 768(%rsp),%ymm8\n"
 "vmovdqu 800(%rsp),%ymm9\n"
 "vmovdqu 832(%rsp),%ymm10\n"
 "vmovdqu 864(%rsp),%ymm11\n"
 "vmovdqu 896(%rsp),%ymm12\n"
 "vmovdqu 928(%rsp),%ymm13\n"
 "vmovdqu 960(%rsp),%ymm14\n"
 "vmovdqu 992(%rsp),%ymm15\n"
 "addq $1032,%rsp\n"
 "popq %r15\n"
 "popq %r14\n"
 "popq %r13\n"
 "popq %r12\n"
 "popq %r11\n"
 "popq %r10\n"
 "popq %r9\n"
 "popq %r8\n"
 "popq %rdi\n"
 "popq %rsi\n"
 "popq %rbp\n"
 "popq %rbx\n"
 "popq %rdx\n"
 "popq %rcx\n"
 "popq %rax\n"
 "popfq\n"
 "jmpq *rpkt_original(%rip)\n"
);}
static __attribute__((naked,used)) ABI void rpkt_post_bridge(void){__asm__ volatile(
 "leaq -8(%rsp),%rsp\n"
 "pushfq\n"
 "pushq %rax\n"
 "pushq %rcx\n"
 "pushq %rdx\n"
 "pushq %rbx\n"
 "pushq %rbp\n"
 "pushq %rsi\n"
 "pushq %rdi\n"
 "pushq %r8\n"
 "pushq %r9\n"
 "pushq %r10\n"
 "pushq %r11\n"
 "pushq %r12\n"
 "pushq %r13\n"
 "pushq %r14\n"
 "pushq %r15\n"
 "subq $1032,%rsp\n"
 "fxsave64 0(%rsp)\n"
 "vmovdqu %ymm0,512(%rsp)\n"
 "vmovdqu %ymm1,544(%rsp)\n"
 "vmovdqu %ymm2,576(%rsp)\n"
 "vmovdqu %ymm3,608(%rsp)\n"
 "vmovdqu %ymm4,640(%rsp)\n"
 "vmovdqu %ymm5,672(%rsp)\n"
 "vmovdqu %ymm6,704(%rsp)\n"
 "vmovdqu %ymm7,736(%rsp)\n"
 "vmovdqu %ymm8,768(%rsp)\n"
 "vmovdqu %ymm9,800(%rsp)\n"
 "vmovdqu %ymm10,832(%rsp)\n"
 "vmovdqu %ymm11,864(%rsp)\n"
 "vmovdqu %ymm12,896(%rsp)\n"
 "vmovdqu %ymm13,928(%rsp)\n"
 "vmovdqu %ymm14,960(%rsp)\n"
 "vmovdqu %ymm15,992(%rsp)\n"
 "movq %rsp,%rdi\n"
 "callq rpkt_after\n"
 "fxrstor64 0(%rsp)\n"
 "vmovdqu 512(%rsp),%ymm0\n"
 "vmovdqu 544(%rsp),%ymm1\n"
 "vmovdqu 576(%rsp),%ymm2\n"
 "vmovdqu 608(%rsp),%ymm3\n"
 "vmovdqu 640(%rsp),%ymm4\n"
 "vmovdqu 672(%rsp),%ymm5\n"
 "vmovdqu 704(%rsp),%ymm6\n"
 "vmovdqu 736(%rsp),%ymm7\n"
 "vmovdqu 768(%rsp),%ymm8\n"
 "vmovdqu 800(%rsp),%ymm9\n"
 "vmovdqu 832(%rsp),%ymm10\n"
 "vmovdqu 864(%rsp),%ymm11\n"
 "vmovdqu 896(%rsp),%ymm12\n"
 "vmovdqu 928(%rsp),%ymm13\n"
 "vmovdqu 960(%rsp),%ymm14\n"
 "vmovdqu 992(%rsp),%ymm15\n"
 "addq $1032,%rsp\n"
 "popq %r15\n"
 "popq %r14\n"
 "popq %r13\n"
 "popq %r12\n"
 "popq %r11\n"
 "popq %r10\n"
 "popq %r9\n"
 "popq %r8\n"
 "popq %rdi\n"
 "popq %rsi\n"
 "popq %rbp\n"
 "popq %rbx\n"
 "popq %rdx\n"
 "popq %rcx\n"
 "popq %rax\n"
 "popfq\n"
 "retq\n"
);}
static __attribute__((naked,used)) ABI uintptr_t rpkt_private_native(void*out,void*left,void*right,int64_t query,int64_t t0,int64_t t1,const float*xmm0){
 __asm__ volatile("movq 8(%rsp),%rax\n vmovups (%rax),%xmm0\n leaq -8(%rsp),%rsp\n callq *rpkt_original(%rip)\n leaq 8(%rsp),%rsp\n retq\n");
}
/* Separate private-kernel API, NEVER called by the read-only observer itself.
 * Uses actual original gateway+native helpers on aligned PRIVATE packet copies.
 * It does not enter a reader, deserialize or alter its cursor/cache. Caller
 * must compare actual captured original-query result before using anotherQ.
 * Opaque host math helper exceptional side effects require separate audit. */
static __attribute__((unused)) int rpkt_private_kernel(const RpkRecord*r,int64_t query,unsigned char output[64],uintptr_t*result){
 if(!rpkt_original||(r->flags&(RPK_OUT_BEFORE|RPK_LEFT_BEFORE|RPK_RIGHT_BEFORE|RPK_EXPECTED_VPTR))!=(RPK_OUT_BEFORE|RPK_LEFT_BEFORE|RPK_RIGHT_BEFORE|RPK_EXPECTED_VPTR)||r->t1<=r->t0||query<r->t0||query>r->t1)return 0;
 if((r->out<=r->left&&r->left-r->out<64)||(r->left<r->out&&r->out-r->left<64)||(r->out<=r->right&&r->right-r->out<64)||(r->right<r->out&&r->out-r->right<64))return 0;
 _Alignas(16)unsigned char out[64],left[64],right[64],fx[512];memcpy(out,r->out_before,64);memcpy(left,r->left_before,64);memcpy(right,r->right_before,64);
 __asm__ volatile("fxsave64 %0\n ldmxcsr %1":"=m"(fx):"m"(r->mxcsr_before):"memory");
 uintptr_t value=rpkt_private_native(out,left,right,query,r->t0,r->t1,r->xmm0_before);
 __asm__ volatile("fxrstor64 %0"::"m"(fx):"memory");
 if(memcmp(left,r->left_before,64)||memcmp(right,r->right_before,64))return 0;memcpy(output,out,64);if(result)*result=value;return 1;
}
static int replay_packet_observer_install(void){
 const char*path=camera_fix_getenv("BB_REPLAY_PACKET_TAPE");if((!path||!*path)&&!rpq_enabled)return 0;
 if(path&&*path){if(MultiByteToWideChar(CP_UTF8,0,path,-1,rpkt_path,4096)<=0)return 0;wchar_t absolute[4096];if(!GetFullPathNameW(rpkt_path,4096,absolute,NULL)||!((absolute[0]=='D'||absolute[0]=='d')&&absolute[1]==':'&&absolute[2]=='\\')){puts("ReplayPacket: diagnostic output must be on D:");return 0;}wcscpy(rpkt_path,absolute);}
 static const unsigned char expected[]={0x55,0x48,0x89,0xe5,0xc4,0xc1,0xfa,0x2a,0xc0,0xc4,0xe1,0xfa,0x2a,0xc9,0xc4,0xc1,0xfa,0x2a,0xd1};
 if(runtime_image_size<0x770de3||memcmp(image+0x770dd0,expected,sizeof expected)){puts("ReplayPacket: original770DD0 signature unavailable; no hook");return 0;}
 unsigned char*gateway=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);if(!gateway)return 0;memcpy(gateway,expected,sizeof expected);uintptr_t resume=(uintptr_t)image+0x770de3;const unsigned char jump[6]={0xff,0x25,0,0,0,0};memcpy(gateway+sizeof expected,jump,6);memcpy(gateway+sizeof expected+6,&resume,8);rpkt_original=(uintptr_t)gateway;
 uintptr_t target=(uintptr_t)rpkt_bridge;memcpy(image+0x770dd0,jump,6);memcpy(image+0x770dd6,&target,8);memset(image+0x770dde,0x90,sizeof expected-14);FlushInstructionCache(GetCurrentProcess(),gateway,sizeof expected+14);FlushInstructionCache(GetCurrentProcess(),image+0x770dd0,sizeof expected);
 puts("ReplayPacket: actual770DD0 once, read-only0x40 input/output snapshots; no extra live reader/query");return 1;
}
