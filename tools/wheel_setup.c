/* WheelSetup: a window to map the steering wheel, the pedals, the wheel's buttons and an H-pattern shifter for
 * DriveClub Recomp. Every control is a row with its current mapping and a live view: click Map (or the row) and move
 * the control, x clears it, Esc cancels. Save writes the wheel_* lines of data\user\input_config\default.ini (see
 * src/runtime_wheel.inc); the force feedback lines and every other setting stay as they are. English, or Russian on
 * a Russian Windows. Windows only, one exe:
 *   clang -O2 -o WheelSetup.exe tools/wheel_setup.c -static $(pkg-config --static --libs sdl3) -lgdi32 -mwindows */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <windows.h>

static bool ru;
#define T(en, ru_text) (ru ? (ru_text) : (en))

static void lower_in_place(char *s) { for (;*s;++s) *s=(char)tolower((unsigned char)*s); }
static char *strip(char *s) {
    while (*s==' ' || *s=='\t') ++s;
    char *e=s+strlen(s);
    while (e>s && (e[-1]==' ' || e[-1]=='\t' || e[-1]=='\r' || e[-1]=='\n')) *--e=0;
    return s;
}

/* ---- controllers --------------------------------------------------------------------------------------------- */
typedef struct {
    SDL_Joystick *j;
    char name[128], lname[128], lpath[512], col[8];   /* col: the HID collection ("col01"), or "" */
    int naxes, nbuttons;
    bool wheel, pedals;
} Dev;
static Dev dev[16];
static int ndev;
/* Many pedals send nothing until they move and read 0 (half pressed) till then: the live view shows an axis as
 * released until it leaves the value it had when the controllers were listed, as the game does. */
static int listed_value[16][64];
static bool axis_moved[16][64];
static int wheel_dev=-1, pedal_dev=-1;   /* pedal_dev -1: the pedals are axes of the wheel */
static int shifter_dev=-1;               /* the controller whose buttons are the gears, -1 unknown */
static int hb_dev=-1;                    /* the handbrake's controller; -1: the pedals' (an axis among theirs) */

static bool looks_like_pedals(const char *lname) { return strstr(lname,"pedal")!=NULL; }
static bool looks_like_wheel(SDL_JoystickID id, const char *lname) {
    if (SDL_GetJoystickTypeForID(id)==SDL_JOYSTICK_TYPE_WHEEL) return true;
    if (looks_like_pedals(lname)) return false;   /* the game's wheel_device = auto passes them over too */
    static const char *const words[]={"wheel","moza","racing","fanatec","thrustmaster","simagic","simucube","cammus",
                                      "logitech g2","logitech g9","g29","g920","g923","t300","t248","t150","t500","tx racing"};
    for (size_t i=0;i<sizeof(words)/sizeof(*words);++i) if (strstr(lname,words[i])) return true;
    return false;
}
static void list_devices(void) {
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;ids && i<count && ndev<16;++i) {
        Dev *d=&dev[ndev];
        if (!(d->j=SDL_OpenJoystick(ids[i]))) continue;
        snprintf(d->name,sizeof d->name,"%s",SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "?");
        snprintf(d->lname,sizeof d->lname,"%s",d->name); lower_in_place(d->lname);
        snprintf(d->lpath,sizeof d->lpath,"%s",SDL_GetJoystickPathForID(ids[i]) ? SDL_GetJoystickPathForID(ids[i]) : "");
        lower_in_place(d->lpath);
        const char *col=strstr(d->lpath,"&col");
        d->col[0]=0;
        if (col && isxdigit((unsigned char)col[4]) && isxdigit((unsigned char)col[5])) snprintf(d->col,sizeof d->col,"col%c%c",col[4],col[5]);
        d->naxes=SDL_GetNumJoystickAxes(d->j); d->nbuttons=SDL_GetNumJoystickButtons(d->j);
        for (int a=0;a<d->naxes && a<64;++a) listed_value[ndev][a]=SDL_GetJoystickAxis(d->j,a);
        d->wheel=looks_like_wheel(ids[i],d->lname);
        d->pedals=!d->wheel && looks_like_pedals(d->lname);
        ++ndev;
    }
    SDL_free(ids);
}
static SDL_Joystick *wheel_joy(void) { return wheel_dev>=0 ? dev[wheel_dev].j : NULL; }
static SDL_Joystick *pedal_joy(void) { return pedal_dev>=0 ? dev[pedal_dev].j : wheel_joy(); }
static int pedal_index(void) { return pedal_dev>=0 ? pedal_dev : wheel_dev; }
static int hb_index(void) { return hb_dev>=0 ? hb_dev : pedal_index(); }
static SDL_Joystick *hb_joy(void) { return hb_index()>=0 ? dev[hb_index()].j : NULL; }
/* How many controllers have a name containing this one's; and this one's place among them. */
static int same_name(int d, int *rank) {
    int n=0;
    for (int i=0;i<ndev;++i) if (strstr(dev[i].lname,dev[d].lname)) { ++n; if (i==d && rank) *rank=n; }
    return n;
}

/* ---- the mapping, as default.ini has it and as edited here ---------------------------------------------------- */
static const char *const pedal_key[]={"throttle","brake","clutch","handbrake"};
static const char *const action_ps[]={"r1","l1","cross","circle","triangle","square","options","touchpad_left",
                                      "touchpad_right","l3","r3","pad_up","pad_down","pad_left","pad_right",
                                      "ffb_gain_up","ffb_gain_down"};   /* the last two: the runtime's, not PS4 buttons */
#define NACT ((int)(sizeof action_ps/sizeof *action_ps))
static const char *const gear_name[]={"1","2","3","4","5","6","r"};
typedef struct { int axis, inv; } AxisSetting;   /* axis -1: none; inv -1: the game decides, 0 normal, 1 inverted */
typedef struct {
    AxisSetting steer, pedal[4];
    int button[NACT], gear[7];
    int hb_button;                                 /* wheel_handbrake = button <n>; -1: an axis (pedal[3]) or none */
    char device[160], pedals[128], shifter[128];   /* wheel_device, wheel_pedals, wheel_shifter as read */
    char hb_device[128];                           /* wheel_handbrake_device as read */
    int rotation;
    char extra[16][96]; int nextra;   /* wheel_button lines for PS4 buttons this tool does not show */
} Mapping;
static Mapping cur, saved;
static bool dirty;

