/* Passive source cache + private original-kernel evaluator. No RT controller
 * application, native setter, source writes, generic query or SO3 conversion.
 * Read actual mode fields at the existing packet call before/after original.
 * Exact nativeQ observations available independently; off-grid requires real
 * lifecycle hooks AND the complete code-derived clock/provider contract. */
#include "native-q-packet-cache-private.inc"

static int rpkt_private_kernel(const RpkRecord*,int64_t,unsigned char[64],uintptr_t*);
typedef struct{uintptr_t actor,provider,reader;NqpcCache cache;} RpqOwner;
typedef struct{uint64_t sequence,generation;uint32_t frame,thread,push_status,flags;NqpcClock before,after;} RpqContextRecord;
_Static_assert(sizeof(NqpcClock)==160,"RPCTX clock160");
_Static_assert(sizeof(RpqContextRecord)==352,"RPCTX record352");
typedef struct{unsigned char magic[8];uint32_t version,record_size,capacity,count,dropped,failed,reserved0,reserved1;uint64_t reserved[3];} RpqContextHeader;
_Static_assert(sizeof(RpqContextHeader)==64,"RPCTX header64");
typedef struct{
 int valid,mode;uintptr_t actor,provider,reader;int64_t nativeQ,query,t0,t1,source_nativeT,source_raw_dt,source_K;uint64_t source_sequence,generation;
 _Alignas(16)float pose[16],linear_velocity[4],angular_velocity[4];unsigned char packet[64];
} RpqTarget;
_Static_assert(offsetof(RpqTarget,pose)%16==0&&_Alignof(RpqTarget)>=16,"native673930 output requires aligned16");
static RpqOwner rpq_owners[32];static SRWLOCK rpq_lock=SRWLOCK_INIT,rpq_context_lock=SRWLOCK_INIT;
static volatile LONG64 rpq_epoch=1;static volatile LONG rpq_lifecycle_ready;
static int rpq_enabled;
static _Thread_local RpqContextRecord rpq_contexts[8];
static wchar_t rpq_context_path[4096];static HANDLE rpq_context_file=INVALID_HANDLE_VALUE;static RpqContextHeader rpq_context_header;
static __attribute__((unused)) void rpq_actor_invalidate(uintptr_t actor){lsq_runtime_forget(actor,0);AcquireSRWLockExclusive(&rpq_lock);for(unsigned i=0;i<32;++i)if(!actor||rpq_owners[i].actor==actor){nqpc_reset(&rpq_owners[i].cache);memset(&rpq_owners[i],0,sizeof rpq_owners[i]);}InterlockedIncrement64(&rpq_epoch);ReleaseSRWLockExclusive(&rpq_lock);}
static __attribute__((unused)) void rpq_lifecycle_observer_ready(unsigned bits){InterlockedOr(&rpq_lifecycle_ready,(LONG)(bits&3));}
static int rpq_clock_fields(uintptr_t service_offset,uintptr_t*clock,int64_t*T,int64_t*dt,uint32_t*frame){uintptr_t p=0;return camera_pose_read((uintptr_t)image+0x1911100,&p,8)&&camera_pose_read(p+service_offset,&p,8)&&camera_pose_read(p+0x1e0,clock,8)&&camera_pose_read(*clock+0x58,T,8)&&camera_pose_read(*clock+0x60,dt,8)&&camera_pose_read(*clock+0x21c,frame,4);}
static void rpq_snapshot(const RpkRecord*r,NqpcClock*m){
 memset(m,0,sizeof*m);m->lifecycle_epoch=(uint64_t)InterlockedCompareExchange64(&rpq_epoch,0,0);
 if(!r->actor||!r->provider||!r->reader)return;
 int valid=rpq_clock_fields(0x968,&m->clock968,&m->T968,&m->dt968,&m->frame968)&&rpq_clock_fields(0x970,&m->clock970,&m->T970,&m->dt970,&m->frame970);
 valid=camera_pose_read(r->provider+0xa8,&m->A8,8)&&valid;valid=camera_pose_read(r->provider+0xb0,&m->B0,8)&&valid;valid=camera_pose_read(r->provider+0xb8,&m->B8,8)&&valid;valid=camera_pose_read(r->provider+0xc0,&m->C0,8)&&valid;valid=camera_pose_read(r->provider+0xc8,&m->C8_bits,4)&&valid;
 valid=camera_pose_read(r->provider+0xcc,&m->CC,1)&&valid;valid=camera_pose_read(r->provider+0xcd,&m->CD,1)&&valid;valid=camera_pose_read(r->provider+0xce,&m->CE,1)&&valid;valid=camera_pose_read(r->actor+0xe4,&m->actorE4,1)&&valid;valid=camera_pose_read(r->reader+0x68,&m->reader68,8)&&valid;valid=camera_pose_read(r->reader+0x70,&m->reader70,8)&&valid;valid=camera_pose_read(r->actor+0xe8,&m->record_marker_E8,8)&&valid;m->complete=(unsigned)valid;
 valid=camera_pose_read(r->provider,&m->provider_vptr,8)&&valid;valid=camera_pose_read(r->reader,&m->reader_vptr,8)&&valid;valid=camera_pose_read(r->reader+0x28,&m->reader_timestamp_stream_vptr,8)&&valid;m->complete=(unsigned)valid;
}

