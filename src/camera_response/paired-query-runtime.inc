/* OPTIONAL causal prototype. Only the TWO original Replay actor reader calls.
 * Changes integer RSI argument, never provider/reader/SDK/physics memory.
 * Marker and pose keep one immutable shift. A between-call scope violation is
 * observable acceptance failure, NOT claimed atomic rollback. No TTL. */
#include "paired-query-core.inc"
#define PQN_TAG UINT64_C(0x50514e3100000001)
#ifdef NQPC_NORMALIZED_TAG
_Static_assert(PQN_TAG==NQPC_NORMALIZED_TAG,"paired runtime/cache provenance tag must agree");
#endif
typedef struct {uintptr_t actor,provider,reader,clock,rbp;uint64_t birth[5];int64_t T,dt,B0,reader70,reader68;uint32_t frame;unsigned rate,CC,CD,CE,E4;} PqnScope;
typedef struct {PqnScope scope;PqnTime time;uint64_t id;unsigned shifted,stage;uintptr_t returnpc;} PqnPair;
typedef struct {uintptr_t actor,provider,reader,clock;uint64_t birth[5];unsigned adapted,positive;} PqnBinding;
typedef struct {uint64_t sequence,qpc,pair,packet;uintptr_t actor,provider,reader,clock,rbp,caller;int64_t T,dt,argument,selected_argument,reader70,reader68,B0,oldK,effectiveK;uint64_t birth[5];uint32_t stage,status,frame,flags,thread,reserved;} PqnRecord;
typedef struct {unsigned char magic[8];uint32_t version,record_size,capacity,count,dropped,failed,reserved0,reserved1;uint64_t qpc_frequency,pair_violations,reserved;} PqnHeader;
_Static_assert(sizeof(PqnHeader)==64,"PQN header64");
_Static_assert(sizeof(PqnRecord)==216,"PQN record216");
static int pqn_enabled;static __attribute__((used)) uintptr_t pqn_peek_original,pqn_main_original;
static _Thread_local PqnPair pqn_pairs[8];static _Thread_local unsigned pqn_count;
static PqnBinding pqn_bindings[64];static SRWLOCK pqn_binding_lock=SRWLOCK_INIT,pqn_tape_lock=SRWLOCK_INIT;
static PqnHeader*pqn_header;static PqnRecord*pqn_records;static volatile LONG64 pqn_pair_sequence,pqn_violations;
static __attribute__((naked,used)) ABI void pqn_main_post_bridge(void);
static int pqn_births(uintptr_t a,uintptr_t p,uintptr_t v,uint64_t out[5]){
 if(!rpl_roles_enabled||!rpl_installed||v>UINTPTR_MAX-0x28)return 0;
 AcquireSRWLockShared(&rpl_roles_lock);uint64_t token=0;int ok=rlr_binding(&rpl_roles,a,p,v,&token);RlrSlot*slots[5]={rlr_find(&rpl_roles,1,a,0),rlr_find(&rpl_roles,2,p,0),rlr_find(&rpl_roles,3,v,0),rlr_find(&rpl_roles,4,v,0),rlr_find(&rpl_roles,4,v+0x28,0)};
 for(unsigned i=0;i<5;++i)out[i]=slots[i]?slots[i]->birth:0;ReleaseSRWLockShared(&rpl_roles_lock);return ok;
}
static int pqn_snapshot(uintptr_t actor,uintptr_t rbp,uintptr_t reader,int64_t argument,PqnScope*s){
 memset(s,0,sizeof*s);s->actor=actor;s->rbp=rbp;s->reader=reader;
 uintptr_t vt=0,rvt=0,svt=0,avt=0,clock2=0;int64_t T2=0,dt2=0;uint32_t f2=0;
 if(!actor||!rbp||!reader||!camera_pose_read(rbp-0xe0,&s->provider,8)||!s->provider||
 !camera_pose_read(actor,&avt,8)||avt!=(uintptr_t)image+0x15b3c00||
 !camera_pose_read(s->provider,&vt,8)||vt!=(uintptr_t)image+0x15ee580||
 !camera_pose_read(reader,&rvt,8)||rvt!=(uintptr_t)image+0x15ee6a0||
 !camera_pose_read(reader+0x28,&svt,8)||svt!=rvt||
 !rpq_clock_fields(0x968,&s->clock,&s->T,&s->dt,&s->frame)||
 !rpq_clock_fields(0x970,&clock2,&T2,&dt2,&f2)||clock2!=s->clock||T2!=s->T||dt2!=s->dt||f2!=s->frame||
 !camera_pose_read(s->provider+0xb0,&s->B0,8)||s->B0!=argument||
 !camera_pose_read(s->provider+0xc8,&s->rate,4)||!camera_pose_read(s->provider+0xcc,&s->CC,1)||
 !camera_pose_read(s->provider+0xcd,&s->CD,1)||!camera_pose_read(s->provider+0xce,&s->CE,1)||
 !camera_pose_read(actor+0xe4,&s->E4,1)||s->E4||s->rate!=UINT32_C(0x3f800000)||s->CD||
 !camera_pose_read(reader+0x70,&s->reader70,8)||!camera_pose_read(reader+0x68,&s->reader68,8)||s->reader68<=0||
 !pqn_births(actor,s->provider,reader,s->birth))return 0;
 /* CC=0 is allowed ONLY at actual dt0 after an observed adapted schedule.
  * It never means lifetime retirement or permission to raw-fallback. */
 return s->T>=s->dt&&s->dt>=0&&s->dt<=PQN_Q&&s->CC<=1&&(s->dt==0||s->CC==1);
}
static int pqn_same_scope(const PqnScope*a,const PqnScope*b){
 return a->actor==b->actor&&a->provider==b->provider&&a->reader==b->reader&&a->clock==b->clock&&a->rbp==b->rbp&&
 a->T==b->T&&a->dt==b->dt&&a->frame==b->frame&&a->rate==b->rate&&a->E4==b->E4&&a->CD==b->CD&&a->CE==b->CE&&a->CC==b->CC&&
 a->reader70==b->reader70&&a->reader68==b->reader68&&!memcmp(a->birth,b->birth,sizeof a->birth);
}
static int pqn_adapted(const PqnScope*s){
 int result=-1;AcquireSRWLockExclusive(&pqn_binding_lock);PqnBinding*slot=NULL,*empty=NULL;
 for(unsigned i=0;i<64;++i){PqnBinding*b=pqn_bindings+i;if(b->actor==s->actor){slot=b;break;}if(!b->actor&&!empty)empty=b;}
 if(!slot)slot=empty;
 if(slot){if(slot->actor!=s->actor||slot->provider!=s->provider||slot->reader!=s->reader||slot->clock!=s->clock||memcmp(slot->birth,s->birth,sizeof s->birth)){
 memset(slot,0,sizeof*slot);slot->actor=s->actor;slot->provider=s->provider;slot->reader=s->reader;slot->clock=s->clock;memcpy(slot->birth,s->birth,sizeof s->birth);}
 if(s->dt>0){slot->positive=1;if(s->dt<PQN_Q)slot->adapted=1;}result=slot->positive?(int)slot->adapted:-2;}
 ReleaseSRWLockExclusive(&pqn_binding_lock);return result;
}
static void pqn_trace(unsigned stage,unsigned status,const PqnPair*p,int64_t argument,int64_t selected,uint64_t packet,uintptr_t caller){
 if(!pqn_header)return;PqnRecord r;memset(&r,0,sizeof r);LARGE_INTEGER q;QueryPerformanceCounter(&q);r.qpc=q.QuadPart;r.stage=stage;r.status=status;r.packet=packet;r.thread=GetCurrentThreadId();r.caller=caller;
 if(p){const PqnScope*s=&p->scope;r.pair=p->id;r.actor=s->actor;r.provider=s->provider;r.reader=s->reader;r.clock=s->clock;r.rbp=s->rbp;r.T=s->T;r.dt=s->dt;r.argument=argument;r.selected_argument=selected;r.reader70=s->reader70;r.reader68=s->reader68;r.B0=s->B0;r.frame=s->frame;r.oldK=p->time.original_K;r.effectiveK=p->time.effective_K;r.flags=p->shifted;memcpy(r.birth,s->birth,sizeof r.birth);}
 AcquireSRWLockExclusive(&pqn_tape_lock);unsigned i=pqn_header->count;if(i<pqn_header->capacity){r.sequence=i;pqn_records[i]=r;MemoryBarrier();pqn_header->count=i+1;}else ++pqn_header->dropped;pqn_header->pair_violations=(uint64_t)InterlockedCompareExchange64(&pqn_violations,0,0);ReleaseSRWLockExclusive(&pqn_tape_lock);
}
static void pqn_test_clobber(void){
#ifdef PQN_TEST_CLOBBER
 __asm__ volatile("fninit\n fld1\n fldpi\n vzeroall\n":::"memory");unsigned csr=0x3f80;__asm__ volatile("ldmxcsr %0"::"m"(csr):"memory");
#endif
}
static __attribute__((used,noinline)) ABI void pqn_peek_before(unsigned char*context){
 uintptr_t caller=0,actor=0,rbp=0,reader=0;int64_t argument=0;memcpy(&caller,context+1160,8);memcpy(&actor,context+1056,8);memcpy(&rbp,context+1112,8);memcpy(&reader,context+1096,8);memcpy(&argument,context+1104,8);
 if(pqn_enabled&&caller==(uintptr_t)image+0x123680){
  /* Remove only transient incomplete scopes proven unwound by native stack,
   * or the previous invocation of this same actor native frame. No age/TTL. */
  while(pqn_count&&pqn_pairs[pqn_count-1].scope.rbp<=rbp){if(pqn_pairs[pqn_count-1].shifted)InterlockedIncrement64(&pqn_violations);--pqn_count;}
  if(pqn_count>=8){InterlockedIncrement64(&pqn_violations);pqn_trace(0,8,NULL,argument,argument,0,caller);}
  else{PqnPair*p=pqn_pairs+pqn_count;memset(p,0,sizeof*p);p->id=(uint64_t)InterlockedIncrement64(&pqn_pair_sequence);p->returnpc=caller;
   int valid=pqn_snapshot(actor,rbp,reader,argument,&p->scope);if(valid&&pqn_time_argument(argument,p->scope.reader70,p->scope.T,p->scope.dt,&p->time)){
    int mode=pqn_adapted(&p->scope);if(mode<0){pqn_trace(1,mode==-1?5:6,p,argument,argument,0,caller);}
    else{p->shifted=(unsigned)mode;p->stage=1;++pqn_count;int64_t selected=p->shifted?p->time.normalized_argument:argument;if(p->shifted)memcpy(context+1104,&selected,8);pqn_trace(1,p->shifted?1:0,p,argument,selected,0,caller);}
   }else pqn_trace(1,2,p,argument,argument,0,caller);
  }
 }
 pqn_test_clobber();
}
static __attribute__((used,noinline)) ABI void pqn_main_before(unsigned char*context){
 uintptr_t caller=0,actor=0,rbp=0,reader=0;int64_t argument=0;memcpy(&caller,context+1160,8);memcpy(&actor,context+1056,8);memcpy(&rbp,context+1112,8);memcpy(&reader,context+1096,8);memcpy(&argument,context+1104,8);
 if(pqn_enabled&&caller==(uintptr_t)image+0x123900){PqnPair*p=pqn_count?pqn_pairs+pqn_count-1:NULL;PqnScope now;PqnTime time;
  if(p&&p->stage==1&&pqn_snapshot(actor,rbp,reader,argument,&now)&&pqn_same_scope(&p->scope,&now)&&pqn_time_argument(argument,now.reader70,now.T,now.dt,&time)&&time.shift==p->time.shift){
   p->scope.B0=now.B0;p->time=time;p->returnpc=caller;p->stage=2;int64_t selected=p->shifted?time.normalized_argument:argument;
   if(p->shifted){memcpy(context+1104,&selected,8);uintptr_t post=(uintptr_t)pqn_main_post_bridge;memcpy(context+1160,&post,8);pqn_trace(2,1,p,argument,selected,0,caller);}
   else{pqn_trace(2,0,p,argument,argument,0,caller);memset(p,0,sizeof*p);--pqn_count;}
  }else{if(p&&p->shifted)InterlockedIncrement64(&pqn_violations);pqn_trace(2,3,p,argument,argument,0,caller);if(p&&p->scope.rbp==rbp)--pqn_count;}
 }
 pqn_test_clobber();
}
static __attribute__((used,noinline)) ABI void pqn_main_after(unsigned char*context){
 /* This shim is installed ONLY for exact return123900. Restore that known
  * original return even if an unexpected reentrant error lost TLS, rather
  * than ever returning to this shim again. Such a run is disqualified. */
 uintptr_t nativepc=(uintptr_t)image+0x123900,rbp=0;memcpy(context+1160,&nativepc,8);memcpy(&rbp,context+1112,8);
 PqnPair*p=pqn_count?pqn_pairs+pqn_count-1:NULL;
 if(p&&p->stage==2&&p->scope.rbp==rbp&&p->returnpc==nativepc){pqn_trace(3,p->shifted?1:0,p,p->time.original_argument,p->time.normalized_argument,0,p->returnpc);memset(p,0,sizeof*p);--pqn_count;}
 else{InterlockedIncrement64(&pqn_violations);pqn_trace(3,9,p,0,0,0,nativepc);}
 pqn_test_clobber();
}
static int pqn_native_main_return(uintptr_t returnpc,uintptr_t native_actor_rbp){
 if(returnpc==(uintptr_t)image+0x123900)return 1;
 if(!pqn_enabled||!pqn_count||returnpc!=(uintptr_t)pqn_main_post_bridge)return 0;
 const PqnPair*p=pqn_pairs+pqn_count-1;return p->stage==2&&p->scope.rbp==native_actor_rbp&&p->returnpc==(uintptr_t)image+0x123900;
}
static void pqn_packet_mark(RpkRecord*r){
 if(!pqn_enabled||!pqn_count)return;PqnPair*p=pqn_pairs+pqn_count-1;int64_t query=0;
 if(p->stage==2&&p->shifted&&p->scope.actor==r->actor&&p->scope.provider==r->provider&&p->scope.reader==r->reader&&p->scope.clock==r->clock&&p->scope.T==r->time_before&&p->scope.dt==r->clock_dt_before&&p->scope.frame==r->frame_before&&
 pqn_iadd(r->cursor,p->scope.reader70,&query)&&pqn_iadd(query,p->time.shift,&query)&&query==r->query&&query>0&&query<p->scope.reader68){r->reserved=PQN_TAG;pqn_trace(4,1,p,r->cursor,r->cursor+p->time.shift,r->sequence,r->caller);}
 else if(p->stage==2&&p->shifted){InterlockedIncrement64(&pqn_violations);pqn_trace(4,4,p,r->cursor,r->cursor,r->sequence,r->caller);}
}
/* Full-context bridges generated from the already tested observer convention. */
#include "paired-query-bridges.inc"
static void pqn_jump(unsigned char*p,uintptr_t target){memcpy(p,"\xff\x25\0\0\0\0",6);memcpy(p+6,&target,8);}
static int paired_query_install(void){
 const char*e=camera_fix_getenv("BB_REPLAY_PAIRED_QUERY_Q");if(!e||strcmp(e,"1"))return 0;
 if(!rpl_installed||!rpl_roles_enabled||!rpq_enabled||!rpkt_original){puts("PairedQueryQ: original packet/lifecycle/roles prerequisites unavailable; disabled");return 0;}
 static const unsigned char peek[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x49,0x89,0xce,0x49,0x89,0xd7};
 static const unsigned char main[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
 if(runtime_image_size<0x766431||memcmp(image+0x7662e0,peek,sizeof peek)||memcmp(image+0x766420,main,sizeof main)){puts("PairedQueryQ: native paired signatures unavailable; no partial install");return 0;}
 unsigned char*gate=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!gate)return 0;
 memcpy(gate,peek,sizeof peek);pqn_jump(gate+sizeof peek,(uintptr_t)image+0x7662f1);memcpy(gate+128,main,sizeof main);pqn_jump(gate+128+sizeof main,(uintptr_t)image+0x766431);
 DWORD old;if(!VirtualProtect(gate,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(gate,0,MEM_RELEASE);return 0;}
 /* Both entry sites are in ONE native 4K page. Acquire it once before mutation. */
 if(!VirtualProtect(image+0x766000,4096,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(gate,0,MEM_RELEASE);return 0;}
 pqn_peek_original=(uintptr_t)gate;pqn_main_original=(uintptr_t)gate+128;pqn_jump(image+0x7662e0,(uintptr_t)pqn_peek_bridge);memset(image+0x7662ee,0x90,sizeof peek-14);pqn_jump(image+0x766420,(uintptr_t)pqn_main_bridge);memset(image+0x76642e,0x90,sizeof main-14);
 DWORD unused;VirtualProtect(image+0x766000,4096,old,&unused);FlushInstructionCache(GetCurrentProcess(),gate,256);FlushInstructionCache(GetCurrentProcess(),image+0x766000,4096);
 pqn_enabled=1;puts("PairedQueryQ: scoped marker+whole-pose query RSI phase; native provider untouched; effective cacheK=T; pair violations disqualify experiment");return 1;
}
