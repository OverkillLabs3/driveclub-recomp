#include "../gpu/shim/bbport_diagnostics.h"
/* Force feedback: the game drives forces only for a Logitech wheel, through the bundled logiWheel.prx. Its exports
 * are replaced here (by NID; the names are not public) so that the game sees a Logitech wheel whenever a wheel is
 * connected, and the forces it plays go to that wheel through SDL's haptic API (DirectInput force feedback).
 *
 * What the game does (guest +0xe12e30 and the methods of its Logitech wheel class, vtable +0x165c020):
 *   logiWheelInit(0x1000c); every frame logiWheelReadState(state): 0 means connected, and the state is filled from
 *   the steering wheel device class data, as logiWheel.prx itself does (+0x2e61ed0);
 *   at connection it creates five effects with logiWheelPlayEffect(&id, params) and stops them, then changes them
 *   with logiWheelUpdateEffect(id, params) while driving:
 *     type 0 constant force   a = level
 *     type 7 spring            a = centre on the steering scale (-32768..32767), c/d = coefficients,
 *                              e/f = saturations: the road feel, a spring that follows the car
 *     type 8 damper            c/d = coefficients, e/f = saturations
 *     type 2 periodic (x2)     a = magnitude, c = period in ms
 *   Levels, coefficients and saturations are on Logitech's 8-bit scale (logiWheel.prx clamps them, +0x2e64080).
 *   Measured on a Moza R3 through DirectInput: a positive constant force turns the wheel left, a positive spring
 *   coefficient centres it.
 * DirectInput can take a long time to answer on some PCs, so the haptic device lives on a thread of its own: the
 * game only records what it wants, and that thread passes it on. Included by runtime_pad.c. */

#define LOGI_OK 0
#define LOGI_ERR_ARG ((int32_t)0x80000001)
#define LOGI_ERR_NOT_CONNECTED ((int32_t)0x80000004)
#define LOGI_EFFECTS 16
#pragma pack(push,1)
typedef struct {
    uint8_t type; int32_t duration, delay; uint8_t flags;
    int32_t a, b, c, d, e, f, g, h; uint8_t tail[4];
} LogiEffectParams;
#pragma pack(pop)
_Static_assert(sizeof(LogiEffectParams)==0x2e,"logiWheel effect parameters (guest +0xe12e9a)");
enum { LOGI_CONSTANT=0, LOGI_PERIODIC=2, LOGI_SPRING=7, LOGI_DAMPER=8 };

static struct {
    pthread_mutex_t m;
    bool started;
    struct { bool used, playing, dirty; LogiEffectParams p; } fx[LOGI_EFFECTS];
    bool autocenter, autocenter_dirty;
} ffb={.m=PTHREAD_MUTEX_INITIALIZER,.autocenter=true};

static PadData wheel_last_read;   /* the last read of the wheel port, under lock */
static int ffb_car_percent=100;   /* the physics model's force for the car being driven (ffb_gain_buttons) */
static int32_t rd_clamp(int32_t v, int32_t lo, int32_t hi) { return v<lo ? lo : v>hi ? hi : v; }
static Sint16 level16(int32_t v) { return (Sint16)(rd_clamp(v,-255,255)*32767/255); }
static Uint16 sat16(int32_t v) { return (Uint16)(rd_clamp(v,0,255)*65535/255); }
/* The game's vibrations: its 55..82 ms wave comes on kerbs and rumble strips, its 20..30 ms wave is the road surface. */
static bool ffb_wave_is_kerb(const LogiEffectParams *p) { return p->c>=45; }
static int ffb_wave_percent(const LogiEffectParams *p) {
    const int own=ffb_wave_is_kerb(p) ? ffb_kerbs : ffb_texture;
    return own>=0 ? own : ffb_vibration;
}
/* A wave's magnitude for the wheel. The game's waves go up to 64 (a quarter of Logitech's scale). Its strongest is
 * wheel_ffb_kerbs / wheel_ffb_texture percent of wheel_ffb_vibration_limit, so those scale inside the limit instead
 * of being cut off by it. */
static Uint16 ffb_wave_level(float raw, const LogiEffectParams *p) {
    /* the physics model: the kerb wave at wheel_ffb_physics_kerbs (100: 30% of the wheel at the game's strongest),
     * the surface wave with the bumps at wheel_ffb_physics_road (100: 8%) */
    if (ffb_physics) {
        const float share=(ffb_wave_is_kerb(p) ? (float)ffb_ph_kerbs/100.0f*0.3f : (float)ffb_ph_road/100.0f*0.08f)*
                          (float)ffb_car_percent/100.0f;
        return (Uint16)(fminf(1.0f,raw/64.0f*share)*32767.0f);
    }
    /* wheel_ffb_vibration_response below 100 lifts the small ones (slow gravel) and keeps the strongest at the limit */
    const float level=powf(fminf(1.0f,raw/64.0f),(float)ffb_vibration_response/100.0f);
    const float x=level*(float)ffb_wave_percent(p)/100.0f*(float)ffb_vibration_limit/100.0f;
    return (Uint16)(fminf(1.0f,x)*32767.0f);
}
/* pos: where the wheel is now, on the game's steering scale. */
static bool ffb_effect_from(const LogiEffectParams *p, SDL_HapticEffect *e, int32_t pos) {
    memset(e,0,sizeof(*e));
    const int sign=ffb_invert ? -1 : 1;
    switch (p->type) {
    case LOGI_CONSTANT:
        e->type=SDL_HAPTIC_CONSTANT;
        e->constant.direction.type=SDL_HAPTIC_CARTESIAN; e->constant.direction.dir[0]=1;
        e->constant.length=SDL_HAPTIC_INFINITY;
        e->constant.level=(Sint16)(sign*level16(p->a*ffb_gain/100));
        return true;
    case LOGI_PERIODIC:
        e->type=ffb_sine ? SDL_HAPTIC_SINE : SDL_HAPTIC_SQUARE;
        e->periodic.direction.type=SDL_HAPTIC_CARTESIAN; e->periodic.direction.dir[0]=1;
        e->periodic.length=SDL_HAPTIC_INFINITY;
        e->periodic.period=(Uint16)rd_clamp(p->c,1,10000);
        e->periodic.magnitude=ffb_wave_level((float)(p->a<0 ? -p->a : p->a),p);
        return true;
    case LOGI_SPRING: case LOGI_DAMPER:
        e->type=p->type==LOGI_SPRING ? SDL_HAPTIC_SPRING : SDL_HAPTIC_DAMPER;
        e->condition.direction.type=SDL_HAPTIC_CARTESIAN; e->condition.direction.dir[0]=1;
        e->condition.length=SDL_HAPTIC_INFINITY;
        e->condition.right_coeff[0]=level16(p->c); e->condition.left_coeff[0]=level16(p->d);
        /* the saturation caps the force; wheel_ffb_gain lifts the cap with the force. The damper has its own
         * wheel_ffb_damper: following the gain it makes a strong wheel heavy without any more feel. */
        if (p->type==LOGI_DAMPER && ffb_damper>=0) {
            e->condition.right_coeff[0]=level16(p->c*ffb_damper/100); e->condition.left_coeff[0]=level16(p->d*ffb_damper/100);
            e->condition.right_sat[0]=sat16(p->e*ffb_damper/100); e->condition.left_sat[0]=sat16(p->f*ffb_damper/100);
        } else if (p->type==LOGI_SPRING && ffb_spring_limit>0) {
            /* The saturation is the game's grip: scaled into wheel_ffb_spring_limit, not clipped by it, so the
             * spring still lightens as the tyres slide. Near the centre the pull stays under it anyway. */
            e->condition.right_sat[0]=(Uint16)(sat16(p->e)*ffb_spring_limit/100);
            e->condition.left_sat[0]=(Uint16)(sat16(p->f)*ffb_spring_limit/100);
        } else {
            e->condition.right_sat[0]=sat16(p->e*ffb_gain/100); e->condition.left_sat[0]=sat16(p->f*ffb_gain/100);
        }
        if (p->type==LOGI_SPRING) {
            /* The centre is where the tyres want the wheel, on the steering scale itself (-32768..32767, as the
             * wheel position the game reads): DriveClub's road feel is this spring following the car. The game
             * asks for the stiffest spring and the wheel rarely sits more than a few percent from that centre, so
             * the force stays well under the saturation: lifting the saturation alone changes nothing. A spring
             * pulls in proportion to how far the wheel is from its centre, so wheel_ffb_gain moves the centre
             * that much further from where the wheel is now (the haptic thread follows the wheel every 4 ms).
             * pos is already on the game's scale: wheel_value flips the axis of an inverted wheel. */
            const int32_t target=pos+(int32_t)((int64_t)(p->a-pos)*(ffb_spring>=0 ? ffb_spring : ffb_gain)/100);
            e->condition.center[0]=(Sint16)rd_clamp(sign*target,-32767,32767);
            e->condition.deadband[0]=(Uint16)(rd_clamp(p->b,0,255)*65535/255);
        }
        return true;
    }
    return false;
}
/* The player's car in the game's simulation, kept by the force feedback inputs hook (runtime_pad.c): 0 while no car
 * is driven. step_us: when the game last passed its inputs (a physics step). */
static struct { volatile uintptr_t state, setup, sim; uint64_t step_us; } ffb_car;
static void ffb_hook_install(void);

