#include "../gpu/shim/bbport_diagnostics.h"
/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Keyboard layout (when no gamepad is connected):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad, Backspace right touchpad,
 *   IJKL d-pad (I up, K down, J left, L right). */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_exit.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define ERR_TOO_MANY_OPENED ((int32_t)0x80920007)

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, sdl_ready;
/* Every open gets its own handle (1..MAX_HANDLES): the game holds a standard pad (port type 0) and a special one
 * (type 2, where the console puts wheels) at once, and logiWheel.prx keeps a scePadOpenExt one of its own. All of
 * them read the one physical pad; the type 2 port is a steering wheel while one is connected (runtime_wheel.inc). */
#define MAX_HANDLES 16
static struct { bool used, ext; int32_t user, type, index; } handles[MAX_HANDLES];
static bool handle_ok(int32_t h) { return h>=1 && h<=MAX_HANDLES && handles[h-1].used; }
static bool is_wheel_port(int32_t h) { return handle_ok(h) && handles[h-1].type==2 && !handles[h-1].ext; }
#define PADLOG(name) do { static unsigned n_; if (n_++<4) printf("Runtime: %s from +0x%llx\n",name,(unsigned long long)((uintptr_t)__builtin_return_address(0)-runtime_image_base)); } while (0)
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

#ifdef _WIN32
#include <direct.h>
#define compat_mkdir_p(d) _mkdir(d)
#else
#define compat_mkdir_p(d) mkdir(d,0755)
#endif
#include "runtime_input.inc"
#include "runtime_wheel.inc"
#include "runtime_logiwheel.inc"

/* The overlay's pedal display (F2): the throttle, brake and clutch the game read last, 0..1. Returns 3 for a wheel
 * with a clutch axis, 2 for a wheel without one, 1 for a gamepad's triggers (R2 throttle, L2 brake), 0 before the
 * game has read a pad. */
static PadData pad_last_read;   /* the last read of any port, under lock */
static bool pad_was_read;
int runtime_pad_pedals(float *throttle, float *brake, float *clutch) {
    pthread_mutex_lock(&lock);
    WheelExtension w; memcpy(&w,wheel_last_read.extension,sizeof w);
    const PadData p=pad_last_read;
    const bool read=pad_was_read, has_clutch=wheel_axis[WAX_CLUTCH].axis>=0;
    pthread_mutex_unlock(&lock);
    if (w.magic==WHEEL_MAGIC) {
        *throttle=(float)w.throttle/65535.0f; *brake=(float)w.brake/65535.0f; *clutch=(float)w.clutch/65535.0f;
        return has_clutch ? 3 : 2;
    }
    if (!read) return 0;
    *throttle=(float)p.r2/255.0f; *brake=(float)p.l2/255.0f; *clutch=0.0f;
    return 1;
}

/* ---- motion -------------------------------------------------------------------------
 * Like shadPS4: the pad's sensors pass straight through. Acceleration is reported the way the
 * console does it (SDL's m/s^2 * 0.098, so rest reads ~0.96 on the vertical axis); angular
 * velocity is rad/s; orientation is the gyro integrated from identity.
 * Pads without sensors and the keyboard get a synthesised signal (the idea of the
 * "motion_shake" binding of shadPS4-AYOUB1080p): G shakes the controller about 7 times a
 * second, H toggles holding it sideways (Shift+H: the other side). */
#define GRAVITY 9.81f
static bool pad_has_gyro, pad_has_accel;
static struct { float q[4]; uint64_t last_us; int side, h_prev; } motion={{0,0,0,1},0,0,0};
/* ---- orientation estimate -------------------------------------------------------------
 * The console reports an orientation whose tilt is corrected by gravity (the accelerometer),
 * so shaking or fast motion does not make it wander. Adding up gyro readings alone (what
 * shadPS4 does) drifts and, after a shake, can point anywhere: inFAMOUS Second Son's spray-can
 * scene then steers the character sideways.
 * This is a Mahony complementary filter run on its own thread at about 500 Hz:
 *   - the gyro is integrated every step (body-frame rates, quaternion x y z w);
 *   - the measured "up" (accelerometer at rest) pulls the estimated "up" toward it, but only
 *     while the acceleration is close to 1 g, so a shake does not corrupt the tilt;
 *   - the gyro bias is learned while the pad is still.
 * Reference frame: identity = lying flat, face up (SDL reports +Y up). Yaw has no reference
 * and, as on the console, is relative to where the estimate started. */
