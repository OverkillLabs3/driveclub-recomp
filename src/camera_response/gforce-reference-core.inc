#include <math.h>
/* Explicit sampled-input model: original nativeAVX GForce transaction at shared
 * global33333us boundaries. Six authored outputs are HELD between boundaries;
 * no tiny fractional derivative/Jolt and no fitted strength/filter constants.
 * The first observed native update seeds the private state at its known time.
 * An initial partial transaction reaches the first globalQ; fullQ thereafter.
 * Earlier unobserved state is not claimed reconstructed. */
typedef struct {
 uintptr_t self,model,owner,binding,clock;uint32_t frame,csr;
 int64_t time,us;float dt,velocity[4],previous_velocity[4],matrix[16];
 int private_late_Q;uint64_t private_sensor_epoch;int64_t private_q;float private_velocity_Q[4],private_seed_velocity[4];
 float before_filter[4],native_filter[4],native_jolt[4];
} GFRInput;
typedef struct {
 uintptr_t self,model,owner,binding,clock;uint32_t frame;unsigned valid,adapted;
 int64_t time,canonical_time,next_boundary;uint64_t ticks,resets;
 int private_late_Q;uint64_t private_sensor_epoch;
 float velocity[4],matrix[16],canonical_velocity[4],filter[4],jolt[4],native_filter[4];
} GFRState;
typedef int(*GFRStep)(void*,const float[4],const float[4],const float[16],float,const float[4],float[4],float[4],uint32_t);
typedef int(*GFRBasis)(const float[16],const float[16],double,float[16]);
static int gfr_finite(const float*p,unsigned count){for(unsigned i=0;i<count;++i)if(!isfinite(p[i]))return 0;return 1;}
static void gfr_invalidate(GFRState*s){uint64_t resets=s->resets+1;memset(s,0,sizeof*s);s->resets=resets;}
static void gfr_seed(GFRState*s,const GFRInput*i){
 uint64_t resets=s->resets,ticks=s->ticks;memset(s,0,sizeof*s);s->resets=resets;s->ticks=ticks;
 s->self=i->self;s->model=i->model;s->owner=i->owner;s->binding=i->binding;s->clock=i->clock;s->frame=i->frame;s->valid=1;s->adapted=i->private_late_Q||i->us!=33333;
 s->time=s->canonical_time=i->time;s->next_boundary=(i->time/33333+1)*33333;
 memcpy(s->velocity,i->velocity,16);memcpy(s->matrix,i->matrix,64);memcpy(s->canonical_velocity,i->velocity,16);s->canonical_velocity[3]=i->previous_velocity[3];
 s->private_late_Q=i->private_late_Q;s->private_sensor_epoch=i->private_sensor_epoch;
 if(i->private_late_Q){memcpy(s->canonical_velocity,i->private_seed_velocity,12);} /* filter keeps ACTUAL observed seed time, never relabeled Q */
 memcpy(s->filter,i->native_filter,16);memcpy(s->jolt,i->native_jolt,16);memcpy(s->native_filter,i->native_filter,16);
}
static int gfr_update(GFRState*state,const GFRInput*i,GFRStep step,GFRBasis basis,void*ctx,float output[6]){
 if(!i->self||!i->model||!i->owner||!i->binding||!i->clock||i->us<=0||i->us>8*33333||i->time<i->us||!gfr_finite(i->velocity,4)||!gfr_finite(i->previous_velocity,4)||!gfr_finite(i->matrix,16)||!gfr_finite(i->before_filter,4)||!gfr_finite(i->native_filter,4)||!gfr_finite(i->native_jolt,4)){gfr_invalidate(state);return -1;}
 if(i->private_late_Q&&(!i->private_sensor_epoch||i->us>33333||!gfr_finite(i->private_velocity_Q,4)||!gfr_finite(i->private_seed_velocity,4))){gfr_invalidate(state);return -4;}
 GFRState s=*state;
 int continuous=s.private_late_Q==i->private_late_Q&&(!i->private_late_Q||s.private_sensor_epoch==i->private_sensor_epoch)&&s.valid&&s.self==i->self&&s.model==i->model&&s.owner==i->owner&&s.binding==i->binding&&s.clock==i->clock&&s.frame+1u==i->frame&&i->time-s.time==i->us&&!memcmp(s.velocity,i->previous_velocity,12)&&!memcmp(s.native_filter,i->before_filter,16);
 if(!continuous){gfr_invalidate(&s);gfr_seed(&s,i);*state=s;return 0;}
 /* A pure native30 epoch is a strict no-override path, including actual reset
    state and all six original setter arguments. An established VRR epoch may
    contain Q-sized stalls and must keep its existing canonical history. */
 if(!s.adapted&&i->us==33333){uint64_t ticks=s.ticks+1;gfr_seed(&s,i);s.ticks=ticks;*state=s;return 0;}
 s.adapted=1;
 while(s.next_boundary<=i->time){
  int64_t duration=s.next_boundary-s.canonical_time;if(duration<=0||duration>33333){gfr_invalidate(state);return -1;}
  double u=(double)(s.next_boundary-s.time)/(double)i->us;if(u<0||u>1){gfr_invalidate(state);return -1;}
  float velocity[4],matrix[16],filter[4],jolt[4];
  if(i->private_late_Q){
   if(i->us>33333||i->private_q!=s.next_boundary||!gfr_finite(i->private_velocity_Q,4)){gfr_invalidate(state);return -4;}
   memcpy(velocity,i->private_velocity_Q,16); /* EXACT private endpoint; raw19 is continuity only */
  }else for(unsigned j=0;j<4;++j)velocity[j]=u==1?i->velocity[j]:(float)((double)s.velocity[j]+((double)i->velocity[j]-s.velocity[j])*u);
  if(!basis(s.matrix,i->matrix,u,matrix)||!gfr_finite(matrix,16)){gfr_invalidate(state);return -1;}
  float dt=(float)duration*1e-6f;
  if(!step(ctx,s.canonical_velocity,velocity,matrix,dt,s.filter,filter,jolt,i->csr)||!gfr_finite(filter,4)||!gfr_finite(jolt,4)){gfr_invalidate(state);return -2;}
  memcpy(s.filter,filter,16);memcpy(s.jolt,jolt,16);memcpy(s.canonical_velocity,velocity,12);s.canonical_time=s.next_boundary;s.next_boundary+=33333;++s.ticks;
 }
 memcpy(output,s.filter,12);memcpy(output+3,s.jolt,12);
 s.time=i->time;s.frame=i->frame;memcpy(s.velocity,i->velocity,16);memcpy(s.matrix,i->matrix,64);memcpy(s.native_filter,i->native_filter,16);*state=s;return 1;
}
/* One native logical timestamp can publish a newer getter result later in the
 * same render phase. Roll back only the private transaction and recompute that
 * timestamp from its original checkpoint. This replaces the knot, not time or
 * canonical tick count. Physical before/after continuity is still verified
 * against the latest actual native update before accepting a replacement. */
