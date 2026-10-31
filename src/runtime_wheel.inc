/* Steering wheels: any wheel Windows lists as a game controller (Moza, Logitech, Thrustmaster, Fanatec, ...) is read
 * through SDL's joystick API and reported to the game the way the console reports a PS4 steering wheel.
 * DriveClub opens a second pad of port type 2 next to the standard one (guest +0xe103d1); when
 * scePadGetControllerInformation gives that pad device class 6 (steering wheel) the game reads it instead of the
 * standard pad, takes the lock from scePadDeviceClassGetExtendedInformation (maxPhysicalWheelAngle) and the wheel
 * from scePadDeviceClassParseData: steering 0..65535 with 32768 the centre, accelerator and brake 0..65535 from
 * released to pressed (guest +0xe10ba0, +0xe10abf, +0xe10b22).
 * The wheel's values travel in the extension bytes of the pad data the game reads, as on the console.
 * Settings are the wheel_* lines of default.ini. Included by runtime_pad.c after runtime_input.inc. */

#define DEVICE_CLASS_STANDARD 0
#define DEVICE_CLASS_STEERING_WHEEL 6
enum { WAX_STEER, WAX_THROTTLE, WAX_BRAKE, WAX_CLUTCH, WAX_HANDBRAKE, WAX_COUNT };
enum { WINV_AUTO, WINV_NO, WINV_YES };
static const char *const wheel_axis_names[WAX_COUNT]={"steering","throttle","brake","clutch","handbrake"};
static struct { int axis, invert; } wheel_axis[WAX_COUNT]={{0,WINV_NO},{1,WINV_AUTO},{2,WINV_AUTO},{-1,WINV_AUTO},{-1,WINV_AUTO}};
static char wheel_device[64]="auto";       /* auto, off, or part of the wheel's name */
static int wheel_rotation=900;              /* degrees lock to lock */
static bool wheel_as_stick, wheel_hat_dpad=true;
/* dev: 0 the wheel, else a slot of button_devs (a third field naming another controller) */
static struct { int index; uint8_t out_kind; uint32_t button; int touch_side; int dev; } wheel_buttons[32];
static int wheel_button_count;
/* wheel_button = <n>, <action>, <part of a controller's name>[ @<which>]: a button of another controller (a button box,
 * a second controller, a Stream Deck that is a game controller), looked for once a second while the wheel is
 * connected (button_device). Slot 0 is unused: dev 0 is the wheel. */
#define BUTTON_DEVICES 4
typedef struct { char name[64]; SDL_Joystick *j; uint64_t last_scan, last_buttons; bool listed; } ButtonDevice;
static ButtonDevice button_devs[BUTTON_DEVICES+1];
static int button_dev_count;
static bool ffb_enabled=true, ffb_invert;  /* force feedback (runtime_logiwheel.inc) */
static int ffb_gain=100, ffb_vibration=100;   /* percent; the strongest forces clip at the wheel's maximum */
static int ffb_damper=-1;                      /* percent of the game's damper; -1: it follows ffb_gain, as before */
static int ffb_spring=-1;                      /* percent of the game's centring spring; -1: it follows ffb_gain */
/* The spring's strongest pull, percent of the wheel's maximum. The game varies the spring's saturation with grip
 * (255 gripping, 15..40 sliding): with a limit set, that range is scaled into the limit, so the grip stays in the
 * force. -1: the saturation follows the gain and clips at the wheel's maximum, as before. */
static int ffb_spring_limit=-1;
static bool ffb_sine;                          /* the game's vibrations as sine waves (road texture on a direct drive) */
/* The game's two vibrations apart: kerbs and rumble strips (its 55..82 ms wave) and the road surface (its 20..30 ms
 * wave, on nearly all the time). Percent; -1: wheel_ffb_vibration. ffb_texture_changes: only the surface's changes
 * (above its own average of the last second or so), not its steady hum. */
static int ffb_kerbs=-1, ffb_texture=-1;
static bool ffb_texture_changes;
/* ffb_long_rumble: percent of the kerb wave kept once it has been on for more than 0.6 s (wheelspin, burnouts);
 * ffb_vibration_limit: the strongest any vibration may be, percent of the wheel's maximum; the game's strongest
 * wave at ffb_kerbs / ffb_texture = 100 gives exactly that. 25: the game's strongest (64 of Logitech's 255), as
 * before these settings. */
static int ffb_long_rumble=100, ffb_vibration_limit=25;
/* ffb_vibration_response: percent; below 100 small vibrations are lifted (the game's level to the power
 * response/100) while the strongest stays at the limit. ffb_texture_floor: percent of the road texture's steady
 * level kept in the changes mode (gravel stays present). */
static int ffb_vibration_response=100, ffb_texture_floor;
/* wheel_ffb_model = software: the steering force is worked out here from the game's spring (its target and grip)
 * every pass of the haptic thread and sent as one force, instead of the game's spring going to the wheel as it is.
 * ffb_falloff: percent the force drops far past the grip limit; ffb_smoothing_ms: the game's values are followed
 * this smoothly between its frames; ffb_soft_lock: degrees lock to lock, -1 the game's operating range, 0 off. */
static bool ffb_software;
static int ffb_falloff=40, ffb_smoothing_ms=8, ffb_soft_lock=-1;
static int ffb_strength=100;   /* percent: the whole steering force of the software model, clipped at the maximum */
/* ffb_center: how much more force the first degrees off the target give (0: in proportion, as the game's spring);
 * ffb_slide: how much more the force drops than the game's grip does (0: as the game's grip). Percent, 0..100. */
static int ffb_center, ffb_slide;
/* wheel_ffb_model = physics: the steering force from the car's front tyres in the game's simulation (the moment their
 * lateral forces put on the steering, runtime_logiwheel.inc), with settings named after Assetto Corsa's, percent:
 * gain (100: a 1 g corner at about 60% of the wheel's maximum), filter, minimum force, kerb, road and slip effects,
 * and a damper that is strongest at a standstill. */
static bool ffb_physics;
static int ffb_ph_gain=100, ffb_ph_filter=0, ffb_ph_min_force=0, ffb_ph_kerbs=50, ffb_ph_road=50, ffb_ph_slip=30, ffb_ph_damper=10;
/* The tyres' trail follows how much of their grip the game's forces say is used (light at the real limit) and the force
 * keeps falling past it; wheel_ffb_physics_tyre = classic: the trail from the brush model's slip, as before. */
static bool ffb_ph_tyre_grip=true;
/* The front tyres' braking and drive forces through the scrub radius (runtime_logiwheel.inc); wheel_ffb_physics_scrub =
 * off leaves them out. */
static bool ffb_ph_scrub=true;
/* The trail from the grip in use by braking, drive and cornering together (the friction circle), and the tyres' grip
 * learnt that way; wheel_ffb_physics_combined = off: from cornering alone. */
