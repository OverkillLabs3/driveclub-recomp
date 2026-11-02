#ifndef CAMERA_ANGULAR_INITIALIZER_RUNTIME_INCLUDED
#define CAMERA_ANGULAR_INITIALIZER_RUNTIME_INCLUDED
/* Optional replacement for owner-local HSC countdown retiming. Native manager
 * still executes exactly once. Only angular/counter fields are committed. */
static int hai_enabled;
static HSIKernel hai_copy_kernel;
static __attribute__((used))volatile uintptr_t hai_copy_resume,hai_reset_resume,hai_tail_resume;
static LONG64 hai_reset_serial;
typedef struct {
 uintptr_t owner,target,frame_address;float dt;HCSClock clock;uint32_t mxcsr_pre,mxcsr_mid;
 unsigned source_seen,reset_seen,valid,finished;int entry_count;uint64_t reset_token,generation,proof_serial;HAIState state;HCALSlot*slot;
 _Alignas(16)unsigned char reset_state[0x3a0],target_state[0x150];float basis[16],velocity[4];
} HAIManagerCall;
static _Thread_local HAIManagerCall*hai_active_call;
#include "private-proof-runtime.inc"
typedef uintptr_t(ABI*HAICtor)(void*,void*,void*);
typedef uintptr_t(ABI*HAIDtor)(void*);
static HAICtor hai_ctor_original;static HAIDtor hai_dtor_original[2];
static void hai_lifetime_mark(uintptr_t owner,int destroyed){
 AcquireSRWLockExclusive(&hcal_lock);for(unsigned i=0;i<64;++i)if(hcal_slots[i].owner==owner){HCALSlot*s=hcal_slots+i;hcal_forget(&s->state);hsc_forget(&s->snap);hai_invalidate(&s->initializer,1);s->generation=++hcal_generation;if(destroyed&&!s->busy&&!s->snap_busy)memset(s,0,sizeof*s);break;}ReleaseSRWLockExclusive(&hcal_lock);
}
static ABI uintptr_t hai_ctor(void*self,void*angular,void*body){uint32_t pre=_mm_getcsr();hai_lifetime_mark((uintptr_t)self,0);_mm_setcsr(pre);uintptr_t result=hai_ctor_original(self,angular,body);uint32_t post=_mm_getcsr();hai_lifetime_mark((uintptr_t)self,0);_mm_setcsr(post);return result;}
static ABI uintptr_t hai_dtor_delete(void*self){uint32_t pre=_mm_getcsr();hai_lifetime_mark((uintptr_t)self,1);_mm_setcsr(pre);uintptr_t result=hai_dtor_original[0](self);uint32_t post=_mm_getcsr();hai_lifetime_mark((uintptr_t)self,1);_mm_setcsr(post);return result;}
static ABI uintptr_t hai_dtor_keep(void*self){uint32_t pre=_mm_getcsr();hai_lifetime_mark((uintptr_t)self,1);_mm_setcsr(pre);uintptr_t result=hai_dtor_original[1](self);uint32_t post=_mm_getcsr();hai_lifetime_mark((uintptr_t)self,1);_mm_setcsr(post);return result;}
static void hai_apply_tail(HAIManagerCall*,void*);
static int hai_source_clock(HAIManagerCall*c,void*frame){
 unsigned f=0;float dt=0;if(!camera_pose_read((uintptr_t)frame-0xb4,&dt,4)||memcmp(&dt,&c->dt,4)||!hat_frame_read(&f)||f!=c->clock.frame)return 0;HCSClock now=hcs_read_clock(f,dt);return now.valid&&now.clock==c->clock.clock&&now.time_us==c->clock.time_us&&now.dt_us==c->clock.dt_us;
}
static __attribute__((used,noinline))ABI void hai_capture_native(unsigned stage,void*frame,void*self,void*target,uint32_t mxcsr,const unsigned char*saved){
 (void)saved;HAIManagerCall*c=hai_active_call;if(!c||c->owner!=(uintptr_t)self)return;
 if(stage==2){hai_apply_tail(c,frame);return;}
 if(stage==1){
  /* This callback follows the ORIGINAL D1952C/D19533 stores in the bridge. */
  c->reset_seen=1;c->reset_token=(uint64_t)InterlockedIncrement64(&hai_reset_serial);
  if(!c->clock.valid||!hai_source_clock(c,frame)||!camera_pose_read((uintptr_t)self,c->reset_state,sizeof c->reset_state)||hai_count(c->reset_state)!=3)c->valid=0;
 }else{
  c->source_seen=1;c->frame_address=(uintptr_t)frame;c->target=(uintptr_t)target;c->mxcsr_mid=mxcsr;
  if(!hai_source_clock(c,frame)||!camera_pose_read((uintptr_t)frame-0x80,c->basis,64)||!camera_pose_read((uintptr_t)target,c->target_state,sizeof c->target_state))c->valid=0;
  memcpy(c->velocity,c->target_state+0x130,16);
 }
}
static void hai_raw_solver_input(void*frame,void*self,void*target,uint32_t mxcsr){
 HAIManagerCall*c=hai_active_call;if(!hai_enabled||!c||c->owner!=(uintptr_t)self)return;c->source_seen=2;c->target=(uintptr_t)target;c->mxcsr_mid=mxcsr;
 float dt=0;unsigned f=0;HCSClock clock={0};if(!camera_pose_read((uintptr_t)frame-0x40,&dt,4)||memcmp(&dt,&c->dt,4)||!hat_frame_read(&f))c->valid=0;else clock=hcs_read_clock(f,dt);
 if(!clock.valid||clock.clock!=c->clock.clock||clock.time_us!=c->clock.time_us||clock.dt_us!=c->clock.dt_us||clock.frame!=c->clock.frame||!camera_pose_read((uintptr_t)self+0x2e0,c->basis,64)||!camera_pose_read((uintptr_t)target,c->target_state,sizeof c->target_state))c->valid=0;
 memcpy(c->velocity,c->target_state+0x130,16);
}
static int hai_runtime_copy(void*ctx,unsigned char camera[0x3a0],const HSIInput*in){(void)ctx;return haip_copy(camera,in);}
static int hai_runtime_step(void*ctx,unsigned char camera[0x3a0],const HACInputs*in){(void)ctx;return haip_solver(camera,in);}
static int hai_runtime_euler(void*ctx,const float basis[16],float out[3]){return hcal_native_euler(ctx,basis,out);}
static void hai_apply_tail(HAIManagerCall*c,void*frame){
 /* Commit BEFORE D19A07 consumes +360 and composes the final +2E0 world
  * basis. A post-D19140 write would be one frame too late for that output. */
 if(c->finished){c->valid=0;return;}c->finished=1;void*self=(void*)c->owner;HCALSlot*slot=c->slot;HAIState state=c->state;uint64_t generation=c->generation;
 _Alignas(16)unsigned char native[0x3a0],out[0x3a0];HAIResult result={HAI_UNSUPPORTED,0,0,0,0,0,0};int read=camera_pose_read((uintptr_t)self,native,sizeof native);
 if(!hai_source_clock(c,frame))c->valid=0;
 if(c->valid&&c->source_seen&&read&&native[0x33a]){
  HAIObservation o={0};o.owner=(uintptr_t)self;o.target=c->target;o.lifetime=generation;o.reset_token=c->reset_seen?c->reset_token:state.reset_token;o.clock=c->clock;o.reset_observed=c->reset_seen;o.reset_time_us=c->clock.time_us;o.reset_state=c->reset_state;o.native_after=native;o.mxcsr_pre=c->mxcsr_pre;o.mxcsr_mid=c->mxcsr_mid;memcpy(o.basis,c->basis,64);memcpy(o.velocity,c->velocity,16);memcpy(o.target_state,c->target_state,sizeof o.target_state);result=hai_update(&state,&o,hai_runtime_copy,hai_runtime_step,hai_runtime_euler,NULL,out);
 }else hai_invalidate(&state,c->reset_seen!=0);
 AcquireSRWLockExclusive(&hcal_lock);unsigned char actual_count[4],actual_angle[24],actual_matrix[64];int unchanged=read&&camera_pose_read((uintptr_t)self+0x33c,actual_count,4)&&camera_pose_read((uintptr_t)self+0x340,actual_angle,24)&&camera_pose_read((uintptr_t)self+0x360,actual_matrix,64)&&!memcmp(actual_count,native+0x33c,4)&&!memcmp(actual_angle,native+0x340,24)&&!memcmp(actual_matrix,native+0x360,64);
 unsigned applied=0;if(slot->generation==generation&&unchanged){slot->initializer=state;if(result.applied){hai_fields(self,out);applied=1;}}else if(slot->generation==generation){hai_invalidate(&slot->initializer,1);slot->generation=++hcal_generation;}
 ReleaseSRWLockExclusive(&hcal_lock);
 float audit[24]={(float)result.status,(float)c->entry_count,read?(float)hai_count(native):0,applied?(float)result.count:read?(float)hai_count(native):0,(float)c->reset_seen,(float)result.copies,(float)unchanged,(float)result.solvers,(float)applied,(float)c->source_seen,(float)state.primed};memcpy(audit+12,&c->clock.time_us,8);memcpy(audit+14,&result.canonical_us,8);memcpy(audit+16,&result.credit_us,8);memcpy(audit+18,&state.reset_token,8);memcpy(audit+20,&generation,8);camera_accuracy_log(14,self,c->clock.clock,generation,c->dt,audit,22);haip_commit(c,&result,native,applied,(unsigned)unchanged);
}
static ABI void hai_manager(void*self,float dt){
 uint32_t entry_csr=_mm_getcsr();HCALSlot*slot;HAIManagerCall call={0};
 AcquireSRWLockExclusive(&hcal_lock);slot=hcal_slot((uintptr_t)self);
 if(!slot||slot->snap_busy){ReleaseSRWLockExclusive(&hcal_lock);HAIManagerCall*previous=hai_active_call;hai_active_call=NULL;_mm_setcsr(entry_csr);hcal_manager_original(self,dt);uint32_t native_csr=_mm_getcsr();hai_active_call=previous;if(slot){AcquireSRWLockExclusive(&hcal_lock);hai_invalidate(&slot->initializer,1);slot->generation=++hcal_generation;ReleaseSRWLockExclusive(&hcal_lock);}_mm_setcsr(native_csr);return;}
 slot->snap_busy=1;call.state=slot->initializer;call.slot=slot;call.generation=slot->generation;ReleaseSRWLockExclusive(&hcal_lock);
 call.owner=(uintptr_t)self;call.proof_serial=haip_next_serial();call.dt=dt;call.mxcsr_pre=entry_csr;call.valid=1;unsigned frame=0;
 if(!camera_pose_read((uintptr_t)self+0x33c,&call.entry_count,4))call.valid=0;
 if(hat_frame_read(&frame))call.clock=hcs_read_clock(frame,dt);if(!call.clock.valid)call.valid=0;
 HAIManagerCall*previous=hai_active_call;hai_active_call=&call;_mm_setcsr(entry_csr);hcal_manager_original(self,dt);uint32_t native_csr=_mm_getcsr();hai_active_call=previous;
 AcquireSRWLockExclusive(&hcal_lock);if(!call.finished&&slot->generation==call.generation)hai_invalidate(&slot->initializer,call.reset_seen!=0);slot->snap_busy=0;ReleaseSRWLockExclusive(&hcal_lock);_mm_setcsr(native_csr);
}
#include "angular-initializer-bridges.inc"
static int hai_input_hooks_install(void){
 static const unsigned char copy[]={0xc5,0xf8,0x28,0x80,0x30,1,0,0,0xc5,0xf8,0x28,0x4d,0x80,0xc5,0xf8,0x59,0xd1};
 static const unsigned char reset[]={0xc6,0x83,0x38,3,0,0,0,0xc7,0x83,0x3c,3,0,0,3,0,0,0};
 static const unsigned char tail[]={0x80,0xbb,0x3a,3,0,0,0,0x74,0x3b,0xc4,0xc1,0x78,0x28,0x46,0x30};
 static const unsigned char ctor[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x49,0x89,0xd7,0x49,0x89,0xf6,0x48,0x89,0xfb};
 static const unsigned char dtor_delete[]={0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,0x48,0x8d,0x05,0xd0,0xbf,0x93,0};
 static const unsigned char dtor_keep[]={0x48,0x8d,0x05,0xa9,0xbf,0x93,0,0x48,0x83,0xc0,0x10,0x48,0x89,0x07};
 if(runtime_image_size<0xd19a16||memcmp(image+0xd1995c,copy,sizeof copy)||memcmp(image+0xd1952c,reset,sizeof reset)||memcmp(image+0xd19a07,tail,sizeof tail)||memcmp(image+0xd18b20,ctor,sizeof ctor)||memcmp(image+0xd18e50,dtor_delete,sizeof dtor_delete)||memcmp(image+0xd18e80,dtor_keep,sizeof dtor_keep)||!hsi_native_init(&hai_copy_kernel,image,runtime_image_size))return 0;
 unsigned char*gateway=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!gateway)return 0;memcpy(gateway,copy,sizeof copy);hsi_jump(gateway+sizeof copy,(uintptr_t)image+0xd1996d);hai_copy_resume=(uintptr_t)gateway;hai_reset_resume=(uintptr_t)image+0xd1953d;
 memcpy(gateway+64,tail,7);gateway[71]=0x75;gateway[72]=14;hsi_jump(gateway+73,(uintptr_t)image+0xd19a4b);memcpy(gateway+87,tail+9,6);hsi_jump(gateway+93,(uintptr_t)image+0xd19a16);hai_tail_resume=(uintptr_t)gateway+64;
 memcpy(gateway+128,ctor,sizeof ctor);hsi_jump(gateway+128+sizeof ctor,(uintptr_t)image+0xd18b34);hai_ctor_original=(HAICtor)(gateway+128);
 /* Preserve the original RIP-relative vptr LEA exactly via MOVABS. A raw
  * stolen-byte copy here would resolve the wrong vtable from the gateway. */
 int32_t disp;uintptr_t vptr;memcpy(&disp,dtor_delete+12,4);vptr=(uintptr_t)image+0xd18e60+disp;memcpy(gateway+224,dtor_delete,9);memcpy(gateway+233,"\x48\xb8",2);memcpy(gateway+235,&vptr,8);hsi_jump(gateway+243,(uintptr_t)image+0xd18e60);hai_dtor_original[0]=(HAIDtor)(gateway+224);
 memcpy(&disp,dtor_keep+3,4);vptr=(uintptr_t)image+0xd18e87+disp;memcpy(gateway+320,"\x48\xb8",2);memcpy(gateway+322,&vptr,8);memcpy(gateway+330,dtor_keep+7,7);hsi_jump(gateway+337,(uintptr_t)image+0xd18e8e);hai_dtor_original[1]=(HAIDtor)(gateway+320);
 DWORD old;if(!VirtualProtect(gateway,4096,PAGE_EXECUTE_READ,&old)||!VirtualProtect(image+0xd18b20,0xd19a16-0xd18b20,PAGE_EXECUTE_READWRITE,&old))return 0;hsi_jump(image+0xd1952c,(uintptr_t)hai_reset_bridge);memset(image+0xd1952c+14,0x90,3);hsi_jump(image+0xd1995c,(uintptr_t)hai_copy_bridge);memset(image+0xd1995c+14,0x90,3);hsi_jump(image+0xd19a07,(uintptr_t)hai_tail_bridge);image[0xd19a15]=0x90;hsi_jump(image+0xd18b20,(uintptr_t)hai_ctor);memset(image+0xd18b20+14,0x90,sizeof ctor-14);hsi_jump(image+0xd18e50,(uintptr_t)hai_dtor_delete);memset(image+0xd18e50+14,0x90,sizeof dtor_delete-14);hsi_jump(image+0xd18e80,(uintptr_t)hai_dtor_keep);DWORD unused;VirtualProtect(image+0xd18b20,0xd19a16-0xd18b20,old,&unused);FlushInstructionCache(GetCurrentProcess(),gateway,368);FlushInstructionCache(GetCurrentProcess(),image+0xd18b20,0xd19a16-0xd18b20);return 1;
}
#endif
