/* Optional local read-only transaction observer. No native math/input/RNG writes.
 * Requires ABI, image, runtime_image_size, camera_pose_read. No extra getters.
 * BB_CAMERA_LF_TAPE must be an absolute D:\ path. This is not a fix. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
enum { LF_LOGIC_DT=1, LF_INPUT_SCOPE, LF_TARGET_RESULT, LF_DESCRIPTOR_RESULT,
       LF_CTOR, LF_RESET_OWNER, LF_RESET_AGGREGATE, LF_DESTROY_AGGREGATE,
       LF_STEP, LF_COLLISION, LF_SPEED, LF_DIRECT, LF_RANDOM_DIRECT, LF_SCALE,
       LF_GAIN, LF_RANGES, LF_CONFIG };
typedef struct {char magic[8];uint32_t version,record_size,capacity,count,dropped,reserved;uint64_t sequence,qpc_frequency;unsigned char pad[16];} LfTapeHeader;
typedef struct {
    uint64_t sequence,call_id,parent_call,dt_scope,input_scope,owner_generation,owner_generation_current;
    uint64_t self,owner,logic,camera_argument,camera,target,descriptor,arg_pointer,caller,result;
    uint64_t clock,logical_us,update_us,real_us,logical_us_after,update_us_after,real_us_after,qpc_before,qpc_after;
    uint32_t operation,phase,thread,flags,frame218,frame21c,frame218_after,frame21c_after,mxcsr,fcw;
    float scope_dt,args[9],camera_source,source_gain;
    uint32_t descriptor_index,descriptor_kind,name_length,camera_kind;
    uint32_t rng[2];
    unsigned char owner_bytes[0x130],logic_bytes[0x50],target_bytes[0x1a0];
    unsigned char descriptor_bytes[0x48],config_bytes[0x30],name[64];
} LfTapeRecord;
_Static_assert(sizeof(LfTapeHeader)==64,"LF tape header");
_Static_assert(sizeof(LfTapeRecord)==1304,"LF transaction record");
typedef struct LfTapeScope {
    struct LfTapeScope *parent;uint64_t call_id,dt_scope,input_scope;
    unsigned operation,reference_skip,reference_apply;uintptr_t self,owner,logic,camera_argument,camera,target,descriptor,arg_pointer,caller;
    float dt,args[9];uint32_t descriptor_index;uint64_t generation;
} LfTapeScope;
typedef struct {uintptr_t owner;uint64_t generation;int busy;} LfTapeOwner;
static LfTapeHeader *lf_tape_header;static LfTapeRecord *lf_tape_records;
static SRWLOCK lf_tape_lock=SRWLOCK_INIT,lf_tape_owner_lock=SRWLOCK_INIT;
static LfTapeOwner lf_tape_owners[512];static uint64_t lf_tape_generation;
static volatile LONG64 lf_tape_call_sequence,lf_tape_event_sequence;
static _Thread_local LfTapeScope *lf_tape_current;
typedef struct {_Alignas(16)unsigned char bytes[512];} LfTapeFP;
static void lf_tape_fp_save(LfTapeFP *fp){__asm__ volatile("fxsave64 %0":"=m"(*fp)::"memory");}
static void lf_tape_fp_restore(const LfTapeFP *fp){
    __asm__ volatile("fxrstor64 %0"::"m"(*fp):"memory","xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15","st","st(1)","st(2)","st(3)","st(4)","st(5)","st(6)","st(7)");
}
static uint64_t lf_tape_owner_generation(uintptr_t owner,int action){
    if(!owner)return 0;uint64_t generation=0;LfTapeOwner *empty=NULL;
    AcquireSRWLockExclusive(&lf_tape_owner_lock);
    for(unsigned i=0;i<512;++i){LfTapeOwner *o=lf_tape_owners+i;
        if(o->owner==owner){if(action>0)o->generation=++lf_tape_generation;if(action==2)o->busy=1;if(action==3)o->busy=0;generation=o->generation;if(action<0)memset(o,0,sizeof *o);goto done;}
        if(!o->owner&&!empty)empty=o;}
    if(action>0&&empty){empty->owner=owner;empty->generation=++lf_tape_generation;empty->busy=action==2;generation=empty->generation;}
done:ReleaseSRWLockExclusive(&lf_tape_owner_lock);return generation;
}

static int lf_tape_read(uintptr_t p,void *out,size_t size){return p&&camera_pose_read(p,out,size);}
static void lf_tape_snapshot(LfTapeScope *s,unsigned phase,uintptr_t result){
 (void)s;(void)phase;(void)result;
}
/* Caller and result are integers, never encoded in float argument slots. */
typedef struct {LfTapeScope scope;uintptr_t caller;} LfTapeInvocation;
static void lf_tape_begin(LfTapeInvocation *v,unsigned op,void *self,uintptr_t owner,uintptr_t caller){
    LfTapeFP fp;lf_tape_fp_save(&fp);memset(v,0,sizeof *v);LfTapeScope *s=&v->scope;
    s->parent=lf_tape_current;s->call_id=(uint64_t)InterlockedIncrement64(&lf_tape_call_sequence);s->operation=op;s->self=(uintptr_t)self;s->owner=owner;s->caller=caller;v->caller=caller;
    if(s->parent){s->dt_scope=s->parent->dt_scope;s->input_scope=s->parent->input_scope;s->logic=s->parent->logic;s->camera_argument=s->parent->camera_argument;s->camera=s->parent->camera;s->target=s->parent->target;s->descriptor=s->parent->descriptor;s->dt=s->parent->dt;}
    s->generation=lf_tape_owner_generation(owner,0);lf_tape_current=s;lf_tape_fp_restore(&fp);
}
static void lf_tape_capture(LfTapeInvocation *v,unsigned phase,uintptr_t result){
    /* caller is stored in a stable scope side-field; record patch is performed
     * by snapshot's source member, never by rewriting committed tape rows. */
    lf_tape_snapshot(&v->scope,phase,result);
}
static void lf_tape_finish(LfTapeInvocation *v,uintptr_t result){lf_tape_capture(v,2,result);lf_tape_current=v->scope.parent;}
typedef uintptr_t(ABI *LfFn0)(void*);
typedef uintptr_t(ABI *LfFn1)(void*,float);
typedef uintptr_t(ABI *LfFn3)(void*,float,float,float);
typedef uintptr_t(ABI *LfFn6)(void*,float,float,float,float,float,float);
typedef uintptr_t(ABI *LfFn9)(void*,float,float,float,float,float,float,float,float,float);
typedef uintptr_t(ABI *LfFnPointer)(void*,void*);
typedef uintptr_t(ABI *LfFnEnum)(void*,void*,unsigned);
static LfFn1 lf_dt_original,lf_step_original,lf_gain_original;
static LfFnPointer lf_scope_original,lf_config_original;
static LfFn0 lf_target_original,lf_reset_original,lf_aggregate_reset_original,lf_destroy_original;
static LfFn3 lf_collision_original,lf_speed_original,lf_direct_original,lf_random_direct_original,lf_scale_original;
static LfFn6 lf_ranges_original;static LfFn9 lf_ctor_original;static LfFnEnum lf_enum_original;
#define LF_CALLER ((uintptr_t)__builtin_return_address(0)-(uintptr_t)image)
#include "lf-shared-q-runtime.inc"
static uintptr_t ABI lf_dt_trace(void *self,float dt){
    LfTapeInvocation v;lf_tape_begin(&v,LF_LOGIC_DT,self,0,LF_CALLER);v.scope.dt_scope=v.scope.call_id;v.scope.logic=(uintptr_t)self;v.scope.dt=dt;v.scope.args[0]=dt;
    lf_tape_capture(&v,1,0);uintptr_t result=lf_dt_original(self,dt);lf_tape_finish(&v,result);return result;
}
static uintptr_t ABI lf_scope_trace(void *self,void *camera_argument){
    LfTapeInvocation v;lf_tape_begin(&v,LF_INPUT_SCOPE,self,0,LF_CALLER);v.scope.input_scope=v.scope.call_id;v.scope.logic=(uintptr_t)self;v.scope.camera_argument=(uintptr_t)camera_argument;LFQInput saved_q_input=lf_shared_q_selected;LONG64 saved_q_epoch=lf_shared_q_selected_epoch;memset(&lf_shared_q_selected,0,sizeof lf_shared_q_selected);
    lf_tape_read((uintptr_t)camera_argument,&v.scope.camera,8);lf_tape_capture(&v,1,0);uintptr_t result=lf_scope_original(self,camera_argument);lf_tape_finish(&v,result);lf_shared_q_selected=saved_q_input;lf_shared_q_selected_epoch=saved_q_epoch;return result;
}
static uintptr_t ABI lf_target_trace(void *self){
    LfTapeInvocation v;lf_tape_begin(&v,LF_TARGET_RESULT,self,0,LF_CALLER);v.scope.camera=(uintptr_t)self;lf_tape_capture(&v,1,0);
    uintptr_t result=lf_target_original(self),p=0,t=0;
    if(lf_tape_read(result+8,&p,8)&&lf_tape_read(p,&t,8))v.scope.target=t;
    lf_shared_q_target(v.scope.parent,self,t);lf_tape_finish(&v,result);if(v.scope.parent&&v.scope.input_scope)v.scope.parent->target=t;return result;
}
static uintptr_t ABI lf_enum_trace(void *out,void *camera,unsigned index){
    LfTapeInvocation v;lf_tape_begin(&v,LF_DESCRIPTOR_RESULT,out,0,LF_CALLER);v.scope.camera=(uintptr_t)camera;v.scope.descriptor_index=index;v.scope.arg_pointer=(uintptr_t)out;lf_tape_capture(&v,1,0);
    uintptr_t result=lf_enum_original(out,camera,index);lf_tape_read((uintptr_t)out,&v.scope.descriptor,8);
    lf_tape_read(v.scope.descriptor+0x40,&v.scope.owner,8);v.scope.generation=lf_tape_owner_generation(v.scope.owner,0);
    lf_tape_finish(&v,result);if(v.scope.parent&&v.scope.input_scope)v.scope.parent->descriptor=v.scope.descriptor;return result;
}
static uintptr_t ABI lf_ctor_trace(void *self,float a,float b,float c,float d,float e,float f,float g,float h,float i){
    LfTapeInvocation v;lf_tape_begin(&v,LF_CTOR,self,(uintptr_t)self,LF_CALLER);v.scope.generation=lf_tape_owner_generation((uintptr_t)self,2);
    float args[]={a,b,c,d,e,f,g,h,i};memcpy(v.scope.args,args,sizeof args);lf_tape_capture(&v,1,0);
    uintptr_t result=lf_ctor_original(self,a,b,c,d,e,f,g,h,i);v.scope.generation=lf_tape_owner_generation((uintptr_t)self,3);lf_tape_finish(&v,result);return result;
}
static uintptr_t ABI lf_reset_trace(void *self){LfTapeInvocation v;lf_tape_begin(&v,LF_RESET_OWNER,self,(uintptr_t)self,LF_CALLER);v.scope.generation=lf_tape_owner_generation((uintptr_t)self,1);lf_tape_capture(&v,1,0);uintptr_t result=lf_reset_original(self);lf_tape_finish(&v,result);return result;}
static uintptr_t ABI lf_aggregate_reset_trace(void *self){uintptr_t owner=(uintptr_t)self-0x10;if(!lf_tape_owner_generation(owner,0))owner=0;LfTapeInvocation v;lf_tape_begin(&v,LF_RESET_AGGREGATE,self,owner,LF_CALLER);if(owner&&(!v.scope.parent||(v.scope.parent->operation!=LF_RESET_OWNER&&v.scope.parent->operation!=LF_CTOR)))v.scope.generation=lf_tape_owner_generation(owner,1);lf_tape_capture(&v,1,0);uintptr_t result=lf_aggregate_reset_original(self);lf_tape_finish(&v,result);return result;}
static uintptr_t ABI lf_destroy_trace(void *self){uintptr_t owner=(uintptr_t)self-0x10;if(!lf_tape_owner_generation(owner,0))owner=0;LfTapeInvocation v;lf_tape_begin(&v,LF_DESTROY_AGGREGATE,self,owner,LF_CALLER);lf_tape_capture(&v,1,0);uintptr_t result=lf_destroy_original(self);lf_tape_finish(&v,result);if(owner)lf_tape_owner_generation(owner,-1);return result;}

