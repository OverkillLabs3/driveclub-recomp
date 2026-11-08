#include "role-core.inc"
static RlrRegistry rpl_roles;static SRWLOCK rpl_roles_lock=SRWLOCK_INIT;static int rpl_roles_enabled;
static uint64_t rpl_roles_begin(unsigned stage,uintptr_t owner){if(!rpl_roles_enabled)return 0;AcquireSRWLockExclusive(&rpl_roles_lock);uint64_t t=rlr_begin(&rpl_roles,stage,owner);ReleaseSRWLockExclusive(&rpl_roles_lock);return t;}
static void rpl_roles_end(unsigned stage,uintptr_t owner,uint64_t token){if(!rpl_roles_enabled)return;AcquireSRWLockExclusive(&rpl_roles_lock);rlr_end(&rpl_roles,stage,owner,token);ReleaseSRWLockExclusive(&rpl_roles_lock);}
static int rpl_binding_ready(uintptr_t actor,uintptr_t provider,uintptr_t reader,uint64_t*token){
 if(token)*token=0;if(!rpl_roles_enabled||!rpl_installed)return 0;AcquireSRWLockShared(&rpl_roles_lock);int ok=rlr_binding(&rpl_roles,actor,provider,reader,token);ReleaseSRWLockShared(&rpl_roles_lock);return ok;
}
static void rpl_roles_install(void){const char*v=camera_fix_getenv("BB_REPLAY_PACKET_LIFECYCLE_ROLES");if(v&&!strcmp(v,"1")){rpl_roles_enabled=1;puts("ReplayPacketRoles: observed native births + published original consumer/class guard; owner scoped, no global readiness2/TTL");}}
