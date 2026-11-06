/* Experimental environment-only mod. No writes to the engine/vehicle clocks.
 * Reverse precipitation uses session history, never a negative simulation step. */
#ifdef _WIN32
#include <math.h>
static int env_timelapse_enabled, env_action, env_hooks_ready;
static double env_hold_rate=500.0,env_key_factor=1.0;
static uint64_t env_cursor_bits, env_oldest_bits, env_native_elapsed_bits,env_frame_scale_bits,env_light_oldest_bits,env_cloud_oldest_bits;
static unsigned env_generation;
static double env_load_double(uint64_t *bits) {
  uint64_t v=__atomic_load_n(bits,__ATOMIC_ACQUIRE); double d;memcpy(&d,&v,8);return d;
}
static void env_store_double(uint64_t *bits,double d) {
  uint64_t v;memcpy(&v,&d,8);__atomic_store_n(bits,v,__ATOMIC_RELEASE);
}
static double env_modifier_factor(int shift,int control) {
  return (shift?2.0:1.0)*(control?0.5:1.0);
}
static int env_keys(void) {
  DWORD pid=0;env_key_factor=1;
  GetWindowThreadProcessId(GetForegroundWindow(),&pid);
  if(pid!=GetCurrentProcessId())return 0;
  int plus=(GetAsyncKeyState(VK_ADD)|GetAsyncKeyState(VK_OEM_PLUS))&0x8000;
  int minus=(GetAsyncKeyState(VK_SUBTRACT)|GetAsyncKeyState(VK_OEM_MINUS))&0x8000;
  env_key_factor=env_modifier_factor((GetAsyncKeyState(VK_SHIFT)&0x8000)!=0,
                                  (GetAsyncKeyState(VK_CONTROL)&0x8000)!=0);
  return plus==minus?0:plus?1:-1;
}
static float env_time_delta(void *self,float dt) {
  static void *last_self;
  if(!env_timelapse_enabled)return dt;
  if(!isfinite(dt)||dt<0||dt>0.25f) {
    __atomic_store_n(&env_action,0,__ATOMIC_RELEASE);
    env_store_double(&env_frame_scale_bits,1);return dt;
  }
  if(self!=last_self) {
    last_self=self;env_store_double(&env_cursor_bits,0);env_store_double(&env_oldest_bits,0);env_store_double(&env_native_elapsed_bits,0);env_store_double(&env_light_oldest_bits,0);env_store_double(&env_cloud_oldest_bits,0);
    __atomic_add_fetch(&env_generation,1,__ATOMIC_RELEASE);
  }
  int action=env_keys();__atomic_store_n(&env_action,action,__ATOMIC_RELEASE);
  double current=env_load_double(&env_cursor_bits);
  env_store_double(&env_native_elapsed_bits,env_load_double(&env_native_elapsed_bits)+dt);
  double next=current+dt*(action?env_hold_rate*env_key_factor*action:1.0);
  double oldest=fmax(fmax(env_load_double(&env_oldest_bits),env_load_double(&env_light_oldest_bits)),env_load_double(&env_cloud_oldest_bits));
  if(next<oldest)next=oldest;
  env_store_double(&env_cursor_bits,next);
  env_store_double(&env_frame_scale_bits,dt>0?(next-current)/dt:0);
  return (float)(next-current);
}