typedef struct {GFRState state,before;int has_before;int64_t last_us;} GFRTransaction;
static int gfr_transaction_update(GFRTransaction*t,const GFRInput*i,GFRStep step,GFRBasis basis,void*ctx,float output[6]){
 GFRState*s=&t->state;
 int duplicate=s->private_late_Q==i->private_late_Q&&(!i->private_late_Q||s->private_sensor_epoch==i->private_sensor_epoch)&&t->has_before&&s->valid&&s->self==i->self&&s->model==i->model&&s->owner==i->owner&&s->binding==i->binding&&s->clock==i->clock&&s->time==i->time&&t->last_us==i->us&&(s->frame==i->frame||s->frame+1u==i->frame)&&!memcmp(s->velocity,i->previous_velocity,12)&&!memcmp(s->native_filter,i->before_filter,16);
 if(duplicate){
  GFRState replacement=t->before;GFRInput input=*i;
  if(replacement.valid){input.frame=replacement.frame+1u;memcpy(input.previous_velocity,replacement.velocity,12);memcpy(input.before_filter,replacement.native_filter,16);}
  int status=gfr_update(&replacement,&input,step,basis,ctx,output);replacement.frame=i->frame;t->state=replacement;return status;
 }
 t->before=*s;t->has_before=1;t->last_us=i->us;return gfr_update(s,i,step,basis,ctx,output);
}