static struct {
    pthread_mutex_t m;
    float q[4], accel[3], gyro[3], bias[3], still_s;
    uint64_t last_ns;
    int running;
} fusion={PTHREAD_MUTEX_INITIALIZER,{0,0,0,1},{0,GRAVITY,0},{0,0,0},{0,0,0},0,0,0};
static void quat_down_in_body(const float q[4], float out[3]) {
    /* the world up axis (0,1,0) expressed in the body frame: rotate by the conjugate of q */
    const float vx=-q[0], vy=-q[1], vz=-q[2], w=q[3];
    const float ux=0, uy=1, uz=0;
    const float cx=vy*uz-vz*uy, cy=vz*ux-vx*uz, cz=vx*uy-vy*ux;      /* qv x u */
    const float dx=vy*cz-vz*cy, dy=vz*cx-vx*cz, dz=vx*cy-vy*cx;      /* qv x (qv x u) */
    out[0]=ux+2*w*cx+2*dx; out[1]=uy+2*w*cy+2*dy; out[2]=uz+2*w*cz+2*dz;
}
static void fusion_step(const float accel[3], const float gyro_raw[3], float dt) {
    float *q=fusion.q;
    float w[3]={gyro_raw[0]-fusion.bias[0], gyro_raw[1]-fusion.bias[1], gyro_raw[2]-fusion.bias[2]};
    const float an=sqrtf(accel[0]*accel[0]+accel[1]*accel[1]+accel[2]*accel[2]);
    const float spin=sqrtf(w[0]*w[0]+w[1]*w[1]+w[2]*w[2]);
    const bool at_rest_g=an>0.5f*GRAVITY && fabsf(an-GRAVITY)<0.15f*GRAVITY;
    if (at_rest_g && spin<0.05f) fusion.still_s+=dt; else fusion.still_s=0.0f;
    if (fusion.still_s>0.5f) for (int i=0;i<3;++i) fusion.bias[i]+=0.01f*(gyro_raw[i]-fusion.bias[i]);
    if (at_rest_g) {
        const float a[3]={accel[0]/an,accel[1]/an,accel[2]/an};
        float v[3];
        quat_down_in_body(q,v);
        const float kp=2.0f;
        w[0]+=kp*(a[1]*v[2]-a[2]*v[1]);
        w[1]+=kp*(a[2]*v[0]-a[0]*v[2]);
        w[2]+=kp*(a[0]*v[1]-a[1]*v[0]);
    }
    const float qx=q[3]*w[0] + q[1]*w[2] - q[2]*w[1];
    const float qy=q[3]*w[1] + q[2]*w[0] - q[0]*w[2];
    const float qz=q[3]*w[2] + q[0]*w[1] - q[1]*w[0];
    const float qw=-q[0]*w[0] - q[1]*w[1] - q[2]*w[2];
    q[0]+=0.5f*qx*dt; q[1]+=0.5f*qy*dt; q[2]+=0.5f*qz*dt; q[3]+=0.5f*qw*dt;
    const float n=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if (n>0.0f) for (int i=0;i<4;++i) q[i]/=n;
    memcpy(fusion.accel,accel,sizeof(fusion.accel));
    for (int i=0;i<3;++i) fusion.gyro[i]=gyro_raw[i]-fusion.bias[i];
}
static void *fusion_thread(void *unused) {
    (void)unused;
    for (;;) {
        compat_sleep_ns(2000000); /* 2 ms */
        float a[3], w[3];
        pthread_mutex_lock(&lock); /* the pad is closed and reopened under this lock */
        SDL_Gamepad *g=gamepad;
        const bool ok=g && SDL_GetGamepadSensorData(g,SDL_SENSOR_ACCEL,a,3) && SDL_GetGamepadSensorData(g,SDL_SENSOR_GYRO,w,3);
        pthread_mutex_unlock(&lock);
        const uint64_t now=now_us()*1000u;
        pthread_mutex_lock(&fusion.m);
        const float dt=fusion.last_ns ? (float)(now-fusion.last_ns)/1e9f : 0.0f;
        fusion.last_ns=now;
        if (ok && dt>0.0f && dt<0.1f) fusion_step(a,w,dt);
        pthread_mutex_unlock(&fusion.m);
    }
    return NULL;
}
static void fusion_start(void) {
    if (fusion.running) return;
    fusion.running=1;
    pthread_t t;
    if (pthread_create(&t,NULL,fusion_thread,NULL)==0) pthread_detach(t);
}
static void fusion_reset(void) {
    pthread_mutex_lock(&fusion.m);
    fusion.q[0]=fusion.q[1]=fusion.q[2]=0.0f; fusion.q[3]=1.0f;
    pthread_mutex_unlock(&fusion.m);
}

static void motion_sample(PadData *d, SDL_Gamepad *g, const bool *k, bool neutral) {
    float accel[3]={0,GRAVITY,0}, gyro[3]={0,0,0};
    const uint64_t now=now_us();
    const float dt=motion.last_us ? (float)(now-motion.last_us)/1e6f : 0.0f;
    motion.last_us=now;
    const bool sensors=g && pad_has_gyro && pad_has_accel;
    float fused_q[4]={0,0,0,1};
    if (sensors) {
        pthread_mutex_lock(&fusion.m);
        memcpy(accel,fusion.accel,sizeof(accel));
        memcpy(gyro,fusion.gyro,sizeof(gyro));
        memcpy(fused_q,fusion.q,sizeof(fused_q));
        pthread_mutex_unlock(&fusion.m);
    }
    const bool keys=!neutral && k;
    const bool shaking=keys && k[SDL_SCANCODE_G];
    if (keys) {
        const bool h=k[SDL_SCANCODE_H];
        if (h && !motion.h_prev) {
            const int other=(k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) ? -1 : 1;
            motion.side = motion.side==other ? 0 : other;
        }
        motion.h_prev=h;
        if (!sensors) {
            /* Synthesised pose: upright, or a quarter turn about the viewing axis. */
            const float c=0.70710678f;
            if (motion.side) { accel[0]=motion.side*GRAVITY; accel[1]=0; }
            motion.q[0]=0; motion.q[1]=0;
            motion.q[2]=motion.side*c; motion.q[3]=motion.side ? c : 1.0f;
        }
        if (shaking) {
            const float sign=((now/70000u)&1u) ? -1.0f : 1.0f; /* about 7 shakes a second */
            if (motion.side && !sensors) accel[0]+=sign*2.5f*GRAVITY*(float)motion.side;
            else accel[1]+=sign*2.5f*GRAVITY;
            gyro[0]+=sign*6.0f;
        }
    }
    (void)dt;
    for (int i=0;i<3;++i) { d->acceleration[i]=accel[i]*0.098f; d->angular_velocity[i]=gyro[i]; }
    memcpy(d->orientation,sensors ? fused_q : motion.q,sizeof(d->orientation));
}