/* ---- wheel_ffb_model = software: the steering force worked out here ---------------------------------------------- */
static int logi_range_deg;   /* the steering lock the game last asked for (logiWheelSetOperatingRange), 0 until then */
static struct {
    int id; bool running, init;
    float target, cap, force;     /* the game's spring target and grip, smoothed; the last force (+: to the right) */
    float last_speed;             /* the wheel's speed at the last pass, degrees a second */
    int32_t last_pos; uint64_t last_us; Sint16 sent; int runaway;
    unsigned passes, clipped; uint64_t meter_since;
} sw={.id=-1};
static void ffb_software_off(SDL_Haptic *haptic, const char *why) {
    if (sw.id>=0) { SDL_DestroyHapticEffect(haptic,sw.id); sw.id=-1; }
    sw.running=false; sw.init=false;
    ffb_software=false;
    pthread_mutex_lock(&ffb.m);   /* the game's spring and constant force go to the wheel again */
    for (int i=0;i<LOGI_EFFECTS;++i) if (ffb.fx[i].used) ffb.fx[i].dirty=true;
    pthread_mutex_unlock(&ffb.m);
    printf("Runtime: FFB software model off: %s; the game's spring is used\n",why);
}
/* One pass: the spring force from the game's target and grip, the game's constant force and the soft lock, as one
 * constant force. pos: the wheel on the game's steering scale. */
static void ffb_software_pass(SDL_Haptic *haptic, const LogiEffectParams *want, const bool *used, const bool *playing, int32_t pos) {
    const LogiEffectParams *spring=NULL; int32_t game_const=0;
    for (int i=0;i<LOGI_EFFECTS;++i) if (used[i] && playing[i]) {
        if (want[i].type==LOGI_SPRING) spring=&want[i];
        else if (want[i].type==LOGI_CONSTANT) game_const=want[i].a;
    }
    const uint64_t now=now_us();
    float dt=sw.last_us ? (float)(now-sw.last_us)*1e-6f : 0.004f;
    if (dt<0.0005f) dt=0.0005f;
    if (dt>0.05f) dt=0.05f;
    sw.last_us=now;
    const int32_t vel=sw.init ? pos-sw.last_pos : 0;
    sw.last_pos=pos;
    const float deg_per_unit=(float)wheel_rotation*0.5f/32767.0f;
    float spring_force=0.0f;   /* +: toward the game's right */
    if (spring) {
        const float s=(float)(ffb_spring>=0 ? ffb_spring : ffb_gain)/100.0f;
        /* wheel_ffb_slide: the game's grip to a power above 1, so a sliding car lightens the wheel more */
        const float sat=powf((float)rd_clamp(spring->e,0,255)/255.0f,1.0f+(float)ffb_slide/100.0f);
        const float cap=ffb_spring_limit>0 ? sat*(float)ffb_spring_limit/100.0f : fminf(1.0f,sat*s);
        const float k=ffb_smoothing_ms>0 ? 1.0f-expf(-dt*1000.0f/(float)ffb_smoothing_ms) : 1.0f;
        if (!sw.init) { sw.target=(float)spring->a; sw.cap=cap; sw.init=true; }
        sw.target+=((float)spring->a-sw.target)*k;
        sw.cap+=(cap-sw.cap)*k;
        /* the game's spring: the full force at a full-scale offset, times wheel_ffb_spring */
        const float pull=(sw.target-(float)pos)/32767.0f*s;
        if (sw.cap>0.001f) {
            /* Up to the grip the game asks for the force is linear, every detail kept; past it it falls off by
             * wheel_ffb_falloff, the way a tyre's self-aligning torque does past its peak, instead of holding there. */
            /* wheel_ffb_center bends the part below the limit: (1+c)u/(1+cu) rises 1+c times as steeply off the
             * target, still meets the limit at u=1, and its slope stays finite (no oscillation on a direct drive) */
            const float u=fabsf(pull)/sw.cap, c=(float)ffb_center/25.0f;
            const float g=u<=1.0f ? (1.0f+c)*u/(1.0f+c*u) : 1.0f-(float)ffb_falloff/100.0f*(1.0f-expf(-(u-1.0f)));
            spring_force=(pull<0.0f ? -g : g)*sw.cap;
        }
        /* The force always pulls toward the target. A wheel that still speeds up away from it, fast, pass after pass,
         * is being pushed by the force: it goes the wrong way on this wheel and a direct drive would spin. (A hand
         * turning against the force slows down.) Back to the game's spring. */
        const float speed=fabsf((float)vel*deg_per_unit/dt);   /* degrees a second */
        const bool away=vel && ((float)pos>sw.target)==(vel>0);
        if (speed>1500.0f && away && speed>=sw.last_speed && fabsf(spring_force)>0.2f) {
            if (++sw.runaway>=8) { ffb_software_off(haptic,"the wheel sped up against the force"); return; }
        } else sw.runaway=0;
        sw.last_speed=speed;
    }
    float force=spring_force*(float)ffb_strength/100.0f;
    /* soft lock: the full force within 3 degrees past the lock, and a brake on the way out */
    const int lock=ffb_soft_lock<0 ? logi_range_deg : ffb_soft_lock;
    if (lock>=90 && lock<wheel_rotation) {
        const float angle=(float)pos*deg_per_unit, over=fabsf(angle)-(float)lock*0.5f;
        if (over>0.0f) {
            const float out=angle>0.0f ? 1.0f : -1.0f;
            const float out_speed=(float)vel*deg_per_unit/dt*out;   /* degrees a second further out */
            force+=-out*(fminf(1.0f,over/3.0f)+(out_speed>0.0f ? fminf(0.5f,out_speed*0.001f) : 0.0f));
        }
    }
    force=fmaxf(-1.0f,fminf(1.0f,force));
    sw.force=force;
    /* clipping meter: how often the force is at the wheel's maximum while the game drives the wheel */
    if (spring) { ++sw.passes; if (fabsf(force)>=0.98f) ++sw.clipped; }
    if (!sw.meter_since) sw.meter_since=now;
    if (now-sw.meter_since>=30000000) {
        if (sw.passes) printf("Runtime: FFB clipping %.1f%% of the last 30 s\n",100.0*sw.clipped/sw.passes);
        sw.passes=sw.clipped=0; sw.meter_since=now;
    }
    /* A positive constant force turns the wheel left. The force is on the game's scale like pos (wheel_value flips
     * the axis of an inverted wheel), so no flip here; wheel_ffb_invert for a wheel that pulls the wrong way. */
    const float dir=-1.0f*(ffb_invert ? -1.0f : 1.0f);
    int32_t level=(int32_t)lroundf(force*dir*32767.0f)+(ffb_invert ? -1 : 1)*level16(game_const*ffb_gain/100);
    level=rd_clamp(level,-32767,32767);
    SDL_HapticEffect e;
    memset(&e,0,sizeof e);
    e.type=SDL_HAPTIC_CONSTANT;
    e.constant.direction.type=SDL_HAPTIC_CARTESIAN; e.constant.direction.dir[0]=1;
    e.constant.length=SDL_HAPTIC_INFINITY;
    e.constant.level=(Sint16)level;
    if (sw.id<0) {
        if ((sw.id=SDL_CreateHapticEffect(haptic,&e))<0) { ffb_software_off(haptic,SDL_GetError()); return; }
        sw.sent=e.constant.level;
        printf("Runtime: FFB software model on: strength %d%%, falloff %d%%, smoothing %d ms, soft lock %d degrees\n",
               ffb_strength,ffb_falloff,ffb_smoothing_ms,lock);
    } else if (abs(e.constant.level-sw.sent)>=16) {
        if (SDL_UpdateHapticEffect(haptic,sw.id,&e)) sw.sent=e.constant.level;
    }
    if (!sw.running) sw.running=SDL_RunHapticEffect(haptic,sw.id,1);
}

/* ---- wheel_ffb_model = physics: the steering force from the car's front tyres ------------------------------------
 * The game's own force follows the front tyres' lateral force alone (the car state's +0xc1c, which its Logitech spring
 * is made from, is that force over about 2300 N). In a sim the force is the moment the front tyres put on the
 * steering: each tyre's lateral force times its trail, the suspension's mechanical trail plus the tyre's pneumatic
 * trail. The pneumatic trail shrinks as more of the contact patch slides (as in the brush tyre model, to nothing when
 * all of it slides), so the wheel goes light as the front washes out and follows the front tyres into a slide.
 * The car's state (runtime_pad.c keeps the pointer), per wheel in the game's order right front, left front, right
 * rear, left rear (the car's x axis points left): +0xa50 load (N), +0xa60 lateral force (N), +0xb30 slip ratio,
 * +0xb50 slip angle (rad). The block the game passes on: +0x18 speed (m/s), +0x2c the front load over half the car's
 * weight. Measured with the game's AI and a fixed steering input: steering right, the slip angles go up and the
 * lateral forces down, and the left front takes the load. */
static bool ffb_log(void);
#define PH_MECH_TRAIL 0.35f   /* the mechanical trail as a share of the pneumatic trail of a tyre that grips */
#define PH_NORM 1.18f         /* the moment of a 1 g corner over the front's weight: 60% of the wheel at gain 100 */
static float smoothstep(float lo, float hi, float x) { const float t=fminf(1.0f,fmaxf(0.0f,(x-lo)/(hi-lo))); return t*t*(3.0f-2.0f*t); }
static struct {
    pthread_mutex_t m;
    uintptr_t car; uint64_t us;   /* the car (its setup) and its last physics step */
    bool valid;
    float torque;                 /* the steering moment over the front's weight and PH_NORM, +: turns the wheel right */
    float road, slip, speed;      /* road: the moment of bumps, same scale; slip: 0..1 */
    float k, mu, weight;          /* cornering stiffness over load (1/rad), peak friction, half the car's weight (N) */
    float lp_load[2], x[2];       /* the front loads' recent average; how much of each front tyre slides, 0..1 */
    uint64_t key;                 /* the car's key for its own gain (ffb_car_key) */
    float relaxed[2]; bool relaxed_set;   /* each front tyre's moment after its relaxation length */
} ph={.m=PTHREAD_MUTEX_INITIALIZER};
/* BB_FFB_TRACE=1: every physics step of the player's car (P rows) and every pass of the haptic thread (H rows) to
 * logs/ffb_trace_<time>.csv, for measuring how the force follows the car: how often the game's physics comes, how long
 * the force takes to reach the wheel, how it builds and drops with the tyres' slip, and (wheel position against the
 * car's steering angle) how the game follows the wheel. Opened by ffb_start before the haptic thread runs; diagnostic
 * builds only (BB_DIAGNOSTICS=ON), as BB_FFB_LOG. */
