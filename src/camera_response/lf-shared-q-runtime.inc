/* Integration dependencies: LfTapeScope,lf_tape_current,lf_tape_read,
 * camera_accuracy_log,hcl_basis,crs_native_us and original LF wrappers.
 * All state is private; no writes to physical state, target or HF owner. */
#include "lf-shared-q-core.inc"
static int lf_shared_q_enabled;
static LFQHistory lf_shared_q_history[64];static SRWLOCK lf_shared_q_lock=SRWLOCK_INIT;
static _Thread_local LFQInput lf_shared_q_selected;
static volatile LONG64 lf_shared_q_epoch;
static _Thread_local LONG64 lf_shared_q_selected_epoch;
static LONG64 lf_shared_q_read_epoch(void){return InterlockedCompareExchange64(&lf_shared_q_epoch,0,0);}
static int lf_shared_q_current(void){return lf_shared_q_enabled&&lf_shared_q_selected.valid&&!lf_shared_q_selected.native_q&&lf_shared_q_selected.due&&lf_shared_q_selected_epoch==lf_shared_q_read_epoch();}
static void lf_shared_q_forget_all(void){AcquireSRWLockExclusive(&lf_shared_q_lock);InterlockedIncrement64(&lf_shared_q_epoch);memset(lf_shared_q_history,0,sizeof lf_shared_q_history);ReleaseSRWLockExclusive(&lf_shared_q_lock);memset(&lf_shared_q_selected,0,sizeof lf_shared_q_selected);}
static void lf_shared_q_target_impl(LfTapeScope*scope,void*camera,uintptr_t target){
 memset(&lf_shared_q_selected,0,sizeof lf_shared_q_selected);
 if(!lf_shared_q_enabled||!scope||!scope->input_scope||!target)return;LONG64 observation_epoch=lf_shared_q_read_epoch();
 uintptr_t clock=0,binding=0,service=0,gain=0;int64_t t=0,us=0,dt_us=0;uint32_t frame=0,kind=0,target_frame=0,target_kind=0;LFQSample raw={0};
 if(!crs_native_us(scope->dt,&dt_us)||!lf_tape_read((uintptr_t)image+0x1911100,&service,8)||!lf_tape_read(service+0x968,&service,8)||!lf_tape_read(service+0x1e0,&clock,8)||!lf_tape_read(clock+0x58,&t,8)||!lf_tape_read(clock+0x60,&us,8)||!lf_tape_read(clock+0x21c,&frame,4)||dt_us!=us||!lf_tape_read(target+0x44,&target_frame,4)||target_frame!=frame||!lf_tape_read(target+8,&target_kind,4)||target_kind!=1||!lf_tape_read(target+0x28,&binding,8)||!lf_tape_read(target+0x120,raw.velocity,12)||!lf_tape_read(target+0xd0,raw.basis,48)||!lf_tape_read(target+0x19c,&raw.speed,4)||!lf_tape_read((uintptr_t)camera+0xc,&kind,4)||!lf_tape_read(service+0x788,&gain,8)||!lf_tape_read(gain+0x20,&raw.gain,4))return;
 raw.basis[3]=raw.basis[7]=raw.basis[11]=0;raw.basis[12]=raw.basis[13]=raw.basis[14]=0;raw.basis[15]=1;raw.t=t;raw.has_camera_source=kind==12||kind==13;
 if(raw.has_camera_source&&!lf_tape_read((uintptr_t)camera+(kind==12?0x380:0x5d0),&raw.camera_source,4))return;
 float quat[4];if(!crr_rigid_quaternion(raw.basis,quat))return;
 AcquireSRWLockExclusive(&lf_shared_q_lock);if(observation_epoch!=lf_shared_q_read_epoch()){ReleaseSRWLockExclusive(&lf_shared_q_lock);return;}LFQHistory*h=NULL;
 for(unsigned i=0;i<64;++i)if(lf_shared_q_history[i].camera==(uintptr_t)camera&&lf_shared_q_history[i].target==target){h=lf_shared_q_history+i;break;}
 if(!h)for(unsigned i=0;i<64;++i)if(!lf_shared_q_history[i].camera){h=lf_shared_q_history+i;break;}
 lf_shared_q_selected_epoch=observation_epoch;
 if(h)lf_shared_q_selected=lfq_advance(h,(uintptr_t)camera,target,binding,clock,frame,us,&raw,hcl_basis);
 ReleaseSRWLockExclusive(&lf_shared_q_lock);
 float data[24]={(float)lf_shared_q_selected.valid,(float)lf_shared_q_selected.native_q,(float)lf_shared_q_selected.due};memcpy(data+3,&t,8);memcpy(data+5,&lf_shared_q_selected.tick,8);memcpy(data+7,lf_shared_q_selected.acceleration,12);data[10]=lf_shared_q_selected.speed;data[11]=lf_shared_q_selected.camera_source;data[12]=lf_shared_q_selected.gain;
 camera_accuracy_log(27,camera,target,0xcd5130,scope->dt,data,13);
}
static void lf_shared_q_target(LfTapeScope*scope,void*camera,uintptr_t target){uint32_t csr;__asm__ volatile("stmxcsr %0":"=m"(csr));lf_shared_q_target_impl(scope,camera,target);__asm__ volatile("ldmxcsr %0"::"m"(csr):"memory");}
static void lf_shared_q_args(unsigned operation,uintptr_t caller,float*a,float*b,float*c){
 if(!lf_shared_q_current())return;
 if(operation==LF_COLLISION&&caller==0xcd55e5){uint32_t csr;__asm__ volatile("stmxcsr %0":"=m"(csr));float args[3];lfq_collision(&lf_shared_q_selected,args);__asm__ volatile("ldmxcsr %0"::"m"(csr):"memory");*a=args[0];*b=args[1];*c=args[2];}
}
static void lf_shared_q_gain(uintptr_t caller,float*a){if(lf_shared_q_current()&&caller==0xcd563d)*a=lf_shared_q_selected.gain;}
static __attribute__((used,noinline)) ABI void lf_shared_q_ratio_mid(void*rbp){
 if(lf_shared_q_current()){float ratio=lfq_speed_ratio(&lf_shared_q_selected);memcpy((char*)rbp-0x64,&ratio,4);}
}
static void*lf_shared_q_speed_gateway;
static __attribute__((naked,used)) ABI void lf_shared_q_speed_bridge(void){
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
  "callq lf_shared_q_ratio_mid\n"
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
  "jmpq *lf_shared_q_speed_gateway(%rip)\n"
 );
}

