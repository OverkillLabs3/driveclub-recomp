#ifndef CAMERA_HEAD_COPY_NATIVE_KERNEL_INCLUDED
#define CAMERA_HEAD_COPY_NATIVE_KERNEL_INCLUDED
/* Private original D1991E..D19A07 transaction. No live target getter iscalled.
 * Original Euler code remains pure native code on the private savedbasis.
 * Actual lookup return + MXCSR are supplied by a measured observation. */
typedef struct {float matrix[16],angular[4];uint32_t mxcsr_mid;} HSIInput;
typedef void(ABI*HSIEuler)(void*,float*,float*,float*);
typedef struct {unsigned char*allocation,*entry;HSIEuler euler;} HSIKernel;
static _Thread_local const HSIInput*hsi_active_input;
static _Thread_local HSIKernel*hsi_active_kernel;
static _Thread_local const float*hsi_observed_euler;
static _Thread_local _Alignas(16)unsigned char hsi_active_target[0x150];
static void ABI hsi_euler_provider(void*matrix,float*p,float*h,float*r){
 if(hsi_observed_euler){uint32_t bits;memcpy(&bits,hsi_observed_euler,4);bits^=0x80000000u;memcpy(p,&bits,4);memcpy(h,hsi_observed_euler+1,4);memcpy(r,hsi_observed_euler+2,4);}
 else hsi_active_kernel->euler(matrix,p,h,r);
}
static void*ABI hsi_target_provider(void*self){(void)self;__asm__ volatile("ldmxcsr %0"::"m"(hsi_active_input->mxcsr_mid):"memory");return hsi_active_target;}
static void hsi_jump(unsigned char*p,uintptr_t target){memcpy(p,"\xff\x25\0\0\0\0",6);memcpy(p+6,&target,8);}
static uint64_t hsi_hash(const unsigned char*p,size_t n){uint64_t h=UINT64_C(14695981039346656037);while(n--)h=(h^*p++)*UINT64_C(1099511628211);return h;}
static int hsi_native_init(HSIKernel*k,unsigned char*native,size_t size){
 memset(k,0,sizeof*k);if(size<0x135e3d0)return 0;
 /* Full transaction signature is checked before any private relocation. */
 if(hsi_hash(native+0xd1991e,0xe9)!=UINT64_C(0xb12dbd819d4c170d))return 0;
 unsigned char*p=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!p)return 0;
 static const unsigned char prefix[]={0x55,0x48,0x89,0xe5,0x53,0x41,0x56,0x48,0x81,0xec,0x80,0,0,0,0x48,0x89,0xfb,0x4c,0x8d,0xb3,0xe0,2,0,0,
  0xc5,0xf8,0x28,0x83,0xe0,2,0,0,0xc5,0xf8,0x29,0x45,0x80,
  0xc5,0xf8,0x28,0x83,0xf0,2,0,0,0xc5,0xf8,0x29,0x45,0x90,
  0xc5,0xf8,0x28,0x83,0x00,3,0,0,0xc5,0xf8,0x29,0x45,0xa0,
  0xc5,0xf8,0x28,0x83,0x10,3,0,0,0xc5,0xf8,0x29,0x45,0xb0};
 unsigned off=sizeof prefix;memcpy(p,prefix,off);memcpy(p+off,native+0xd1991e,0xe9);
 static const unsigned char tail[]={0x48,0x81,0xc4,0x80,0,0,0,0x41,0x5e,0x5b,0x5d,0xc3};memcpy(p+off+0xe9,tail,sizeof tail);
 /* Two direct calls, one XOR-sign constant; the arithmetic bytes areuntouched. */
 const unsigned call_off[]={0x19,0x39};uintptr_t functions[]={(uintptr_t)hsi_euler_provider,(uintptr_t)hsi_target_provider};
 for(unsigned i=0;i<2;++i){unsigned thunk=512+i*16;hsi_jump(p+thunk,functions[i]);int32_t d=(int32_t)thunk-(int32_t)(off+call_off[i]+5);memcpy(p+off+call_off[i]+1,&d,4);}
 memcpy(p+576,native+0x135e3c0,16);int32_t displacement=576-(int32_t)(off+0x26+8);memcpy(p+off+0x26+4,&displacement,4);
 DWORD old;if(!VirtualProtect(p,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(p,0,MEM_RELEASE);return 0;}FlushInstructionCache(GetCurrentProcess(),p,592);k->allocation=k->entry=p;k->euler=(HSIEuler)(native+0x674180);return 1;
}
static int hsi_native_copy_impl(HSIKernel*k,unsigned char camera[0x3a0],const HSIInput*input,const float*observed_euler,uint32_t*post){
 int32_t count;memcpy(&count,camera+0x33c,4);if(!k||!k->entry||!input||count<=0||count>3||(input->mxcsr_mid&~0xffffu))return 0;
 _Alignas(16)unsigned char copy[0x3a0];memcpy(copy,camera,sizeof copy);memcpy(copy+0x2e0,input->matrix,64);memset(hsi_active_target,0,sizeof hsi_active_target);memcpy(hsi_active_target+0x130,input->angular,16);
 const HSIInput*previous=hsi_active_input;HSIKernel*previous_kernel=hsi_active_kernel;const float*previous_euler=hsi_observed_euler;hsi_active_input=input;hsi_active_kernel=k;hsi_observed_euler=observed_euler;uint32_t host;__asm__ volatile("stmxcsr %0":"=m"(host));__asm__ volatile("ldmxcsr %0"::"m"(input->mxcsr_mid):"memory");typedef void(ABI*Copy)(void*);((Copy)k->entry)(copy);uint32_t after;__asm__ volatile("stmxcsr %0":"=m"(after));__asm__ volatile("ldmxcsr %0"::"m"(host):"memory");hsi_active_input=previous;hsi_active_kernel=previous_kernel;hsi_observed_euler=previous_euler;memcpy(camera,copy,sizeof copy);if(post)*post=after;return 1;
}
static __attribute__((unused))int hsi_native_copy(HSIKernel*k,unsigned char camera[0x3a0],const HSIInput*input,uint32_t*post){return hsi_native_copy_impl(k,camera,input,NULL,post);}
/* Oracle only: captured native Euler avoids confusing a different external
 * CPU-test libm with the original copy's projection/decrement arithmetic. */
static __attribute__((unused))int hsi_native_copy_observed(HSIKernel*k,unsigned char camera[0x3a0],const HSIInput*input,const float euler[3],uint32_t*post){return hsi_native_copy_impl(k,camera,input,euler,post);}
static __attribute__((unused))void hsi_native_destroy(HSIKernel*k){if(k->allocation)VirtualFree(k->allocation,0,MEM_RELEASE);memset(k,0,sizeof*k);}
#endif