static bool ffb_ph_combined=true;
/* Self-centring from the steering axis' inclination (the front wheels' load, the steering angle); wheel_ffb_physics_centre
 * = off leaves it out. */
static bool ffb_ph_centre=true;
/* Each front tyre's moment follows over its relaxation length (runtime_logiwheel.inc); wheel_ffb_physics_relax = off:
 * the game's forces as they come, step by step. */
static bool ffb_ph_relax=true;
/* wheel_button = <n>, ffb_gain_up / ffb_gain_down: wheel buttons that make the physics model's force for the car being
 * driven 5% stronger or weaker, kept for each car (runtime_logiwheel.inc, ffb_cars.ini); -1: not bound. ffb_gain_dev:
 * the controller, as wheel_buttons' dev. Keys too: ffb_gain_up = <key> (runtime_input.inc). */
static int ffb_gain_button[2]={-1,-1}, ffb_gain_dev[2];

static SDL_Joystick *wheel;
static SDL_JoystickID wheel_id;
static bool wheel_inverted[WAX_COUNT];      /* decided when the wheel is opened (pedals at rest) */
/* Many wheels and pedals send nothing until an axis moves, and SDL reads such an axis as 0 (half pressed) until then.
 * A pedal reads released until it leaves the value it had when its controller was opened. */
static int wheel_rest_raw[WAX_COUNT];
static bool wheel_touched[WAX_COUNT];

/* Pedals: axes of the wheel (wheel_pedals = wheel) or a controller of their own on another USB (Fanatec, Moza,
 * Simagic, Heusinkveld... pedals): part of its name. The pedal axis numbers are then that controller's. */
static char pedals_device[64]="wheel";
static SDL_Joystick *pedals;
static SDL_JoystickID pedals_id;

/* Handbrake: an axis of the pedals' controller (wheel_handbrake_device = pedals, the default), of the wheel's, or of a
 * controller of its own (a USB handbrake: part of its name); wheel_handbrake = button <n> for a handbrake that is a
 * switch. The game takes it as the pad's circle (see wheel_sample). */
static char handbrake_device[64]="pedals";
static int handbrake_button=-1;
static SDL_Joystick *handbrake;
static SDL_JoystickID handbrake_id;

/* H-pattern shifter: a controller of its own (a Logitech Driving Force Shifter on an adapter, ...) or buttons of the
 * wheel. Each wheel_gear line gives the button held in one gear. The game takes the gear the way a Logitech G29
 * gives its shifter's (guest +0xe132c0): gears 1..6 as the bits 1, 2, 4, 8, 16, 32, reverse 0x80, neutral 0. */
static char shifter_device[64]="auto";      /* auto (a controller named "shifter"), wheel, off, or part of a name */
static struct { int index; uint8_t gear_bits; } shifter_gears[8];
static int shifter_gear_count;
static SDL_Joystick *shifter;

typedef struct { uint16_t steer, throttle, brake, clutch, handbrake; uint8_t gear, magic; } WheelExtension;
#define WHEEL_MAGIC 0x57
_Static_assert(sizeof(WheelExtension)<=sizeof(((PadData *)0)->extension),"wheel data fits the pad extension");
typedef struct {
    int32_t device_class; uint8_t reserved[4];
    uint8_t capability, reserved1; uint16_t max_physical_wheel_angle; uint8_t reserved2[8];
} DeviceClassExtendedInfo;
typedef struct {
    int32_t device_class; uint8_t data_valid, pad[3];
    float steering_wheel_angle; uint16_t steering_wheel, accelerator_pedal, brake_pedal, clutch_pedal, hand_brake;
    uint8_t gear, reserved;
} DeviceClassData;
_Static_assert(sizeof(DeviceClassExtendedInfo)==20,"ScePadDeviceClassExtendedInformation layout");
_Static_assert(sizeof(DeviceClassData)==24,"ScePadDeviceClassData layout");
_Static_assert(__builtin_offsetof(DeviceClassExtendedInfo,max_physical_wheel_angle)==10,"maxPhysicalWheelAngle offset (guest +0xe1027d)");
_Static_assert(__builtin_offsetof(DeviceClassData,steering_wheel)==12,"steeringWheel offset (guest +0xe10bf4)");
_Static_assert(__builtin_offsetof(DeviceClassData,accelerator_pedal)==14,"acceleratorPedal offset (guest +0xe10ade)");

