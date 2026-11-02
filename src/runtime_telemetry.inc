/* Car data for the telemetry output and the adaptive triggers, read every frame whatever the controller is.
 *
 * Each frame the game calls +0xd93790 (call site +0xd93044) once per local player to feed the steering wheel's force
 * feedback; for the player's own car it gets the car through the controller object. That call is redirected here
 * (tele_entry), the same chain is followed to the car (the code at +0xd937a4..+0xd93a09: controller manager
 * [[*(image+0x1911100)+0x970]+0xe0], its +0x48 and +0x80 give the controller, +0xd60700 the driver's object, whose
 * +0x350 returns the car's simulation: +0x8 its state, +0x20 its setup, whose +0x8 is the drivetrain; the driver's
 * +0x180 is the speed in m/s), and the original is called.
 */
#include <windows.h>

typedef ABI void (*tele_entry_fn)(void *, int, void *, int);
typedef ABI uintptr_t (*tele_obj1_fn)(uintptr_t);
typedef ABI uintptr_t (*tele_obj2_fn)(uintptr_t, int);
typedef ABI int (*tele_int_fn)(uintptr_t);
typedef ABI float (*tele_float_fn)(uintptr_t);

static struct { uintptr_t state, setup, drive; float speed; uint64_t us; } tele_car;   /* the last sample; us 0 = none */

static uintptr_t tele_rd(uintptr_t p, size_t off) { return p ? *(const uintptr_t *)(p + off) : 0; }
static uintptr_t tele_vt(uintptr_t obj, size_t off) { return tele_rd(tele_rd(obj, 0), off); }



/* ---- Telemetry out (UDP, the Forza "Dash" layout of Forza Motorsport 7, 311 bytes: SimHub, SimTools and the
 * Buttkicker software read it) and the DualSense's adaptive triggers. Both are off unless asked for.
 * What the game's car state gives (the same block the wheel force feedback reads): per wheel in the order right front,
 * left front, right rear, left rear: load (N) at +0xa50, lateral force +0xa60, longitudinal force +0xa70, slip ratio
 * +0xb30, slip angle +0xb50. The speed is the driver object's. */
#include <winsock2.h>
#include <ws2tcpip.h>

static SOCKET tele_socket = INVALID_SOCKET;
static struct sockaddr_in tele_to;
static int tele_udp_on = -1;
static int tele_triggers_on = -1;

static void tele_put_f(uint8_t *p, size_t at, float v) { memcpy(p + at, &v, 4); }
static void tele_put_i(uint8_t *p, size_t at, int32_t v) { memcpy(p + at, &v, 4); }

static bool tele_udp_init(void) {
    const char *spec = getenv("BB_TELEMETRY");
    if (!spec || !*spec || *spec == '0') return false;
    char host[64] = "127.0.0.1";
    int port = 5300;   /* a port SimHub's Forza option can listen on; BB_TELEMETRY=host:port */
    if (strchr(spec, ':')) {
        const char *colon = strrchr(spec, ':');
        const size_t n = (size_t)(colon - spec) < sizeof host - 1 ? (size_t)(colon - spec) : sizeof host - 1;
        memcpy(host, spec, n);
        host[n] = 0;
        port = atoi(colon + 1);
    } else if (spec[0] != '1' || spec[1]) {
        snprintf(host, sizeof host, "%s", spec);
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    tele_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tele_socket == INVALID_SOCKET) return false;
    memset(&tele_to, 0, sizeof tele_to);
    tele_to.sin_family = AF_INET;
    tele_to.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &tele_to.sin_addr) != 1) {
        printf("Runtime: telemetry: '%s' is not an IPv4 address\n", host);
        closesocket(tele_socket);
        tele_socket = INVALID_SOCKET;
        return false;
    }
    printf("Runtime: telemetry out to %s:%d (Forza Dash layout, 60 Hz)\n", host, port);
    return true;
}

/* On a Ferrari 458: the engine speed (rpm, 800 at idle) is a float at +0xc of the car's
 * state, the gear an int at +0xf8 (1 and up forward, -1 reverse); the drivetrain has the rev limiter at +0xc and the
 * highest engine speed at +0x40. */
