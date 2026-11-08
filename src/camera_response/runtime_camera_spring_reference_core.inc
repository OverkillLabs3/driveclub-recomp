/* Isolated native30 scalar-state core, NOT installed. Runtime uses the raw-input
 * provider at canonical timestamps. The legacy scalar-input entry is retained
 * for CPU regression comparison only. Inactive timed impulses, native lifecycle
 * generation, camera+E8 frame after native near-zero dt normalization.
 */
#ifndef CAMERA_REFERENCE_C_PROPOSAL
#define CAMERA_REFERENCE_C_PROPOSAL
#include "shared-phase-clock-core.inc"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <float.h>
#include <limits.h>
#define CRS_FORCE_CAPACITY 128
#define CRS_STATE_CAPACITY 128
#ifndef ABI
#define ABI __attribute__((sysv_abi))
#endif
/* D847D0 ->78C8C/78C94 ->CC891A/CC891F: native float32 fixed30 duration,
 * truncated to signed64 microseconds, then converted back to float32 seconds.
 * Chase force/spring calls receive this dt unchanged; nominal1/30 differs. */
static const float crs_T=(float)(int64_t)((1.0f/30.0f)*1000000.0f)*1e-6f;
static const int64_t crs_period_us=(int64_t)((1.0f/30.0f)*1000000.0f);
static int crs_native_us(float dt,int64_t*out){
 if(!(dt>0)||!isfinite(dt)||(double)dt>64.0*(double)crs_T)return 0;
 int64_t us=(int64_t)llround((double)dt/(double)1e-6f);
 if(us<=0||us>64*crs_period_us)return 0;
 float roundtrip=(float)us*1e-6f;if(memcmp(&dt,&roundtrip,4))return 0;
 *out=us;return 1;
}
static double crs_elapsed(double dt){
 /* Only native float32 deltas with a bitwise-valid microsecond reconstruction
    enter this clock domain. Arbitrary/scaled/double elapsed samples stay intact.
    This changes canonical HISTORY TIME, never the observed force or native dt. */
 float raw=(float)dt;int64_t us;
 if((double)raw==dt&&crs_native_us(raw,&us))return(double)crs_T*((double)us/(double)crs_period_us);
 return dt;
}
static double crs_time_add(double*sum,double*compensation,double elapsed){
 /* Compensate repeated nonbinary native-us fractions. A long90/120Hz epoch
    must not miss its final canonical Q merely through += accumulation drift. */
 double y=elapsed-*compensation,next=*sum+y;*compensation=(next-*sum)-y;*sum=next;return next;
}
typedef float(ABI *CRSForce)(void*,float,float);
typedef float(ABI *CRSStep)(void*,float);
typedef struct { CRSForce force;CRSStep step; } CRSOps;
typedef int(*CRSForceProvider)(void*,double,float*);
typedef struct { float x,v; } CRSState;
typedef struct { CRSState stored;float returned; } CRSSample;
typedef struct { double time;float force; } CRSForceSample;
typedef struct {CRSSample display;CRSState canonical;double time,canonical_time;unsigned ticks;int reset,duplicate,partial;int64_t global_time_us,global_canonical_us;unsigned global_primed;} CRSOutput;
typedef struct {
 uintptr_t owner,spring;uint64_t generation,frame;int initialized,have_frame;
 uintptr_t global_clock;int64_t global_time_us,global_canonical_us;unsigned global_primed;
 unsigned char settings[0x2c];CRSState canonical,last_display;CRSSample last_native;
 double time,time_compensation,provider_origin;uint64_t provider_epoch;unsigned ticks,force_count;CRSForceSample forces[CRS_FORCE_CAPACITY];CRSOutput last_output;
} CRSAdapter;
typedef struct { CRSAdapter slots[CRS_STATE_CAPACITY]; } CRSStore;
static float crs_float(const void*p,unsigned off){float v;memcpy(&v,(const unsigned char*)p+off,4);return v;}
static unsigned crs_uint(const void*p,unsigned off){unsigned v;memcpy(&v,(const unsigned char*)p+off,4);return v;}
static int crs_same(CRSState a,CRSState b){return !memcmp(&a,&b,sizeof a);}
static int crs_finite(CRSState s){return isfinite(s.x)&&isfinite(s.v);}
static int crs_supported(const void*p){
 float d=crs_float(p,4),k=crs_float(p,0x18),lim=crs_float(p,8),step=crs_float(p,0x1c);
 unsigned maximum=crs_uint(p,0xc);
 return isfinite(d)&&d>=0&&isfinite(k)&&k>0&&isfinite(lim)&&lim>=0&&
        isfinite(step)&&step>0&&crs_T/step<(float)(INT_MAX-1)&&maximum&&maximum<=1024&&
        crs_float(p,0x20)==0&&isfinite(crs_float(p,0x28))&&crs_float(p,0x28)<=0;
}
static int crs_settings_same(const unsigned char*a,const void*b){
 return !memcmp(a,b,0x10)&&!memcmp(a+0x18,(const unsigned char*)b+0x18,0x14);
}
static CRSAdapter* crs_lookup(CRSStore*s,uintptr_t owner,uintptr_t spring){
 if(!owner||!spring)return 0;
 CRSAdapter*empty=0;
 for(unsigned i=0;i<CRS_STATE_CAPACITY;++i){CRSAdapter*a=s->slots+i;if(a->owner==owner&&a->spring==spring)return a;if(!a->owner&&!empty)empty=a;}
 if(empty){memset(empty,0,sizeof *empty);empty->owner=owner;empty->spring=spring;}return empty;
}
static void crs_invalidate_owner(CRSStore*s,uintptr_t owner){for(unsigned i=0;i<CRS_STATE_CAPACITY;++i)if(s->slots[i].owner==owner)memset(s->slots+i,0,sizeof s->slots[i]);}
static void crs_reset(CRSAdapter*a,uint64_t generation,const void*p,CRSState state,float force){
 memcpy(a->settings,p,0x2c);a->generation=generation;a->canonical=a->last_display=state;a->last_native=(CRSSample){state,state.x};
 a->time=0;a->time_compensation=0;a->ticks=0;a->force_count=1;a->forces[0]=(CRSForceSample){0,force};a->have_frame=0;a->initialized=1;
}
static float crs_force_at(const CRSAdapter*a,double t){
 for(unsigned i=1;i<a->force_count;++i)if(t<=a->forces[i].time){CRSForceSample l=a->forces[i-1],r=a->forces[i];double u=(t-l.time)/(r.time-l.time);return(float)(l.force+(r.force-l.force)*u);}
 return a->forces[a->force_count-1].force;
}
static CRSSample crs_native30(const CRSAdapter*a,CRSOps op,float force){
 _Alignas(16)unsigned char record[0x30]={0};memcpy(record,a->settings,0x2c);
 memcpy(record+0x10,&a->canonical.v,4);memcpy(record+0x14,&a->canonical.x,4);
 op.force(record,force,crs_T);float returned=op.step(record,crs_T);
 return(CRSSample){{crs_float(record,0x14),crs_float(record,0x10)},returned};
}
typedef struct{double a,b,c,d;} CRSMat;
static CRSMat crs_mul(CRSMat x,CRSMat y){return(CRSMat){x.a*y.a+x.b*y.c,x.a*y.b+x.b*y.d,x.c*y.a+x.d*y.c,x.c*y.b+x.d*y.d};}
static CRSMat crs_power(CRSMat m,unsigned n){CRSMat r={1,0,0,1};while(n){if(n&1)r=crs_mul(r,m);m=crs_mul(m,m);n>>=1;}return r;}
static int crs_fractional(const CRSAdapter*a,double phase,float force,CRSSample*out){
 float damping=crs_float(a->settings,4),stiffness=crs_float(a->settings,0x18),limit=crs_float(a->settings,8),max_step=crs_float(a->settings,0x1c);
 unsigned n=(unsigned)(crs_T/max_step)+1,maximum=crs_uint(a->settings,0xc);if(n>maximum)n=maximum;
 double h=(double)(crs_T/n),drag=1-damping*h;
 CRSMat m=crs_power((CRSMat){1-stiffness*h*h,h*drag,-stiffness*h,drag},n);
 double b0=m.b*force*crs_T,b1=m.d*force*crs_T,den=(1-m.a)*(1-m.d)-m.b*m.c;
 if(!isfinite(den)||fabs(den)<1e-12)return 0;
 double eqx=((1-m.d)*b0+m.b*b1)/den,eqv=(m.c*b0+(1-m.a)*b1)/den;
 double half=(m.a+m.d)*.5,det=m.a*m.d-m.b*m.c,disc=half*half-det,r=phase/crs_T,scale,identity;
 if(disc>1e-12){double gap=sqrt(disc),lo=half-gap,hi=half+gap;if(lo<=0)return 0;double pl=pow(lo,r),ph=pow(hi,r);scale=(ph-pl)/(2*gap);identity=(hi*pl-lo*ph)/(2*gap);}
 else if(disc< -1e-12){if(det<=0)return 0;double gap=sqrt(-disc),rho=sqrt(det),theta=atan2(gap,half),pr=pow(rho,r);scale=pr*sin(r*theta)/gap;identity=pr*cos(r*theta)-scale*half;}
 else {if(half<=0)return 0;scale=r*pow(half,r-1);identity=(1-r)*pow(half,r);}
 double dx=a->canonical.x-eqx,dv=a->canonical.v-eqv;
 float x=(float)(eqx+(scale*m.a+identity)*dx+scale*m.b*dv),v=(float)(eqv+scale*m.c*dx+(scale*m.d+identity)*dv);
 if(!isfinite(x)||!isfinite(v))return 0;
 *out=(CRSSample){{x,v},x};
 /* Known chase getters consume stored x. Bound only this displayed copy;
    native clamp timing in private canonical state remains untouched. */
 if(x>limit){out->stored.x=limit;out->stored.v=0;}else if(x < -limit){out->stored.x=-limit;out->stored.v=0;}
 return 1;
}
static int crs_render_provider(CRSAdapter*a,uint64_t generation,uint64_t frame,const void*settings,CRSState before_force,
 double dt,float force_end,CRSOps op,CRSOutput*out,int native_reset,
 CRSForceProvider provider,void*context,double raw_end,uint64_t raw_epoch){
 if(!a||!out||!op.force||!op.step||!crs_supported(settings)||!crs_finite(before_force)||!isfinite(force_end)||!isfinite(dt)||dt<=0)return 0;
 if(a->initialized&&!native_reset&&(!provider||a->provider_epoch==raw_epoch)&&generation==a->generation&&crs_settings_same(a->settings,settings)&&a->have_frame&&frame==a->frame){*out=a->last_output;out->duplicate=1;return 1;}
 int rebase=!a->initialized||native_reset||(provider&&a->provider_epoch!=raw_epoch)||generation!=a->generation||!crs_settings_same(a->settings,settings)||!crs_same(before_force,a->last_display)||(a->have_frame&&frame<a->frame);
 if(rebase){crs_reset(a,generation,settings,before_force,force_end);a->provider_origin=raw_end-crs_elapsed(dt);a->provider_epoch=raw_epoch;}
 double end=crs_time_add(&a->time,&a->time_compensation,crs_elapsed(dt));if(end-(double)a->ticks*crs_T>64.0*crs_T){a->initialized=0;return 0;}if(!isfinite(end)||end/crs_T>UINT_MAX-1||a->force_count==CRS_FORCE_CAPACITY){a->initialized=0;return 0;}
 a->forces[a->force_count++]=(CRSForceSample){end,force_end};a->time=end;a->frame=frame;a->have_frame=1;
 double eps=64*DBL_EPSILON*fmax(1.0,end);
 while((double)(a->ticks+1)*crs_T<=end+eps){double t=(double)(a->ticks+1)*crs_T;float sampled=crs_force_at(a,t);if(provider&&!provider(context,a->provider_origin+t,&sampled)){a->initialized=0;return 0;}CRSSample n=crs_native30(a,op,sampled);if(!crs_finite(n.stored)||!isfinite(n.returned)){a->initialized=0;return 0;}a->canonical=n.stored;a->last_native=n;++a->ticks;}
 double stamp=(double)a->ticks*crs_T,phase=end-stamp;int partial=phase>eps;CRSSample shown=a->last_native;
 if(partial&&provider&&!provider(context,a->provider_origin+end,&force_end)){a->initialized=0;return 0;}
 if(partial&&!crs_fractional(a,phase,force_end,&shown)){a->initialized=0;return 0;}
 a->last_display=shown.stored;*out=(CRSOutput){shown,a->canonical,end,stamp,a->ticks,rebase,0,partial,0,0,0};a->last_output=*out;
 unsigned drop=0;while(a->force_count-drop>2&&a->forces[drop+1].time<=stamp)++drop;
 if(drop){a->force_count-=drop;memmove(a->forces,a->forces+drop,a->force_count*sizeof a->forces[0]);}
 return 1;
}
static __attribute__((unused)) int crs_render(CRSAdapter*a,uint64_t generation,uint64_t frame,const void*settings,CRSState before_force,
 double dt,float force_end,CRSOps op,CRSOutput*out,int native_reset){
 return crs_render_provider(a,generation,frame,settings,before_force,dt,force_end,op,out,native_reset,NULL,NULL,0,0);
}

