/* Isolated whole-bank WRITE candidate. HEAD_REFERENCE must be explicitly0.
 * This source policy is HEAD_NATIVE_OBSERVATION ZOH, not reconstruction of an
 * unobserved SDK trajectory: W/M/config/raw primitives are exact actual input
 * publications at T, latest T<=Q; sameT latest wins. Normal Replay linear stays
 * the separate existing private LSQ post-feedback_Q contract. */
#include "head-temporal-group-arena.inc"
#include "head-temporal-group-schedule.inc"
#define HGG_OWNERS 16
#define HGG_OBSERVATIONS 64
typedef struct {
 int64_t selected_T,raw_T,raw_dt;uint32_t domain;
 uint64_t serial;HKTInputs input;unsigned char actor[0x380];
 HprRecord primitive;uint32_t mxcsr;
} HggObservation;
typedef struct {
 unsigned used,count,known_replay,main_bl;uintptr_t actor,vehicle,clock;
 uint64_t epoch,source_token;LsqBinding binding;uint64_t lsq_epoch;
 HggObservation history[HGG_OBSERVATIONS];HggState state;
 unsigned pause_mode;uint64_t pause_metadata_token;int64_t pause_raw_T,pause_origin_Q;
 float pause_linear[4];HKTInputs last_native;int64_t last_raw_T,last_selected_T;
 uint32_t last_frame;int last_status;
 uintptr_t source_reader;uint64_t source_birth[5];unsigned source_birth_valid;
} HggOwner;
static HgrNativeArena hgg_arena;
static HggOwner hgg_owners[HGG_OWNERS];static SRWLOCK hgg_lock=SRWLOCK_INIT;
static uint64_t hgg_serial;static unsigned hgg_ready;
static volatile LONG64 hgg_counts[16];
static void hgg_trace(const HtpRequest*r,const HCSClock*c,const HggOwner*s,const HggResult*out){
 const HKTRecord*h=r->hkt;float d[24]={(float)out->status,(float)out->ready,(float)(s?s->known_replay:0),(float)(s?s->state.ticks:0),(float)c->domain,(float)out->native30_bypass};
 int64_t observed=0;if(s&&out->ready)for(unsigned i=0;i<s->count;++i)if(s->history[i].selected_T<=out->Q)observed=s->history[i].selected_T;
 memcpy(d+6,&r->logical_T,8);memcpy(d+8,&c->time_us,8);memcpy(d+10,&out->Q,8);memcpy(d+12,&observed,8);memcpy(d+14,&r->key.epoch,8);memcpy(d+16,&out->binding_token,8);memcpy(d+18,&out->version,8);memcpy(d+20,&r->head_serial,8);d[22]=(float)(s?s->state.duplicates:0);d[23]=(float)out->initial_partial;
 camera_accuracy_log(47,(void*)(uintptr_t)h->actor,(uintptr_t)h->vehicle,h->frame,h->input.dt[0],d,24);
}
static uint64_t hgg_hash(uint64_t h,const void*p,size_t n){const unsigned char*b=p;while(n--)h=(h^*b++)*UINT64_C(1099511628211);return h;}
static uint64_t hgg_token(const HggOwner*s){
 uint64_t h=UINT64_C(14695981039346656037);h=hgg_hash(h,&s->actor,sizeof s->actor);h=hgg_hash(h,&s->vehicle,sizeof s->vehicle);h=hgg_hash(h,&s->clock,sizeof s->clock);h=hgg_hash(h,&s->epoch,sizeof s->epoch);h=hgg_hash(h,&s->known_replay,sizeof s->known_replay);
 h=hgg_hash(h,&s->main_bl,sizeof s->main_bl);if(s->known_replay){h=hgg_hash(h,&s->binding,sizeof s->binding);h=hgg_hash(h,&s->lsq_epoch,sizeof s->lsq_epoch);}return h?h:1;
}
static void hgg_runtime_forget(uintptr_t actor){
 AcquireSRWLockExclusive(&hgg_lock);for(unsigned i=0;i<HGG_OWNERS;++i)if(hgg_owners[i].used&&(!actor||hgg_owners[i].actor==actor))memset(hgg_owners+i,0,sizeof hgg_owners[i]);ReleaseSRWLockExclusive(&hgg_lock);
}
static int hgg_runtime_prepare(void){
 if(hgg_ready)return 1;const char*group=camera_fix_getenv("BB_CAMERA_HEAD_GROUP"),*write=camera_fix_getenv("BB_CAMERA_TEMPORAL_GROUP_WRITE"),*old=camera_fix_getenv("BB_CAMERA_HEAD_REFERENCE");
 if(!group||strcmp(group,"1")||!write||strcmp(write,"1")||!old||strcmp(old,"0"))return 0;
 uint32_t prefactor=0;if(!camera_pose_read((uintptr_t)image+0x12a769c,&prefactor,4)||prefactor!=UINT32_C(0x3f400000))return 0;
 if(!hgr_native_create(&hgg_arena,image,runtime_image_size,camera_pose_read))return 0;
 hgg_ready=1;puts("HeadTemporalGroupPause: ORIGINAL bank; HEAD observation ZOH + private Replay LSQ linearQ; selected68 frozen-native samebinding continuation; isolated HEAD_REFERENCE0");return 1;
}
static HggOwner*hgg_owner(const HtpRequest*r,const HCSClock*c,const HKTRecord*h,int known,const LsqPoint*held){
 HggOwner*slot=NULL;for(unsigned i=0;i<HGG_OWNERS;++i)if(hgg_owners[i].used&&hgg_owners[i].actor==r->key.owner){slot=hgg_owners+i;break;}
 if(!slot)for(unsigned i=0;i<HGG_OWNERS;++i)if(!hgg_owners[i].used){slot=hgg_owners+i;break;}
 if(!slot)return NULL;
 int same=slot->used&&!slot->pause_mode&&slot->vehicle==h->vehicle&&slot->clock==c->clock&&slot->epoch==r->key.epoch&&slot->known_replay==(unsigned)known&&slot->main_bl==r->key.main_bl;
 if(same&&known)same=held&&slot->lsq_epoch==held->epoch&&lsq_binding_equal(&slot->binding,&held->binding);
 if(!same){memset(slot,0,sizeof*slot);slot->used=1;slot->actor=r->key.owner;slot->vehicle=h->vehicle;slot->clock=c->clock;slot->epoch=r->key.epoch;slot->known_replay=known;slot->main_bl=r->key.main_bl;if(known&&held){slot->binding=held->binding;slot->lsq_epoch=held->epoch;}slot->source_token=hgg_token(slot);}
 return slot;
}
static int hgg_observe(HggOwner*s,const HCSClock*c,const HKTRecord*h,const HprRecord*p){
 HggObservation x;memset(&x,0,sizeof x);x.selected_T=c->time_us;x.raw_T=p->helper_logical_T;x.raw_dt=p->helper_logical_dt;x.domain=c->domain;x.serial=++hgg_serial;x.input=h->input;x.mxcsr=h->mxcsr_before;memcpy(x.actor,h->before,sizeof x.actor);x.primitive=*p;
 if(s->count){HggObservation*last=s->history+s->count-1;if(x.selected_T<last->selected_T||(x.domain!=last->domain&&!(s->pause_mode==2&&last->domain==0&&x.domain==1)))return 0;if(x.selected_T==last->selected_T){*last=x;return 1;}}
 if(s->count==HGG_OBSERVATIONS){memmove(s->history,s->history+1,(HGG_OBSERVATIONS-1)*sizeof x);--s->count;}s->history[s->count++]=x;return 1;
}
static void hgg_prefactor(float out[4],const float in[4],unsigned main_bl,uint32_t csr){
 if(!main_bl){memcpy(out,in,16);return;}uint32_t old=_mm_getcsr();_mm_setcsr(csr);const float k=.75f;
 __asm__ volatile("vmovups (%1),%%xmm0\n vbroadcastss (%2),%%xmm1\n vmulps %%xmm1,%%xmm0,%%xmm0\n vmovups %%xmm0,(%0)"::"r"(out),"r"(in),"r"(&k):"xmm0","xmm1","memory");_mm_setcsr(old);
}
static int hgg_sample_runtime(void*context,int64_t Q,HggSample*out){
 HggOwner*s=context;const HggObservation*o=NULL;for(unsigned i=0;i<s->count;++i)if(s->history[i].selected_T<=Q)o=s->history+i;else break;if(!o||(o->domain&&s->pause_mode!=2))return 0;
 memset(out,0,sizeof*out);out->Q=out->linear_Q=Q;out->head_publication_T=out->primitive_publication_T=o->selected_T;out->epoch=s->epoch;out->binding_token=s->source_token;out->provenance=HGG_SOURCE_MASK;out->mxcsr=o->mxcsr;out->input=o->input;memcpy(out->actor,o->actor,sizeof out->actor);out->primitive=o->primitive;
 uint64_t version=hgg_hash(UINT64_C(14695981039346656037),&o->serial,sizeof o->serial);
 if(s->pause_mode==2){memcpy(out->input.current_linear,s->pause_linear,16);version=hgg_hash(version,s->pause_linear,16);}
 else if(s->known_replay){LsqSelector selector={0};selector.vehicle=s->vehicle;selector.clock=s->clock;LsqPoint point;
  if(lsq_hud_tuple_Q(&selector,Q,LSQ_BIT(LSQ_LATE_VELOCITY),&point)!=LSQ_OK||point.epoch!=s->lsq_epoch||!lsq_binding_equal(&point.binding,&s->binding))return 0;
  hgg_prefactor(out->input.current_linear,point.vector[LSQ_LATE_VELOCITY],out->primitive.parent_mode_bl,out->mxcsr);
  /* Field-only LSQ metadata changes cannot apply a duplicate recurrence. */
  version=hgg_hash(version,out->input.current_linear,16);
 }
 out->version=version?version:1;return 1;
}
static int hgg_step_runtime(void*context,unsigned char actor[0x380],const HKTInputs*i,const HprRecord*p,uint32_t csr,HgrNativeOutput*out){(void)context;return hgr_native_step(&hgg_arena,actor,i,p,csr,out);}
#include "head-temporal-group-frozen-pause.inc"
static int hgg_runtime_select(const HtpRequest*r,HggResult*out){
 memset(out,0,sizeof*out);if(!hgg_ready||!r||!r->hkt||!r->primitives||!r->key.epoch){out->status=HGG_BAD_CLOCK;return out->status;}
 const HKTRecord*h=r->hkt;const HprRecord*p=r->primitives;HCSClock clock=hps_head_clock(h);
 int64_t us=0;if(!clock.valid||clock.frame!=h->frame||clock.clock!=p->helper_clock||!crs_native_us(h->input.dt[0],&us)||us!=clock.dt_us){hgg_runtime_forget(r->key.owner);out->status=HGG_BAD_CLOCK;InterlockedIncrement64(hgg_counts+2);return out->status;}
 /* The selected68 reader is continuous from a VERIFIED lastlogical anchor.
  * Frozen raw58 NEVER labels paused user-input observations. */
 out->domain=clock.domain;if(clock.domain)return hgg_pause_select(r,&clock,out);
 if(clock.time_us!=p->helper_logical_T||clock.dt_us!=p->helper_logical_dt){out->status=HGG_BAD_CLOCK;return out->status;}
 LsqSelector selector={0};selector.vehicle=h->vehicle;selector.clock=clock.clock;int known=0;LsqPoint held;memset(&held,0,sizeof held);int status=lsq_scope_hold(&selector,clock.time_us,&known,&held);
 if(known&&!held.epoch){const LsqTrack*t=NULL;AcquireSRWLockShared(&lsq_lock);int selected=lsq_select_track(&lsq_store,&selector,&t);if(selected!=LSQ_AMBIGUOUS&&t&&t->active){held.binding=t->binding;held.epoch=t->epoch;}ReleaseSRWLockShared(&lsq_lock);}
 /* A known Replay tombstone is not permission to consume physical rawR10. */
 if(known&&(!held.epoch||!lsq_binding_valid(&held.binding))){out->status=HGG_MISSING_SOURCE;InterlockedIncrement64(hgg_counts+4);return out->status;}(void)status;
 AcquireSRWLockExclusive(&hgg_lock);HggOwner*s=hgg_owner(r,&clock,h,known,&held);if(!s||!hgg_observe(s,&clock,h,p)){ReleaseSRWLockExclusive(&hgg_lock);out->status=HGG_BAD_SOURCE;return out->status;}
 HggRequest request={h->actor,h->vehicle,clock.clock,r->key.epoch,clock.time_us,clock.dt_us,clock.domain,r->key.main_bl,h,p};
 int result=hgg_update(&s->state,&request,hgg_sample_runtime,s,hgg_step_runtime,NULL,out);s->last_native=h->input;s->last_raw_T=p->helper_logical_T;s->last_selected_T=clock.time_us;s->last_frame=h->frame;s->last_status=result;s->source_birth_valid=s->known_replay&&hgg_pause_births(&s->binding,&s->source_reader,s->source_birth);hgg_trace(r,&clock,s,out);ReleaseSRWLockExclusive(&hgg_lock);unsigned counter=result==HGG_SELECTED?0:result==HGG_NATIVE30?1:result==HGG_PREFIX_WAIT?5:6;InterlockedIncrement64(hgg_counts+counter);return result;
}
