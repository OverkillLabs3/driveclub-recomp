#include "gforce-native-metadata.inc"
typedef struct {unsigned char*allocation,*derivative,*projection;} GFRNativeKernel;
typedef struct {_Alignas(16)float vector[4],matrix[16];float dt;} GFRNativeInput;
static __attribute__((naked,used)) ABI void gfr_native_finish(void){
 __asm__ volatile("movq -120(%rbp),%rax\nvmovaps -96(%rbp),%xmm0\nvmovups %xmm0,(%rax)\nleaq 8(%rbp),%rsp\npopq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %rbx\npopq %rbp\nretq\n");
}
static __attribute__((naked,used)) ABI void gfr_native_execute(void*component,const GFRNativeInput*input,float*out,const void*code){
 __asm__ volatile(
 "pushq %rbp\npushq %rbx\npushq %r12\npushq %r13\npushq %r14\npushq %r15\n"
 "subq $136,%rsp\nleaq 128(%rsp),%rbp\nmovq %rcx,-128(%rbp)\nmovq %rdx,-120(%rbp)\nmovq %rdi,%r15\n"
 "vmovups 0(%rsi),%xmm0\nvmovaps %xmm0,-64(%rbp)\nvmovaps %xmm0,-96(%rbp)\n"
 "vmovss 80(%rsi),%xmm0\nvmovss %xmm0,-68(%rbp)\nleaq 16(%rsi),%rax\njmpq *-128(%rbp)\n");
}
static void gfr_native_jump(unsigned char*where,uintptr_t target){
 static const unsigned char op[]={0xff,0x25,0,0,0,0};memcpy(where,op,6);memcpy(where+6,&target,8);
}
static uint64_t gfr_native_body_hash(const unsigned char*p,size_t n){uint64_t h=UINT64_C(14695981039346656037);while(n--)h=(h^*p++)*UINT64_C(1099511628211);return h;}
static int gfr_native_init(GFRNativeKernel*k,const unsigned char*original,size_t size){
 if(size<0x159dc4c||gfr_native_body_hash(original+0xd2a71c,GFR_DERIVATIVE_BYTES_SIZE)!=GFR_DERIVATIVE_BYTES_HASH||gfr_native_body_hash(original+0xd2a763,GFR_PROJECTION_BYTES_SIZE)!=GFR_PROJECTION_BYTES_HASH)return 0;
 unsigned char*mem=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!mem)return 0;
 k->allocation=mem;k->derivative=mem;k->projection=mem+256;
 memcpy(k->derivative,original+0xd2a71c,GFR_DERIVATIVE_BYTES_SIZE);memcpy(k->projection,original+0xd2a763,GFR_PROJECTION_BYTES_SIZE);
 if(gfr_native_body_hash(k->derivative,GFR_DERIVATIVE_BYTES_SIZE)!=GFR_DERIVATIVE_BYTES_HASH||gfr_native_body_hash(k->projection,GFR_PROJECTION_BYTES_SIZE)!=GFR_PROJECTION_BYTES_HASH){VirtualFree(mem,0,MEM_RELEASE);memset(k,0,sizeof*k);return 0;}
 const uint32_t targets[]={0x135f14c,0x135f150,0x159dc48};
 for(unsigned i=0;i<3;++i)memcpy(mem+4096+16*i,original+targets[i],4);
 struct Rel {unsigned block,offset,length,disp,index;} rel[]={
  {0,15,8,4,0},{1,32,8,4,1},{1,143,7,3,2},{1,159,8,4,0}};
 for(unsigned i=0;i<4;++i){unsigned char*at=(rel[i].block?k->projection:k->derivative)+rel[i].offset;intptr_t delta=(mem+4096+16*rel[i].index)-(at+rel[i].length);if(delta<INT32_MIN||delta>INT32_MAX){VirtualFree(mem,0,MEM_RELEASE);memset(k,0,sizeof*k);return 0;}int32_t disp=(int32_t)delta;memcpy(at+rel[i].disp,&disp,4);}
 gfr_native_jump(k->derivative+GFR_DERIVATIVE_BYTES_SIZE,(uintptr_t)gfr_native_finish);gfr_native_jump(k->projection+GFR_PROJECTION_BYTES_SIZE,(uintptr_t)gfr_native_finish);
 DWORD old;if(!VirtualProtect(mem,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(mem,0,MEM_RELEASE);memset(k,0,sizeof*k);return 0;}FlushInstructionCache(GetCurrentProcess(),mem,4096);return 1;
}
static __attribute__((unused)) void gfr_native_destroy(GFRNativeKernel*k){if(k->allocation)VirtualFree(k->allocation,0,MEM_RELEASE);memset(k,0,sizeof*k);}
static void gfr_native_step(GFRNativeKernel*k,const float previous_velocity[4],const float velocity[4],const float matrix[16],float dt,const float previous_filter[4],float filter[4],float jolt[4],uint32_t csr,uint32_t*after){
 _Alignas(16)unsigned char component[96]={0};GFRNativeInput input={0};float world[4];uint32_t saved;
 memcpy(component+0x40,previous_velocity,16);memcpy(component+0x50,previous_filter,16);memcpy(input.vector,velocity,16);memcpy(input.matrix,matrix,64);input.dt=dt;
 __asm__ volatile("stmxcsr %0":"=m"(saved));__asm__ volatile("ldmxcsr %0"::"m"(csr):"memory");
 gfr_native_execute(component,&input,world,k->derivative);memcpy(input.vector,world,16);gfr_native_execute(component,&input,jolt,k->projection);memcpy(filter,component+0x50,16);
 if(after)__asm__ volatile("stmxcsr %0":"=m"(*after));__asm__ volatile("ldmxcsr %0"::"m"(saved):"memory");
}
/* Projection-only oracle is useful when the live original derivative itself
 * is already captured. This executes the same original AVX instructions. */
static __attribute__((unused)) void gfr_native_project(GFRNativeKernel*k,const float world[4],const float matrix[16],float dt,const float previous_filter[4],float filter[4],float jolt[4],uint32_t csr,uint32_t*after){
 _Alignas(16)unsigned char component[96]={0};GFRNativeInput input={0};uint32_t saved;
 memcpy(component+0x50,previous_filter,16);memcpy(input.vector,world,16);memcpy(input.matrix,matrix,64);input.dt=dt;
 __asm__ volatile("stmxcsr %0":"=m"(saved));__asm__ volatile("ldmxcsr %0"::"m"(csr):"memory");gfr_native_execute(component,&input,jolt,k->projection);memcpy(filter,component+0x50,16);
 if(after)__asm__ volatile("stmxcsr %0":"=m"(*after));__asm__ volatile("ldmxcsr %0"::"m"(saved):"memory");
}