enum { ENV_HISTORY_COUNT=32768, ENV_CURVES=21, ENV_RAIN_BYTES=0x190 };
typedef struct {
  double time;
  unsigned char driver[0x50],rain[ENV_RAIN_BYTES];
  uint32_t indices[ENV_CURVES];
  uint32_t curve_offset;
  uintptr_t renderer;
} EnvSnapshot;
static EnvSnapshot *env_history;
static unsigned env_history_first,env_history_count;
static uintptr_t env_rain_renderer(void) {
  uintptr_t root=0,services=0,renderer=0; SIZE_T got;
  if(!ReadProcessMemory(GetCurrentProcess(),image+0x1911100,&root,8,&got)||!root)return 0;
  if(!ReadProcessMemory(GetCurrentProcess(),(void *)(root+0x970),&services,8,&got)||!services)return 0;
  if(!ReadProcessMemory(GetCurrentProcess(),(void *)(services+0x258),&renderer,8,&got)||!renderer)return 0;
  uintptr_t vtable=0;
  if(!ReadProcessMemory(GetCurrentProcess(),(void *)renderer,&vtable,8,&got)||vtable!=(uintptr_t)image+0x15aee90)return 0;
  return renderer;
}
static EnvSnapshot *env_snapshot(unsigned n) {
  return &env_history[(env_history_first+n)%ENV_HISTORY_COUNT];
}
static int env_capture(void *self,double time,EnvSnapshot *s) {
  unsigned char *p=self;
  if(!p[0x430]||!p[0x431])return 0;
  uintptr_t curve=0;memcpy(&curve,p+0x3f8,8);
  if(curve<(uintptr_t)p+8 || curve>(uintptr_t)p+0x3c8 || (curve-(uintptr_t)p-8)%0x30)return 0;
  uintptr_t renderer=env_rain_renderer();if(!renderer)return 0;
  s->time=time;s->renderer=renderer;s->curve_offset=(uint32_t)(curve-(uintptr_t)p);
  memcpy(s->driver,p+0x400,sizeof s->driver);
  for(unsigned i=0;i<ENV_CURVES;i++)memcpy(&s->indices[i],p+8+i*0x30,4);
  memcpy(s->rain,(void *)(renderer+0x38),sizeof s->rain);
  return 1;
}
static int env_restore(void *self,EnvSnapshot *s) {
  unsigned char *p=self;
  if(s->renderer!=env_rain_renderer())return 0;
  // Curves/resources remain owned by the live driver. Restore indices and scalar
  // state only; never restore an old vector, allocation or graphics resource.
  unsigned char active=p[0x430],started=p[0x431];
  memcpy(p+0x400,s->driver,sizeof s->driver);
  // Lifecycle events (start/pause/end of race) are not part of weather seeking.
  p[0x430]=active;p[0x431]=started;
  for(unsigned i=0;i<ENV_CURVES;i++)memcpy(p+8+i*0x30,&s->indices[i],4);
  uintptr_t curve=(uintptr_t)p+s->curve_offset;memcpy(p+0x3f8,&curve,8);
  memcpy((void *)(s->renderer+0x38),s->rain,sizeof s->rain);
  return 1;
}
typedef void (ABI *EnvUpdate)(void *, float);
static EnvUpdate env_precip_original;
static EnvUpdate env_rain_fx_original;
typedef uintptr_t (ABI *EnvRenderUpdate)(void *,void *,float);
static EnvRenderUpdate env_sky_original;

/* Broad visual groups. Membership comes from the live CrowdManager registry,
 * never asset names. AnimatedObjectManager joins all its jobs before returning,
 * so the immutable sorted membership array remains valid for every worker. */
#define ENV_MAX_CROWD 8192
static uintptr_t env_crowd_objects[ENV_MAX_CROWD];
static unsigned env_crowd_count;
/* Animation load class 9 is used by scene components and crowd; the vehicle
 * driver factory uses class 4. Observe that engine classification at load/unload,
 * rather than guessing from skeletons or listing prop asset names. */