static AxisSetting parse_axis(const char *v) {
    char what[16]="", flag[16]="";
    sscanf(v," %15[^, ] , %15s",what,flag);
    lower_in_place(what); lower_in_place(flag);
    if (!strncmp(what,"axis",4)) memmove(what,what+4,strlen(what+4)+1);
    AxisSetting s;
    s.axis=(!*what || !strcmp(what,"none") || !strcmp(what,"off")) ? -1 : atoi(what);
    s.inv=!strcmp(flag,"inverted") ? 1 : !strcmp(flag,"normal") ? 0 : -1;
    return s;
}
static void load_mapping(const char *path, Mapping *m) {
    memset(m,0,sizeof *m);
    m->steer.axis=-1;
    for (int p=0;p<4;++p) { m->pedal[p].axis=-1; m->pedal[p].inv=-1; }
    for (int i=0;i<NACT;++i) m->button[i]=-1;
    for (int g=0;g<7;++g) m->gear[g]=-1;
    m->rotation=900;
    snprintf(m->device,sizeof m->device,"auto");
    snprintf(m->pedals,sizeof m->pedals,"wheel");
    snprintf(m->hb_device,sizeof m->hb_device,"pedals");
    m->hb_button=-1;
    FILE *f=fopen(path,"rb");
    if (!f) return;
    char line[512];
    while (fgets(line,sizeof line,f)) {
        char *hash=strchr(line,'#'); if (hash) *hash=0;
        char *eq=strchr(line,'=');
        if (!eq) continue;
        *eq=0;
        char *k=strip(line), *v=strip(eq+1);
        lower_in_place(k);
        if (strncmp(k,"wheel_",6)) continue;
        k+=6;
        if (!strcmp(k,"steering")) { m->steer=parse_axis(v); if (m->steer.inv<0) m->steer.inv=0; continue; }
        if (!strcmp(k,"handbrake") && !SDL_strncasecmp(v,"button",6)) { m->hb_button=atoi(v+6); m->pedal[3].axis=-1; continue; }
        for (int p=0;p<4;++p) if (!strcmp(k,pedal_key[p])) m->pedal[p]=parse_axis(v);
        if (!strcmp(k,"handbrake_device")) snprintf(m->hb_device,sizeof m->hb_device,"%s",v);
        if (!strcmp(k,"button")) {
            int index=-1; char ps[32]="";
            if (sscanf(v," %d , %31s",&index,ps)!=2 || index<0) continue;
            lower_in_place(ps);
            int a=-1;
            for (int i=0;i<NACT && a<0;++i) if (!strcmp(ps,action_ps[i])) a=i;
            if (a>=0) m->button[a]=index;
            else if (m->nextra<16) snprintf(m->extra[m->nextra++],sizeof m->extra[0],"wheel_button = %d, %s",index,ps);
        } else if (!strcmp(k,"gear")) {
            int index=-1; char g[8]="";
            if (sscanf(v," %d , %7s",&index,g)!=2 || index<0) continue;
            lower_in_place(g);
            const int n=g[0]=='r' ? 7 : atoi(g);
            if (n>=1 && n<=7) m->gear[n-1]=index;
        } else if (!strcmp(k,"device")) snprintf(m->device,sizeof m->device,"%s",v);
        else if (!strcmp(k,"pedals")) snprintf(m->pedals,sizeof m->pedals,"%s",v);
        else if (!strcmp(k,"shifter")) snprintf(m->shifter,sizeof m->shifter,"%s",v);
        else if (!strcmp(k,"rotation") && atoi(v)>=90 && atoi(v)<=3600) m->rotation=atoi(v);
    }
    fclose(f);
}
/* The controller a setting names, as the game picks it: part of the name, then " @col01" (part of the device path)
 * or " @2" (the second of that name); auto: the first that looks like a wheel. -1: none of them. */
static int find_device(const char *setting, bool for_wheel) {
    char want[160]; snprintf(want,sizeof want,"%s",setting); lower_in_place(want);
    char *w=strip(want), which[32]="";
    if (!*w || !strcmp(w,"off") || !strcmp(w,"none") || !strcmp(w,"wheel")) return -1;
    if (for_wheel && !strcmp(w,"auto")) { for (int i=0;i<ndev;++i) if (dev[i].wheel) return i; return -1; }
    char *at=strrchr(w,'@');
    if (at && at>w && at[-1]==' ' && at[1]) { snprintf(which,sizeof which,"%s",at+1); *at=0; w=strip(w); }
    int matched=0;
    for (int i=0;i<ndev;++i) {
        if (!strstr(dev[i].lname,w)) continue;
        if (for_wheel && dev[i].pedals) continue;
        if (!*which) return i;
        if (atoi(which)>0) { if (++matched==atoi(which)) return i; continue; }
        if (strstr(dev[i].lpath,which)) return i;
    }
    return -1;
}
static void resolve_devices(void) {
    wheel_dev=find_device(cur.device,true);
    if (wheel_dev<0) for (int i=0;i<ndev && wheel_dev<0;++i) if (dev[i].wheel) wheel_dev=i;
    pedal_dev=find_device(cur.pedals,false);
    if (pedal_dev==wheel_dev) pedal_dev=-1;
    char s[128]; snprintf(s,sizeof s,"%s",cur.shifter); lower_in_place(s);
    if (!strcmp(strip(s),"wheel")) shifter_dev=wheel_dev;
    else if (!strcmp(strip(s),"auto")) { shifter_dev=-1; for (int i=0;i<ndev && shifter_dev<0;++i) if (strstr(dev[i].lname,"shifter")) shifter_dev=i; }
    else shifter_dev=find_device(cur.shifter,false);
    snprintf(s,sizeof s,"%s",cur.hb_device); lower_in_place(s);
    const char *hb=strip(s);
    hb_dev=!strcmp(hb,"wheel") ? wheel_dev : !*hb || !strcmp(hb,"pedals") || !strcmp(hb,"auto") ? -1 : find_device(cur.hb_device,false);
}

/* ---- rows ------------------------------------------------------------------------------------------------------ */
enum { R_STEER=0, R_PEDAL=1, R_BUTTON=R_PEDAL+4, R_GEAR=R_BUTTON+NACT, NROWS=R_GEAR+7 };
static const char *row_label(int r) {
    static const char *const pedal_label[4][2]={{"Throttle","Газ"},{"Brake","Тормоз"},{"Clutch","Сцепление"},{"Handbrake","Ручник"}};
    static const char *const action_label[][2]={
        {"Shift up","Передача вверх"},{"Shift down","Передача вниз"},{"Cross: accept","Крестик: выбрать"},
        {"Circle: back, handbrake","Кружок: назад, ручник"},{"Triangle: camera","Треугольник: камера"},{"Square","Квадрат"},
        {"Options: pause","Options: пауза"},{"Touchpad left: reset","Тачпад слева: сброс"},{"Touchpad right","Тачпад справа"},
        {"L3","L3"},{"R3","R3"},{"D-pad up","Крестовина вверх"},{"D-pad down","Крестовина вниз"},
        {"D-pad left","Крестовина влево"},{"D-pad right","Крестовина вправо"},
        {"FFB gain + (this car)","Сила FFB + (эта машина)"},{"FFB gain - (this car)","Сила FFB - (эта машина)"}};
    static const char *const gear_label[7][2]={{"1st gear","1-я"},{"2nd gear","2-я"},{"3rd gear","3-я"},{"4th gear","4-я"},
                                                {"5th gear","5-я"},{"6th gear","6-я"},{"Reverse","Задняя"}};
    if (r==R_STEER) return T("Steering","Руль");
    if (r<R_BUTTON) return pedal_label[r-R_PEDAL][ru];
    if (r<R_GEAR) return action_label[r-R_BUTTON][ru];
    return gear_label[r-R_GEAR][ru];
}