#include "lf_native_clock.h"
#include <xmmintrin.h>
static int lf_native_reference_enabled;
static int lf_reference_hooks_installed;
static uint64_t lf_reference_applied,lf_reference_skipped,lf_reference_fallback;
static LFNativeDecision lf_reference_clock(float dt){
 uintptr_t q=0;int64_t t=0,d=0;
 if(!lf_tape_read((uintptr_t)image+0x1911100,&q,8)||!lf_tape_read(q+0x970,&q,8)||
    !lf_tape_read(q+0x1e0,&q,8)||!lf_tape_read(q+0x58,&t,8)||!lf_tape_read(q+0x60,&d,8))return(LFNativeDecision){0};
 return lf_native_decision(t,d,dt);
}
static int lf_reference_call(unsigned operation,float*dt){
 if(!lf_native_reference_enabled)return 1;
 int step=operation==LF_STEP;
 int input=lf_tape_current&&lf_tape_current->input_scope&&
     (operation==LF_COLLISION||operation==LF_SPEED||operation==LF_GAIN);
 if(!step&&!input)return 1;
 float elapsed=step?*dt:lf_tape_current->dt;
 uint32_t saved_csr=_mm_getcsr();LFNativeDecision d=lf_reference_clock(elapsed);_mm_setcsr(saved_csr);
 if(!d.supported){++lf_reference_fallback;return 1;}
 if(!d.due){++lf_reference_skipped;return 0;}
 if(step&&!d.native_q)*dt=33333.f*1e-6f;
 ++lf_reference_applied;return 1;
}
#define LF_WRAP1(name,operation) static uintptr_t ABI lf_##name##_trace(void*self,float a){LfTapeInvocation v;lf_tape_begin(&v,operation,self,(uintptr_t)self,LF_CALLER);int run=lf_reference_call(operation,&a);if(run&&operation==LF_GAIN)lf_shared_q_gain(v.scope.caller,&a);v.scope.reference_skip=!run;v.scope.reference_apply=lf_native_reference_enabled&&run;v.scope.args[0]=a;lf_tape_capture(&v,1,0);if(!run){lf_tape_finish(&v,0);return 0;}unsigned fixed_before=camera_noise_fixed_step;if(lf_native_reference_enabled)camera_noise_fixed_step=1;uintptr_t result=lf_##name##_original(self,a);camera_noise_fixed_step=fixed_before;lf_tape_finish(&v,result);return result;}
#define LF_WRAP3(name,operation) static uintptr_t ABI lf_##name##_trace(void*self,float a,float b,float c){LfTapeInvocation v;lf_tape_begin(&v,operation,self,(uintptr_t)self,LF_CALLER);v.scope.args[0]=a;v.scope.args[1]=b;v.scope.args[2]=c;float unused_dt=0;int run=lf_reference_call(operation,&unused_dt);if(run){lf_shared_q_args(operation,v.scope.caller,&a,&b,&c);v.scope.args[0]=a;v.scope.args[1]=b;v.scope.args[2]=c;}v.scope.reference_skip=!run;v.scope.reference_apply=lf_native_reference_enabled&&run;lf_tape_capture(&v,1,0);if(!run){lf_tape_finish(&v,0);return 0;}unsigned fixed_before=camera_noise_fixed_step;if(lf_native_reference_enabled)camera_noise_fixed_step=1;uintptr_t result=lf_##name##_original(self,a,b,c);camera_noise_fixed_step=fixed_before;lf_tape_finish(&v,result);return result;}
LF_WRAP1(step,LF_STEP) LF_WRAP1(gain,LF_GAIN)
LF_WRAP3(collision,LF_COLLISION) LF_WRAP3(speed,LF_SPEED) LF_WRAP3(direct,LF_DIRECT) LF_WRAP3(random_direct,LF_RANDOM_DIRECT) LF_WRAP3(scale,LF_SCALE)
static uintptr_t ABI lf_ranges_trace(void*self,float a,float b,float c,float d,float e,float f){LfTapeInvocation v;lf_tape_begin(&v,LF_RANGES,self,(uintptr_t)self,LF_CALLER);float args[]={a,b,c,d,e,f};memcpy(v.scope.args,args,sizeof args);lf_tape_capture(&v,1,0);uintptr_t result=lf_ranges_original(self,a,b,c,d,e,f);lf_tape_finish(&v,result);return result;}
static uintptr_t ABI lf_config_trace(void*self,void*config){uintptr_t owner=(uintptr_t)self-0x10;if(!lf_tape_owner_generation(owner,0))owner=0;LfTapeInvocation v;lf_tape_begin(&v,LF_CONFIG,self,owner,LF_CALLER);v.scope.arg_pointer=(uintptr_t)config;lf_tape_capture(&v,1,0);uintptr_t result=lf_config_original(self,config);lf_tape_finish(&v,result);return result;}
#undef LF_WRAP1
#undef LF_WRAP3
#undef LF_CALLER
/* Register-preserving absolute branch. No generic unrelocated trampoline. */
static size_t lf_tape_jump(unsigned char *out,uintptr_t target){const unsigned char op[]={0xff,0x25,0,0,0,0};memcpy(out,op,6);memcpy(out+6,&target,8);return 14;}
static size_t lf_tape_call(unsigned char *out,uintptr_t target){const unsigned char op[]={0xff,0x15,2,0,0,0,0xeb,8};memcpy(out,op,8);memcpy(out+8,&target,8);return 16;}
typedef struct {size_t offset;unsigned length,special;const unsigned char *bytes;uintptr_t wrapper;void *gateway;} LfTapeHook;
/* special1: JAEx8 at12; special2: addRDI,10 +relative tailJMP;
 * special3: relative CALL at9; special4:JBE at3. */
