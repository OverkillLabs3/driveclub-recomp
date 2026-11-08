/* Raw native camera-target observations; no physics writes. Type1 point velocity
 * is sampled before projection/gain. Canonical resampling assumes piecewise
 * linear velocity and shortest-arc SO(3) orientation between observed frames.
 * Unobserved impacts/teleports are not claimed exact; lifecycle starts new epoch.
 */
#define CRR_CAPACITY 64
#define CRR_SAMPLES 128
#define CRR_HAS_NATIVE_OBSERVATION 1
typedef struct {double t;float velocity[3],q[4],forward[3];} CRRPoint;
typedef struct {
 uintptr_t owner,target,binding;uint64_t epoch;uint32_t frame;unsigned count;
 double time,time_compensation;uintptr_t global_clock;int64_t global_time_us,global_dt_us,global_birth_us,global_origin_end_us;float gains[4],reset_force[2];unsigned reset_force_mask,origin_frame;int origin_known,origin_reset;CRRPoint points[CRR_SAMPLES];
} CRRHistory;
static CRRHistory crr_histories[CRR_CAPACITY];
static SRWLOCK crr_lock=SRWLOCK_INIT;
static uint64_t crr_next_epoch;
typedef struct{uintptr_t self,binding;unsigned frame,kind;float dt;int reset,consecutive;float velocity[3],previous[3],basis[16];} CRRNativeObservation;
static CRRNativeObservation crr_observations[128];
static void camera_reference_target_discontinuity(uintptr_t);
static void crr_forget_native_observation(uintptr_t target){for(unsigned i=0;i<128;++i)if(crr_observations[i].self==target)memset(crr_observations+i,0,sizeof crr_observations[i]);}
static __attribute__((unused))void camera_spring_native_observation(void*self,uint32_t frame,float dt,int reset,int consecutive){
    CRRNativeObservation observation={0};observation.self=(uintptr_t)self;observation.frame=frame;observation.dt=dt;observation.reset=reset;observation.consecutive=consecutive;
    if(reset)camera_reference_target_discontinuity((uintptr_t)self);
    if(!camera_pose_read((uintptr_t)self+0x28,&observation.binding,8)||!camera_pose_read((uintptr_t)self+8,&observation.kind,4)||
       !camera_pose_read((uintptr_t)self+0x120,observation.velocity,12)||!camera_pose_read((uintptr_t)self+0x200,observation.previous,12)||!camera_pose_read((uintptr_t)self+0xd0,observation.basis,64))return;
    AcquireSRWLockExclusive(&crr_lock);CRRNativeObservation*slot=NULL;
    for(unsigned i=0;i<128;++i)if(crr_observations[i].self==(uintptr_t)self){slot=crr_observations+i;break;}
    if(!slot)for(unsigned i=0;i<128;++i)if(!crr_observations[i].self){slot=crr_observations+i;break;}
    if(slot)*slot=observation;ReleaseSRWLockExclusive(&crr_lock);
}
static __attribute__((unused)) unsigned crr_native_observation_flags(uintptr_t target,unsigned frame){unsigned flags=0;AcquireSRWLockShared(&crr_lock);for(unsigned i=0;i<128;++i)if(crr_observations[i].self==target&&crr_observations[i].frame==frame){flags=crr_observations[i].reset?1:crr_observations[i].consecutive?2:0;break;}ReleaseSRWLockShared(&crr_lock);return flags;}
static void crr_invalidate(uintptr_t owner){
 AcquireSRWLockExclusive(&crr_lock);
 for(unsigned i=0;i<CRR_CAPACITY;++i)if(crr_histories[i].owner==owner)memset(crr_histories+i,0,sizeof crr_histories[i]);
 ReleaseSRWLockExclusive(&crr_lock);
}
static int crr_rigid_quaternion(const float*m,float*q){
 double error=0,det=(double)m[0]*(m[5]*m[10]-m[6]*m[9])-(double)m[1]*(m[4]*m[10]-m[6]*m[8])+(double)m[2]*(m[4]*m[9]-m[5]*m[8]);
 for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c){double dot=0;for(unsigned j=0;j<3;++j){if(!isfinite(m[r*4+j]))return 0;dot+=(double)m[r*4+j]*m[c*4+j];}double e=fabs(dot-(r==c));if(e>error)error=e;}
 /* A float32 numeric validity bound, not an artistic spring coefficient. */
 if(error>512*FLT_EPSILON||fabs(det-1)>1024*FLT_EPSILON)return 0;
 double x,y,z,w,s,trace=(double)m[0]+m[5]+m[10];
 if(trace>0){s=sqrt(trace+1)*2;w=s*.25;x=(m[9]-m[6])/s;y=(m[2]-m[8])/s;z=(m[4]-m[1])/s;}
 else if(m[0]>m[5]&&m[0]>m[10]){s=sqrt(1+(double)m[0]-m[5]-m[10])*2;w=(m[9]-m[6])/s;x=s*.25;y=(m[1]+m[4])/s;z=(m[2]+m[8])/s;}
 else if(m[5]>m[10]){s=sqrt(1+(double)m[5]-m[0]-m[10])*2;w=(m[2]-m[8])/s;x=(m[1]+m[4])/s;y=s*.25;z=(m[6]+m[9])/s;}
 else{s=sqrt(1+(double)m[10]-m[0]-m[5])*2;w=(m[4]-m[1])/s;x=(m[2]+m[8])/s;y=(m[6]+m[9])/s;z=s*.25;}
 double norm=sqrt(x*x+y*y+z*z+w*w);if(!(norm>0)||!isfinite(norm))return 0;
 q[0]=(float)(x/norm);q[1]=(float)(y/norm);q[2]=(float)(z/norm);q[3]=(float)(w/norm);return 1;
}
static void crr_slerp(const float*a,const float*b,double u,float*out){
 double dot=0;for(unsigned i=0;i<4;++i)dot+=(double)a[i]*b[i];double sign=dot<0?-1:1;dot=fmin(1.,fabs(dot));
 double l=1-u,r=u;
 if(1-dot>32*DBL_EPSILON){double theta=acos(dot),den=sin(theta);l=sin((1-u)*theta)/den;r=sin(u*theta)/den;}
 double norm=0;for(unsigned i=0;i<4;++i){out[i]=(float)(l*a[i]+r*sign*b[i]);norm+=(double)out[i]*out[i];}
 norm=sqrt(norm);for(unsigned i=0;i<4;++i)out[i]=(float)(out[i]/norm);
}
static int crr_value(const CRRHistory*h,double t,CRRPoint*out){
 double eps=64*DBL_EPSILON*fmax(1.,h->time);
 if(!h->count||t<h->points[0].t-eps||t>h->time+eps)return 0;
 if(t<=h->points[0].t+eps){*out=h->points[0];return 1;}
 for(unsigned i=1;i<h->count;++i)if(t<=h->points[i].t+eps){
  CRRPoint a=h->points[i-1],b=h->points[i];double u=(t-a.t)/(b.t-a.t);out->t=t;
  /* Integer-us history time and canonical ticks are accumulated differently.
     Within roundoff, preserve the observed native vector exactly rather than
     needlessly re-projecting an almost-endpoint quaternion. */
  if(fabs(t-b.t)<=eps){*out=b;return 1;}
  for(unsigned j=0;j<3;++j)out->velocity[j]=(float)((double)a.velocity[j]+((double)b.velocity[j]-a.velocity[j])*u);
  crr_slerp(a.q,b.q,u,out->q);
  double x=out->q[0],y=out->q[1],z=out->q[2],w=out->q[3];
  out->forward[0]=(float)(2*(x*z-y*w));out->forward[1]=(float)(2*(y*z+x*w));out->forward[2]=(float)(1-2*(x*x+y*y));return 1;
 }
 *out=h->points[h->count-1];return 1;
}
typedef struct {const CRRHistory*history;unsigned axis;float current_force;} CRRForceContext;
static __attribute__((unused)) int crr_force(void*context,double t,float*out){
 const CRRForceContext*c=context;const CRRHistory*h=c->history;CRRPoint now,before;
 if(h->origin_known&&t<=(double)crs_T&&h->origin_reset){if(!(h->reset_force_mask&(1u<<c->axis)))return 0;*out=h->reset_force[c->axis];return 1;}
 if(h->origin_known&&t<(double)crs_T){*out=c->current_force;return isfinite(*out);}
 if(!crr_value(h,t,&now)||!crr_value(h,t-(double)crs_T,&before))return 0;
 volatile float inv=1.f/crs_T;float a[3];
 for(unsigned j=0;j<3;++j){volatile float delta=now.velocity[j]-before.velocity[j];a[j]=delta*inv;}
 volatile float value;
 if(c->axis==0){
  volatile float d0=now.forward[0]*a[0],d1=now.forward[1]*a[1],d2=now.forward[2]*a[2];volatile float xy=d0+d1;value=xy+d2;
  *out=value*h->gains[value>0?0:1];
 }else{value=a[1];*out=value*h->gains[value>0?2:3];}
 return isfinite(*out);
}
/* Core-side injection helper also used by CPU tests. Count one observation/frame. */
static __attribute__((unused)) int crr_advance(CRRHistory*h,uintptr_t owner,uintptr_t target,uintptr_t binding,uint32_t frame,float dt,const float*v,const float*m,const float*g){
 float q[4];if(!(dt>0)||dt>.25f||!crr_rigid_quaternion(m,q))return 0;
 for(unsigned i=0;i<3;++i)if(!isfinite(v[i]))return 0;
 for(unsigned i=0;i<4;++i)if(!isfinite(g[i]))return 0;
 int reset=h->owner!=owner||h->target!=target||h->binding!=binding||memcmp(h->gains,g,16)||(h->count&&frame!=h->frame&&frame!=h->frame+1u);
 if(reset){memset(h,0,sizeof *h);h->owner=owner;h->target=target;h->binding=binding;h->epoch=++crr_next_epoch;memcpy(h->gains,g,16);}
 if(h->count&&frame==h->frame)return 1;
 if(h->count)crs_time_add(&h->time,&h->time_compensation,crs_elapsed(dt));
 if(h->count==CRR_SAMPLES){memmove(h->points,h->points+1,(CRR_SAMPLES-1)*sizeof h->points[0]);--h->count;}
 CRRPoint*p=h->points+h->count++;p->t=h->time;memcpy(p->velocity,v,12);memcpy(p->q,q,16);memcpy(p->forward,m+8,12);h->frame=frame;return 1;
}
static int crr_advance_global(CRRHistory*h,uintptr_t owner,uintptr_t target,uintptr_t binding,uint32_t frame,float dt,const float*v,const float*m,const float*g,const HCSClock*clock){
 float q[4];if(!clock||!clock->valid||clock->frame!=frame||clock->dt_us<=0||clock->dt_us>8*33333)return 0;float reconstructed=(float)clock->dt_us*1e-6f;if(memcmp(&reconstructed,&dt,4))return 0;if(!(dt>0)||dt>.25f||!crr_rigid_quaternion(m,q))return 0;
 for(unsigned i=0;i<3;++i)if(!isfinite(v[i]))return 0;
 for(unsigned i=0;i<4;++i)if(!isfinite(g[i]))return 0;
 int reset=h->global_clock!=clock->clock||(h->count&&frame!=h->frame&&h->global_time_us+clock->dt_us!=clock->time_us)||h->owner!=owner||h->target!=target||h->binding!=binding||memcmp(h->gains,g,16)||(h->count&&frame!=h->frame&&frame!=h->frame+1u);
 if(reset){memset(h,0,sizeof *h);h->owner=owner;h->target=target;h->binding=binding;h->epoch=++crr_next_epoch;memcpy(h->gains,g,16);}
 if(h->count&&frame==h->frame)return 1;
 h->time=(double)crs_T*((double)clock->time_us/33333.);h->global_clock=clock->clock;h->global_time_us=clock->time_us;h->global_dt_us=clock->dt_us;
 if(h->count==CRR_SAMPLES){memmove(h->points,h->points+1,(CRR_SAMPLES-1)*sizeof h->points[0]);--h->count;}
 CRRPoint*p=h->points+h->count++;p->t=h->time;memcpy(p->velocity,v,12);memcpy(p->q,q,16);memcpy(p->forward,m+8,12);h->frame=frame;return 1;
}
static int crr_capture(void*camera,uint32_t frame,float dt,unsigned axis,float observed_force,CRRHistory*out){
 HCSClock clock=hcs_read_clock(frame,dt);if(!clock.valid)return 0;
 uintptr_t begin=0,end=0,target=0,binding=0;unsigned kind=0,target_frame=0;float v[3],m[16],g[4];
 if(!camera_pose_read((uintptr_t)camera+0x148,&begin,8)||!camera_pose_read((uintptr_t)camera+0x150,&end,8)||end<=begin||
    (end-begin)%8||end-begin>8*4096||!camera_pose_read(begin,&target,8)||
    !camera_pose_read(target+8,&kind,4)||kind!=1||!camera_pose_read(target+0x44,&target_frame,4)||target_frame!=frame||
    !camera_pose_read(target+0x28,&binding,8)||!binding||!camera_pose_read(target+0x120,v,12)||!camera_pose_read(target+0xd0,m,64)||
    !camera_pose_read((uintptr_t)camera+0x4fc,g,8)||!camera_pose_read((uintptr_t)camera+0x4ec,g+2,8))return 0;
 AcquireSRWLockExclusive(&crr_lock);CRRHistory*h=NULL;
 for(unsigned i=0;i<CRR_CAPACITY;++i)if(crr_histories[i].owner==(uintptr_t)camera){h=crr_histories+i;break;}
 if(!h)for(unsigned i=0;i<CRR_CAPACITY;++i)if(!crr_histories[i].owner){h=crr_histories+i;break;}
 int ok=h&&crr_advance_global(h,(uintptr_t)camera,target,binding,frame,dt,v,m,g,&clock);
 if(ok&&h->count==1){
    const CRRNativeObservation*obs=NULL;for(unsigned i=0;i<128;++i)if(crr_observations[i].self==target&&crr_observations[i].frame==frame){obs=crr_observations+i;break;}
    if(obs&&obs->binding==binding&&obs->kind==kind&&!memcmp(&obs->dt,&dt,4)&&(obs->reset||obs->consecutive)){
        CRRPoint current=h->points[0];h->points[0]=current;h->global_birth_us=clock.time_us-clock.dt_us;h->global_origin_end_us=clock.time_us;h->points[0].t=(double)crs_T*((double)h->global_birth_us/33333.);
        memcpy(h->points[0].velocity,obs->reset?obs->velocity:obs->previous,12);
        h->points[1]=current;h->count=2;h->time=current.t;h->origin_known=1;h->origin_reset=obs->reset;h->origin_frame=frame;
    }
 }
 if(ok&&h->origin_known&&h->origin_reset&&frame==h->origin_frame){
    if(observed_force!=0){h->origin_known=0;h->origin_reset=0;h->reset_force_mask=0;h->points[0]=h->points[h->count-1];h->count=1;}
    else{h->reset_force[axis]=observed_force;h->reset_force_mask|=1u<<axis;}
 }
 if(ok)*out=*h;ReleaseSRWLockExclusive(&crr_lock);return ok;
}

static int crr_force_global(void*context,int64_t endpoint_us,int64_t interval_us,float*out){
 const CRRForceContext*c=context;const CRRHistory*h=c->history;CRRPoint now,before;if(interval_us<=0)return 0;
 if(h->origin_known&&h->origin_reset&&endpoint_us<=h->global_origin_end_us){if(!(h->reset_force_mask&(1u<<c->axis)))return 0;*out=h->reset_force[c->axis];return 1;}
 double end=(double)crs_T*((double)endpoint_us/33333.),start=(double)crs_T*((double)(endpoint_us-interval_us)/33333.);
 if(!crr_value(h,end,&now)||!crr_value(h,start,&before))return 0;
 volatile float inv=1.f/((float)interval_us*1e-6f);float a[3];for(unsigned j=0;j<3;++j){volatile float delta=now.velocity[j]-before.velocity[j];a[j]=delta*inv;}
 volatile float value;if(c->axis==0){volatile float d0=now.forward[0]*a[0],d1=now.forward[1]*a[1],d2=now.forward[2]*a[2],xy=d0+d1;value=xy+d2;*out=value*h->gains[value>0?0:1];}else{value=a[1];*out=value*h->gains[value>0?2:3];}return isfinite(*out);
}
