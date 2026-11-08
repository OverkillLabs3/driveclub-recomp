#ifndef HEAD_ANGULAR_NATIVE_CLONE_INCLUDED
#define HEAD_ANGULAR_NATIVE_CLONE_INCLUDED
/* CPU-only original-helper clone with measured external Euler/target providers.
 * No live CC9B30 or game math state is called or altered. */
typedef struct {float dt,euler[3];uint32_t mxcsr_pre,mxcsr_mid;unsigned char target[0x150];} HACInputs;
typedef struct {uint16_t offset;uint8_t length,disp_offset,index;uint32_t native_target;} HACReloc;
typedef struct {uint16_t offset;uint32_t native_target;} HACCall;
#include "head-angular-relocations.inc"
typedef struct {unsigned char*allocation,*code;unsigned data_count;} HACNativeKernel;
static _Thread_local const HACInputs*hac_active_input;
static _Thread_local unsigned char*hac_active_camera;
static _Thread_local _Alignas(16)unsigned char hac_active_target[0x150];
static void ABI hac_euler(void*matrix,float*p,float*h,float*r){
 (void)matrix;uint32_t bits;memcpy(&bits,hac_active_input->euler,4);bits^=0x80000000u;memcpy(p,&bits,4);memcpy(h,hac_active_input->euler+1,4);memcpy(r,hac_active_input->euler+2,4);
}
static void*ABI hac_target(void*self){(void)self;if(hcal_mid_record)memcpy(hcal_mid_record->mid,hac_active_camera,0x3a0);__asm__ volatile("ldmxcsr %0"::"m"(hac_active_input->mxcsr_mid):"memory");return hac_active_target;}
static __attribute__((noreturn))ABI void hac_canary_failure(void){abort();}
static int hac_native_init(HACNativeKernel*k,const unsigned char*native,size_t size){
 memset(k,0,sizeof *k);if(size<0x166e260||hat_hash(native+0xd19dd0,2028)!=UINT64_C(0x31c37121c2ca54b6))return 0;
 unsigned char*p=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!p)return 0;memcpy(p,native+0xd19dd0,2028);
 unsigned count=sizeof(hac_data_targets)/sizeof(*hac_data_targets);for(unsigned i=0;i<count;++i){memcpy(p+4096+i*16,native+hac_data_targets[i],16);if(hac_data_targets[i]==0x166e250){uintptr_t canary=(uintptr_t)p+4096+count*16;memcpy(p+4096+i*16,&canary,8);uint64_t value=UINT64_C(0x1234567890abcdef);memcpy((void*)canary,&value,8);}}
 for(unsigned i=0;i<sizeof(hac_relocs)/sizeof(*hac_relocs);++i){const HACReloc*q=hac_relocs+i;int32_t d;memcpy(&d,native+0xd19dd0+q->offset+q->disp_offset,4);if((int64_t)0xd19dd0+q->offset+q->length+d!=q->native_target||hac_data_targets[q->index]!=q->native_target){VirtualFree(p,0,MEM_RELEASE);return 0;}d=4096+q->index*16-q->offset-q->length;memcpy(p+q->offset+q->disp_offset,&d,4);}
 for(unsigned i=0;i<sizeof(hac_calls)/sizeof(*hac_calls);++i){const HACCall*q=hac_calls+i;int32_t d;memcpy(&d,native+0xd19dd0+q->offset+1,4);if(native[0xd19dd0+q->offset]!=0xe8||(int64_t)0xd19dd0+q->offset+5+d!=q->native_target){VirtualFree(p,0,MEM_RELEASE);return 0;}unsigned slot=2064+i*16;uintptr_t fn=q->native_target==0x674180?(uintptr_t)hac_euler:q->native_target==0xcc9b30?(uintptr_t)hac_target:(uintptr_t)hac_canary_failure;hat_jump(p+slot,fn);d=(int32_t)slot-q->offset-5;memcpy(p+q->offset+1,&d,4);}
 DWORD old;if(!VirtualProtect(p,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(p,0,MEM_RELEASE);return 0;}FlushInstructionCache(GetCurrentProcess(),p,2112);k->allocation=k->code=p;k->data_count=count;return 1;
}
static int hac_native_step(void*opaque,unsigned char camera[0x3a0],const HACInputs*input,uint32_t*post){
 HACNativeKernel*k=opaque;if(!k||!k->code||(input->mxcsr_pre|input->mxcsr_mid)&~0xffffu)return 0;
 _Alignas(16)unsigned char copy[0x3a0];memcpy(copy,camera,sizeof copy);memcpy(hac_active_target,input->target,sizeof hac_active_target);
 const HACInputs*previous=hac_active_input;unsigned char*previous_camera=hac_active_camera;hac_active_input=input;hac_active_camera=copy;
 uint32_t host;__asm__ volatile("stmxcsr %0":"=m"(host));__asm__ volatile("ldmxcsr %0"::"m"(input->mxcsr_pre):"memory");typedef void(ABI *Fn)(void*,float);((Fn)k->code)(copy,input->dt);uint32_t after;__asm__ volatile("stmxcsr %0":"=m"(after));__asm__ volatile("ldmxcsr %0"::"m"(host):"memory");
 hac_active_input=previous;hac_active_camera=previous_camera;memcpy(camera,copy,sizeof copy);if(post)*post=after;return 1;
}
static void hac_native_destroy(HACNativeKernel*k){if(k->allocation)VirtualFree(k->allocation,0,MEM_RELEASE);memset(k,0,sizeof *k);}
#endif