static void apply_light(void);
/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) {
        /* Controllers must keep working while the game window is not the foreground window. */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    }
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    current_wheel(); /* a wheel that SDL also knows as a gamepad stays the wheel */
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        int first=0;
        while (ids && first<count && wheel && ids[first]==wheel_id) ++first;
        if (ids && first<count) {
            gamepad=SDL_OpenGamepad(ids[first]);
            if (gamepad) {
                ++connected_count;
                printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad));
                pad_has_gyro=SDL_GamepadHasSensor(gamepad,SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(gamepad,SDL_SENSOR_GYRO,true);
                pad_has_accel=SDL_GamepadHasSensor(gamepad,SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(gamepad,SDL_SENSOR_ACCEL,true);
                if (pad_has_gyro && pad_has_accel) fusion_start();
                apply_light();
                printf("Runtime: motion sensors: gyro %s, accelerometer %s%s\n",pad_has_gyro ? "on" : "none",
                       pad_has_accel ? "on" : "none",
                       pad_has_gyro && pad_has_accel ? "" : " (keyboard G = shake, H = hold sideways)");
            }
        }
        SDL_free(ids);
    }
    return gamepad;
}
static void sample_buttons(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    input_load();
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_UpdateGamepads(); /* the window's event loop may not pump this thread's view of the pad */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        static const struct { SDL_GamepadButton sdl; uint32_t ps; } map[]={
            {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
            {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS},
            {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
        };
        for (size_t i=0;i<sizeof(map)/sizeof(*map);++i) {
            int sdl=map[i].sdl;
            for (int r=0;r<pad_remap_count;++r) if ((uint32_t)pad_remap[r][0]==map[i].ps) sdl=pad_remap[r][1];
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)sdl)) d->buttons|=map[i].ps;
        }
        if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_BACK) || SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_TOUCHPAD)) d->buttons|=BTN_TOUCHPAD;
        d->left_x=dz_axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX),0); d->left_y=dz_axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY),0);
        d->right_x=dz_axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX),1); d->right_y=dz_axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY),1);
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        {
            static uint32_t last_buttons=~0u; static uint8_t last_axes[4]; static unsigned logged;
            if (logged<200 && (d->buttons!=last_buttons || abs(d->left_x-last_axes[0])>40 || abs(d->left_y-last_axes[1])>40)) {
                ++logged; last_buttons=d->buttons; last_axes[0]=d->left_x; last_axes[1]=d->left_y;
                printf("Runtime: gamepad input: buttons 0x%x left %u,%u triggers %u,%u\n",d->buttons,d->left_x,d->left_y,d->l2,d->r2);
            }
        }
    }
    /* Keyboard and mouse work together with a pad, like shadPS4. */
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
    float mx=0, my=0;
    const SDL_MouseButtonFlags mouse=SDL_GetMouseState(&mx,&my);
    int axis_sum[4]={0,0,0,0}; bool axis_used[4]={false,false,false,false};
    bool kb_l2=false, kb_r2=false;
    const bool modal_pointer=runtime_exit_captures()!=0;
    for (int i=0;i<binding_count;++i) {
        const Binding *b=&bindings[i];
        if (!binding_down(b,k,mouse)) continue;
        if (modal_pointer) {
            bool pointer_binding=false;
            for(unsigned j=0;j<b->in_count;++j) if(b->in[j].kind==IN_MOUSE) pointer_binding=true;
            if(pointer_binding) continue; /* Modal pointer clicks have their own hit targets. */
        }
        if (b->out_kind==OUT_BUTTON) {
            d->buttons|=b->button;
            if (b->button==BTN_L2) kb_l2=true;
            if (b->button==BTN_R2) kb_r2=true;
        } else if (b->out_kind==OUT_AXIS) {
            axis_sum[b->axis]+=b->sign*127; axis_used[b->axis]=true;
        } else {
            d->buttons|=BTN_TOUCHPAD;
            d->touch_count=1;
            d->touches[0]=(PadTouch){.x=(uint16_t)(b->touch_side==0 ? 480 : b->touch_side==2 ? 1440 : 960),.y=471,.id=0};
        }
    }
    /* Keys press a pedal or steer all the way at once; a ramp makes them usable in a car: the pedal and the
     * wheel move toward the key at a set speed, and a reversal of the steering is faster than a return. */
    {
        static struct timespec last; static bool have_last; static float pedal[2], steer;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
        float dt=have_last ? (float)(now.tv_sec-last.tv_sec)+(float)(now.tv_nsec-last.tv_nsec)/1e9f : 0.0f;
        last=now; have_last=true;
        if (dt<0 || dt>0.1f) dt=0.1f;
        const bool want[2]={kb_l2,kb_r2};
        for (int p=0;p<2;++p) {
            const float rise=kb_pedal_time[0]>0 ? dt/kb_pedal_time[0] : 1.0f, fall=kb_pedal_time[1]>0 ? dt/kb_pedal_time[1] : 1.0f;
            pedal[p]+=want[p] ? rise : -fall;
            pedal[p]=pedal[p]<0 ? 0 : pedal[p]>1 ? 1 : pedal[p];
        }
        uint8_t *trig[2]={&d->l2,&d->r2};
        for (int p=0;p<2;++p) { const uint8_t v=(uint8_t)(pedal[p]*255.0f+0.5f); if (v>*trig[p]) *trig[p]=v; }
        const float target=axis_used[0] ? (axis_sum[0]<0 ? -1.0f : axis_sum[0]>0 ? 1.0f : 0.0f) : 0.0f;
        const float rise=kb_steer_time[0]>0 ? dt/kb_steer_time[0] : 2.0f, fall=kb_steer_time[1]>0 ? dt/kb_steer_time[1] : 2.0f;
        float step;
        if (target==0.0f) step=fall;
        else if (steer*target<0) step=rise*2.0f;   /* turning the other way: quickly through the middle */
        else step=rise;
        if (steer<target) { steer+=step; if (steer>target) steer=target; }
        else if (steer>target) { steer-=step; if (steer<target) steer=target; }
        axis_sum[0]=(int)(steer*127.0f);
        axis_used[0]=axis_used[0] || steer!=0.0f;
    }
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (axis_used[i]) {
        const int v=128+(axis_sum[i]<-128 ? -128 : axis_sum[i]>127 ? 127 : axis_sum[i]);
        if (abs(v-128)>abs((int)*axes[i]-128)) *axes[i]=(uint8_t)v;
    }
    /* Mouse as a stick: while the window has focus the pointer is captured and its movement turns the stick. */
    /* F7 turns the mouse stick on and off (the pointer is released while it is off). */
    static bool mouse_on=true; static int f7_prev;
    const int f7=k && k[SDL_SCANCODE_F7];
    if (f7 && !f7_prev) mouse_on=!mouse_on;
    f7_prev=f7;
    bbgpu_mouse_stick_capture(mouse_target && mouse_on);
    if (mouse_target) {
        SDL_Window *w=mouse_on && !bbgpu_overlay_captures_input() ? SDL_GetKeyboardFocus() : NULL;
        float dx=0, dy=0;
        if (w) SDL_GetRelativeMouseState(&dx,&dy);
        if (w && (dx!=0 || dy!=0)) {
            float speed=sqrtf(dx*dx+dy*dy)*mouse_speed+mouse_speed_offset*128.0f;
            if (speed<mouse_dz_offset*128.0f) speed=mouse_dz_offset*128.0f;
            if (speed>128.0f) speed=128.0f;
            const float angle=atan2f(dy,dx);
            const float ax=cosf(angle)*speed, ay=sinf(angle)*speed;
            uint8_t *sx=mouse_target==1 ? &d->left_x : &d->right_x, *sy=mouse_target==1 ? &d->left_y : &d->right_y;
            *sx=(uint8_t)(128+(ax<-128 ? -128 : ax>127 ? 127 : (int)ax));
            *sy=(uint8_t)(128+(ay<-128 ? -128 : ay>127 ? 127 : (int)ay));
        }
    }
}
static void sample_host(PadData *d) {
    sample_buttons(d);
    SDL_Gamepad *g=gamepad;
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    motion_sample(d,g,k,bbgpu_overlay_captures_input());
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=BB_DIAGNOSTIC_ENV("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=BB_DIAGNOSTIC_ENV("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static bool exit_suppressed;
static void neutralize_pad(PadData *d) {
    d->buttons=0; d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->l2=d->r2=d->touch_count=0;
    memset(d->touches,0,sizeof(d->touches));
    memset(d->acceleration,0,sizeof(d->acceleration));
    memset(d->angular_velocity,0,sizeof(d->angular_velocity));
    memset(d->orientation,0,sizeof(d->orientation)); d->orientation[3]=1;
}
static const char *exit_button_label(int button, bool ps) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ps ? "Cross" : "A";
    case SDL_GAMEPAD_BUTTON_EAST: return ps ? "Circle" : "B";
    case SDL_GAMEPAD_BUTTON_WEST: return ps ? "Square" : "X";
    case SDL_GAMEPAD_BUTTON_NORTH: return ps ? "Triangle" : "Y";
    default: { const char *name=SDL_GetGamepadStringForButton((SDL_GamepadButton)button); return name ? name : "Button"; }
    }
}
static bool exit_input(PadData *d) {
    uint32_t physical=0; int axis_direction=0,device=2;
    bool ps=false;
    if (gamepad) {
        SDL_GamepadType type=SDL_GetGamepadType(gamepad);
        ps=type==SDL_GAMEPAD_TYPE_PS3 || type==SDL_GAMEPAD_TYPE_PS4 || type==SDL_GAMEPAD_TYPE_PS5;
        if (ps) device=1;
        for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT && i<32;++i)
            if(SDL_GetGamepadButton(gamepad,(SDL_GamepadButton)i)) physical|=1u<<i;
        int x=SDL_GetGamepadAxis(gamepad,SDL_GAMEPAD_AXIS_LEFTX), y=SDL_GetGamepadAxis(gamepad,SDL_GAMEPAD_AXIS_LEFTY);
        if(abs(x)>22000 || abs(y)>22000) axis_direction=abs(x)>=abs(y) ? (x>0 ? 1:2) : (y>0 ? 3:4);
    }
    static uint32_t previous_physical; static int previous_axis;
    bool activity=(physical & ~previous_physical) || (axis_direction && axis_direction!=previous_axis);
    previous_physical=physical; previous_axis=axis_direction;
    int confirm=SDL_GAMEPAD_BUTTON_SOUTH,back=SDL_GAMEPAD_BUTTON_EAST;
    for(int i=0;i<pad_remap_count;++i) {
        if((uint32_t)pad_remap[i][0]==BTN_CROSS) confirm=pad_remap[i][1];
        if((uint32_t)pad_remap[i][0]==BTN_CIRCLE) back=pad_remap[i][1];
    }
    bool neutral=!d->buttons && !d->touch_count && !SDL_GetMouseState(NULL,NULL) && d->l2<25 && d->r2<25 &&
        abs(d->left_x-128)<32 && abs(d->left_y-128)<32 &&
        abs(d->right_x-128)<32 && abs(d->right_y-128)<32;
    return runtime_exit_pad(d->buttons,d->left_x,d->left_y,neutral,
        bbgpu_exit_dialog_available(),activity,device,exit_button_label(confirm,ps),exit_button_label(back,ps));
}
static void sample(PadData *d) {
    sample_host(d);
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else {
        /* Launcher switch: both sticks pressed together tap the right half of the touchpad, so a pad
         * without one still reaches the game's touchpad-right action (photo mode). Edge triggered: one
         * tap per press, not one per frame. */
        static bool combo_down;
        const bool combo=l3r3_touchpad_right && (d->buttons & BTN_L3) && (d->buttons & BTN_R3);
        if (combo && !combo_down && !d->touch_count) touch_click(d,1);
        combo_down=combo;
        if (!combo && (d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    }
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    /* Modal actions use the same mapped logical buttons as the game. Sampling
     * must continue while captured to observe releases and neutral sticks. */
    exit_suppressed=exit_input(d);
    if (exit_suppressed || bbgpu_overlay_captures_input()) {
        hold_after_capture=1; neutralize_pad(d); return;
    }
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
/* Opens are keyed like shadPS4's (user, type, index): the same key twice while open is "already opened", any other
 * logged-in user, port type or index gets the one physical pad. scePadOpenExt opens are keyed apart: logiWheel.prx
 * holds one on port type 2 for good, and the game's own scePadOpen of that port must still succeed. */
static int32_t open_port(int32_t user, int32_t type, int32_t index, bool ext) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0 || type==1) { printf("Runtime: scePadOpen(user %d, type %d, index %d) -> invalid\n",user,type,index); return ERR_INVALID_ARG; }
    pthread_mutex_lock(&lock);
    int free_slot=-1;
    for (int i=0;i<MAX_HANDLES;++i) {
        if (!handles[i].used) { if (free_slot<0) free_slot=i; continue; }
        if (handles[i].user==user && handles[i].type==type && handles[i].index==index && handles[i].ext==ext) {
            pthread_mutex_unlock(&lock);
            { static int shown; if (shown++ < 3) printf("Runtime: scePadOpen(user %d, type %d, index %d) -> already opened (later ones are not logged)\n",user,type,index); }
            return ERR_ALREADY_OPENED;
        }
    }
    if (free_slot<0) { pthread_mutex_unlock(&lock); return ERR_TOO_MANY_OPENED; }
    handles[free_slot].used=true; handles[free_slot].ext=ext;
    handles[free_slot].user=user; handles[free_slot].type=type; handles[free_slot].index=index;
    pthread_mutex_unlock(&lock);
    static unsigned ext_shown;   /* logiWheel.prx opens and closes its port every few seconds */
    if (!ext || ext_shown++<3)
        printf("Runtime: pad %d opened for user %d (type %d, index %d%s; SDL gamepad or keyboard%s)\n",free_slot+1,user,type,index,
               ext ? ", ext; later ones are not logged" : "",type==2 && !ext ? ", or a steering wheel" : "");
    return free_slot+1;
}
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    return open_port(user,type,index,false);
}
static ABI int32_t pad_close(int32_t handle) {
    pthread_mutex_lock(&lock);
    const bool ok=handle_ok(handle);
    if (ok) handles[handle-1].used=false;
    pthread_mutex_unlock(&lock);
    return ok ? 0 : ERR_INVALID_HANDLE;
}