static void lf_shared_q_reset_target(void*target){AcquireSRWLockExclusive(&lf_shared_q_lock);int changed=0;for(unsigned i=0;i<64;++i)if(lf_shared_q_history[i].target==(uintptr_t)target){if(!changed++)InterlockedIncrement64(&lf_shared_q_epoch);memset(lf_shared_q_history+i,0,sizeof lf_shared_q_history[i]);}ReleaseSRWLockExclusive(&lf_shared_q_lock);}
static void lfq_jump(unsigned char*out,uintptr_t target){const unsigned char op[]={0xff,0x25,0,0,0,0};memcpy(out,op,6);memcpy(out+6,&target,8);}
static int lf_shared_q_install(void){
 const char*requested=camera_fix_getenv("BB_CAMERA_LF_SHARED_Q_INPUT");if(!requested||strcmp(requested,"1"))return 0;
 /* CD5604..CD5618 only. The native threshold branch atCD5619 remains intact.
  * RIP constants are copied with their exact original bytes to the gateway. */
 static const unsigned char expected[]={0xc5,0xfa,0x10,0x45,0x9c,0xc5,0xf8,0x54,0x05,0xef,0x62,0x68,0x00,0xc5,0xf8,0x2e,0x05,0x6b,0x63,0x68,0x00};
 if(runtime_image_size<0xcd5604+sizeof expected||memcmp(image+0xcd5604,expected,sizeof expected))return 0;
 unsigned char*g=VirtualAlloc(NULL,1024,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);if(!g)return 0;
 memcpy(g,expected,sizeof expected);memcpy(g+512,image+0x135b900,16);memcpy(g+528,image+0x135b984,4);
 int32_t rel=512-13;memcpy(g+9,&rel,4);rel=528-21;memcpy(g+17,&rel,4);lfq_jump(g+sizeof expected,(uintptr_t)image+0xcd5619);
 DWORD old;if(!VirtualProtect(image+0xcd5604,sizeof expected,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(g,0,MEM_RELEASE);return 0;}
 lf_shared_q_speed_gateway=g;lf_shared_q_enabled=1;lfq_jump(image+0xcd5604,(uintptr_t)lf_shared_q_speed_bridge);memset(image+0xcd5604+14,0x90,sizeof expected-14);DWORD ignored;VirtualProtect(image+0xcd5604,sizeof expected,old,&ignored);FlushInstructionCache(GetCurrentProcess(),image+0xcd5604,sizeof expected);
 puts("LFSharedQ: opt-in private nativeQ world acceleration/SO3 basis/speed/gain BEFORE native LF nonlinear input; strict native30 no-override; HF inputs untouched");return 1;
}