static FILE *ffb_trace_file;
static pthread_mutex_t ffb_trace_m=PTHREAD_MUTEX_INITIALIZER;
static uint64_t ffb_trace_t0;
static void ffb_trace_open(void) {
    const char *e=BB_DIAGNOSTIC_ENV("BB_FFB_TRACE");
    if (!e || !*e || *e=='0') return;
    const char *dir=getenv("BB_CRASH_DIR");   /* the launcher's logs folder */
    char path[700];
    const time_t t=time(NULL);
    const struct tm *tm=localtime(&t);
    snprintf(path,sizeof path,"%s/ffb_trace_%04d%02d%02d_%02d%02d%02d.csv",dir && *dir ? dir : ".",
             tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday,tm->tm_hour,tm->tm_min,tm->tm_sec);
    if (!(ffb_trace_file=fopen(path,"wb"))) { printf("Runtime: FFB trace not written: %s\n",path); return; }
    setvbuf(ffb_trace_file,NULL,_IOFBF,1<<20);
    ffb_trace_t0=now_us();
    fprintf(ffb_trace_file,
            "# P: a physics step of the player's car. Wheels right front, left front, right rear, left rear; slip angles in rad\n"
            "# (+ steering right), forces in N (lateral negative turning right), torque = the model's steering moment (+ right).\n"
            "# H: a pass of the haptic thread. pos: the wheel in degrees from the centre (+ right); want: torque times the\n"
            "# gains; out: after the filter; force: to the wheel (-1..1, + right) before the device's direction; sent: the\n"
            "# effect was updated; age_us: how old the physics step behind it is.\n"
            "P,t_ms,speed,steer,slip_rf,slip_lf,slip_rr,slip_lr,fy_rf,fy_lf,fy_rr,fy_lr,fz_rf,fz_lf,fz_rr,fz_lr,fx_rf,fx_lf,fx_rr,fx_lr,ratio_rf,ratio_lf,torque,slide_rf,slide_lf,road,k,mu,weight\n"
            "H,t_ms,pos,want,out,force,sent,age_us\n");
    printf("Runtime: FFB trace to %s\n",path);
}

/* A car's key for its own force feedback gain (ffb_cars.ini): values of its setup and drivetrain that stay the same
 * while it is driven and from one session to the next (the steering lock, engine figures), hashed. */
