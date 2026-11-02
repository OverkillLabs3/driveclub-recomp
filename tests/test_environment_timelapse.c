/* Host fixtures exercise the actual optional module. No game is launched. */
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define ABI __attribute__((sysv_abi))
static unsigned char *image;
static size_t runtime_image_size;
static unsigned allocations,frees,protections,fail_alloc,fail_protect;
static int plus_key,minus_key,shift_key,ctrl_key,focused=1;
static void *native_alloc(size_t size){return VirtualAlloc(NULL,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);}
static void *test_alloc(void*a,SIZE_T n,DWORD t,DWORD p){++allocations;if(allocations==fail_alloc)return NULL;return VirtualAlloc(a,n,t,p);}
static BOOL test_free(void*a,SIZE_T n,DWORD f){++frees;return VirtualFree(a,n,f);}
static BOOL test_protect(void*a,SIZE_T n,DWORD p,PDWORD old){++protections;if(protections==fail_protect)return FALSE;return VirtualProtect(a,n,p,old);}
static HWND test_foreground(void){return(HWND)(uintptr_t)1;}
static DWORD test_window_pid(HWND w,LPDWORD pid){(void)w;*pid=GetCurrentProcessId()+(focused?0:1);return 1;}
static SHORT test_key(int k){int down=k==VK_ADD||k==VK_OEM_PLUS?plus_key:k==VK_SUBTRACT||k==VK_OEM_MINUS?minus_key:k==VK_SHIFT?shift_key:k==VK_CONTROL?ctrl_key:0;return down?(SHORT)0x8000:0;}
#define VirtualAlloc test_alloc
#define VirtualFree test_free
#define VirtualProtect test_protect
#define GetForegroundWindow test_foreground
#define GetWindowThreadProcessId test_window_pid
#define GetAsyncKeyState test_key
#include "../src/runtime_environment_timelapse.inc"
#include "../src/runtime_scene_debug.inc"
#include "../gpu/shim/bbport_scene_clock.h"
static void set_scale(double x){env_store_double(&env_frame_scale_bits,x);}
static const unsigned char time_sig[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xf8,0,0,0};
static void fill_image(void){
 runtime_image_size=0x1a00000;image=native_alloc(runtime_image_size);assert(image);
 for(unsigned i=0;i<sizeof env_hooks/sizeof*env_hooks;i++)memcpy(image+env_hooks[i].offset,env_hooks[i].signature,env_hooks[i].size);
 memcpy(image+0x75a00,time_sig,sizeof time_sig);
}
static void entries_original(void){for(unsigned i=0;i<sizeof env_hooks/sizeof*env_hooks;i++){EnvHook*h=env_hooks+i;assert(!memcmp(image+h->offset,h->signature,h->size));}}
static void slots_empty(void){for(unsigned i=0;i<sizeof env_hooks/sizeof*env_hooks;i++){uintptr_t ptr=1;memcpy(&ptr,env_hooks[i].original,sizeof ptr);assert(!ptr&&!env_hooks[i].code);}}
static void input_case(int plus,int minus,int shift,int ctrl,double expected){
 plus_key=plus;minus_key=minus;shift_key=shift;ctrl_key=ctrl;
 float dt=env_time_delta((void*)1,.01f);assert(fabs(dt-.01*expected)<.00002);assert(fabs(env_load_double(&env_frame_scale_bits)-expected)<.0001);
}
static float seen_time;static unsigned time_calls;static double car_clock=123.5;
static void ABI fake_time(void*self,float dt){++time_calls;seen_time=dt;*(float*)((char*)self+0xc)+=dt;}
static int test_input(void){
 fill_image();_putenv_s("BB_SCENE_OVERLAY","0");_putenv_s("BB_ENV_TIMELAPSE","");env_timelapse_install();scene_debug_install();assert(!env_hooks_ready&&!env_timelapse_enabled&&!allocations);entries_original();assert(!memcmp(image+0x75a00,time_sig,sizeof time_sig));
 _putenv_s("BB_ENV_TIMELAPSE","trace");env_timelapse_install();assert(!env_hooks_ready&&!allocations);entries_original();
 _putenv_s("BB_ENV_TIMELAPSE","1");image[env_hooks[5].offset]^=1;env_timelapse_install();assert(!env_timelapse_enabled&&!allocations);image[env_hooks[5].offset]^=1;entries_original();
 fail_alloc=3;env_timelapse_install();assert(!env_hooks_ready&&!env_timelapse_enabled&&frees==2);entries_original();slots_empty();
 allocations=frees=protections=fail_alloc=0;fail_protect=3;env_timelapse_install();assert(!env_hooks_ready&&!env_timelapse_enabled&&allocations==3&&frees==3);entries_original();slots_empty();
 allocations=frees=protections=fail_protect=0;env_timelapse_install();assert(env_hooks_ready&&!env_timelapse_enabled);image[0x75a00]^=1;scene_debug_install();assert(!env_hooks_ready&&!env_timelapse_enabled);entries_original();slots_empty();image[0x75a00]^=1;
 allocations=frees=protections=0;env_timelapse_install();assert(env_hooks_ready&&!env_timelapse_enabled);fail_alloc=allocations+1;scene_debug_install();assert(!env_hooks_ready&&!env_timelapse_enabled);entries_original();slots_empty();fail_alloc=0;
 allocations=frees=protections=0;_putenv_s("BB_ENV_TIMELAPSE_RATE","600junk");env_timelapse_install();assert(env_hooks_ready&&!env_timelapse_enabled&&env_hold_rate==600);scene_debug_install();assert(env_timelapse_enabled&&scene_time_original);
 for(unsigned i=0;i<sizeof env_hooks/sizeof*env_hooks;i++){EnvHook*h=env_hooks+i;uintptr_t ptr=0;memcpy(&ptr,h->original,8);assert(ptr==(uintptr_t)h->code&&!memcmp(h->code,h->signature,h->size));assert(!memcmp(h->code+h->size,"\xff\x25\0\0\0\0",6));uintptr_t resume=0;memcpy(&resume,h->code+h->size+6,8);assert(resume==(uintptr_t)image+h->offset+h->size);MEMORY_BASIC_INFORMATION m;assert(VirtualQuery(h->code,&m,sizeof m)&&m.Protect==PAGE_EXECUTE_READ);}
 unsigned char physics_before[64];memcpy(physics_before,image+0x663040,64);
 input_case(0,0,0,0,1);input_case(1,0,0,0,600);input_case(1,0,1,0,1200);input_case(1,0,0,1,300);input_case(1,0,1,1,600);input_case(0,1,0,1,-300);input_case(0,1,1,0,-1200);input_case(1,1,0,0,1);input_case(0,0,0,0,1);
 focused=0;input_case(1,0,1,0,1);focused=1;env_store_double(&env_cursor_bits,.1);input_case(0,1,0,0,-10);input_case(0,1,0,0,0);input_case(0,0,0,0,1);
 env_store_double(&env_cloud_oldest_bits,500);env_store_double(&env_light_oldest_bits,400);env_store_double(&env_cursor_bits,1000);plus_key=minus_key=0;float step=env_time_delta((void*)2,.01f);assert(step>0&&step<.1f&&env_load_double(&env_cursor_bits)<.1);
 plus_key=1;env_time_delta((void*)2,.01f);assert(env_action==1);assert(env_time_delta((void*)2,.5f)==.5f&&env_action==0&&env_load_double(&env_frame_scale_bits)==1);assert(isnan(env_time_delta((void*)2,NAN))&&env_action==0);
 unsigned char self[128]={0};*(float*)(self+0xc)=12;*(float*)(self+0x10)=2;scene_time_original=fake_time;
 const struct {int plus,minus,shift,ctrl;float rate;const char *label;} display[]={
   {1,0,0,0,1200,"Time Compression: 1200x"},
   {1,0,1,0,2400,"Time Compression: 2400x"},
   {1,0,0,1,600,"Time Compression: 600x"},
   {0,1,0,0,-1200,"Time Compression: -1200x"},
   {0,1,1,0,-2400,"Time Compression: -2400x"},
   {0,1,0,1,-600,"Time Compression: -600x"},
   {0,0,0,0,2,"Time Compression: 2x"}
 };
 float hour,compression;char label[64];
 for(unsigned i=0;i<sizeof display/sizeof *display;i++){
   plus_key=display[i].plus;minus_key=display[i].minus;shift_key=display[i].shift;ctrl_key=display[i].ctrl;
   env_store_double(&env_cursor_bits,1000);scene_time_update(self,.01f);
   assert(time_calls==i+1&&fabsf(seen_time-.01f*display[i].rate/2)<.00001f&&car_clock==123.5);
   assert(runtime_scene_debug_time(&hour,&compression)&&fabsf(compression-display[i].rate)<.001f);
   bbport_scene_compression_text(label,sizeof label,compression,1);assert(!strcmp(label,display[i].label));
 }
 /* A partial final reverse step reports the actual clamped delta, then zero. */
 plus_key=shift_key=ctrl_key=0;minus_key=1;env_store_double(&env_cursor_bits,.1);
 scene_time_update(self,.01f);assert(runtime_scene_debug_time(&hour,&compression)&&fabsf(compression+20)<.001f);
 bbport_scene_compression_text(label,sizeof label,compression,1);assert(!strcmp(label,"Time Compression: -20x"));
 scene_time_update(self,.01f);assert(runtime_scene_debug_time(&hour,&compression)&&compression==0);
 bbport_scene_compression_text(label,sizeof label,compression,1);assert(!strcmp(label,"Time Compression: 0x"));
 /* The published sample remains coherent if the next keyboard sample changes. */
 env_action=1;set_scale(1200);assert(runtime_scene_debug_time(&hour,&compression)&&compression==0);
 minus_key=0;scene_time_update(self,.01f);assert(runtime_scene_debug_time(&hour,&compression)&&compression==2);
 bbport_scene_compression_text(label,sizeof label,432000,1);assert(!strcmp(label,"Time Compression: 432000x"));
 bbport_scene_compression_text(label,sizeof label,NAN,1);assert(!strcmp(label,"Time Compression: --"));
 bbport_scene_compression_text(label,sizeof label,-INFINITY,1);assert(!strcmp(label,"Time Compression: --"));
 bbport_scene_compression_text(label,sizeof label,-1,0);assert(!strcmp(label,"Time Compression: --"));
 bbport_scene_compression_text(label,sizeof label,-1,1);assert(!strcmp(label,"Time Compression: -1x"));
 bbport_scene_compression_text(label,sizeof label,2,0);assert(!strcmp(label,"Time Compression: 2x"));
 assert(!memcmp(physics_before,image+0x663040,64));
 env_timelapse_time_ready(0);entries_original();slots_empty();assert(!env_timelapse_enabled);
 puts("PASS: opt-in, complete startup rollback, RX gateways, keys/modifiers/focus/release/bounds/reset/invalid-dt, actual compression, physics entry unchanged");return 0;
}