/* Global interval provider: endpoint and interval are original integerus. */
typedef int(*CRSGlobalForceProvider)(void*,int64_t,int64_t,float*);
static CRSSample crs_native_interval(const CRSAdapter*a,CRSOps op,float force,int64_t us){
 _Alignas(16)unsigned char record[0x30]={0};memcpy(record,a->settings,0x2c);memcpy(record+0x10,&a->canonical.v,4);memcpy(record+0x14,&a->canonical.x,4);float dt=(float)us*1e-6f;op.force(record,force,dt);float returned=op.step(record,dt);return(CRSSample){{crs_float(record,0x14),crs_float(record,0x10)},returned};
}
static int crs_render_global(CRSAdapter*a,uint64_t generation,uint64_t frame,const void*settings,CRSState before_force,float dt,float force_end,CRSOps op,CRSOutput*out,int native_reset,CRSGlobalForceProvider provider,void*context,uint64_t raw_epoch,const HCSClock*clock){
 int64_t us=0;if(!a||!out||!provider||!clock||!clock->valid||!op.force||!op.step||!crs_supported(settings)||!crs_finite(before_force)||!isfinite(force_end)||!crs_native_us(dt,&us)||us!=clock->dt_us||frame!=clock->frame||us>8*33333){if(a)a->initialized=0;return 0;}
 if(a->initialized&&!native_reset&&a->provider_epoch==raw_epoch&&generation==a->generation&&crs_settings_same(a->settings,settings)&&a->have_frame&&frame==a->frame&&a->global_time_us==clock->time_us){*out=a->last_output;out->duplicate=1;return 1;}
 int rebase=!a->initialized||native_reset||a->provider_epoch!=raw_epoch||generation!=a->generation||!crs_settings_same(a->settings,settings)||!crs_same(before_force,a->last_display)||(a->have_frame&&frame!=a->frame+1u)||a->global_clock!=clock->clock||(a->initialized&&a->global_time_us+us!=clock->time_us);
 if(rebase){crs_reset(a,generation,settings,before_force,force_end);a->provider_epoch=raw_epoch;a->global_clock=clock->clock;a->global_canonical_us=clock->time_us-us;a->global_primed=a->global_canonical_us%33333==0;}
 int64_t interval=33333-a->global_canonical_us%33333;
 while(a->global_canonical_us+interval<=clock->time_us){int64_t endpoint=a->global_canonical_us+interval;float f;if(!provider(context,endpoint,interval,&f)){a->initialized=0;return 0;}CRSSample n=crs_native_interval(a,op,f,interval);if(!crs_finite(n.stored)||!isfinite(n.returned)){a->initialized=0;return 0;}a->canonical=n.stored;a->last_native=n;++a->ticks;a->global_canonical_us=endpoint;a->global_primed=1;interval=33333;}
 int64_t partial_us=clock->time_us-a->global_canonical_us;int partial=partial_us>0;CRSSample shown=a->last_native;
 if(partial){float f;if(!provider(context,clock->time_us,partial_us,&f)){a->initialized=0;return 0;}if(a->global_primed){double phase=(double)crs_T*((double)partial_us/33333.);if(!crs_fractional(a,phase,f,&shown)){a->initialized=0;return 0;}}else shown=crs_native_interval(a,op,f,partial_us);}
 if(!crs_finite(shown.stored)||!isfinite(shown.returned)){a->initialized=0;return 0;}
 a->frame=frame;a->have_frame=1;a->global_time_us=clock->time_us;a->time=(double)crs_T*((double)clock->time_us/33333.);a->last_display=shown.stored;*out=(CRSOutput){shown,a->canonical,a->time,(double)crs_T*((double)a->global_canonical_us/33333.),a->ticks,rebase,0,partial,clock->time_us,a->global_canonical_us,a->global_primed};a->last_output=*out;return 1;
}
#endif