static uint64_t ffb_car_key(uintptr_t setup) {
    static const uint16_t setup_at[]={0x45c,0x704,0x718}, drive_at[]={0x0c,0x3c,0x40};
    float v[6]={0};
    for (int i=0;i<3;++i) memcpy(&v[i],(const uint8_t *)setup+setup_at[i],4);
    uintptr_t drive; memcpy(&drive,(const uint8_t *)setup+8,8);
    if (drive) for (int i=0;i<3;++i) memcpy(&v[3+i],(const uint8_t *)drive+drive_at[i],4);
    uint64_t h=1469598103934665603ull;   /* FNV-1a */
    for (size_t i=0;i<sizeof v;++i) h=(h^((const uint8_t *)v)[i])*1099511628211ull;
    printf("Runtime: FFB car %016llx (setup %g %g %g, drivetrain %g %g %g)\n",(unsigned long long)h,v[0],v[1],v[2],v[3],v[4],v[5]);
    return h;
}
/* One physics step of the game (its thread, from the inputs hook): the force the front tyres put on the steering. */
static void ffb_physics_step(const float *st, const uint8_t *in, uintptr_t car) {
    float speed, front;
    memcpy(&speed,in+0x18,4); memcpy(&front,in+0x2c,4);
    const float *load=st+0xa50/4, *lat=st+0xa60/4, *lon=st+0xa70/4, *ratio=st+0xb30/4, *slip=st+0xb50/4;
    const uint64_t now=now_us();
    pthread_mutex_lock(&ph.m);
    if (ph.car!=car) {   /* another car: its tyres are learnt again from the usual values */
        ph.car=car; ph.us=0; ph.k=24.0f; ph.mu=1.3f; ph.weight=0.0f; ph.lp_load[0]=ph.lp_load[1]=0.0f;
        ph.relaxed_set=false;
        ph.key=ffb_car_key(car);
    }
    float dt=ph.us ? (float)(now-ph.us)*1e-6f : 0.016f;
    dt=fminf(0.1f,fmaxf(0.001f,dt));
    ph.us=now;
    if (front>0.2f) {
        const float w=(load[0]+load[1])/front;
        if (w>500.0f && w<50000.0f) ph.weight=ph.weight>0.0f ? ph.weight+(w-ph.weight)*0.01f : w;
    }
    if (ph.weight<=0.0f) { ph.valid=false; pthread_mutex_unlock(&ph.m); return; }
    /* The car's tyres: the lateral force per load and slip angle while they grip, and the most they give. Combined
     * (wheel_ffb_physics_combined): the stiffness only while the tyre does not brake or drive much, and the most it
     * gives from braking, drive and cornering together (its friction circle), learnt where it slides either way. The
     * game's tyre has about one budget for both: a player's straight-line braking peaked at 1.26-1.28 of the load, the
     * cornering plateau of that car is 1.22; learnt from cornering alone, braking in corners left mu too low (1.22
     * for the 458, whose plateau is about 1.28). */
    for (int i=0;i<2;++i) if (speed>10.0f && load[i]>1000.0f) {
        const float a=fabsf(slip[i]), f=fabsf(lat[i])/load[i];
        if (a>0.004f && a<0.02f && (!ffb_ph_combined || fabsf(lon[i])<0.1f*load[i])) ph.k+=(fminf(60.0f,fmaxf(8.0f,f/a))-ph.k)*0.002f;
        else if (ffb_ph_combined) {
            if (a>0.12f || fabsf(ratio[i])>0.1f) ph.mu+=(fminf(2.5f,fmaxf(0.6f,hypotf(lat[i],lon[i])/load[i]))-ph.mu)*0.01f;
        } else if (a>0.12f) ph.mu+=(fminf(2.5f,fmaxf(0.6f,f))-ph.mu)*0.01f;
    }
    const float a_slide=3.0f*ph.mu/ph.k, a_peak=2.0f*ph.mu/ph.k;   /* all the patch slides; the most lateral force */
    /* wheel_ffb_physics_tyre = grip: where the game's tyre runs out of grip, about 1.6 mu/k (measured on the 458 and the
     * Zonda R: its lateral force is linear to about mu/k and flat from about 1.6 mu/k on) */
    const float a_sat=1.6f*ph.mu/ph.k;
    float moment=0.0f, slipping=0.0f, tyre[2]={0.0f,0.0f};   /* each front tyre's moment on the steering */
    for (int i=0;i<2;++i) {
        if (load[i]<50.0f) { ph.x[i]=1.0f; continue; }   /* in the air */
        if (ffb_ph_tyre_grip) {
            /* The pneumatic trail from the grip in use, u = (Fy/Fz)/mu from the game's own forces: (1-u^4) keeps most of it
             * to about 70% of the grip and takes it away by the limit, so the force peaks well before the limit and is
             * about half of that at it (measured on the 458: -24% at 4.9 degrees of slip, -51% where the tyre saturates,
             * where classic gives -8% and -26%). 0.78 keeps the peak where classic has it, so the gains mean the same.
             * Past the limit the game's lateral force stays flat where a real tyre's falls off: the force falls by up to
             * 30% over the next 14 degrees of slip, so countersteer brings it back step by step.
             * Combined: the grip in use from braking or drive and cornering together, sqrt(Fy^2+Fx^2)/(Fz mu): the more
             * of the patch the braking makes slide, the shorter the trail, so braking hard into a corner lightens the
             * wheel as the front nears its limit, as a real tyre's aligning moment does (a player's race: 11% of the
             * cornering with braking, the grip in use 0.92 against 0.69 from cornering alone). */
            const float used=ffb_ph_combined ? hypotf(lat[i],lon[i]) : fabsf(lat[i]);
            const float u=fminf(1.0f,used/load[i]/ph.mu), u2=u*u;
            const float past=fmaxf(0.0f,fabsf(slip[i])-a_sat), fall=1.0f-0.3f*fminf(1.0f,past/0.25f);
            ph.x[i]=u;
            tyre[i]=lat[i]*fall*0.78f*(PH_MECH_TRAIL+1.0f-u2*u2);
        } else {
            const float x=fminf(1.0f,fabsf(tanf(slip[i]))/tanf(a_slide)), r=1.0f-x;
            ph.x[i]=x;
            tyre[i]=lat[i]*(PH_MECH_TRAIL+r*r*r/(1.0f-x+x*x/3.0f));
        }
        slipping=fmaxf(slipping,fabsf(slip[i])/a_peak-1.0f);
    }
    /* The front tyres' braking and drive forces (+0xa70, negative braking) through the scrub radius: the steering axis
     * meets the road a little inside each tyre's contact patch (about 1 cm on a road car, as the caster trail here; the
     * game does not give its geometry), so each tyre's force turns its wheel about that axis. Equal forces cancel; a
     * difference turns the steering: the harder braking wheel pulls it to its side (a kerb, a bump, the grass under
     * braking, ABS), the harder driven one pushes it away (torque steer). In the moment's units above (the pneumatic
     * trail of a gripping tyre, about 2.5 cm), scaled as the grip curve is. The 458 and Zonda R on the AI benchmark: the
     * fronts' forces differ by 1-1.4 kN in the hardest 10% of braking, a few percent of the steering force; kerbs and
     * the grass give several kN. */
    if (ffb_ph_scrub) {
        const float scrub=0.4f*(ffb_ph_tyre_grip ? 0.78f : 1.0f);
        tyre[0]-=lon[0]*scrub;   /* the right front */
        tyre[1]+=lon[1]*scrub;
    }
    /* Relaxation: a tyre's forces build up over the first part of a wheel turn, its relaxation length (about 0.4 m on a
     * road car), not in an instant, and its load cannot follow a single physics step. The game's kerb strikes and
     * landings put 15-41 kN (two to five times the car's weight) on one front tyre for one step, which gave a full-force
     * hit; each tyre's moment now follows over 0.4 m of road: 15 ms at 100 km/h, at most 50 ms. A player's race replayed:
     * steps that jump by more than 0.3 of the wheel's force from 69 to 23, the biggest from 1.69 to 0.66; the force
     * otherwise about one physics step (9 ms) later. A tyre in the air lets go the same way. */
    if (ffb_ph_relax) {
        const float tau=fminf(0.05f,fmaxf(0.003f,0.4f/fmaxf(speed,1.0f))), k=1.0f-expf(-dt/tau);
        if (!ph.relaxed_set) { ph.relaxed[0]=tyre[0]; ph.relaxed[1]=tyre[1]; ph.relaxed_set=true; }
        for (int i=0;i<2;++i) tyre[i]=ph.relaxed[i]+=(tyre[i]-ph.relaxed[i])*k;
    } else ph.relaxed_set=false;
    moment=tyre[0]+tyre[1];
    for (int i=0;i<4;++i) slipping=fmaxf(slipping,(fabsf(ratio[i])-0.12f)*5.0f);   /* wheelspin, locked wheels */
    const float moving=smoothstep(1.0f,4.0f,speed);   /* slip angles mean little at a crawl */
    /* Self-centring from the steering axis' inclination: steering lifts the car a little on its front wheels, so their
     * load turns the steering back, in proportion to the sine of the steering angle (st+0xe4). About 1 cm of lever on
     * a road car (the game does not give its geometry), in the units above: 13% of a 1 g corner's force at 17 degrees of
     * front wheel angle, 24% at full lock, 1-2% at the angles of fast corners. Not at a standstill: the tyres' grip on the
     * road holds the wheels where they are until they roll (the damper is that resistance). */
    float centre=0.0f;
    if (ffb_ph_centre) centre=-sinf(st[0xe4/4])*(load[0]+load[1])*0.4f*(ffb_ph_tyre_grip ? 0.78f : 1.0f)*smoothstep(0.0f,1.0f,speed);
    ph.torque=(moment*moving+centre)/(ph.weight*PH_NORM);
    /* Road: each front tyre's load against its average of the last 0.08 s; a bump on one side turns the wheel */
    const float kf=1.0f-expf(-dt/0.08f);
    float bump[2];
    for (int i=0;i<2;++i) {
        if (ph.lp_load[i]<=0.0f) ph.lp_load[i]=load[i];
        ph.lp_load[i]+=(load[i]-ph.lp_load[i])*kf;
        bump[i]=load[i]-ph.lp_load[i];
    }
    ph.road=fminf(0.5f,fmaxf(-0.5f,(bump[1]-bump[0])/ph.weight*0.5f))*moving;
    ph.slip=fminf(1.0f,fmaxf(0.0f,slipping))*smoothstep(2.0f,6.0f,speed);
    ph.speed=speed;
    ph.valid=true;
    if (ffb_trace_file) {
        const float *fx=st+0xa70/4;
        pthread_mutex_lock(&ffb_trace_m);
        fprintf(ffb_trace_file,"P,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.3f,%.3f,%.4f,%.3f,%.3f,%.4f,%.2f,%.3f,%.0f\n",
                (double)(now-ffb_trace_t0)*1e-3,speed,st[0xe4/4],slip[0],slip[1],slip[2],slip[3],lat[0],lat[1],lat[2],lat[3],
                load[0],load[1],load[2],load[3],fx[0],fx[1],fx[2],fx[3],ratio[0],ratio[1],ph.torque,ph.x[0],ph.x[1],ph.road,
                ph.k,ph.mu,ph.weight);
        pthread_mutex_unlock(&ffb_trace_m);
    }
    /* BB_FFB_LOG: ten lines a second of the model while driving */
    static uint64_t logged; static unsigned lines;
    if (ffb_log() && now-logged>=100000 && lines<6000) {
        logged=now; ++lines;
        printf("FFB physics: %.0f km/h steer %+.3f | slip %+.3f %+.3f lat %+6.0f %+6.0f load %5.0f %5.0f | slide %.2f %.2f | moment %+.2f road %+.2f slip %.2f | k %.1f mu %.2f weight %.0f\n",
               speed*3.6f,st[0xe4/4],slip[0],slip[1],lat[0],lat[1],load[0],load[1],ph.x[0],ph.x[1],ph.torque,ph.road,ph.slip,ph.k,ph.mu,ph.weight);
    }
    pthread_mutex_unlock(&ph.m);
}
/* True while a car is driven: its physics steps come in. */
static bool ffb_physics_live(void) {
    pthread_mutex_lock(&ph.m);
    const bool live=ph.valid && ffb_car.state && now_us()-ph.us<300000;
    pthread_mutex_unlock(&ph.m);
    return live;
}
static bool ffb_zero;   /* BB_FFB_DRY=2: every force at zero strength */
static struct {
    int id, damper_id, slip_id; bool running, damper_running, slip_running;
    Sint16 sent; Uint16 damper_sent, slip_sent;
    float out, force; int32_t last_pos; uint64_t last_us; bool init;
    unsigned passes, clipped; uint64_t meter_since;
} pw={.id=-1,.damper_id=-1,.slip_id=-1};
static void ffb_physics_stop(SDL_Haptic *haptic) {
    if (pw.id>=0) SDL_DestroyHapticEffect(haptic,pw.id);
    if (pw.damper_id>=0) SDL_DestroyHapticEffect(haptic,pw.damper_id);
    if (pw.slip_id>=0) SDL_DestroyHapticEffect(haptic,pw.slip_id);
    pw.id=pw.damper_id=pw.slip_id=-1; pw.running=pw.damper_running=pw.slip_running=false; pw.init=false;
}
/* Creates or changes an effect and keeps it running; false when the wheel refuses it. */
static bool ffb_physics_effect(SDL_Haptic *haptic, int *id, bool *running, const SDL_HapticEffect *e) {
    if (*id<0) {
        if ((*id=SDL_CreateHapticEffect(haptic,e))<0) {
            static unsigned n; if (n++<4) printf("Runtime: FFB physics: effect type %u not created: %s\n",e->type,SDL_GetError());
            return false;
        }
    } else if (!SDL_UpdateHapticEffect(haptic,*id,e)) return false;
    if (!*running) *running=SDL_RunHapticEffect(haptic,*id,1);
    return true;
}
/* ---- the force for each car: wheel_button = <n>, ffb_gain_up / ffb_gain_down -------------------------------------
 * The physics model's force (steering, road, kerbs and slip; not the damper or the soft lock) times a percentage for the
 * car being driven: 100 until its buttons are pressed, then kept in ffb_cars.ini in the user folder. The player's own
 * wheel_ffb_physics_* settings stay as they are. */