#ifdef _WIN32
/* DR_WATCH=1: on the first nonzero button read, put a hardware read/write watchpoint on the game's pad
 * buffer (all threads) and log which guest instructions touch it. Development aid for input tracing. */
#include <windows.h>
#include <tlhelp32.h>
static LONG WINAPI wp_handler(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode!=EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6&1)) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip=(uintptr_t)e->ContextRecord->Rip;
    if (rip>=runtime_image_base && rip<runtime_image_base+0x3400000) {
        static uintptr_t seen[128]; static volatile LONG count;
        LONG n=count; int found=0;
        for (LONG i=0;i<n && i<128;++i) if (seen[i]==rip) { found=1; break; }
        if (!found && n<128 && InterlockedCompareExchange(&count,n+1,n)==n) {
            seen[n]=rip;
            printf("Watch: pad buffer accessed by guest +0x%llx (thread %lu)\n",(unsigned long long)(rip-runtime_image_base),GetCurrentThreadId());
        }
    }
    e->ContextRecord->Dr6=0;
    e->ContextRecord->EFlags|=0x10000; /* resume flag: do not re-trigger on this instruction */
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void watch_install(void *address) {
    AddVectoredExceptionHandler(1,wp_handler);
    DWORD self=GetCurrentThreadId(), pid=GetCurrentProcessId();
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 te={.dwSize=sizeof te};
    int n=0;
    for (BOOL ok=Thread32First(snap,&te); ok; ok=Thread32Next(snap,&te)) {
        if (te.th32OwnerProcessID!=pid) continue;
        HANDLE h=OpenThread(THREAD_ALL_ACCESS,FALSE,te.th32ThreadID);
        if (!h) continue;
        if (te.th32ThreadID!=self) SuspendThread(h);
        CONTEXT c={.ContextFlags=CONTEXT_DEBUG_REGISTERS};
        if (GetThreadContext(h,&c)) {
            c.Dr0=(DWORD64)(uintptr_t)address; c.Dr7=1|(3u<<16)|(3u<<18);
            if (SetThreadContext(h,&c)) ++n;
        }
        if (te.th32ThreadID!=self) ResumeThread(h);
        CloseHandle(h);
    }
    CloseHandle(snap);
    printf("Watch: pad buffer %p armed on %d threads\n",address,n);
}
/* The car's simulation for the physics force feedback model. Each physics step the game (guest +0xd93790) fills a
 * 0x30-byte block from the player's car and passes it to +0xe0f5d0, which copies it into its force feedback object
 * (+0xf4..+0x124; read by +0xe0ebb0, which makes the Logitech forces from it). That setter is replaced here: the same
 * copy, and the pointers the caller read the block from are kept (its frame: rbp-0x78 = the car's simulation
 * interface, whose +0x8 is the car's state and +0x20 its setup, whose +0x8 is the drivetrain and tyres). */
