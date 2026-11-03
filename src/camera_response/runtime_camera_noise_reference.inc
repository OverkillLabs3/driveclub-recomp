/* Candidate replacement for CameraNoisePose + camera_noise_update in
 * runtime_camera_rate.inc. It uses the existing original function pointer,
 * camera_noise_fixed_step and camera_rate_hook. No source-tree integration.
 *
 * Causal policy: execute native 30 Hz steps and hold the latest complete native
 * matrix between steps. Never evaluate a future tick: noise consumes the game's
 * shared RNG. No matrix-element interpolation or custom spring coefficients.
 * Keep fractional credit across changes of cadence, including exact 30 Hz calls.
 * Invalid/nonpositive dt and long clock discontinuities hold the latest pose and
 * retain fractional credit; they never replay a pause through the shared RNG.
 */
#include <float.h>
#include <math.h>

#define CAMERA_NOISE_SLOT_COUNT 64
/* Catch-up CPU/RNG budget in reference ticks, not an authored motion coefficient. */
#define CAMERA_NOISE_MAX_TICKS 8
/* Native fixed30 camera clock, preserving float32 arithmetic and signed64
 * microsecond truncation: D847D0 ->78C8C/78C94 ->CC891A/CC891F. The camera dt
 * passed unchanged to CE3540 is quantized, not the nominal float32 1/30. */
static const float camera_noise_period=
    (float)(int64_t)((1.0f/30.0f)*1000000.0f)*1e-6f;
static const int64_t camera_noise_period_us=
    (int64_t)((1.0f/30.0f)*1000000.0f);
/* Recover only an exactly representable native microsecond interval. The
 * bounded nearest-integer estimate is validated by the ORIGINAL float32 forward
 * conversion, bit for bit. No epsilon or guessed clock-quantization tolerance. */
static int camera_noise_native_us(float dt,int64_t *out) {
    if(!(dt>0)||!isfinite(dt)||
       (double)dt>CAMERA_NOISE_MAX_TICKS*(double)camera_noise_period)return 0;
    int64_t us=(int64_t)llround((double)dt/(double)1e-6f);
    if(us<=0||us>CAMERA_NOISE_MAX_TICKS*camera_noise_period_us)return 0;
    float roundtrip=(float)us*1e-6f;
    if(memcmp(&dt,&roundtrip,sizeof dt))return 0;
    *out=us;return 1;
}
static double camera_noise_elapsed_units(float dt) {
    int64_t us;
    if(camera_noise_native_us(dt,&us))return (double)us;
    /* Scaled/non-native float deltas have a fractional reference-clock contract,
       rather than being silently rounded to integer microseconds. */
    return ((double)dt/(double)camera_noise_period)*(double)camera_noise_period_us;
}
typedef void (ABI *CameraNoiseConstructor)(void *,float,float,float,float,float,float);
static CameraNoiseConstructor camera_noise_constructor_original;
typedef struct {
    void *self;
    uint64_t age,generation;
    double credit; /* Native microsecond reference units, possibly fractional. */
    float last_output[16];
    int has_output;
    DWORD last_thread;
} CameraNoiseState;
typedef struct {
    uint64_t calls,constructors,external_resets,invalid_inputs,invalid_native,
             direct_steps,fixed_steps,evictions,thread_changes,untracked_calls,
             stall_calls;
    double max_orthogonal_error,max_determinant_error,max_affine_error;
} CameraNoiseDiagnostics;
static CameraNoiseState camera_noise_states[CAMERA_NOISE_SLOT_COUNT];
static SRWLOCK camera_noise_state_lock=SRWLOCK_INIT;
static uint64_t camera_noise_age,camera_noise_generation;
static int camera_noise_diagnostics_enabled;
static CameraNoiseDiagnostics camera_noise_diagnostics;

static void camera_noise_identity(float *m) {
    memset(m,0,64);m[0]=m[5]=m[10]=m[15]=1;
}
/* Numerical validity bounds, not motion-strength controls. The native constructor
 * writes this affine identity and the updater writes a rotation plus translation.
 * Keep diagnostics to check the native fast-trigonometry error in actual scenes.
 */
