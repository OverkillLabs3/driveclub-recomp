#ifndef CAMERA_ANGULAR_LIVE_CORE_INCLUDED
#define CAMERA_ANGULAR_LIVE_CORE_INCLUDED
/* Authored angular solver. Private canonical24 bytes; original world basis is
 * composed on every render. Q forcing uses observed angular velocity and a
 * shortest-arc rigid basis; no synthetic GForce or hand-tuned strength. */
typedef struct {uintptr_t owner,target;uint32_t frame;unsigned valid,adapted;int64_t credit_us,canonical_us,time_us;uintptr_t clock;unsigned primed;uint64_t ticks,resets;unsigned char canonical[24],display[24];float matrix[16],velocity[4];} HCALState;
typedef int(*HCALStep)(void*,unsigned char[0x3a0],const HACInputs*,uint32_t*);
typedef int(*HCALEuler)(void*,const float[16],float[3]);
static void hcal_forget(HCALState*s){uint64_t n=s->resets+1;memset(s,0,sizeof *s);s->resets=n;}
static void hcal_remember(HCALState*s,const HATRecord*r,const unsigned char*display,const HCSClock*clock){s->clock=clock->clock;s->time_us=clock->time_us;s->owner=r->camera;s->target=r->target;s->frame=r->frame;s->valid=1;memcpy(s->matrix,r->before+0x2e0,64);memcpy(s->velocity,r->target_consumed+0x130,16);memcpy(s->display,display,24);}
static int hcal_update(HCALState*state,const HATRecord*r,HCALStep step,void*context,HCALEuler euler,void*euler_context,unsigned char output[0x3a0],const HCSClock*clock){
 int64_t us;float q[4];if(!clock||!clock->valid){hcal_forget(state);return HCL_UNSUPPORTED;}const float*matrix=(const float*)(r->before+0x2e0);
 if((r->flags&15)!=15||memcmp(&r->dt,&r->dt_math,4)||!crs_native_us(r->dt,&us)||us>8*33333||us!=clock->dt_us||r->frame!=clock->frame||!hcl_finite(r->before+0x340,6)||!hcl_finite(r->target_consumed+0x130,4)||!crr_rigid_quaternion(matrix,q)){hcal_forget(state);return HCL_UNSUPPORTED;}
 HCALState s=*state;
 if(!s.valid||s.owner!=r->camera||s.target!=r->target||s.clock!=clock->clock||s.time_us+us!=clock->time_us||r->frame!=s.frame+1u||memcmp(s.display,r->before+0x340,24)){
  hcal_forget(&s);memcpy(s.canonical,r->before+0x340,24);memcpy(s.matrix,matrix,64);memcpy(s.velocity,r->target_consumed+0x130,16);s.canonical_us=clock->time_us-us;s.primed=s.canonical_us%33333==0;
 }
 if(!s.adapted&&us==33333&&!s.credit_us){memcpy(s.canonical,r->after+0x340,24);s.canonical_us=clock->time_us;s.primed=s.canonical_us%33333==0;++s.ticks;hcal_remember(&s,r,r->after+0x340,clock);*state=s;return HCL_NATIVE;}
 s.adapted=1;int64_t step_us=33333-s.canonical_us%33333;
 while(s.canonical_us+step_us<=clock->time_us){
  int64_t endpoint=s.canonical_us+step_us,offset=endpoint-(clock->time_us-us);double u=(double)offset/us;_Alignas(16)unsigned char camera[0x3a0];memcpy(camera,r->before,sizeof camera);memcpy(camera+0x340,s.canonical,24);
  _Alignas(16)float basis[16];hcl_basis(s.matrix,matrix,u,basis);memcpy(camera+0x2e0,basis,64);
  HACInputs in={0};in.dt=(float)step_us*1e-6f;in.mxcsr_pre=r->mxcsr_pre;in.mxcsr_mid=r->mxcsr_mid;memcpy(in.target,r->target_consumed,sizeof in.target);
  if(u>=1)memcpy(in.euler,r->euler_consumed,12);else if(!euler(euler_context,basis,in.euler)){hcal_forget(state);return HCL_BACKEND_FAILED;}
  for(unsigned j=0;j<4;++j){float a,b;memcpy(&a,s.velocity+j,4);memcpy(&b,r->target_consumed+0x130+4*j,4);float v=u>=1?b:(float)((double)a+((double)b-a)*u);memcpy(in.target+0x130+4*j,&v,4);}
  if(!step(context,camera,&in,NULL)||!hcl_finite(camera+0x340,6)){hcal_forget(state);return HCL_BACKEND_FAILED;}
  memcpy(s.canonical,camera+0x340,24);++s.ticks;s.canonical_us=endpoint;s.primed=1;step_us=33333;
 }
 s.credit_us=clock->time_us-s.canonical_us;
 memcpy(output,r->before,0x3a0);memcpy(output+0x340,s.canonical,24);
 if(s.credit_us){
  HACInputs in={0};in.dt=(float)s.credit_us*1e-6f;memcpy(in.euler,r->euler_consumed,12);in.mxcsr_pre=r->mxcsr_pre;in.mxcsr_mid=r->mxcsr_mid;memcpy(in.target,r->target_consumed,sizeof in.target);
  if(!step(context,output,&in,NULL)||!hcl_finite(output+0x340,6)){hcal_forget(state);return HCL_BACKEND_FAILED;}
 }else{
  /* Recompose with CURRENT base without advancing the private state. At dt0
     this original helper computes the basis and leaves the angular state. */
  HACInputs in={0};in.dt=0;memcpy(in.euler,r->euler_consumed,12);in.mxcsr_pre=r->mxcsr_pre;in.mxcsr_mid=r->mxcsr_mid;memcpy(in.target,r->target_consumed,sizeof in.target);
  if(!step(context,output,&in,NULL)||!hcl_finite(output+0x340,6)){hcal_forget(state);return HCL_BACKEND_FAILED;}
 }
 float display_q[4];if(!crr_rigid_quaternion((float*)(output+0x360),display_q)){hcal_forget(state);return HCL_UNSUPPORTED;}
 hcal_remember(&s,r,output+0x340,clock);*state=s;return HCL_APPLIED;
}
#endif
