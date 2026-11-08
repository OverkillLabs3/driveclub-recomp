/* OPTIONAL causal source experiment, not native30 physics reconstruction.
 * Existing RlqCall layout/current_vel are deliberately unchanged for the
 * separately tested mean-aware final-addition addon. Pure all-Q epochs execute
 * the original whole region without target/cache validation or extra getter.
 * Adapted target comes ONLY from actual immutable native packets. Current SDK
 * position/mean are still the old render-endpoint linear sampling EXPERIMENT.
 * Unsupported/missing Q data skips the entire linear branch, NEVER native .3
 * at render dt. Existing angular path and authored angular90 zero are unchanged.
 */
static void rlpq_packet_trace(void*actor,uintptr_t vehicle,uintptr_t provider,int64_t T,int64_t Q,const RpqTarget*target){
 (void)actor;(void)vehicle;(void)provider;(void)T;(void)Q;(void)target;
}
static __attribute__((used,noinline)) ABI uintptr_t rlq_enter(void*frame,void*actor,void*vehicle,unsigned char*context){
 const uintptr_t skip=(uintptr_t)image+0x123ee3;float dt=0;camera_pose_read((uintptr_t)frame-0xb0,&dt,4);
 if(rlq_call.active){rlq_status(8,actor,(uintptr_t)vehicle,dt,0,0,0);return skip;}
 RlqInput input;memset(&input,0,sizeof input);int64_t us=0;int have_clock=rlq_clock(&input.clock,&input.time,&us);
 int observed=camera_pose_read((uintptr_t)frame-0xe0,&input.provider,8)&&camera_pose_read((uintptr_t)actor,input.actor_bytes,sizeof input.actor_bytes)&&camera_pose_read((uintptr_t)frame-0xc0,input.current_pos,16)&&camera_pose_read((uintptr_t)frame-0x50,input.current_vel,16);
 rlq_call.active=1;rlq_call.due=0;rlq_call.frame=(uintptr_t)frame;rlq_call.actor=(uintptr_t)actor;rlq_call.vehicle=(uintptr_t)vehicle;rlq_call.time=input.time;rlq_call.dt=dt;rlq_call.q=0;
 uint64_t packet_epoch=(uint64_t)InterlockedCompareExchange64(&rpq_epoch,0,0);
 AcquireSRWLockExclusive(&rlq_lock);RlqState*s=rlq_find((uintptr_t)actor,(uintptr_t)vehicle);
 if(!s){ReleaseSRWLockExclusive(&rlq_lock);rlq_status(7,actor,(uintptr_t)vehicle,dt,input.time,0,us);return skip;}
 /* Native30 is the unchanged native operation, including exceptional values.
  * Finite/cache/source checks below only govern an adapted off-grid epoch. */
 if(have_clock&&!s->adapted&&us==RLQ_Q_US){s->previous=input;s->initialized=observed;s->last_q=input.time;s->packet_epoch=packet_epoch;ReleaseSRWLockExclusive(&rlq_lock);if(observed)rlq_record32(frame,actor,vehicle,&input,dt,.3f);rlq_status(0,actor,(uintptr_t)vehicle,dt,input.time,input.time,us);return rlq_original_entry;}
 s->adapted=1;
 if(!have_clock||us<=0||!observed||!rlq_finite(input.current_pos,4)||!rlq_finite(input.current_vel,4)){
  s->initialized=0;ReleaseSRWLockExclusive(&rlq_lock);rlq_status(7,actor,(uintptr_t)vehicle,dt,input.time,0,us);return skip;
 }
 int reset=!s->initialized||s->vehicle!=(uintptr_t)vehicle||s->previous.provider!=input.provider||s->previous.clock!=input.clock||input.time<s->previous.time||s->packet_epoch!=packet_epoch;
 if(reset){s->vehicle=(uintptr_t)vehicle;s->previous=input;s->initialized=1;s->packet_epoch=packet_epoch;s->last_q=(input.time/RLQ_Q_US)*RLQ_Q_US;int64_t q=s->last_q;ReleaseSRWLockExclusive(&rlq_lock);rlq_call.q=q;rlq_record32(frame,actor,vehicle,&input,dt,0);rlq_status(2,actor,(uintptr_t)vehicle,dt,input.time,q,us);return skip;}
 if(input.time==s->previous.time){s->previous=input;int64_t q=s->last_q;ReleaseSRWLockExclusive(&rlq_lock);rlq_call.q=q;rlq_record32(frame,actor,vehicle,&input,dt,0);rlq_status(3,actor,(uintptr_t)vehicle,dt,input.time,q,us);return skip;}
 int64_t q=0,next_q=0;if(!nqpc_add(s->last_q,RLQ_Q_US,&q)||!nqpc_add(q,RLQ_Q_US,&next_q)||us>RLQ_Q_US||next_q<=input.time){s->previous=input;s->last_q=(input.time/RLQ_Q_US)*RLQ_Q_US;ReleaseSRWLockExclusive(&rlq_lock);rlq_record32(frame,actor,vehicle,&input,dt,0);rlq_status(6,actor,(uintptr_t)vehicle,dt,input.time,q,us);return skip;}
 if(q>input.time){s->previous=input;ReleaseSRWLockExclusive(&rlq_lock);rlq_call.q=q;rlq_record32(frame,actor,vehicle,&input,dt,0);rlq_status(1,actor,(uintptr_t)vehicle,dt,input.time,q,us);return skip;}
 if(q<s->previous.time){s->previous=input;s->last_q=(input.time/RLQ_Q_US)*RLQ_Q_US;ReleaseSRWLockExclusive(&rlq_lock);rlq_call.q=q;rlq_record32(frame,actor,vehicle,&input,dt,0);rlq_status(5,actor,(uintptr_t)vehicle,dt,input.time,q,us);return skip;}
 double u=(double)(q-s->previous.time)/(double)(input.time-s->previous.time);RlqInput before=s->previous;s->previous=input;s->last_q=q;ReleaseSRWLockExclusive(&rlq_lock);
 RlqCall*c=&rlq_call;c->q=q;RpqTarget target;memset(&target,0,sizeof target);int available=rpq_target_at_native_Q((uintptr_t)actor,input.provider,q,&target);
 if(!available||!target.valid||!rlq_finite(target.pose,16)||!rlq_finite(target.linear_velocity,4)){
  rlpq_packet_trace(actor,(uintptr_t)vehicle,input.provider,input.time,q,&target);rlq_record32(frame,actor,vehicle,&input,dt,0);rlq_status(9,actor,(uintptr_t)vehicle,dt,input.time,q,us);return skip;
 }
 memcpy(c->actor_bytes,input.actor_bytes,sizeof c->actor_bytes);float curp[4],curv[4];
 for(unsigned j=0;j<4;++j){curp[j]=(float)((double)before.current_pos[j]+((double)input.current_pos[j]-before.current_pos[j])*u);curv[j]=(float)((double)before.current_vel[j]+((double)input.current_vel[j]-before.current_vel[j])*u);}
 memcpy(c->actor_bytes+0x40,target.linear_velocity,16);memcpy(c->actor_bytes+0x60,target.pose,64);
 c->due=1;memcpy(c->current_pos,input.current_pos,16);memcpy(c->current_vel,input.current_vel,16);memcpy(&c->r12,context+1064,8);memcpy(&c->r14,context+1048,8);
 const float nativeQ=33333.f*1e-6f;memcpy((char*)frame-0xb0,&nativeQ,4);memcpy((char*)frame-0xc0,curp,16);memcpy((char*)frame-0x50,curv,16);
 uintptr_t actorQ=(uintptr_t)c->actor_bytes,poseQ=actorQ+0x60;memcpy(context+1064,&actorQ,8);memcpy(context+1048,&poseQ,8);memcpy(context+512,target.linear_velocity,16);memcpy(context+544,target.pose+12,16);
 /* Logging follows all selected-input computation; even a destructive test
  * logger cannot change rounding of the selected SDK/current endpoints. */
 float trace[24]={0};memcpy(trace,curv,16);memcpy(trace+4,target.linear_velocity,16);memcpy(trace+8,curp,16);memcpy(trace+12,target.pose+12,16);memcpy(trace+16,&q,8);memcpy(trace+18,&input.time,8);trace[20]=(float)u;rlpq_packet_trace(actor,(uintptr_t)vehicle,input.provider,input.time,q,&target);
 camera_accuracy_log(37,actor,(uintptr_t)vehicle,input.provider,nativeQ,trace,21);rlq_record32(frame,actor,vehicle,&input,dt,.3f);rlq_status(4,actor,(uintptr_t)vehicle,dt,input.time,q,us);return rlq_original_entry;
}
