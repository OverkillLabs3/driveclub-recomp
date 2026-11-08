#ifndef CAMERA_HEAD_KERNEL_TAPE_INCLUDED
#define CAMERA_HEAD_KERNEL_TAPE_INCLUDED
/* Paired core input/output transaction; optional BB_CAMERA_HEAD_TAPE=D:\\....
 * Original coupled body/head math B919A1..B91DF5 remains unchanged.
 * Requires image, runtime_image_base/runtime_image_size and camera_pose_read.
 * Optional HKT_FRAME_READ(unsigned*) supplies a measured global frame identity.
 * Captured input is actual effective dt, not an assumed physical/view clock. */
typedef struct {
 unsigned char magic[8];
 uint32_t version,record_size,capacity,count,overflow,unpaired,invalid,skipped_depth;
 uint32_t mxcsr_unknown,flags,hash_lo,hash_hi,reserved0,reserved1;
} HKTHeader;
typedef struct {
 float dt[4],current_linear[4],current_angular[4],delta_linear[4],delta_angular[4];
 float inverse[4],broadcast_dt[4],matrix[16],global_half[4],global_one[4];
} HKTInputs;
typedef struct {
 uint64_t sequence,wall_ms,actor,vehicle,frame_address,matrix_address;
 uint32_t frame,thread,mxcsr_before,mxcsr_after;
 HKTInputs input;
 unsigned char before[0x380],after[0x380];
 uint32_t flags,reserved[3];
} HKTRecord;
_Static_assert(sizeof(HKTHeader)==64,"Head kernel tape header ABI");
_Static_assert(sizeof(HKTInputs)==208,"Head kernel tape input ABI");
_Static_assert(sizeof(HKTRecord)==2080,"Head kernel tape record ABI");
enum {HKT_READ_COMPLETE=1,HKT_FRAME_VALID=2,HKT_NATIVE_KERNEL=4};
static int hpr_group_active_for_head(void);
static void hpr_group_head_before(const HKTRecord*);
static void hpr_group_head_after(const HKTRecord*);
static HKTHeader*hkt_header;static HKTRecord*hkt_records;
static SRWLOCK hkt_lock=SRWLOCK_INIT;
typedef struct {HKTRecord records[4];unsigned depth,extra_depth;} HKTPending;
static _Thread_local HKTPending hkt_pending;
static __attribute__((used)) uintptr_t hkt_before_resume,hkt_after_resume;
static int hkt_installed;
#ifndef HKT_FRAME_READ
static int hkt_frame_read(unsigned*out){uintptr_t p=0;return camera_pose_read(runtime_image_base+0x1911100,&p,8)&&camera_pose_read(p+0x968,&p,8)&&camera_pose_read(p+0x1e0,&p,8)&&camera_pose_read(p+0x21c,out,4);}
#define HKT_FRAME_READ hkt_frame_read
#endif
static uint64_t hkt_hash(const unsigned char*p,size_t n){uint64_t h=UINT64_C(14695981039346656037);while(n--)h=(h^*p++)*UINT64_C(1099511628211);return h;}
static void hkt_counter(uint32_t*p){InterlockedIncrement((volatile LONG*)p);}
static int hkt_mapping(const wchar_t*path){(void)path;return 0;}
#include "head-live-runtime.inc"
static __attribute__((used,noinline)) ABI void hkt_before(void*frame,void*self,void*vehicle,void*matrix,uint32_t mxcsr){
 if(!hkt_header&&!hcl_enabled&&!hpr_group_active_for_head())return;
 if(hkt_pending.depth>=4){++hkt_pending.extra_depth;if(hkt_header)hkt_counter(&hkt_header->skipped_depth);return;}
 HKTRecord*r=hkt_pending.records+hkt_pending.depth++;memset(r,0,sizeof *r);
 r->wall_ms=GetTickCount64();r->actor=(uintptr_t)self;r->vehicle=(uintptr_t)vehicle;r->frame_address=(uintptr_t)frame;r->matrix_address=(uintptr_t)matrix;
 r->thread=GetCurrentThreadId();r->mxcsr_before=mxcsr;r->frame=UINT32_MAX;r->flags=HKT_NATIVE_KERNEL;
 if(HKT_FRAME_READ(&r->frame))r->flags|=HKT_FRAME_VALID;
 uintptr_t fp=(uintptr_t)frame;
 hpr_group_head_before(r); /* epoch opens BEFORE actual input snapshot */
 int ok=camera_pose_read((uintptr_t)self,r->before,sizeof r->before)&&
  camera_pose_read(fp-0xf0,r->input.dt,16)&&camera_pose_read(fp-0x40,r->input.current_linear,16)&&camera_pose_read(fp-0x50,r->input.current_angular,16)&&
  camera_pose_read(fp-0x110,r->input.delta_linear,16)&&camera_pose_read(fp-0x120,r->input.delta_angular,16)&&camera_pose_read(fp-0x130,r->input.inverse,16)&&
  camera_pose_read(fp-0x100,r->input.broadcast_dt,16)&&camera_pose_read((uintptr_t)matrix,r->input.matrix,64)&&
  camera_pose_read(runtime_image_base+0x191b0f0,r->input.global_half,16)&&camera_pose_read(runtime_image_base+0x191b100,r->input.global_one,16);
 if(ok)r->flags|=HKT_READ_COMPLETE;else if(hkt_header)hkt_counter(&hkt_header->invalid);
 #ifdef HKT_TEST_CLOBBER
 HKT_TEST_CLOBBER();
 #endif
}
static __attribute__((used,noinline)) ABI unsigned hkt_after(void*frame,void*self,void*vehicle,void*matrix,uint32_t mxcsr){
 (void)vehicle;(void)matrix;if(!hkt_header&&!hcl_enabled&&!hpr_group_active_for_head())return 0;
 if(hkt_pending.extra_depth){--hkt_pending.extra_depth;return 0;}
 if(!hkt_pending.depth){if(hkt_header)hkt_counter(&hkt_header->unpaired);return 0;}
 HKTRecord*r=hkt_pending.records+hkt_pending.depth-1;
 if(r->frame_address!=(uintptr_t)frame||r->actor!=(uintptr_t)self){if(hkt_header)hkt_counter(&hkt_header->unpaired);return 0;}
 --hkt_pending.depth;r->mxcsr_after=mxcsr;
 if(!camera_pose_read((uintptr_t)self,r->after,sizeof r->after)){if(hkt_header)hkt_counter(&hkt_header->invalid);return 0;}
 if(hkt_header){ AcquireSRWLockExclusive(&hkt_lock);uint32_t at=hkt_header->count;
 if(at<hkt_header->capacity){r->sequence=at;hkt_records[at]=*r;MemoryBarrier();hkt_header->count=at+1;}else ++hkt_header->overflow;
 ReleaseSRWLockExclusive(&hkt_lock);}
 hpr_group_head_after(r);
 int applied=hcl_apply(r);
 #ifdef HKT_TEST_CLOBBER
 HKT_TEST_CLOBBER();
 #endif
 return (unsigned)applied;
}
static __attribute__((naked,used)) ABI void hkt_before_bridge(void){
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
  "subq $1024, %rsp\n"
  "fxsave64 512(%rsp)\n"
  "vmovdqu %ymm0, 0(%rsp)\n"
  "vmovdqu %ymm1, 32(%rsp)\n"
  "vmovdqu %ymm2, 64(%rsp)\n"
  "vmovdqu %ymm3, 96(%rsp)\n"
  "vmovdqu %ymm4, 128(%rsp)\n"
  "vmovdqu %ymm5, 160(%rsp)\n"
  "vmovdqu %ymm6, 192(%rsp)\n"
  "vmovdqu %ymm7, 224(%rsp)\n"
  "vmovdqu %ymm8, 256(%rsp)\n"
  "vmovdqu %ymm9, 288(%rsp)\n"
  "vmovdqu %ymm10, 320(%rsp)\n"
  "vmovdqu %ymm11, 352(%rsp)\n"
  "vmovdqu %ymm12, 384(%rsp)\n"
  "vmovdqu %ymm13, 416(%rsp)\n"
  "vmovdqu %ymm14, 448(%rsp)\n"
  "vmovdqu %ymm15, 480(%rsp)\n"
  "cld\n"
  "movq %rbp, %rdi\n"
  "movq %r15, %rsi\n"
  "movq %r14, %rdx\n"
  "movq 1088(%rsp), %rcx\n"
  "movl 536(%rsp), %r8d\n"
  "callq hkt_before\n"
  "fxrstor64 512(%rsp)\n"
  "vmovdqu 0(%rsp), %ymm0\n"
  "vmovdqu 32(%rsp), %ymm1\n"
  "vmovdqu 64(%rsp), %ymm2\n"
  "vmovdqu 96(%rsp), %ymm3\n"
  "vmovdqu 128(%rsp), %ymm4\n"
  "vmovdqu 160(%rsp), %ymm5\n"
  "vmovdqu 192(%rsp), %ymm6\n"
  "vmovdqu 224(%rsp), %ymm7\n"
  "vmovdqu 256(%rsp), %ymm8\n"
  "vmovdqu 288(%rsp), %ymm9\n"
  "vmovdqu 320(%rsp), %ymm10\n"
  "vmovdqu 352(%rsp), %ymm11\n"
  "vmovdqu 384(%rsp), %ymm12\n"
  "vmovdqu 416(%rsp), %ymm13\n"
  "vmovdqu 448(%rsp), %ymm14\n"
  "vmovdqu 480(%rsp), %ymm15\n"
  "addq $1024, %rsp\n"
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
  "jmpq *hkt_before_resume(%rip)\n"
 );
}
static __attribute__((naked,used)) ABI void hkt_after_bridge(void){
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
  "subq $1024, %rsp\n"
  "fxsave64 512(%rsp)\n"
  "vmovdqu %ymm0, 0(%rsp)\n"
  "vmovdqu %ymm1, 32(%rsp)\n"
  "vmovdqu %ymm2, 64(%rsp)\n"
  "vmovdqu %ymm3, 96(%rsp)\n"
  "vmovdqu %ymm4, 128(%rsp)\n"
  "vmovdqu %ymm5, 160(%rsp)\n"
  "vmovdqu %ymm6, 192(%rsp)\n"
  "vmovdqu %ymm7, 224(%rsp)\n"
  "vmovdqu %ymm8, 256(%rsp)\n"
  "vmovdqu %ymm9, 288(%rsp)\n"
  "vmovdqu %ymm10, 320(%rsp)\n"
  "vmovdqu %ymm11, 352(%rsp)\n"
  "vmovdqu %ymm12, 384(%rsp)\n"
  "vmovdqu %ymm13, 416(%rsp)\n"
  "vmovdqu %ymm14, 448(%rsp)\n"
  "vmovdqu %ymm15, 480(%rsp)\n"
  "cld\n"
  "movq %rbp, %rdi\n"
  "movq %r15, %rsi\n"
  "movq %r14, %rdx\n"
  "movq 1088(%rsp), %rcx\n"
  "movl 536(%rsp), %r8d\n"
  "callq hkt_after\n"
  "testl %eax, %eax\n"
  "jz 1f\n"
  "vmovups 0x1d0(%r15), %xmm0\n"
  "vmovups %xmm0, 0(%rsp)\n"
  "vmovups 0x1b0(%r15), %xmm0\n"
  "vmovups %xmm0, 96(%rsp)\n"
  "1:\n"
  "fxrstor64 512(%rsp)\n"
  "vmovdqu 0(%rsp), %ymm0\n"
  "vmovdqu 32(%rsp), %ymm1\n"
  "vmovdqu 64(%rsp), %ymm2\n"
  "vmovdqu 96(%rsp), %ymm3\n"
  "vmovdqu 128(%rsp), %ymm4\n"
  "vmovdqu 160(%rsp), %ymm5\n"
  "vmovdqu 192(%rsp), %ymm6\n"
  "vmovdqu 224(%rsp), %ymm7\n"
  "vmovdqu 256(%rsp), %ymm8\n"
  "vmovdqu 288(%rsp), %ymm9\n"
  "vmovdqu 320(%rsp), %ymm10\n"
  "vmovdqu 352(%rsp), %ymm11\n"
  "vmovdqu 384(%rsp), %ymm12\n"
  "vmovdqu 416(%rsp), %ymm13\n"
  "vmovdqu 448(%rsp), %ymm14\n"
  "vmovdqu 480(%rsp), %ymm15\n"
  "addq $1024, %rsp\n"
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
  "jmpq *hkt_after_resume(%rip)\n"
 );
}