/* wheel_<setting> = <value>; lhs is lower case, rhs as written. */
static void wheel_parse(const char *lhs, char *rhs) {
    const char *key=lhs+6;
    if (!strcmp(key,"device")) { snprintf(wheel_device,sizeof wheel_device,"%s",rhs); return; }
    if (!strcmp(key,"shifter")) { snprintf(shifter_device,sizeof shifter_device,"%s",rhs); return; }
    if (!strcmp(key,"pedals")) { snprintf(pedals_device,sizeof pedals_device,"%s",rhs); return; }
    if (!strcmp(key,"handbrake_device")) { snprintf(handbrake_device,sizeof handbrake_device,"%s",rhs); return; }
    if (!strcmp(key,"handbrake")) {   /* button <n> here; an axis below, as the pedals */
        char v[32]; snprintf(v,sizeof v,"%s",rhs); lower(v);
        const char *s=v; while (*s==' ') ++s;
        handbrake_button=-1;
        if (!strncmp(s,"button",6)) { handbrake_button=atoi(s+6); wheel_axis[WAX_HANDBRAKE].axis=-1; return; }
    }
    if (!strcmp(key,"gear")) {
        /* <button number>, <1..6|r> */
        int index=-1; char g[8]="";
        if (sscanf(rhs," %d , %7s",&index,g)!=2 || index<0 || shifter_gear_count>=(int)(sizeof shifter_gears/sizeof *shifter_gears)) return;
        lower(g);
        const int n=atoi(g);
        const uint8_t bits=g[0]=='r' ? 0x80 : n>=1 && n<=6 ? (uint8_t)(1u<<(n-1)) : 0;
        if (!bits) return;
        shifter_gears[shifter_gear_count].index=index; shifter_gears[shifter_gear_count].gear_bits=bits;
        ++shifter_gear_count;
        return;
    }
    if (!strcmp(key,"rotation")) { int v=atoi(rhs); wheel_rotation=v<90 ? 90 : v>3600 ? 3600 : v; return; }
    if (!strcmp(key,"ffb_gain") || !strcmp(key,"ffb_vibration") || !strcmp(key,"ffb_damper") || !strcmp(key,"ffb_spring")) {
        lower(rhs);
        int v=atoi(rhs); v=v<0 ? 0 : v>2000 ? 2000 : v;
        if (!strcmp(key,"ffb_gain")) ffb_gain=v;
        else if (!strcmp(key,"ffb_vibration")) ffb_vibration=v;
        else if (!strcmp(key,"ffb_damper")) ffb_damper=!strcmp(rhs,"gain") ? -1 : v;
        else ffb_spring=!strcmp(rhs,"gain") ? -1 : v;
        return;
    }
    if (!strcmp(key,"ffb_spring_limit")) {
        lower(rhs);
        const int v=atoi(rhs);
        ffb_spring_limit=!strcmp(rhs,"off") || !strcmp(rhs,"gain") ? -1 : v<5 ? 5 : v>100 ? 100 : v;
        return;
    }
    if (!strcmp(key,"ffb_vibration_wave")) {
        lower(rhs);
        ffb_sine=!strcmp(rhs,"sine");
        return;
    }
    if (!strcmp(key,"ffb_kerbs") || !strcmp(key,"ffb_texture")) {
        lower(rhs);
        int v=atoi(rhs); v=v<0 ? 0 : v>2000 ? 2000 : v;
        if (!strcmp(rhs,"vibration")) v=-1;
        if (!strcmp(key,"ffb_kerbs")) ffb_kerbs=v; else ffb_texture=v;
        return;
    }
    if (!strcmp(key,"ffb_texture_mode")) {
        lower(rhs);
        ffb_texture_changes=!strcmp(rhs,"changes");
        return;
    }
    if (!strcmp(key,"ffb_vibration_response")) { const int v=atoi(rhs); ffb_vibration_response=v<20 ? 20 : v>100 ? 100 : v; return; }
    if (!strcmp(key,"ffb_texture_floor")) { const int v=atoi(rhs); ffb_texture_floor=v<0 ? 0 : v>100 ? 100 : v; return; }
    if (!strcmp(key,"ffb_long_rumble") || !strcmp(key,"ffb_vibration_limit")) {
        int v=atoi(rhs); v=v<0 ? 0 : v>100 ? 100 : v;
        if (!strcmp(key,"ffb_long_rumble")) ffb_long_rumble=v; else ffb_vibration_limit=v;
        return;
    }
    if (!strcmp(key,"ffb_model")) { lower(rhs); ffb_software=!strcmp(rhs,"software"); ffb_physics=!strcmp(rhs,"physics"); return; }
    if (!strncmp(key,"ffb_physics_",12)) {
        const char *k=key+12; const int v=atoi(rhs);
        if (!strcmp(k,"gain")) ffb_ph_gain=v<0 ? 0 : v>500 ? 500 : v;
        else if (!strcmp(k,"filter")) ffb_ph_filter=v<0 ? 0 : v>100 ? 100 : v;
        else if (!strcmp(k,"min_force")) ffb_ph_min_force=v<0 ? 0 : v>30 ? 30 : v;
        else if (!strcmp(k,"kerbs")) ffb_ph_kerbs=v<0 ? 0 : v>200 ? 200 : v;
        else if (!strcmp(k,"road")) ffb_ph_road=v<0 ? 0 : v>200 ? 200 : v;
        else if (!strcmp(k,"slip")) ffb_ph_slip=v<0 ? 0 : v>200 ? 200 : v;
        else if (!strcmp(k,"damper")) ffb_ph_damper=v<0 ? 0 : v>100 ? 100 : v;
        else if (!strcmp(k,"tyre")) { lower(rhs); ffb_ph_tyre_grip=strncmp(rhs,"classic",7)!=0; }
        else if (!strcmp(k,"scrub")) { lower(rhs); ffb_ph_scrub=strcmp(rhs,"off") && strcmp(rhs,"false") && strcmp(rhs,"0"); }
        else if (!strcmp(k,"combined")) { lower(rhs); ffb_ph_combined=strcmp(rhs,"off") && strcmp(rhs,"false") && strcmp(rhs,"0"); }
        else if (!strcmp(k,"centre") || !strcmp(k,"center")) { lower(rhs); ffb_ph_centre=strcmp(rhs,"off") && strcmp(rhs,"false") && strcmp(rhs,"0"); }
        else if (!strcmp(k,"relax")) { lower(rhs); ffb_ph_relax=strcmp(rhs,"off") && strcmp(rhs,"false") && strcmp(rhs,"0"); }
        return;
    }
    if (!strcmp(key,"ffb_strength")) { const int v=atoi(rhs); ffb_strength=v<0 ? 0 : v>1000 ? 1000 : v; return; }
    if (!strcmp(key,"ffb_center")) { const int v=atoi(rhs); ffb_center=v<0 ? 0 : v>100 ? 100 : v; return; }
    if (!strcmp(key,"ffb_slide")) { const int v=atoi(rhs); ffb_slide=v<0 ? 0 : v>100 ? 100 : v; return; }
    if (!strcmp(key,"ffb_falloff")) { const int v=atoi(rhs); ffb_falloff=v<0 ? 0 : v>100 ? 100 : v; return; }
    if (!strcmp(key,"ffb_smoothing")) { const int v=atoi(rhs); ffb_smoothing_ms=v<0 ? 0 : v>100 ? 100 : v; return; }
    if (!strcmp(key,"ffb_soft_lock")) {
        lower(rhs);
        const int v=atoi(rhs);
        ffb_soft_lock=!strcmp(rhs,"game") ? -1 : !strcmp(rhs,"off") ? 0 : v<90 ? 0 : v>3600 ? 3600 : v;
        return;
    }
    if (!strcmp(key,"as_stick") || !strcmp(key,"hat_dpad") || !strcmp(key,"ffb") || !strcmp(key,"ffb_invert")) {
        lower(rhs);
        const bool on=!strcmp(rhs,"true") || !strcmp(rhs,"on") || !strcmp(rhs,"yes") || !strcmp(rhs,"1");
        if (!strcmp(key,"as_stick")) wheel_as_stick=on;
        else if (!strcmp(key,"hat_dpad")) wheel_hat_dpad=on;
        else if (!strcmp(key,"ffb")) ffb_enabled=on;
        else ffb_invert=on;
        return;
    }
    for (int a=0;a<WAX_COUNT;++a) if (!strcmp(key,wheel_axis_names[a])) {
        /* <axis number>|none [, inverted|normal] */
        char what[16]="", flag[16]="";
        sscanf(rhs," %15[^, ] , %15s",what,flag);
        lower(what); lower(flag);
        if (!strncmp(what,"axis",4)) memmove(what,what+4,strlen(what+4)+1);
        wheel_axis[a].axis=(!*what || !strcmp(what,"none") || !strcmp(what,"off")) ? -1 : atoi(what);
        wheel_axis[a].invert=!strcmp(flag,"inverted") ? WINV_YES : !strcmp(flag,"normal") ? WINV_NO : a==WAX_STEER ? WINV_NO : WINV_AUTO;
        return;
    }
    if (!strcmp(key,"button")) {
        /* <button number>, <PS4 button>[, <another controller's name>] */
        int index=-1; char name[32]="", devname[64]="";
        const int got=sscanf(rhs," %d , %31[^,] , %63[^\n]",&index,name,devname);
        if (got<2 || index<0 || wheel_button_count>=(int)(sizeof wheel_buttons/sizeof *wheel_buttons)) return;
        trim(name); lower(name); trim(devname);
        int dev=0;
        if (got==3 && *devname && SDL_strcasecmp(devname,"wheel")) {
            for (int k=1;k<=button_dev_count && !dev;++k) if (!SDL_strcasecmp(button_devs[k].name,devname)) dev=k;
            if (!dev && button_dev_count<BUTTON_DEVICES) {
                dev=++button_dev_count;
                snprintf(button_devs[dev].name,sizeof button_devs[dev].name,"%s",devname);
            }
            if (!dev) return;
        }
        if (!strcmp(name,"ffb_gain_up") || !strcmp(name,"ffb_gain_down")) { ffb_gain_button[name[9]=='d']=index; ffb_gain_dev[name[9]=='d']=dev; return; }
        int slot=wheel_button_count;
        wheel_buttons[slot].index=index;
        wheel_buttons[slot].dev=dev;
        if (!strcmp(name,"touchpad") || !strcmp(name,"touchpad_left") || !strcmp(name,"touchpad_right") || !strcmp(name,"touchpad_center")) {
            wheel_buttons[slot].out_kind=OUT_TOUCH;
            wheel_buttons[slot].touch_side=!strcmp(name,"touchpad_right") ? 2 : !strcmp(name,"touchpad_center") ? 1 : 0;
            ++wheel_button_count;
            return;
        }
        for (size_t i=0;i<sizeof(button_names)/sizeof(*button_names);++i) if (!strcmp(name,button_names[i].name)) {
            wheel_buttons[slot].out_kind=OUT_BUTTON; wheel_buttons[slot].button=button_names[i].ps;
            ++wheel_button_count;
            return;
        }
    }
}