/* ---- mapping a control: what the player moves while a row listens --------------------------------------------- */
static struct {
    int row, phase;             /* -1: not listening; pedals: 0 waiting for the press, 1 for the release */
    int base[64];               /* steering: the axes when Map was clicked */
    int start[16][64], first[16][64], lo[16][64], hi[16][64]; bool moved[16][64];   /* pedals, per controller */
    int dev, axis, rest, close, last; Uint64 still_since; bool hinted;
    bool base_btn[16][256];     /* buttons held when Map was clicked */
} L={.row=-1};
static char status[300];
static void say(const char *s) { snprintf(status,sizeof status,"%s",s); }

static void stop_listening(const char *why) { L.row=-1; say(why); }
static void start_listening(int r) {
    memset(&L,0,sizeof L);
    L.row=r;
    SDL_UpdateJoysticks();
    SDL_Joystick *w=wheel_joy(), *p=pedal_joy();
    char buf[300];
    if (r==R_STEER) {
        if (!w) { stop_listening(T("No wheel chosen.","Руль не выбран.")); return; }
        for (int a=0;a<SDL_GetNumJoystickAxes(w) && a<64;++a) L.base[a]=SDL_GetJoystickAxis(w,a);
        say(T("Hold the wheel straight, then turn it all the way LEFT.   (Esc: cancel)",
              "Держи руль прямо, затем поверни до упора ВЛЕВО.   (Esc — отмена)"));
    } else if (r<R_BUTTON) {
        /* the handbrake may be any controller's axis or button (a USB handbrake of its own); a pedal is the pedals' */
        if (!p && r!=R_PEDAL+3) { stop_listening(T("No wheel chosen.","Руль не выбран.")); return; }
        for (int d=0;d<ndev;++d) {
            for (int a=0;a<dev[d].naxes && a<64;++a) L.start[d][a]=SDL_GetJoystickAxis(dev[d].j,a);
            for (int b=0;b<dev[d].nbuttons && b<256;++b) L.base_btn[d][b]=SDL_GetJoystickButton(dev[d].j,b);
        }
        if (r==R_PEDAL+3) snprintf(buf,sizeof buf,"%s",T("Pull the handbrake all the way, then release it (any controller).   (Esc: cancel)",
                                                          "Потяни ручник до конца и отпусти (любой контроллер).   (Esc — отмена)"));
        else snprintf(buf,sizeof buf,T("Press the %s all the way, then release it.   (Esc: cancel)",
                                       "Выжми до конца: %s, затем отпусти.   (Esc — отмена)"),row_label(r));
        say(buf);
    } else if (r<R_GEAR) {
        if (!w) { stop_listening(T("No wheel chosen.","Руль не выбран.")); return; }
        for (int b=0;b<SDL_GetNumJoystickButtons(w) && b<256;++b) L.base_btn[0][b]=SDL_GetJoystickButton(w,b);
        snprintf(buf,sizeof buf,T("Press the wheel button for: %s   (Esc: cancel)","Нажми кнопку на руле для: %s   (Esc — отмена)"),row_label(r));
        say(buf);
    } else {
        for (int d=0;d<ndev;++d) for (int b=0;b<dev[d].nbuttons && b<256;++b) L.base_btn[d][b]=SDL_GetJoystickButton(dev[d].j,b);
        snprintf(buf,sizeof buf,T("Put the shifter in: %s   (Esc: cancel)","Включи на шифтере: %s   (Esc — отмена)"),row_label(r));
        say(buf);
    }
}
static void done(const char *what) {
    char buf[300];
    snprintf(buf,sizeof buf,"%s: %s.",row_label(L.row),what);
    L.row=-1; dirty=true;
    say(buf);
}
/* One look at the controllers for the listening row. */
static void poll_listening(void) {
    if (L.row<0) return;
    char what[160];
    const int r=L.row;
    if (r==R_STEER) {
        SDL_Joystick *w=wheel_joy();
        int best=-1, best_d=0;
        for (int a=0;a<SDL_GetNumJoystickAxes(w) && a<64;++a) {
            bool used=false;   /* the pedal axes, when they are axes of the wheel */
            if (pedal_dev<0) for (int p=0;p<4;++p) if (cur.pedal[p].axis==a) used=true;
            const int d=SDL_GetJoystickAxis(w,a)-L.base[a];
            if (!used && abs(d)>abs(best_d)) { best=a; best_d=d; }
        }
        if (best<0 || abs(best_d)<=22000) return;
        cur.steer.axis=best; cur.steer.inv=best_d>0;   /* left must read below the centre */
        snprintf(what,sizeof what,T("axis %d%s","ось %d%s"),best,cur.steer.inv ? T(", reversed",", перевёрнута") : "");
        done(what);
    } else if (r<R_BUTTON) {
        /* Many pedals send nothing until they move and read 0 till then, so no rest value is taken beforehand: the
         * axis that travels most of its range is the pedal, the end its first value sent was near is its rest, and
         * it is released when it is back near that end and still for a moment (or on Enter). */
        const int p=r-R_PEDAL;
        const bool handbrake=p==3;
        if (L.phase==0) {
            for (int d=0;d<ndev;++d) {
                if (!handbrake && d!=pedal_index()) continue;
                SDL_Joystick *j=dev[d].j;
                if (handbrake) for (int b=0;b<dev[d].nbuttons && b<256;++b) {   /* a handbrake that is a switch */
                    if (L.base_btn[d][b] || !SDL_GetJoystickButton(j,b)) continue;
                    hb_dev=d; cur.hb_button=b; cur.pedal[3].axis=-1;
                    snprintf(what,sizeof what,T("button %d of %s","кнопка %d (%s)"),b,dev[d].name);
                    done(what);
                    return;
                }
                for (int a=0;a<dev[d].naxes && a<64;++a) {
                    if (d==wheel_dev && cur.steer.axis==a) continue;
                    const int v=SDL_GetJoystickAxis(j,a);
                    if (!L.moved[d][a]) {
                        if (abs(v-L.start[d][a])<1500) continue;
                        L.moved[d][a]=true; L.first[d][a]=L.lo[d][a]=L.hi[d][a]=v;
                        if (L.start[d][a]) {
                            if (L.start[d][a]<L.lo[d][a]) L.lo[d][a]=L.start[d][a];
                            if (L.start[d][a]>L.hi[d][a]) L.hi[d][a]=L.start[d][a];
                            L.first[d][a]=L.start[d][a];
                        }
                    }
                    if (v<L.lo[d][a]) L.lo[d][a]=v;
                    if (v>L.hi[d][a]) L.hi[d][a]=v;
                    if (L.hi[d][a]-L.lo[d][a]<=40000) continue;
                    L.dev=d; L.axis=a;
                    L.rest=L.first[d][a]-L.lo[d][a]<L.hi[d][a]-L.first[d][a] ? L.lo[d][a] : L.hi[d][a];
                    L.close=(L.hi[d][a]-L.lo[d][a])/4;
                    L.last=v; L.still_since=SDL_GetTicks(); L.phase=1;
                    /* it moves over from another row that had it on this controller */
                    for (int q=0;q<3;++q) if (q!=p && d==pedal_index() && cur.pedal[q].axis==a) cur.pedal[q].axis=-1;
                    if (!handbrake && d==hb_index() && cur.hb_button<0 && cur.pedal[3].axis==a) cur.pedal[3].axis=-1;
                    if (handbrake) { hb_dev=d==pedal_index() ? -1 : d; cur.hb_button=-1; }
                    cur.pedal[p].axis=a; cur.pedal[p].inv=L.rest==L.hi[d][a];
                    char buf[300];
                    if (handbrake && hb_dev>=0)
                        snprintf(buf,sizeof buf,T("%s: axis %d of %s%s. Release it.","%s: ось %d (%s)%s. Отпусти."),row_label(r),a,dev[d].name,
                                 cur.pedal[p].inv ? T(" (reversed)"," (перевёрнута)") : "");
                    else snprintf(buf,sizeof buf,T("%s: axis %d%s. Release it.","%s: ось %d%s. Отпусти."),row_label(r),a,
                                  cur.pedal[p].inv ? T(" (reversed)"," (перевёрнута)") : "");
                    say(buf);
                    return;
                }
            }
        } else {
            const int v=SDL_GetJoystickAxis(dev[L.dev].j,L.axis);
            if (abs(v-L.last)>600) { L.last=v; L.still_since=SDL_GetTicks(); }
            if (abs(v-L.rest)<=L.close && SDL_GetTicks()-L.still_since>=250) {
                snprintf(what,sizeof what,T("axis %d%s","ось %d%s"),L.axis,cur.pedal[p].inv ? T(", reversed",", перевёрнута") : "");
                done(what);
            } else if (!L.hinted && SDL_GetTicks()-L.still_since>=4000) {
                L.hinted=true;
                say(T("Released but nothing happens? Press Enter.","Отпустил, но ничего не происходит? Нажми Enter."));
            }
        }
    } else if (r<R_GEAR) {
        SDL_Joystick *w=wheel_joy();
        for (int b=0;b<SDL_GetNumJoystickButtons(w) && b<256;++b) {
            if (L.base_btn[0][b] || !SDL_GetJoystickButton(w,b)) continue;
            const int i=r-R_BUTTON;
            char taken[160]="";
            for (int k=0;k<NACT;++k) if (k!=i && cur.button[k]==b) {
                cur.button[k]=-1;
                snprintf(taken,sizeof taken,T(" (taken from \"%s\")"," (снята с «%s»)"),row_label(R_BUTTON+k));
            }
            cur.button[i]=b;
            snprintf(what,sizeof what,T("button %d%s","кнопка %d%s"),b,taken);
            done(what);
            return;
        }
    } else {
        for (int d=0;d<ndev;++d) for (int b=0;b<dev[d].nbuttons && b<256;++b) {
            if (L.base_btn[d][b] || !SDL_GetJoystickButton(dev[d].j,b)) continue;
            const int g=r-R_GEAR;
            char note[160]="";
            if (shifter_dev>=0 && d!=shifter_dev) {   /* the gears are on another controller now: the old ones go */
                for (int k=0;k<7;++k) cur.gear[k]=-1;
                snprintf(note,sizeof note,"%s",T(" (the other gears were on another controller: map them again)",
                                                 " (остальные передачи были на другом контроллере: настрой их заново)"));
            }
            for (int k=0;k<7;++k) if (k!=g && cur.gear[k]==b) cur.gear[k]=-1;
            shifter_dev=d; cur.gear[g]=b;
            snprintf(what,sizeof what,T("button %d of %s%s","кнопка %d (%s)%s"),b,dev[d].name,note);
            done(what);
            return;
        }
    }
}
static void clear_row(int r) {
    if (r==R_STEER) cur.steer.axis=-1;
    else if (r<R_BUTTON) { cur.pedal[r-R_PEDAL].axis=-1; if (r==R_PEDAL+3) { cur.hb_button=-1; hb_dev=-1; } }
    else if (r<R_GEAR) cur.button[r-R_BUTTON]=-1;
    else cur.gear[r-R_GEAR]=-1;
    dirty=true;
    char buf[200]; snprintf(buf,sizeof buf,T("%s: cleared.","%s: сброшено."),row_label(r)); say(buf);
}

