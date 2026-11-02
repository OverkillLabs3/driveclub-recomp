/* PRIVATE optional producer/consumer API. Include after parent RLQ module.
 * Native consumers must explicitly call exact/hold APIs below; this module
 * NEVER supplies actual non-Q raw10 as a fallback private Q knot.
 */
#include "late-sensor-q-core.inc"
static LsqStore lsq_store;static SRWLOCK lsq_lock=SRWLOCK_INIT;
static int lsq_enabled;
typedef struct {int valid;uintptr_t frame;LsqPoint point;} LsqPending;
static _Thread_local LsqPending lsq_pending;
static HANDLE lsq_trace_file=INVALID_HANDLE_VALUE;static SRWLOCK lsq_trace_lock=SRWLOCK_INIT;
typedef struct {char magic[8];uint32_t version,size,capacity,count,dropped,failed;uint64_t reserved[4];} LsqTraceHeader;
typedef struct {uint64_t sequence,qpc;uint32_t stage,status;LsqPoint point;} LsqTraceRecord;
static LsqTraceHeader lsq_trace_header;
static void lsq_trace(unsigned stage,int status,const LsqPoint*p){
 (void)stage;(void)status;(void)p;
}
static void lsq_runtime_init(void){
 const char*flag=camera_fix_getenv("BB_PRIVATE_LATE_Q");lsq_enabled=flag&&!strcmp(flag,"1");if(!lsq_enabled)return;
 const char*parent=camera_fix_getenv("BB_REPLAY_LINEAR_NATIVE_Q"),*mean=camera_fix_getenv("BB_REPLAY_LINEAR_MEAN_REBASE");
 if(!parent||strcmp(parent,"1")||!mean||strcmp(mean,"1")){lsq_enabled=0;puts("PrivateLateQ: requires packetQ and mean-rebase flags; unavailable");return;}
 puts("PrivateLateQ: explicit private post-feedback Q publication; render SDK baseline separate; no raw/history writes");
}
/* Exact observed component/SDK wrapper binding by pointer reads, not a guest
 * getter. Same bounded registry path already used by actual palette observer.
 * Unknown/multiple aliases fail closed for owner-based GForce queries.
 */