#define WHEEL_SECTION_MARKER "# Steering wheel"
static const char wheel_default_config[]=
    "\n" WHEEL_SECTION_MARKER "\n"
    "# Any wheel that Windows lists as a game controller (Moza, Logitech, Thrustmaster, Fanatec...) is shown to the game\n"
    "# as a PS4 steering wheel. The log (logs\\driveclub_*.log) names the wheel that was found and, for the first moves,\n"
    "# the numbers of the axes and buttons you touch (\"Runtime: wheel axis 2 ...\", \"Runtime: wheel button 5\").\n"
    "# wheel_device: auto (a controller whose name says it is a wheel), off, or part of the name the log shows;\n"
    "# a wheel that is several controllers of one name (a Fanatec base is two \"FANATEC Wheel\") is told apart by\n"
    "# \" @col01\" (part of its device path) or \" @2\" (the second of that name) after the name; WheelSetup writes it.\n"
    "wheel_device = auto\n"
    "# Lock to lock rotation in degrees: the same as in the wheel's own software (Moza Pit House: steering wheel angle).\n"
    "wheel_rotation = 900\n"
    "# Axis numbers (or none). Add \", inverted\" to turn an axis around. A pedal that reads pressed at rest when the\n"
    "# game starts is turned around by itself; \", normal\" stops that.\n"
    "# wheel_pedals: wheel (the pedals are plugged into the wheel) or part of the name of the pedals' own controller\n"
    "# when they have a USB of their own; the pedal axis numbers are then that controller's.\n"
    "# wheel_handbrake_device: pedals (an axis of the pedals' controller), wheel, or part of the name of a handbrake\n"
    "# with a USB of its own; wheel_handbrake = <axis number> or button <number> (a handbrake that is a switch).\n"
    "wheel_pedals = wheel\n"
    "wheel_steering = 0\n"
    "wheel_throttle = 1\n"
    "wheel_brake = 2\n"
    "wheel_clutch = none\n"
    "wheel_handbrake = none\n"
    "# Wheel buttons: wheel_button = <button number>, <PS4 button>; PS4 buttons as above (cross, r1, options,\n"
    "# pad_up, touchpad_left...). The paddles are usually the first two: shift down is l1, shift up is r1.\n"
    "# ffb_gain_up and ffb_gain_down in place of a PS4 button: the force feedback of the car being driven 5% stronger\n"
    "# or weaker (Reworked Physics), kept for each car in ffb_cars.ini (WheelSetup has them as FFB gain + and -).\n"
    "# A button of another controller (a button box): wheel_button = <button number>, <PS4 button>, <part of its name>.\n"
    "# Keys (a Stream Deck's too): <PS4 button> = <key> as in the keyboard bindings above, ffb_gain_up = <key> likewise.\n"
    "wheel_button = 0, r1\n"
    "wheel_button = 1, l1\n"
    "# The wheel's hat switch works as the d-pad.\n"
    "wheel_hat_dpad = true\n"
    "# true also moves the left stick and the l2/r2 triggers with the wheel and pedals.\n"
    "wheel_as_stick = false\n"
    "# Force feedback: the game is told a Logitech wheel is connected and its forces go to this wheel.\n"
    "# wheel_ffb = off goes back to a wheel without forces. wheel_ffb_gain: percent of the game's forces (0..2000;\n"
    "# the game's steering forces are light on most wheels, so several hundred is normal; the strongest forces clip\n"
    "# at the wheel's maximum). wheel_ffb_vibration: the same for the road and kerb vibrations (0..2000), which the\n"
    "# game keeps weak (at most 25%).\n"
    "# wheel_ffb_damper: percent of the game's damper, the resistance to turning the wheel at all (0..2000), or gain\n"
    "# (it follows wheel_ffb_gain). Lower it when the wheel feels heavy and dull at a high gain.\n"
    "# wheel_ffb_spring: percent of the game's road-feel spring, which pulls the wheel to where the tyres want it\n"
    "# (0..2000), or gain. wheel_ffb_spring_limit: its strongest pull, percent of the wheel's maximum (5..100), or\n"
    "# off: lower it when the wheel fights back too hard far from the centre (holding a drift). The game makes the\n"
    "# spring lighter as the tyres lose grip; with a limit that lightening is kept in proportion.\n"
    "# wheel_ffb_vibration_wave: square (as the game asks) or sine (smoother road texture on a direct drive wheel).\n"
    "# wheel_ffb_vibration_limit: the strongest any vibration may be, percent of the wheel's maximum (0..100).\n"
    "# wheel_ffb_kerbs and wheel_ffb_texture: the two vibrations apart, kerbs and rumble strips (also wheelspin) and\n"
    "# the road surface, as percent of that limit at the game's strongest (0..100, or vibration: wheel_ffb_vibration).\n"
    "# wheel_ffb_texture_mode: full, or changes (only the surface's changes, rough patches and going off the road,\n"
    "# without the steady hum).\n"
    "# wheel_ffb_texture_floor: in the changes mode, percent of the steady level kept (0..100; gravel stays present).\n"
    "# wheel_ffb_vibration_response: 100 in proportion to the game; lower lifts small vibrations (20..100).\n"
    "# wheel_ffb_long_rumble: percent of the kerb vibration kept when it goes on for more than about half a second\n"
    "# (wheelspin, burnouts; 0..100).\n"
    "# wheel_ffb_model: game (the game's spring goes to the wheel as it is) or software (the steering force is worked\n"
    "# out here, smoothed between the game's frames, and falls off past the grip limit). With software:\n"
    "# wheel_ffb_strength: percent of the steering force (0..1000; the log's \"FFB clipping\" lines say how often it\n"
    "# is at the wheel's maximum, where detail is lost: keep that low);\n"
    "# wheel_ffb_center: more force in the first degrees off centre (0..100; 0: in proportion, as the game);\n"
    "# wheel_ffb_slide: the force drops more than the game's grip as the tyres slide (0..100; 0: as the game);\n"
    "# wheel_ffb_falloff: percent the force drops far past the grip limit (0..100; 0 holds it, as the game does);\n"
    "# wheel_ffb_smoothing: milliseconds (0..100); wheel_ffb_soft_lock: game (the steering lock the game asks for),\n"
    "# off, or degrees lock to lock: a force stops the wheel there.\n"
    "# wheel_ffb_model = physics: the force comes from the car's front tyres in the game's simulation, as in a sim. Its\n"
    "# settings, percent: wheel_ffb_physics_gain (0..300; 100: a 1 g corner at about 60% of the wheel's maximum),\n"
    "# wheel_ffb_physics_filter (0..100), wheel_ffb_physics_min_force (0..30, for gear and belt wheels),\n"
    "# wheel_ffb_physics_kerbs, wheel_ffb_physics_road and wheel_ffb_physics_slip (0..200), wheel_ffb_physics_damper\n"
    "# (0..100). wheel_ffb_soft_lock, wheel_ffb_vibration_wave and wheel_ffb_invert apply too.\n"
    "# The wheel goes light where the game's tyres run out of grip and keeps getting lighter in a slide, and the front\n"
    "# tyres' braking and drive forces turn it through the steering geometry (a kerb under braking, torque steer), and\n"
    "# braking hard into a corner lightens it as the front nears its limit (its grip used by braking and cornering\n"
    "# together), and it centres itself when rolling with the wheel turned (the steering axis' inclination); the tyres'\n"
    "# forces build up over their relaxation length (kerb strikes without full-force hits); wheel_ffb_physics_tyre =\n"
    "# classic, wheel_ffb_physics_scrub = off, wheel_ffb_physics_combined = off, wheel_ffb_physics_centre = off and\n"
    "# wheel_ffb_physics_relax = off go back to the model before those.\n"
    "# wheel_ffb_invert = true if the wheel pulls the wrong way.\n"
    "# H-pattern shifter: wheel_shifter = auto (a controller named \"Shifter\"), wheel (its buttons are on the wheel),\n"
    "# off, or part of its name; then one line per gear: wheel_gear = <button number>, <1..6 or r>. WheelSetup.exe\n"
    "# finds the numbers. The game uses the shifter with a manual gearbox chosen in its settings.\n"
    "wheel_shifter = auto\n"
    "wheel_ffb = on\n"
    "wheel_ffb_gain = 100\n"
    "wheel_ffb_vibration = 100\n"
    "wheel_ffb_invert = false\n";

