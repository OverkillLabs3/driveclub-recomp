#ifndef LF_SHARED_Q_CORE_INCLUDED
#define LF_SHARED_Q_CORE_INCLUDED
#include <stdint.h>
#include <string.h>
#include <math.h>
/* Private observation model: world velocity/scalars linear between actual
 * native reads, body basis rigid shortest arc via the supplied basis function.
 * Authoritative knots are global integer33333us boundaries, not render ends.
 * This models sampled inputs; it does not invent unseen collisions. */
typedef struct {int64_t t;float velocity[3],basis[16],speed,camera_source,gain;int has_camera_source;} LFQSample;
typedef struct {uintptr_t camera,target,binding,clock;uint32_t frame;unsigned count,adapted;LFQSample samples[128];} LFQHistory;
typedef struct {int valid,native_q,due,has_camera_source;int64_t tick;float acceleration[3],basis[16],speed,camera_source,gain;} LFQInput;
typedef void(*LFQBasis)(const float*,const float*,double,float*);
static int lfq_finite_sample(const LFQSample*s){for(unsigned i=0;i<3;++i)if(!isfinite(s->velocity[i]))return 0;for(unsigned i=0;i<16;++i)if(!isfinite(s->basis[i]))return 0;return isfinite(s->speed)&&isfinite(s->camera_source)&&isfinite(s->gain);}
static int lfq_sample(const LFQHistory*h,int64_t t,LFQSample*out,LFQBasis basis){
 if(!h->count||t<h->samples[0].t||t>h->samples[h->count-1].t)return 0;
 for(unsigned i=0;i<h->count;++i){const LFQSample*b=h->samples+i;if(t==b->t){*out=*b;return 1;}if(b->t>t){if(!i)return 0;const LFQSample*a=b-1;double u=(double)(t-a->t)/(double)(b->t-a->t);*out=*b;out->t=t;
 for(unsigned j=0;j<3;++j)out->velocity[j]=(float)((double)a->velocity[j]+((double)b->velocity[j]-a->velocity[j])*u);
 out->speed=(float)((double)a->speed+((double)b->speed-a->speed)*u);out->camera_source=(float)((double)a->camera_source+((double)b->camera_source-a->camera_source)*u);out->gain=(float)((double)a->gain+((double)b->gain-a->gain)*u);basis(a->basis,b->basis,u,out->basis);return lfq_finite_sample(out);}}
 return 0;
}
static LFQInput lfq_advance(LFQHistory*h,uintptr_t camera,uintptr_t target,uintptr_t binding,uintptr_t clock,uint32_t frame,int64_t us,const LFQSample*raw,LFQBasis basis){
 LFQInput out={0};out.native_q=us==33333;
 if(!camera||!target||!binding||!clock||us<=0||us>33333||raw->t<us||!lfq_finite_sample(raw)){memset(h,0,sizeof *h);return out;}
 if(h->camera!=camera||h->target!=target||h->binding!=binding||h->clock!=clock||(h->count&&((frame!=h->frame+1u&&frame!=h->frame)||raw->t-h->samples[h->count-1].t!=(frame==h->frame?0:us)))){memset(h,0,sizeof *h);h->camera=camera;h->target=target;h->binding=binding;h->clock=clock;}
 if(h->count&&frame==h->frame&&raw->t==h->samples[h->count-1].t){h->samples[h->count-1]=*raw;}else{if(h->count==128){memmove(h->samples,h->samples+1,127*sizeof h->samples[0]);--h->count;}h->samples[h->count++]=*raw;}h->frame=frame;h->adapted|=us!=33333;out.native_q=us==33333&&!h->adapted;
 out.tick=(raw->t/33333)*33333;out.due=raw->t/33333!=(raw->t-us)/33333;
 if(out.native_q||!out.due)return out; /* Strict native30 no-override. */
 LFQSample now,before;if(!lfq_sample(h,out.tick,&now,basis)||!lfq_sample(h,out.tick-33333,&before,basis))return out;
 volatile float inverse=1.f/(33333.f*1e-6f);for(unsigned i=0;i<3;++i){volatile float delta=now.velocity[i]-before.velocity[i];out.acceleration[i]=delta*inverse;if(!isfinite(out.acceleration[i]))return out;}
 memcpy(out.basis,now.basis,64);out.speed=now.speed;out.camera_source=now.camera_source;out.gain=now.gain;out.has_camera_source=now.has_camera_source;out.valid=1;return out;
}
/* Preserve original float32 add/multiply order from CD5554..CD55DC. */
static void lfq_collision(const LFQInput*q,float args[3]){
 float dots[3];for(unsigned i=0;i<3;++i){volatile float x=q->acceleration[0]*q->basis[4*i],y=q->acceleration[1]*q->basis[4*i+1],z=q->acceleration[2]*q->basis[4*i+2],xy=x+y;dots[i]=z+xy;}
 volatile float lateral=dots[0]*-.01f,forward=dots[2]*.01f,vertical=dots[1]*-.01f;args[0]=lateral;args[1]=forward+vertical;args[2]=lateral;
}
/* Original camera ratio CD530E..CD5345, including ordered <=0 and clamp1. */
static float lfq_speed_ratio(const LFQInput*q){if(!q->has_camera_source)return 0;volatile float denom=q->camera_source*.44703999161720276f;float ratio=q->speed/denom;if(!(ratio>0))return 0;return ratio<1?ratio:1;}
#endif
