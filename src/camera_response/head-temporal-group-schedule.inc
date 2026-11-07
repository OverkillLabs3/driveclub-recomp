/* Ordered original-bank scheduler. Input sampling is an explicit provider
 * contract: this layer never interpolates pose, velocities, axes or targets.
 * Native30 uses its already executed original transaction unchanged. */
#define HGG_QUANTUM INT64_C(33333)
#define HGG_MAX_STEPS 8u
typedef struct {
 int64_t Q,linear_Q,head_publication_T,primitive_publication_T;
 uint64_t epoch,binding_token,version;
 uint32_t provenance,mxcsr;
 HKTInputs input;
 unsigned char actor[0x380];
 HprRecord primitive;
} HggSample;
enum {
 HGG_SOURCE_LINEAR_Q=1, HGG_SOURCE_PHYSICAL_ZOH=2,
 HGG_SOURCE_PRIMITIVE_ZOH=4, HGG_SOURCE_SAME_BINDING=8,
 HGG_SOURCE_MASK=15,
 HGG_NO_STATE=0,HGG_NATIVE30=1,HGG_SELECTED=2,HGG_PREFIX_WAIT=3,
 HGG_MISSING_SOURCE=-1,HGG_BAD_CLOCK=-2,HGG_BAD_SOURCE=-3,
 HGG_MATH_FAILURE=-4,HGG_TOO_MANY_Q=-5,HGG_UNSUPPORTED_DOMAIN=-6
};
typedef int(*HggSampleAtQ)(void*,int64_t,HggSample*);
typedef int(*HggStep)(void*,unsigned char[0x380],const HKTInputs*,const HprRecord*,uint32_t,HgrNativeOutput*);
typedef struct {
 uintptr_t owner,vehicle,clock;uint64_t epoch;
 int64_t T,dt;uint32_t domain,main_bl;
 const HKTRecord*head;const HprRecord*primitive;
} HggRequest;
typedef struct {
 unsigned valid,adapted,ready,checkpoint_valid;
 uintptr_t owner,vehicle,clock;uint64_t epoch,binding_token;
 int64_t observed_T,seed_T,Q;uint64_t version,ticks,holds,duplicates,resets;
 uint32_t main_bl,initial_partial,domain;int status;
 unsigned char actor[0x380];
 HgrNativeOutput output;
 /* An authoritative replacement of the SAME Q is recomputed from its saved
  * pre-Q state, never applied as a second native recurrence. */
 int64_t checkpoint_Q;uint32_t checkpoint_duration;
 unsigned char checkpoint_actor[0x380];
} HggState;
typedef struct {
 int status;unsigned ready,native30_bypass,initial_partial,domain;
 int64_t Q,selected_T;uint64_t epoch,binding_token,version;
 HgrNativeOutput output;
} HggResult;
static void hgg_forget(HggState*s){uint64_t resets=s->resets+1;memset(s,0,sizeof*s);s->resets=resets;}
static void hgg_copy_bank(unsigned char*to,const unsigned char*from){
 memcpy(to+0x130,from+0x130,32); /* PRIVATE input history only. */
 memcpy(to+0x1a0,from+0x1a0,64);memcpy(to+0x1e0,from+0x1e0,8);
 memcpy(to+0x1f0,from+0x1f0,48);memcpy(to+0x220,from+0x220,8);
 memcpy(to+0x2b0,from+0x2b0,4);memcpy(to+0x350,from+0x350,12);
}
static int hgg_finite(const float*x,unsigned n){for(unsigned i=0;i<n;++i)if(!isfinite(x[i]))return 0;return 1;}
static int hgg_request_valid(const HggRequest*r){
 if(!r||!r->head||!r->primitive||!r->owner||!r->vehicle||!r->clock||!r->epoch||r->T<0||r->dt<0||r->T<r->dt||r->main_bl>1)return 0;
 const HKTRecord*h=r->head;const HprRecord*p=r->primitive;
 int time=r->domain==0?(p->helper_logical_T==r->T&&p->helper_logical_dt==r->dt):(r->domain==1&&p->helper_logical_dt==0&&p->helper_logical_T<=r->T);
 return time&&h->actor==r->owner&&h->vehicle==r->vehicle&&p->owner==r->owner&&p->main_frame==h->frame_address&&p->helper_clock==r->clock&&p->parent_mode_bl==r->main_bl&&h->thread==p->helper_thread&&h->frame==p->helper_frame_number&&(h->flags&3)==3;
}
static int hgg_sample_valid(const HggSample*x,const HggRequest*r,int64_t Q,uint64_t binding){
 if(x->Q!=Q||x->linear_Q!=Q||x->epoch!=r->epoch||!x->binding_token||(binding&&x->binding_token!=binding)||!x->version||(x->provenance&HGG_SOURCE_MASK)!=HGG_SOURCE_MASK||x->head_publication_T>Q||x->primitive_publication_T>Q||x->primitive.parent_mode_bl!=r->main_bl)return 0;
 unsigned route=x->primitive.flags&(256|512);if(route!=256&&route!=512)return 0;
 if(route==256&&!(x->primitive.flags&16))return 0;
 if(x->actor[0x120]&&!(x->primitive.flags&32))return 0;
 return hgg_finite(x->input.current_linear,4)&&hgg_finite(x->input.current_angular,4)&&hgg_finite(x->input.matrix,16);
}
static void hgg_make_input(HKTInputs*out,const HggSample*x,const unsigned char*old,uint32_t duration){
 uint32_t csr=_mm_getcsr();_mm_setcsr(x->mxcsr);
 *out=x->input;out->dt[0]=(float)duration*1e-6f;volatile float inv=1.f/out->dt[0];
 for(unsigned j=0;j<4;++j){out->broadcast_dt[j]=out->dt[0];out->inverse[j]=inv;
  float previous;memcpy(&previous,old+0x130+j*4,4);volatile float d=out->current_linear[j]-previous;out->delta_linear[j]=d;
  memcpy(&previous,old+0x140+j*4,4);volatile float a=out->current_angular[j]-previous;out->delta_angular[j]=a;
 }
 _mm_setcsr(csr);
}
static int hgg_run_one(HggState*s,const HggRequest*r,const HggSample*x,int64_t Q,uint32_t duration,HggStep step,void*context){
 _Alignas(16)unsigned char actor[0x380];HKTInputs input;HgrNativeOutput out;
 memcpy(actor,x->actor,sizeof actor);hgg_copy_bank(actor,s->actor);hgg_make_input(&input,x,actor,duration);
 if(!step(context,actor,&input,&x->primitive,x->mxcsr,&out)||out.status||!hgg_finite((const float*)(actor+0x1a0),16)||!hgg_finite((const float*)(actor+0x1e0),2)||!hgg_finite((const float*)(actor+0x1f0),14)||!hgg_finite((const float*)(actor+0x2b0),1)||!hgg_finite((const float*)(actor+0x350),3))return 0;
 memcpy(s->actor,actor,sizeof actor);s->output=out;s->Q=Q;s->binding_token=x->binding_token;s->version=x->version;s->ready=1;s->initial_partial=duration!=HGG_QUANTUM;(void)r;return 1;
}
static void hgg_result(const HggState*s,HggResult*out,int status){
 memset(out,0,sizeof*out);out->status=status;if(s&&s->ready){out->ready=1;out->Q=s->Q;out->selected_T=s->observed_T;out->epoch=s->epoch;out->binding_token=s->binding_token;out->version=s->version;out->initial_partial=s->initial_partial;out->domain=s->domain;out->output=s->output;}
}
static int hgg_update_domain(HggState*s,const HggRequest*r,HggSampleAtQ sample,void*source,HggStep step,void*backend,HggResult*out,unsigned admitted_frozen){
 memset(out,0,sizeof*out);if(!s||!hgg_request_valid(r)||!sample||!step){out->status=HGG_BAD_CLOCK;return out->status;}
 if(r->domain&&(!admitted_frozen||r->domain!=1||!s->valid||!s->adapted||!s->ready||r->dt<=0||s->observed_T!=r->T-r->dt)){out->status=HGG_UNSUPPORTED_DOMAIN;return out->status;}
 int continuity=s->valid&&s->owner==r->owner&&s->vehicle==r->vehicle&&s->clock==r->clock&&s->epoch==r->epoch&&s->main_bl==r->main_bl&&((r->T==s->observed_T)||(r->dt>0&&s->observed_T==r->T-r->dt));
 if(!continuity){hgg_forget(s);s->valid=1;s->owner=r->owner;s->vehicle=r->vehicle;s->clock=r->clock;s->epoch=r->epoch;s->main_bl=r->main_bl;s->observed_T=r->T;}
 s->observed_T=r->T;
 /* This bypass persists for an entirely nativeQ epoch, including duplicate
  * zero-duration callbacks. A later Q stall never silently disables adaptation. */
 if(!s->adapted&&(r->dt==HGG_QUANTUM||r->dt==0)){out->native30_bypass=1;out->status=HGG_NATIVE30;return out->status;}
 if(!s->adapted||(!s->ready&&s->status==HGG_MISSING_SOURCE)){s->adapted=1;s->seed_T=s->Q=r->T-r->dt;memcpy(s->actor,r->head->before,sizeof s->actor);}
 s->observed_T=r->T;
 if(r->dt>HGG_MAX_STEPS*HGG_QUANTUM){out->status=HGG_TOO_MANY_Q;return out->status;}
 HggState next=*s;unsigned count=0;
 /* Latest same-Q producer replacement uses the immutable pre-Q checkpoint. */
 if(next.checkpoint_valid&&next.ready){HggSample x;memset(&x,0,sizeof x);
  if(!sample(source,next.Q,&x)){out->status=HGG_MISSING_SOURCE;return out->status;}
  if(!hgg_sample_valid(&x,r,next.Q,next.binding_token)){out->status=HGG_BAD_SOURCE;return out->status;}
  if(x.version!=next.version){
   memcpy(next.actor,next.checkpoint_actor,sizeof next.actor);
   if(!hgg_run_one(&next,r,&x,next.checkpoint_Q,next.checkpoint_duration,step,backend)){out->status=HGG_MATH_FAILURE;return out->status;}++next.duplicates;
  }
 }
 while((next.Q/HGG_QUANTUM+1)*HGG_QUANTUM<=r->T){
  int64_t Q=(next.Q/HGG_QUANTUM+1)*HGG_QUANTUM;uint32_t duration=(uint32_t)(Q-next.Q);
  if(++count>HGG_MAX_STEPS){out->status=HGG_TOO_MANY_Q;return out->status;}
  HggSample x;memset(&x,0,sizeof x);
  if(!sample(source,Q,&x)){s->status=out->status=HGG_MISSING_SOURCE;return out->status;}
  if(!hgg_sample_valid(&x,r,Q,next.binding_token)){out->status=HGG_BAD_SOURCE;return out->status;}
  memcpy(next.checkpoint_actor,next.actor,sizeof next.actor);next.checkpoint_Q=Q;next.checkpoint_duration=duration;next.checkpoint_valid=1;
  if(!hgg_run_one(&next,r,&x,Q,duration,step,backend)){out->status=HGG_MATH_FAILURE;return out->status;}++next.ticks;
 }
 next.domain=r->domain;next.status=next.ready?HGG_SELECTED:HGG_PREFIX_WAIT;if(!count)++next.holds;*s=next;hgg_result(s,out,next.status);return out->status;
}
static int hgg_update(HggState*s,const HggRequest*r,HggSampleAtQ sample,void*source,HggStep step,void*backend,HggResult*out){return hgg_update_domain(s,r,sample,source,step,backend,out,0);}
static int hgg_update_frozen(HggState*s,const HggRequest*r,HggSampleAtQ sample,void*source,HggStep step,void*backend,HggResult*out){return hgg_update_domain(s,r,sample,source,step,backend,out,1);}