/* A default.ini from before wheel support keeps the player's lines and gets the wheel section added at its end. */
static void wheel_config_append(const char *path) {
    FILE *f=fopen(path,"rb");
    if (!f) return;
    char *text=calloc(1,65536); const size_t n=text ? fread(text,1,65535,f) : 0;
    fclose(f);
    const bool present=!text || strstr(text,"wheel_device")!=NULL;
    const bool newline=n && text[n-1]=='\n';
    free(text);
    if (present || !(f=fopen(path,"ab"))) return;
    if (!newline) fputc('\n',f);
    fwrite(wheel_default_config,1,sizeof(wheel_default_config)-1,f);
    fclose(f);
    printf("Runtime: steering wheel settings added to %s\n",path);
}
static bool pedals_separate(void) {
    char want[64]; snprintf(want,sizeof want,"%s",pedals_device); lower(want); trim(want);
    return *want && strcmp(want,"wheel") && strcmp(want,"auto");
}
enum { HB_PEDALS, HB_WHEEL, HB_OWN };
static int handbrake_where(void) {
    char want[64]; snprintf(want,sizeof want,"%s",handbrake_device); lower(want); trim(want);
    return !*want || !strcmp(want,"pedals") || !strcmp(want,"auto") ? HB_PEDALS : !strcmp(want,"wheel") ? HB_WHEEL : HB_OWN;
}
static bool handbrake_name_matches(const char *name) {
    if (handbrake_where()!=HB_OWN) return false;
    char n[128]; snprintf(n,sizeof n,"%s",name ? name : ""); lower(n);
    char want[64]; snprintf(want,sizeof want,"%s",handbrake_device); lower(want); trim(want);
    return strstr(n,want)!=NULL;
}
static bool pedals_name_matches(const char *name) {
    if (!pedals_separate()) return false;
    char n[128]; snprintf(n,sizeof n,"%s",name ? name : ""); lower(n);
    char want[64]; snprintf(want,sizeof want,"%s",pedals_device); lower(want); trim(want);
    return strstr(n,want)!=NULL;
}
/* wheel_device in lower case without its " @<which>" (not '#': that starts a comment in default.ini), for wheels that are several controllers of one name (a Fanatec
 * base is two "FANATEC Wheel"): which is part of the controller's device path (col01: the HID collection, which stays
 * the same from start to start, unlike the order Windows lists them in) or a number, the n-th of that name. */
static void device_want(const char *setting, char *want, size_t size, char *which, size_t which_size) {
    snprintf(want,size,"%s",setting); lower(want); trim(want);
    which[0]=0;
    char *at=strrchr(want,'@');
    if (!at || at==want || at[-1]!=' ' || !at[1]) return;
    snprintf(which,which_size,"%s",at+1); trim(which);
    *at=0; trim(want);
}
static void wheel_device_want(char *want, size_t size, char *which, size_t which_size) {
    device_want(wheel_device,want,size,which,which_size);
}
static bool wheel_which_matches(SDL_JoystickID id, const char *which, int *matched) {
    if (!*which) return true;
    if (atoi(which)>0) return ++*matched==atoi(which);
    char path[512]; snprintf(path,sizeof path,"%s",SDL_GetJoystickPathForID(id) ? SDL_GetJoystickPathForID(id) : ""); lower(path);
    return strstr(path,which)!=NULL;
}
static bool wheel_name_matches(const char *name) {
    char n[128]; snprintf(n,sizeof n,"%s",name ? name : ""); lower(n);
    char want[64], which[32]; wheel_device_want(want,sizeof want,which,sizeof which);
    if (strcmp(want,"auto")) return *want && strstr(n,want)!=NULL;
    if (strstr(n,"pedal")) return false;   /* "FANATEC ClubSport Pedals" and the like are no wheel */
    static const char *const words[]={"wheel","moza","racing","fanatec","thrustmaster","simagic","simucube","cammus",
                                      "logitech g2","logitech g9","g29","g920","g923","t300","t248","t150","t500","tx racing"};
    for (size_t i=0;i<sizeof(words)/sizeof(*words);++i) if (strstr(n,words[i])) return true;
    return false;
}
static int axis_raw(SDL_Joystick *j, int axis) {
    return axis>=0 && axis<SDL_GetNumJoystickAxes(j) ? SDL_GetJoystickAxis(j,axis) : -32768;
}
/* Takes the rest value of axis a of controller j, just opened with the pedals at rest, and decides whether it reads
 * backwards. */