static struct { float rpm, max_rpm, limiter; int gear; } tele_engine;
static void tele_send_dash(const uint8_t *state, float speed) {
    static uint64_t last_us;
    const uint64_t now = now_us();
    if (now - last_us < 16000) return;   /* about 60 packets a second */
    last_us = now;
    const float *load = (const float *)(state + 0xa50), *lat = (const float *)(state + 0xa60),
                *lon = (const float *)(state + 0xa70), *ratio = (const float *)(state + 0xb30),
                *angle = (const float *)(state + 0xb50);
    /* Forza's wheel order is front left, front right, rear left, rear right. */
    static const int order[4] = {1, 0, 3, 2};
    uint8_t p[311];
    memset(p, 0, sizeof p);
    tele_put_i(p, 0, 1);                                  /* IsRaceOn */
    tele_put_i(p, 4, (int32_t)(now / 1000));              /* TimestampMS */
    float weight = 0.0f, sum_lat = 0.0f, sum_lon = 0.0f;
    for (int i = 0; i < 4; ++i) { weight += load[i]; sum_lat += lat[i]; sum_lon += lon[i]; }
    const float mass = weight > 1000.0f ? weight / 9.81f : 1000.0f;
    tele_put_f(p, 20, -sum_lat / mass);                   /* AccelerationX: to the right */
    tele_put_f(p, 28, sum_lon / mass);                    /* AccelerationZ: forward */
    tele_put_f(p, 40, speed);                             /* VelocityZ */
    tele_put_f(p, 8, tele_engine.max_rpm);                /* EngineMaxRpm */
    tele_put_f(p, 12, 800.0f);                            /* EngineIdleRpm */
    tele_put_f(p, 16, tele_engine.rpm);                   /* CurrentEngineRpm */
    for (int i = 0; i < 4; ++i) {
        const int w = order[i];
        const float norm = weight > 1.0f ? load[w] / (weight * 0.5f) : 0.5f;
        tele_put_f(p, 68 + 4 * (size_t)i, norm > 1.0f ? 1.0f : norm);   /* NormalizedSuspensionTravel */
        tele_put_f(p, 84 + 4 * (size_t)i, ratio[w]);                     /* TireSlipRatio */
        tele_put_f(p, 164 + 4 * (size_t)i, angle[w]);                    /* TireSlipAngle */
        const float combined = sqrtf(ratio[w] * ratio[w] + angle[w] * angle[w]);
        tele_put_f(p, 180 + 4 * (size_t)i, combined);                    /* TireCombinedSlip */
    }
    tele_put_i(p, 212, 1);                                /* CarOrdinal */
    tele_put_f(p, 244, speed);                            /* Speed (m/s) */
    p[303] = pad_last_read.r2;                            /* Accel */
    p[304] = pad_last_read.l2;                            /* Brake */
    p[308] = (uint8_t)(int8_t)((int)pad_last_read.left_x - 128);   /* Steer */
    p[307] = (uint8_t)(tele_engine.gear < 0 ? 0 : tele_engine.gear);   /* Gear: 0 is reverse */
    sendto(tele_socket, (const char *)p, (int)sizeof p, 0, (const struct sockaddr *)&tele_to, (int)sizeof tele_to);
}

/* The DualSense's adaptive triggers (BB_ADAPTIVE_TRIGGERS=1): the brake trigger firms up and pulses when a front wheel
 * locks, the throttle trigger vibrates when a rear wheel spins. SDL3 passes the raw output report through. */
static void tele_trigger_zones(uint8_t *effect, uint8_t mode, int first_zone, int strength, int frequency) {
    uint32_t zones = 0, force = 0;
    for (int z = first_zone; z < 10; ++z) {
        zones |= 1u << z;
        force |= (uint32_t)((strength - 1) & 7) << (3 * z);
    }
    effect[0] = mode;
    effect[1] = (uint8_t)(zones & 0xff);
    effect[2] = (uint8_t)(zones >> 8);
    memcpy(effect + 3, &force, 4);
    effect[9] = (uint8_t)frequency;
}

