#ifndef CAMERA_HEAD_ANGULAR_TAPE_INCLUDED
#define CAMERA_HEAD_ANGULAR_TAPE_INCLUDED
/* Read-only optional diagnostic. No calls to live lookup/Euler outside original
 * helper. BB_CAMERA_HEAD_ANGULAR_TAPE must be absolute D: path. */
typedef struct {unsigned char magic[8];uint32_t version,record_size,capacity,count,overflow,unpaired,invalid,skipped_depth;uint32_t reserved[6];} HATHeader;
typedef struct {
 uint64_t sequence,wall_ms,camera,target;
 uint32_t frame,thread,mxcsr_pre,mxcsr_mid,mxcsr_post,flags;
 float dt,dt_math,reserved_dt[2],euler_consumed[4],composition_global[16];
 unsigned char target_consumed[0x150],before[0x3a0],mid[0x3a0],after[0x3a0];
 uint32_t reserved[2];
} HATRecord;
_Static_assert(sizeof(HATHeader)==64,"Angular tape header ABI");
_Static_assert(sizeof(HATRecord)==3280,"Angular tape record ABI");
enum {HAT_PRE=1,HAT_MID=2,HAT_POST=4,HAT_FRAME=8};
static HATHeader*hat_header;static HATRecord*hat_records;static SRWLOCK hat_lock=SRWLOCK_INIT;
typedef struct {HATRecord r[4];uintptr_t frame_address[4];unsigned depth,extra_depth;} HATPending;
static _Thread_local HATPending hat_pending;
static __attribute__((used))uintptr_t hat_pre_resume,hat_mid_resume,hat_post_resume;
static int hat_installed;
static uint64_t hat_hash(const unsigned char*p,size_t n){uint64_t h=UINT64_C(14695981039346656037);while(n--)h=(h^*p++)*UINT64_C(1099511628211);return h;}
static void hat_jump(unsigned char*p,uintptr_t target){memcpy(p,"\xff\x25\0\0\0\0",6);memcpy(p+6,&target,8);}
static int hat_frame_read(unsigned*out){uintptr_t p=0;return camera_pose_read(runtime_image_base+0x1911100,&p,8)&&camera_pose_read(p+0x968,&p,8)&&camera_pose_read(p+0x1e0,&p,8)&&camera_pose_read(p+0x21c,out,4);}
static void hat_counter(uint32_t*p){InterlockedIncrement((volatile LONG*)p);}
static void hcal_apply(const HATRecord*);
static void hai_raw_solver_input(void*,void*,void*,uint32_t);
static __attribute__((used,noinline)) ABI void hat_capture(unsigned stage,void*frame,void*self,void*target,uint32_t mxcsr,const unsigned char*saved_ymm){
 if(stage==1)hai_raw_solver_input(frame,self,target,mxcsr); if(!hat_header)return;HATRecord*r;
 if(stage==0){if(hat_pending.depth>=4){++hat_pending.extra_depth;hat_counter(&hat_header->skipped_depth);return;}
  unsigned at=hat_pending.depth++;r=hat_pending.r+at;memset(r,0,sizeof *r);hat_pending.frame_address[at]=(uintptr_t)frame;r->wall_ms=GetTickCount64();r->camera=(uintptr_t)self;r->thread=GetCurrentThreadId();r->frame=UINT32_MAX;r->mxcsr_pre=mxcsr;memcpy(&r->dt,saved_ymm,4);
  if(hat_frame_read(&r->frame))r->flags|=HAT_FRAME;
  if(camera_pose_read((uintptr_t)self,r->before,sizeof r->before)&&camera_pose_read(runtime_image_base+0x27e9460,r->composition_global,64))r->flags|=HAT_PRE;else hat_counter(&hat_header->invalid);
 }else{
  if(hat_pending.extra_depth){if(stage==2)--hat_pending.extra_depth;return;}
  if(!hat_pending.depth){hat_counter(&hat_header->unpaired);return;}
  unsigned at=hat_pending.depth-1;r=hat_pending.r+at;
  if(r->camera!=(uintptr_t)self||hat_pending.frame_address[at]!=(uintptr_t)frame){hat_counter(&hat_header->unpaired);return;}
  if(stage==1){r->target=(uintptr_t)target;r->mxcsr_mid=mxcsr;
   int ok=camera_pose_read((uintptr_t)self,r->mid,sizeof r->mid)&&camera_pose_read((uintptr_t)target,r->target_consumed,sizeof r->target_consumed)&&camera_pose_read((uintptr_t)frame-0x40,&r->dt_math,4);
   for(unsigned i=0;i<3;++i)ok=ok&&camera_pose_read((uintptr_t)frame-0x1c-4*i,r->euler_consumed+i,4);
   if(ok)r->flags|=HAT_MID;else hat_counter(&hat_header->invalid);
  }else if(stage==2){--hat_pending.depth;r->mxcsr_post=mxcsr;
   if(camera_pose_read((uintptr_t)self,r->after,sizeof r->after))r->flags|=HAT_POST;else hat_counter(&hat_header->invalid);
   AcquireSRWLockExclusive(&hat_lock);unsigned n=hat_header->count;if(n<hat_header->capacity){r->sequence=n;hat_records[n]=*r;MemoryBarrier();hat_header->count=n+1;}else ++hat_header->overflow;ReleaseSRWLockExclusive(&hat_lock);hcal_apply(r);
  }
 }
 #ifdef HAT_TEST_CLOBBER
 HAT_TEST_CLOBBER();
 #endif
}
static __attribute__((naked,used)) ABI void hat_pre_bridge(void){
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
  "movl $0, %edi\n"
  "movq %rbp, %rsi\n"
  "movq %rbx, %rdx\n"
  "movq 1088(%rsp), %rcx\n"
  "movl 536(%rsp), %r8d\n"
  "movq %rsp, %r9\n"
  "callq hat_capture\n"
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
  "jmpq *hat_pre_resume(%rip)\n"
 );
}
static __attribute__((naked,used)) ABI void hat_mid_bridge(void){
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
  "movl $1, %edi\n"
  "movq %rbp, %rsi\n"
  "movq %rbx, %rdx\n"
  "movq 1088(%rsp), %rcx\n"
  "movl 536(%rsp), %r8d\n"
  "movq %rsp, %r9\n"
  "callq hat_capture\n"
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
  "jmpq *hat_mid_resume(%rip)\n"
 );
}
static __attribute__((naked,used)) ABI void hat_post_bridge(void){
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
  "movl $2, %edi\n"
  "movq %rbp, %rsi\n"
  "movq %rbx, %rdx\n"
  "movq 1088(%rsp), %rcx\n"
  "movl 536(%rsp), %r8d\n"
  "movq %rsp, %r9\n"
  "callq hat_capture\n"
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
  "jmpq *hat_post_resume(%rip)\n"
 );
}