static void wheel_take_rest(SDL_Joystick *j, int a, const char *what) {
    wheel_rest_raw[a]=axis_raw(j,wheel_axis[a].axis);
    wheel_touched[a]=false;
    /* A pedal at rest sits at one end of its axis; at the top end it reads backwards. An axis that has sent nothing
     * yet reads 0: wheel_value decides when it first moves. */
    wheel_inverted[a]=wheel_axis[a].invert==WINV_YES ||
                      (wheel_axis[a].invert==WINV_AUTO && wheel_axis[a].axis>=0 && wheel_rest_raw[a]>16384);
    if (wheel_axis[a].axis>=0)
        printf("Runtime: %s %s = axis %d%s\n",what,wheel_axis_names[a],wheel_axis[a].axis,wheel_inverted[a] ? ", inverted" : "");
}
/* The connected wheel, opened on first use and after a reconnect; called under lock. */
static SDL_Joystick *current_wheel(void) {
    if (sdl_ready<=0) return NULL;
    { char d[8]; snprintf(d,sizeof d,"%s",wheel_device); lower(d); trim(d); if (!strcmp(d,"off") || !strcmp(d,"none")) return NULL; }
    if (wheel && !SDL_JoystickConnected(wheel)) {
        printf("Runtime: wheel disconnected\n");
        SDL_CloseJoystick(wheel); wheel=NULL; wheel_id=0;
    }
    if (wheel) return wheel;
    static uint64_t last_scan;
    const uint64_t now=now_us();
    if (last_scan && now-last_scan<1000000) return NULL; /* look for a newly plugged wheel once a second */
    last_scan=now;
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    char want[64], which[32]; wheel_device_want(want,sizeof want,which,sizeof which);
    int matched=0;
    for (int i=0;ids && i<count && !wheel;++i) {
        const char *name=SDL_GetJoystickNameForID(ids[i]);
        const bool by_type=!SDL_strcasecmp(wheel_device,"auto") && SDL_GetJoystickTypeForID(ids[i])==SDL_JOYSTICK_TYPE_WHEEL;
        if (!by_type && !wheel_name_matches(name)) continue;
        if (pedals_name_matches(name) || handbrake_name_matches(name)) continue;   /* separate pedals or handbrake of the wheel's brand are not the wheel */
        if (!wheel_which_matches(ids[i],which,&matched)) continue;
        if (!(wheel=SDL_OpenJoystick(ids[i]))) continue;
        wheel_id=ids[i];
        SDL_UpdateJoysticks();
        printf("Runtime: wheel connected: %s (%d axes, %d buttons, %d hats), %d degrees, reported to the game as a PS4 steering wheel\n",
               name ? name : "?",SDL_GetNumJoystickAxes(wheel),SDL_GetNumJoystickButtons(wheel),SDL_GetNumJoystickHats(wheel),wheel_rotation);
        printf("Runtime: wheel path %s\n",SDL_GetJoystickPathForID(ids[i]) ? SDL_GetJoystickPathForID(ids[i]) : "?");
        printf("Runtime: wheel axes at rest:");
        for (int a=0;a<SDL_GetNumJoystickAxes(wheel);++a) printf(" %d=%d",a,SDL_GetJoystickAxis(wheel,a));
        printf("\n");
        /* With pedals of their own only the steering is the wheel's; current_pedals takes the pedals' rest. */
        for (int a=0;a<(pedals_separate() ? 1 : WAX_COUNT);++a) if (a!=WAX_HANDBRAKE) wheel_take_rest(wheel,a,"wheel");
        if (handbrake_where()==HB_WHEEL || (handbrake_where()==HB_PEDALS && !pedals_separate())) wheel_take_rest(wheel,WAX_HANDBRAKE,"wheel");
    }
    if (!wheel && count>0 && ids) {
        static bool listed;
        if (!listed) {
            listed=true;
            printf("Runtime: no wheel among the game controllers:");
            for (int i=0;i<count;++i) printf(" \"%s\"",SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "?");
            printf(" (wheel_device in input_config\\default.ini picks one by name)\n");
        }
    }
    SDL_free(ids);
    return wheel;
}
/* The shifter's controller, looked for once a second while the wheel is connected; called under lock. NULL also
 * when the gears are buttons of the wheel (wheel_shifter = wheel). */
static SDL_Joystick *current_shifter(void) {
    char want[64]; snprintf(want,sizeof want,"%s",shifter_device); lower(want); trim(want);
    if (!wheel || !shifter_gear_count || !strcmp(want,"off") || !strcmp(want,"none") || !strcmp(want,"wheel")) return NULL;
    if (shifter && !SDL_JoystickConnected(shifter)) { printf("Runtime: shifter disconnected\n"); SDL_CloseJoystick(shifter); shifter=NULL; }
    if (shifter) return shifter;
    static uint64_t last_scan;
    const uint64_t now=now_us();
    if (last_scan && now-last_scan<1000000) return NULL;
    last_scan=now;
    if (!strcmp(want,"auto")) snprintf(want,sizeof want,"shifter");
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;ids && i<count && !shifter;++i) {
        if (ids[i]==wheel_id) continue;
        char n[128]; snprintf(n,sizeof n,"%s",SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : ""); lower(n);
        if (!strstr(n,want) || !(shifter=SDL_OpenJoystick(ids[i]))) continue;
        printf("Runtime: shifter connected: %s (%d buttons), %d gears set\n",SDL_GetJoystickName(shifter),SDL_GetNumJoystickButtons(shifter),shifter_gear_count);
    }
    SDL_free(ids);
    return shifter;
}
/* The controller the pedal axes are read from: the wheel, or the pedals' own controller, looked for once a second
 * while the wheel is connected (NULL until it is found: the pedals read released); called under lock. */