/* ---- saving ---------------------------------------------------------------------------------------------------- */
static char ini[1200];
static void ini_path(void) {
    char dir[1024]="";
    const char *b=SDL_GetBasePath();
    if (b) snprintf(dir,sizeof dir,"%s",b);
    static const char *const tries[]={"data\\user\\input_config\\default.ini","..\\data\\user\\input_config\\default.ini"};
    for (size_t i=0;i<2;++i) {
        snprintf(ini,sizeof ini,"%s%s",dir,tries[i]);
        FILE *f=fopen(ini,"rb");
        if (f) { fclose(f); return; }
    }
    ini[0]=0;
}
static bool save(int rotation) {
    /* wheel_device: auto when the game would find this wheel by itself; by name otherwise, and a wheel that is
     * several controllers of one name (a Fanatec base is two "FANATEC Wheel") with " @<which>": its HID collection
     * (col01...) stays the same from start to start, the order Windows lists them in does not. */
    char device[200]="auto";
    if (wheel_dev>=0) {
        int first_wheel=-1, rank=0;
        for (int i=0;i<ndev && first_wheel<0;++i) if (dev[i].wheel) first_wheel=i;
        const int same=same_name(wheel_dev,&rank);
        if (same>1 && dev[wheel_dev].col[0]) snprintf(device,sizeof device,"%s @%s",dev[wheel_dev].name,dev[wheel_dev].col);
        else if (same>1) snprintf(device,sizeof device,"%s @%d",dev[wheel_dev].name,rank);
        else if (first_wheel!=wheel_dev || pedal_dev>=0) snprintf(device,sizeof device,"%s",dev[wheel_dev].name);
    }
    bool any_gear=false;
    for (int g=0;g<7;++g) if (cur.gear[g]>=0) any_gear=true;
    char shifter[160]="off";
    if (any_gear) {
        if (shifter_dev<0) snprintf(shifter,sizeof shifter,"%s",cur.shifter[0] ? cur.shifter : "auto");
        else if (shifter_dev==wheel_dev) snprintf(shifter,sizeof shifter,"wheel");
        else snprintf(shifter,sizeof shifter,"%s",strstr(dev[shifter_dev].lname,"shifter") ? "auto" : dev[shifter_dev].name);
    }

    /* default.ini: the old wheel_* lines go, except the player's force feedback tuning and the two switches this
     * window does not show; everything else stays; the new lines are added at the end. */
    FILE *f=fopen(ini,"rb");
    if (!f) return false;
    char *text=calloc(1,1<<20); size_t n=text ? fread(text,1,(1<<20)-1,f) : 0; fclose(f);
    if (!text) return false;
    char backup[1300]; snprintf(backup,sizeof backup,"%s.bak",ini);
    if ((f=fopen(backup,"wb"))) { fwrite(text,1,n,f); fclose(f); }
    if (!(f=fopen(ini,"wb"))) { free(text); return false; }
    bool kept_ffb=false, kept_hat=false, kept_stick=false;
    for (char *l=text; l && *l;) {
        char *next=strchr(l,'\n');
        const size_t len=next ? (size_t)(next-l)+1 : strlen(l);
        const char *s=l; while (*s==' ' || *s=='\t') ++s;
        const bool ffb=!SDL_strncasecmp(s,"wheel_ffb",9), hat=!SDL_strncasecmp(s,"wheel_hat_dpad",14), stick=!SDL_strncasecmp(s,"wheel_as_stick",14);
        kept_ffb|=ffb; kept_hat|=hat; kept_stick|=stick;
        if (ffb || hat || stick || SDL_strncasecmp(s,"wheel_",6)) fwrite(l,1,len,f);
        l=next ? next+1 : NULL;
    }
    if (n && text[n-1]!='\n') fputc('\n',f);
    free(text);
    time_t now=time(NULL); char when[32]; strftime(when,sizeof when,"%Y-%m-%d %H:%M",localtime(&now));
    fprintf(f,"\n# Wheel settings written by WheelSetup.exe on %s for %s\n",when,wheel_dev>=0 ? dev[wheel_dev].name : "?");
    fprintf(f,"wheel_device = %s\n",device);
    fprintf(f,"wheel_rotation = %d\n",rotation);
    if (cur.steer.axis<0) fprintf(f,"wheel_steering = none\n");
    else fprintf(f,"wheel_steering = %d%s\n",cur.steer.axis,cur.steer.inv==1 ? ", inverted" : "");
    fprintf(f,"wheel_pedals = %s\n",pedal_dev>=0 ? dev[pedal_dev].name : "wheel");
    /* the handbrake's controller: the pedals', the wheel's, or one of its own by name */
    char hb_device[160]="pedals";
    if (hb_dev>=0 && hb_dev==wheel_dev) snprintf(hb_device,sizeof hb_device,"wheel");
    else if (hb_dev>=0 && hb_dev!=pedal_index()) snprintf(hb_device,sizeof hb_device,"%s",dev[hb_dev].name);
    fprintf(f,"wheel_handbrake_device = %s\n",hb_device);
    for (int p=0;p<4;++p) {
        const AxisSetting a=cur.pedal[p];
        if (p==3 && cur.hb_button>=0) fprintf(f,"wheel_handbrake = button %d\n",cur.hb_button);
        else if (a.axis<0) fprintf(f,"wheel_%s = none\n",pedal_key[p]);
        else if (a.inv<0) fprintf(f,"wheel_%s = %d\n",pedal_key[p],a.axis);
        else fprintf(f,"wheel_%s = %d, %s\n",pedal_key[p],a.axis,a.inv ? "inverted" : "normal");
    }
    for (int i=0;i<NACT;++i) if (cur.button[i]>=0) fprintf(f,"wheel_button = %d, %s\n",cur.button[i],action_ps[i]);
    for (int i=0;i<cur.nextra;++i) fprintf(f,"%s\n",cur.extra[i]);
    fprintf(f,"wheel_shifter = %s\n",shifter);
    for (int g=0;g<7;++g) if (cur.gear[g]>=0) fprintf(f,"wheel_gear = %d, %s\n",cur.gear[g],gear_name[g]);
    if (!kept_hat) fprintf(f,"wheel_hat_dpad = true\n");
    if (!kept_stick) fprintf(f,"wheel_as_stick = false\n");
    if (!kept_ffb) fprintf(f,"wheel_ffb = on\nwheel_ffb_gain = 100\nwheel_ffb_vibration = 100\nwheel_ffb_invert = false\n");
    fclose(f);
    snprintf(cur.device,sizeof cur.device,"%s",device);
    snprintf(cur.pedals,sizeof cur.pedals,"%s",pedal_dev>=0 ? dev[pedal_dev].name : "wheel");
    snprintf(cur.shifter,sizeof cur.shifter,"%s",shifter);
    snprintf(cur.hb_device,sizeof cur.hb_device,"%s",hb_device);
    cur.rotation=rotation;
    saved=cur; dirty=false;
    return true;
}