static struct { uint64_t key; int percent; } ffb_cars[512];
static int ffb_car_count;
static bool ffb_cars_loaded;
static void ffb_cars_path(char *path, size_t size) {
    const char *user=runtime_file_user_dir();
    snprintf(path,size,"%s/ffb_cars.ini",user && *user ? user : ".");
}
static int ffb_car_find(uint64_t key) {
    if (!ffb_cars_loaded) {
        ffb_cars_loaded=true;
        char path[600], line[128];
        ffb_cars_path(path,sizeof path);
        FILE *f=fopen(path,"rb");
        while (f && fgets(line,sizeof line,f) && ffb_car_count<(int)(sizeof ffb_cars/sizeof *ffb_cars)) {
            unsigned long long k; int percent;
            if (sscanf(line," car_%llx = %d",&k,&percent)==2 && percent>=10 && percent<=300) {
                ffb_cars[ffb_car_count].key=k; ffb_cars[ffb_car_count++].percent=percent;
            }
        }
        if (f) fclose(f);
    }
    for (int i=0;i<ffb_car_count;++i) if (ffb_cars[i].key==key) return i;
    return -1;
}
static void ffb_cars_save(void) {
    char path[600], tmp[620];
    ffb_cars_path(path,sizeof path);
    snprintf(tmp,sizeof tmp,"%s.new",path);
    FILE *f=fopen(tmp,"wb");
    if (!f) return;
    fprintf(f,"# Force feedback for each car (Reworked Physics): percent of the force your wheel_ffb_physics settings give.\n"
              "# The wheel buttons set as ffb_gain_up and ffb_gain_down (WheelSetup: FFB gain + and -) change the car being driven.\n");
    for (int i=0;i<ffb_car_count;++i) fprintf(f,"car_%016llx = %d\n",(unsigned long long)ffb_cars[i].key,ffb_cars[i].percent);
    fclose(f);
    remove(path);
    rename(tmp,path);
}
/* Each pass of the haptic thread. car: the key of the car being driven, 0 while none is. */
static void ffb_gain_buttons(uint64_t car) {
    static uint64_t was_car;
    if (car!=was_car) {   /* another car: its own percentage */
        was_car=car;
        const int i=car ? ffb_car_find(car) : -1;
        ffb_car_percent=i>=0 ? ffb_cars[i].percent : 100;
        if (i>=0) printf("Runtime: FFB %d%% for car %016llx (ffb_cars.ini)\n",ffb_car_percent,(unsigned long long)car);
    }
    static bool was_down[2];
    bool down[2]={false,false};
    pthread_mutex_lock(&lock);
    for (int b=0;b<2;++b) if (wheel && ffb_gain_button[b]>=0) down[b]=SDL_GetJoystickButton(wheel,ffb_gain_button[b]);
    pthread_mutex_unlock(&lock);
    for (int b=0;b<2;++b) {
        const bool pressed=down[b] && !was_down[b];
        was_down[b]=down[b];
        if (!pressed) continue;
        if (!ffb_physics) { bbgpu_overlay_note("FFB gain per car: with Reworked Physics only"); continue; }
        if (!car) { bbgpu_overlay_note("FFB gain per car: drive a car first"); continue; }
        int i=ffb_car_find(car);
        if (i<0 && ffb_car_count<(int)(sizeof ffb_cars/sizeof *ffb_cars)) { i=ffb_car_count++; ffb_cars[i].key=car; ffb_cars[i].percent=100; }
        if (i<0) continue;
        const int percent=rd_clamp(ffb_cars[i].percent+(b ? -5 : 5),10,300);
        ffb_cars[i].percent=ffb_car_percent=percent;
        ffb_cars_save();
        char note[96];
        snprintf(note,sizeof note,"FFB %d%% for this car   (wheel: FFB gain + / -)",percent);
        bbgpu_overlay_note(note);
        printf("Runtime: FFB %d%% for car %016llx\n",percent,(unsigned long long)car);
    }
}

/* One pass of the haptic thread: the steering force, road and slip, the damper and the soft lock to the wheel.
 * pos: the wheel on the game's steering scale. */
static void ffb_physics_pass(SDL_Haptic *haptic, int32_t pos) {
    pthread_mutex_lock(&ph.m);
    const float torque=ph.torque, road=ph.road, slip=ph.slip, speed=ph.speed;
    const uint64_t step_us=ph.us;
    pthread_mutex_unlock(&ph.m);
    const float car_gain=(float)ffb_car_percent/100.0f;   /* this car's own percentage (ffb_gain_buttons) */
    const uint64_t now=now_us();
    float dt=pw.last_us ? (float)(now-pw.last_us)*1e-6f : 0.004f;
    dt=fminf(0.05f,fmaxf(0.0005f,dt));
    pw.last_us=now;
    const int32_t vel=pw.init ? pos-pw.last_pos : 0;
    pw.last_pos=pos;
    /* gain, road effect, filter (6 ms always: the game's physics steps come about 70 times a second, and a direct drive
     * feels each step; 100: 46 ms), minimum force (lifts the smallest forces, smoothly from zero) */
    const float want=(torque*(float)ffb_ph_gain/100.0f+road*(float)ffb_ph_road/100.0f)*car_gain;
    if (!pw.init) { pw.out=want; pw.init=true; }
    pw.out+=(want-pw.out)*(1.0f-expf(-dt*1000.0f/(6.0f+0.4f*(float)ffb_ph_filter)));
    float force=pw.out;
    if (ffb_ph_min_force>0) {
        const float m=(float)ffb_ph_min_force/100.0f, a=fabsf(force);
        force=copysignf(m*fminf(1.0f,a/0.02f)+(1.0f-m)*a,force);
    }
    /* soft lock, as the software model's */
    const float deg_per_unit=(float)wheel_rotation*0.5f/32767.0f;
    const int lock=ffb_soft_lock<0 ? logi_range_deg : ffb_soft_lock;
    if (lock>=90 && lock<wheel_rotation) {
        const float angle=(float)pos*deg_per_unit, over=fabsf(angle)-(float)lock*0.5f;
        if (over>0.0f) {
            const float out=angle>0.0f ? 1.0f : -1.0f, out_speed=(float)vel*deg_per_unit/dt*out;
            force+=-out*(fminf(1.0f,over/3.0f)+(out_speed>0.0f ? fminf(0.5f,out_speed*0.001f) : 0.0f));
        }
    }
    force=fmaxf(-1.0f,fminf(1.0f,force));
    pw.force=force;
    ++pw.passes; if (fabsf(force)>=0.98f) ++pw.clipped;
    if (ffb_zero) force=0.0f;
    if (!pw.meter_since) pw.meter_since=now;
    if (now-pw.meter_since>=30000000) {
        if (pw.passes) printf("Runtime: FFB clipping %.1f%% of the last 30 s (physics model, gain %d%%)\n",100.0*pw.clipped/pw.passes,ffb_ph_gain);
        pw.passes=pw.clipped=0; pw.meter_since=now;
    }
    /* a positive constant force turns the wheel toward its axis' low end (left, unless the axis is inverted) */
    const float dir=(wheel_inverted[WAX_STEER] ? 1.0f : -1.0f)*(ffb_invert ? -1.0f : 1.0f);
    SDL_HapticEffect e;
    memset(&e,0,sizeof e);
    e.type=SDL_HAPTIC_CONSTANT;
    e.constant.direction.type=SDL_HAPTIC_CARTESIAN; e.constant.direction.dir[0]=1;
    e.constant.length=SDL_HAPTIC_INFINITY;
    e.constant.level=(Sint16)rd_clamp((int32_t)lroundf(force*dir*32767.0f),-32767,32767);
    bool sent=false;
    if (pw.id<0 || abs(e.constant.level-pw.sent)>=16) {
        const bool first=pw.id<0;
        if (ffb_physics_effect(haptic,&pw.id,&pw.running,&e)) {
            pw.sent=e.constant.level;
            sent=true;
            if (first) printf("Runtime: FFB physics model on: gain %d%%, filter %d%%, minimum force %d%%, kerbs %d%%, road %d%%, slip %d%%, damper %d%%, tyre %s, scrub %s, combined %s, centre %s, relax %s\n",
                              ffb_ph_gain,ffb_ph_filter,ffb_ph_min_force,ffb_ph_kerbs,ffb_ph_road,ffb_ph_slip,ffb_ph_damper,
                              ffb_ph_tyre_grip ? "grip" : "classic",ffb_ph_scrub ? "on" : "off",ffb_ph_combined ? "on" : "off",
                              ffb_ph_centre ? "on" : "off",ffb_ph_relax ? "on" : "off");
        }
    }
    if (ffb_trace_file) {
        const uint64_t done=now_us();
        pthread_mutex_lock(&ffb_trace_m);
        fprintf(ffb_trace_file,"H,%.2f,%.2f,%.4f,%.4f,%.4f,%d,%lld\n",(double)(done-ffb_trace_t0)*1e-3,(double)((float)pos*deg_per_unit),
                want,pw.out,pw.force,sent,(long long)(step_us ? done-step_us : -1));
        static uint64_t flushed;   /* the process ends without closing it (runtime_ffb_release flushes too) */
        if (done-flushed>=500000) { fflush(ffb_trace_file); flushed=done; }
        pthread_mutex_unlock(&ffb_trace_m);
    }
    /* damper: the wheel's own, strongest at a standstill (the tyres scrub) and a third of that at speed */
    const Uint16 damping=ffb_zero ? 0 : (Uint16)(32767.0f*(float)ffb_ph_damper/100.0f*(0.35f+0.65f*(1.0f-smoothstep(0.0f,12.0f,speed))));
    if (!ffb_ph_damper && pw.damper_id>=0) { SDL_DestroyHapticEffect(haptic,pw.damper_id); pw.damper_id=-1; pw.damper_running=false; }
    if (ffb_ph_damper>0 && (SDL_GetHapticFeatures(haptic)&SDL_HAPTIC_DAMPER) && (pw.damper_id<0 || abs((int)damping-(int)pw.damper_sent)>=256)) {
        memset(&e,0,sizeof e);
        e.type=SDL_HAPTIC_DAMPER;
        e.condition.direction.type=SDL_HAPTIC_CARTESIAN; e.condition.direction.dir[0]=1;
        e.condition.length=SDL_HAPTIC_INFINITY;
        e.condition.right_coeff[0]=e.condition.left_coeff[0]=(Sint16)damping;
        e.condition.right_sat[0]=e.condition.left_sat[0]=65535;
        if (ffb_physics_effect(haptic,&pw.damper_id,&pw.damper_running,&e)) pw.damper_sent=damping;
    }
    /* slip: a buzz as the tyres slide past their grip, spin or lock (up to 25% of the wheel at 100) */
    const Uint16 buzz=ffb_zero ? 0 : (Uint16)(fminf(1.0f,slip*(float)ffb_ph_slip/100.0f*car_gain)*0.25f*32767.0f);
    if ((SDL_GetHapticFeatures(haptic)&SDL_HAPTIC_SINE) && (pw.slip_id<0 || abs((int)buzz-(int)pw.slip_sent)>=256)) {
        memset(&e,0,sizeof e);
        e.type=SDL_HAPTIC_SINE;
        e.periodic.direction.type=SDL_HAPTIC_CARTESIAN; e.periodic.direction.dir[0]=1;
        e.periodic.length=SDL_HAPTIC_INFINITY;
        e.periodic.period=25;
        e.periodic.magnitude=(Sint16)buzz;
        if (ffb_physics_effect(haptic,&pw.slip_id,&pw.slip_running,&e)) pw.slip_sent=buzz;
    }
}