static unsigned char engines[3][0x1a00];
static float seen[3];static unsigned char objects[3][0x100];
static uintptr_t ABI fake_load(void *self,const char *asset,int kind){return !strcmp(asset,"ok")?0x12340001:0x12340000;}
static uintptr_t ABI fake_unload(void *self){return 0x12345678;}
static void ABI fake_animate(void *self,float dt){for(int i=0;i<3;i++)if(self==objects[i]){seen[i]=dt;*(float *)(engines[i]+0x19c8)+=dt;}}
static void ABI fake_manager(void *self,float dt) {
 for(int i=0;i<3;i++){
   void *object=objects[i];void *context[2]={0,&dt};unsigned char job[0x28]={0};
   *(void **)(job+0x18)=&object;*(void **)(job+0x20)=context;
   assert(env_animated_job(job)==0);
 }
}
static int test_groups(void){
 env_timelapse_enabled=1;
 image=calloc(1,0x1920000);assert(image);
 unsigned char *root=calloc(1,0x1000),*services=calloc(1,0x700);
 *(void **)(image+0x1911100)=root;*(void **)(root+0x970)=services;
 uintptr_t *vt=(uintptr_t *)(image+0x1652b30);vt[2]=(uintptr_t)fake_animate;
 for(int i=0;i<3;i++){*(void **)objects[i]=vt;*(void **)(objects[i]+0xe0)=engines[i];*(float *)(objects[i]+0xb4)=1;*(float *)(engines[i]+0x19c8)=20;}
 env_animation_load_original=fake_load;env_animation_unload_original=fake_unload;
 env_animated_manager_original=fake_manager;
 assert(env_animation_load(objects[0],"ok",9)==0x12340001);
 assert(env_animation_load(objects[1],"ok",4)==0x12340001);
 assert(env_animation_load(objects[2],"ok",9)==0x12340001);
 env_action=1;set_scale(600);env_animated_manager_update(NULL,.01f);
 assert(env_crowd_count==2);assert(fabsf(seen[0]-6)<1e-5f&&fabsf(seen[2]-6)<1e-5f&&seen[1]==.01f);
 env_action=-1;set_scale(-600);env_animated_manager_update(NULL,.01f);
 assert(fabsf(seen[0]+6)<1e-5f&&seen[1]==.01f);assert(env_group_step(.01f,0)==0);
 set_scale(0);env_animated_manager_update(NULL,.01f);assert(seen[0]==0&&seen[1]==.01f);
 env_action=0;env_animated_manager_update(NULL,.01f);assert(seen[0]==.01f&&seen[1]==.01f);
 assert(env_animation_unload(objects[0])==0x12345678);env_refresh_crowd();assert(env_crowd_count==1);
 assert(env_animation_load(objects[2],"fail",9)==0x12340000);env_refresh_crowd();assert(env_crowd_count==0);
 env_animation_load(objects[2],"ok",9);
 env_action=-1;set_scale(-600);*(float *)(engines[2]+0x19c8)=.25f;
 env_animated_manager_update(NULL,.01f);assert(seen[2]==-.25f&&*(float *)(engines[2]+0x19c8)==0);
 env_action=1;set_scale(600);env_animation_load(objects[2],"ok",4);
 env_animated_manager_update(NULL,.01f);assert(seen[2]==.01f);
 env_animation_load(objects[2],"ok",9);env_timelapse_enabled=0;env_action=1;set_scale(600);
 env_animated_manager_update(NULL,.01f);assert(seen[2]==.01f);

 puts("PASS: scene-class selection, vehicle exclusion, +/-/release, rewind boundary, load failure, unload/reuse, disabled passthrough, native returns");
 return 0;
}

