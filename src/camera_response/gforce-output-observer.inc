/* Native-Q GForce consumer hooks; OUTPUT_TRACE remains explicit diagnostics.
 * Original native update/getters/animation run once; private reference selects
 * only the six inputs. No physics/history writes or authored gain changes.
 * All four expected original signatures must match BEFORE any patch is made.
 * Mid probes preserve every GPR, all 16 YMM registers, flags and MXCSR. */
typedef void(ABI*GfoUpdate)(void*,float);
typedef unsigned char(ABI*GfoSetter)(void*,const char*,float);
static GfoUpdate gfo_update_original;
static GfoSetter gfo_setter_original;
static __attribute__((used)) uintptr_t gfo_raw_resume,gfo_basis_resume;
typedef struct GfoScope {
    struct GfoScope *parent;
    uintptr_t self,owner,resolved_owner,model,entity,clock,basis;
    int64_t logical_time,clock_dt;
    uint32_t frame218,frame21c,flags,mask,accepted,sequence;
    float dt,raw_dt,previous[4],raw[4],world_accel[4],axes[16];
    float filtered_before[4],filtered_after[4],previous_after[4],variables[6];
    uintptr_t native_frame;int reference_computed,reference_applied;uint64_t reference_epoch;float reference_values[6];
    int sampled;
} GfoScope;
static _Thread_local GfoScope *gfo_scope;
static volatile LONG gfo_samples;static int gfo_trace_enabled;
enum {GFO_ENTRY_READ=1,GFO_CLOCK_READ=2,GFO_RAW_READ=4,GFO_BASIS_READ=8,GFO_EXIT_READ=16};

static void gfo_clock(GfoScope*s){
    uintptr_t clock=0;
    if(camera_pose_read(runtime_image_base+0x1911100,&clock,8)&&
       camera_pose_read(clock+0x970,&clock,8)&&camera_pose_read(clock+0x1e0,&clock,8)&&
       camera_pose_read(clock+0x58,&s->logical_time,8)&&camera_pose_read(clock+0x60,&s->clock_dt,8)&&
       camera_pose_read(clock+0x218,&s->frame218,4)&&camera_pose_read(clock+0x21c,&s->frame21c,4)){
        s->clock=clock;s->flags|=GFO_CLOCK_READ;
    }
}

static void driver_palette_register(void*p){(void)p;}
static void driver_palette_discontinuity(void){}
static void driver_palette_install(void){}
#include "gforce-reference-runtime.inc"
static __attribute__((used,noinline)) ABI void gfo_raw_capture(void*frame,void*self,void*owner){
    GfoScope*s=gfo_scope;
    if(!s||(!s->sampled&&!gfr_enabled)||s->self!=(uintptr_t)self)return;
    s->resolved_owner=(uintptr_t)owner;s->native_frame=(uintptr_t)frame;
    if(camera_pose_read((uintptr_t)frame-0x40,s->raw,16)&&
       camera_pose_read((uintptr_t)self+0x40,s->previous,16)&&
       camera_pose_read((uintptr_t)frame-0x44,&s->raw_dt,4))s->flags|=GFO_RAW_READ;
}
static __attribute__((used,noinline)) ABI void gfo_basis_capture(void*frame,void*self,void*basis){
    GfoScope*s=gfo_scope;
    if(!s||(!s->sampled&&!gfr_enabled)||s->self!=(uintptr_t)self)return;
    s->basis=(uintptr_t)basis;
    if(camera_pose_read((uintptr_t)basis,s->axes,64)&&
       camera_pose_read((uintptr_t)frame-0x60,s->world_accel,16))s->flags|=GFO_BASIS_READ;
}