static int hat_signatures(void){
 static const unsigned char pre[]={0xc5,0xfa,0x59,0x8b,0x4c,0x03,0,0,0xc5,0xf2,0x58,0x8b,0x40,0x03,0,0};
 static const unsigned char mid[]={0xc5,0xfa,0x10,0x45,0xe4,0xc5,0xfa,0x10,0x93,0x40,0x03,0,0,0xc5,0xfa,0x5c,0xc2};
 static const unsigned char post[]={0x49,0x8b,0x06,0x48,0x3b,0x45,0xe8,0x75,0x09,0x48,0x83,0xc4,0x70,0x5b,0x41,0x5e,0x5d,0xc3};
 static const unsigned char euler_call[]={0xe8,0xc2,0xa2,0x95,0xff},target_call[]={0xe8,0x58,0xfc,0xfa,0xff};
 return runtime_image_size>0x27e94a0&&!memcmp(image+0xd19dec,pre,16)&&!memcmp(image+0xd19ed8,mid,17)&&!memcmp(image+0xd1a5a5,post,18)&&!memcmp(image+0xd19eb9,euler_call,5)&&!memcmp(image+0xd19ed3,target_call,5)&&hat_hash(image+0xd19dd0,2028)==UINT64_C(0x31c37121c2ca54b6);
}
static int hat_install(void){
 if(hat_installed)return 1;const char*init=camera_fix_getenv("BB_CAMERA_ANGULAR_INIT_SHARED_Q");if(!(init&&atoi(init)==1))return 0;
 if(!hat_signatures()){puts("HeadAngularTape: unsupported original helper; no observer installed");return 0;}
 unsigned char*c=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!c)return 0;
 memcpy(c,image+0xd19dec,16);hat_jump(c+16,runtime_image_base+0xd19dfc);
 memcpy(c+64,image+0xd19ed8,17);hat_jump(c+81,runtime_image_base+0xd19ee9);
 /* Original canary test + exact normal epilogue; relocate only its shortJNE.
    Failure still enters original stack-canary failure call. No near assumption. */
 memcpy(c+128,image+0xd1a5a5,7);memcpy(c+135,"\x0f\x85\x09\0\0\0",6);memcpy(c+141,image+0xd1a5ae,9);hat_jump(c+150,runtime_image_base+0xd1a5b7);
 DWORD old;if(!VirtualProtect(c,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(c,0,MEM_RELEASE);return 0;}

 hat_pre_resume=(uintptr_t)c;hat_mid_resume=(uintptr_t)c+64;hat_post_resume=(uintptr_t)c+128;
 if(!VirtualProtect(image+0xd19dec,0xd1a5b5-0xd19dec,PAGE_EXECUTE_READWRITE,&old))return 0;
 hat_jump(image+0xd19dec,(uintptr_t)hat_pre_bridge);memset(image+0xd19dec+14,0x90,2);
 hat_jump(image+0xd19ed8,(uintptr_t)hat_mid_bridge);memset(image+0xd19ed8+14,0x90,3);
 hat_jump(image+0xd1a5a5,(uintptr_t)hat_post_bridge);memset(image+0xd1a5a5+14,0x90,2);
 DWORD unused;VirtualProtect(image+0xd19dec,0xd1a5b5-0xd19dec,old,&unused);FlushInstructionCache(GetCurrentProcess(),c,164);FlushInstructionCache(GetCurrentProcess(),image+0xd19dec,0xd1a5b5-0xd19dec);hat_installed=1;
 puts("HeadAngularTape: read-only original angular helper pre/consumed-input/post observer enabled (D: only)");return 1;
}
#endif