static unsigned char sky[0x700],wrapper[0x40],cloud_objects[3][0x800],keys[3][5][0x90];
static uintptr_t seq[3],ptr[3][5];static unsigned dirty,produced;
static void ABI fake_dirty(void *a){dirty++;}
static void ABI fake_remove_atmos(void *a,int idx){uintptr_t q=(uintptr_t)a+0x10;*(uintptr_t *)(q+0x28)-=8;}
static void ABI fake_remove_cloud(void *a,int idx){uintptr_t q=(uintptr_t)a+8;*(uintptr_t *)(q+0x28)-=8;}
static int ABI fake_index(void *s,float t){uintptr_t b=*(uintptr_t *)((char *)s+0x20),e=*(uintptr_t *)((char *)s+0x28);int n=-1;for(uintptr_t i=b;i<e;i+=8)if(*(float *)(*(uintptr_t *)i+8)<=t)n=(int)((i-b)/8);return n;}
static void ABI fake_produce(void *s){produced++;for(int g=0;g<3;g++){if(*(int *)(seq[g]+0x4c)<=0)continue;for(int j=0;j<4;j++){*(float *)(keys[g][j]+8)+=1;keys[g][j][0x20]++;}}}
static void cloud_jump(size_t off,void *fn){unsigned char op[14]={0xff,0x25};memcpy(op+6,&fn,8);memcpy(image+off,op,14);}
static int test_clouds(void){
 env_timelapse_enabled=1;env_generation=1;
 image=VirtualAlloc(NULL,0x900000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);assert(image);
 cloud_jump(0x867420,fake_dirty);cloud_jump(0x86e630,fake_remove_atmos);cloud_jump(0x889a60,fake_remove_cloud);cloud_jump(0x85f470,fake_index);cloud_jump(0x602720,fake_produce);
 *(uintptr_t *)(sky+0x50)=(uintptr_t)wrapper;*(uintptr_t *)(wrapper+0x18)=(uintptr_t)cloud_objects[0];*(uintptr_t *)(wrapper+8)=(uintptr_t)cloud_objects[1];*(uintptr_t *)(wrapper+0x10)=(uintptr_t)cloud_objects[2];
 for(int g=0;g<3;g++){seq[g]=(uintptr_t)cloud_objects[g]+(g?8:0x10);*(uintptr_t *)(seq[g]+0x20)=(uintptr_t)ptr[g];*(uintptr_t *)(seq[g]+0x28)=(uintptr_t)(ptr[g]+4);for(int j=0;j<5;j++){ptr[g][j]=(uintptr_t)keys[g][j];*(float *)(keys[g][j]+8)=(float)j;keys[g][j][0x20]=(unsigned char)(g*10+j);*(uintptr_t *)keys[g][j]=0x12345678;}cloud_objects[g][0x7f0]=99;}
 env_store_double(&env_cursor_bits,100.25);env_sky_prepare(sky,100.25f);assert(produced==100&&env_sky_count==101);assert(*(float *)(keys[1][0]+8)==100);
 env_action=-1;env_store_double(&env_cursor_bits,25.25);env_sky_prepare(sky,25.25f);assert(*(float *)(keys[1][0]+8)==25&&keys[1][0][0x20]==35&&dirty==1);
 for(int g=0;g<3;g++){assert(*(uintptr_t *)(seq[g]+0x20)==(uintptr_t)ptr[g]);assert(cloud_objects[g][0x7f0]==99);for(int j=0;j<4;j++)assert(*(uintptr_t *)keys[g][j]==0x12345678);}
 env_action=0;env_store_double(&env_cursor_bits,26.25);env_sky_prepare(sky,26.25f);assert(env_sky_at(env_sky_count-1)->anchor==26);
 env_store_double(&env_cursor_bits,3000.25);env_sky_prepare(sky,3000.25f);env_store_double(&env_cursor_bits,6000.25);env_sky_prepare(sky,6000.25f);env_store_double(&env_cursor_bits,9000.25);env_sky_prepare(sky,9000.25f);assert(env_sky_count==8192&&env_load_double(&env_cloud_oldest_bits)>0);
 env_generation++;env_store_double(&env_cursor_bits,9000.5);env_sky_prepare(sky,9000.5f);assert(env_sky_count==1);
 free(env_sky_history);VirtualFree(image,0,MEM_RELEASE);puts("PASS: native producer catch-up, reverse keys, branch, bounded history, lifecycle, live pointers and unrelated state preserved");
 return 0;
}