static ABI unsigned char gfo_setter(void*instance,const char*name,float value){
    uintptr_t caller=(uintptr_t)__builtin_return_address(0);
    uint32_t reference_csr;__asm__ volatile("stmxcsr %0":"=m"(reference_csr));
    if(caller>=runtime_image_base+0xd2a873&&caller<=runtime_image_base+0xd2a8e8)value=gfr_select(gfo_scope,name,value);
    __asm__ volatile("ldmxcsr %0"::"m"(reference_csr):"memory");
    unsigned char result=gfo_setter_original(instance,name,value);
    uint32_t csr;__asm__ volatile("stmxcsr %0":"=m"(csr));
    GfoScope*s=gfo_scope;
    if(s&&s->sampled&&caller>=runtime_image_base+0xd2a873&&caller<=runtime_image_base+0xd2a8e8){
        static const uintptr_t names[6]={0x135f1d0,0x135f1de,0x135f1ec,0x135f1fa,0x135f20c,0x135f21e};
        for(unsigned i=0;i<6;++i)if((uintptr_t)name==runtime_image_base+names[i]){
            s->variables[i]=value;s->mask|=1u<<i;if(result)s->accepted|=1u<<i;break;
        }
    }
    __asm__ volatile("ldmxcsr %0"::"m"(csr):"memory");return result;
}

static void gfo_emit(GfoScope*s){
 (void)s;
}

static ABI void gfo_update(void*self,float dt){
    uint32_t entry_csr;__asm__ volatile("stmxcsr %0":"=m"(entry_csr));
    GfoScope scope={0};scope.parent=gfo_scope;scope.self=(uintptr_t)self;scope.dt=dt;
    /* Bounded to 262144 component updates; no additional files/output buffers. */
    if(gfo_trace_enabled){scope.sequence=(uint32_t)InterlockedIncrement(&gfo_samples);scope.sampled=scope.sequence<=262144;}
    if(scope.sampled||gfr_enabled){
        gfo_clock(&scope);
        if(camera_pose_read((uintptr_t)self+8,&scope.entity,8)&&
           camera_pose_read((uintptr_t)self+0x18,&scope.model,8)&&
           camera_pose_read((uintptr_t)self+0x20,&scope.owner,8)&&
           camera_pose_read((uintptr_t)self+0x40,scope.previous,16)&&
           camera_pose_read((uintptr_t)self+0x50,scope.filtered_before,16))scope.flags|=GFO_ENTRY_READ;
    }
    gfo_scope=&scope;
    __asm__ volatile("ldmxcsr %0"::"m"(entry_csr):"memory");
    gfo_update_original(self,dt); /* Exactly one original call, original args. */
    uint32_t exit_csr;__asm__ volatile("stmxcsr %0":"=m"(exit_csr));
    driver_palette_register(self);
    gfo_scope=scope.parent;
    if(scope.sampled){
        if(camera_pose_read((uintptr_t)self+0x50,scope.filtered_after,16)&&
           camera_pose_read((uintptr_t)self+0x40,scope.previous_after,16))scope.flags|=GFO_EXIT_READ;
        gfo_emit(&scope);
    }
    __asm__ volatile("ldmxcsr %0"::"m"(exit_csr):"memory");
}

/* Generated below: naked bridge prologue/epilogue preserves exact original
 * state before executing the displaced non-RIP instructions once. */
static __attribute__((naked,used)) ABI void gfo_raw_bridge(void){
 __asm__ volatile(
  "pushfq\n"
  "pushq %rax\n"
  "pushq %rbx\n"
  "pushq %rcx\n"
  "pushq %rdx\n"
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
  "subq $528, %rsp\n"
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
  "stmxcsr 512(%rsp)\n"
  "movq %rbp, %rdi\n"
  "movq %r15, %rsi\n"
  "movq %r14, %rdx\n"
  "callq gfo_raw_capture\n"
  "ldmxcsr 512(%rsp)\n"
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
  "addq $528, %rsp\n"
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
  "popq %rdx\n"
  "popq %rcx\n"
  "popq %rbx\n"
  "popq %rax\n"
  "popfq\n"
  "jmpq *gfo_raw_resume(%rip)\n"
 );
}
static __attribute__((naked,used)) ABI void gfo_basis_bridge(void){
 __asm__ volatile(
  "pushfq\n"
  "pushq %rax\n"
  "pushq %rbx\n"
  "pushq %rcx\n"
  "pushq %rdx\n"
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
  "subq $528, %rsp\n"
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
  "stmxcsr 512(%rsp)\n"
  "movq %rbp, %rdi\n"
  "movq %r15, %rsi\n"
  "movq %rax, %rdx\n"
  "callq gfo_basis_capture\n"
  "ldmxcsr 512(%rsp)\n"
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
  "addq $528, %rsp\n"
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
  "popq %rdx\n"
  "popq %rcx\n"
  "popq %rbx\n"
  "popq %rax\n"
  "popfq\n"
  "jmpq *gfo_basis_resume(%rip)\n"
 );
}