static void hkt_jump(unsigned char*p,uintptr_t dest){memcpy(p,"\xff\x25\0\0\0\0",6);memcpy(p+6,&dest,8);}
static int hkt_signatures(void){
 static const unsigned char pre[]={0xc5,0x78,0x28,0xbd,0x10,0xff,0xff,0xff,0xc5,0x78,0x2e,0x3d,0x63,0xfc,0x7b,0x00};
 static const unsigned char post[]={0xc4,0xc1,0x7a,0x10,0x4f,0x68,0xc4,0x41,0x38,0x57,0xc0,0xc5,0xba,0x10,0xc9};
 static const unsigned char call[]={0x49,0x8b,0x06,0x4c,0x89,0xf7,0xff,0x50,0x60};
 return runtime_image_size>0x191b110&&!memcmp(image+0xb91998,call,sizeof call)&&!memcmp(image+0xb919a1,pre,sizeof pre)&&!memcmp(image+0xb91df5,post,sizeof post)&&
  hkt_hash(image+0xb919a1,0xb91df5-0xb919a1)==UINT64_C(0x61f9209a41ef3d57);
}
static int hkt_install(void){
 if(hkt_installed)return 1;wchar_t path[4096];DWORD n=0;
 int valid_path=n>=4&&n<4096&&((path[0]=='D'||path[0]=='d')&&path[1]==':'&&(path[2]=='\\'||path[2]=='/'));
 const char*requested=camera_fix_getenv("BB_CAMERA_HEAD_REFERENCE");if(!valid_path&&(!requested||atoi(requested)!=1)&&!hpr_group_active_for_head())return 0;
 if(!hkt_signatures()){puts("HeadKernelTape: unsupported original coupled-kernel signature; observer not installed");return 0;}
 unsigned char*code=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!code)return 0;
 /* Relocate original first16 bytes. VMOVAPS is copied; RIP-relative VUCOMISS is
    replaced by the exact same compare through saved RAX, preserving flags/RAX.
    No +/-2GB allocation assumption and no register-clobbering resume jump. */
 memcpy(code,image+0xb919a1,8);code[8]=0x50;code[9]=0x48;code[10]=0xb8;uintptr_t constant=runtime_image_base+0x1351614;memcpy(code+11,&constant,8);
 memcpy(code+19,"\xc5\x78\x2e\x38\x58",5);hkt_jump(code+24,runtime_image_base+0xb919b1);
 memcpy(code+64,image+0xb91df5,15);hkt_jump(code+79,runtime_image_base+0xb91e04);
 DWORD old;if(!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(code,0,MEM_RELEASE);return 0;}
 hcl_install();
 int mapped=valid_path&&hkt_mapping(path);
 if(!mapped&&!hcl_enabled&&!hpr_group_active_for_head()){VirtualFree(code,0,MEM_RELEASE);return 0;}
 if(valid_path&&!mapped)puts("HeadKernel: trace mapping unavailable; ready runtime remains active");
 hkt_before_resume=(uintptr_t)code;hkt_after_resume=(uintptr_t)(code+64);
 if(!VirtualProtect(image+0xb919a1,0xb91e04-0xb919a1,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(code,0,MEM_RELEASE);return 0;}
 hkt_jump(image+0xb919a1,(uintptr_t)hkt_before_bridge);memset(image+0xb919a1+14,0x90,2);
 hkt_jump(image+0xb91df5,(uintptr_t)hkt_after_bridge);image[0xb91df5+14]=0x90;
 DWORD unused;VirtualProtect(image+0xb919a1,0xb91e04-0xb919a1,old,&unused);
 FlushInstructionCache(GetCurrentProcess(),code,93);FlushInstructionCache(GetCurrentProcess(),image+0xb919a1,0xb91e04-0xb919a1);hkt_installed=1;
 puts(hkt_header?"HeadKernel: coupled runtime/optional original transaction trace active":"HeadKernel: coupled runtime active without diagnostic mapping");return 1;
}
#endif
