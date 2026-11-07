#ifndef CAMERA_Q_TARGET_DERIVATIVE_INCLUDED
#define CAMERA_Q_TARGET_DERIVATIVE_INCLUDED
/* Camera-target-only input. Original world velocities, authored33333us
 * difference, shared logical Q boundaries. No physical state/strength writes. */
typedef struct {int64_t t;float v[6];} QTargetSample;
typedef struct {uintptr_t owner,binding,clock;unsigned frame,kind,count,adapted;QTargetSample samples[128];} QTargetHistory;
static int qtarget_sample(const QTargetHistory*h,int64_t t,float out[6]){
 if(!h->count||t<h->samples[0].t||t>h->samples[h->count-1].t)return 0;
 for(unsigned i=0;i<h->count;++i){const QTargetSample*b=h->samples+i;
  if(t==b->t){memcpy(out,b->v,24);return 1;}
  if(b->t>t){if(!i)return 0;const QTargetSample*a=b-1;double u=(double)(t-a->t)/(double)(b->t-a->t);for(unsigned j=0;j<6;++j)out[j]=(float)((double)a->v[j]+((double)b->v[j]-a->v[j])*u);return 1;}
 }return 0;
}
static int qtarget_advance(QTargetHistory*h,uintptr_t owner,uintptr_t binding,uintptr_t clock,unsigned frame,unsigned kind,int reset,int64_t time,int64_t us,const float velocity[6],float derivative[6]){
 if(!owner||!binding||!clock||kind!=1||time<us||us<=0||us>8*33333||!hcl_finite(velocity,6)){memset(h,0,sizeof *h);return -1;}
 if(reset||h->owner!=owner||h->binding!=binding||h->clock!=clock||h->kind!=kind||(h->count&&((frame!=h->frame+1u&&frame!=h->frame)||time-h->samples[h->count-1].t!=(frame==h->frame?0:us)))){memset(h,0,sizeof *h);h->owner=owner;h->binding=binding;h->clock=clock;h->kind=kind;}
 int duplicate=h->count&&frame==h->frame&&time==h->samples[h->count-1].t;
 h->frame=frame;
 if(!duplicate&&h->count==128){memmove(h->samples,h->samples+1,127*sizeof h->samples[0]);--h->count;}
 QTargetSample*s=duplicate?h->samples+h->count-1:h->samples+h->count++;s->t=time;memcpy(s->v,velocity,24);
 h->adapted|=us!=33333;
 if(!h->adapted)return 0; /* Strict all-native30 epoch: no stores. A Q-sized
                            * VRR stall keeps its established shared-Q phase. */
 int64_t tick=(time/33333)*33333;float now[6],before[6];
 if(!qtarget_sample(h,tick,now)||!qtarget_sample(h,tick-33333,before))return 0;
 volatile float inverse=1.f/(33333.f*1e-6f);
 for(unsigned j=0;j<6;++j){volatile float delta=now[j]-before[j];derivative[j]=delta*inverse;if(!isfinite(derivative[j]))return -1;}
 return 1;
}
#endif
