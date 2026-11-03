static _Thread_local HATRecord*hcal_mid_record;
#include "head-angular-native-clone.inc"
#include "angular-live-core.inc"
#include "head-copy-native-kernel.inc"
static void haip_label(unsigned,int64_t,int64_t);
#include "angular-initializer-core.inc"
#include "runtime_camera_head_snap_counter.inc"
static HACNativeKernel hcal_kernel;static int hcal_enabled;
typedef struct {HCALState state;HSCState snap;HAIState initializer;uintptr_t owner;uint64_t generation;unsigned busy,snap_busy;} HCALSlot;
static HCALSlot hcal_slots[64];static uint64_t hcal_generation;
static SRWLOCK hcal_lock=SRWLOCK_INIT;
static void hcal_invalidate_all(uintptr_t owner){
 AcquireSRWLockExclusive(&hcal_lock);for(unsigned i=0;i<64;++i)if(!owner||hcal_slots[i].owner==owner){HCALSlot*s=hcal_slots+i;hcal_forget(&s->state);hsc_forget(&s->snap);hai_invalidate(&s->initializer,1);s->generation=++hcal_generation;}ReleaseSRWLockExclusive(&hcal_lock);
}
static HCALSlot*hcal_slot(uintptr_t owner){
 for(unsigned i=0;i<64;++i)if(hcal_slots[i].owner==owner)return hcal_slots+i;
 for(unsigned i=0;i<64;++i)if(!hcal_slots[i].owner){hcal_slots[i].owner=owner;hcal_slots[i].generation=++hcal_generation;return hcal_slots+i;}return NULL;
}
static HATHeader*hcal_header;static HATRecord*hcal_records;static SRWLOCK hcal_tape_lock=SRWLOCK_INIT;
static int hcal_original_step(void*context,unsigned char camera[0x3a0],const HACInputs*input,uint32_t*post){
 const HATRecord*source=context;HATRecord r=*source;r.dt=r.dt_math=input->dt;r.mxcsr_pre=input->mxcsr_pre;r.mxcsr_mid=input->mxcsr_mid;memcpy(r.before,camera,0x3a0);memcpy(r.euler_consumed,input->euler,12);memcpy(r.target_consumed,input->target,sizeof r.target_consumed);
 HATRecord*previous=hcal_mid_record;hcal_mid_record=hcal_header?&r:NULL;
 uint32_t after;int ok=hac_native_step(&hcal_kernel,camera,input,&after);hcal_mid_record=previous;if(!ok)return 0;if(post)*post=after;
 if(hcal_header){memcpy(r.after,camera,0x3a0);r.mxcsr_post=after;AcquireSRWLockExclusive(&hcal_tape_lock);unsigned n=hcal_header->count;if(n<hcal_header->capacity){r.sequence=n;hcal_records[n]=r;MemoryBarrier();hcal_header->count=n+1;}else ++hcal_header->overflow;ReleaseSRWLockExclusive(&hcal_tape_lock);}return 1;
}
static int hcal_native_euler(void*context,const float matrix[16],float euler[3]){
 (void)context;_Alignas(16)float copy[16];memcpy(copy,matrix,64);typedef void(ABI*Euler)(void*,float*,float*,float*);
 /* Pure native trigonometry on a PRIVATE basis. Never calls CC9B30 again. */
 ((Euler)(runtime_image_base+0x674180))(copy,euler,euler+1,euler+2);uint32_t p;memcpy(&p,euler,4);p^=0x80000000u;memcpy(euler,&p,4);return hcl_finite(euler,3);
}
static HSCNativeUpdate hcal_manager_original;
#include "angular-initializer-runtime.inc"
static void hcal_apply(const HATRecord*r){
 if(hai_enabled)return;
 if(!hcal_enabled)return;HCALState state;HCALSlot*slot;uint64_t generation;
 AcquireSRWLockExclusive(&hcal_lock);slot=hcal_slot(r->camera);
 if(!slot||slot->busy){ReleaseSRWLockExclusive(&hcal_lock);return;}slot->busy=1;state=slot->state;generation=slot->generation;ReleaseSRWLockExclusive(&hcal_lock);
 _Alignas(16)unsigned char out[0x3a0];HCSClock clock=hcs_read_clock(r->frame,r->dt);int status=hcal_update(&state,r,hcal_original_step,(void*)r,hcal_native_euler,NULL,out,&clock),applied=0;
 AcquireSRWLockExclusive(&hcal_lock);unsigned char actual[24];int unchanged=camera_pose_read(r->camera+0x340,actual,24)&&!memcmp(actual,r->after+0x340,24);
 if(slot->generation==generation&&unchanged){slot->state=state;if(status==HCL_APPLIED){memcpy((void*)(r->camera+0x340),out+0x340,24);memcpy((void*)(r->camera+0x360),out+0x360,64);applied=1;}}
 else if(slot->generation==generation){hcal_forget(&slot->state);slot->generation=++hcal_generation;}
 slot->busy=0;ReleaseSRWLockExclusive(&hcal_lock);
 float data[24]={(float)status,(float)applied,(float)state.credit_us,(float)state.ticks,(float)state.resets,(float)unchanged};memcpy(data+8,state.canonical,24);camera_accuracy_log(13,(void*)(uintptr_t)r->camera,r->target,r->frame,r->dt,data,24);
}
typedef struct {void*self;unsigned char after[8];} HCALSnapCall;
static _Thread_local HCALSnapCall*hcal_snap_call;
static ABI void hcal_snap_native_proxy(void*private_state,float dt){
 HCALSnapCall*c=hcal_snap_call;hcal_manager_original(c->self,dt);memcpy(c->after,(unsigned char*)c->self+0x338,8);memcpy((unsigned char*)private_state+0x338,c->after,8);
}
static ABI void hcal_manager(void*self,float dt){
 if(hai_enabled){hai_manager(self,dt);return;}
 uint32_t entry_csr=_mm_getcsr();
 HCALSlot*slot;HSCState snap;uint64_t generation;unsigned char before[8];uintptr_t clock=0;int64_t us=0;uint32_t bits;memcpy(&bits,&dt,4);
 if(!camera_pose_read((uintptr_t)self+0x338,before,8)){_mm_setcsr(entry_csr);hcal_manager_original(self,dt);return;}
 AcquireSRWLockExclusive(&hcal_lock);slot=hcal_slot((uintptr_t)self);
 if(!slot||slot->snap_busy){ReleaseSRWLockExclusive(&hcal_lock);_mm_setcsr(entry_csr);hcal_manager_original(self,dt);return;}
 if(before[0]){hcal_forget(&slot->state);hsc_forget(&slot->snap);slot->generation=++hcal_generation;}
 slot->snap_busy=1;snap=slot->snap;generation=slot->generation;ReleaseSRWLockExclusive(&hcal_lock);
 int context=camera_pose_read(runtime_image_base+0x1911100,&clock,8)&&camera_pose_read(clock+0x968,&clock,8)&&camera_pose_read(clock+0x1e0,&clock,8)&&(dt==0||crs_native_us(dt,&us));
 HSCContext c={generation,clock,generation,(double)us,bits,(unsigned)context,bits==0x3d08882fu};
 _Alignas(16)unsigned char copy[0x3a0]={0};memcpy(copy+0x338,before,8);HCALSnapCall call={self,{0}},*previous=hcal_snap_call;hcal_snap_call=&call;
 _mm_setcsr(entry_csr);HSCResult result=hsc_update(&snap,&c,copy,dt,hcal_snap_native_proxy);uint32_t native_csr=_mm_getcsr();hcal_snap_call=previous;
 AcquireSRWLockExclusive(&hcal_lock);unsigned char actual[8];int unchanged=camera_pose_read((uintptr_t)self+0x338,actual,8)&&!memcmp(actual,call.after,8);
 if(slot->generation==generation&&unchanged){slot->snap=snap;if(result.after!=result.native_after)memcpy((unsigned char*)self+0x33c,&result.after,4);}
 else if(slot->generation==generation){hsc_forget(&slot->snap);slot->generation=++hcal_generation;}
 slot->snap_busy=0;ReleaseSRWLockExclusive(&hcal_lock);
 float data[24]={(float)result.status,(float)result.before,(float)result.native_after,(float)result.after,(float)result.reset,(float)result.ticks,(float)unchanged};camera_accuracy_log(14,self,clock,generation,dt,data,7);_mm_setcsr(native_csr);
}
static int hcal_install(void){
 const char*v=camera_fix_getenv("BB_CAMERA_ANGULAR_REFERENCE");if(!v||atoi(v)!=1)return 0;
 static const unsigned char manager[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x98,0,0,0};
 if(!hsc_image_supported(image,runtime_image_size)||!hac_native_init(&hcal_kernel,image,runtime_image_size))return 0;
 const char*init=camera_fix_getenv("BB_CAMERA_ANGULAR_INIT_SHARED_Q");int init_requested=init&&atoi(init)==1;if(init_requested&&(!hat_install()||!hai_input_hooks_install())){puts("AngularInitializer: unsupported input hooks; manager not adapted");return 0;}
 hcal_manager_original=(HSCNativeUpdate)camera_rate_hook(0xd19140,manager,sizeof manager,(uintptr_t)hcal_manager);
 if(!hcal_manager_original){hac_native_destroy(&hcal_kernel);return 0;}
 if(init_requested){hai_enabled=1;puts("AngularInitializer: actual reset token, original copies/solver on shared globalQ, commit before native world composition");}
 hcal_enabled=1;puts("AngularReference: LIVE native angular solver and three-Q snap window; sampled rigid input, native30 unchanged");return 1;
}