static uintptr_t lsq_owner_for_vehicle(uintptr_t vehicle){
 uintptr_t root=0,service=0,manager=0,sentinel=0,node=0,owner=0;
 if(!camera_pose_read((uintptr_t)image+0x1911100,&root,8)||!camera_pose_read(root+0x970,&service,8)||!camera_pose_read(service+0x2d0,&manager,8)||!camera_pose_read(manager+0x18,&sentinel,8)||!camera_pose_read(sentinel,&node,8))return 0;
 for(unsigned i=0;i<128&&node&&node!=sentinel;++i){uintptr_t component=0,bound=0,entity=0;
  if(!camera_pose_read(node+0x18,&component,8)||!camera_pose_read(component+0x50,&bound,8)||!camera_pose_read(component+8,&entity,8))return 0;
  if(bound==vehicle){if(owner&&owner!=entity)return 0;owner=entity;}
  if(!camera_pose_read(node,&node,8))return 0;
 }
 return owner;
}
static int lsq_call_binding(const RlqCall*c,LsqBinding*out,int64_t*us){
 uintptr_t root=0,service=0,clock=0;int64_t T=INT64_MIN;
 memset(out,0,sizeof*out);out->actor=c->actor;out->vehicle=c->vehicle;out->raw_state=rlq_body_state(c->vehicle);
 if(!camera_pose_read(c->frame-0xe0,&out->provider,8)||!camera_pose_read((uintptr_t)image+0x1911100,&root,8)||!camera_pose_read(root+0x970,&service,8)||!camera_pose_read(service+0x1e0,&clock,8)||!camera_pose_read(clock+0x58,&T,8)||!camera_pose_read(clock+0x60,us,8)||T!=c->time)return 0;
 out->clock=clock;out->owner=lsq_owner_for_vehicle(c->vehicle);return lsq_binding_valid(out);
}
static void lsq_native_add(const float*a,const float*b,uint32_t csr,float*out,uint32_t*after){
 uint32_t saved;__asm__ volatile("stmxcsr %0":"=m"(saved));
 __asm__ volatile("ldmxcsr %2\n vmovups (%3),%%xmm0\n vaddps (%4),%%xmm0,%%xmm0\n vmovups %%xmm0,(%5)\n stmxcsr %0\n ldmxcsr %1":"=m"(*after):"m"(saved),"m"(csr),"r"(a),"r"(b),"r"(out):"xmm0","memory");
}
static int lsq_point_begin(const RlqCall*c,LsqPoint*p){
 memset(p,0,sizeof*p);int64_t us=0;if(!lsq_call_binding(c,&p->binding,&us))return 0;
 p->q=c->q;p->observed_render_T=c->time;p->observed_render_dt_us=us;p->kind=LSQ_NORMAL;
 p->mask=LSQ_BIT(LSQ_LATE_VELOCITY)|LSQ_BIT(LSQ_PHYSICAL_ARGUMENT);
 p->field_phase[LSQ_LATE_VELOCITY]=LSQ_PHASE_PRIVATE_POST_FEEDBACK;p->field_phase[LSQ_PHYSICAL_ARGUMENT]=LSQ_PHASE_OBSERVED_RENDER_SETTER;
 p->field_observed_T[LSQ_LATE_VELOCITY]=c->time;p->field_observed_T[LSQ_PHYSICAL_ARGUMENT]=c->time;
 memcpy(p->observed_render_mean,c->current_vel,16);return 1;
}
static void lsq_capture_before_add(void*frame,const RlqCall*c){
 if(!lsq_enabled||!c->active||!c->due||c->frame!=(uintptr_t)frame)return;
 uint32_t native_mxcsr;__asm__ volatile("stmxcsr %0":"=m"(native_mxcsr)); /* before ReadProcessMemory/libc helpers */
 LsqPending pending;memset(&pending,0,sizeof pending);pending.frame=(uintptr_t)frame;
 if(!lsq_point_begin(c,&pending.point))return;
 LsqPoint*p=&pending.point;p->native_mxcsr=native_mxcsr;
 memcpy(p->private_mean_Q,(char*)frame-0x50,16);memcpy(p->correction_Q,(char*)frame-0xc0,16);
 lsq_native_add(p->private_mean_Q,p->correction_Q,p->native_mxcsr,p->vector[LSQ_LATE_VELOCITY],&p->private_add_mxcsr);
 uint32_t ignored;lsq_native_add(p->observed_render_mean,p->correction_Q,p->native_mxcsr,p->vector[LSQ_PHYSICAL_ARGUMENT],&ignored);
 pending.valid=1;lsq_pending=pending;
}
static void lsq_complete_before_restore(void*frame,const void*call){
 const RlqCall*c=call;
 if(!lsq_enabled||!c->active||!c->due||c->frame!=(uintptr_t)frame)return;
 LsqPoint point;memset(&point,0,sizeof point);int ready=0;uintptr_t stored=0;memcpy(&stored,(char*)frame-0xc0,8);
 if(stored==(uintptr_t)c->actor_bytes+0x40){
  if(lsq_point_begin(c,&point)){point.kind=LSQ_TELEPORT;memcpy(point.private_mean_Q,(char*)frame-0x50,16);memcpy(point.vector[LSQ_LATE_VELOCITY],c->actor_bytes+0x40,16);memcpy(point.vector[LSQ_PHYSICAL_ARGUMENT],c->actor_bytes+0x40,16);__asm__ volatile("stmxcsr %0":"=m"(point.native_mxcsr));ready=1;}
 }else if(lsq_pending.valid&&lsq_pending.frame==(uintptr_t)frame&&lsq_pending.point.binding.actor==c->actor&&lsq_pending.point.binding.vehicle==c->vehicle&&lsq_pending.point.q==c->q&&lsq_pending.point.observed_render_T==c->time){
  point=lsq_pending.point;float actual_argument[4];memcpy(actual_argument,(char*)frame-0x60,16); /* signed disp8 0xa0 in original123ECE */
  if(!memcmp(actual_argument,point.vector[LSQ_PHYSICAL_ARGUMENT],16)){memcpy(point.vector[LSQ_PHYSICAL_ARGUMENT],actual_argument,16);ready=1;}
 }
 memset(&lsq_pending,0,sizeof lsq_pending);int status=LSQ_INVALID;LsqBinding invalid_binding={0};int64_t invalid_us=0;int known_invalid=!ready&&lsq_call_binding(c,&invalid_binding,&invalid_us);
 AcquireSRWLockExclusive(&lsq_lock);
 if(ready){status=lsq_publish(&lsq_store,&point,1);if(status==LSQ_OK||status==LSQ_DISCONTINUITY){LsqTrack*t=lsq_find_actor(&lsq_store,&point.binding);if(t&&t->active&&t->count)point=t->points[t->count-1];}else{lsqt_invalidate_locked(c->actor,0);lsq_mark_scope(&lsq_store,&point.binding,status);}}
 else{lsqt_invalidate_locked(c->actor,0);if(known_invalid)lsq_mark_scope(&lsq_store,&invalid_binding,LSQ_INVALID);}
 ReleaseSRWLockExclusive(&lsq_lock);if(ready&&status==LSQ_OK)status=lsqt_commit_binding(&point);lsq_trace(2,status,ready?&point:NULL);
}
static void lsq_parent_status(unsigned status,void*actor,uintptr_t vehicle,float dt,int64_t time,int64_t q,int64_t us){
 (void)dt;(void)q;
 if(!lsq_enabled||status==0)return; /* strict native30 has no private writes */
 RlqCall*c=&rlq_call;if(!c->active||c->actor!=(uintptr_t)actor||c->vehicle!=vehicle){if(status>=5){AcquireSRWLockExclusive(&lsq_lock);lsqt_invalidate_locked((uintptr_t)actor,0);ReleaseSRWLockExclusive(&lsq_lock);}return;}
 if(status==1||status==3||status==4)return;
 LsqBinding binding;int64_t observed=0;int valid=lsq_call_binding(c,&binding,&observed);
 /* An observed logical pause may hold its existing private Q value, without
  * inventing a knot or treating real-clock motion as new logical time. A
  * different binding/seek/out-of-cell value still invalidates below. */
 if(status==7&&us==0&&valid){LsqSelector selector={0};selector.actor=binding.actor;selector.clock=binding.clock;LsqPoint held;
  AcquireSRWLockShared(&lsq_lock);int hold=lsq_hold(&lsq_store,&selector,time,LSQ_BIT(LSQ_LATE_VELOCITY),&held);ReleaseSRWLockShared(&lsq_lock);
  if(hold==LSQ_OK&&lsq_binding_equal(&held.binding,&binding))return;
 }
 int reason=status==6?LSQ_UNSUPPORTED_LOW_FPS:status==2?LSQ_UNAVAILABLE:LSQ_INVALID;
 AcquireSRWLockExclusive(&lsq_lock);lsqt_invalidate_locked((uintptr_t)actor,0);if(valid)lsq_mark_scope(&lsq_store,&binding,reason);ReleaseSRWLockExclusive(&lsq_lock);lsq_trace(1,reason,NULL);
}
static void lsq_runtime_forget(uintptr_t actor,uintptr_t raw_state){
 if(!lsq_enabled)return;AcquireSRWLockExclusive(&lsq_lock);lsqt_invalidate_locked(actor,raw_state);ReleaseSRWLockExclusive(&lsq_lock);
 if(!actor||lsq_pending.point.binding.actor==actor)memset(&lsq_pending,0,sizeof lsq_pending);lsq_trace(3,LSQ_DISCONTINUITY,NULL);
}
/* Consumer APIs never read actual raw10. Missing fields are unsupported.
 * GForce: selector.owner+clock. HEAD: selector.vehicle+clock. HUD: raw_state+
 * clock and explicit PREPUB masks. ExactQ endpoints and HOLD are different APIs.
 */