static void tele_triggers(const uint8_t *state, float speed) {
    static uint64_t last_us, kick_until, kick_started;
    static uint8_t last[47];
    static int last_gear = 0, kick_strength = 0, kick_right = 0, kick_left = 0;
    const uint64_t now = now_us();
    /* A gear change is felt at once, not at the next report. */
    const int gear = tele_engine.gear;
    if (last_gear != 0 && gear != last_gear && gear != 0) {
        const bool up = gear > last_gear && gear > 1;
        kick_started = now;
        kick_until = now + 90000;
        kick_strength = up ? 8 : 5;
        kick_right = 1;
        kick_left = up ? 0 : 1;
    }
    last_gear = gear;
    if (now - last_us < 20000 && now > kick_until) return;   /* the report goes out at most about 50 times a second */
    last_us = now;
    SDL_Gamepad *g = gamepad;
    if (!g || SDL_GetGamepadType(g) != SDL_GAMEPAD_TYPE_PS5) return;
    const float *ratio = (const float *)(state + 0xb30);
    const float front = fminf(ratio[0], ratio[1]);   /* most negative: a locking front wheel */
    const float rear = fmaxf(fabsf(ratio[2]), fabsf(ratio[3]));
    const bool moving = speed > 8.0f;   /* slip ratios jump about at walking pace: no rattle from a standstill */
    uint8_t report[47];
    memset(report, 0, sizeof report);
    report[0] = 0x04 | 0x08;   /* the right and the left trigger effect */
    uint8_t *right = report + 10, *left = report + 21;
    if (now < kick_until && kick_right) {
        tele_trigger_zones(right, 0x21, 0, 8, 0);                /* a wall through the whole travel: the shift pushes the trigger back */
    } else if (tele_engine.limiter > 1000.0f && tele_engine.rpm >= tele_engine.limiter * 0.985f &&
               pad_last_read.r2 > 80) {
        tele_trigger_zones(right, 0x26, 2, 4, 45);               /* the engine on its rev limiter */
    } else if (moving && rear > 0.3f && pad_last_read.r2 > 80) {
        const int strength = rear > 0.7f ? 7 : rear > 0.5f ? 5 : 3;
        tele_trigger_zones(right, 0x26, 2, strength, 30);        /* a rear wheel spinning */
    } else {
        tele_trigger_zones(right, 0x21, 3, 2, 0);                /* a light, even resistance */
    }
    if (now < kick_until && kick_left) {
        tele_trigger_zones(left, 0x21, 0, kick_strength, 0);     /* a downshift pushes the brake side too */
    } else if (moving && front < -0.3f && pad_last_read.l2 > 80) {
        tele_trigger_zones(left, 0x26, 2, 6, 20);                /* a front wheel locking */
    } else {
        tele_trigger_zones(left, 0x21, 2, 4, 0);                 /* a firmer brake pedal */
    }
    (void)kick_started;
    if (memcmp(report, last, sizeof report) == 0) return;
    memcpy(last, report, sizeof report);
    SDL_SendGamepadEffect(g, report, (int)sizeof report);
}

/* Called for every frame the player's car was found. */
static void tele_publish(const uint8_t *state, float speed) {
    if (tele_udp_on < 0) tele_udp_on = tele_udp_init();
    if (tele_triggers_on < 0) {
        const char *e = getenv("BB_ADAPTIVE_TRIGGERS");
        tele_triggers_on = e && *e && *e != '0';
        if (tele_triggers_on) printf("Runtime: adaptive triggers on (DualSense only)\n");
    }
    if (tele_udp_on) tele_send_dash(state, speed);
    if (tele_triggers_on) {
        tele_triggers(state, speed);
        hap_feed(state, speed, tele_engine.rpm, tele_engine.limiter, tele_engine.gear, pad_last_read.r2 * (1.0f / 255.0f), pad_last_read.l2 * (1.0f / 255.0f));
    }
}

/* Whether the car hook is wanted at all: otherwise it is not installed and the game runs as without it. */
static bool tele_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) {
        const char *a = getenv("BB_TELEMETRY"), *b = getenv("BB_ADAPTIVE_TRIGGERS");
        wanted = (a && *a && *a != '0') || (b && *b && *b != '0');
    }
    return wanted;
}

