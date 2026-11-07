#ifndef PRIVATE_LATE_SENSOR_Q_CORE_INCLUDED
#define PRIVATE_LATE_SENSOR_Q_CORE_INCLUDED
/* PRIVATE publication only. No guest pointer dereference, clock/source/raw/
 * history/RNG writes, coefficients, normalization, TTL or interpolated values.
 * NativeQ is an explicit PRIVATE controller sample, not a timestamp assigned
 * to the observed render SDK mean. Each field retains its own phase/origin.
 */
#define LSQ_Q INT64_C(33333)
#define LSQ_TRACKS 16
#define LSQ_KNOTS 32
enum {LSQ_LATE_VELOCITY=0,LSQ_PHYSICAL_ARGUMENT=1,LSQ_PREPUB_RAWLAST=2,
 LSQ_HUD_B0=3,LSQ_HUD_ANGULAR90=4,LSQ_HUD_BODY110=5,LSQ_HUD_AXIS140=6,
 LSQ_ANGULAR_VELOCITY=7,LSQ_BODY_BASIS=8};
#define LSQ_BIT(n) (UINT32_C(1)<<(n))
enum {LSQ_PHASE_PRIVATE_POST_FEEDBACK=1,LSQ_PHASE_OBSERVED_RENDER_SETTER=2,
 LSQ_PHASE_PRIVATE_PRE_PUBLICATION=3,LSQ_PHASE_PRIVATE_NATIVE_SOURCE=4};
enum {LSQ_NORMAL=1,LSQ_TELEPORT=2};
enum {LSQ_OK=1,LSQ_NATIVE_BYPASS=0,LSQ_UNAVAILABLE=-1,LSQ_INVALID=-2,
 LSQ_GAP=-3,LSQ_REWIND=-4,LSQ_DISCONTINUITY=-5,LSQ_AMBIGUOUS=-6,
 LSQ_UNSUPPORTED_LOW_FPS=-7,LSQ_MISSING_FIELDS=-8};
typedef struct {uintptr_t actor,vehicle,owner,raw_state,clock,provider;} LsqBinding;
typedef struct {
 LsqBinding binding;uint64_t epoch,version;
 int64_t q,observed_render_T,observed_render_dt_us;
 uint32_t kind,mask,native_mxcsr,private_add_mxcsr;
 int64_t field_observed_T[9];uint32_t field_phase[9];
 float vector[8][4],body_basis[16],private_mean_Q[4],correction_Q[4],observed_render_mean[4];
} LsqPoint;
typedef struct {LsqBinding binding;uint64_t epoch;unsigned active,count;int status;LsqPoint points[LSQ_KNOTS];} LsqTrack;
typedef struct {uint64_t generation,version;LsqTrack tracks[LSQ_TRACKS];} LsqStore;
typedef struct {uintptr_t actor,vehicle,owner,raw_state,clock,provider;} LsqSelector;
static int lsq_binding_equal(const LsqBinding*a,const LsqBinding*b){return !memcmp(a,b,sizeof*a);}
static int lsq_binding_valid(const LsqBinding*b){return b->actor&&b->vehicle&&b->raw_state&&b->clock&&b->provider;}
static int lsq_finite(const float*p,unsigned n){for(unsigned i=0;i<n;++i)if(!isfinite(p[i]))return 0;return 1;}
static void lsq_clear_track(LsqStore*s,LsqTrack*t){memset(t,0,sizeof*t);t->epoch=++s->generation;}
static void lsq_forget(LsqStore*s,uintptr_t actor,uintptr_t raw_state){
 for(unsigned i=0;i<LSQ_TRACKS;++i){LsqTrack*t=s->tracks+i;if(t->active&&(!actor||t->binding.actor==actor)&&(!raw_state||t->binding.raw_state==raw_state))lsq_clear_track(s,t);}
}
static LsqTrack*lsq_find_actor(LsqStore*s,const LsqBinding*b){
 LsqTrack*empty=NULL;for(unsigned i=0;i<LSQ_TRACKS;++i){LsqTrack*t=s->tracks+i;if(t->active&&t->binding.actor==b->actor)return t;if(!t->active&&!empty)empty=t;}
 return empty;
}
/* Mark an actual parent Replay scope as waiting/unsupported without inventing
 * a Q knot. Source/native30 callers do not call this operation. */