/* caller_rbp and ret: the caller's frame and the return address, passed in by the patched code */
static ABI void ffb_inputs_hook(uint8_t *obj, const uint8_t *in, const uintptr_t *caller, uintptr_t ret) {
    memcpy(obj+0xf4,in,0x30);
    if (ret!=runtime_image_base+0xd93c3e) return;
    if (!in[0]) { ffb_car.state=ffb_car.setup=ffb_car.sim=0; return; }   /* no car under the player's control */
    /* the caller has just read the block from these (+0xd9393d..+0xd93945), so they are valid */
    const uintptr_t iface=caller[-0x78/8];
    const uintptr_t state=*(const uintptr_t *)(iface+0x8), setup=*(const uintptr_t *)(iface+0x20), sim=*(const uintptr_t *)(setup+0x8);
    if (state!=ffb_car.state || setup!=ffb_car.setup) printf("Runtime: FFB physics: car state %p, setup %p, drivetrain %p\n",(void *)state,(void *)setup,(void *)sim);
    ffb_car.state=state; ffb_car.setup=setup; ffb_car.sim=sim;
    __atomic_store_n(&ffb_car.step_us,now_us(),__ATOMIC_RELEASE);
    if (ffb_physics) ffb_physics_step((const float *)state,in,setup);   /* the setup tells cars apart (the state's memory is reused) */
}
static void ffb_hook_install(void) {
    static bool done;
    if (done || !runtime_image_base) return;
    done=true;
    unsigned char *at=(unsigned char *)(runtime_image_base+0xe0f5d0);
    static const unsigned char expect[]={0x48,0x8b,0x46,0x28,0x48,0x89,0x87,0x1c,0x01,0x00,0x00};   /* mov rax,[rsi+0x28]; mov [rdi+0x11c],rax */
    if (memcmp(at,expect,sizeof expect)) { printf("Runtime: FFB physics: the game's code differs, not hooked\n"); return; }
    DWORD old;
    if (!VirtualProtect(at,32,PAGE_EXECUTE_READWRITE,&old)) return;
    const uint64_t target=(uint64_t)(uintptr_t)ffb_inputs_hook;
    static const unsigned char head[]={0x48,0x89,0xea,0x48,0x8b,0x0c,0x24};   /* mov rdx, rbp; mov rcx, [rsp] */
    memcpy(at,head,sizeof head);
    at[7]=0x48; at[8]=0xb8; memcpy(at+9,&target,8);   /* movabs rax, hook */
    at[17]=0xff; at[18]=0xe0;                          /* jmp rax */
    VirtualProtect(at,32,old,&old);
    FlushInstructionCache(GetCurrentProcess(),at,32);
    printf("Runtime: FFB physics: reading the car's simulation\n");
}
#endif

