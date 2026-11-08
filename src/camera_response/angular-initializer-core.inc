#ifndef CAMERA_ANGULAR_INITIALIZER_CORE_INCLUDED
#define CAMERA_ANGULAR_INITIALIZER_CORE_INCLUDED
/* Experimental CPU-only transaction backend. The caller supplies an ACTUAL
 * D19533 reset notification, actual lifetime/binding identity, native after
 * state and a coherent shared clock. Count==3 never establishes an epoch.
 * Known reset state is anchored at its actual observed time, not T-dt.
 * No native manager, getter, or live whole-object write occurs in this core. */
enum {HAI_NATIVE=0,HAI_APPLIED=1,HAI_UNSUPPORTED=-1,HAI_BACKEND_FAILED=-2};
typedef int(*HAICopy)(void*,unsigned char[0x3a0],const HSIInput*);
typedef int(*HAIStep)(void*,unsigned char[0x3a0],const HACInputs*);
typedef int(*HAIEuler)(void*,const float[16],float[3]);
typedef struct {
 uintptr_t owner,target,clock;uint64_t lifetime,reset_token;
 int64_t time_us,canonical_us;uint32_t frame;unsigned valid,adapted,primed;
 uint64_t copies,solvers,resets;int count;
 unsigned char angular[24],matrix[64],display_angular[24],display_matrix[64];int display_count;float basis[16],velocity[4];
} HAIState;
typedef struct {
 uintptr_t owner,target;uint64_t lifetime,reset_token;HCSClock clock;
 /* Set only for the current, genuinely observed D19533 write. Its saved
  * state is AFTER the original reset-copy and count=3 store. */
 unsigned reset_observed;int64_t reset_time_us;
 const unsigned char *reset_state,*native_after;
 float basis[16],velocity[4];unsigned char target_state[0x150];
 uint32_t mxcsr_pre,mxcsr_mid;
} HAIObservation;
typedef struct {int status;unsigned applied;unsigned copies,solvers;int count;int64_t canonical_us,credit_us;} HAIResult;
static int hai_count(const unsigned char*p){int v;memcpy(&v,p+0x33c,4);return v;}
static void hai_put_count(unsigned char*p,int v){memcpy(p+0x33c,&v,4);}
static void hai_fields(unsigned char*dst,const unsigned char*src){
 memcpy(dst+0x33c,src+0x33c,4);memcpy(dst+0x340,src+0x340,24);memcpy(dst+0x360,src+0x360,64);
}
static void hai_invalidate(HAIState*s,int actual_lifetime_end){
 uint64_t n=s->resets+1;unsigned role=actual_lifetime_end?0:s->adapted;
 uintptr_t owner=s->owner,target=s->target,clock=s->clock;uint64_t life=s->lifetime,token=s->reset_token;
 memset(s,0,sizeof*s);s->resets=n;s->adapted=role;
 if(!actual_lifetime_end){s->owner=owner;s->target=target;s->clock=clock;s->lifetime=life;s->reset_token=token;}
}
static void hai_commit(HAIState*s,const unsigned char*p){s->count=hai_count(p);memcpy(s->angular,p+0x340,24);memcpy(s->matrix,p+0x360,64);}
static void hai_restore(unsigned char*p,const HAIState*s){hai_put_count(p,s->count);memcpy(p+0x340,s->angular,24);memcpy(p+0x360,s->matrix,64);}
static void hai_remember(HAIState*s,const HAIObservation*o,const unsigned char*display){s->time_us=o->clock.time_us;s->frame=o->clock.frame;memcpy(s->basis,o->basis,64);memcpy(s->velocity,o->velocity,16);s->display_count=hai_count(display);memcpy(s->display_angular,display+0x340,24);memcpy(s->display_matrix,display+0x360,64);s->valid=1;}
static HAIResult hai_update(HAIState*state,const HAIObservation*o,HAICopy copy,HAIStep step,HAIEuler euler,void*ctx,unsigned char output[0x3a0]){
 HAIResult r={HAI_NATIVE,0,0,0,0,0,0};HAIState s=*state;float quat[4];int64_t us=o->clock.dt_us,t=o->clock.time_us;
 if(!o->native_after||!o->owner||!o->target||!o->lifetime||!o->clock.valid||us<0||us>8*33333||t<0||!hcl_finite(o->velocity,4)||!hcl_finite(o->native_after+0x340,6)||hai_count(o->native_after)<0||hai_count(o->native_after)>3||!crr_rigid_quaternion(o->basis,quat)){
  hai_invalidate(state,0);r.status=HAI_UNSUPPORTED;return r;
 }
 int changed=s.owner!=o->owner||s.target!=o->target||s.lifetime!=o->lifetime||s.clock!=o->clock.clock;
 if(changed){hai_invalidate(&s,1);s.owner=o->owner;s.target=o->target;s.clock=o->clock.clock;s.lifetime=o->lifetime;}
 int reset=o->reset_observed&&o->reset_token&&o->reset_token!=s.reset_token;
 if(reset){
  if(!o->reset_state||o->reset_time_us!=t||hai_count(o->reset_state)!=3||!hcl_finite(o->reset_state+0x340,6)){
   hai_invalidate(state,0);r.status=HAI_UNSUPPORTED;return r;
  }
  unsigned role=s.adapted;hai_invalidate(&s,0);s.adapted=role;s.reset_token=o->reset_token;s.canonical_us=t;s.primed=t%33333==0;hai_commit(&s,o->reset_state);hai_remember(&s,o,o->reset_state);
 }else if(!s.reset_token||s.reset_token!=o->reset_token){
  /* An unobserved construction/reset is not reconstructed from a counter. */
  hai_invalidate(&s,1);s.owner=o->owner;s.target=o->target;s.clock=o->clock.clock;s.lifetime=o->lifetime;*state=s;return r;
 }
 if(!s.adapted&&us==33333){
  /* Strict pure native30: original manager already ran exactly once. No
   * private transaction and no output write, including all three copies. */
  hai_commit(&s,o->native_after);s.canonical_us=t;s.primed=t%33333==0;hai_remember(&s,o,o->native_after);*state=s;r.count=s.count;r.canonical_us=t;return r;
 }
 if(!reset){
  if(!s.valid){
   /* Re-prime from a known CURRENT native state, never fabricate T-dt. */
   hai_commit(&s,o->native_after);s.canonical_us=t;s.primed=t%33333==0;hai_remember(&s,o,o->native_after);*state=s;r.count=s.count;r.canonical_us=t;return r;
  }
  if(t==s.time_us&&o->clock.frame==s.frame){memcpy(output,o->native_after,0x3a0);hai_put_count(output,s.display_count);memcpy(output+0x340,s.display_angular,24);memcpy(output+0x360,s.display_matrix,64);r.status=HAI_APPLIED;r.applied=1;r.count=s.count;r.canonical_us=s.canonical_us;r.credit_us=t-s.canonical_us;return r;}
  if(us<=0||t!=s.time_us+us||o->clock.frame!=s.frame+1u){hai_invalidate(state,0);r.status=HAI_UNSUPPORTED;return r;}
 }
 s.adapted=1;_Alignas(16)unsigned char work[0x3a0];memcpy(work,o->native_after,sizeof work);hai_restore(work,&s);
 if(reset&&t%33333==0){
  /* The original count=3 write and first copy are in the SAME native call.
   * A birth already ON a global boundary consumes that discrete copy now;
   * no solver integration or fabricated pre-birth interval is introduced. */
  HSIInput in={0};memcpy(in.matrix,o->basis,64);memcpy(in.angular,o->velocity,16);in.mxcsr_mid=o->mxcsr_mid;
  haip_label(1,t,0);if(!copy(ctx,work,&in)||hai_count(work)!=2){hai_invalidate(state,0);r.status=HAI_BACKEND_FAILED;return r;}hai_commit(&s,work);++s.copies;++r.copies;
 }
 int64_t delta=33333-s.canonical_us%33333;
 while(s.canonical_us+delta<=t){
  int64_t endpoint=s.canonical_us+delta;
  if(endpoint<s.time_us||t<=s.time_us){hai_invalidate(state,0);r.status=HAI_UNSUPPORTED;return r;}
  double u=(double)(endpoint-s.time_us)/(double)(t-s.time_us);HSIInput in={0};hcl_basis(s.basis,o->basis,u,in.matrix);
  for(unsigned a=0;a<4;++a)in.angular[a]=u>=1?o->velocity[a]:(float)((double)s.velocity[a]+((double)o->velocity[a]-s.velocity[a])*u);
  in.mxcsr_mid=o->mxcsr_mid;memcpy(work+0x2e0,in.matrix,64);
  if(s.count>0){haip_label(2,endpoint,delta);if(!copy(ctx,work,&in)||hai_count(work)!=s.count-1){hai_invalidate(state,0);r.status=HAI_BACKEND_FAILED;return r;}++s.copies;++r.copies;}
  else{HACInputs solver={0};solver.dt=(float)delta*1e-6f;solver.mxcsr_pre=o->mxcsr_pre;solver.mxcsr_mid=o->mxcsr_mid;memcpy(solver.target,o->target_state,sizeof solver.target);memcpy(solver.target+0x130,in.angular,16);haip_label(3,endpoint,delta);if(!euler(ctx,in.matrix,solver.euler)||!step(ctx,work,&solver)){hai_invalidate(state,0);r.status=HAI_BACKEND_FAILED;return r;}++s.solvers;++r.solvers;}
  if(!hcl_finite(work+0x340,6)||hai_count(work)<0||hai_count(work)>3){hai_invalidate(state,0);r.status=HAI_BACKEND_FAILED;return r;}
  hai_commit(&s,work);s.canonical_us=endpoint;s.primed=1;delta=33333;
 }
 /* Compose a private CURRENT display without consuming a countdown call. */
 memcpy(work+0x2e0,o->basis,64);int64_t residual=t-s.canonical_us;
 if(s.count>0){
  HSIInput in={0};memcpy(in.matrix,o->basis,64);memcpy(in.angular,o->velocity,16);in.mxcsr_mid=o->mxcsr_mid;
  haip_label(4,t,residual);if(!copy(ctx,work,&in)||hai_count(work)!=s.count-1){hai_invalidate(state,0);r.status=HAI_BACKEND_FAILED;return r;}hai_put_count(work,s.count);
 }else{
  HACInputs solver={0};solver.dt=(float)residual*1e-6f;solver.mxcsr_pre=o->mxcsr_pre;solver.mxcsr_mid=o->mxcsr_mid;memcpy(solver.target,o->target_state,sizeof solver.target);memcpy(solver.target+0x130,o->velocity,16);
  haip_label(5,t,residual);if(!euler(ctx,o->basis,solver.euler)||!step(ctx,work,&solver)){hai_invalidate(state,0);r.status=HAI_BACKEND_FAILED;return r;}
 }
 if(!hcl_finite(work+0x340,6)||!crr_rigid_quaternion((const float*)(work+0x360),quat)){hai_invalidate(state,0);r.status=HAI_BACKEND_FAILED;return r;}
 memcpy(output,o->native_after,0x3a0);hai_fields(output,work);hai_remember(&s,o,output);*state=s;r.status=HAI_APPLIED;r.applied=1;r.count=s.count;r.canonical_us=s.canonical_us;r.credit_us=t-s.canonical_us;return r;
}
#endif
