#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../src/camera_response/late-sensor-q-core.inc"
#include "../../src/camera_response/late-sensor-q-tombstone-core.inc"
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 LsqPoint p={0},out;unsigned stage=6;uintptr_t owner=0x1010;int64_t T=33333;
 p.binding=(LsqBinding){owner,0x2020,0x3030,0x4040,0x5050,0x6060};
 p.q=p.observed_render_T=T;p.observed_render_dt_us=8333;p.kind=LSQ_NORMAL;
 p.mask=LSQ_BIT(LSQ_LATE_VELOCITY)|LSQ_BIT(LSQ_PHYSICAL_ARGUMENT);
 p.field_phase[LSQ_LATE_VELOCITY]=LSQ_PHASE_PRIVATE_POST_FEEDBACK;
 p.field_phase[LSQ_PHYSICAL_ARGUMENT]=LSQ_PHASE_OBSERVED_RENDER_SETTER;
 p.field_observed_T[LSQ_PHYSICAL_ARGUMENT]=T;

 LsqStore s={0},before=s;LsqtLease leases[LSQ_TRACKS]={0};LsqSelector x={0};x.owner=p.binding.owner;x.clock=p.binding.clock;
 CHECK(lsq_publish(&s,&p,0)==LSQ_NATIVE_BYPASS&&!memcmp(&s,&before,sizeof s));CHECK(lsq_publish(&s,&p,1)==LSQ_OK);CHECK(lsq_hold(&s,&x,T,1,&out)==LSQ_OK);uint64_t epoch=out.epoch;CHECK(lsqt_bind_lease(leases,p.binding.actor,p.binding.provider,0xABC0));
 CHECK(stage==6&&owner==p.binding.actor);CHECK(lsqt_retire_role(&s,leases,stage,owner)==0);lsqt_invalidate_data(&s,owner,0);const LsqTrack*known=NULL;CHECK(lsq_select_track(&s,&x,&known)==LSQ_UNAVAILABLE&&known&&known->active&&known->count==0);CHECK(known->epoch!=epoch);CHECK(lsq_hold(&s,&x,T,1,&out)==LSQ_UNAVAILABLE);CHECK(lsq_pair(&s,&x,p.q,1,&out,&p)==LSQ_UNAVAILABLE);
 /* A synthetic waiting frame is known Replay, never source/raw mode. */
 int private_route=known!=NULL;int valid_private_data=lsq_hold(&s,&x,T,1,&out)==LSQ_OK;CHECK(private_route&&!valid_private_data);
 /* Constructor reuse and destructor are identity retirement, not data reset. */
 CHECK(lsqt_retire_role(&s,leases,0,owner)==1);known=NULL;CHECK(lsq_select_track(&s,&x,&known)==LSQ_UNAVAILABLE&&!known);
 CHECK(lsq_publish(&s,&p,1)==LSQ_OK);CHECK(lsqt_bind_lease(leases,p.binding.actor,p.binding.provider,0xABC0));CHECK(lsqt_retire_role(&s,leases,6,p.binding.actor)==0);CHECK(lsqt_retire_role(&s,leases,10,p.binding.provider)==1);known=NULL;CHECK(lsq_select_track(&s,&x,&known)==LSQ_UNAVAILABLE&&!known);
 for(unsigned event=15;event<=17;++event){CHECK(lsq_publish(&s,&p,1)==LSQ_OK);CHECK(lsqt_bind_lease(leases,p.binding.actor,p.binding.provider,0xABC0));CHECK(lsqt_retire_role(&s,leases,event,event==17?0xABE8:0xABC0)==1);known=NULL;CHECK(lsq_select_track(&s,&x,&known)==LSQ_UNAVAILABLE&&!known);}
 CHECK(lsq_publish(&s,&p,1)==LSQ_OK);lsqt_invalidate_data(&s,0,p.binding.raw_state);CHECK(lsq_select_track(&s,&x,&known)==LSQ_UNAVAILABLE&&known);CHECK(lsq_mark_scope(&s,&p.binding,LSQ_UNAVAILABLE)==LSQ_UNAVAILABLE);CHECK(lsq_exact(&s,&x,p.q,1,&out)==LSQ_UNAVAILABLE);CHECK(lsq_attach(&s,&x,p.q,epoch,LSQ_HUD_B0,(float[4]){0},T,LSQ_PHASE_PRIVATE_PRE_PUBLICATION)==LSQ_UNAVAILABLE);
 CHECK(lsqt_retire_role(&s,leases,1,owner)==1);memset(&s,0,sizeof s);before=s;lsq_forget(&s,0,0);lsqt_invalidate_data(&s,0,0);CHECK(!memcmp(&s,&before,sizeof s));CHECK(lsq_publish(&s,NULL,0)==LSQ_NATIVE_BYPASS&&!memcmp(&s,&before,sizeof s));
 printf("PASS synthetic lifecycle/reset sequence: known unavailable Replay tombstone, no fakeQ/force/raw fallback; exact actor/provider/reader/stream birth/death/reuse retirement, source/no-existing-scope and native30 no-write. checks=%u\n",checks);return 0;
}