/* One frame, for the local player `index`. Anything missing on the way means no car is driven. */
static void tele_sample(int index) {
    static int stage_logged=-1;
    int stage=0;
    const uintptr_t base=runtime_image_base;
    uintptr_t iface=0, driver=0;
    do {
        const uintptr_t manager=tele_rd(tele_rd(tele_rd(base+0x1911100,0),0x970),0xe0);
        if (!manager) break;
        stage=1;
        const uintptr_t slot=((tele_obj2_fn)tele_vt(manager,0x48))(manager,index);
        if (!slot) break;
        stage=2;
        const uintptr_t controller=((tele_obj1_fn)tele_vt(slot,0x80))(slot);
        if (!controller) break;
        stage=3;
        driver=((tele_obj1_fn)(base+0xd60700))(controller);
        if (!driver) break;
        stage=4;
        if (((tele_int_fn)tele_vt(controller,0xa8))(controller)!=3) break;   /* not driving */
        stage=5;
        iface=((tele_obj1_fn)tele_vt(driver,0x350))(driver);
        if (!iface) break;
        stage=6;
    } while (0);
    if (stage!=stage_logged) {   /* where the chain stops, when it changes: no car, or the game's code is not what was read */
        stage_logged=stage;
        printf("Runtime: telemetry: car chain stage %d%s\n",stage,stage==6 ? " (car found)" : "");
    }
    if (stage<6) { tele_car.us=0; return; }
    const uintptr_t state=tele_rd(iface,0x8), setup=tele_rd(iface,0x20), drive=tele_rd(setup,0x8);
    if (!state || !setup || !drive) { tele_car.us=0; return; }
    const float speed=((tele_float_fn)tele_vt(driver,0x180))(driver);
    tele_car.state=state; tele_car.setup=setup; tele_car.drive=drive; tele_car.speed=speed;
    tele_car.us=now_us();
    tele_engine.rpm=*(const float *)(state+0xc); tele_engine.gear=*(const int32_t *)(state+0xf8);
    tele_engine.max_rpm=*(const float *)(drive+0x40); tele_engine.limiter=*(const float *)(drive+0x0c);
    tele_publish((const uint8_t *)state,speed);
}

static ABI void tele_entry(void *a, int type, void *b, int index) {
    if (index==0) tele_sample(index);
    ((tele_entry_fn)(runtime_image_base+0xd93790))(a,type,b,index);
}

/* A stub within reach of the call: memory allocated near the image, or else the padding after a function in it (it
 * holds the 12 bytes of the stub; never executed otherwise). */
static unsigned char *tele_stub_place(unsigned char *where) {
    for (uintptr_t at=((uintptr_t)where&~(uintptr_t)0xffff)-0x10000; at>0x10000 && (uintptr_t)where-at<0x70000000; at-=0x10000) {
        unsigned char *p=(unsigned char *)VirtualAlloc((void *)at,64,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    for (uintptr_t at=((uintptr_t)where&~(uintptr_t)0xffff)+0x4000000; at<(uintptr_t)where+0x70000000; at+=0x10000) {
        unsigned char *p=(unsigned char *)VirtualAlloc((void *)at,64,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    for (unsigned char *p=where-0x100000; p<where+0x100000; ++p) {
        if (p[-1]!=0xc3) continue;
        uintptr_t q=((uintptr_t)p+15)&~(uintptr_t)15;
        if (q-(uintptr_t)p<12) continue;
        bool pad=true;
        for (unsigned char *b=p; b<(unsigned char *)q; ++b)
            if (*b!=0x66 && *b!=0x2e && *b!=0x0f && *b!=0x1f && *b!=0x84 && *b!=0x00 && *b!=0x90 && *b!=0x44 && *b!=0x40 && *b!=0x80 && *b!=0xcc) { pad=false; break; }
        if (!pad) continue;
        DWORD old;
        if (VirtualProtect(p,12,PAGE_EXECUTE_READWRITE,&old)) return p;
    }
    return NULL;
}

/* Redirect the call at +0xd93044 to tele_entry through a stub within reach of it (a call has a 32-bit offset). */
static void tele_install(void) {
    static bool done;
    if (done || !runtime_image_base) return;
    done=true;
    unsigned char *call=(unsigned char *)(runtime_image_base+0xd93044);
    int32_t rel; memcpy(&rel,call+1,4);
    if (call[0]!=0xe8 || (uintptr_t)(call+5+rel)!=runtime_image_base+0xd93790) {
        printf("Runtime: telemetry: the game's code differs, not hooked\n");
        return;
    }
    unsigned char *stub=tele_stub_place(call);
    if (!stub) { printf("Runtime: telemetry: no room for the stub near the game's code, not hooked\n"); return; }
    const uint64_t target=(uint64_t)(uintptr_t)tele_entry;
    stub[0]=0x48; stub[1]=0xb8; memcpy(stub+2,&target,8);   /* movabs rax, tele_entry */
    stub[10]=0xff; stub[11]=0xe0;                            /* jmp rax */
    const int64_t off=(int64_t)((uintptr_t)stub-(uintptr_t)(call+5));
    DWORD old;
    if (off!=(int32_t)off || !VirtualProtect(call,5,PAGE_EXECUTE_READWRITE,&old)) { printf("Runtime: telemetry: not hooked\n"); return; }
    const int32_t newrel=(int32_t)off;
    memcpy(call+1,&newrel,4);
    VirtualProtect(call,5,old,&old);
    FlushInstructionCache(GetCurrentProcess(),call,5);
    printf("Runtime: telemetry: reading the player's car every frame\n");
}
