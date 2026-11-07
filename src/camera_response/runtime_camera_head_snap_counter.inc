#ifndef CAMERA_HEAD_SNAP_COUNTER_INCLUDED
#define CAMERA_HEAD_SNAP_COUNTER_INCLUDED
/* Isolated diagnostic only. No hooks are installed here. A serialized owner
 * registry must supply HSCState and a proven clock/epoch for each D19140 call.
 * Only positive native activation count33C is retimed. Native caller, mutable
 * lookups, current base, final composition and dt==0 paths still run once. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
typedef void (ABI *HSCNativeUpdate)(void *,float);
typedef struct {
 uint64_t camera_generation,clock_identity,epoch;
 double elapsed_us; /* Authoritative logical elapsed, not wall time or guessed FPS. */
 uint32_t incoming_dt_bits;
 unsigned valid,native_fixed30;
} HSCContext;
typedef struct {
 uint64_t camera_generation,clock_identity,epoch,serial;
 double credit_us;
 int32_t expected_count;
 unsigned valid,busy;
 uint64_t retimed,native_q,zero_dt,invalidated;
} HSCState;
typedef struct {int status;int32_t before,native_after,after;unsigned reset,ticks;} HSCResult;
enum {HSC_NATIVE=0,HSC_RETIMED=1,HSC_NATIVE_Q=2,HSC_ZERO_DT=3,HSC_UNSUPPORTED=-1};
static int hsc_image_supported(const unsigned char*code,size_t size){
 if(size<0xd19d9e)return 0;uint64_t h=UINT64_C(14695981039346656037);
 for(size_t i=0xd19140;i<0xd19d9e;++i)h=(h^code[i])*UINT64_C(1099511628211);
 return h==UINT64_C(0x958d53c9efb1aca8);
}
static int32_t hsc_count(const void*self){int32_t n;memcpy(&n,(const unsigned char*)self+0x33c,4);return n;}
static void hsc_store(void*self,int32_t n){memcpy((unsigned char*)self+0x33c,&n,4);}
/* Invoke before AND after construction/config replacement; before destructor,
 * owner retirement, clock/seek epoch changes. Serial blocks an in-flight old
 * invocation from writing a freshly reconstructed/rebound camera. */
static void hsc_forget(HSCState*s){++s->serial;s->valid=0;s->credit_us=0;++s->invalidated;}
static HSCResult hsc_update(HSCState*s,const HSCContext*c,void*self,float dt,HSCNativeUpdate native){
 HSCResult r={0};uint32_t entry_csr=_mm_getcsr(),bits;memcpy(&bits,&dt,4);
 r.before=hsc_count(self);r.reset=((unsigned char*)self)[0x338]!=0;
 unsigned angular=((unsigned char*)self)[0x33a];
 unsigned nested=s->busy!=0;uint64_t serial=s->serial;
 int context=c&&c->valid&&c->camera_generation&&c->clock_identity&&
  c->incoming_dt_bits==bits&&c->elapsed_us>=0&&c->elapsed_us<=33333&&
  !(bits&0x80000000)&&bits<0x7f800000&&
  ((bits==0)==(c->elapsed_us==0));
 int match=context&&s->valid&&s->camera_generation==c->camera_generation&&
  s->clock_identity==c->clock_identity&&s->epoch==c->epoch&&
  s->expected_count==r.before;
 int start=context&&r.reset&&angular==1;
 int tracked=!nested&&angular==1&&(start||(match&&r.before>=0&&r.before<=3));
 /* An unobserved positive startup count has an unknown fractional age. Do not
  * invent one. First activation/reset supplies the only new cold anchor. */
 if(!tracked){hsc_forget(s);serial=s->serial;}
 ++s->busy;_mm_setcsr(entry_csr);native(self,dt);uint32_t native_csr=_mm_getcsr();--s->busy;
 r.native_after=hsc_count(self);r.after=r.native_after;
 if(serial!=s->serial||nested){r.status=HSC_UNSUPPORTED;_mm_setcsr(native_csr);return r;}
 if(!tracked||((unsigned char*)self)[0x33a]!=1||((unsigned char*)self)[0x338]){
  if(tracked)hsc_forget(s);r.status=HSC_UNSUPPORTED;_mm_setcsr(native_csr);return r;
 }
 int32_t before=start?3:r.before;
 int32_t expected_native=before?before-1:0;
 if(r.native_after!=expected_native){hsc_forget(s);r.status=HSC_UNSUPPORTED;_mm_setcsr(native_csr);return r;}
 if(start){s->camera_generation=c->camera_generation;s->clock_identity=c->clock_identity;
  s->epoch=c->epoch;s->credit_us=0;s->valid=1;}
 if(bits==0){
  /* Preserve the original surprising countdown-on-dt0 behavior while positive,
   * and the separate count0/dt0 snap which does not decrement. Re-anchor age. */
  s->credit_us=0;s->expected_count=r.native_after;++s->zero_dt;r.status=HSC_ZERO_DT;
 }else if(before>0){
  int exact_q=c->native_fixed30&&bits==0x3d08882f&&c->elapsed_us==33333&&s->credit_us==0;
  s->credit_us+=c->elapsed_us;
  while(s->credit_us>=33333){s->credit_us-=33333;++r.ticks;}
  r.after=before-(int32_t)r.ticks;if(r.after<0)r.after=0;
  /* At a Q-aligned native30 update the original count already is exact. No
   * camera store is performed, including on the reset's first 3 -> 2 step. */
  if(!exact_q&&r.after!=r.native_after){hsc_store(self,r.after);++s->retimed;}
  if(exact_q){++s->native_q;r.status=HSC_NATIVE_Q;}else r.status=HSC_RETIMED;
  s->expected_count=r.after;if(!r.after)s->credit_us=0;
 }else{s->expected_count=0;s->credit_us=0;r.status=HSC_NATIVE;}
 _mm_setcsr(native_csr);return r;
}
static __attribute__((unused)) int hsc_requested(void){const char*p=camera_fix_getenv("BB_CAMERA_HEAD_SNAP_COUNTER");return p&&!strcmp(p,"1");}
#endif
