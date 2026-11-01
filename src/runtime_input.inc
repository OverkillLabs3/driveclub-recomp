/* Keyboard, mouse and controller bindings read from <user>/input_config/default.ini, in the same format and
 * with the same default bindings as shadPS4:
 *   <output> = <key>[, <key>, <key>]     keyboard or mouse input (all keys of a line held together)
 *   <output> = <controller input>        controller remap (cross = cross)
 *   analog_deadzone = leftjoystick|rightjoystick|l2|r2, inner, outer
 *   override_controller_color = true|false, r, g, b
 *   mouse_to_joystick = none|left|right
 *   mouse_movement_params = deadzone offset, speed, speed offset
 * Included by runtime_pad.c. */

enum { IN_KEY, IN_MOUSE };
enum { OUT_BUTTON, OUT_AXIS, OUT_TOUCH };
typedef struct {
    uint8_t in_count;
    struct { uint8_t kind; int code; } in[3];
    uint8_t out_kind;
    uint32_t button;     /* OUT_BUTTON */
    int axis, sign;      /* OUT_AXIS: 0 lx, 1 ly, 2 rx, 3 ry; sign -1 or +1 */
    int touch_side;      /* OUT_TOUCH: 0 left, 1 center, 2 right */
} Binding;
#define MAX_BINDINGS 128
static Binding bindings[MAX_BINDINGS];
static int binding_count;
static int pad_remap[32][2];          /* controller section: {output button, SDL button} */
static int pad_remap_count;
static float kb_steer_time[2]={0.18f,0.10f}, kb_pedal_time[2]={0.10f,0.06f};  /* seconds to full and back */
static int dz_inner[4]={5,5,0,0}, dz_outer[4]={127,127,127,127};   /* left stick, right stick, l2, r2 */
static bool color_override; static uint8_t color_rgb[3]={0,0,255};
static int mouse_target;               /* 0 none, 1 left stick, 2 right stick */
static float mouse_dz_offset=0.5f, mouse_speed=1.0f, mouse_speed_offset=0.125f;
static int input_loaded;

