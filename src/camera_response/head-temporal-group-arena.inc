
/* Original ordered temporal bank; no files, live SDK queries, world writes or
 * invented coefficients. Inputs at canonical Q are an explicit caller contract. */
#include "native-group-metadata.inc"
typedef struct{float pre_frame90[16],pre_base100[4],dependent_target130[4];unsigned char helper_entry[0x380],helper_exit[0x380],final_actor[0x380];float helper_dt,helper_response;uint32_t helper_csr_before,helper_csr_after,final_csr,status;} HgrNativeOutput;
typedef struct{uintptr_t main,look,accum,skip_accum,args,ce,remap;unsigned char*allocation;size_t size;int half_slot,one_slot;SRWLOCK lock;unsigned ready;} HgrNativeArena;
_Static_assert(offsetof(HgrNativeArena,args)==32,"arena args ABI");
_Static_assert(offsetof(HgrNativeOutput,helper_entry)==96,"native output geometry ABI");
typedef struct{HgrNativeArena*arena;const HprRecord*primitive;HgrNativeOutput*out;} HgrNativeContext;
static _Thread_local HgrNativeContext*hgr_native_context;
static __attribute__((naked,used)) ABI void hgr_look_execute(void*actor,const void*code){__asm__ volatile(
"pushq %r12\n"
"movq %rdi,%r12\n"
"callq *%rsi\n"
"popq %r12\n"
"retq\n"
);}
static __attribute__((naked,used)) ABI void hgr_after_accum(void){__asm__ volatile(
"vmovups 232(%r12),%xmm3\n"
"vmovups 248(%r12),%xmm4\n"
"vmovups 264(%r12),%xmm5\n"
"vmovups 280(%r12),%xmm6\n"
"vmovups 296(%r12),%xmm7\n"
"vmovss 280(%r12),%xmm6\n"
"vmovss %xmm6,-76(%rbp)\n"
"vmovss 312(%r12),%xmm1\n"
"jmpq *32(%r13)\n"
);}
static __attribute__((naked,used)) ABI void hgr_controller_finish(void){__asm__ volatile(
"addq $104,%rsp\n"
"popq %rbx\n"
"popq %r12\n"
"popq %r13\n"
"popq %r14\n"
"popq %r15\n"
"popq %rbp\n"
"retq\n"
);}
static __attribute__((naked,used)) ABI void hgr_controller_execute(void*actor,const HprRecord*p,const HgrNativeArena*arena,float dt,float response){__asm__ volatile(
"pushq %rbp\n"
"movq %rsp,%rbp\n"
"pushq %r15\n"
"pushq %r14\n"
"pushq %r13\n"
"pushq %r12\n"
"pushq %rbx\n"
"subq $104,%rsp\n"
"movq %rdi,%r15\n"
"movq %rsi,%r12\n"
"movq %rdx,%r13\n"
"vmovss %xmm0,-80(%rbp)\n"
"vmovss %xmm1,-84(%rbp)\n"
"movl 136(%r12),%r14d\n"
"movl 152(%r12),%eax\n"
"movl %eax,-72(%rbp)\n"
"movl 156(%r12),%eax\n"
"movl %eax,-68(%rbp)\n"
"leaq 512(%r15),%rbx\n"
"testb %r14b,%r14b\n"
"jz 1f\n"
"jmpq *16(%r13)\n"
"1:\n"
"jmpq *24(%r13)\n"
);}
static __attribute__((naked,used)) ABI void hgr_finish(void){__asm__ volatile(
"movq -312(%rbp),%rax\n"
"vmovups -144(%rbp),%xmm0\n"
"vmovups %xmm0,0(%rax)\n"
"vmovups -128(%rbp),%xmm0\n"
"vmovups %xmm0,16(%rax)\n"
"vmovups -112(%rbp),%xmm0\n"
"vmovups %xmm0,32(%rax)\n"
"vmovups -96(%rbp),%xmm0\n"
"vmovups %xmm0,48(%rax)\n"
"vmovups -256(%rbp),%xmm0\n"
"vmovups %xmm0,64(%rax)\n"
"vmovups -304(%rbp),%xmm0\n"
"vmovups %xmm0,80(%rax)\n"
"leaq 8(%rbp),%rsp\n"
"popq %r15\n"
"popq %r14\n"
"popq %r13\n"
"popq %r12\n"
"popq %rbx\n"
"popq %rbp\n"
"retq\n"
);}
static __attribute__((naked,used)) ABI void hgr_execute(void*actor,const HKTInputs*input,const void*entry,HgrNativeOutput*out,const HprRecord*p){__asm__ volatile(
"pushq %rbp\n"
"pushq %rbx\n"
"pushq %r12\n"
"pushq %r13\n"
"pushq %r14\n"
"pushq %r15\n"
"subq $328,%rsp\n"
"leaq 320(%rsp),%rbp\n"
"movq %rdx,-320(%rbp)\n"
"movq %rcx,-312(%rbp)\n"
"movq %rdi,%r15\n"
"movl 140(%r8),%ebx\n"
"vmovups 0(%rsi),%xmm0\n"
"vmovaps %xmm0,-240(%rbp)\n"
"vmovups 16(%rsi),%xmm0\n"
"vmovaps %xmm0,-64(%rbp)\n"
"vmovups 32(%rsi),%xmm0\n"
"vmovaps %xmm0,-80(%rbp)\n"
"vmovups 48(%rsi),%xmm0\n"
"vmovaps %xmm0,-272(%rbp)\n"
"vmovups 64(%rsi),%xmm0\n"
"vmovaps %xmm0,-288(%rbp)\n"
"vmovups 80(%rsi),%xmm0\n"
"vmovaps %xmm0,-304(%rbp)\n"
"vmovups 96(%rsi),%xmm0\n"
"vmovaps %xmm0,-256(%rbp)\n"
"leaq 112(%rsi),%rax\n"
"jmpq *-320(%rbp)\n"
);}