static void rpq_context_write(const RpqContextRecord*r){
 (void)r;
}
static void rpq_packet_before(const RpkRecord*r,unsigned depth){if(!rpq_enabled||depth>=8)return;RpqContextRecord*c=rpq_contexts+depth;memset(c,0,sizeof*c);c->sequence=r->sequence;c->frame=r->frame_before;c->thread=r->thread;rpq_snapshot(r,&c->before);}
static void rpq_packet_after(const RpkRecord*r,unsigned depth){if(!rpq_enabled||depth>=8)return;RpqContextRecord*c=rpq_contexts+depth;if(c->sequence!=r->sequence)return;rpq_snapshot(r,&c->after);
 if(!memcmp(&c->before,&c->after,sizeof c->before)){c->before.stable_before_after=c->after.stable_before_after=1;c->flags|=1;}if(c->before.complete&&c->after.complete)c->flags|=2;
 AcquireSRWLockExclusive(&rpq_lock);
 /* An actual invalidation can land after the post snapshot, before this lock.
  * Never repopulate a retired binding from that already-completed call. */
 if(c->after.lifecycle_epoch!=(uint64_t)InterlockedCompareExchange64(&rpq_epoch,0,0)){c->flags|=4;c->push_status=3;ReleaseSRWLockExclusive(&rpq_lock);rpq_context_write(c);return;}
 RpqOwner*owner=NULL,*empty=NULL;
 for(unsigned i=0;i<32;++i){if(rpq_owners[i].actor==r->actor&&r->actor){owner=rpq_owners+i;break;}if(!rpq_owners[i].actor&&!empty)empty=rpq_owners+i;}
 if(!owner&&nqpc_packet_valid(r)&&empty){owner=empty;owner->actor=r->actor;owner->provider=r->provider;owner->reader=r->reader;}
 if(owner){if(!(c->flags&1)){nqpc_reset(&owner->cache);c->push_status=2;}else{c->push_status=nqpc_push(&owner->cache,r,&c->before)?1:0;owner->provider=r->provider;owner->reader=r->reader;}c->generation=owner->cache.generation;}
 ReleaseSRWLockExclusive(&rpq_lock);rpq_context_write(c);
}
static int rpq_original_packet_eval(void*unused,const RpkRecord*r,int64_t query,unsigned char out[64],uintptr_t*result){(void)unused;return rpkt_private_kernel(r,query,out,result);}
static __attribute__((unused)) int rpq_target_at_native_Q(uintptr_t actor,uintptr_t provider,int64_t Q,RpqTarget*out){
 if(!out)return 0;memset(out,0,sizeof*out);if(!rpq_enabled)return 0;AcquireSRWLockExclusive(&rpq_lock);RpqOwner*owner=NULL;for(unsigned i=0;i<32;++i)if(rpq_owners[i].actor==actor&&(!provider||rpq_owners[i].provider==provider)){owner=rpq_owners+i;break;}if(!owner||!owner->cache.count||nqpc_at(&owner->cache,owner->cache.count-1)->clock.lifecycle_epoch!=(uint64_t)InterlockedCompareExchange64(&rpq_epoch,0,0)){ReleaseSRWLockExclusive(&rpq_lock);return 0;}
 uintptr_t rv=0;NqpcResult result;uint64_t role_token=0;unsigned forward=(InterlockedCompareExchange(&rpq_lifecycle_ready,0,0)&1)&&rpl_binding_ready(owner->actor,owner->provider,owner->reader,&role_token);int mode=nqpc_evaluate(&owner->cache,Q,rpq_original_packet_eval,NULL,out->packet,&rv,&result,forward);
 if(!mode||!(rv&255)){ReleaseSRWLockExclusive(&rpq_lock);return 0;}const RpkRecord*source=NULL;for(unsigned i=0;i<owner->cache.count;++i){const RpkRecord*p=&nqpc_at(&owner->cache,i)->packet;if(p->sequence==result.source_sequence){source=p;break;}}if(!source){ReleaseSRWLockExclusive(&rpq_lock);return 0;}
 _Alignas(16)float quaternion[4];_Alignas(16)unsigned char fx[512];memcpy(quaternion,out->packet+0x18,16);__asm__ volatile("fxsave64 %0\n ldmxcsr %1":"=m"(fx):"m"(source->mxcsr_after):"memory");typedef void(ABI*NativeBasis)(const float*,float*);((NativeBasis)(image+0x673930))(quaternion,out->pose);__asm__ volatile("fxrstor64 %0"::"m"(fx):"memory");memcpy(out->pose+12,out->packet+0xc,12);memcpy(out->linear_velocity,out->packet+0x28,12);memcpy(out->angular_velocity,out->packet+0x34,12);
 if(forward||mode==NQPC_PROVEN_FORWARD_INTERVAL){uint64_t after_token=0;if(!forward||!rpl_binding_ready(owner->actor,owner->provider,owner->reader,&after_token)||after_token!=role_token){memset(out,0,sizeof*out);ReleaseSRWLockExclusive(&rpq_lock);return 0;}}
 out->valid=1;out->mode=mode;out->actor=actor;out->provider=owner->provider;out->reader=owner->reader;out->nativeQ=Q;out->query=result.mapped_query;out->t0=source->t0;out->t1=source->t1;out->source_nativeT=source->time_before;out->source_raw_dt=source->clock_dt_before;nqpc_key(source,&out->source_K);out->source_sequence=source->sequence;out->generation=result.cache_generation;ReleaseSRWLockExclusive(&rpq_lock);return 1;
}

static int rpq_runtime_install(void){const char*p=camera_fix_getenv("BB_NATIVE_PACKET_CACHE");if(!p||strcmp(p,"1"))return 0;rpq_enabled=1;
 const char*path=camera_fix_getenv("BB_REPLAY_PACKET_CONTEXT_TAPE");if(path&&*path){wchar_t temp[4096],full[4096];if(MultiByteToWideChar(CP_UTF8,0,path,-1,temp,4096)>0&&GetFullPathNameW(temp,4096,full,NULL)&&((full[0]=='D'||full[0]=='d')&&full[1]==':'&&full[2]=='\\'))wcscpy(rpq_context_path,full);else puts("NativePacketCache: context tape unavailable (requires absolute D: path)");}
 puts("NativePacketCache: passive immutable packets + original private evaluator; no RT writes; offgrid waits for verified lifecycle+clock contract");return 1;
}