static int lsq_mark_scope(LsqStore*s,const LsqBinding*b,int status){
 if(!lsq_binding_valid(b))return LSQ_INVALID;LsqTrack*t=lsq_find_actor(s,b);if(!t)return LSQ_UNAVAILABLE;
 if(!t->active||!lsq_binding_equal(&t->binding,b)){lsq_clear_track(s,t);t->binding=*b;t->active=1;}
 if(status<0&&t->count){lsq_clear_track(s,t);t->binding=*b;t->active=1;}
 t->status=status;return status;
}
static int lsq_publish(LsqStore*s,const LsqPoint*source,int adapted){
 if(!adapted)return LSQ_NATIVE_BYPASS; /* strict no-write, even malformed input */
 if(!source||!lsq_binding_valid(&source->binding)||source->q<0||source->q%LSQ_Q||
    source->observed_render_T<source->q||source->observed_render_T-source->q>=LSQ_Q||source->observed_render_dt_us<=0)return LSQ_INVALID;
 if(source->observed_render_dt_us>LSQ_Q)return lsq_mark_scope(s,&source->binding,LSQ_UNSUPPORTED_LOW_FPS);
 uint32_t required=LSQ_BIT(LSQ_LATE_VELOCITY)|LSQ_BIT(LSQ_PHYSICAL_ARGUMENT);
 if((source->kind!=LSQ_NORMAL&&source->kind!=LSQ_TELEPORT)||(source->mask&required)!=required||!lsq_finite(source->private_mean_Q,4)||!lsq_finite(source->correction_Q,4)||!lsq_finite(source->observed_render_mean,4))return LSQ_INVALID;
 for(unsigned i=0;i<8;++i)if((source->mask&LSQ_BIT(i))&&!lsq_finite(source->vector[i],4))return LSQ_INVALID;
 if((source->mask&LSQ_BIT(LSQ_BODY_BASIS))&&!lsq_finite(source->body_basis,16))return LSQ_INVALID;
 if(source->field_phase[LSQ_LATE_VELOCITY]!=LSQ_PHASE_PRIVATE_POST_FEEDBACK||source->field_phase[LSQ_PHYSICAL_ARGUMENT]!=LSQ_PHASE_OBSERVED_RENDER_SETTER||source->field_observed_T[LSQ_PHYSICAL_ARGUMENT]!=source->observed_render_T)return LSQ_INVALID;
 LsqTrack*t=lsq_find_actor(s,&source->binding);if(!t)return LSQ_UNAVAILABLE;
 if(!t->active||!lsq_binding_equal(&t->binding,&source->binding)){lsq_clear_track(s,t);t->binding=source->binding;t->active=1;}
 if(t->count){
  const LsqPoint*last=t->points+t->count-1;
  if(source->q<last->q||source->observed_render_T<last->observed_render_T){lsq_clear_track(s,t);return LSQ_REWIND;}
  if(source->q>last->q&&source->q-last->q!=LSQ_Q){lsq_clear_track(s,t);t->binding=source->binding;t->active=1;t->status=LSQ_GAP;return LSQ_GAP;}
 }
 if(source->kind==LSQ_TELEPORT){lsq_clear_track(s,t);t->binding=source->binding;t->active=1;}
 else if(source->kind!=LSQ_NORMAL)return LSQ_INVALID;
 LsqPoint point=*source;point.epoch=t->epoch;point.version=++s->version;
 if(t->count&&t->points[t->count-1].q==point.q)t->points[t->count-1]=point; /* replace, never extra Q tick */
 else{if(t->count==LSQ_KNOTS){memmove(t->points,t->points+1,(LSQ_KNOTS-1)*sizeof* t->points);--t->count;}t->points[t->count++]=point;}
 t->status=source->kind==LSQ_TELEPORT?LSQ_DISCONTINUITY:LSQ_OK;return t->status;
}
static int lsq_selector_matches(const LsqBinding*b,const LsqSelector*x){return(!x->actor||b->actor==x->actor)&&(!x->vehicle||b->vehicle==x->vehicle)&&(!x->owner||b->owner==x->owner)&&(!x->raw_state||b->raw_state==x->raw_state)&&(!x->clock||b->clock==x->clock)&&(!x->provider||b->provider==x->provider);}
static int lsq_select_track(const LsqStore*s,const LsqSelector*x,const LsqTrack**out){
 if(!x||(!x->actor&&!x->vehicle&&!x->owner&&!x->raw_state)||!x->clock)return LSQ_INVALID;
 const LsqTrack*found=NULL;for(unsigned i=0;i<LSQ_TRACKS;++i){const LsqTrack*t=s->tracks+i;if(t->active&&lsq_selector_matches(&t->binding,x)){if(found)return LSQ_AMBIGUOUS;found=t;}}
 if(!found)return LSQ_UNAVAILABLE;*out=found;return found->status;
}
static int lsq_exact(const LsqStore*s,const LsqSelector*x,int64_t q,uint32_t mask,LsqPoint*out){
 const LsqTrack*t=NULL;int status=lsq_select_track(s,x,&t);if(status<0)return status;
 for(unsigned i=0;i<t->count;++i)if(t->points[i].q==q){const LsqPoint*p=t->points+i;if(p->kind!=LSQ_NORMAL)return LSQ_DISCONTINUITY;if((p->mask&mask)!=mask)return LSQ_MISSING_FIELDS;*out=*p;return LSQ_OK;}
 return LSQ_UNAVAILABLE;
}
static int lsq_pair(const LsqStore*s,const LsqSelector*x,int64_t q,uint32_t mask,LsqPoint*previous,LsqPoint*current){
 if(q<LSQ_Q)return LSQ_UNAVAILABLE;int status=lsq_exact(s,x,q-LSQ_Q,mask,previous);if(status!=LSQ_OK)return status;status=lsq_exact(s,x,q,mask,current);if(status!=LSQ_OK)return status;return previous->epoch==current->epoch?LSQ_OK:LSQ_DISCONTINUITY;
}
static int lsq_hold(const LsqStore*s,const LsqSelector*x,int64_t actual_logical_T,uint32_t mask,LsqPoint*out){
 const LsqTrack*t=NULL;int status=lsq_select_track(s,x,&t);if(status<0)return status;if(!t->count)return LSQ_UNAVAILABLE;
 const LsqPoint*p=t->points+t->count-1;
 /* This is canonical-cell coverage, NOT a wall-clock TTL. Native pause may
  * hold indefinitely while logicalT is stopped. Missing nextQ fails closed. */
 if(actual_logical_T<p->q||actual_logical_T-p->q>=LSQ_Q)return LSQ_GAP;
 return lsq_exact(s,x,p->q,mask,out);
}
/* Optional phase-specific fields are merged ONLY into the exact existing Q
 * point. HUDB0 is prePUB; it is never derived from the post-feedback velocity.
 */
