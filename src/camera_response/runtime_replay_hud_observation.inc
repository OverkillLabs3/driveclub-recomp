#include "runtime_hud_full_tape.inc"
/* Isolated candidate replacement for runtime_replay_hud_observation.inc.
 * Native478A20/6CE5B0 math, authored parameters, substeps, clamps and RNG are
 * retained. A private aligned observation supplies causally sampled raw vectors.
 * Moving-input equivalence means the stated piecewise-linear sampled-input
 * contract, not reconstruction of unobserved physics or exact PS4 footage.
 * No shared physics input or global RNG word is ever written by this adapter.
 */
#include <math.h>
#define HUD_REFERENCE_CAPACITY 64
#define HUD_REFERENCE_MAX_TICKS 8
static const float hud_reference_Q=(float)(int64_t)((1.f/30.f)*1000000.f)*1e-6f;
static const int64_t hud_reference_Q_us=(int64_t)((1.f/30.f)*1000000.f);
typedef void(ABI *ReplayHudUpdate)(void*,void*,float);
typedef void(ABI *HudReferenceCtor)(void*);
typedef void*(ABI *HudReferenceResolve)(void*);
typedef void*(ABI *HudReferenceGetObservation)(void*);
static ReplayHudUpdate replay_hud_update_original;
static HudReferenceCtor hud_reference_ctor_original;
static HudReferenceCtor hud_reference_destroy_original;
static int hud_reference_enabled;
typedef struct {
    uintptr_t subject,vehicle,observation,kind_ptr,target_ptr,state_ptr;
    uint32_t kind;
    float vectors[4][4]; /* state+B0, target+140, state+90, state+110 */
} HudReferenceInput;
typedef struct {
    _Alignas(16) unsigned char kind[16];
    _Alignas(16) unsigned char target[0x150];
    _Alignas(16) unsigned char state[0x120];
    uintptr_t descriptor[3];
} HudReferencePrivate;
typedef struct {
    uintptr_t self;uint64_t generation;
    unsigned frame;int frame_known,initialized,busy,has_input;
    double credit;
    unsigned char canonical[0x84];
    HudReferenceInput previous;
} HudReferenceState;
static void hsq_forget(uintptr_t owner);

typedef struct {
    uint64_t native_ticks,constructors,external_resets,subject_resets,frame_resets,
             duplicates,invalid_dt,stalls,overflow,busy_fallbacks,failed_input;
} HudReferenceDiagnostics;
static HudReferenceState hud_reference_states[HUD_REFERENCE_CAPACITY];
static SRWLOCK hud_reference_lock=SRWLOCK_INIT;
static uint64_t hud_reference_generation;
static HudReferenceDiagnostics hud_reference_diagnostics;
#define HUD_REFERENCE_COUNT(member) ((void)InterlockedIncrement64((volatile LONG64*)&hud_reference_diagnostics.member))
/* This TLS pointer is a saved/restored call scope, never persistent history. */
static _Thread_local uintptr_t *hud_reference_supplied_observation;
static __attribute__((used)) uintptr_t hud_reference_lookup_resume;

static int hud_reference_native_frame(unsigned*out){
    uintptr_t a=0;
    return camera_pose_read((uintptr_t)image+0x1911100,&a,8)&&
        camera_pose_read(a+0x968,&a,8)&&camera_pose_read(a+0x1e0,&a,8)&&
        camera_pose_read(a+0x21c,out,4);
}
#ifndef HUD_REFERENCE_GET_FRAME
#define HUD_REFERENCE_GET_FRAME hud_reference_native_frame
#endif
static int hud_reference_native_us(float dt,int64_t*out){
    if(!(dt>0)||!isfinite(dt)||(double)dt>HUD_REFERENCE_MAX_TICKS*(double)hud_reference_Q)return 0;
    int64_t us=(int64_t)llround((double)dt/(double)1e-6f);
    if(us<=0||us>HUD_REFERENCE_MAX_TICKS*hud_reference_Q_us)return 0;
    float forward=(float)us*1e-6f;if(memcmp(&forward,&dt,4))return 0;
    *out=us;return 1;
}
static double hud_reference_units(float dt){
    int64_t us;if(hud_reference_native_us(dt,&us))return(double)us;
    return((double)dt/(double)hud_reference_Q)*(double)hud_reference_Q_us;
}
static void *hud_reference_native_lookup(void *subject){
    HudReferenceResolve resolve=(HudReferenceResolve)(image+0xd60700);
    void *vehicle=resolve(subject);uintptr_t type=0,method=0;
    if(!camera_pose_read((uintptr_t)vehicle,&type,8)||!camera_pose_read(type+0x350,&method,8)||!method)return NULL;
    return((HudReferenceGetObservation)method)(vehicle);
}
static __attribute__((used,noinline)) ABI void *hud_reference_lookup(void *subject){
    if(hud_reference_supplied_observation)return hud_reference_supplied_observation;
    return hud_reference_native_lookup(subject);
}
/* Replaces the complete native lookup sequence, not a renderer or physics API.
 * RSP is16-aligned here; 80 bytes of pushes plus256 bytes of XMM storage retain
 * that alignment for the SysV helper. Its RAX result replaces saved RAX. */