/* The wheel_ffb_* lines of default.ini again when the file has changed (FFB Tuner saves it while the game runs);
 * checked about once a second by the haptic thread. True when they were read. */
static bool ffb_settings_reload(void) {
    static uint64_t last_check; static char seen[4096]; static bool have_seen;
    const uint64_t now=now_us();
    if (now-last_check<1000000) return false;
    last_check=now;
    char path[600];
    const char *user=runtime_file_user_dir();
    snprintf(path,sizeof path,"%s/input_config/default.ini",user && *user ? user : ".");
    FILE *f=fopen(path,"rb");
    if (!f) return false;
    /* the wheel_ffb lines as they are now (file times are too coarse for two saves in one second) */
    char lines[4096]=""; size_t used_len=0; char line[256];
    while (fgets(line,sizeof line,f)) {
        const char *s=line; while (*s==' ' || *s=='\t') ++s;
        const size_t n=strlen(s);
        if (SDL_strncasecmp(s,"wheel_ffb",9) || used_len+n>=sizeof lines) continue;
        memcpy(lines+used_len,s,n+1); used_len+=n;
    }
    fclose(f);
    if (!have_seen) { memcpy(seen,lines,sizeof seen); have_seen=true; return false; }   /* the game read them at start */
    if (!strcmp(seen,lines)) return false;
    memcpy(seen,lines,sizeof seen);
    pthread_mutex_lock(&lock);
    for (char *l=lines, *next; l && *l; l=next) {
        next=strchr(l,'\n');
        if (next) *next++=0;
        snprintf(line,sizeof line,"%s",l);
        parse_line(line);
    }
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&ffb.m);   /* every effect again with the new settings */
    for (int i=0;i<LOGI_EFFECTS;++i) ffb.fx[i].dirty=ffb.fx[i].used;
    pthread_mutex_unlock(&ffb.m);
    if (ffb_physics)
        printf("Runtime: FFB settings reloaded from default.ini (physics model: gain %d%%, filter %d%%, minimum force %d%%, kerbs %d%%, road %d%%, slip %d%%, damper %d%%, tyre %s)\n",
               ffb_ph_gain,ffb_ph_filter,ffb_ph_min_force,ffb_ph_kerbs,ffb_ph_road,ffb_ph_slip,ffb_ph_damper,
               ffb_ph_tyre_grip ? "grip" : "classic");
    else printf("Runtime: FFB settings reloaded from default.ini (%s model, strength %d%%, falloff %d%%)\n",
                ffb_software ? "software" : "game",ffb_strength,ffb_falloff);
    return true;
}

/* BB_FFB_LOG=1: the effects as the game sets them, and ten lines a second of the forces while driving (for tuning). */
static bool ffb_log(void) { static int on=-1; if (on<0) { const char *e=BB_DIAGNOSTIC_ENV("BB_FFB_LOG"); on=e && *e && *e!='0'; } return on; }
static void ffb_log_params(const char *what, int id, const LogiEffectParams *p) {
    static unsigned n;
    if (ffb_log() && n++<400) printf("Runtime: FFB %s %d: type %u a %d b %d c %d d %d e %d f %d\n",what,id,p->type,p->a,p->b,p->c,p->d,p->e,p->f);
}

/* Safety: the force the game set last keeps playing in the wheel until something stops it, after the game is gone
 * too (the runtime ends through _Exit or TerminateProcess): a direct drive base then spins or pushes into its lock
 * until it is reconnected. runtime_ffb_release asks the haptic thread to stop everything and let the wheel go before
 * the process ends; while the game does not read the wheel (hung, stuck loading) its forces are stopped.
 * Never through the device gain: some drivers keep it as the wheel's own setting (Thrustmaster writes it to
 * HKLM\...\tmhidusb\Parameters\<id>\OverallGain), so a gain of 0 left the wheel without force feedback in every
 * program, across reboots. Stopping effects is a command the driver does not keep. */
static volatile bool ffb_release_asked, ffb_release_done;
static const char *volatile ffb_release_why;
static uint64_t ffb_last_read_us;   /* when the game last read the wheel (logiWheelReadState) */
static bool ffb_held;               /* the forces are stopped because the game stopped reading the wheel */
/* At the start of each pass of the haptic thread. True: no pass now (ffb_release_asked: the thread ends). */
static bool ffb_guard(SDL_Haptic **haptic, bool *running) {
    if (ffb_release_asked) {
        if (*haptic) {
            SDL_StopHapticEffects(*haptic);
            SDL_CloseHaptic(*haptic); *haptic=NULL;   /* closing destroys the effects */
            printf("Runtime: FFB released (%s): the wheel's forces are stopped\n",ffb_release_why);
            fflush(stdout);
        }
        ffb_release_done=true;
        return true;
    }
    if (!*haptic) return false;
    const uint64_t last=__atomic_load_n(&ffb_last_read_us,__ATOMIC_RELAXED);
    if (last && now_us()-last>500000) {
        if (!ffb_held) {
            /* every effect stops; each kind starts its own again once it is no longer marked running */
            ffb_held=true;
            SDL_StopHapticEffects(*haptic);
            for (int i=0;i<LOGI_EFFECTS;++i) running[i]=false;
            sw.running=false;
            pw.running=pw.damper_running=pw.slip_running=false;
            static unsigned n; if (n++<20) printf("Runtime: FFB: the game stopped reading the wheel; its forces are off until it does\n");
        }
        return true;
    }
    if (ffb_held) {
        ffb_held=false;
        /* the effects as the game has them now (it may have changed them meanwhile) start again */
        pthread_mutex_lock(&ffb.m);
        for (int i=0;i<LOGI_EFFECTS;++i) ffb.fx[i].dirty=ffb.fx[i].used;
        pthread_mutex_unlock(&ffb.m);
        static unsigned n; if (n++<20) printf("Runtime: FFB: the game reads the wheel again; forces back on\n");
    }
    return false;
}
/* Before the process ends any way the runtime controls it (window closed, quit, crash...). Waits at most 300 ms:
 * from a crash handler the device may never answer, and the process must still end. */
void runtime_ffb_release(const char *why) {
    static int once;
    if (__atomic_exchange_n(&once,1,__ATOMIC_ACQ_REL)) return;
    if (ffb_trace_file) { pthread_mutex_lock(&ffb_trace_m); fflush(ffb_trace_file); pthread_mutex_unlock(&ffb_trace_m); }
    pthread_mutex_lock(&ffb.m);
    const bool started=ffb.started;
    pthread_mutex_unlock(&ffb.m);
    if (!started) return;
    ffb_release_why=why;
    ffb_release_asked=true;
    for (int i=0;i<75 && !ffb_release_done;++i) compat_sleep_ns(4000000);
}