static const struct { const char *name; uint32_t ps; } button_names[]={
    {"cross",BTN_CROSS},{"circle",BTN_CIRCLE},{"square",BTN_SQUARE},{"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1},{"r1",BTN_R1},{"l2",BTN_L2},{"r2",BTN_R2},{"l3",BTN_L3},{"r3",BTN_R3},
    {"options",BTN_OPTIONS},{"pad_up",BTN_UP},{"pad_down",BTN_DOWN},{"pad_left",BTN_LEFT},{"pad_right",BTN_RIGHT},
};
static const struct { const char *name; int sdl; } sdl_button_names[]={
    {"cross",SDL_GAMEPAD_BUTTON_SOUTH},{"circle",SDL_GAMEPAD_BUTTON_EAST},{"square",SDL_GAMEPAD_BUTTON_WEST},
    {"triangle",SDL_GAMEPAD_BUTTON_NORTH},{"l1",SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},{"r1",SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"l3",SDL_GAMEPAD_BUTTON_LEFT_STICK},{"r3",SDL_GAMEPAD_BUTTON_RIGHT_STICK},{"options",SDL_GAMEPAD_BUTTON_START},
    {"pad_up",SDL_GAMEPAD_BUTTON_DPAD_UP},{"pad_down",SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"pad_left",SDL_GAMEPAD_BUTTON_DPAD_LEFT},{"pad_right",SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"back",SDL_GAMEPAD_BUTTON_BACK},{"touchpad_center",SDL_GAMEPAD_BUTTON_TOUCHPAD},
};
static const struct { const char *name; SDL_Scancode sc; } key_names[]={
    {"up",SDL_SCANCODE_UP},{"down",SDL_SCANCODE_DOWN},{"left",SDL_SCANCODE_LEFT},{"right",SDL_SCANCODE_RIGHT},
    {"enter",SDL_SCANCODE_RETURN},{"return",SDL_SCANCODE_RETURN},{"space",SDL_SCANCODE_SPACE},{"tab",SDL_SCANCODE_TAB},
    {"backspace",SDL_SCANCODE_BACKSPACE},{"escape",SDL_SCANCODE_ESCAPE},{"esc",SDL_SCANCODE_ESCAPE},
    {"lshift",SDL_SCANCODE_LSHIFT},{"rshift",SDL_SCANCODE_RSHIFT},{"lctrl",SDL_SCANCODE_LCTRL},{"rctrl",SDL_SCANCODE_RCTRL},
    {"lalt",SDL_SCANCODE_LALT},{"ralt",SDL_SCANCODE_RALT},{"capslock",SDL_SCANCODE_CAPSLOCK},
    {"comma",SDL_SCANCODE_COMMA},{"period",SDL_SCANCODE_PERIOD},{"minus",SDL_SCANCODE_MINUS},{"equals",SDL_SCANCODE_EQUALS},
    {"semicolon",SDL_SCANCODE_SEMICOLON},{"apostrophe",SDL_SCANCODE_APOSTROPHE},{"slash",SDL_SCANCODE_SLASH},
    {"backslash",SDL_SCANCODE_BACKSLASH},{"leftbracket",SDL_SCANCODE_LEFTBRACKET},{"rightbracket",SDL_SCANCODE_RIGHTBRACKET},
    {"grave",SDL_SCANCODE_GRAVE},{"insert",SDL_SCANCODE_INSERT},{"delete",SDL_SCANCODE_DELETE},{"home",SDL_SCANCODE_HOME},
    {"end",SDL_SCANCODE_END},{"pageup",SDL_SCANCODE_PAGEUP},{"pagedown",SDL_SCANCODE_PAGEDOWN},
    {"kpenter",SDL_SCANCODE_KP_ENTER},{"kpplus",SDL_SCANCODE_KP_PLUS},{"kpminus",SDL_SCANCODE_KP_MINUS},
    {"kpmultiply",SDL_SCANCODE_KP_MULTIPLY},{"kpdivide",SDL_SCANCODE_KP_DIVIDE},{"kpperiod",SDL_SCANCODE_KP_PERIOD},
};

static void trim(char *s) {
    size_t n=strlen(s);
    while (n && (s[n-1]==' ' || s[n-1]=='\t' || s[n-1]=='\r' || s[n-1]=='\n')) s[--n]=0;
    char *p=s; while (*p==' ' || *p=='\t') ++p;
    if (p!=s) memmove(s,p,strlen(p)+1);
}
static void lower(char *s) { for (; *s; ++s) if (*s>='A' && *s<='Z') *s+=32; }
static bool parse_input(const char *name, uint8_t *kind, int *code) {
    char n[32]; snprintf(n,sizeof n,"%s",name); lower(n);
    if (!strcmp(n,"lclick")) { *kind=IN_MOUSE; *code=SDL_BUTTON_LEFT; return true; }
    if (!strcmp(n,"rclick")) { *kind=IN_MOUSE; *code=SDL_BUTTON_RIGHT; return true; }
    if (!strcmp(n,"mclick")) { *kind=IN_MOUSE; *code=SDL_BUTTON_MIDDLE; return true; }
    if (!strcmp(n,"mouse4") || !strcmp(n,"x1click")) { *kind=IN_MOUSE; *code=SDL_BUTTON_X1; return true; }
    if (!strcmp(n,"mouse5") || !strcmp(n,"x2click")) { *kind=IN_MOUSE; *code=SDL_BUTTON_X2; return true; }
    *kind=IN_KEY;
    if (n[0] && !n[1]) {
        if (n[0]>='a' && n[0]<='z') { *code=SDL_SCANCODE_A+(n[0]-'a'); return true; }
        if (n[0]>='1' && n[0]<='9') { *code=SDL_SCANCODE_1+(n[0]-'1'); return true; }
        if (n[0]=='0') { *code=SDL_SCANCODE_0; return true; }
    }
    if (n[0]=='f' && n[1]>='1' && n[1]<='9' && (!n[2] || (n[2]>='0' && n[2]<='2' && !n[3]))) {
        int f=atoi(n+1); if (f>=1 && f<=12) { *code=SDL_SCANCODE_F1+(f-1); return true; }
    }
    if (n[0]=='k' && n[1]=='p' && n[2]>='0' && n[2]<='9' && !n[3]) {
        *code=n[2]=='0' ? SDL_SCANCODE_KP_0 : SDL_SCANCODE_KP_1+(n[2]-'1'); return true;
    }
    for (size_t i=0;i<sizeof(key_names)/sizeof(*key_names);++i) if (!strcmp(n,key_names[i].name)) { *code=key_names[i].sc; return true; }
    SDL_Scancode sc=SDL_GetScancodeFromName(name);
    if (sc!=SDL_SCANCODE_UNKNOWN) { *code=sc; return true; }
    return false;
}
static void add_binding(const Binding *b) { if (binding_count<MAX_BINDINGS) bindings[binding_count++]=*b; }
/* ffb_gain_up / ffb_gain_down = <key>[, <key>, <key>]: keys (a Stream Deck's too) for the per-car force feedback gain
 * (ffb_gain_buttons in runtime_logiwheel.inc); several lines each. */
static Binding ffb_gain_keys[2][4];
static int ffb_gain_key_count[2];
static void wheel_parse(const char *lhs, char *rhs);   /* runtime_wheel.inc */
static void wheel_config_append(const char *path);

#define LAYOUT_MARKER "# DriveClub racing layout 4"
static const char default_config[]=
    LAYOUT_MARKER "\n"
    "# Keyboard layout in the style of PC racing games (Forza Horizon, Need for Speed).\n"
    "# Edit freely: a line with the layout marker above is kept as it is.\n\n"
    "# Keyboard bindings\n\n"
    "# Driving\n"
    "r2 = w\nl2 = s\n"
    "axis_left_x_minus = a\naxis_left_x_plus = d\n"
    "circle = space\n"
    "l1 = q\nr1 = e\n\n"
    "# Menus and cameras\n"
    "cross = enter\ncross = z\ncircle = backspace\n"
    "triangle = c\nsquare = x\n"
    "l3 = lctrl\nr3 = v\n"
    "options = escape\noptions = p\n"
    "# DriveClub splits the touchpad into a left and a right half, each its own press (reset to track and photo mode)\n"
    "touchpad_left = r\ntouchpad_right = t\n\n"
    "pad_up = up\npad_down = down\npad_left = left\npad_right = right\n\n"
    "axis_right_x_minus = j\naxis_right_x_plus = l\naxis_right_y_minus = i\naxis_right_y_plus = k\n\n"
    "# Mouse\n# mouse_to_joystick: none, left or right. The mouse then moves that stick (turns the camera).\n"
    "mouse_to_joystick = right\nmouse_movement_params = 0.5, 1, 0.125\n"
    "# Mouse buttons can be bound like keys, for example: cross = lclick\n\n"
    "# Smooth keyboard steering and pedals: seconds to reach full steering or pedal, and to return.\n"
    "# 0 gives the instant, all-or-nothing response of a plain button.\n"
    "keyboard_steer_time = 0.18, 0.10\n"
    "keyboard_pedal_time = 0.10, 0.06\n\n"
    "# Controller bindings\n\n"
    "triangle = triangle\ncross = cross\nsquare = square\ncircle = circle\n\n"
    "l1 = l1\nl2 = l2\nl3 = l3\nr1 = r1\nr2 = r2\nr3 = r3\n\n"
    "options = options\ntouchpad_center = back\n\n"
    "pad_up = pad_up\npad_down = pad_down\npad_left = pad_left\npad_right = pad_right\n\n"
    "axis_left_x = axis_left_x\naxis_left_y = axis_left_y\naxis_right_x = axis_right_x\naxis_right_y = axis_right_y\n\n"
    "# Range of deadzones: 1 (almost none) to 127 (max)\n"
    "analog_deadzone = leftjoystick, 5, 127\nanalog_deadzone = rightjoystick, 5, 127\n\n"
    "override_controller_color = false, 0, 0, 255\n";

static void parse_line(char *line) {
    char *hash=strchr(line,'#'); if (hash) *hash=0;
    trim(line);
    if (!*line) return;
    char *eq=strchr(line,'=');
    if (!eq) return;
    *eq=0;
    char *lhs=line, *rhs=eq+1;
    trim(lhs); trim(rhs); lower(lhs);
    if (!strcmp(lhs,"analog_deadzone")) {
        char dev[32]; int in=0, out=127;
        if (sscanf(rhs," %31[^, ] , %d , %d",dev,&in,&out)>=3) {
            lower(dev);
            int i=!strcmp(dev,"leftjoystick") ? 0 : !strcmp(dev,"rightjoystick") ? 1 : !strcmp(dev,"l2") ? 2 : !strcmp(dev,"r2") ? 3 : -1;
            if (i>=0) { dz_inner[i]=in<0 ? 0 : in>126 ? 126 : in; dz_outer[i]=out<=in ? in+1 : out>127 ? 127 : out; }
        }
        return;
    }
    if (!strcmp(lhs,"override_controller_color")) {
        char b[8]; int r=0,g=0,bl=255;
        if (sscanf(rhs," %7[^, ] , %d , %d , %d",b,&r,&g,&bl)>=1) {
            lower(b); color_override=!strcmp(b,"true"); color_rgb[0]=(uint8_t)r; color_rgb[1]=(uint8_t)g; color_rgb[2]=(uint8_t)bl;
        }
        return;
    }
    if (!strcmp(lhs,"keyboard_steer_time") || !strcmp(lhs,"keyboard_pedal_time")) {
        float up=0, down=0;
        if (sscanf(rhs," %f , %f",&up,&down)>=1) {
            if (down<=0 && strchr(rhs,',')==NULL) down=up;
            float *dst=!strcmp(lhs,"keyboard_steer_time") ? kb_steer_time : kb_pedal_time;
            dst[0]=up<0 ? 0 : up>2 ? 2 : up; dst[1]=down<0 ? 0 : down>2 ? 2 : down;
        }
        return;
    }
    if (!strncmp(lhs,"wheel_",6)) { wheel_parse(lhs,rhs); return; }
    if (!strcmp(lhs,"mouse_to_joystick")) { lower(rhs); mouse_target=!strcmp(rhs,"left") ? 1 : !strcmp(rhs,"right") ? 2 : 0; return; }
    if (!strcmp(lhs,"mouse_movement_params")) {
        float a,b,c; if (sscanf(rhs," %f , %f , %f",&a,&b,&c)==3) { mouse_dz_offset=a; mouse_speed=b; mouse_speed_offset=c; }
        return;
    }
    Binding b; memset(&b,0,sizeof b);
    bool out_ok=false;
    for (size_t i=0;i<sizeof(button_names)/sizeof(*button_names) && !out_ok;++i)
        if (!strcmp(lhs,button_names[i].name)) { b.out_kind=OUT_BUTTON; b.button=button_names[i].ps; out_ok=true; }
    static const struct { const char *name; int axis, sign; } axes[]={
        {"axis_left_x_minus",0,-1},{"axis_left_x_plus",0,1},{"axis_left_y_minus",1,-1},{"axis_left_y_plus",1,1},
        {"axis_right_x_minus",2,-1},{"axis_right_x_plus",2,1},{"axis_right_y_minus",3,-1},{"axis_right_y_plus",3,1},
    };
    for (size_t i=0;i<sizeof(axes)/sizeof(*axes) && !out_ok;++i)
        if (!strcmp(lhs,axes[i].name)) { b.out_kind=OUT_AXIS; b.axis=axes[i].axis; b.sign=axes[i].sign; out_ok=true; }
    if (!out_ok && (!strcmp(lhs,"touchpad_center") || !strcmp(lhs,"touchpad_left") || !strcmp(lhs,"touchpad_right"))) {
        b.out_kind=OUT_TOUCH; b.touch_side=!strcmp(lhs,"touchpad_left") ? 0 : !strcmp(lhs,"touchpad_right") ? 2 : 1; out_ok=true;
    }
    const int ffb_gain=!strcmp(lhs,"ffb_gain_up") ? 0 : !strcmp(lhs,"ffb_gain_down") ? 1 : -1;
    if (!out_ok && ffb_gain<0) return;
    /* Controller section: the right hand side is a controller input name. */
    lower(rhs);
    for (size_t i=0;i<sizeof(sdl_button_names)/sizeof(*sdl_button_names);++i) {
        if (!strcmp(rhs,sdl_button_names[i].name) && b.out_kind==OUT_BUTTON && ffb_gain<0) {
            if (pad_remap_count<32) { pad_remap[pad_remap_count][0]=(int)b.button; pad_remap[pad_remap_count][1]=sdl_button_names[i].sdl; ++pad_remap_count; }
            return;
        }
        if (!strcmp(rhs,"back") && b.out_kind==OUT_TOUCH) return;
    }
    if (!strcmp(rhs,"l2") || !strcmp(rhs,"r2") || !strncmp(rhs,"axis_",5)) return;
    char *save=NULL; char *tok=strtok_r(rhs,",",&save);
    while (tok && b.in_count<3) {
        trim(tok);
        uint8_t kind; int code;
        if (parse_input(tok,&kind,&code)) { b.in[b.in_count].kind=kind; b.in[b.in_count].code=code; ++b.in_count; }
        tok=strtok_r(NULL,",",&save);
    }
    if (b.in_count && ffb_gain>=0) { if (ffb_gain_key_count[ffb_gain]<4) ffb_gain_keys[ffb_gain][ffb_gain_key_count[ffb_gain]++]=b; }
    else if (b.in_count) add_binding(&b);
}
static void input_load(void) {
    if (input_loaded) return;
    input_loaded=1;
    char path[600], dir[560];
    const char *user=runtime_file_user_dir();
    snprintf(dir,sizeof dir,"%s/input_config",user && *user ? user : ".");
    snprintf(path,sizeof path,"%s/default.ini",dir);
    FILE *f=fopen(path,"rb");
    if (f) {   /* a file made by an older version (no layout marker) is kept as default.ini.old and replaced */
        char *text=calloc(1,65536); size_t n=text ? fread(text,1,65535,f) : 0; (void)n;
        const bool current=text && strstr(text,LAYOUT_MARKER);
        free(text); fclose(f); f=NULL;
        if (!current) {
            char old[620]; snprintf(old,sizeof old,"%s.old",path);
            remove(old); rename(path,old);
        }
        f=fopen(path,"rb");
    }
    if (!f) {
        compat_mkdir_p(dir);
        if ((f=fopen(path,"wb"))) { fwrite(default_config,1,sizeof(default_config)-1,f); fclose(f); }
        f=fopen(path,"rb");
    }
    if (f) { fclose(f); wheel_config_append(path); f=fopen(path,"rb"); }
    if (f) {
        char line[256];
        while (fgets(line,sizeof line,f)) parse_line(line);
        fclose(f);
        printf("Runtime: input config %s: %d bindings, mouse %s\n",path,binding_count,mouse_target==1 ? "left stick" : mouse_target==2 ? "right stick" : "off");
    }
    if (!binding_count) {   /* unreadable config: the built-in defaults */
        char copy[sizeof default_config]; memcpy(copy,default_config,sizeof copy);
        for (char *p=strtok(copy,"\n"); p; p=strtok(NULL,"\n")) { char line[256]; snprintf(line,sizeof line,"%s",p); parse_line(line); }
    }
}
static bool binding_down(const Binding *b, const bool *keys, SDL_MouseButtonFlags mouse) {
    for (int i=0;i<b->in_count;++i) {
        if (b->in[i].kind==IN_KEY) { if (!keys || !keys[b->in[i].code]) return false; }
        else if (!(mouse & SDL_BUTTON_MASK(b->in[i].code))) return false;
    }
    return b->in_count>0;
}
/* One of the per-car gain's keys is held (0: up, 1: down); the game's window has the keyboard. */
static bool ffb_gain_key_down(int which) {
    if (!ffb_gain_key_count[which]) return false;
    const bool *keys=SDL_GetKeyboardState(NULL);
    for (int i=0;i<ffb_gain_key_count[which];++i) if (binding_down(&ffb_gain_keys[which][i],keys,0)) return true;
    return false;
}
/* A 0..255 stick value (128 centre) through the configured inner and outer dead zones. */
static uint8_t dz_axis(int16_t v, int which) {
    const float x=(float)v/32768.0f*127.5f;      /* -127.5 .. 127.5 */
    const float inner=(float)dz_inner[which], outer=(float)dz_outer[which];
    float m=fabsf(x);
    if (m<=inner) m=0; else { m=(m-inner)/(outer-inner)*127.5f; if (m>127.5f) m=127.5f; }
    const float out=128.0f+(x<0 ? -m : m);
    return (uint8_t)(out<0 ? 0 : out>255 ? 255 : out);
}