/* ---- the window ------------------------------------------------------------------------------------------------ */
static HWND win, wheel_combo, pedal_combo, rotation_edit;
static HFONT font, bold;
static float scale=1.0f;
static int S(int v) { return (int)(v*scale+0.5f); }
static int hover=-1;
enum { ID_WHEEL=1, ID_PEDALS, ID_ROTATION, ID_SAVE, ID_UNDO, ID_CLOSE };
#define HEAD_Y 84
#define SECTION_H 28
#define ROW_H 24
#define LIVE_X 400
#define MAP_X 640
#define CLEAR_X 720
#define WIN_W 780
static int row_top(int r) {
    int y=HEAD_Y+SECTION_H;
    if (r>=R_BUTTON) y+=SECTION_H;
    if (r>=R_GEAR) y+=SECTION_H;
    return y+r*ROW_H;
}
static int rows_bottom(void) { return row_top(NROWS); }

static void wide(const char *s, wchar_t *out, int n) { if (!MultiByteToWideChar(CP_UTF8,0,s,-1,out,n)) out[0]=0; }
static void draw_text(HDC dc, int x, int y, int w, int h, const char *s, COLORREF c, HFONT f, UINT fmt) {
    wchar_t buf[512]; wide(s,buf,512);
    SelectObject(dc,f); SetTextColor(dc,c); SetBkMode(dc,TRANSPARENT);
    RECT r={S(x),S(y),S(x+w),S(y+h)};
    DrawTextW(dc,buf,-1,&r,fmt|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
}
static void fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    HBRUSH b=CreateSolidBrush(c); RECT r={S(x),S(y),S(x+w),S(y+h)}; FillRect(dc,&r,b); DeleteObject(b);
}
static void frame(HDC dc, int x, int y, int w, int h, COLORREF c) {
    HBRUSH b=CreateSolidBrush(c); RECT r={S(x),S(y),S(x+w),S(y+h)}; FrameRect(dc,&r,b); DeleteObject(b);
}
static const COLORREF C_BG=RGB(246,246,246), C_TEXT=RGB(30,30,30), C_DIM=RGB(140,140,140), C_LINE=RGB(220,220,220),
                      C_HOVER=RGB(234,240,250), C_LISTEN=RGB(255,244,200), C_BAR=RGB(60,130,220), C_ON=RGB(60,170,90),
                      C_BUTTON=RGB(255,255,255), C_HEAD=RGB(90,90,90);