static int camera_noise_rigid(const float *m) {
    for(unsigned i=0;i<16;++i) if(!isfinite(m[i]))return 0;
    double orth=0;
    for(unsigned a=0;a<3;++a) for(unsigned b=a;b<3;++b) {
        double dot=0;for(unsigned k=0;k<3;++k)dot+=(double)m[a*4+k]*m[b*4+k];
        double e=fabs(dot-(a==b?1.0:0.0));if(e>orth)orth=e;
    }
    double det=(double)m[0]*(m[5]*m[10]-m[6]*m[9])
              -(double)m[1]*(m[4]*m[10]-m[6]*m[8])
              +(double)m[2]*(m[4]*m[9]-m[5]*m[8]);
    double de=fabs(det-1),affine=fabs(m[3])+fabs(m[7])+fabs(m[11])+fabs(m[15]-1);
    if(camera_noise_diagnostics_enabled) {
        if(orth>camera_noise_diagnostics.max_orthogonal_error)camera_noise_diagnostics.max_orthogonal_error=orth;
        if(de>camera_noise_diagnostics.max_determinant_error)camera_noise_diagnostics.max_determinant_error=de;
        if(affine>camera_noise_diagnostics.max_affine_error)camera_noise_diagnostics.max_affine_error=affine;
    }
    const double tolerance=512.0*FLT_EPSILON;
    return orth<=tolerance && de<=tolerance && affine<=tolerance;
}
static CameraNoiseState *camera_noise_state(void *self,int allow_eviction) {
    CameraNoiseState *oldest=&camera_noise_states[0];
    for(unsigned i=0;i<CAMERA_NOISE_SLOT_COUNT;++i) {
        CameraNoiseState *s=&camera_noise_states[i];
        if(s->self==self){s->age=++camera_noise_age;return s;}
        if(!s->self){oldest=s;break;}
        if(s->age<oldest->age)oldest=s;
    }
    if(oldest->self && !allow_eviction)return NULL;
    if(oldest->self && camera_noise_diagnostics_enabled)++camera_noise_diagnostics.evictions;
    memset(oldest,0,sizeof(*oldest));oldest->self=self;oldest->age=++camera_noise_age;
    oldest->generation=++camera_noise_generation;
    return oldest;
}
static void camera_noise_seed(CameraNoiseState *s,const float *m) {
    s->credit=0;
    if(camera_noise_rigid(m))memcpy(s->last_output,m,64);
    else {
        /* Literal native-constructor identity, not an authored substitute pose. */
        camera_noise_identity(s->last_output);
        if(camera_noise_diagnostics_enabled)++camera_noise_diagnostics.invalid_inputs;
    }
    s->has_output=1;
}
static void camera_noise_accept(CameraNoiseState *s,void *self) {
    float *m=(float *)((unsigned char *)self+0x160);
    if(camera_noise_rigid(m))memcpy(s->last_output,m,64);
    else {
        if(camera_noise_diagnostics_enabled)++camera_noise_diagnostics.invalid_native;
        memcpy(m,s->last_output,64);
    }
}
static ABI void camera_noise_constructor(void *self,float a,float b,float c,float d,float e,float f) {
    /* Native CE3250 does not call CE3540. Hold one lock through construction so
       an update cannot observe a partially initialized effect through this hook. */
    AcquireSRWLockExclusive(&camera_noise_state_lock);
    camera_noise_constructor_original(self,a,b,c,d,e,f);
    CameraNoiseState *s=camera_noise_state(self,1);
    s->generation=++camera_noise_generation;
    camera_noise_seed(s,(const float *)((const unsigned char *)self+0x160));
    if(camera_noise_diagnostics_enabled)++camera_noise_diagnostics.constructors;
    ReleaseSRWLockExclusive(&camera_noise_state_lock);
}
static ABI void camera_noise_update(void *self,float dt) {
    /* Shared registry deliberately replaces TLS. Jobs can process one object on
       different workers; its simulation credit must follow the object. Native
       CE3540 has no recursive CE3540 call, so the bounded registry can serialize
       this small effect without a recursive-lock contract. */
    AcquireSRWLockExclusive(&camera_noise_state_lock);
    CameraNoiseState *s=camera_noise_state(self,0);
    float *m=(float *)((unsigned char *)self+0x160);
    if(!s) {
        /* More live objects than the bounded registry: preserve native cadence
           rather than repeatedly evicting their fractional clocks and freezing
           every effect. Constructor registration may replace an old slot. */
        float before[16];memcpy(before,m,64);
        if(!camera_noise_rigid(before))camera_noise_identity(before);
        unsigned was=camera_noise_fixed_step;camera_noise_fixed_step=1;
        if(dt>0 && isfinite(dt) &&
           (double)dt<=CAMERA_NOISE_MAX_TICKS*(double)camera_noise_period)
            camera_noise_update_original(self,dt);
        else if(dt>0 && isfinite(dt) && camera_noise_diagnostics_enabled)
            ++camera_noise_diagnostics.stall_calls;
        camera_noise_fixed_step=was;
        if(!camera_noise_rigid(m))memcpy(m,before,64);
        if(camera_noise_diagnostics_enabled)++camera_noise_diagnostics.untracked_calls;
        ReleaseSRWLockExclusive(&camera_noise_state_lock);return;
    }
    if(camera_noise_diagnostics_enabled) {
        ++camera_noise_diagnostics.calls;
        DWORD tid=GetCurrentThreadId();
        if(s->last_thread && s->last_thread!=tid)++camera_noise_diagnostics.thread_changes;
        s->last_thread=tid;
    }
    if(!s->has_output)camera_noise_seed(s,m);
    else if(memcmp(s->last_output,m,64)) {
        /* Respect native/owner resets even when the same object address survives
           the switch. Constructor generation also catches identical-matrix reset. */
        if(camera_noise_diagnostics_enabled)++camera_noise_diagnostics.external_resets;
        camera_noise_seed(s,m);
    }
    memcpy(m,s->last_output,64);
    if(!(dt>0) || !isfinite(dt)) {
        /* Native CE3540 exits for nonpositive/unordered dt. A no-step call must
           not discard the sub-tick time already accumulated by this object. */
        ReleaseSRWLockExclusive(&camera_noise_state_lock);return;
    }
    if((double)dt>CAMERA_NOISE_MAX_TICKS*(double)camera_noise_period) {
        /* Explicit discontinuity policy: hold through a long stall/pause rather
           than consuming an unbounded burst of global RNG or slowly replaying
           its elapsed time. Preserve the phase from before the discontinuity.
           At most eight native ticks can execute in an accepted update. */
        if(camera_noise_diagnostics_enabled)++camera_noise_diagnostics.stall_calls;
        ReleaseSRWLockExclusive(&camera_noise_state_lock);return;
    }
    unsigned was=camera_noise_fixed_step;camera_noise_fixed_step=1;
    int direct=dt==camera_noise_period && s->credit==0;
    s->credit+=camera_noise_elapsed_units(dt);
    unsigned ticks=0;
    while(s->credit>=(double)camera_noise_period_us && ticks<CAMERA_NOISE_MAX_TICKS) {
        memcpy(m,s->last_output,64);
        camera_noise_update_original(self,camera_noise_period);
        camera_noise_accept(s,self);
        s->credit-=(double)camera_noise_period_us;
        ++ticks;
        if(camera_noise_diagnostics_enabled) {
            if(direct)++camera_noise_diagnostics.direct_steps;
            else ++camera_noise_diagnostics.fixed_steps;
        }
    }
    memcpy(m,s->last_output,64);
    camera_noise_fixed_step=was;
#if defined(BB_CAMERA_DIAGNOSTICS) && BB_CAMERA_DIAGNOSTICS
    if(camera_noise_diagnostics_enabled && (camera_noise_diagnostics.calls%240)==0)
        printf("NoiseReference: calls=%llu ctor=%llu reset=%llu invalid_in=%llu invalid_out=%llu thread_changes=%llu direct=%llu fixed=%llu stalls=%llu ortho=%.9g determinant_error=%.9g\n",
            camera_noise_diagnostics.calls,camera_noise_diagnostics.constructors,camera_noise_diagnostics.external_resets,camera_noise_diagnostics.invalid_inputs,camera_noise_diagnostics.invalid_native,camera_noise_diagnostics.thread_changes,camera_noise_diagnostics.direct_steps,camera_noise_diagnostics.fixed_steps,camera_noise_diagnostics.stall_calls,camera_noise_diagnostics.max_orthogonal_error,camera_noise_diagnostics.max_determinant_error);
#endif
    ReleaseSRWLockExclusive(&camera_noise_state_lock);
}
static int camera_noise_lifecycle_install(void) {
    const char *diag=camera_fix_getenv("BB_CAMERA_NOISE_DIAG");
    camera_noise_diagnostics_enabled=diag && atoi(diag)!=0;
    static const unsigned char ctor[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38};
    camera_noise_constructor_original=(CameraNoiseConstructor)camera_rate_hook(
        0xce3250,ctor,sizeof ctor,(uintptr_t)camera_noise_constructor);
    /* Caller must not install the update hook if lifecycle validation failed. */
    return camera_noise_constructor_original!=NULL;
}
static __attribute__((unused)) CameraNoiseDiagnostics camera_noise_diagnostics_snapshot(void) {
    AcquireSRWLockShared(&camera_noise_state_lock);
    CameraNoiseDiagnostics result=camera_noise_diagnostics;
    ReleaseSRWLockShared(&camera_noise_state_lock);return result;
}