static void *lf_tape_gateway(const LfTapeHook *h){
    unsigned char *g=VirtualAlloc(NULL,256,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);if(!g)return NULL;size_t at=0;
    if(h->special==1){memcpy(g,h->bytes,12);at=12;g[at++]=0x72;g[at++]=14;at+=lf_tape_jump(g+at,(uintptr_t)image+h->offset+14+(int8_t)h->bytes[13]);}
    else if(h->special==2){memcpy(g,h->bytes,4);int32_t rel;memcpy(&rel,h->bytes+5,4);lf_tape_jump(g+4,(uintptr_t)image+h->offset+9+rel);return g;}
    else if(h->special==3){memcpy(g,h->bytes,9);at=9;int32_t rel;memcpy(&rel,h->bytes+10,4);at+=lf_tape_call(g+at,(uintptr_t)image+h->offset+14+rel);}
    else if(h->special==4){memcpy(g,h->bytes,3);at=3;g[at++]=0x77;g[at++]=14;at+=lf_tape_jump(g+at,(uintptr_t)image+h->offset+5+(int8_t)h->bytes[4]);memcpy(g+at,h->bytes+5,h->length-5);at+=h->length-5;}
    else {memcpy(g,h->bytes,h->length);at=h->length;}
    lf_tape_jump(g+at,(uintptr_t)image+h->offset+h->length);return g;
}
#include "lf-hook-signatures.inc"
static int lf_tape_install_hooks(void){
    for(unsigned i=0;i<sizeof lf_tape_hooks/sizeof *lf_tape_hooks;++i){LfTapeHook *h=lf_tape_hooks+i;
        if(h->offset+h->length>runtime_image_size||memcmp(image+h->offset,h->bytes,h->length))return 0;}
    for(unsigned i=0;i<sizeof lf_tape_hooks/sizeof *lf_tape_hooks;++i){LfTapeHook *h=lf_tape_hooks+i;h->gateway=lf_tape_gateway(h);if(!h->gateway)return 0;}
    lf_tape_assign_originals();
    for(unsigned i=0;i<sizeof lf_tape_hooks/sizeof *lf_tape_hooks;++i){LfTapeHook *h=lf_tape_hooks+i;DWORD old;
        if(!VirtualProtect(image+h->offset,h->length,PAGE_EXECUTE_READWRITE,&old)){
            for(unsigned j=0;j<i;++j){LfTapeHook *prior=lf_tape_hooks+j;DWORD previous;if(VirtualProtect(image+prior->offset,prior->length,PAGE_EXECUTE_READWRITE,&previous)){memcpy(image+prior->offset,prior->bytes,prior->length);DWORD ignored;VirtualProtect(image+prior->offset,prior->length,previous,&ignored);FlushInstructionCache(GetCurrentProcess(),image+prior->offset,prior->length);}}
            return 0;
        }
        lf_tape_jump(image+h->offset,h->wrapper);if(h->length>14)memset(image+h->offset+14,0x90,h->length-14);
        DWORD ignored;VirtualProtect(image+h->offset,h->length,old,&ignored);FlushInstructionCache(GetCurrentProcess(),image+h->offset,h->length);}
    return 1;
}
static __attribute__((unused)) void lf_transaction_tape_install(void){
 const char*reference=camera_fix_getenv("BB_CAMERA_LF_REFERENCE");lf_native_reference_enabled=reference&&!strcmp(reference,"1");
 if(lf_native_reference_enabled)lf_shared_q_install();
 if(lf_reference_hooks_installed)return;
 if(lf_native_reference_enabled){lf_reference_hooks_installed=lf_tape_install_hooks();puts(lf_reference_hooks_installed?"LFReference: common native clock active without diagnostic mapping":"LFReference: hook installation failed");}
}