static __attribute__((unused)) int lsq_velocity_at_Q(const LsqSelector*x,int64_t q,float*out,uint64_t*epoch){
 if(!lsq_enabled)return LSQ_UNAVAILABLE;LsqPoint p;AcquireSRWLockShared(&lsq_lock);int status=lsq_exact(&lsq_store,x,q,LSQ_BIT(LSQ_LATE_VELOCITY),&p);ReleaseSRWLockShared(&lsq_lock);if(status==LSQ_OK){memcpy(out,p.vector[LSQ_LATE_VELOCITY],16);if(epoch)*epoch=p.epoch;}return status;
}
/* A known-but-unavailable Replay scope must not silently use the old mixed
 * raw observation path. The returned point is an immutable copy after unlock. */
static __attribute__((unused)) int lsq_scope_hold(const LsqSelector*x,int64_t actual_logical_T,int*known,LsqPoint*out){
 if(known)*known=0;if(!lsq_enabled)return LSQ_UNAVAILABLE;
 AcquireSRWLockShared(&lsq_lock);const LsqTrack*t=NULL;int selected=lsq_select_track(&lsq_store,x,&t);int actual_known=t!=NULL||selected==LSQ_AMBIGUOUS;if(known)*known=actual_known;LsqBinding mode_binding={0};if(t)mode_binding=t->binding;
 int status=t?lsq_hold(&lsq_store,x,actual_logical_T,LSQ_BIT(LSQ_LATE_VELOCITY),out):selected;ReleaseSRWLockShared(&lsq_lock);lsqm_observe(x,&mode_binding,actual_logical_T,actual_known,status);return status;
}
static __attribute__((unused)) int lsq_velocity_pair_Q(const LsqSelector*x,int64_t q,LsqPoint*previous,LsqPoint*current){
 if(!lsq_enabled)return LSQ_UNAVAILABLE;AcquireSRWLockShared(&lsq_lock);int status=lsq_pair(&lsq_store,x,q,LSQ_BIT(LSQ_LATE_VELOCITY),previous,current);ReleaseSRWLockShared(&lsq_lock);return status;
}
static __attribute__((unused)) int lsq_velocity_hold(const LsqSelector*x,int64_t actual_logical_T,float*out,uint64_t*epoch){
 if(!lsq_enabled)return LSQ_UNAVAILABLE;LsqPoint p;AcquireSRWLockShared(&lsq_lock);int status=lsq_hold(&lsq_store,x,actual_logical_T,LSQ_BIT(LSQ_LATE_VELOCITY),&p);ReleaseSRWLockShared(&lsq_lock);if(status==LSQ_OK){memcpy(out,p.vector[LSQ_LATE_VELOCITY],16);if(epoch)*epoch=p.epoch;}return status;
}
static __attribute__((unused)) int lsq_hud_tuple_Q(const LsqSelector*x,int64_t q,uint32_t requested_fields,LsqPoint*out){
 if(!lsq_enabled)return LSQ_UNAVAILABLE;AcquireSRWLockShared(&lsq_lock);int status=lsq_exact(&lsq_store,x,q,requested_fields,out);ReleaseSRWLockShared(&lsq_lock);return status;
}
static __attribute__((unused)) int lsq_attach_native_Q_field(const LsqSelector*x,int64_t q,uint64_t epoch,unsigned field,const float*value,int64_t observedT,unsigned phase){
 if(!lsq_enabled)return LSQ_UNAVAILABLE;AcquireSRWLockExclusive(&lsq_lock);int status=lsq_attach(&lsq_store,x,q,epoch,field,value,observedT,phase);ReleaseSRWLockExclusive(&lsq_lock);return status;
}

#include "frozen-pause-binding.inc"