/* Driving options from the launcher: BB_GEARBOX (automatic, manual), BB_BRAKE_ASSIST (0 off .. 3 high),
 * BB_HARDCORE (0, 1: hardcore handling), BB_WHEEL_ASSIST (0, 1); unset: the game's own choice. They are set with the
 * game's own setters, on its thread, once its options exist and again whenever they differ (the profile loads a
 * little later; while the launcher sets one, it wins over the game's menu). The options (eboot 1.28, its options
 * menu +0x2e18d0..+0x2e20db): S = *(*(*(image+0x1911100)+0x970)+0x788); S+0x10 gearbox (int, 1 automatic; setter
 * +0x1a33c0, which also tells the game), S+0x14 brake assist (float 0, 1/3, 2/3, 1; setter +0x1a3490), S+0x24 hardcore
 * handling (byte; setter +0x1a3a00, which also sets the byte the handling code reads), S+0x1f8 and S+0x258 wheel assist
 * (a byte in the wheel settings of each of the two controller kinds, see +0x1a83b0). */
typedef ABI void (*game_int_setter)(void *, int);
typedef ABI void (*game_float_setter)(void *, float);
static int env_choice(const char *name, const char *const *words, int count) {
    const char *e=getenv(name);
    if (!e || !*e) return -1;
    for (int i=0;i<count;++i) if (!SDL_strcasecmp(e,words[i])) return i;
    const int v=atoi(e);
    return v>=0 && v<count ? v : -1;
}
static void game_options_apply(void) {
    static int gearbox=-2, brake, hardcore, wheel_assist;
    if (gearbox==-2) {
        static const char *const gear[]={"manual","automatic"}, *const level[]={"off","low","medium","high"}, *const onoff[]={"off","on"};
        gearbox=env_choice("BB_GEARBOX",gear,2); brake=env_choice("BB_BRAKE_ASSIST",level,4);
        hardcore=env_choice("BB_HARDCORE",onoff,2); wheel_assist=env_choice("BB_WHEEL_ASSIST",onoff,2);
        if (gearbox>=0 || brake>=0 || hardcore>=0 || wheel_assist>=0)
            printf("Runtime: driving options from the launcher: gearbox %d, brake assist %d, hardcore handling %d, wheel assist %d (-1: the game's)\n",
                   gearbox,brake,hardcore,wheel_assist);
    }
    if (gearbox<0 && brake<0 && hardcore<0 && wheel_assist<0) return;
    static uint64_t last;
    const uint64_t now=now_us();
    if (now-last<250000 || !runtime_image_base) return;
    last=now;
    const unsigned char *g=*(unsigned char *const *)(runtime_image_base+0x1911100);
    const unsigned char *p=g ? *(const unsigned char *const *)(g+0x970) : NULL;
    unsigned char *s=p ? *(unsigned char *const *)(p+0x788) : NULL;
    if (!s) return;
    unsigned char *d=s+0x10;
    static unsigned changes;
    /* the gearbox setter tells the game through its event queue (*(G+0x970)+0x590), which comes a little later */
    const unsigned char *events=*(const unsigned char *const *)(p+0x590);
    if (gearbox>=0 && *(const int *)d!=gearbox && events && *(const void *const *)(events+8)) {
        ((game_int_setter)(runtime_image_base+0x1a33c0))(d,gearbox);
        if (changes++<20) printf("Runtime: gearbox set to %s\n",gearbox ? "automatic" : "manual");
    }
    if (brake>=0 && fabsf(*(const float *)(d+4)-(float)brake/3.0f)>0.01f) {
        ((game_float_setter)(runtime_image_base+0x1a3490))(d,(float)brake/3.0f);
        if (changes++<20) printf("Runtime: brake assist set to %d of 3\n",brake);
    }
    const unsigned char *handling=(const unsigned char *)(runtime_image_base+0x274be46);
    if (hardcore>=0 && (d[0x14]!=hardcore || *handling!=hardcore)) {
        ((game_int_setter)(runtime_image_base+0x1a3a00))(d,hardcore);
        if (changes++<20) printf("Runtime: hardcore handling %s\n",hardcore ? "on" : "off");
    }
    if (wheel_assist>=0 && (s[0x1f8]!=wheel_assist || s[0x258]!=wheel_assist)) {
        s[0x1f8]=s[0x258]=(unsigned char)wheel_assist;
        if (changes++<20) printf("Runtime: wheel assist %s\n",wheel_assist ? "on" : "off");
    }
}
/* Field of view: data/user/camera.ini fov=<percent of the game's own> (the launcher's slider), F5 narrower and F6
 * wider while playing (saved there). The game builds every camera's projection from half its angle, the angle times
 * the float at +0x12efac4 (0.5; the "FOV fix for wide resolutions" patch makes it 0.65625), read each frame
 * (+0x731cb0): percent/100 x 0.5 goes there. At 100% (or with no file) the value is left as the game and patches
 * have it. */
