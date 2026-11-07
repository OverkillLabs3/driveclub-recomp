static void camera_reference_trace_after(void*,uintptr_t,float,const unsigned char*,const CameraReferenceInput*,CRSState,float,int,const CRSOutput*,uint64_t);

/* Isolated helmet/head-view six-spring candidate. Included after the common
 * original force/step pointers exist. D2EA50 itself still runs every real update,
 * so view-base sampling and final native matrix composition remain VRR.
 * Only scalar force+spring records are clocked canonically, with a precise
 * piecewise-linear observed native-force contract. No gain changes, no RNG
 * rewind and no assumption about unobserved nonlinear motion between samples.
 */
#define CHR_CELLS 192
static const unsigned chr_offsets[6]={0x70,0x9c,0xc8,0xf4,0x120,0x14c};
static const uintptr_t chr_force_returns[6]={0xd2ec03,0xd2ec1f,0xd2ec4f,0xd2ec6b,0xd2ec83,0xd2eca4};
static const uintptr_t chr_step_returns[6]={0xd2ecb2,0xd2ecc3,0xd2ecd4,0xd2ece1,0xd2ecee,0xd2ecfb};
typedef struct {double time,time_compensation;unsigned count,frame;uintptr_t binding,global_clock;int64_t global_time_us;CRSForceSample points[128];} CHRHistory;
typedef struct {uintptr_t owner,spring;uint64_t generation,revision;int busy;CHRHistory history;CRSAdapter adapter;} CHRCell;
typedef struct {void*self;float dt,force,post_v;unsigned char before[0x2c];int ready;} CHRInput;
typedef struct {void*owner;uintptr_t binding;unsigned frame;int known;CHRInput inputs[6];} CHRScope;
static _Thread_local CHRScope *chr_scope;
static CHRCell chr_cells[CHR_CELLS];static SRWLOCK chr_lock=SRWLOCK_INIT;
static uint64_t chr_generation,chr_revision;
typedef void(ABI *CHRUpdate)(void*,float);
typedef void(ABI *CHRLife)(void*);
static CHRUpdate chr_update_original;static CHRLife chr_config_original,chr_destructor_original;
static __attribute__((used))uintptr_t chr_constructor_resume;
static int chr_enabled;
typedef struct {uint64_t applied,native,warming,unsupported,stale,exhausted,constructors,resets,destructors;} CHRDiagnostics;
static CHRDiagnostics chr_diagnostics;
#define CHR_COUNT(member) ((void)InterlockedIncrement64((volatile LONG64*)&chr_diagnostics.member))
static void camera_helmet_invalidate(uintptr_t owner){AcquireSRWLockExclusive(&chr_lock);for(unsigned i=0;i<CHR_CELLS;++i)if(chr_cells[i].owner==owner)memset(chr_cells+i,0,sizeof chr_cells[i]);ReleaseSRWLockExclusive(&chr_lock);}
static __attribute__((unused))void camera_helmet_reference_discontinuity(void){AcquireSRWLockExclusive(&chr_lock);memset(chr_cells,0,sizeof chr_cells);ReleaseSRWLockExclusive(&chr_lock);}
static int chr_native_frame(unsigned*out){uintptr_t a=0;return camera_pose_read((uintptr_t)image+0x1911100,&a,8)&&camera_pose_read(a+0x968,&a,8)&&camera_pose_read(a+0x1e0,&a,8)&&camera_pose_read(a+0x21c,out,4);}
#ifndef CHR_GET_FRAME
#define CHR_GET_FRAME chr_native_frame
#endif
static int chr_site(void*self,uintptr_t caller,int force){if(!chr_enabled||!chr_scope||!chr_scope->known)return -1;for(unsigned i=0;i<6;++i)if(caller==(force?chr_force_returns[i]:chr_step_returns[i])&&(uintptr_t)self==(uintptr_t)chr_scope->owner+chr_offsets[i])return(int)i;return -1;}
static ABI void camera_helmet_update(void*self,float dt){
    CHRScope local={0};local.owner=self;local.known=CHR_GET_FRAME(&local.frame);camera_pose_read((uintptr_t)self+0x38,&local.binding,8);
    CHRScope*saved=chr_scope;chr_scope=&local;chr_update_original(self,dt);chr_scope=saved;
}
static float camera_helmet_force_at(void*self,float f,float dt,unsigned axis){
    CHRInput*input=chr_scope->inputs+axis;memset(input,0,sizeof *input);input->self=self;input->dt=dt;input->force=f;
    input->ready=camera_pose_read((uintptr_t)self,input->before,sizeof input->before);
    float result=camera_reference_force_original(self,f,dt);if(input->ready)input->ready=camera_pose_read((uintptr_t)self+0x10,&input->post_v,4);return result;
}
static int chr_history_force(void*context,double t,float*out){
    CHRHistory*h=context;if(!h->count)return 0;double eps=64*DBL_EPSILON*fmax(1.,h->time);
    if(t<h->points[0].time-eps||t>h->time+eps)return 0;
    if(t<=h->points[0].time){*out=h->points[0].force;return 1;}
    for(unsigned i=1;i<h->count;++i)if(t<=h->points[i].time){CRSForceSample a=h->points[i-1],b=h->points[i];double u=(t-a.time)/(b.time-a.time);*out=(float)(a.force+((double)b.force-a.force)*u);return isfinite(*out);}
    *out=h->points[h->count-1].force;return isfinite(*out);
}
static int chr_history_force_global(void*context,int64_t endpoint,int64_t interval,float*out){(void)interval;return chr_history_force(context,(double)crs_T*((double)endpoint/33333.),out);}
static float camera_helmet_step_at(void*self,float dt,unsigned axis){
    CHRInput input=chr_scope->inputs[axis];chr_scope->inputs[axis].ready=0;
    unsigned char before[0x2c];int readable=camera_pose_read((uintptr_t)self,before,sizeof before);
    float result=camera_dynamics_spring_original(self,dt);float native_return=result;CRSState native_after={NAN,NAN};
    readable=readable&&camera_pose_read((uintptr_t)self+0x14,&native_after.x,4)&&camera_pose_read((uintptr_t)self+0x10,&native_after.v,4);
    /* Explicit BB_CAMERA_REFERENCE_HELMET controls this private adapter; native30 still passes through. */
    HCSClock global_clock=hcs_read_clock(chr_scope->frame,dt);if(!global_clock.valid){camera_helmet_invalidate((uintptr_t)chr_scope->owner);CHR_COUNT(unsupported);return result;}
    if(!readable||!input.ready||input.self!=self||memcmp(&input.dt,&dt,4)||input.post_v!=crs_float(before,0x10)||
       crs_float(input.before,0x14)!=crs_float(before,0x14)||!crs_settings_same(input.before,before)||!crs_supported(before)||!isfinite(input.force)||!(dt>0)||global_clock.dt_us>8*33333){camera_helmet_invalidate((uintptr_t)chr_scope->owner);CHR_COUNT(unsupported);return result;}
    CHRCell working;unsigned index=CHR_CELLS;
    AcquireSRWLockExclusive(&chr_lock);
    for(unsigned i=0;i<CHR_CELLS;++i)if(chr_cells[i].owner==(uintptr_t)chr_scope->owner&&chr_cells[i].spring==(uintptr_t)self){index=i;break;}
    if(index==CHR_CELLS)for(unsigned i=0;i<CHR_CELLS;++i)if(!chr_cells[i].owner){index=i;break;}
    if(index==CHR_CELLS){ReleaseSRWLockExclusive(&chr_lock);CHR_COUNT(exhausted);return result;}
    CHRCell*c=chr_cells+index;
    if(!c->owner){c->owner=(uintptr_t)chr_scope->owner;c->spring=(uintptr_t)self;c->generation=(uint64_t)InterlockedIncrement64((volatile LONG64*)&chr_generation);}
    if(c->busy){c->generation=(uint64_t)InterlockedIncrement64((volatile LONG64*)&chr_generation);c->busy=0;c->adapter.initialized=0;ReleaseSRWLockExclusive(&chr_lock);CHR_COUNT(stale);return result;}
    c->busy=1;c->revision=++chr_revision;working=*c;ReleaseSRWLockExclusive(&chr_lock);
    CHRHistory*h=&working.history;
    int reset=h->global_clock!=global_clock.clock||(h->count&&h->frame!=chr_scope->frame&&h->global_time_us+global_clock.dt_us!=global_clock.time_us)||h->binding!=chr_scope->binding||(h->count&&h->frame!=chr_scope->frame&&h->frame+1u!=chr_scope->frame);
    if(reset){memset(h,0,sizeof *h);h->binding=chr_scope->binding;working.generation=(uint64_t)InterlockedIncrement64((volatile LONG64*)&chr_generation);working.adapter.initialized=0;}
    if(!h->count){
        /* The first observed native scalar is the explicit cold-start hold over
           its own first elapsed interval. Anchor BEFORE that original force;
           never warm the canonical state from an already VRR-integrated state. */
        h->global_clock=global_clock.clock;h->points[h->count++]=(CRSForceSample){(double)crs_T*((double)(global_clock.time_us-global_clock.dt_us)/33333.),input.force};h->time=(double)crs_T*((double)global_clock.time_us/33333.);h->global_time_us=global_clock.time_us;
        h->points[h->count++]=(CRSForceSample){h->time,input.force};h->frame=chr_scope->frame;
    }else if(h->frame!=chr_scope->frame){h->time=(double)crs_T*((double)global_clock.time_us/33333.);h->global_time_us=global_clock.time_us;if(h->count==128){memmove(h->points,h->points+1,127*sizeof h->points[0]);--h->count;}h->points[h->count++]=(CRSForceSample){h->time,input.force};h->frame=chr_scope->frame;}
    CRSOutput out;int adapted=0;
    if(dt==crs_T&&!working.adapter.initialized){CHR_COUNT(native);}
    else if(h->count<2){working.adapter.initialized=0;CHR_COUNT(warming);}
    else adapted=crs_render_global(&working.adapter,working.generation,chr_scope->frame,before,
        (CRSState){crs_float(input.before,0x14),crs_float(input.before,0x10)},dt,input.force,
        (CRSOps){camera_reference_force_original,camera_dynamics_spring_original},&out,0,chr_history_force_global,h,working.generation,&global_clock);
    AcquireSRWLockExclusive(&chr_lock);c=chr_cells+index;CRSState current={NAN,NAN};unsigned frame=0;uintptr_t binding=0;
    int valid=c->owner==working.owner&&c->spring==working.spring&&c->revision==working.revision&&c->busy&&
      camera_pose_read((uintptr_t)self+0x14,&current.x,4)&&camera_pose_read((uintptr_t)self+0x10,&current.v,4)&&crs_same(current,native_after)&&
      CHR_GET_FRAME(&frame)&&frame==chr_scope->frame&&camera_pose_read((uintptr_t)chr_scope->owner+0x38,&binding,8)&&binding==chr_scope->binding;
    if(valid){working.busy=0;*c=working;if(adapted){memcpy((char*)self+0x14,&out.display.stored.x,4);memcpy((char*)self+0x10,&out.display.stored.v,4);result=out.display.returned;CHR_COUNT(applied);}}
    else if(c->owner==working.owner&&c->spring==working.spring&&c->revision==working.revision){c->busy=0;c->adapter.initialized=0;c->history.count=0;++c->revision;}
    ReleaseSRWLockExclusive(&chr_lock);if(!valid)CHR_COUNT(stale);
    if(valid&&camera_reference_trace_enabled){uint32_t saved_csr;__asm__ volatile("stmxcsr %0":"=m"(saved_csr));CameraReferenceInput trace={0};trace.self=self;trace.camera=chr_scope->owner;trace.frame=chr_scope->frame;trace.dt=dt;trace.force=input.force;trace.x=crs_float(input.before,0x14);trace.v=crs_float(input.before,0x10);trace.post_v=input.post_v;camera_reference_trace_after(self,chr_step_returns[axis],dt,before,&trace,native_after,native_return,adapted?1:0,adapted?&out:NULL,working.generation);__asm__ volatile("ldmxcsr %0"::"m"(saved_csr):"memory");}
    return result;
}
static __attribute__((used,noinline))ABI void camera_helmet_constructor_before(void*self){camera_helmet_invalidate((uintptr_t)self);CHR_COUNT(constructors);}
static __attribute__((naked,used))ABI void camera_helmet_constructor_bridge(void){
    __asm__ volatile("pushfq\n pushq %rax\n pushq %rcx\n pushq %rdx\n pushq %rsi\n pushq %rdi\n pushq %r8\n pushq %r9\n pushq %r10\n pushq %r11\n subq $264,%rsp\n"
    "vmovdqu %xmm0,0(%rsp)\n vmovdqu %xmm1,16(%rsp)\n vmovdqu %xmm2,32(%rsp)\n vmovdqu %xmm3,48(%rsp)\n vmovdqu %xmm4,64(%rsp)\n vmovdqu %xmm5,80(%rsp)\n vmovdqu %xmm6,96(%rsp)\n vmovdqu %xmm7,112(%rsp)\n"
    "vmovdqu %xmm8,128(%rsp)\n vmovdqu %xmm9,144(%rsp)\n vmovdqu %xmm10,160(%rsp)\n vmovdqu %xmm11,176(%rsp)\n vmovdqu %xmm12,192(%rsp)\n vmovdqu %xmm13,208(%rsp)\n vmovdqu %xmm14,224(%rsp)\n vmovdqu %xmm15,240(%rsp)\n"
    "callq camera_helmet_constructor_before\n"
    "vmovdqu 0(%rsp),%xmm0\n vmovdqu 16(%rsp),%xmm1\n vmovdqu 32(%rsp),%xmm2\n vmovdqu 48(%rsp),%xmm3\n vmovdqu 64(%rsp),%xmm4\n vmovdqu 80(%rsp),%xmm5\n vmovdqu 96(%rsp),%xmm6\n vmovdqu 112(%rsp),%xmm7\n"
    "vmovdqu 128(%rsp),%xmm8\n vmovdqu 144(%rsp),%xmm9\n vmovdqu 160(%rsp),%xmm10\n vmovdqu 176(%rsp),%xmm11\n vmovdqu 192(%rsp),%xmm12\n vmovdqu 208(%rsp),%xmm13\n vmovdqu 224(%rsp),%xmm14\n vmovdqu 240(%rsp),%xmm15\n"
    "addq $264,%rsp\n popq %r11\n popq %r10\n popq %r9\n popq %r8\n popq %rdi\n popq %rsi\n popq %rdx\n popq %rcx\n popq %rax\n popfq\n jmpq *chr_constructor_resume(%rip)\n");
}
static ABI void camera_helmet_config(void*self){camera_helmet_invalidate((uintptr_t)self);chr_config_original(self);camera_helmet_invalidate((uintptr_t)self);CHR_COUNT(resets);}
static ABI void camera_helmet_destructor(void*self){camera_helmet_invalidate((uintptr_t)self);chr_destructor_original(self);CHR_COUNT(destructors);}
static int chr_ctor_hook(const unsigned char*expected){
    unsigned char*code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!code)return 0;memcpy(code,expected,10);
    uintptr_t call=runtime_image_base+0x531b60,resume=runtime_image_base+0xd2c5bf;memcpy(code+10,"\x49\xbb",2);memcpy(code+12,&call,8);memcpy(code+20,"\x41\xff\xd3\x49\xbb",5);memcpy(code+25,&resume,8);memcpy(code+33,"\x41\xff\xe3",3);
    DWORD old;if(!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(code,0,MEM_RELEASE);return 0;}FlushInstructionCache(GetCurrentProcess(),code,36);chr_constructor_resume=(uintptr_t)code;
    uintptr_t target=(uintptr_t)camera_helmet_constructor_bridge;memcpy(image+0xd2c5b0,"\x48\xb8",2);memcpy(image+0xd2c5b2,&target,8);memcpy(image+0xd2c5ba,"\xff\xe0",2);memset(image+0xd2c5bc,0x90,3);FlushInstructionCache(GetCurrentProcess(),image+0xd2c5b0,15);return 1;
}
static void camera_helmet_reference_install(int requested){
    const char*enabled=camera_fix_getenv("BB_CAMERA_REFERENCE_HELMET");if(!requested||!enabled||strcmp(enabled,"1"))return;
    static const unsigned char update[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x78};
    static const unsigned char ctor[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x89,0xfb,0xe8,0xa1,0x55,0x80,0xff};
    static const unsigned char config[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50};
    static const unsigned char destruct[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x30};
    if(runtime_image_size<0xd2ea50+sizeof update||memcmp(image+0xd2ea50,update,sizeof update)||memcmp(image+0xd2c5b0,ctor,sizeof ctor)||memcmp(image+0xd2ddb0,config,sizeof config)||memcmp(image+0xd2ca50,destruct,sizeof destruct)){puts("HelmetReference: native signatures unavailable; skipped");return;}
    if(!chr_ctor_hook(ctor))return;
    chr_config_original=(CHRLife)camera_rate_hook(0xd2ddb0,config,sizeof config,(uintptr_t)camera_helmet_config);
    chr_destructor_original=(CHRLife)camera_rate_hook(0xd2ca50,destruct,sizeof destruct,(uintptr_t)camera_helmet_destructor);
    chr_update_original=(CHRUpdate)camera_rate_hook(0xd2ea50,update,sizeof update,(uintptr_t)camera_helmet_update);
    chr_enabled=chr_config_original&&chr_destructor_original&&chr_update_original&&camera_reference_force_original&&camera_dynamics_spring_original;
    puts(chr_enabled?"HelmetReference: native30 six-spring state enabled; view-base composition remains native VRR":"HelmetReference: unavailable");
}