static SDL_Joystick *current_pedals(void) {
    if (!pedals_separate()) return wheel;
    if (!wheel) return NULL;
    if (pedals && !SDL_JoystickConnected(pedals)) { printf("Runtime: pedals disconnected\n"); SDL_CloseJoystick(pedals); pedals=NULL; pedals_id=0; }
    if (pedals) return pedals;
    static uint64_t last_scan;
    const uint64_t now=now_us();
    if (last_scan && now-last_scan<1000000) return NULL;
    last_scan=now;
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;ids && i<count && !pedals;++i) {
        if (ids[i]==wheel_id || (shifter && ids[i]==SDL_GetJoystickID(shifter))) continue;
        const char *name=SDL_GetJoystickNameForID(ids[i]);
        if (!pedals_name_matches(name) || !(pedals=SDL_OpenJoystick(ids[i]))) continue;
        pedals_id=ids[i];
        SDL_UpdateJoysticks();
        printf("Runtime: pedals connected: %s (%d axes)\n",name ? name : "?",SDL_GetNumJoystickAxes(pedals));
        printf("Runtime: pedal axes at rest:");
        for (int a=0;a<SDL_GetNumJoystickAxes(pedals);++a) printf(" %d=%d",a,SDL_GetJoystickAxis(pedals,a));
        printf("\n");
        for (int a=WAX_THROTTLE;a<WAX_COUNT;++a) if (a!=WAX_HANDBRAKE || handbrake_where()==HB_PEDALS) wheel_take_rest(pedals,a,"pedals");
    }
    if (!pedals) {
        static bool listed;
        if (!listed) {
            listed=true;
            printf("Runtime: no pedals named \"%s\" among the game controllers:",pedals_device);
            for (int i=0;ids && i<count;++i) printf(" \"%s\"",SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "?");
            printf(" (wheel_pedals in input_config\\default.ini picks one by name)\n");
        }
    }
    SDL_free(ids);
    return pedals;
}
/* The handbrake's controller (wheel_handbrake_device), looked for once a second while the wheel is connected when it
 * has one of its own; called under lock. */
static SDL_Joystick *current_handbrake(void) {
    const int where=handbrake_where();
    if (where==HB_WHEEL) return wheel;
    if (where==HB_PEDALS) return current_pedals();
    if (!wheel) return NULL;
    if (handbrake && !SDL_JoystickConnected(handbrake)) { printf("Runtime: handbrake disconnected\n"); SDL_CloseJoystick(handbrake); handbrake=NULL; handbrake_id=0; }
    if (handbrake) return handbrake;
    static uint64_t last_scan;
    const uint64_t now=now_us();
    if (last_scan && now-last_scan<1000000) return NULL;
    last_scan=now;
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;ids && i<count && !handbrake;++i) {
        if (ids[i]==wheel_id) continue;
        const char *name=SDL_GetJoystickNameForID(ids[i]);
        if (!handbrake_name_matches(name) || !(handbrake=SDL_OpenJoystick(ids[i]))) continue;
        handbrake_id=ids[i];
        SDL_UpdateJoysticks();
        printf("Runtime: handbrake connected: %s (%d axes, %d buttons), %s\n",name ? name : "?",SDL_GetNumJoystickAxes(handbrake),
               SDL_GetNumJoystickButtons(handbrake),handbrake_button>=0 ? "a button" : "an axis");
        wheel_take_rest(handbrake,WAX_HANDBRAKE,"handbrake");
    }
    if (!handbrake) {
        static bool listed;
        if (!listed) {
            listed=true;
            printf("Runtime: no handbrake named \"%s\" among the game controllers:",handbrake_device);
            for (int i=0;ids && i<count;++i) printf(" \"%s\"",SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "?");
            printf(" (wheel_handbrake_device in input_config\\default.ini picks one by name)\n");
        }
    }
    SDL_free(ids);
    return handbrake;
}
/* The controller of wheel_button lines that name one (button_devs slot k), looked for once a second while the wheel
 * is connected; slot 0: the wheel. Called under lock. */