/* The haptic thread: opens the wheel's force feedback and keeps its effects as the game last asked. */
static void *ffb_thread(void *unused) {
    (void)unused;
    SDL_Haptic *haptic=NULL;
    int sdl_id[LOGI_EFFECTS]; bool running[LOGI_EFFECTS]; Sint16 spring_centre[LOGI_EFFECTS];
    float wave_avg[LOGI_EFFECTS]; bool wave_avg_set[LOGI_EFFECTS]; Uint16 wave_sent[LOGI_EFFECTS]; uint64_t wave_held_us[LOGI_EFFECTS];
    for (int i=0;i<LOGI_EFFECTS;++i) { sdl_id[i]=-1; running[i]=false; spring_centre[i]=0; wave_avg[i]=0.0f; wave_avg_set[i]=false; wave_sent[i]=0; wave_held_us[i]=0; }
    uint64_t pass_at=now_us();
    for (;;) {
        compat_sleep_ns(4000000); /* 4 ms */
        const uint64_t pass_now=now_us(), pass_us=pass_now-pass_at;   /* the time this pass stands for */
        pass_at=pass_now;
        if (ffb_guard(&haptic,running)) { if (ffb_release_asked) return NULL; continue; }
        if (!haptic) {
            pthread_mutex_lock(&lock);
            SDL_Joystick *j=wheel;
            pthread_mutex_unlock(&lock);
            if (!j) { compat_sleep_ns(100000000); continue; }
            /* SDL's haptic subsystem starts once the wheel is open, so SDL itself is up by then. */
            if (!SDL_WasInit(SDL_INIT_HAPTIC) && !SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
                printf("Runtime: FFB off: %s\n",SDL_GetError());
                return NULL;
            }
            char wheel_name[128]="";
            pthread_mutex_lock(&lock);
            if ((j=wheel)) {
                haptic=SDL_OpenHapticFromJoystick(j);
                snprintf(wheel_name,sizeof wheel_name,"%s",SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : "");
            }
            pthread_mutex_unlock(&lock);
            if (!j) continue;
            if (!haptic && *wheel_name) {
                /* Some wheels (Fanatec) are several game controllers of one name, and the forces go to another one
                 * than the axes: the first of that name with a constant force takes them. */
                char why[256]; snprintf(why,sizeof why,"%s",SDL_GetError());
                int n=0;
                SDL_HapticID *hids=SDL_GetHaptics(&n);
                for (int i=0;hids && i<n && !haptic;++i) {
                    const char *hn=SDL_GetHapticNameForID(hids[i]);
                    if (!hn || strcmp(hn,wheel_name) || !(haptic=SDL_OpenHaptic(hids[i]))) continue;
                    if (!(SDL_GetHapticFeatures(haptic)&SDL_HAPTIC_CONSTANT)) { SDL_CloseHaptic(haptic); haptic=NULL; continue; }
                    printf("Runtime: FFB: the wheel's own controller has none (%s), forces go to its other \"%s\"\n",why,hn);
                }
                SDL_free(hids);
                if (!haptic) SDL_SetError("%s",why);
            }
            if (!haptic) {
                static bool said;
                if (!said) { said=true; printf("Runtime: FFB: the wheel offers no force feedback to Windows (%s)\n",SDL_GetError()); }
                compat_sleep_ns(1000000000);
                continue;
            }
            SDL_StopHapticEffects(haptic);   /* forces a previous run left playing (a killed process) end here */
            const Uint32 f=SDL_GetHapticFeatures(haptic);
            printf("Runtime: FFB on %s: constant %s, spring %s, damper %s, square %s, gain %s\n",SDL_GetHapticName(haptic),
                   f&SDL_HAPTIC_CONSTANT ? "yes" : "no",f&SDL_HAPTIC_SPRING ? "yes" : "no",f&SDL_HAPTIC_DAMPER ? "yes" : "no",
                   f&SDL_HAPTIC_SQUARE ? "yes" : "no",f&SDL_HAPTIC_GAIN ? "yes" : "no");
            /* The device gain is left as the player set it in the wheel's software: some drivers keep a gain set
             * here as their own setting (see the safety note above). The gains are applied to the forces themselves,
             * and BB_FFB_DRY=2 makes every force zero there. */
            pthread_mutex_lock(&ffb.m);
            for (int i=0;i<LOGI_EFFECTS;++i) ffb.fx[i].dirty=ffb.fx[i].used;
            ffb.autocenter_dirty=true;
            pthread_mutex_unlock(&ffb.m);
        }
        pthread_mutex_lock(&lock);
        const bool gone=!wheel;
        const int32_t pos=wheel ? (int32_t)wheel_value(wheel,WAX_STEER)-32768 : 0;   /* the game's steering scale */
        pthread_mutex_unlock(&lock);
        if (gone) {
            SDL_CloseHaptic(haptic); haptic=NULL;
            for (int i=0;i<LOGI_EFFECTS;++i) { sdl_id[i]=-1; running[i]=false; }
            sw.id=-1; sw.running=false; sw.init=false;
            pw.id=pw.damper_id=pw.slip_id=-1; pw.running=pw.damper_running=pw.slip_running=false; pw.init=false;
            continue;
        }
        LogiEffectParams want[LOGI_EFFECTS]; bool dirty[LOGI_EFFECTS], used[LOGI_EFFECTS], playing[LOGI_EFFECTS];
        bool autocenter, autocenter_dirty;
        pthread_mutex_lock(&ffb.m);
        for (int i=0;i<LOGI_EFFECTS;++i) {
            want[i]=ffb.fx[i].p; dirty[i]=ffb.fx[i].dirty; used[i]=ffb.fx[i].used; playing[i]=ffb.fx[i].playing;
            ffb.fx[i].dirty=false;
        }
        autocenter=ffb.autocenter; autocenter_dirty=ffb.autocenter_dirty; ffb.autocenter_dirty=false;
        pthread_mutex_unlock(&ffb.m);
        if (autocenter_dirty && (SDL_GetHapticFeatures(haptic)&SDL_HAPTIC_AUTOCENTER)) SDL_SetHapticAutocenter(haptic,autocenter && !ffb_zero ? 100 : 0);
        if (ffb_log()) {   /* ten lines a second while the game plays a force, up to ten minutes of driving */
            static uint64_t last; static unsigned lines;
            const uint64_t now=now_us();
            bool any=false;
            for (int i=0;i<LOGI_EFFECTS;++i) if (used[i] && playing[i] && (want[i].a || (want[i].type==LOGI_SPRING && want[i].e))) any=true;
            if (any && now-last>=100000 && lines<6000) {
                last=now; ++lines;
                pthread_mutex_lock(&lock);
                WheelExtension w; memcpy(&w,wheel_last_read.extension,sizeof w);
                pthread_mutex_unlock(&lock);
                printf("FFB: steer %+6d",w.magic==WHEEL_MAGIC ? (int)w.steer-32768 : 0);
                for (int i=0;i<LOGI_EFFECTS;++i) if (used[i]) {
                    const LogiEffectParams *p=&want[i];
                    if (p->type==LOGI_CONSTANT) printf(" | const%s %+4d",playing[i] ? "" : "(off)",p->a);
                    else if (p->type==LOGI_SPRING) printf(" | spring%s ctr %+4d k %d/%d sat %d/%d",playing[i] ? "" : "(off)",p->a,p->c,p->d,p->e,p->f);
                    else if (p->type==LOGI_DAMPER) printf(" | damper%s k %d/%d sat %d/%d",playing[i] ? "" : "(off)",p->c,p->d,p->e,p->f);
                    else printf(" | wave%s %d ms %d",playing[i] ? "" : "(off)",p->c,p->a);
                }
                if (ffb_software) printf(" | software %+.2f",sw.force);
                if (ffb_physics) {
                    pthread_mutex_lock(&ph.m);
                    printf(" | physics %+.2f road %+.2f slip %.2f force %+.2f | %.0f km/h k %.1f mu %.2f slide %.2f %.2f",
                           ph.torque,ph.road,ph.slip,pw.force,ph.speed*3.6f,ph.k,ph.mu,ph.x[0],ph.x[1]);
                    pthread_mutex_unlock(&ph.m);
                }
                printf("\n");
            }
        }
        /* The physics model owns the wheel only while a car is controlled.
         * Do not start the guest's hardware spring in intros, menus or replays:
         * it is unrelated to the physics output and can oscillate a T300 even
         * before the first driving sample. Other models retain guest effects. */
        const bool physics=ffb_physics && ffb_physics_live();
        { static bool was;
          if (was && !physics) {
              ffb_physics_stop(haptic);
              pthread_mutex_lock(&ffb.m);
              for (int i=0;i<LOGI_EFFECTS;++i) if (ffb.fx[i].used) dirty[i]=ffb.fx[i].dirty=true;
              pthread_mutex_unlock(&ffb.m);
          }
          was=physics; }
        for (int i=0;i<LOGI_EFFECTS;++i) {
            /* with the software model the spring and the constant force are worked into its own force */
            if (!used[i] || ffb_zero || (ffb_software && (want[i].type==LOGI_SPRING || want[i].type==LOGI_CONSTANT)) ||
                (ffb_physics && (!physics || want[i].type!=LOGI_PERIODIC))) {
                if (sdl_id[i]>=0) { SDL_DestroyHapticEffect(haptic,sdl_id[i]); sdl_id[i]=-1; running[i]=false; }
                continue;
            }
            /* a spring follows the wheel: its centre is redone once the wheel is 0.2% of the steering
             * range off it, not on every 4 ms poll */
            SDL_HapticEffect e;
            const bool have=ffb_effect_from(&want[i],&e,pos);
            const bool spring_moved=have && want[i].type==LOGI_SPRING && sdl_id[i]>=0 && (ffb_spring>=0 ? ffb_spring : ffb_gain)!=100 &&
                                    abs(e.condition.center[0]-spring_centre[i])>=64;
            /* The vibrations are redone every pass: wheel_ffb_texture_mode = changes, the long rumble fade and the
             * vibration limit all change them between the game's updates. */
            bool wave_moved=false;
            if (have && want[i].type==LOGI_PERIODIC) {
                const bool kerb=ffb_wave_is_kerb(&want[i]);
                const float raw=(float)abs(want[i].a);
                if (ffb_texture_changes && !kerb && !ffb_physics) {
                    /* the surface wave above its own slow average, and wheel_ffb_texture_floor of the steady part */
                    if (!wave_avg_set[i]) { wave_avg[i]=raw; wave_avg_set[i]=true; }
                    wave_avg[i]+=(raw-wave_avg[i])*0.003f;   /* about a second and a half at 4 ms a pass */
                    const float above=raw-wave_avg[i], steady=fminf(raw,wave_avg[i]);
                    e.periodic.magnitude=ffb_wave_level((above>0.0f ? above : 0.0f)+steady*(float)ffb_texture_floor/100.0f,&want[i]);
                }
                /* A kerb is over in a fraction of a second; the same wave held on is wheelspin or a burnout, which on
                 * a direct drive shakes the wheel off the table: after 0.6 s it fades to wheel_ffb_long_rumble. */
                if (kerb && playing[i] && raw>=24.0f) wave_held_us[i]+=pass_us; else wave_held_us[i]=0;
                /* the physics model has its own wheelspin (slip effect) and keeps a quarter */
                const int long_rumble=ffb_physics ? 25 : ffb_long_rumble;
                if (kerb && long_rumble<100 && wave_held_us[i]>600000) {
                    const float t=fminf(1.0f,(float)(wave_held_us[i]-600000)/500000.0f), keep=(float)long_rumble/100.0f;
                    e.periodic.magnitude=(Uint16)((float)e.periodic.magnitude*(1.0f-t*(1.0f-keep)));
                }
                wave_moved=sdl_id[i]>=0 && abs((int)e.periodic.magnitude-(int)wave_sent[i])>=256;
            }
            if (dirty[i] || spring_moved || wave_moved) {
                if (have) {
                    if (want[i].type==LOGI_SPRING) spring_centre[i]=e.condition.center[0];
                    if (want[i].type==LOGI_PERIODIC) wave_sent[i]=e.periodic.magnitude;
                    if (sdl_id[i]<0) {
                        sdl_id[i]=SDL_CreateHapticEffect(haptic,&e);
                        if (sdl_id[i]<0) { static unsigned n; if (n++<8) printf("Runtime: FFB effect type %u not created: %s\n",want[i].type,SDL_GetError()); }
                    } else if (!SDL_UpdateHapticEffect(haptic,sdl_id[i],&e)) {
                        static unsigned n; if (n++<8) printf("Runtime: FFB effect %d not updated: %s\n",i,SDL_GetError());
                    }
                }
            }
            if (sdl_id[i]<0) continue;
            if (playing[i] && !running[i]) running[i]=SDL_RunHapticEffect(haptic,sdl_id[i],1);
            else if (!playing[i] && running[i]) { SDL_StopHapticEffect(haptic,sdl_id[i]); running[i]=false; }
        }
        if (ffb_software) ffb_software_pass(haptic,want,used,playing,pos);
        else if (sw.id>=0) { SDL_DestroyHapticEffect(haptic,sw.id); sw.id=-1; sw.running=false; sw.init=false; }
        if (physics) ffb_physics_pass(haptic,pos);
        { uint64_t car=0;
          if (physics) { pthread_mutex_lock(&ph.m); car=ph.key; pthread_mutex_unlock(&ph.m); }
          ffb_gain_buttons(car); }
        ffb_settings_reload();
    }
    return NULL;
}
static void ffb_start(void) {
    pthread_mutex_lock(&ffb.m);
    const bool start=ffb_enabled && !ffb.started;
    ffb.started=true;
    pthread_mutex_unlock(&ffb.m);
    if (!start) return;
    /* BB_FFB_DRY=1: the game computes its forces, but none reach the wheel; 2: the physics model runs and sends its
     * forces at zero strength, the game's are left out (unattended test runs) */
    { const char *e=getenv("BB_FFB_DRY");
      if (e && *e=='2') { ffb_zero=true; printf("Runtime: FFB dry run: every force at zero strength\n"); }
      else if (e && *e && *e!='0') { printf("Runtime: FFB dry run: no forces to the wheel\n"); return; } }
    /* BB_FFB_MODEL=game|software|physics: the model for this start, whatever default.ini says (test runs) */
    { const char *e=getenv("BB_FFB_MODEL");
      if (e && *e) { ffb_software=!strcmp(e,"software"); ffb_physics=!strcmp(e,"physics"); printf("Runtime: FFB model %s (BB_FFB_MODEL)\n",e); } }
    /* BB_FFB_TYRE=grip|classic: wheel_ffb_physics_tyre for this start (test runs) */
    { const char *e=getenv("BB_FFB_TYRE");
      if (e && *e) { ffb_ph_tyre_grip=!strncmp(e,"grip",4); printf("Runtime: FFB tyre %s (BB_FFB_TYRE)\n",ffb_ph_tyre_grip ? "grip" : "classic"); } }
    ffb_trace_open();
    pthread_t t;
    if (pthread_create(&t,NULL,ffb_thread,NULL)==0) pthread_detach(t);
}

