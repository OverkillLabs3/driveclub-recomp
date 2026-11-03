/* PRIVATE data invalidation is not a native lifetime retirement. No TTL. */
typedef struct{uintptr_t actor,provider,reader;} LsqtLease;
static void lsqt_invalidate_data(LsqStore*s,uintptr_t actor,uintptr_t raw_state){
 for(unsigned i=0;i<LSQ_TRACKS;++i){LsqTrack*t=s->tracks+i;
  if(t->active&&(!actor||t->binding.actor==actor)&&(!raw_state||t->binding.raw_state==raw_state)){
   LsqBinding binding=t->binding;lsq_clear_track(s,t);t->binding=binding;t->active=1;t->status=LSQ_UNAVAILABLE;
  }
 }
}
static int lsqt_retirement_stage(unsigned stage){return stage<=3||stage==8||stage==9||stage==10||stage==15||stage==16||stage==17;}
static int lsqt_role_matches(const LsqBinding*b,const LsqtLease*lease,unsigned stage,uintptr_t owner){
 if(!owner||!lsqt_retirement_stage(stage))return 0;
 if(stage<=3)return b->actor==owner;
 if(stage==8||stage==9||stage==10)return b->provider==owner;
 if(!lease||lease->actor!=b->actor||lease->provider!=b->provider||!lease->reader)return 0;
 if(stage==15||stage==16)return lease->reader==owner;
 return lease->reader==owner||(lease->reader<=UINTPTR_MAX-0x28&&lease->reader+0x28==owner);
}
static unsigned lsqt_retire_role(LsqStore*s,LsqtLease*leases,unsigned stage,uintptr_t owner){
 unsigned count=0;if(!lsqt_retirement_stage(stage)||!owner)return 0;
 for(unsigned i=0;i<LSQ_TRACKS;++i){LsqTrack*t=s->tracks+i;if(!t->active)continue;LsqtLease*lease=NULL;
  for(unsigned j=0;j<LSQ_TRACKS;++j)if(leases[j].actor==t->binding.actor&&leases[j].provider==t->binding.provider){lease=leases+j;break;}
  if(lsqt_role_matches(&t->binding,lease,stage,owner)){uintptr_t actor=t->binding.actor;lsq_clear_track(s,t);++count;for(unsigned j=0;j<LSQ_TRACKS;++j)if(leases[j].actor==actor)memset(leases+j,0,sizeof leases[j]);}
 }
 return count;
}
static int lsqt_bind_lease(LsqtLease*leases,uintptr_t actor,uintptr_t provider,uintptr_t reader){
 if(!actor||!provider||!reader)return 0;LsqtLease*empty=NULL;
 for(unsigned i=0;i<LSQ_TRACKS;++i){LsqtLease*l=leases+i;if(l->actor==actor){*l=(LsqtLease){actor,provider,reader};return 1;}if(!l->actor&&!empty)empty=l;}
 if(!empty)return 0;*empty=(LsqtLease){actor,provider,reader};return 1;
}