static SRWLOCK env_animation_registry_lock=SRWLOCK_INIT;
static struct {uintptr_t object;int kind;} env_animation_registry[16384];
static unsigned env_animation_registry_count;
typedef uintptr_t (ABI *EnvAnimationLoad)(void *,const char *,int);
typedef uintptr_t (ABI *EnvAnimationUnload)(void *);
static EnvAnimationLoad env_animation_load_original;
static EnvAnimationUnload env_animation_unload_original;
static void env_animation_register(uintptr_t object,int kind) {
  AcquireSRWLockExclusive(&env_animation_registry_lock);
  unsigned i=0;while(i<env_animation_registry_count&&env_animation_registry[i].object!=object)++i;
  if(kind>=0&&i<16384) {
    env_animation_registry[i].object=object;env_animation_registry[i].kind=kind;
    if(i==env_animation_registry_count)++env_animation_registry_count;
  } else if(i<env_animation_registry_count) {
    env_animation_registry[i]=env_animation_registry[--env_animation_registry_count];
  }
  ReleaseSRWLockExclusive(&env_animation_registry_lock);
}
static ABI uintptr_t env_animation_load(void *self,const char *asset,int kind) {
  uintptr_t result=env_animation_load_original(self,asset,kind);
  env_animation_register((uintptr_t)self,(result&255)?kind:-1);
  return result;
}
static ABI uintptr_t env_animation_unload(void *self) {
  env_animation_register((uintptr_t)self,-1);
  return env_animation_unload_original(self);
}
static EnvUpdate env_animated_manager_original,env_world_animation_original;
typedef void (ABI *EnvParticlesUpdate)(void *,void *,float,int,float);
static EnvParticlesUpdate env_particles_original;
static float env_group_step(float dt,int reversible) {
  if(!env_timelapse_enabled||!isfinite(dt)||dt<0||dt>0.25f)return dt;
  int a=__atomic_load_n(&env_action,__ATOMIC_ACQUIRE);
  if(!a)return dt;
  if(a<0&&!reversible)return 0;
  return dt*(float)env_load_double(&env_frame_scale_bits);
}
static int env_compare_ptr(const void *a,const void *b) {
  uintptr_t x=*(const uintptr_t *)a,y=*(const uintptr_t *)b;return (x>y)-(x<y);
}
static void env_refresh_crowd(void) {
  env_crowd_count=0;
  uintptr_t root=*(uintptr_t *)(image+0x1911100);
  uintptr_t services=root?*(uintptr_t *)(root+0x970):0;
  uintptr_t crowd=services?*(uintptr_t *)(services+0x608):0;
  if(crowd&&*(uintptr_t *)crowd==(uintptr_t)image+0x1659710) {
    uintptr_t begin=*(uintptr_t *)(crowd+0x18),end=*(uintptr_t *)(crowd+0x20);
    if(begin&&end>=begin&&(end-begin)%8==0&&(end-begin)/8<=ENV_MAX_CROWD) {
      for(uintptr_t it=begin;it<end;it+=8) {
        uintptr_t record=*(uintptr_t *)it;
        if(record&&*(uintptr_t *)record==(uintptr_t)image+0x16597a0)
          env_crowd_objects[env_crowd_count++]=record+0x10;
      }
    }
  }
  AcquireSRWLockShared(&env_animation_registry_lock);
  for(unsigned i=0;i<env_animation_registry_count&&env_crowd_count<ENV_MAX_CROWD;i++)
    if(env_animation_registry[i].kind==9)env_crowd_objects[env_crowd_count++]=env_animation_registry[i].object;
  ReleaseSRWLockShared(&env_animation_registry_lock);
  qsort(env_crowd_objects,env_crowd_count,8,env_compare_ptr);
  unsigned n=0;
  for(unsigned i=0;i<env_crowd_count;i++)if(!i||env_crowd_objects[i]!=env_crowd_objects[i-1])env_crowd_objects[n++]=env_crowd_objects[i];
  env_crowd_count=n;
}
static ABI void env_animated_manager_update(void *self,float dt) {
  env_refresh_crowd();env_animated_manager_original(self,dt);
}
static ABI uintptr_t env_animated_job(void *job) {
  unsigned char *p=job;
  void *object=**(void ***)(p+0x18);
  float dt;
  void *context=*(void **)(p+0x20);
  dt=**(float **)((unsigned char *)context+8);
  uintptr_t key=(uintptr_t)object;
  int crowd=bsearch(&key,env_crowd_objects,env_crowd_count,8,env_compare_ptr)!=NULL;
  float step=crowd?env_group_step(dt,1):dt;
  if(crowd&&step<0&&*(uintptr_t *)object==(uintptr_t)image+0x1652b30) {
    uintptr_t engine=*(uintptr_t *)((unsigned char *)object+0xe0);
    float speed=*(float *)((unsigned char *)object+0xb4);
    if(engine&&isfinite(speed)&&speed>0) {
      float age=*(float *)(engine+0x19c8);
      if(isfinite(age))step=fmaxf(step,-fmaxf(age,0)/speed);
    }
  }
  ((EnvUpdate)(*(uintptr_t **)object)[2])(object,step);
  return 0;
}
static ABI void env_world_animation_update(void *self,float dt) {
  env_world_animation_original(self,env_group_step(dt,1));
}
/* One Haboob simulation submission for every emitter: smoke, spray, leaves and
 * other GPU effects. Keep context/replay flags and all vehicle clocks intact.
 * Reverse is held at zero; dead GPU particles cannot be resurrected by negative
 * integration. Exact reverse needs GPU state checkpoints, not pointer snapshots. */