/* ---- logiWheel.prx exports ---------------------------------------------------------------------- */
static bool logi_connected(void) {   /* wheel_ffb = off: no Logitech wheel, as before force feedback */
    pthread_mutex_lock(&lock);
    input_load();
    const bool c=ffb_enabled && current_wheel()!=NULL;
    pthread_mutex_unlock(&lock);
    return c;
}
static ABI int32_t logi_init(uint32_t version) {
    pthread_mutex_lock(&lock); input_load(); pthread_mutex_unlock(&lock);
    printf("Runtime: logiWheelInit(0x%x): Logitech wheel library replaced, force feedback %s\n",version,ffb_enabled ? "to the wheel" : "off");
    ffb_hook_install();
    ffb_start();
    return LOGI_OK;
}
static ABI int32_t logi_term(void) { return LOGI_OK; }
/* The state the game reads (guest +0xe13260..+0xe13350): steering at +8, accelerator, brake and clutch at +0xa..+0xe
 * (0..65535), shifter gear at +0x10, the pad buttons from +0x11. Filled from the wheel port's last read. */
static ABI int32_t logi_read_state(uint8_t *state) {
    if (!state) return LOGI_ERR_ARG;
    __atomic_store_n(&ffb_last_read_us,now_us(),__ATOMIC_RELAXED);
    if (!logi_connected()) return LOGI_ERR_NOT_CONNECTED;
    /* The wheel now, with the buttons of the pad last read on the wheel port (gamepad, keyboard and wheel). */
    PadData d; memset(&d,0,sizeof d);
    pthread_mutex_lock(&lock);
    wheel_sample(&d,bbgpu_overlay_captures_input());
    d.buttons|=wheel_last_read.buttons;
    pthread_mutex_unlock(&lock);
    WheelExtension w; memcpy(&w,d.extension,sizeof w);
    memset(state,0,0x25);
    if (w.magic!=WHEEL_MAGIC) { w.steer=32768; w.throttle=w.brake=w.clutch=0; w.gear=0; }
    memcpy(state+8,&w.steer,2); memcpy(state+0xa,&w.throttle,2); memcpy(state+0xc,&w.brake,2); memcpy(state+0xe,&w.clutch,2);
    state[0x10]=w.gear;
    memcpy(state+0x11,&d.buttons,4);
    return LOGI_OK;
}
static ABI int32_t logi_play_effect(int32_t *id, const LogiEffectParams *p) {
    if (!id || !p) return LOGI_ERR_ARG;
    if (!logi_connected()) return LOGI_ERR_NOT_CONNECTED;
    pthread_mutex_lock(&ffb.m);
    int slot=-1;
    for (int i=0;i<LOGI_EFFECTS && slot<0;++i) if (!ffb.fx[i].used) slot=i;
    if (slot>=0) { ffb.fx[slot].used=true; ffb.fx[slot].playing=true; ffb.fx[slot].dirty=true; ffb.fx[slot].p=*p; }
    ffb.autocenter=false; ffb.autocenter_dirty=true;   /* logiWheel turns the centring spring off for a force */
    pthread_mutex_unlock(&ffb.m);
    if (slot<0) return LOGI_ERR_ARG;
    *id=slot;
    ffb_log_params("play",slot,p);
    return LOGI_OK;
}
static ABI int32_t logi_update_effect(int32_t id, const LogiEffectParams *p) {
    if (!p || id<0 || id>=LOGI_EFFECTS) return LOGI_ERR_ARG;
    pthread_mutex_lock(&ffb.m);
    const bool ok=ffb.fx[id].used;
    if (ok) {
        if (memcmp(&ffb.fx[id].p,p,sizeof(*p))) { ffb.fx[id].p=*p; ffb.fx[id].dirty=true; }
        ffb.fx[id].playing=true;
    }
    pthread_mutex_unlock(&ffb.m);
    if (ok) {
        /* the first changes of each effect, and then every 300th, for the force scale */
        static unsigned count[LOGI_EFFECTS];
        if (count[id]++%300<3) ffb_log_params("update",id,p);
    }
    return ok ? LOGI_OK : LOGI_ERR_ARG;
}
static ABI int32_t logi_stop_effect(int32_t id) {
    if (id<0 || id>=LOGI_EFFECTS) return LOGI_ERR_ARG;
    pthread_mutex_lock(&ffb.m);
    ffb.fx[id].playing=false;
    pthread_mutex_unlock(&ffb.m);
    return LOGI_OK;
}
static ABI int32_t logi_set_autocenter(uint8_t on) {
    pthread_mutex_lock(&ffb.m);
    ffb.autocenter=on!=0; ffb.autocenter_dirty=true;
    pthread_mutex_unlock(&ffb.m);
    return logi_connected() ? LOGI_OK : LOGI_ERR_NOT_CONNECTED;
}
static ABI int32_t logi_set_range(int32_t degrees, uint8_t flag) {
    static unsigned n; if (n++<20) printf("Runtime: logiWheel operating range %d degrees (%u); the wheel keeps its own setting%s\n",degrees,flag,
                                          (ffb_software || ffb_physics) && ffb_soft_lock<0 ? ", the soft lock follows it" : "");
    if (degrees>=90 && degrees<=3600) logi_range_deg=degrees;
    return logi_connected() ? LOGI_OK : LOGI_ERR_NOT_CONNECTED;
}
static ABI int32_t logi_byte_call(uint8_t v) {   /* two calls taking one byte: the rev LEDs and an unknown one */
    (void)v;
    return logi_connected() ? LOGI_OK : LOGI_ERR_NOT_CONNECTED;
}
static ABI int32_t logi_unknown_byte(uint8_t v) {
    static unsigned n; if (n++<4) printf("Runtime: logiWheel call with %u\n",v);
    return logi_connected() ? LOGI_OK : LOGI_ERR_NOT_CONNECTED;
}
static const struct { const char *nid; void *function; } logi_exports[]={
    {"rqu39g1mv38",(void *)logi_init},          /* logiWheelInit */
    {"MjWOyyyHMbw",(void *)logi_term},
    {"pkk2E4Izy-Q",(void *)logi_read_state},    /* logiWheelReadState */
    {"OO6XBeuXhsg",(void *)logi_play_effect},
    {"mPcvqcI2O50",(void *)logi_update_effect},
    {"0gsOoL2NyqQ",(void *)logi_stop_effect},
    {"-8HzgEWqpx4",(void *)logi_set_autocenter},
    {"rFRMV3lLSJs",(void *)logi_set_range},
    {"xqPuCy0mIrc",(void *)logi_byte_call},     /* rev LEDs: 0x1f >> n (guest +0xe13870) */
    {"MubUr+Qm4M4",(void *)logi_unknown_byte},
};
