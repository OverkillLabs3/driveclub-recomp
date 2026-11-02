"""Exercise the actual runtime effect loop against an in-memory haptic backend.

No SDL device is opened and no motor commands can be emitted.
"""
from pathlib import Path
import os
import shutil
import subprocess

root = Path(__file__).resolve().parent.parent
src = (root / 'src/runtime_logiwheel.inc').read_text()
start = src.index('        for (int i=0;i<LOGI_EFFECTS;++i) {', src.index('was=physics; }'))
end = src.index('        if (ffb_software) ffb_software_pass', start)
loop = src[start:end]
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef int16_t Sint16; typedef uint16_t Uint16;
#define LOGI_EFFECTS 16
enum {LOGI_CONSTANT=0,LOGI_PERIODIC=2,LOGI_SPRING=7,LOGI_DAMPER=8,SDL_HAPTIC_SPRING=128};
typedef struct {int type,a,c;} LogiEffectParams;
typedef struct {int type;struct {Sint16 center[3];}condition;struct {Uint16 magnitude;}periodic;} SDL_HapticEffect;
typedef struct {int unused;} SDL_Haptic;
static bool ffb_zero,ffb_software,ffb_physics,ffb_texture_changes;
static int ffb_spring=-1,ffb_gain=100,ffb_texture_floor=0,ffb_long_rumble=25;
static int creates,updates,runs,stops,destroys,created_type[512];
static bool ffb_effect_from(const LogiEffectParams*p,SDL_HapticEffect*e,int pos){(void)pos;memset(e,0,sizeof(*e));e->type=p->type==7?128:p->type;e->periodic.magnitude=1000;return true;}
static bool ffb_wave_is_kerb(const LogiEffectParams*p){return p->c>=45;}
static Uint16 ffb_wave_level(float f,const LogiEffectParams*p){(void)p;return (Uint16)f;}
static void SDL_DestroyHapticEffect(SDL_Haptic*h,int id){(void)h;(void)id;destroys++;}
static int SDL_CreateHapticEffect(SDL_Haptic*h,const SDL_HapticEffect*e){(void)h;creates++;created_type[e->type]++;return 100+creates;}
static bool SDL_UpdateHapticEffect(SDL_Haptic*h,int id,const SDL_HapticEffect*e){(void)h;(void)id;(void)e;updates++;return true;}
static bool SDL_RunHapticEffect(SDL_Haptic*h,int id,int n){(void)h;(void)id;(void)n;runs++;return true;}
static bool SDL_StopHapticEffect(SDL_Haptic*h,int id){(void)h;(void)id;stops++;return true;}
static const char* SDL_GetError(void){return "fake backend";}
static int scenario(const char*name,bool model,bool active,bool software,bool dry,bool existing,int expected_destroy,int expected_create,int expected_update,int expected_run){
    ffb_physics=model;ffb_software=software;ffb_zero=dry;
    creates=updates=runs=stops=destroys=0;memset(created_type,0,sizeof created_type);
    SDL_Haptic fake={0};SDL_Haptic*haptic=&fake;
    bool physics=active;int pos=0;uint64_t pass_us=4000;
    LogiEffectParams want[16]={0};bool used[16]={0},playing[16]={0},dirty[16]={0},running[16]={0};
    int sdl_id[16];Sint16 spring_centre[16]={0};Uint16 wave_sent[16]={0};
    float wave_avg[16]={0};bool wave_avg_set[16]={0};uint64_t wave_held_us[16]={0};
    const int types[]={LOGI_CONSTANT,LOGI_SPRING,LOGI_DAMPER,LOGI_PERIODIC,LOGI_PERIODIC};
    for(int i=0;i<16;i++)sdl_id[i]=-1;
    for(int i=0;i<5;i++){want[i].type=types[i];want[i].a=64;want[i].c=i==3?82:30;used[i]=playing[i]=dirty[i]=true;sdl_id[i]=existing?i:-1;}
'''
suffix = r'''
    if(destroys!=expected_destroy||creates!=expected_create||updates!=expected_update||runs!=expected_run){
        printf("FAIL %s destroy=%d create=%d update=%d run=%d\n",name,destroys,creates,updates,runs);return 1;
    }
    if(model&&(created_type[LOGI_CONSTANT]||created_type[SDL_HAPTIC_SPRING]||created_type[LOGI_DAMPER])){puts("FAIL hardware steering effect created in physics model");return 1;}
    printf("PASS %s\n",name);return 0;
}
int main(void){int fail=0;
    fail+=scenario("intro creates no effects",true,false,false,false,false,0,0,0,0);
    fail+=scenario("pause destroys guest effects without restarting",true,false,false,false,true,5,0,0,0);
    fail+=scenario("controlled physics car retains only guest periodic effects",true,true,false,false,true,3,0,2,2);
    fail+=scenario("controlled physics car creates only guest periodic effects",true,true,false,false,false,0,2,0,2);
    fail+=scenario("original model retains its five effects",false,false,false,false,false,0,5,0,5);
    fail+=scenario("software model retains damper and two waves",false,false,true,false,true,2,0,3,3);
    fail+=scenario("zero-force mode destroys all guest effects",true,true,false,true,true,5,0,0,0);
    return fail?1:0;
}
'''
out = root / 'out/ffb-menu-routing-test'
out.mkdir(parents=True, exist_ok=True)
cfile = out / 'actual_routing_loop.c'
cfile.write_text(prefix + loop + suffix)
exe = out / 'menu-routing-test.exe'
compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
if not compiler:
    raise SystemExit('Set CC to a C compiler, or put clang/cc on PATH.')
subprocess.run([compiler, str(cfile), '-Wall', '-Wextra', '-Werror', '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