static ABI void env_particles_update(void *self,void *context,float dt,int replay,float replay_time) {
  env_particles_original(self,context,env_group_step(dt,0),replay,replay_time);
}

#include "runtime_environment_lightning.inc"
static double env_offset(void) {
  return env_load_double(&env_cursor_bits)-env_load_double(&env_native_elapsed_bits);
}
/* The game, not Simul's cloud keyframer, owns the clock at +0x4c8.
 * It publishes this to the sky before building cloud/atmosphere constants.
 * Move that clock once; remapping only the keyframer leaves the actual sky
 * render clock and wind at native speed. Keep the render update's real dt. */
#include "runtime_environment_clouds.inc"
static ABI uintptr_t env_sky_update(void *self,void *context,float dt) {
  static void *last_self;static unsigned generation;static double last_offset;
  unsigned g=__atomic_load_n(&env_generation,__ATOMIC_ACQUIRE);
  double offset=env_offset();unsigned char *p=self;
  float before=*(float *)(p+0x4c8),wind=*(float *)(p+0x158);
  if(self!=last_self||generation!=g){last_self=self;generation=g;last_offset=offset;}
  if(env_timelapse_enabled&&isfinite(before)) {
    double extra=offset-last_offset;*(float *)(p+0x4c8)=(float)fmax(0.0,before+extra);
    if(isfinite(wind)&&dt>0&&dt<=0.25f)*(float *)(p+0x158)=wind*(float)(1.0+extra/dt);
  }
  env_sky_prepare(self,*(float *)(p+0x4c8)+dt);last_offset=offset;
  uintptr_t result=env_sky_original(self,context,dt);
  *(float *)(p+0x158)=wind;return result;
}
static ABI void env_rain_fx_update(void *self,float dt) {
  env_rain_fx_original(self,env_group_step(dt,0));
}
static ABI void env_precip_update(void *self,float dt) {
  if(!env_timelapse_enabled){env_precip_original(self,dt);return;}
  static void *last_self;static unsigned generation,profile;
  unsigned g=__atomic_load_n(&env_generation,__ATOMIC_ACQUIRE);
  double cursor=env_load_double(&env_cursor_bits);
  unsigned current_profile=*(unsigned *)((unsigned char *)self+0x400);
  if(self!=last_self||g!=generation||profile!=current_profile){last_self=self;generation=g;profile=current_profile;env_history_first=env_history_count=0;}
  if(!env_history)env_history=calloc(ENV_HISTORY_COUNT,sizeof *env_history);
  if(!env_history){env_precip_original(self,dt);return;}
  int action=__atomic_load_n(&env_action,__ATOMIC_ACQUIRE);
  if(action<0&&env_history_count) {
    unsigned lo=0,hi=env_history_count;
    while(lo+1<hi){unsigned mid=(lo+hi)/2;if(env_snapshot(mid)->time<=cursor)lo=mid;else hi=mid;}
    if(env_restore(self,env_snapshot(lo)))return;
  }
  while(env_history_count&&env_snapshot(env_history_count-1)->time>cursor)--env_history_count;
  /* The native driver handles one profile transition per call and discards
   * excess dt. Small weather-only steps preserve transitions at timelapse rate. */
  double remaining=action>0?env_group_step(dt,0):dt;
  if(!isfinite(remaining)||remaining<0||dt>0.25f)remaining=dt;
  do {
    float step=(float)fmin(remaining,0.25);
    env_precip_original(self,step);remaining-=step;
  } while(remaining>0.000001);
  EnvSnapshot s;
  if(env_capture(self,cursor,&s)) {
    if(env_history_count==ENV_HISTORY_COUNT){env_history_first=(env_history_first+1)%ENV_HISTORY_COUNT;--env_history_count;}
    *env_snapshot(env_history_count++)=s;
    env_store_double(&env_oldest_bits,env_snapshot(0)->time);
  } else if(!env_history_count)env_store_double(&env_oldest_bits,0);
}
/* Native common render-parameter builder. The caller supplies a 624-byte
 * per-view shader block. +0x248 is the simulation timestamp in seconds; vertex
 * wind shaders feed it into their procedural displacement. Map this visual
 * clock only, after native construction. Matrices, dt/interpolation, vehicle
 * state and TimingComponent are unchanged. No material/asset/hash list. */
