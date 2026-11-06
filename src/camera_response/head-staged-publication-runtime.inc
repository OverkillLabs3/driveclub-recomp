/* Optional three-stage WRITE prototype. It cannot install or publish unless a
 * separate proved producer/accessor registers ALL required readiness bits.
 * No numeric/input policy lives here. Pure native30 must return bypass. */
static HtpProducer htp_producer;static HtpForget htp_forget;static HtpAccessor htp_accessor;static void*htp_context;
static unsigned htp_proof;static int htp_installed;static volatile LONG htp_poison;
static SRWLOCK htp_epoch_lock=SRWLOCK_INIT;static volatile LONG64 htp_epoch=1;
static volatile LONG htp_reset_active;static _Thread_local unsigned htp_reset_depth;
static _Thread_local struct{HtpTransaction transaction;int leased;DWORD thread;}htp_tls;
static __attribute__((used))uintptr_t htp_original1,htp_original2,htp_original3;
static volatile LONG64 htp_applied[3],htp_bypassed,htp_invalid;
typedef uintptr_t(ABI*HtpReset)(void*);
static HtpReset htp_reset_original;
static __attribute__((unused)) int htp_functional_current(uintptr_t actor,uintptr_t frame,uint32_t native_frame,uint32_t thread,const void**head,const void**primitive,uint64_t*epoch,uint64_t*serial){
 const HKTRecord*h=NULL;const HprRecord*p=NULL;
 if(!hpr_group_current(actor,frame,native_frame,thread,&h,&p,epoch,serial))return 0;
 *head=h;*primitive=p;return 1;
}
static uint64_t htp_source_epoch(uintptr_t actor){(void)actor;
 if(InterlockedCompareExchange(&htp_reset_active,0,0))return 0;
 uint64_t e=(uint64_t)InterlockedCompareExchange64(&htp_epoch,0,0);
 return InterlockedCompareExchange(&htp_reset_active,0,0)?0:e;
}
static int htp_requested(void){const char*p=camera_fix_getenv("BB_CAMERA_TEMPORAL_GROUP_WRITE");return !htp_poison&&p&&!strcmp(p,"1")&&htp_producer&&htp_accessor&&(htp_proof&7)==7;}
static __attribute__((unused)) int htp_register_producer(HtpProducer f,HtpForget forget,HtpAccessor access,void*context,unsigned proof){
 if(htp_installed||!f||!forget||!access||(proof&7)!=7)return 0;
 AcquireSRWLockExclusive(&htp_epoch_lock);htp_producer=f;htp_forget=forget;htp_accessor=access;htp_context=context;htp_proof=proof;ReleaseSRWLockExclusive(&htp_epoch_lock);return 1;
}
static void htp_reset_begin(uintptr_t owner){
 if(htp_reset_depth++){if(htp_forget)htp_forget(htp_context,owner);return;}
 /* No same-thread reset may occur inside this checked PURE composition span.
  * Do not invent partial rollback after any bank publication. */
 if(htp_tls.leased){InterlockedExchange(&htp_poison,1);puts("HeadGroup: reentrant reset violated pure-span contract");__builtin_trap();}
 AcquireSRWLockExclusive(&htp_epoch_lock);InterlockedExchange(&htp_reset_active,1);InterlockedIncrement64(&htp_epoch);
 if(htp_forget)htp_forget(htp_context,owner);
}
static void htp_reset_end(void){
 if(!htp_reset_depth){InterlockedExchange(&htp_poison,1);return;}
 if(--htp_reset_depth)return;
 InterlockedIncrement64(&htp_epoch);InterlockedExchange(&htp_reset_active,0);ReleaseSRWLockExclusive(&htp_epoch_lock);
}
static ABI uintptr_t htp_reset(void*self){
 htp_reset_begin((uintptr_t)self);uintptr_t result=htp_reset_original(self);htp_reset_end();return result;
}
static void htp_release(void){if(htp_tls.leased){htp_tls.leased=0;ReleaseSRWLockShared(&htp_epoch_lock);}memset(&htp_tls.transaction,0,sizeof htp_tls.transaction);}
static void htp_invalid_transaction(void){
 InterlockedIncrement64(&htp_invalid);InterlockedExchange(&htp_poison,1);
 /* This is a detected integration failure, NEVER a successful native fallback.
  * Verified entry/whole-span/epoch lease makes it unreachable on a valid call. */
 htp_release();puts("HeadGroup: staged identity/order violation; future publication disabled");
}
static void htp_commit_trace(const HtpTransaction*t){
 if(!camera_accuracy_trace_active())return;
 const HtpKey*k=&t->selected.key;float data[24]={0};
 data[0]=(float)t->stage;data[1]=(float)htp_poison;data[2]=(float)htp_tls.leased;data[3]=(float)k->domain;
 memcpy(data+4,&k->Q,8);memcpy(data+6,&k->epoch,8);memcpy(data+8,&k->binding_token,8);
 memcpy(data+10,&k->main_frame,8);memcpy(data+12,&k->clock,8);
 for(unsigned i=0;i<3;++i){int64_t n=InterlockedCompareExchange64(htp_applied+i,0,0);memcpy(data+14+i*2,&n,8);}
 int64_t invalid=InterlockedCompareExchange64(&htp_invalid,0,0);memcpy(data+20,&invalid,8);
 data[22]=(float)k->main_bl;data[23]=(float)t->selected.ready;
 /* Existing optional sink only; no producer, getter or diagnostic file. The
  * shared epoch lease is still held, and native bridge restores ALL FP/flags. */
 camera_accuracy_log(48,(void*)k->owner,k->clock,k->native_frame,0,data,24);
}
static int htp_scope(uintptr_t actor,uintptr_t frame){uintptr_t caller=0;
 return actor>=0x10000&&frame>=0x10000&&camera_pose_read(frame+8,&caller,8)&&caller==(uintptr_t)image+0xb959f9;
}
static __attribute__((used,noinline))ABI void htp_before(unsigned stage,unsigned char*c){
 uintptr_t actor=0,frame=0;memcpy(&actor,c+1040,8);memcpy(&frame,c+1120,8);
 if(stage==1){
  if(!htp_requested()||!htp_scope(actor,frame)){InterlockedIncrement64(&htp_bypassed);return;}
  if(htp_tls.leased){htp_invalid_transaction();return;}
  AcquireSRWLockShared(&htp_epoch_lock);htp_tls.leased=1;htp_tls.thread=GetCurrentThreadId();
  const void*hkt=NULL,*primitive=NULL;uint64_t epoch=0,serial=0;
  /* Exact current source record and common helper transaction are supplied by
   * the proved headerless collector; no older latest-by-owner lookup or TTL. */
  if(hkt_pending.depth>=4||hkt_pending.extra_depth){htp_release();InterlockedIncrement64(&htp_bypassed);return;}
  const HKTRecord*r=hkt_pending.records+hkt_pending.depth;uint32_t thread=GetCurrentThreadId();
  /* hkt_after has popped exactly this completed slot. It remains immutable
   * until another native HKT entry, which the functional collector also
   * rejects by its before/after serial and epoch checks. */
  if(r->actor!=actor||r->frame_address!=frame||r->thread!=thread||
     !htp_accessor(actor,frame,r->frame,thread,&hkt,&primitive,&epoch,&serial)||!hkt||!primitive||!epoch||epoch!=htp_source_epoch(actor)){
   htp_release();InterlockedIncrement64(&htp_bypassed);return;}
  r=hkt;const HprRecord*p=primitive;HtpRequest request;memset(&request,0,sizeof request);
  request.key.owner=actor;request.key.main_frame=frame;request.key.clock=p->helper_clock;request.key.epoch=epoch;
  request.key.native_frame=r->frame;request.key.thread=thread;memcpy(&request.key.main_bl,c+1128,1);
  request.logical_T=p->helper_logical_T;request.logical_dt=p->helper_logical_dt;request.hkt=hkt;request.primitives=primitive;request.head_serial=serial;
  HtpSelection out;memset(&out,0,sizeof out);int selected=htp_producer(htp_context,&request,&out);
  if(selected!=HTP_SELECTED||!out.ready||out.native30_bypass){htp_release();InterlockedIncrement64(&htp_bypassed);return;}
  HtpKey actual=out.key;actual.owner=actor;actual.main_frame=frame;actual.clock=request.key.clock;actual.epoch=epoch;actual.native_frame=r->frame;actual.thread=thread;actual.main_bl=request.key.main_bl;
  if(htp_publish1(&htp_tls.transaction,&out,&actual,(unsigned char*)actor,(unsigned char*)frame)!=HTP_SELECTED){htp_release();InterlockedIncrement64(&htp_invalid);return;}
  InterlockedIncrement64(htp_applied);return;
 }
 if(!htp_tls.leased)return;
 HtpKey actual=htp_tls.transaction.selected.key;actual.owner=actor;actual.main_frame=frame;actual.thread=GetCurrentThreadId();
 if(stage==2)memcpy(&actual.main_bl,c+1128,1);
 if(!htp_scope(actor,frame)||actual.thread!=htp_tls.thread||actual.epoch!=htp_source_epoch(actor)){htp_invalid_transaction();return;}
 if(stage==2){unsigned char ymm[16][32];memcpy(ymm,c+512,sizeof ymm);
  if(htp_publish2(&htp_tls.transaction,&actual,(unsigned char*)actor,ymm)!=HTP_SELECTED){htp_invalid_transaction();return;}
  memcpy(c+512,ymm,sizeof ymm);InterlockedIncrement64(htp_applied+1);return;
 }
 if(stage==3){if(htp_publish3(&htp_tls.transaction,&actual,(unsigned char*)actor)!=HTP_SELECTED){htp_invalid_transaction();return;}
  InterlockedIncrement64(htp_applied+2);htp_commit_trace(&htp_tls.transaction);htp_release();return;}
 htp_invalid_transaction();
}
#include "head-staged-publication-bridges.inc"
#include "head-staged-publication-install.inc"