#ifdef _WIN32
static void camera_fov_set(int percent) {
    float *k=(float *)(runtime_image_base+0x12efac4);
    DWORD old;
    if (!VirtualProtect(k,4,PAGE_READWRITE,&old)) return;
    *k=0.5f*(float)percent/100.0f;
    VirtualProtect(k,4,old,&old);
}
static void camera_fov_path(char *path, size_t size) {
    const char *user=runtime_file_user_dir();
    snprintf(path,size,"%s/camera.ini",user && *user ? user : ".");
}
static void camera_fov_apply(void) {
    static int percent=-1; static bool loaded;
    if (!runtime_image_base) return;
    if (!loaded) {
        loaded=true;
        char path[600]; camera_fov_path(path,sizeof path);
        FILE *f=fopen(path,"rb");
        if (f) {
            char line[64];
            while (fgets(line,sizeof line,f)) if (!strncmp(line,"fov=",4)) percent=atoi(line+4);
            fclose(f);
        }
        if (percent<50 || percent>150) percent=-1;
        if (percent>0 && percent!=100) { camera_fov_set(percent); printf("Runtime: field of view %d%% of the game's\n",percent); }
    }
    /* F5 and F6 while the game's window is in front */
    static bool f5, f6;
    const bool f5_now=(GetAsyncKeyState(VK_F5)&0x8000)!=0, f6_now=(GetAsyncKeyState(VK_F6)&0x8000)!=0;
    const int step=(f6_now && !f6) ? 5 : (f5_now && !f5) ? -5 : 0;
    f5=f5_now; f6=f6_now;
    if (!step) return;
    DWORD pid=0; GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    if (pid!=GetCurrentProcessId()) return;
    if (percent<0) percent=(int)lroundf(*(const float *)(runtime_image_base+0x12efac4)/0.5f*100.0f);   /* from the game's (or the patch's) */
    percent=percent+step<50 ? 50 : percent+step>150 ? 150 : percent+step;   /* the launcher slider's range */
    camera_fov_set(percent);
    char path[600]; camera_fov_path(path,sizeof path);
    FILE *f=fopen(path,"wb");
    if (f) { fprintf(f,"fov=%d\n",percent); fclose(f); }
    char note[96]; snprintf(note,sizeof note,"Field of view %d%%   (F5 narrower, F6 wider)",percent);
    bbgpu_overlay_note(note);
}
#else
static void camera_fov_apply(void) {}
#endif
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    runtime_exit_tick();
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    if (is_wheel_port(handle)) { wheel_sample(data,bbgpu_overlay_captures_input()); wheel_last_read=*data; }
    if (exit_suppressed || bbgpu_overlay_captures_input()) neutralize_pad(data);
    pad_last_read=*data; pad_was_read=true;