static ABI void hgr_forbidden_call(void){if(hgr_native_context)hgr_native_context->out->status|=16;}
static ABI void hgr_look_primitive(void*actor){HgrNativeContext*c=hgr_native_context;if(!c)return;const HprRecord*p=c->primitive;if(!(p->flags&32)){c->out->status|=1;return;}memcpy((char*)actor+0x2b4,&p->look_target,4);memcpy((char*)actor+0x2c0,p->look_cache,16);hgr_look_execute(actor,(void*)c->arena->look);}
static ABI void hgr_input_primitive(void*actor,float dt,float response){HgrNativeContext*c=hgr_native_context;if(!c)return;HgrNativeOutput*out=c->out;const HprRecord*p=c->primitive;out->helper_dt=dt;out->helper_response=response;out->helper_csr_before=_mm_getcsr();memcpy(out->helper_entry,actor,0x380);
 if(p->flags&256){if(!(p->flags&16)){out->status|=2;return;}hgr_controller_execute(actor,p,c->arena,dt,response);}else if(p->flags&512)memcpy((char*)actor+0x200,p->head_exit+0x200,32);else{out->status|=4;return;}
 out->helper_csr_after=_mm_getcsr();memcpy(out->helper_exit,actor,0x380);
}
static void hgr_abs_jump(unsigned char*p,uintptr_t target){memcpy(p,"\xff\x25\0\0\0\0",6);memcpy(p+6,&target,8);}
typedef int(*HgrMemoryRead)(uintptr_t,void*,size_t);
static uint64_t hgr_native_body_hash(const unsigned char*p,size_t n){uint64_t h=UINT64_C(14695981039346656037);while(n--)h=(h^*p++)*UINT64_C(1099511628211);return h;}
static int hgr_native_create(HgrNativeArena*a,const unsigned char*source,size_t source_size,HgrMemoryRead read){
 if(!a||!source||!read)return 0;memset(a,0,sizeof*a);a->half_slot=a->one_slot=-1;unsigned char verify[HGR_MAIN_SIZE];
 /* Must run BEFORE installing HKT/HPR/publication detours on the original image. */
 for(unsigned i=0;i<sizeof hgr_parts/sizeof*hgr_parts;++i){if(hgr_parts[i].native+hgr_parts[i].size>source_size||!read((uintptr_t)source+hgr_parts[i].native,verify,hgr_parts[i].size)||hgr_native_body_hash(verify,hgr_parts[i].size)!=hgr_parts[i].hash)return 0;}
 unsigned char stub[3];if(0xe70b13>source_size||!read((uintptr_t)source+0xe70b10,stub,3)||memcmp(stub,"\x31\xc0\xc3",3))return 0;
 unsigned char*code=VirtualAlloc(NULL,HGR_TOTAL,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!code)return 0;a->allocation=code;a->size=HGR_TOTAL;InitializeSRWLock(&a->lock);
 for(unsigned i=0;i<sizeof hgr_parts/sizeof*hgr_parts;++i){if(!read((uintptr_t)source+hgr_parts[i].native,code+hgr_parts[i].offset,hgr_parts[i].size)||hgr_native_body_hash(code+hgr_parts[i].offset,hgr_parts[i].size)!=hgr_parts[i].hash){VirtualFree(code,0,MEM_RELEASE);memset(a,0,sizeof*a);return 0;}}
 for(unsigned i=0;i<sizeof hgr_data_targets/sizeof*hgr_data_targets;++i){uintptr_t target=hgr_data_targets[i];if(target+16>source_size||!read((uintptr_t)source+target,code+HGR_DATA+i*16,16)){VirtualFree(code,0,MEM_RELEASE);memset(a,0,sizeof*a);return 0;}if(target==0x191b0f0)a->half_slot=(int)i;if(target==0x191b100)a->one_slot=(int)i;}
 if(a->half_slot<0||a->one_slot<0){VirtualFree(code,0,MEM_RELEASE);memset(a,0,sizeof*a);return 0;}
 for(unsigned i=0;i<sizeof hgr_relocs/sizeof*hgr_relocs;++i){int32_t d=(int32_t)(HGR_DATA+hgr_relocs[i].index*16-hgr_relocs[i].offset-hgr_relocs[i].length);memcpy(code+hgr_relocs[i].offset+hgr_relocs[i].disp,&d,4);}
 for(unsigned i=0;i<sizeof hgr_calls/sizeof*hgr_calls;++i){uintptr_t target=(uintptr_t)hgr_forbidden_call;switch(hgr_calls[i].target){case 0x123e820:target=(uintptr_t)source+0x123e820;break;case 0x675e00:target=(uintptr_t)code+HGR_REMAP;break;case 0xb92590:target=(uintptr_t)hgr_look_primitive;break;case 0xb92a40:target=(uintptr_t)hgr_input_primitive;break;case 0xce03b0:target=(uintptr_t)code+HGR_CE;break;}
  unsigned island=HGR_ISLANDS+i*14;hgr_abs_jump(code+island,target);int32_t d=(int32_t)(island-hgr_calls[i].offset-5);memcpy(code+hgr_calls[i].offset+1,&d,4);
 }
 hgr_abs_jump(code+HGR_MAIN+HGR_MAIN_SIZE,(uintptr_t)hgr_finish);code[HGR_LOOK+HGR_LOOK_SIZE]=0xc3;hgr_abs_jump(code+HGR_ACCUM+HGR_ACCUM_SIZE,(uintptr_t)hgr_after_accum);hgr_abs_jump(code+HGR_ARGS+HGR_ARGS_SIZE,(uintptr_t)hgr_controller_finish);
 #define HGR_MAIN_PC(pc) (code+HGR_MAIN+(pc)-0xb919a1)
 /* Coherent original bank skips current render composition. The live publisher
  * performs that original composition once with its CURRENT world/quaternion. */
 hgr_abs_jump(HGR_MAIN_PC(0xb921d2),(uintptr_t)HGR_MAIN_PC(0xb9223c));
 /* Original E70B10 is guarded pure xorEAX/EAX/ret, so BL1 selects directH.
  * No root/SDK read is needed for unused arguments to that constant function. */
 hgr_abs_jump(HGR_MAIN_PC(0xb9224b),(uintptr_t)HGR_MAIN_PC(0xb922de));
 hgr_abs_jump(HGR_MAIN_PC(0xb9230b),(uintptr_t)HGR_MAIN_PC(0xb92383));
 hgr_abs_jump(HGR_MAIN_PC(0xb9240e),(uintptr_t)HGR_MAIN_PC(0xb9254c));
 #undef HGR_MAIN_PC
 a->main=(uintptr_t)code+HGR_MAIN;a->look=(uintptr_t)code+HGR_LOOK;a->accum=(uintptr_t)code+HGR_ACCUM;a->skip_accum=a->accum+(0xb92d24-0xb92cb4);a->args=(uintptr_t)code+HGR_ARGS;a->ce=(uintptr_t)code+HGR_CE;a->remap=(uintptr_t)code+HGR_REMAP;
 DWORD old;if(!VirtualProtect(code,HGR_DATA,PAGE_EXECUTE_READ,&old)){VirtualFree(code,0,MEM_RELEASE);memset(a,0,sizeof*a);return 0;}FlushInstructionCache(GetCurrentProcess(),code,HGR_DATA);a->ready=1;return 1;
}
static int hgr_native_step(HgrNativeArena*a,unsigned char actor[0x380],const HKTInputs*input,const HprRecord*primitive,uint32_t mxcsr,HgrNativeOutput*out){
 if(!a||!a->ready||!actor||!input||!primitive||!out||hgr_native_context||primitive->parent_mode_bl>1)return 0;
 unsigned route=primitive->flags&(256|512);if(route!=256&&route!=512)return 0;_Alignas(16)unsigned char fx[512],scratch[0x380];_Alignas(16)HKTInputs aligned_input;memcpy(scratch,actor,0x380);memcpy(&aligned_input,input,sizeof aligned_input);AcquireSRWLockExclusive(&a->lock);__asm__ volatile("fxsave64 %0":"=m"(fx));
 memcpy(a->allocation+HGR_DATA+a->half_slot*16,input->global_half,16);memcpy(a->allocation+HGR_DATA+a->one_slot*16,input->global_one,16);memset(out,0,sizeof*out);HgrNativeContext c={a,primitive,out};hgr_native_context=&c;_mm_setcsr(mxcsr);hgr_execute(scratch,&aligned_input,(void*)a->main,out,primitive);out->final_csr=_mm_getcsr();memcpy(out->final_actor,scratch,0x380);if(!out->status)memcpy(actor,scratch,0x380);hgr_native_context=NULL;__asm__ volatile("fxrstor64 %0"::"m"(fx):"memory");ReleaseSRWLockExclusive(&a->lock);return out->status==0;
}
