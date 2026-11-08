/* PRIVATE/CPU bounded original-packet cache. No reader/query/SDK/source write.
 * Exact observed nativeQ is supported on v1 capture. Off-grid mapping is
 * unavailable unless ALL actual native clock/provider/reader guards are read.
 * Mode fields below must be coherently captured, never inferred from FPS or
 * fitted from two poses. Original replay-provider update code proof is separate.
 */
#define NQPC_CAPACITY 64
typedef struct{
 unsigned complete,stable_before_after;uint64_t lifecycle_epoch;
 uintptr_t clock968,clock970;int64_t T968,T970,dt968,dt970;uint32_t frame968,frame970;
 int64_t A8,B0,B8,C0,reader70,reader68;uint32_t C8_bits;unsigned char CC,CD,CE,actorE4;uint64_t record_marker_E8;
 uintptr_t provider_vptr,reader_vptr,reader_timestamp_stream_vptr;
} NqpcClock;
typedef struct{RpkRecord packet;NqpcClock clock;} NqpcSample;
typedef struct{NqpcSample samples[NQPC_CAPACITY];unsigned first,count;uint64_t generation,resets;} NqpcCache;
typedef int(*NqpcEvaluate)(void*,const RpkRecord*,int64_t,unsigned char[64],uintptr_t*);
typedef struct{int mode;int64_t requested_nativeQ,mapped_query,interval_T0,interval_T1,interval_K0,interval_K1;uint64_t source_sequence,cache_generation;} NqpcResult;
enum{NQPC_UNAVAILABLE=0,NQPC_EXACT_OBSERVED=1,NQPC_PROVEN_FORWARD_INTERVAL=2};
static void nqpc_reset(NqpcCache*c){uint64_t generation=c->generation+1,resets=c->resets+1;memset(c,0,sizeof*c);c->generation=generation;c->resets=resets;}
static NqpcSample*nqpc_at(NqpcCache*c,unsigned n){return c->samples+(c->first+n)%NQPC_CAPACITY;}
static int nqpc_packet_valid(const RpkRecord*r){return(r->flags&0x3fff)==0x3fff&&r->actor&&r->provider&&r->reader&&r->time_before==r->time_after&&r->frame_before==r->frame_after&&r->t1>r->t0&&r->query>=r->t0&&r->query<=r->t1;}
/* E8 is the selected record's timestamp/notification marker, NOT binding. */
static int nqpc_same_binding(const NqpcSample*a,const NqpcSample*b){return a->packet.actor==b->packet.actor&&a->packet.provider==b->packet.provider&&a->packet.reader==b->packet.reader&&a->clock.lifecycle_epoch==b->clock.lifecycle_epoch&&a->packet.reserved==b->packet.reserved;}
static int nqpc_sub(int64_t a,int64_t b,int64_t*out){if((b>0&&a<INT64_MIN+b)||(b<0&&a>INT64_MAX+b))return 0;*out=a-b;return 1;}
static int nqpc_add(int64_t a,int64_t b,int64_t*out){if((b>0&&a>INT64_MAX-b)||(b<0&&a<INT64_MIN-b))return 0;*out=a+b;return 1;}
/* Provider update consumes the previous logical duration before current clock
 * advance. K is its NATIVE-30-END LABEL, not a shifted actual wall/native time.
 * Keep actual T/dt untouched; only admitted original role may use K off-grid.
 * Native dt33333 makes K==T, preserving each actual native30 query exactly. */