static void mapping_text(int r, char *out, size_t n) {
    const char *none=T("not set","нет");
    if (r==R_STEER) {
        if (cur.steer.axis<0) snprintf(out,n,"%s",none);
        else snprintf(out,n,T("axis %d%s","ось %d%s"),cur.steer.axis,cur.steer.inv==1 ? T(", reversed",", перевёрнута") : "");
    } else if (r==R_PEDAL+3 && (cur.hb_button>=0 || hb_dev>=0)) {   /* the handbrake on a controller of its own, or a switch */
        const char *of=hb_index()>=0 ? dev[hb_index()].name : "?";
        if (cur.hb_button>=0) snprintf(out,n,T("button %d of %s","кнопка %d (%s)"),cur.hb_button,of);
        else if (cur.pedal[3].axis<0) snprintf(out,n,"%s",none);
        else snprintf(out,n,T("axis %d of %s%s","ось %d (%s)%s"),cur.pedal[3].axis,of,cur.pedal[3].inv==1 ? T(", reversed",", перевёрнута") : "");
    } else if (r<R_BUTTON) {
        const AxisSetting a=cur.pedal[r-R_PEDAL];
        if (a.axis<0) snprintf(out,n,"%s",none);
        else snprintf(out,n,T("axis %d%s%s","ось %d%s%s"),a.axis,pedal_dev>=0 ? T(" of the pedals"," педалей") : "",
                      a.inv==1 ? T(", reversed",", перевёрнута") : "");
    } else if (r<R_GEAR) {
        const int b=cur.button[r-R_BUTTON];
        if (b<0) snprintf(out,n,"%s",none); else snprintf(out,n,T("button %d","кнопка %d"),b);
    } else {
        const int b=cur.gear[r-R_GEAR];
        if (b<0) snprintf(out,n,"%s",none); else snprintf(out,n,T("button %d","кнопка %d"),b);
    }
}
/* The live view of a row: the steering from the centre, a pedal from released to pressed, a button lit. */
static void draw_live(HDC dc, int r, int y) {
    const int x=LIVE_X, w=210, h=ROW_H-10;
    y+=5;
    if (r==R_PEDAL+3 && cur.hb_button>=0) {   /* a handbrake that is a switch: lit while pulled */
        SDL_Joystick *j=hb_joy();
        const bool on=j && cur.hb_button<SDL_GetNumJoystickButtons(j) && SDL_GetJoystickButton(j,cur.hb_button);
        fill(dc,x,y,h,h,on ? C_ON : RGB(255,255,255)); frame(dc,x,y,h,h,on ? C_ON : C_LINE);
        if (on) draw_text(dc,x+h+8,y-5,120,ROW_H,T("pulled","потянут"),C_ON,font,0);
        return;
    }
    if (r<R_BUTTON) {
        SDL_Joystick *j=r==R_STEER ? wheel_joy() : r==R_PEDAL+3 ? hb_joy() : pedal_joy();
        const AxisSetting a=r==R_STEER ? cur.steer : cur.pedal[r-R_PEDAL];
        fill(dc,x,y,w,h,RGB(255,255,255)); frame(dc,x,y,w,h,C_LINE);
        if (!j || a.axis<0 || a.axis>=SDL_GetNumJoystickAxes(j)) return;
        int v=SDL_GetJoystickAxis(j,a.axis);
        if (a.inv==1) v=-v-1;
        if (r==R_STEER) {
            const int mid=x+w/2, len=(int)((float)v/32768.0f*(w/2-2));
            if (len>=0) fill(dc,mid,y+2,len,h-4,C_BAR); else fill(dc,mid+len,y+2,-len,h-4,C_BAR);
            fill(dc,mid,y,1,h,C_DIM);
        } else {
            const int d=r==R_PEDAL+3 ? hb_index() : pedal_index();
            if (d>=0 && a.axis<64 && !axis_moved[d][a.axis]) {
                if (SDL_GetJoystickAxis(j,a.axis)==listed_value[d][a.axis]) return;   /* silent so far: released */
                axis_moved[d][a.axis]=true;
            }
            const float t=(v+32768.0f)/65535.0f;
            fill(dc,x+2,y+2,(int)(t*(w-4)),h-4,C_BAR);
        }
        return;
    }
    SDL_Joystick *j=NULL; int b=-1;
    if (r<R_GEAR) { j=wheel_joy(); b=cur.button[r-R_BUTTON]; }
    else { j=shifter_dev>=0 ? dev[shifter_dev].j : NULL; b=cur.gear[r-R_GEAR]; }
    const bool on=j && b>=0 && b<SDL_GetNumJoystickButtons(j) && SDL_GetJoystickButton(j,b);
    fill(dc,x,y,h,h,on ? C_ON : RGB(255,255,255)); frame(dc,x,y,h,h,on ? C_ON : C_LINE);
    if (on) draw_text(dc,x+h+8,y-5,120,ROW_H,T("pressed","нажата"),C_ON,font,0);
}
static void paint(HDC dc, int cw, int ch) {
    RECT all={0,0,cw,ch};
    HBRUSH bg=CreateSolidBrush(C_BG); FillRect(dc,&all,bg); DeleteObject(bg);
    draw_text(dc,16,12,70,26,T("Wheel","Руль"),C_TEXT,bold,0);
    draw_text(dc,16,46,70,26,T("Pedals","Педали"),C_TEXT,bold,0);
    draw_text(dc,530,12,100,26,T("Rotation °","Угол °"),C_TEXT,bold,0);
    static const char *const heads[3][2]={{"Steering and pedals","Руль и педали"},{"Wheel buttons","Кнопки руля"},
                                          {"H-pattern shifter","H-шифтер"}};
    for (int s=0;s<3;++s) {
        const int y=row_top(s==0 ? R_STEER : s==1 ? R_BUTTON : R_GEAR)-SECTION_H;
        draw_text(dc,16,y+4,400,SECTION_H-4,heads[s][ru],C_HEAD,bold,0);
        fill(dc,16,y+SECTION_H-2,WIN_W-32,1,C_LINE);
    }
    char buf[200];
    for (int r=0;r<NROWS;++r) {
        const int y=row_top(r);
        if (r==L.row) fill(dc,12,y,WIN_W-24,ROW_H,C_LISTEN);
        else if (r==hover) fill(dc,12,y,WIN_W-24,ROW_H,C_HOVER);
        draw_text(dc,24,y,180,ROW_H,row_label(r),C_TEXT,font,0);
        mapping_text(r,buf,sizeof buf);
        const bool unset=(r==R_STEER && cur.steer.axis<0) ||
                         (r>=R_PEDAL && r<R_BUTTON && cur.pedal[r-R_PEDAL].axis<0 && (r!=R_PEDAL+3 || cur.hb_button<0)) ||
                         (r>=R_BUTTON && r<R_GEAR && cur.button[r-R_BUTTON]<0) || (r>=R_GEAR && cur.gear[r-R_GEAR]<0);
        draw_text(dc,210,y,185,ROW_H,buf,unset ? C_DIM : C_TEXT,font,0);
        draw_live(dc,r,y);
        fill(dc,MAP_X,y+3,70,ROW_H-6,C_BUTTON); frame(dc,MAP_X,y+3,70,ROW_H-6,r==L.row ? C_BAR : C_LINE);
        draw_text(dc,MAP_X,y+1,70,ROW_H-2,r==L.row ? T("Cancel","Отмена") : T("Map","Задать"),r==L.row ? C_BAR : C_TEXT,font,DT_CENTER);
        fill(dc,CLEAR_X,y+3,30,ROW_H-6,C_BUTTON); frame(dc,CLEAR_X,y+3,30,ROW_H-6,C_LINE);
        draw_text(dc,CLEAR_X,y+1,30,ROW_H-2,"\xc3\x97",C_DIM,font,DT_CENTER);
    }
    const int sy=rows_bottom()+8;
    fill(dc,12,sy,WIN_W-24,32,L.row>=0 ? C_LISTEN : RGB(236,236,236));
    draw_text(dc,22,sy,WIN_W-44,32,status,C_TEXT,L.row>=0 ? bold : font,0);
}

