#include "gforce-native-kernel.inc"
#include "gforce-reference-core.inc"
#include "gforce-basis-helpers.inc"
static GFRNativeKernel gfr_kernel;static int gfr_enabled;
static SRWLOCK gfr_lock=SRWLOCK_INIT;static GFRTransaction gfr_states[64];
static volatile LONG gfr_step_sequence;static volatile LONG64 gfr_epoch=1;
static void gfr_forget(uintptr_t owner){
 driver_palette_discontinuity();
 AcquireSRWLockExclusive(&gfr_lock);for(unsigned i=0;i<64;++i)if(!owner||gfr_states[i].state.self==owner){gfr_invalidate(&gfr_states[i].state);gfr_states[i].has_before=0;}InterlockedIncrement64(&gfr_epoch);ReleaseSRWLockExclusive(&gfr_lock);
}
static int gfr_basis(const float*a,const float*b,double u,float*out){
 memcpy(out,b,64);if(u==1||(!memcmp(a,b,12)&&!memcmp(a+4,b+4,12)&&!memcmp(a+8,b+8,12)))return 1;
 float qa[4],qb[4],q[4];if(!gfr_rigid_quaternion(a,qa)||!gfr_rigid_quaternion(b,qb))return 0;gfr_slerp(qa,qb,u,q);
 double x=q[0],y=q[1],z=q[2],w=q[3];out[0]=(float)(1-2*(y*y+z*z));out[1]=(float)(2*(x*y-z*w));out[2]=(float)(2*(x*z+y*w));out[4]=(float)(2*(x*y+z*w));out[5]=(float)(1-2*(x*x+z*z));out[6]=(float)(2*(y*z-x*w));out[8]=(float)(2*(x*z-y*w));out[9]=(float)(2*(y*z+x*w));out[10]=(float)(1-2*(x*x+y*y));return 1;
}
static int gfr_step(void*context,const float*previous,const float*velocity,const float*matrix,float dt,const float*before,float*filter,float*jolt,uint32_t csr){
 GfoScope*s=context;uint32_t after;
 gfr_native_step(&gfr_kernel,previous,velocity,matrix,dt,before,filter,jolt,csr,&after);
 if(camera_accuracy_trace_active()){uint32_t sequence=(uint32_t)InterlockedIncrement(&gfr_step_sequence);
 float data[24]={0};memcpy(data,previous,16);memcpy(data+4,velocity,16);memcpy(data+8,before,16);memcpy(data+12,filter,16);memcpy(data+16,jolt,16);data[20]=dt;memcpy(data+21,&csr,4);memcpy(data+22,&after,4);memcpy(data+23,&sequence,4);
 camera_accuracy_log(28,(void*)s->self,s->model,0xd2a763,dt,data,24);
 memset(data,0,sizeof data);memcpy(data,matrix,64);memcpy(data+16,&s->logical_time,8);memcpy(data+18,&s->frame21c,4);memcpy(data+19,&s->sequence,4);memcpy(data+23,&sequence,4);
 camera_accuracy_log(29,(void*)s->self,s->clock,0xd2a763,dt,data,24);}
 return 1;
}
#ifdef GFR_REFERENCE_OWNS_SEEK
typedef uintptr_t(ABI*GFRSeek)(void*,int64_t);static GFRSeek gfr_seek_original;
static ABI uintptr_t gfr_seek(void*self,int64_t time){gfr_forget(0);uintptr_t result=gfr_seek_original(self,time);gfr_forget(0);return result;}
#endif
static int gfr_install(void){
 const char*p=camera_fix_getenv("BB_GFORCE_REFERENCE");if(!p||strcmp(p,"1"))return 1;
 if(!gfr_native_init(&gfr_kernel,image,runtime_image_size)){puts("GForceReference: original AVX signatures unavailable; no reference or observer installed");return 0;}
#ifdef GFR_REFERENCE_OWNS_SEEK
 static const unsigned char seek[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x49,0x89,0xfe};
 gfr_seek_original=(GFRSeek)camera_rate_hook(0x762340,seek,sizeof seek,(uintptr_t)gfr_seek);
 if(!gfr_seek_original){gfr_native_destroy(&gfr_kernel);puts("GForceReference: existing seek hook not recognized; skipped rather than duplicating lifecycle hooks");return 0;}
#endif
 gfr_enabled=1;puts("GForceReference: isolated native33333us canonical six animation inputs held betweenQ; native30 no override, original component memory/animation kept");return 1;
}
static float gfr_select(GfoScope*s,const char*name,float native){
 if(!gfr_enabled||!s)return native;
 static const uintptr_t names[]={0x135f1d0,0x135f1de,0x135f1ec,0x135f1fa,0x135f20c,0x135f21e};unsigned which=6;
 for(unsigned j=0;j<6;++j)if((uintptr_t)name==runtime_image_base+names[j]){which=j;break;}if(which==6)return native;
 if(!s->reference_computed){
  s->reference_computed=1;s->reference_epoch=(uint64_t)InterlockedCompareExchange64(&gfr_epoch,0,0);
  GFRInput input={0};input.self=s->self;input.owner=s->owner;input.binding=s->resolved_owner;input.clock=s->clock;input.frame=s->frame21c;input.time=s->logical_time;input.us=s->clock_dt;input.dt=s->dt;
  __asm__ volatile("stmxcsr %0":"=m"(input.csr));
  memcpy(input.velocity,s->raw,16);memcpy(input.previous_velocity,s->previous,16);memcpy(input.matrix,s->axes,64);memcpy(input.before_filter,s->filtered_before,16);
  int valid=(s->flags&15)==15&&camera_pose_read(s->self+0x18,&input.model,8)&&camera_pose_read(s->self+0x50,input.native_filter,16)&&s->native_frame&&camera_pose_read(s->native_frame-0x60,input.native_jolt,16);
  float roundtrip=(float)input.us*1e-6f;valid=valid&&!memcmp(&roundtrip,&input.dt,4)&&!memcmp(&s->raw_dt,&s->dt,4);
  /* Query/copy outside GFR lock; LSQ never calls back into GFR or HSQ. */
  LsqSelector selector={0};selector.owner=input.owner;selector.clock=input.clock;LsqPoint private_point;int known_replay=0;
  int private_status=lsq_scope_hold(&selector,input.time,&known_replay,&private_point);
  if(known_replay){
   input.private_late_Q=1;
   if(private_status==LSQ_OK&&input.us>0&&input.us<=33333){input.private_sensor_epoch=private_point.epoch;input.private_q=private_point.q;memcpy(input.private_velocity_Q,private_point.vector[LSQ_LATE_VELOCITY],16);memcpy(input.private_seed_velocity,private_point.vector[LSQ_LATE_VELOCITY],16);}
   else valid=0; /* explicit unavailable; NEVER old raw interpolation for known Replay scope */
  }
  int status=-3;GFRState snapshot={0};
  AcquireSRWLockExclusive(&gfr_lock);GFRTransaction*state=NULL;
  for(unsigned j=0;j<64;++j)if(gfr_states[j].state.self==s->self){state=gfr_states+j;break;}
  if(!state)for(unsigned j=0;j<64;++j)if(!gfr_states[j].state.self){state=gfr_states+j;break;}
  if(state&&s->reference_epoch==(uint64_t)InterlockedCompareExchange64(&gfr_epoch,0,0)){
   if(valid){status=gfr_transaction_update(state,&input,gfr_step,gfr_basis,s,s->reference_values);s->reference_applied=status==1;}else{gfr_invalidate(&state->state);state->has_before=0;}snapshot=state->state;
  }
  ReleaseSRWLockExclusive(&gfr_lock);
  if(camera_accuracy_trace_active()){
  float data[24]={0};memcpy(data,input.native_filter,12);memcpy(data+3,input.native_jolt,12);memcpy(data+6,s->reference_values,24);data[12]=(float)status;data[13]=(float)s->reference_applied;data[14]=(float)snapshot.ticks;data[15]=(float)snapshot.resets;memcpy(data+16,&s->logical_time,8);memcpy(data+18,&snapshot.canonical_time,8);memcpy(data+23,&s->sequence,4);
  camera_accuracy_log(30,(void*)s->self,input.model,0xd2a650,s->dt,data,24);
  memset(data,0,sizeof data);memcpy(data,snapshot.canonical_velocity,16);memcpy(data+4,snapshot.filter,16);memcpy(data+8,snapshot.jolt,16);memcpy(data+12,&snapshot.next_boundary,8);memcpy(data+14,&s->reference_epoch,8);memcpy(data+16,&snapshot.time,8);data[18]=(float)snapshot.adapted;memcpy(data+23,&s->sequence,4);
  camera_accuracy_log(31,(void*)s->self,input.owner,0xd2a650,s->dt,data,24);
  }
  if(lsq_enabled&&lsq_trace_file!=INVALID_HANDLE_VALUE){LsqPoint diagnostic={0};diagnostic.q=input.private_q;diagnostic.observed_render_T=input.time;diagnostic.epoch=input.private_sensor_epoch;diagnostic.binding.owner=input.owner;diagnostic.binding.clock=input.clock;diagnostic.binding.vehicle=known_replay&&private_status==LSQ_OK?private_point.binding.vehicle:0;diagnostic.mask=(uint32_t)input.private_late_Q;memcpy(diagnostic.vector[0],input.private_velocity_Q,16);lsq_trace(4,known_replay?private_status:LSQ_NATIVE_BYPASS,&diagnostic);}

 }
 if(s->reference_applied&&s->reference_epoch==(uint64_t)InterlockedCompareExchange64(&gfr_epoch,0,0))return s->reference_values[which];return native;
}
