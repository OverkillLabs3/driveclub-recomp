/* Optional PRIVATE cache invalidation only. No guest/source data writes.
 * E8 is a record timestamp, never a lifetime token. Source-level events clear
 * all cache owners conservatively; there is no TTL or estimated FPS.
 * Full native execution and all machine state are retained by entry gateways.
 * Readiness bit2 is intentionally NOT raised until real coverage is accepted. */
#ifndef RPL_LIFECYCLE_INC
#define RPL_LIFECYCLE_INC
static int rpl_enabled,rpl_installed;
static volatile LONG64 rpl_calls[18];
typedef struct{unsigned char magic[8];uint32_t version,record_size,capacity,count,dropped,reserved;uint64_t qpc_frequency,reserved64[3];} RplHeader;
typedef struct{uint64_t sequence,qpc,owner,clear_actor,clock,T,dt,vptr,native_rsi;uint32_t stage,thread,frame,flags;} RplRecord;
_Static_assert(sizeof(RplHeader)==64,"RPL header64");
_Static_assert(sizeof(RplRecord)==88,"RPL record88");
static RplHeader*rpl_header;static RplRecord*rpl_records;static SRWLOCK rpl_tape_lock=SRWLOCK_INIT;

static void rpl_trace(unsigned stage,uintptr_t owner,uint64_t native_rsi){
 (void)stage;(void)owner;(void)native_rsi;
}
static void rpl_tape_open(void){}
#include "runtime-role-registry.inc"
static __attribute__((used,noinline))ABI void rpl_capture(unsigned stage,uintptr_t owner,uint64_t native_rsi){
 if(stage>=18)return;
 InterlockedIncrement64(&rpl_calls[stage]);
 uint64_t role_token=rpl_roles_begin(stage,owner);
 lsqt_role_event(stage,owner);
 rpq_actor_invalidate(stage>=8?0:owner);
 rpl_roles_end(stage,owner,role_token);
 rpl_trace(stage,owner,native_rsi);
}
#include "lifecycle-bridges.inc"
typedef struct{uintptr_t site,bridge,*resume,rip_target;const unsigned char*signature;size_t size;unsigned kind,rip_offset;} RplHook;
static RplHook rpl_hooks[18];
static void rpl_jump(unsigned char*p,uintptr_t target){memcpy(p,"\xff\x25\0\0\0\0",6);memcpy(p+6,&target,8);}
static size_t rpl_gateway(unsigned char*p,const RplHook*h){
 size_t n=0;
 if(h->kind==1){
  memcpy(p,h->signature,h->rip_offset);n=h->rip_offset;
  memcpy(p+n,"\x48\xb8",2);uintptr_t v=(uintptr_t)image+h->rip_target;memcpy(p+n+2,&v,8);n+=10;
  memcpy(p+n,h->signature+h->rip_offset+7,h->size-h->rip_offset-7);n+=h->size-h->rip_offset-7;
 }else{memcpy(p,h->signature,h->size);n=h->size;}
 if(h->kind!=2){rpl_jump(p+n,(uintptr_t)image+h->site+h->size);n+=14;}
 return n;
}
static int rpl_install(void){
 const char*e=camera_fix_getenv("BB_REPLAY_PACKET_LIFECYCLE");if(!e||strcmp(e,"1"))return 0;
 if(rpl_installed)return 1;
 #include "lifecycle-spec.inc"
 for(unsigned i=0;i<18;++i){RplHook*h=&rpl_hooks[i];if(runtime_image_size<h->site+h->size||memcmp(image+h->site,h->signature,h->size)){printf("ReplayPacketLifecycle: unsupported hook %u; not installed\n",i);return 0;}}
 unsigned char*c=VirtualAlloc(NULL,18*128,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!c)return 0;
 for(unsigned i=0;i<18;++i){rpl_gateway(c+128*i,&rpl_hooks[i]);*rpl_hooks[i].resume=(uintptr_t)c+128*i;}
 DWORD old;if(!VirtualProtect(c,18*128,PAGE_EXECUTE_READ,&old))return 0;
 for(unsigned i=0;i<18;++i){RplHook*h=&rpl_hooks[i];if(!VirtualProtect(image+h->site,h->size,PAGE_EXECUTE_READWRITE,&old))return 0;rpl_jump(image+h->site,h->bridge);memset(image+h->site+14,0x90,h->size-14);DWORD unused;VirtualProtect(image+h->site,h->size,old,&unused);FlushInstructionCache(GetCurrentProcess(),image+h->site,h->size);}
 FlushInstructionCache(GetCurrentProcess(),c,18*128);rpl_tape_open();rpl_enabled=1;rpl_installed=1;rpl_roles_install();
 puts("ReplayPacketLifecycle: actor ctor/dtors/registration/events and source ctor/dtor/load/reader invalidation active; readiness2 remains blocked pending actual coverage");
 return 1;
}
#endif