static void gfo_indirect_jump(unsigned char*where,uintptr_t target){
 const unsigned char op[6]={0xff,0x25,0,0,0,0};memcpy(where,op,6);memcpy(where+6,&target,8);
}
static int gfo_install(void){
 const char*requested=camera_fix_getenv("BB_GFORCE_OUTPUT_TRACE");gfo_trace_enabled=requested&&!strcmp(requested,"1");
 const char*core=camera_fix_getenv("BB_GFORCE_REFERENCE");if(!gfo_trace_enabled&&(!core||strcmp(core,"1")))return 0;
 const char*oracle=camera_fix_getenv("BB_CAMERA_ACCURACY_ORACLE");if(!oracle||strcmp(oracle,"1")){
  puts("GForceOutputTrace: isolated oracle run required; existing correction hooks left intact");return 0;
 }
 static int installed;if(installed)return 1;
 static const unsigned char update[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x40};
 static const unsigned char setter[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x20};
 static const unsigned char raw[]={0xc4,0xc1,0x78,0x28,0x47,0x40,0xc5,0xf8,0x28,0x4d,0xc0,0xc5,0xf0,0x5c,0xd0};
 static const unsigned char basis[]={0xc5,0xf8,0x28,0x5d,0xa0,0xc5,0xe0,0x59,0x00,0xc5,0xf9,0x70,0xc8,0x00};
 struct GfoHook {size_t at,count;const unsigned char*expected;uintptr_t target;} hooks[]={
  {0xd2a650,sizeof update,update,(uintptr_t)gfo_update},
  {0xe1aef0,sizeof setter,setter,(uintptr_t)gfo_setter},
  {0xd2a71c,sizeof raw,raw,(uintptr_t)gfo_raw_bridge},
  {0xd2a763,sizeof basis,basis,(uintptr_t)gfo_basis_bridge}
 };
 for(unsigned i=0;i<4;++i)if(runtime_image_size<hooks[i].at+hooks[i].count||memcmp(image+hooks[i].at,hooks[i].expected,hooks[i].count)){
  printf("GForceOutputTrace: original signature unavailable at %zx; all observer hooks skipped\n",hooks[i].at);return 0;
 }
 if(!gfr_install())return 0;
 driver_palette_install();
 unsigned char*code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!code)return 0;
 for(unsigned i=0;i<4;++i){memcpy(code+64*i,hooks[i].expected,hooks[i].count);gfo_indirect_jump(code+64*i+hooks[i].count,runtime_image_base+hooks[i].at+hooks[i].count);}
 DWORD old;if(!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(code,0,MEM_RELEASE);return 0;}
 FlushInstructionCache(GetCurrentProcess(),code,4096);
 gfo_update_original=(GfoUpdate)(code);gfo_setter_original=(GfoSetter)(code+64);gfo_raw_resume=(uintptr_t)(code+128);gfo_basis_resume=(uintptr_t)(code+192);
 for(unsigned i=0;i<4;++i){gfo_indirect_jump(image+hooks[i].at,hooks[i].target);if(hooks[i].count>14)memset(image+hooks[i].at+14,0x90,hooks[i].count-14);FlushInstructionCache(GetCurrentProcess(),image+hooks[i].at,hooks[i].count);}
 installed=1;puts(gfo_trace_enabled?"GForce: native-Q core plus optional six-setter trace":"GForce: native-Q six animation inputs; diagnostic sampling off");return 1;
}