static SDL_Joystick *button_device(int k) {
    if (k<=0 || k>button_dev_count) return wheel;
    if (!wheel) return NULL;
    ButtonDevice *d=&button_devs[k];
    if (d->j && !SDL_JoystickConnected(d->j)) { printf("Runtime: button controller \"%s\" disconnected\n",d->name); SDL_CloseJoystick(d->j); d->j=NULL; }
    if (d->j) return d->j;
    const uint64_t now=now_us();
    if (d->last_scan && now-d->last_scan<1000000) return NULL;
    d->last_scan=now;
    char want[64], which[32]; device_want(d->name,want,sizeof want,which,sizeof which);
    int count=0, matched=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;ids && i<count && !d->j;++i) {
        if (ids[i]==wheel_id) continue;   /* the wheel's own buttons are lines without a controller name */
        char n[128]; snprintf(n,sizeof n,"%s",SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : ""); lower(n);
        if (!*want || !strstr(n,want) || !wheel_which_matches(ids[i],which,&matched) || !(d->j=SDL_OpenJoystick(ids[i]))) continue;
        printf("Runtime: button controller connected: %s (%d buttons) for the wheel_button lines naming \"%s\"\n",
               SDL_GetJoystickName(d->j),SDL_GetNumJoystickButtons(d->j),d->name);
    }
    if (!d->j && !d->listed) {
        d->listed=true;
        printf("Runtime: no controller named \"%s\" (wheel_button) among the game controllers:",d->name);
        for (int i=0;ids && i<count;++i) printf(" \"%s\"",SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "?");
        printf("\n");
    }
    SDL_free(ids);
    return d->j;
}
/* A wheel_button binding's button: on the wheel, or on the controller its line names. Under lock. */
static bool wheel_button_down(int dev, int index) {
    SDL_Joystick *j=button_device(dev);
    return j && index>=0 && index<SDL_GetNumJoystickButtons(j) && SDL_GetJoystickButton(j,index);
}
static uint8_t shifter_gear(SDL_Joystick *w) {
    SDL_Joystick *s=current_shifter();
    char want[16]; snprintf(want,sizeof want,"%s",shifter_device); lower(want); trim(want);
    if (!s && !strcmp(want,"wheel")) s=w;
    if (!s) return 0;
    for (int i=0;i<shifter_gear_count;++i) if (SDL_GetJoystickButton(s,shifter_gears[i].index)) return shifter_gears[i].gear_bits;
    return 0;
}
static uint16_t wheel_value(SDL_Joystick *j, int a) {
    if (wheel_axis[a].axis<0) return a==WAX_STEER ? 32768 : 0;
    const int raw=axis_raw(j,wheel_axis[a].axis);
    if (a!=WAX_STEER && !wheel_touched[a]) {
        if (raw==wheel_rest_raw[a]) return 0;
        wheel_touched[a]=true;
        /* Silent until now: its first value is near its rest, which tells which way it reads. */
        if (wheel_axis[a].invert==WINV_AUTO && !wheel_rest_raw[a]) {
            wheel_inverted[a]=raw>0;
            printf("Runtime: %s first moved from %d%s\n",wheel_axis_names[a],raw,wheel_inverted[a] ? ", inverted" : "");
        }
    }
    int v=raw+32768;   /* 0..65535 */
    if (wheel_inverted[a]) v=65535-v;
    return (uint16_t)(v<0 ? 0 : v>65535 ? 65535 : v);
}
/* Adds the wheel to pad data of the wheel port: its buttons, and its axes in the extension bytes. Under lock. */
static void wheel_sample(PadData *d, bool neutral) {
    SDL_Joystick *j=current_wheel();
    if (!j) return;
    WheelExtension w={32768,0,0,0,0,0,WHEEL_MAGIC};
    if (!neutral) {
        w.steer=wheel_value(j,WAX_STEER);
        SDL_Joystick *p=current_pedals();
        if (p) {
            w.throttle=wheel_value(p,WAX_THROTTLE); w.brake=wheel_value(p,WAX_BRAKE);
            w.clutch=wheel_value(p,WAX_CLUTCH);
        }
        SDL_Joystick *hb=current_handbrake();
        if (hb) w.handbrake=handbrake_button>=0 ? (SDL_GetJoystickButton(hb,handbrake_button) ? 65535 : 0) : wheel_value(hb,WAX_HANDBRAKE);
        /* The game reads the wheel through logiWheel, which has no handbrake: a pulled handbrake is the pad's circle,
         * the handbrake in a race (on from 40% pulled, off again under 25%). */
        if (wheel_axis[WAX_HANDBRAKE].axis>=0 || handbrake_button>=0) {
            static bool pulled;
            pulled=w.handbrake>(pulled ? 16384 : 26214);
            if (pulled) d->buttons|=BTN_CIRCLE;
        }
        if (p && p!=j) {
            /* For the wheel_* settings: which axis numbers of the pedals' controller the player touches. */
            static int last_paxis[16]; static bool have_plast; static unsigned plogged;
            if (plogged<100) {
                for (int a=0;a<SDL_GetNumJoystickAxes(p) && a<16;++a) {
                    const int v=SDL_GetJoystickAxis(p,a);
                    if (!have_plast) last_paxis[a]=v;
                    else if (abs(v-last_paxis[a])>8000) { printf("Runtime: pedals axis %d = %d\n",a,v); last_paxis[a]=v; ++plogged; }
                }
                have_plast=true;
            }
        }
        w.gear=shifter_gear(j);
        { static uint8_t last_gear; if (w.gear!=last_gear) { static unsigned n; if (n++<60) printf("Runtime: shifter gear %s\n",w.gear==0x80 ? "R" : !w.gear ? "N" : w.gear==1 ? "1" : w.gear==2 ? "2" : w.gear==4 ? "3" : w.gear==8 ? "4" : w.gear==16 ? "5" : "6"); last_gear=w.gear; } }
        for (int i=0;i<wheel_button_count;++i) {
            if (!wheel_button_down(wheel_buttons[i].dev,wheel_buttons[i].index)) continue;
            if (wheel_buttons[i].out_kind==OUT_BUTTON) d->buttons|=wheel_buttons[i].button;
            else touch_click(d,wheel_buttons[i].touch_side==2);
        }
        /* For the wheel_button settings: which buttons of the other controllers the player presses. */
        for (int k=1;k<=button_dev_count;++k) {
            static unsigned logged_other;
            SDL_Joystick *b=button_device(k);
            if (!b || logged_other>=300) continue;
            uint64_t now=0;
            for (int i=0;i<SDL_GetNumJoystickButtons(b) && i<64;++i) if (SDL_GetJoystickButton(b,i)) now|=UINT64_C(1)<<i;
            for (int i=0;i<64;++i) if ((now & ~button_devs[k].last_buttons)>>i & 1) { printf("Runtime: \"%s\" button %d\n",button_devs[k].name,i); ++logged_other; }
            button_devs[k].last_buttons=now;
        }
        if (wheel_hat_dpad && SDL_GetNumJoystickHats(j)>0) {
            const uint8_t hat=SDL_GetJoystickHat(j,0);
            if (hat & SDL_HAT_UP) d->buttons|=BTN_UP;
            if (hat & SDL_HAT_DOWN) d->buttons|=BTN_DOWN;
            if (hat & SDL_HAT_LEFT) d->buttons|=BTN_LEFT;
            if (hat & SDL_HAT_RIGHT) d->buttons|=BTN_RIGHT;
        }
        if (wheel_as_stick) {
            const uint8_t x=(uint8_t)(w.steer>>8), l2=(uint8_t)(w.brake>>8), r2=(uint8_t)(w.throttle>>8);
            if (abs(x-128)>abs((int)d->left_x-128)) d->left_x=x;
            if (l2>d->l2) d->l2=l2;
            if (r2>d->r2) d->r2=r2;
            if (d->l2>30) d->buttons|=BTN_L2;
            if (d->r2>30) d->buttons|=BTN_R2;
        }
        /* For the wheel_* settings: which axis and button numbers the player touches. */
        static int last_axis[16]; static bool have_last; static unsigned logged; static uint64_t last_buttons;
        const int naxes=SDL_GetNumJoystickAxes(j), nbuttons=SDL_GetNumJoystickButtons(j);
        if (logged<300) {
            for (int a=0;a<naxes && a<16;++a) {
                const int v=SDL_GetJoystickAxis(j,a);
                if (!have_last) last_axis[a]=v;
                else if (abs(v-last_axis[a])>8000) { printf("Runtime: wheel axis %d = %d\n",a,v); last_axis[a]=v; ++logged; }
            }
            have_last=true;
            uint64_t b=0;
            for (int i=0;i<nbuttons && i<64;++i) if (SDL_GetJoystickButton(j,i)) b|=UINT64_C(1)<<i;
            for (int i=0;i<64;++i) if ((b & ~last_buttons)>>i & 1) { printf("Runtime: wheel button %d\n",i); ++logged; }
            last_buttons=b;
        }
    }
    memcpy(d->extension,&w,sizeof w);
}

static ABI int32_t pad_device_class_extended_info(int32_t handle, DeviceClassExtendedInfo *info) { PADLOG("scePadDeviceClassGetExtendedInformation");
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    pthread_mutex_lock(&lock);
    const bool is_wheel=is_wheel_port(handle) && current_wheel();
    pthread_mutex_unlock(&lock);
    info->device_class=is_wheel ? DEVICE_CLASS_STEERING_WHEEL : DEVICE_CLASS_STANDARD;
    if (is_wheel) info->max_physical_wheel_angle=(uint16_t)wheel_rotation;
    return 0;
}
static ABI int32_t pad_device_class_parse(int32_t handle, const PadData *data, DeviceClassData *out) {
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    if (!data || !out) return ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    WheelExtension w;
    memcpy(&w,data->extension,sizeof w);
    if (!is_wheel_port(handle) || w.magic!=WHEEL_MAGIC) { out->device_class=DEVICE_CLASS_STANDARD; return 0; }
    out->device_class=DEVICE_CLASS_STEERING_WHEEL; out->data_valid=1;
    out->steering_wheel_angle=((float)w.steer-32768.0f)/32767.0f*(float)wheel_rotation*0.5f;
    out->steering_wheel=w.steer; out->accelerator_pedal=w.throttle; out->brake_pedal=w.brake;
    out->clutch_pedal=w.clutch; out->hand_brake=w.handbrake; out->gear=w.gear;
    return 0;
}