static __attribute__((naked,used)) ABI void hud_reference_lookup_bridge(void){
    __asm__ volatile(
        "pushfq\n pushq %rax\n pushq %rcx\n pushq %rdx\n pushq %rsi\n pushq %rdi\n"
        "pushq %r8\n pushq %r9\n pushq %r10\n pushq %r11\n subq $256,%rsp\n"
        "vmovdqu %xmm0,0(%rsp)\n vmovdqu %xmm1,16(%rsp)\n vmovdqu %xmm2,32(%rsp)\n vmovdqu %xmm3,48(%rsp)\n"
        "vmovdqu %xmm4,64(%rsp)\n vmovdqu %xmm5,80(%rsp)\n vmovdqu %xmm6,96(%rsp)\n vmovdqu %xmm7,112(%rsp)\n"
        "vmovdqu %xmm8,128(%rsp)\n vmovdqu %xmm9,144(%rsp)\n vmovdqu %xmm10,160(%rsp)\n vmovdqu %xmm11,176(%rsp)\n"
        "vmovdqu %xmm12,192(%rsp)\n vmovdqu %xmm13,208(%rsp)\n vmovdqu %xmm14,224(%rsp)\n vmovdqu %xmm15,240(%rsp)\n"
        "callq hud_reference_lookup\n movq %rax,320(%rsp)\n"
        "vmovdqu 0(%rsp),%xmm0\n vmovdqu 16(%rsp),%xmm1\n vmovdqu 32(%rsp),%xmm2\n vmovdqu 48(%rsp),%xmm3\n"
        "vmovdqu 64(%rsp),%xmm4\n vmovdqu 80(%rsp),%xmm5\n vmovdqu 96(%rsp),%xmm6\n vmovdqu 112(%rsp),%xmm7\n"
        "vmovdqu 128(%rsp),%xmm8\n vmovdqu 144(%rsp),%xmm9\n vmovdqu 160(%rsp),%xmm10\n vmovdqu 176(%rsp),%xmm11\n"
        "vmovdqu 192(%rsp),%xmm12\n vmovdqu 208(%rsp),%xmm13\n vmovdqu 224(%rsp),%xmm14\n vmovdqu 240(%rsp),%xmm15\n"
        "addq $256,%rsp\n popq %r11\n popq %r10\n popq %r9\n popq %r8\n popq %rdi\n popq %rsi\n popq %rdx\n popq %rcx\n popq %rax\n popfq\n"
        "jmpq *hud_reference_lookup_resume(%rip)\n");
}
static int hud_reference_capture(void *subject,float dt,HudReferenceInput*out){
    (void)dt;
    memset(out,0,sizeof *out);out->subject=(uintptr_t)subject;
    HudReferenceResolve resolve=(HudReferenceResolve)(image+0xd60700);
    out->vehicle=(uintptr_t)resolve(subject);uintptr_t type=0,method=0;
    if(!camera_pose_read(out->vehicle,&type,8)||!camera_pose_read(type+0x350,&method,8)||!method)return 0;
    out->observation=(uintptr_t)((HudReferenceGetObservation)method)((void*)out->vehicle);
    uintptr_t pointers[3];
    if(!camera_pose_read(out->observation,pointers,24))return 0;
    out->kind_ptr=pointers[0];out->target_ptr=pointers[1];out->state_ptr=pointers[2];
    if(!camera_pose_read(out->kind_ptr,&out->kind,4)||
       !camera_pose_read(out->state_ptr+0xb0,out->vectors[0],16)||
       !camera_pose_read(out->target_ptr+0x140,out->vectors[1],16)||
       !camera_pose_read(out->state_ptr+0x90,out->vectors[2],16)||
       !camera_pose_read(out->state_ptr+0x110,out->vectors[3],16))return 0;
    /* Use current native fields. Replay angular-zero is authored behavior;
       no prepared derivative or frame-label-based latency substitution. */
    /* Native scalar reductions consume xyz. Preserve unused padding/w bits; do
     * not reject otherwise valid physics vectors because padding is nonfinite. */
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<3;++j)if(!isfinite(out->vectors[i][j]))return 0;
    return 1;
}
static int hud_reference_same_input(const HudReferenceInput*a,const HudReferenceInput*b){
    return a->subject==b->subject&&a->vehicle==b->vehicle&&a->observation==b->observation&&
        a->kind_ptr==b->kind_ptr&&a->target_ptr==b->target_ptr&&a->state_ptr==b->state_ptr&&a->kind==b->kind;
}
static void hud_reference_private(HudReferencePrivate*p,const HudReferenceInput*a,const HudReferenceInput*b,double alpha){
    memset(p,0,sizeof *p);memcpy(p->kind,&b->kind,4);
    float vectors[4][4];
    if(alpha<=0)memcpy(vectors,a->vectors,sizeof vectors);
    else if(alpha>=1)memcpy(vectors,b->vectors,sizeof vectors);
    else for(unsigned i=0;i<4;++i){for(unsigned j=0;j<3;++j)
        vectors[i][j]=(float)((double)a->vectors[i][j]+((double)b->vectors[i][j]-(double)a->vectors[i][j])*alpha);
        vectors[i][3]=b->vectors[i][3];}
    memcpy(p->state+0xb0,vectors[0],16);memcpy(p->target+0x140,vectors[1],16);
    memcpy(p->state+0x90,vectors[2],16);memcpy(p->state+0x110,vectors[3],16);
    p->descriptor[0]=(uintptr_t)p->kind;p->descriptor[1]=(uintptr_t)p->target;p->descriptor[2]=(uintptr_t)p->state;
}
static HudReferenceState *hud_reference_find(uintptr_t self,int create){
    HudReferenceState*empty=NULL;
    for(unsigned i=0;i<HUD_REFERENCE_CAPACITY;++i){HudReferenceState*s=&hud_reference_states[i];if(s->self==self)return s;if(!s->self&&!empty)empty=s;}
    if(!create||!empty)return NULL;
    memset(empty,0,sizeof *empty);empty->self=self;empty->generation=++hud_reference_generation;return empty;
}
/* Lifecycle entry points for a native destruction and explicit scene/replay seek
 * notification. No wall-clock heuristic substitutes for an actual replay epoch. */