static unsigned char controller[0x100],geometry[0x10200],inner[0x40];
static unsigned char nodes[8][0x50];static uintptr_t pool[8];static unsigned char draw[640];
static void ABI native_update(void *self,float dt){if(dt>0){*(float *)(geometry+0x48)+=dt;controller[0x10]++;}}
static int test_lightning(void){
 env_timelapse_enabled=1;env_generation=1;
 image=calloc(1,0x1060000);assert(image);
 *(void **)(inner+0x28)=controller;*(void **)(controller+0xd0)=geometry;
 *(uintptr_t *)(geometry+0x100)=(uintptr_t)draw;*(uintptr_t *)(geometry+0x108)=(uintptr_t)draw+40;*(uintptr_t *)(geometry+0x110)=(uintptr_t)draw+sizeof draw;
 *(uintptr_t *)(geometry+0xd8)=(uintptr_t)pool;*(uint64_t *)(geometry+0xe0)=8;*(uint64_t *)(geometry+0xe8)=2;*(uint64_t *)(geometry+0xf0)=2;
 for(unsigned i=0;i<8;i++){pool[i]=(uintptr_t)nodes[i];nodes[i][0]=(unsigned char)(10+i);}
 *(unsigned *)(geometry+0x40)=1;*(unsigned *)(geometry+0x44)=2;*(unsigned *)(geometry+0x10120)=2;
 memset(geometry+0x120,42,32);memset(draw,50,40);
 EnvLightSnapshot *s=env_light_capture((uintptr_t)controller,(uintptr_t)geometry,1);assert(s&&s->nodes==2&&s->points==2&&s->draw_bytes==40);
 geometry[0x120]=99;draw[0]=99;nodes[2][0]=99;controller[0x10]=55;controller[0x35]=1;
 uintptr_t livevec=*(uintptr_t *)(geometry+0x100),livepool=*(uintptr_t *)(geometry+0xd8);
 assert(env_light_restore(s,(uintptr_t)controller,(uintptr_t)geometry));assert(geometry[0x120]==42&&draw[0]==50&&nodes[0][0]==12&&nodes[1][0]==13&&controller[0x10]==0&&controller[0x35]==0);
 assert(*(uintptr_t *)(geometry+0x100)==livevec&&*(uintptr_t *)(geometry+0xd8)==livepool&&*(uint64_t *)(geometry+0xe0)==8);
 free(s);env_light_original=native_update;
 env_store_double(&env_cursor_bits,1);env_light_update(inner,.01f);assert(env_light_count==1);
 env_store_double(&env_cursor_bits,2);env_light_update(inner,.01f);assert(env_light_count==2);
 env_action=-1;env_store_double(&env_cursor_bits,1);env_light_update(inner,.01f);assert(fabsf(*(float *)(geometry+0x48)-.01f)<1e-5f);
 env_action=0;env_store_double(&env_cursor_bits,1.5);env_light_update(inner,.01f);assert(env_light_count==2&&env_light_at(1)->time==1.5);
 env_light_clear();assert(env_light_count==0&&env_light_bytes==0);
 puts("PASS: lightning scalar/geometry/queue restoration, live resource pointers, audio suppression, seek/branch/lifecycle");return 0;
}