#define NQPC_NORMALIZED_TAG UINT64_C(0x50514e3100000001)
static int nqpc_normalized(const RpkRecord*r){return r->reserved==NQPC_NORMALIZED_TAG;}
static int nqpc_key(const RpkRecord*r,int64_t*K){
 if(nqpc_normalized(r)){if(r->time_before<0||r->clock_dt_before<0||r->clock_dt_before>33333)return 0;*K=r->time_before;return 1;}
 int64_t S=0;return r->clock_dt_before>0&&r->clock_dt_before<INT64_C(16777216)&&nqpc_sub(r->time_before,r->clock_dt_before,&S)&&nqpc_add(S,33333,K);
}
static int nqpc_push(NqpcCache*c,const RpkRecord*r,const NqpcClock*clock){
 int64_t K=0;if(!nqpc_packet_valid(r)||!nqpc_key(r,&K)){nqpc_reset(c);return 0;}NqpcSample s;memset(&s,0,sizeof s);s.packet=*r;if(clock)s.clock=*clock;
 if(c->count){NqpcSample*p=nqpc_at(c,c->count-1);int64_t dt=0,dq=0,previous_K=0,dK=0;int differences=nqpc_key(&p->packet,&previous_K)&&nqpc_sub(r->time_before,p->packet.time_before,&dt)&&nqpc_sub(r->query,p->packet.query,&dq)&&nqpc_sub(K,previous_K,&dK);
  if(!differences||!nqpc_same_binding(p,&s)||dt<0||dK<0||(dt==0&&(dq!=0||dK!=0))||(dt>0&&(dt!=r->clock_dt_before||dq!=dK||r->frame_before!=p->packet.frame_before+1u)))nqpc_reset(c);
  else if(dt==0){*p=s;return 1;} /* Same-T latest original observation wins. */
 }
 if(c->count==NQPC_CAPACITY){c->first=(c->first+1)%NQPC_CAPACITY;--c->count;}*nqpc_at(c,c->count++)=s;return 1;
}
static int nqpc_native_classes(const NqpcSample*s){uintptr_t packet_vptr=0;memcpy(&packet_vptr,s->packet.out_before,8);if(packet_vptr<0x15f0fd0)return 0;uintptr_t base=packet_vptr-0x15f0fd0;return s->clock.provider_vptr==base+0x15ee580&&s->clock.reader_vptr==base+0x15ee6a0&&s->clock.reader_timestamp_stream_vptr==base+0x15ee6a0;}
static int nqpc_clock_valid(const NqpcSample*s){const NqpcClock*m=&s->clock;const RpkRecord*r=&s->packet;int64_t query=0;
 return m->complete&&m->stable_before_after&&nqpc_native_classes(s)&&m->clock968&&m->clock968==m->clock970&&m->clock970==r->clock&&m->T968==m->T970&&m->T970==r->time_before&&m->dt968==m->dt970&&m->dt970==r->clock_dt_before&&r->clock_dt_before==r->clock_dt_after&&m->frame968==m->frame970&&m->frame970==r->frame_before&&m->C8_bits==UINT32_C(0x3f800000)&&m->CC==1&&m->CD==0&&m->actorE4==0&&m->B0>0&&m->B0<m->B8&&(m->C0<=0||m->B0<m->C0)&&m->reader68>0&&r->cursor==m->B0&&nqpc_add(m->B0,m->reader70,&query)&&(!nqpc_normalized(r)||nqpc_add(query,r->clock_dt_before-33333,&query))&&r->query==query&&r->query>0&&r->query<m->reader68;
}
static int nqpc_interval_valid(const NqpcSample*a,const NqpcSample*b){
 if(!nqpc_same_binding(a,b)||!nqpc_clock_valid(a)||!nqpc_clock_valid(b))return 0;
 const NqpcClock*x=&a->clock,*y=&b->clock;int64_t dt=0,dA=0,dB=0,dq=0,Ka=0,Kb=0,dK=0;
 return nqpc_key(&a->packet,&Ka)&&nqpc_key(&b->packet,&Kb)&&nqpc_sub(Kb,Ka,&dK)&&nqpc_sub(b->packet.time_before,a->packet.time_before,&dt)&&nqpc_sub(y->A8,x->A8,&dA)&&nqpc_sub(y->B0,x->B0,&dB)&&nqpc_sub(b->packet.query,a->packet.query,&dq)&&x->clock968==y->clock968&&dt>0&&dt<INT64_C(16777216)&&dK>0&&dK<INT64_C(16777216)&&y->frame968==x->frame968+1u&&dt==y->dt968&&(nqpc_normalized(&a->packet)?(dK==dt&&dA==x->dt968&&dB==x->dt968):(dK==x->dt968&&dA==dK&&dB==dK))&&dq==dK&&x->B8==y->B8&&x->C0==y->C0&&x->C8_bits==y->C8_bits&&x->CC==y->CC&&x->CD==y->CD&&x->CE==y->CE&&x->actorE4==y->actorE4&&x->reader70==y->reader70&&x->reader68==y->reader68;
}
static int nqpc_evaluate(NqpcCache*c,int64_t Q,NqpcEvaluate evaluate,void*context,unsigned char out[64],uintptr_t*rv,NqpcResult*result,unsigned allow_forward){
 if(result)memset(result,0,sizeof*result);if(!c||!c->count||!evaluate||Q<0||Q%33333||Q>nqpc_at(c,c->count-1)->packet.time_before)return NQPC_UNAVAILABLE;
 for(unsigned j=c->count;j>0;--j){NqpcSample*s=nqpc_at(c,j-1);int64_t K=0;if(!nqpc_key(&s->packet,&K)||K!=Q)continue;if(s->packet.clock_dt_before!=33333&&(!allow_forward||!nqpc_clock_valid(s)))continue;if(!evaluate(context,&s->packet,s->packet.query,out,rv))return NQPC_UNAVAILABLE;if(result){result->mode=1;result->requested_nativeQ=Q;result->mapped_query=s->packet.query;result->interval_T0=result->interval_T1=s->packet.time_before;result->interval_K0=result->interval_K1=K;result->source_sequence=s->packet.sequence;result->cache_generation=c->generation;}return NQPC_EXACT_OBSERVED;}
 if(!allow_forward)return NQPC_UNAVAILABLE;
 for(unsigned j=c->count;j>1;--j){NqpcSample*b=nqpc_at(c,j-1),*a=nqpc_at(c,j-2);int64_t Ka=0,Kb=0;if(!nqpc_key(&a->packet,&Ka)||!nqpc_key(&b->packet,&Kb)||!(Ka<Q&&Q<Kb)||!nqpc_interval_valid(a,b))continue;
  int64_t delta=Q-Ka,query=0;if(!nqpc_add(a->packet.query,delta,&query))return NQPC_UNAVAILABLE;
  for(unsigned k=j;k>0;--k){NqpcSample*s=nqpc_at(c,k-1);if(!nqpc_same_binding(s,b)||query<s->packet.t0||query>s->packet.t1)continue;
   if(!evaluate(context,&s->packet,query,out,rv))return NQPC_UNAVAILABLE;if(result){result->mode=2;result->requested_nativeQ=Q;result->mapped_query=query;result->interval_T0=a->packet.time_before;result->interval_T1=b->packet.time_before;result->interval_K0=Ka;result->interval_K1=Kb;result->source_sequence=s->packet.sequence;result->cache_generation=c->generation;}return NQPC_PROVEN_FORWARD_INTERVAL;
  }return NQPC_UNAVAILABLE;
 }return NQPC_UNAVAILABLE;
}