typedef void (ABI *EnvShaderGlobals)(void *,void *,void *,unsigned);
static EnvShaderGlobals env_shader_globals_original;
static void env_shader_clock_map(void *constants) {
  if(!env_timelapse_enabled||!constants)return;
  float *clock=(float *)((unsigned char *)constants+0x248);
  double offset=env_load_double(&env_cursor_bits)-env_load_double(&env_native_elapsed_bits);
  double mapped=(double)*clock+offset;
  if(isfinite(*clock)&&isfinite(mapped)&&mapped>=0&&mapped<1e8)*clock=(float)mapped;
}
static ABI void env_shader_globals(void *self,void *constants,void *viewport,unsigned flag) {
  env_shader_globals_original(self,constants,viewport,flag);
  env_shader_clock_map(constants);
}

static void env_jump(unsigned char *p, uintptr_t target) {
  static const unsigned char op[]={0xff,0x25,0,0,0,0};
  memcpy(p,op,6);memcpy(p+6,&target,8);
}
/* Startup-only transaction, before guest code/workers start. Validate every
 * entry and prepare every RX gateway before changing a native instruction. */
typedef struct {size_t offset;const unsigned char *signature;size_t size;
  uintptr_t target;void *original;unsigned char *code;} EnvHook;
static uintptr_t env_animated_job_trampoline;
static const unsigned char env_sig_rain[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0xc5,0xf8,0x28,0xd0};
static const unsigned char env_sig_sky[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x48,0x01,0,0};
static const unsigned char env_sig_rainfx[]={0x55,0x48,0x89,0xe5,0x53,0x50,0xc5,0xfa,0x11,0x45,0xf4,0x48,0x89,0xfb};
static const unsigned char env_sig_world[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x48};
static const unsigned char env_sig_animated[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x78,0x1,0x0,0x0};
static const unsigned char env_sig_job[]={0x55,0x48,0x89,0xe5,0x48,0x8b,0x47,0x18,0x48,0x8b,0x4f,0x20,0x48,0x8b,0x49,0x8};
static const unsigned char env_sig_particles[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0x0,0x0,0x0};
static const unsigned char env_sig_load[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x41,0x89,0xd6,0x49,0x89,0xf7};
static const unsigned char env_sig_unload[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x89,0xfb,0x48,0x8b,0xbb,0xe0,0x0,0x0,0x0};
static const unsigned char env_sig_light[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xfe};
static const unsigned char env_sig_globals[]={0x8b,0x42,0x2c,0x8b,0x52,0x30,0xc4,0xe1,0xfa,0x2a,0xc0,0xc4,0xe1,0xfa,0x2a,0xca};
#define ENV_HOOK(off,sig,fn,slot) {off,sig,sizeof sig,(uintptr_t)fn,&slot,NULL}
static EnvHook env_hooks[]={
  ENV_HOOK(0xf4b7c0,env_sig_rain,env_precip_update,env_precip_original),
  ENV_HOOK(0x601df0,env_sig_sky,env_sky_update,env_sky_original),
  ENV_HOOK(0x8e4e0,env_sig_rainfx,env_rain_fx_update,env_rain_fx_original),
  ENV_HOOK(0x5377b0,env_sig_world,env_world_animation_update,env_world_animation_original),
  ENV_HOOK(0xc9cc10,env_sig_animated,env_animated_manager_update,env_animated_manager_original),
  ENV_HOOK(0xc9d3a0,env_sig_job,env_animated_job,env_animated_job_trampoline),
  ENV_HOOK(0x65b200,env_sig_particles,env_particles_update,env_particles_original),
  ENV_HOOK(0xc97ec0,env_sig_load,env_animation_load,env_animation_load_original),
  ENV_HOOK(0xca1150,env_sig_unload,env_animation_unload,env_animation_unload_original),
  ENV_HOOK(0x70ba40,env_sig_light,env_light_update,env_light_original),
  ENV_HOOK(0x7992d0,env_sig_globals,env_shader_globals,env_shader_globals_original)
};
#undef ENV_HOOK
static int env_signature(size_t offset,const unsigned char *bytes,size_t count) {
  return offset<=runtime_image_size&&count<=runtime_image_size-offset&&!memcmp(image+offset,bytes,count);
}
static void env_hooks_release(void) {
  for(unsigned i=0;i<sizeof env_hooks/sizeof *env_hooks;i++) {
    EnvHook *h=env_hooks+i;
    if(env_hooks_ready){memcpy(image+h->offset,h->signature,h->size);FlushInstructionCache(GetCurrentProcess(),image+h->offset,h->size);}
    if(h->code){VirtualFree(h->code,0,MEM_RELEASE);h->code=NULL;}
    memset(h->original,0,sizeof(uintptr_t));
  }
  env_hooks_ready=env_timelapse_enabled=0;
  __atomic_store_n(&env_action,0,__ATOMIC_RELEASE);env_store_double(&env_frame_scale_bits,1);
}
static void env_timelapse_time_ready(int ready) {
  if(!ready){env_hooks_release();return;}
  env_timelapse_enabled=env_hooks_ready;
  if(env_timelapse_enabled)printf("Environment timelapse: hold +/- at %.0fx, Shift x2, Ctrl x0.5; vehicle physics clock unchanged\n",env_hold_rate);
}
static void env_timelapse_install(void) {
  const char *mode=getenv("BB_ENV_TIMELAPSE");
  if(!mode||strcmp(mode,"1")||env_hooks_ready)return;
  static const unsigned char time_signature[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xf8,0,0,0};
  if(!env_signature(0x75a00,time_signature,sizeof time_signature))return;
  for(unsigned i=0;i<sizeof env_hooks/sizeof *env_hooks;i++)if(!env_signature(env_hooks[i].offset,env_hooks[i].signature,env_hooks[i].size)) {
    puts("Environment timelapse: unsupported image; disabled");return;
  }
  for(unsigned i=0;i<sizeof env_hooks/sizeof *env_hooks;i++) {
    EnvHook *h=env_hooks+i;h->code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!h->code){env_hooks_release();return;}
    memcpy(h->code,h->signature,h->size);env_jump(h->code+h->size,(uintptr_t)image+h->offset+h->size);
    DWORD old;if(!VirtualProtect(h->code,4096,PAGE_EXECUTE_READ,&old)){env_hooks_release();return;}
    FlushInstructionCache(GetCurrentProcess(),h->code,4096);
  }
  /* All slots and gateways are ready before the first native entry is changed. */
  for(unsigned i=0;i<sizeof env_hooks/sizeof *env_hooks;i++)memcpy(env_hooks[i].original,&env_hooks[i].code,sizeof(uintptr_t));
  for(unsigned i=0;i<sizeof env_hooks/sizeof *env_hooks;i++) {
    EnvHook *h=env_hooks+i;env_jump(image+h->offset,h->target);memset(image+h->offset+14,0x90,h->size-14);FlushInstructionCache(GetCurrentProcess(),image+h->offset,h->size);
  }
  const char *rate=getenv("BB_ENV_TIMELAPSE_RATE");
  if(rate){char *end=NULL;double n=strtod(rate,&end);if(end!=rate&&!*end&&isfinite(n)&&n>=1&&n<=3600)env_hold_rate=n;}
  env_hooks_ready=1; /* Enabled only after the time-of-day hook also succeeds. */
}
#else
static void env_timelapse_install(void) {}
#endif