static bool filling;   /* the window fills its own controls: no change of the player's */
static void fill_combos(void) {
    filling=true;
    SendMessageW(wheel_combo,CB_RESETCONTENT,0,0);
    SendMessageW(pedal_combo,CB_RESETCONTENT,0,0);
    wchar_t w[300];
    wide(T("On the wheel (no USB of their own)","На руле (без своего USB)"),w,300);
    SendMessageW(pedal_combo,CB_ADDSTRING,0,(LPARAM)w);
    for (int i=0;i<ndev;++i) {
        char item[300];
        snprintf(item,sizeof item,T("%d. %s%s%s (%d axes, %d buttons)%s","%d. %s%s%s (%d осей, %d кнопок)%s"),i+1,dev[i].name,
                 dev[i].col[0] ? ", " : "",dev[i].col,dev[i].naxes,dev[i].nbuttons,
                 dev[i].wheel ? T("  - wheel","  - руль") : dev[i].pedals ? T("  - pedals","  - педали") : "");
        wide(item,w,300);
        SendMessageW(wheel_combo,CB_ADDSTRING,0,(LPARAM)w);
        SendMessageW(pedal_combo,CB_ADDSTRING,0,(LPARAM)w);
    }
    SendMessageW(wheel_combo,CB_SETCURSEL,wheel_dev>=0 ? wheel_dev : (WPARAM)-1,0);
    SendMessageW(pedal_combo,CB_SETCURSEL,pedal_dev+1,0);
    wchar_t rot[16]; swprintf(rot,16,L"%d",cur.rotation);
    SetWindowTextW(rotation_edit,rot);
    filling=false;
}
static int rotation_value(void) {
    wchar_t w[16]; GetWindowTextW(rotation_edit,w,16);
    const int v=_wtoi(w);
    return v>=90 && v<=3600 ? v : cur.rotation;
}
static int row_at(int y) {
    for (int r=0;r<NROWS;++r) if (y>=S(row_top(r)) && y<S(row_top(r)+ROW_H)) return r;
    return -1;
}
static void do_save(void) {
    if (L.row>=0) stop_listening("");
    if (save(rotation_value())) {
        char buf[300]; snprintf(buf,sizeof buf,T("Saved. The game uses it the next time it starts (the old file is default.ini.bak).",
                                                 "Сохранено. Игра применит это при следующем запуске (прежний файл — default.ini.bak)."));
        say(buf);
    } else say(T("Could not save default.ini (is the game running?).","Не удалось сохранить default.ini (игра открыта?)."));
}
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        SDL_UpdateJoysticks();
        poll_listening();
        { RECT r={0,S(HEAD_Y),S(WIN_W),S(rows_bottom()+44)}; InvalidateRect(h,&r,FALSE); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT cr; GetClientRect(h,&cr);
        HDC mem=CreateCompatibleDC(dc); HBITMAP bmp=CreateCompatibleBitmap(dc,cr.right,cr.bottom);
        HGDIOBJ old=SelectObject(mem,bmp);
        paint(mem,cr.right,cr.bottom);
        BitBlt(dc,0,0,cr.right,cr.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h,&ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        const int r=row_at((short)HIWORD(lp));
        if (r!=hover) { hover=r; InvalidateRect(h,NULL,FALSE); }
        TRACKMOUSEEVENT t={sizeof t,TME_LEAVE,h,0}; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: hover=-1; InvalidateRect(h,NULL,FALSE); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        const int x=(short)LOWORD(lp), r=row_at((short)HIWORD(lp));
        if (r<0) return 0;
        if (x>=S(CLEAR_X) && x<S(CLEAR_X+30)) { if (L.row==r) L.row=-1; clear_row(r); }
        else if (L.row==r) stop_listening(T("Cancelled.","Отменено."));
        else start_listening(r);
        InvalidateRect(h,NULL,FALSE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp)==ID_WHEEL && HIWORD(wp)==CBN_SELCHANGE) {
            const int sel=(int)SendMessageW(wheel_combo,CB_GETCURSEL,0,0);
            if (sel>=0 && sel<ndev) { wheel_dev=sel; if (pedal_dev==wheel_dev) { pedal_dev=-1; SendMessageW(pedal_combo,CB_SETCURSEL,0,0); } dirty=true; }
            say(T("Wheel chosen. Map the steering on it.","Руль выбран. Задай на нём руление."));
        } else if (LOWORD(wp)==ID_PEDALS && HIWORD(wp)==CBN_SELCHANGE) {
            const int sel=(int)SendMessageW(pedal_combo,CB_GETCURSEL,0,0)-1;
            pedal_dev=sel>=0 && sel<ndev && sel!=wheel_dev ? sel : -1;
            if (sel==wheel_dev) SendMessageW(pedal_combo,CB_SETCURSEL,0,0);
            dirty=true;
            say(T("Pedals chosen. Map throttle, brake and clutch again if their axes changed.",
                  "Педали выбраны. Задай газ, тормоз и сцепление заново, если оси изменились."));
        } else if (LOWORD(wp)==ID_ROTATION && HIWORD(wp)==EN_CHANGE) { if (!filling) dirty=true; }
        else if (LOWORD(wp)==ID_SAVE) do_save();
        else if (LOWORD(wp)==ID_UNDO) {
            L.row=-1; cur=saved; resolve_devices(); fill_combos(); dirty=false;
            say(T("Back to the saved settings.","Возвращены сохранённые настройки."));
        } else if (LOWORD(wp)==ID_CLOSE) PostMessageW(h,WM_CLOSE,0,0);
        InvalidateRect(h,NULL,FALSE);
        return 0;
    case WM_CLOSE:
        if (dirty) {
            wchar_t q[300]; wide(T("Save the changes?","Сохранить изменения?"),q,300);
            const int a=MessageBoxW(h,q,L"WheelSetup",MB_YESNOCANCEL|MB_ICONQUESTION);
            if (a==IDCANCEL) return 0;
            if (a==IDYES) { do_save(); if (dirty) return 0; }
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
static HWND child(const wchar_t *cls, const char *text, DWORD style, int x, int y, int w, int h, int id) {
    wchar_t t[200]; wide(text,t,200);
    HWND c=CreateWindowExW(0,cls,t,WS_CHILD|WS_VISIBLE|style,S(x),S(y),S(w),S(h),win,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(c,WM_SETFONT,(WPARAM)font,TRUE);
    return c;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    SetProcessDPIAware();
    ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    { HDC s=GetDC(NULL); scale=GetDeviceCaps(s,LOGPIXELSY)/96.0f; ReleaseDC(NULL,s); }
    font=CreateFontW(-S(14),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    bold=CreateFontW(-S(14),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");

    ini_path();
    if (!ini[0]) {
        wchar_t m[400]; wide(T("data\\user\\input_config\\default.ini was not found.\nPut WheelSetup.exe next to DriveClub.exe and start the game once.",
                               "Не найден data\\user\\input_config\\default.ini.\nПоложи WheelSetup.exe рядом с DriveClub.exe и хотя бы раз запусти игру."),m,400);
        MessageBoxW(NULL,m,L"WheelSetup",MB_ICONERROR);
        return 1;
    }
    load_mapping(ini,&cur);
    saved=cur;

    WNDCLASSW wc={0};
    wc.lpfnWndProc=wndproc; wc.hInstance=inst; wc.lpszClassName=L"WheelSetup";
    wc.hCursor=LoadCursorW(NULL,(LPCWSTR)IDC_ARROW); wc.hIcon=LoadIconW(NULL,(LPCWSTR)IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r={0,0,S(WIN_W),S(rows_bottom()+92)};
    const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    AdjustWindowRect(&r,style,FALSE);
    wchar_t title[100]; wide(T("Wheel setup - DriveClub Recomp","Настройка руля - DriveClub Recomp"),title,100);
    win=CreateWindowExW(0,L"WheelSetup",title,style,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,NULL,NULL,inst,NULL);
    wheel_combo=child(L"COMBOBOX","",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,90,12,420,300,ID_WHEEL);
    pedal_combo=child(L"COMBOBOX","",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,90,46,420,300,ID_PEDALS);
    rotation_edit=child(L"EDIT","",WS_BORDER|ES_NUMBER|WS_TABSTOP,640,13,70,24,ID_ROTATION);
    const int by=rows_bottom()+50;
    child(L"BUTTON",T("Save","Сохранить"),BS_DEFPUSHBUTTON|WS_TABSTOP,WIN_W-16-3*110-20,by,110,30,ID_SAVE);
    child(L"BUTTON",T("Undo changes","Отменить"),WS_TABSTOP,WIN_W-16-2*110-10,by,110,30,ID_UNDO);
    child(L"BUTTON",T("Close","Закрыть"),WS_TABSTOP,WIN_W-16-110,by,110,30,ID_CLOSE);
    say(T("Looking for controllers... On some PCs Windows takes a while to answer.",
          "Ищу контроллеры... На некоторых ПК Windows отвечает не сразу."));
    ShowWindow(win,show); UpdateWindow(win);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    if (!SDL_Init(SDL_INIT_JOYSTICK)) {
        wchar_t m[300]; wide(SDL_GetError(),m,300); MessageBoxW(win,m,L"SDL",MB_ICONERROR); return 1;
    }
    for (int i=0;i<40;++i) { SDL_UpdateJoysticks(); SDL_Delay(5); }   /* the controllers are listed shortly after start */
    list_devices();
    resolve_devices();
    fill_combos();
    if (!ndev) say(T("Windows lists no game controller. Connect the wheel, then start WheelSetup again.",
                     "Windows не видит ни одного контроллера. Подключи руль и запусти WheelSetup снова."));
    else say(T("Click Map (or a row), then move that control. x clears a row. Save when done.",
               "Нажми «Задать» (или строку) и подвигай этот орган управления. × сбрасывает. В конце — «Сохранить»."));
    InvalidateRect(win,NULL,FALSE);
    SetTimer(win,1,16,NULL);

    MSG m;
    while (GetMessageW(&m,NULL,0,0)>0) {
        if (m.message==WM_KEYDOWN && m.wParam==VK_ESCAPE && L.row>=0) { stop_listening(T("Cancelled.","Отменено.")); InvalidateRect(win,NULL,FALSE); continue; }
        if (m.message==WM_KEYDOWN && m.wParam==VK_RETURN && L.row>=R_PEDAL && L.row<R_BUTTON && L.phase==1) {
            char what[80]; snprintf(what,sizeof what,T("axis %d%s","ось %d%s"),L.axis,cur.pedal[L.row-R_PEDAL].inv ? T(", reversed",", перевёрнута") : "");
            done(what); InvalidateRect(win,NULL,FALSE); continue;
        }
        if (IsDialogMessageW(win,&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    for (int i=0;i<ndev;++i) SDL_CloseJoystick(dev[i].j);
    SDL_Quit();
    return 0;
}