static void hud_reference_forget(void*self){
    hsq_forget((uintptr_t)self);
    AcquireSRWLockExclusive(&hud_reference_lock);HudReferenceState*s=hud_reference_find((uintptr_t)self,0);
    if(s){memset(s,0,sizeof *s);++hud_reference_generation;}ReleaseSRWLockExclusive(&hud_reference_lock);
}
static ABI void hud_reference_destroy(void*self){
    /* ELF RELA: HUD vtable15D79A0 first slot ->478980. That native destructor
     * is RET plus15bytes of padding; two adjacent class slots share it. Forget
     * only an already tracked address, leaving other classes unregistered. */
    hud_reference_forget(self);hud_reference_destroy_original(self);
}
static __attribute__((unused)) void hud_reference_discontinuity(void){
    hsq_forget(0);
    AcquireSRWLockExclusive(&hud_reference_lock);
    for(unsigned i=0;i<HUD_REFERENCE_CAPACITY;++i)if(hud_reference_states[i].self){HudReferenceState*s=&hud_reference_states[i];s->initialized=0;s->generation=++hud_reference_generation;s->busy=0;}
    ReleaseSRWLockExclusive(&hud_reference_lock);
}
static ABI void hud_reference_constructor(void*self){
    hsq_forget((uintptr_t)self);
    AcquireSRWLockExclusive(&hud_reference_lock);HudReferenceState*s=hud_reference_find((uintptr_t)self,1);
    /* Invalidate BEFORE native construction begins, including identical-value
     * same-address construction. Nested/inflight work must not consume a stale
     * object's next random tick while this constructor is in progress. */
    if(s){s->generation=++hud_reference_generation;s->initialized=0;s->busy=1;}
    ReleaseSRWLockExclusive(&hud_reference_lock);
    hud_reference_ctor_original(self);
    AcquireSRWLockExclusive(&hud_reference_lock);s=hud_reference_find((uintptr_t)self,0);
    if(s){s->generation=++hud_reference_generation;s->initialized=0;s->busy=0;s->has_input=0;s->credit=0;}
    HUD_REFERENCE_COUNT(constructors);ReleaseSRWLockExclusive(&hud_reference_lock);
}
static int hud_reference_token_valid(uintptr_t self,uint64_t generation){
    AcquireSRWLockShared(&hud_reference_lock);HudReferenceState*s=hud_reference_find(self,0);
    int valid=s&&s->generation==generation&&s->busy;ReleaseSRWLockShared(&hud_reference_lock);return valid;
}
#include "hud-shared-q-runtime.inc"
static ABI void hud_reference_update_body(void*self,void*subject,float dt){
    if(hsq_update(self,subject,dt))return;
    if(!hud_reference_enabled){replay_hud_update_original(self,subject,dt);return;}
    if(!(dt>0)||!isfinite(dt)){HUD_REFERENCE_COUNT(invalid_dt);hud_reference_forget(self);replay_hud_update_original(self,subject,dt);return;}
    HudReferenceInput input;
    if(!hud_reference_capture(subject,dt,&input)){
        HUD_REFERENCE_COUNT(failed_input);hud_reference_forget(self);
        return;
    }
    unsigned frame=0;int known=HUD_REFERENCE_GET_FRAME(&frame);
    /* Source-geometry observation is separate from executed canonical steps.
       Do not append a fictitious per-render native HUD math transaction. */
    HudTapeScope observation_only;hud_tape_begin(&observation_only,self,subject,dt);
    hud_tape_input_raw(input.observation,input.target_ptr,input.state_ptr,input.kind,input.vectors);
    hud_tape_current=observation_only.previous;
    HudReferenceState local;
    AcquireSRWLockExclusive(&hud_reference_lock);HudReferenceState*s=hud_reference_find((uintptr_t)self,1);
    if(!s||s->busy){if(s)HUD_REFERENCE_COUNT(busy_fallbacks);else HUD_REFERENCE_COUNT(overflow);
        int native_fallback=!s;ReleaseSRWLockExclusive(&hud_reference_lock);
        if(native_fallback)replay_hud_update_original(self,subject,dt);return;}
    int reset=!s->initialized;
    if(s->initialized&&memcmp(s->canonical,(char*)self+8,0x84)){reset=1;HUD_REFERENCE_COUNT(external_resets);}
    if(s->has_input&&!hud_reference_same_input(&s->previous,&input)){reset=1;HUD_REFERENCE_COUNT(subject_resets);}
    if(!reset&&known&&s->frame_known&&frame==s->frame){HUD_REFERENCE_COUNT(duplicates);
        s->previous=input;ReleaseSRWLockExclusive(&hud_reference_lock);return;}
    if(!reset&&known&&s->frame_known&&frame!=s->frame+1u){reset=1;HUD_REFERENCE_COUNT(frame_resets);}
    if(reset){s->credit=0;s->previous=input;s->has_input=1;memcpy(s->canonical,(char*)self+8,0x84);s->initialized=1;}
    s->frame=frame;s->frame_known=known;s->busy=1;local=*s;
    ReleaseSRWLockExclusive(&hud_reference_lock);
    if((double)dt>HUD_REFERENCE_MAX_TICKS*(double)hud_reference_Q){HUD_REFERENCE_COUNT(stalls);local.previous=input;}
    else {
        double elapsed=hud_reference_units(dt),old_credit=local.credit;
        local.credit+=elapsed;unsigned ticks=0;
        while(local.credit>=hud_reference_Q_us&&ticks<HUD_REFERENCE_MAX_TICKS){
            if(!hud_reference_token_valid((uintptr_t)self,local.generation))return;
            double alpha=((double)hud_reference_Q_us-old_credit+(double)ticks*hud_reference_Q_us)/elapsed;
            HudReferencePrivate supplied;hud_reference_private(&supplied,&local.previous,&input,alpha);
            memcpy((char*)self+8,local.canonical,0x84);
            uintptr_t*saved=hud_reference_supplied_observation;hud_reference_supplied_observation=supplied.descriptor;
            HudTapeScope native_q_tape;hud_tape_begin(&native_q_tape,self,subject,hud_reference_Q);
            float actual_vectors[4][4];
            memcpy(actual_vectors[0],supplied.state+0xb0,16);
            memcpy(actual_vectors[1],supplied.target+0x140,16);
            memcpy(actual_vectors[2],supplied.state+0x90,16);
            memcpy(actual_vectors[3],supplied.state+0x110,16);
            ++hud_tape_private_input;
            hud_tape_input_raw(input.observation,(uintptr_t)supplied.target,(uintptr_t)supplied.state,input.kind,actual_vectors);
            --hud_tape_private_input;
            replay_hud_update_original(self,subject,hud_reference_Q);
            hud_tape_end(&native_q_tape);
            hud_reference_supplied_observation=saved;
            if(!hud_reference_token_valid((uintptr_t)self,local.generation))return;
            memcpy(local.canonical,(char*)self+8,0x84);local.credit-=hud_reference_Q_us;++ticks;
            HUD_REFERENCE_COUNT(native_ticks);
        }
        local.previous=input;local.has_input=1;
    }
    AcquireSRWLockExclusive(&hud_reference_lock);s=hud_reference_find((uintptr_t)self,0);
    if(s&&s->generation==local.generation&&s->busy){local.busy=0;*s=local;}
    ReleaseSRWLockExclusive(&hud_reference_lock);
}
static ABI void replay_hud_update_impl(void*self,void*subject,float dt){
    hud_reference_update_body(self,subject,dt);
}
static ABI void replay_hud_update(void *self,void *subject,float dt){
    if(!camera_accuracy_trace_active()){replay_hud_update_impl(self,subject,dt);return;}
    float audit[24]={0};const unsigned p[3]={0x1c,0x48,0x74};
    for(unsigned i=0;i<3;++i)camera_pose_read((uintptr_t)self+p[i],audit+i,4);
    replay_hud_update_impl(self,subject,dt);
    for(unsigned i=0;i<3;++i)camera_pose_read((uintptr_t)self+p[i],audit+3+i,4);
    audit[14]=1;camera_accuracy_log(1,self,(uintptr_t)subject,0,dt,audit,15);
}
static int replay_hud_observation_patch(void){
    static const unsigned char lookup[]={0xe8,0xbf,0x7c,0x8e,0,0x48,0x8b,0x08,0x48,0x89,0xc7,0xff,0x91,0x50,0x03,0,0};
    if(runtime_image_size<0x478a4d||memcmp(image+0x478a3c,lookup,sizeof lookup))return 0;
    uintptr_t target=(uintptr_t)hud_reference_lookup_bridge;hud_reference_lookup_resume=(uintptr_t)image+0x478a4d;
    const unsigned char jump[]={0x48,0xb8},tail[]={0xff,0xe0};
    memcpy(image+0x478a3c,jump,2);memcpy(image+0x478a3e,&target,8);memcpy(image+0x478a46,tail,2);memset(image+0x478a48,0x90,5);
    FlushInstructionCache(GetCurrentProcess(),image+0x478a3c,sizeof lookup);return 1;
}
static void replay_hud_install(void){
    static const unsigned char entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28};
    static const unsigned char ctor[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x89,0xfb};
    static const unsigned char destroy[]={0xc3,0x66,0x66,0x66,0x66,0x66,0x66,0x2e,0x0f,0x1f,0x84,0,0,0,0,0};
    if(runtime_image_size<0x478a31||memcmp(image+0x478a20,entry,sizeof entry)||memcmp(image+0x478870,ctor,sizeof ctor)||memcmp(image+0x478980,destroy,sizeof destroy))return;
    if(!replay_hud_observation_patch())return;
    hud_reference_ctor_original=(HudReferenceCtor)camera_rate_hook(0x478870,ctor,sizeof ctor,(uintptr_t)hud_reference_constructor);
    hud_reference_destroy_original=(HudReferenceCtor)camera_rate_hook(0x478980,destroy,sizeof destroy,(uintptr_t)hud_reference_destroy);
    replay_hud_update_original=(ReplayHudUpdate)camera_rate_hook(0x478a20,entry,sizeof entry,(uintptr_t)replay_hud_update);
    hud_reference_enabled=hud_reference_ctor_original&&hud_reference_destroy_original&&replay_hud_update_original;
    if(hsq_requested()&&!hsq_install())puts("HUDSharedQ: requested but not installed; existing HUD route retained");
    printf("HUDReference: native30 canonical dynamics %s (authored parameters; causal sampled inputs)\n",hud_reference_enabled?"on":"unavailable");
}