static int test_wind(void){env_timelapse_enabled=1;unsigned char block[624],before[624];memset(block,0xa5,sizeof block);float *clock=(float *)(block+0x248);*clock=10;
memcpy(before,block,624);env_store_double(&env_cursor_bits,600);env_store_double(&env_native_elapsed_bits,10);env_shader_clock_map(block);assert(*clock==600);
assert(!memcmp(block,before,0x248));assert(!memcmp(block+0x24c,before+0x24c,624-0x24c));
*clock=11;env_store_double(&env_cursor_bits,601);env_store_double(&env_native_elapsed_bits,11);env_shader_clock_map(block);assert(*clock==601);
*clock=12;env_store_double(&env_cursor_bits,300);env_store_double(&env_native_elapsed_bits,12);env_shader_clock_map(block);assert(*clock==300);
*clock=13;env_timelapse_enabled=0;env_shader_clock_map(block);assert(*clock==13);env_timelapse_enabled=1;
*clock=NAN;env_shader_clock_map(block);assert(isnan(*clock));*clock=14;env_store_double(&env_cursor_bits,NAN);env_shader_clock_map(block);assert(*clock==14);
env_shader_clock_map(NULL);puts("PASS: common shader clock forward/reverse/release, disabled/invalid guards; all unrelated render bytes preserved");return 0;}

int main(int argc,char**argv){if(argc!=2)return 2;if(!strcmp(argv[1],"input"))return test_input();if(!strcmp(argv[1],"groups"))return test_groups();if(!strcmp(argv[1],"clouds"))return test_clouds();if(!strcmp(argv[1],"lightning"))return test_lightning();if(!strcmp(argv[1],"wind"))return test_wind();return 2;}