static int lsq_attach(LsqStore*s,const LsqSelector*x,int64_t q,uint64_t epoch,unsigned field,const float*value,int64_t observedT,unsigned phase){
 if(field<LSQ_PREPUB_RAWLAST||field>LSQ_BODY_BASIS||!value||observedT<0)return LSQ_INVALID;
 if(phase!=(field<=LSQ_HUD_AXIS140?LSQ_PHASE_PRIVATE_PRE_PUBLICATION:LSQ_PHASE_PRIVATE_NATIVE_SOURCE))return LSQ_INVALID;
 const LsqTrack*selected=NULL;int status=lsq_select_track(s,x,&selected);if(status!=LSQ_OK)return status;LsqTrack*t=s->tracks+(selected-s->tracks);
 for(unsigned i=0;i<t->count;++i){LsqPoint*p=t->points+i;if(p->q==q&&p->epoch==epoch){if(!lsq_finite(value,field==LSQ_BODY_BASIS?16:4))return LSQ_INVALID;if(field==LSQ_BODY_BASIS)memcpy(p->body_basis,value,64);else memcpy(p->vector[field],value,16);p->field_observed_T[field]=observedT;p->field_phase[field]=phase;p->mask|=LSQ_BIT(field);p->version=++s->version;return LSQ_OK;}}
 return LSQ_UNAVAILABLE;
}
#endif
