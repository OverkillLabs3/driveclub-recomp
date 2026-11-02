/* Include after the native role registry/lifecycle declarations. Existing native
 * gateways remain unchanged. No source/SDK/raw/camera/RNG writes or new getter.
 * Mode exit without native retirement/rebind is explicitly UNSUPPORTED here;
 * E4 local selector and CC stop-update gate are NOT lifetime proof.
 */
#include "late-sensor-q-tombstone-core.inc"
static int lsqt_enabled;static LsqtLease lsqt_leases[LSQ_TRACKS];
static void lsqt_init(void){const char*f=camera_fix_getenv("BB_PRIVATE_LATE_Q_TOMBSTONE");lsqt_enabled=lsq_enabled&&f&&!strcmp(f,"1");if(lsqt_enabled)puts("PrivateLateQTombstone: data reset preserves known unavailable Replay; real observed native birth/death/rebind retires identity; mode-exit predicate not yet established");}
/* Caller already holds LSQ; never call a registry/HSQ/GFR/RPQ callback here. */
static void lsqt_invalidate_locked(uintptr_t actor,uintptr_t raw_state){if(lsqt_enabled)lsqt_invalidate_data(&lsq_store,actor,raw_state);else lsq_forget(&lsq_store,actor,raw_state);}
static int lsqt_commit_binding(const void*point){
 if(!lsqt_enabled)return LSQ_OK;const LsqPoint*p=point;uintptr_t reader=0;
 /* Copy RPQ binding before acquiring LSQ. Never nest LSQ -> RPQ. */
 AcquireSRWLockShared(&rpq_lock);for(unsigned i=0;i<32;++i)if(rpq_owners[i].actor==p->binding.actor&&rpq_owners[i].provider==p->binding.provider){reader=rpq_owners[i].reader;break;}ReleaseSRWLockShared(&rpq_lock);
 uint64_t token=0;int live=reader&&rpl_binding_ready(p->binding.actor,p->binding.provider,reader,&token);
 AcquireSRWLockExclusive(&lsq_lock);int bound=live&&lsqt_bind_lease(lsqt_leases,p->binding.actor,p->binding.provider,reader);if(!bound)lsqt_invalidate_data(&lsq_store,p->binding.actor,0);ReleaseSRWLockExclusive(&lsq_lock);
 lsq_trace(7,bound?LSQ_OK:LSQ_UNAVAILABLE,p);return bound?LSQ_OK:LSQ_UNAVAILABLE;
}
/* Called after actual rpl_roles_begin and before the old conservative packet
 * cache invalidation. These native stage identities are ctor/dtor/rebind proof;
 * ordinary mutation/seek/data invalidation preserves unavailable scope instead.
 */
static void lsqt_role_event(unsigned stage,uintptr_t owner){
 if(!lsqt_enabled||!lsqt_retirement_stage(stage))return;
 AcquireSRWLockExclusive(&lsq_lock);unsigned retired=lsqt_retire_role(&lsq_store,lsqt_leases,stage,owner);ReleaseSRWLockExclusive(&lsq_lock);
 if(retired){LsqPoint metadata={0};metadata.kind=stage;metadata.binding.actor=owner;metadata.mask=retired;lsq_trace(6,LSQ_DISCONTINUITY,&metadata);}
}