#ifdef _WIN32
    { static int armed; if (data->buttons && !armed && BB_DIAGNOSTIC_ENV("DR_WATCH")) { armed=1; watch_install(data); } }
#endif
    { static unsigned nz; if (data->buttons && nz++<40) printf("Runtime: game read buttons 0x%x at %p from +0x%llx\n",data->buttons,(void *)data,(unsigned long long)((uintptr_t)__builtin_return_address(0)-runtime_image_base)); }
    pthread_mutex_unlock(&lock);
    game_options_apply();   /* the game's thread, outside the lock: the setters tell the rest of the game */
    camera_fov_apply();
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
/* scePadOpenExt: the same standard pad; no extended controller is reported, so logiWheel.prx and the game's own
 * Thrustmaster code leave it alone. A wheel reaches the game as the device class of the type 2 port instead. */
static ABI int32_t pad_open_ext(int32_t user,int32_t type,int32_t index,const void *param) { (void)param; return open_port(user,type==1 ? 0 : type,index,true); }
static ABI int32_t pad_ext_info(int32_t handle,void *info) { PADLOG("scePadGetExtControllerInformation"); (void)info; return handle_ok(handle) ? ERR_INVALID_ARG : ERR_INVALID_HANDLE; }
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) { PADLOG("scePadGetControllerInformation");
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=DEVICE_CLASS_STANDARD;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    if (is_wheel_port(handle) && current_wheel()) info->device_class=DEVICE_CLASS_STEERING_WHEEL;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Like shadPS4: the console's {large, small} motor pair goes to SDL as {low, high} frequency motors, held until the next
 * call (the game sends 0, 0 to stop). */
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    { static unsigned n; if (n++<12 || ((param[0] || param[1]) && n<200)) printf("Runtime: scePadSetVibration large %u small %u\n",param[0],param[1]); }
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g && !SDL_RumbleGamepad(g,(uint16_t)(param[1]*257u),(uint16_t)(param[0]*257u),0xFFFFFFFFu)) {
        static int warned; if (!warned++) printf("Runtime: rumble not available on this pad: %s\n",SDL_GetError());
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static uint8_t light_rgb[3]={0,0,255};
static void apply_light(void) {
    SDL_Gamepad *g=gamepad;
    if (!g) return;
    const uint8_t *c=color_override ? color_rgb : light_rgb;
    SDL_SetGamepadLED(g,c[0],c[1],c[2]);
}
static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *param) { PADLOG("scePadSetLightBar");
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    current_gamepad();
    light_rgb[0]=param[0]; light_rgb[1]=param[1]; light_rgb[2]=param[2];
    apply_light();
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_reset_light_bar(int32_t handle) { PADLOG("scePadResetLightBar");
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    pthread_mutex_lock(&lock);
    current_gamepad();
    light_rgb[0]=0; light_rgb[1]=0; light_rgb[2]=255;
    apply_light();
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle_ok(handle) ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_reset_orientation(int32_t handle) { PADLOG("scePadResetOrientation");
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    motion.q[0]=motion.q[1]=motion.q[2]=0.0f; motion.q[3]=1.0f;
    fusion_reset();
    return 0;
}
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { PADLOG("scePadSetTiltCorrection/motion"); (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpenExt",pad_open_ext}, {"scePadGetExtControllerInformation",pad_ext_info}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadDeviceClassGetExtendedInformation",pad_device_class_extended_info}, {"scePadDeviceClassParseData",pad_device_class_parse},
    {"scePadResetOrientation",pad_reset_orientation},
    {"scePadSetLightBar",pad_light_bar}, {"scePadSetLightBarForTracker",pad_light_bar}, {"scePadResetLightBar",pad_reset_light_bar},
    {"scePadSetLightBarBaseBrightness",pad_ok_handle_flag}, {"scePadSetLightBarBlinking",pad_ok_handle_flag}, {"scePadSetVibrationForce",pad_ok_handle_flag},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) {
    /* logiWheel.prx's exports have no public names: matched by NID, ahead of the module's own code */
    for (size_t i=0;i<sizeof(logi_exports)/sizeof(*logi_exports);++i)
        if (!strncmp(name,logi_exports[i].nid,11) && name[11]=='#') return (uintptr_t)logi_exports[i].function;
    return RUNTIME_LOOKUP(exports,name);
}
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s, wheel=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none",wheel ? SDL_GetJoystickName(wheel) : "none"); }
