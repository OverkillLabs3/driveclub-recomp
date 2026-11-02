/* Native-frozen selected68 continuation. It never marks an unavailable LSQ as
 * OK or obtains a new logicalQ. Only our OWN prior canonical linear is held.
 * Raw axes/look/caps continue publishing in the selected producer clock. */
static int hgg_pause_zero(const float*x){for(unsigned j=0;j<4;++j)if(x[j]!=0)return 0;return 1;}
static int hgg_pause_bank_equal(const unsigned char*a,const unsigned char*b){
 return !memcmp(a+0x1a0,b+0x1a0,64)&&!memcmp(a+0x1e0,b+0x1e0,8)&&!memcmp(a+0x1f0,b+0x1f0,48)&&!memcmp(a+0x220,b+0x220,8)&&!memcmp(a+0x2b0,b+0x2b0,4)&&!memcmp(a+0x350,b+0x350,12);
}
static int hgg_pause_binding(const LsqSelector*x,LsqBinding*b,uint64_t*epoch){
 const LsqTrack*t=NULL;int ok=0;AcquireSRWLockShared(&lsq_lock);int status=lsq_select_track(&lsq_store,x,&t);
 if(status!=LSQ_AMBIGUOUS&&t&&t->active&&!t->count&&t->status==LSQ_UNAVAILABLE&&t->epoch){*b=t->binding;*epoch=t->epoch;ok=1;}ReleaseSRWLockShared(&lsq_lock);return ok;
}
static int hgg_pause_births(const LsqBinding*b,uintptr_t*reader,uint64_t birth[5]){
 uintptr_t selected=0;int ambiguous=0;AcquireSRWLockShared(&lsq_lock);
 for(unsigned i=0;i<LSQ_TRACKS;++i)if(lsqt_leases[i].actor==b->actor&&lsqt_leases[i].provider==b->provider){if(selected&&selected!=lsqt_leases[i].reader)ambiguous=1;selected=lsqt_leases[i].reader;}
 ReleaseSRWLockShared(&lsq_lock);if(ambiguous||!selected||!pqn_births(b->actor,b->provider,selected,birth))return 0;*reader=selected;return 1;
}
static void hgg_pause_trace(const HtpRequest*r,const HCSClock*c,const HggOwner*s,const HggResult*out,int reason,uint64_t metadata){
 const HKTRecord*h=r->hkt;float d[24]={(float)out->status,(float)reason,(float)(s?s->pause_mode:0),(float)c->domain};int64_t origin=s?s->pause_origin_Q:0;
 memcpy(d+4,&r->logical_T,8);memcpy(d+6,&c->time_us,8);memcpy(d+8,&out->Q,8);memcpy(d+10,&origin,8);memcpy(d+12,&r->key.epoch,8);memcpy(d+14,&metadata,8);if(s)memcpy(d+16,&s->source_token,8);if(s)memcpy(d+18,s->pause_linear,16);d[22]=(float)out->ready;d[23]=(float)(s?s->known_replay:0);
 d[23]=(float)(s?s->source_birth_valid:0);camera_accuracy_log(49,(void*)(uintptr_t)h->actor,(uintptr_t)h->vehicle,h->frame,h->input.dt[0],d,24);
}
static int hgg_pause_select(const HtpRequest*r,const HCSClock*c,HggResult*out){
 const HKTRecord*h=r->hkt;const HprRecord*p=r->primitives;out->domain=c->domain;out->status=HGG_UNSUPPORTED_DOMAIN;
 AcquireSRWLockExclusive(&hgg_lock);HggOwner*s=NULL;for(unsigned i=0;i<HGG_OWNERS;++i)if(hgg_owners[i].used&&hgg_owners[i].actor==r->key.owner){s=hgg_owners+i;break;}
 int reason=1;uint64_t metadata=0;LsqBinding binding={0};LsqSelector selector={0};selector.vehicle=h->vehicle;selector.clock=c->clock;
 if(!s||c->domain!=1||p->helper_logical_dt!=0||!s->known_replay||!s->state.valid||!s->state.adapted||!s->state.ready||s->last_status!=HGG_SELECTED)goto unavailable;
 reason=2;if(s->vehicle!=h->vehicle||s->clock!=c->clock||s->epoch!=r->key.epoch||s->main_bl!=r->key.main_bl||h->frame!=s->last_frame+1u||s->state.observed_T!=s->last_selected_T||c->time_us-c->dt_us!=s->last_selected_T||s->last_raw_T!=p->helper_logical_T)goto unavailable;
 reason=3;if(!hgg_pause_bank_equal(h->before,s->state.output.final_actor))goto unavailable;
 reason=4;if(memcmp(h->input.current_linear,s->last_native.current_linear,16)||memcmp(h->input.current_angular,s->last_native.current_angular,16)||memcmp(h->input.matrix,s->last_native.matrix,64)||!hgg_pause_zero(h->input.delta_linear)||!hgg_pause_zero(h->input.delta_angular))goto unavailable;
 reason=5;if(!hgg_pause_binding(&selector,&binding,&metadata)||!lsq_binding_equal(&binding,&s->binding))goto unavailable;
 uintptr_t reader=0;uint64_t birth[5]={0};reason=11;if(!s->source_birth_valid||!hgg_pause_births(&binding,&reader,birth)||reader!=s->source_reader||memcmp(birth,s->source_birth,sizeof birth))goto unavailable;
 if(s->pause_mode==2){reason=6;if(metadata!=s->pause_metadata_token||s->pause_raw_T!=p->helper_logical_T)goto unavailable;}
 else{reason=7;if(s->state.domain!=0||s->last_selected_T!=p->helper_logical_T||s->pause_mode)goto unavailable;s->pause_mode=2;s->pause_metadata_token=metadata;s->pause_raw_T=p->helper_logical_T;s->pause_origin_Q=s->state.Q;memcpy(s->pause_linear,s->state.actor+0x130,16);}
 reason=8;if(!hgg_observe(s,c,h,p))goto unavailable;
 HggRequest request={h->actor,h->vehicle,c->clock,r->key.epoch,c->time_us,c->dt_us,1,r->key.main_bl,h,p};
 int result=hgg_update_frozen(&s->state,&request,hgg_sample_runtime,s,hgg_step_runtime,NULL,out);
 LsqBinding after={0};uint64_t after_metadata=0;reason=9;if(!hgg_pause_binding(&selector,&after,&after_metadata)||after_metadata!=metadata||!lsq_binding_equal(&after,&binding))goto unavailable;
 uintptr_t after_reader=0;uint64_t after_birth[5]={0};reason=12;if(!hgg_pause_births(&binding,&after_reader,after_birth)||after_reader!=reader||memcmp(after_birth,birth,sizeof birth))goto unavailable;
 reason=10;if(result!=HGG_SELECTED||!out->ready||out->domain!=1)goto unavailable;
 s->last_native=h->input;s->last_raw_T=p->helper_logical_T;s->last_selected_T=c->time_us;s->last_frame=h->frame;s->last_status=result;
 hgg_trace(r,c,s,out);hgg_pause_trace(r,c,s,out,0,metadata);ReleaseSRWLockExclusive(&hgg_lock);InterlockedIncrement64(hgg_counts+7);return result;
unavailable:
 memset(out,0,sizeof*out);out->domain=c->domain;out->status=HGG_UNSUPPORTED_DOMAIN;hgg_pause_trace(r,c,s,out,reason,metadata);hgg_trace(r,c,NULL,out);if(s)memset(s,0,sizeof*s);ReleaseSRWLockExclusive(&hgg_lock);InterlockedIncrement64(hgg_counts+3);return out->status;
}
