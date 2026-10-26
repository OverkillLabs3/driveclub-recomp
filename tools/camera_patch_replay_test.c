#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#define ABI __attribute__((sysv_abi))
static unsigned char *image;
static uintptr_t runtime_image_base;
static uint64_t runtime_image_size;
#include "../src/runtime_camera_patch.inc"
static void entry(size_t offset,const unsigned char *bytes,size_t n,const unsigned char *unwind,size_t u,uintptr_t target) {
    memcpy(image+offset,bytes,n); memcpy(image+offset+n,unwind,u);
    const unsigned char j[]={0xff,0x25,0,0,0,0};
    memcpy(image+offset+n+u,j,6); memcpy(image+offset+n+u+6,&target,8);
}
static __attribute__((naked)) ABI void recorded_feedback_call(void *frame,void *actor,void *vehicle) {
 __asm__ volatile("pushq %rbp\n pushq %r12\n pushq %rbx\n movq %rdi,%rbp\n movq %rsi,%r12\n movq %rdx,%rbx\n leaq 1f(%rip),%rax\n movq %rax,replay_feedback_resume(%rip)\n jmp replay_feedback_bridge\n 1: popq %rbx\n popq %r12\n popq %rbp\n retq\n");
}
static __attribute__((naked)) ABI void recorded_driver_call(void *frame,void *self,void *vehicle) {
 __asm__ volatile("pushq %rbp\n pushq %r15\n pushq %r14\n movq %rdi,%rbp\n movq %rsi,%r15\n movq %rdx,%r14\n leaq 1f(%rip),%rax\n movq %rax,replay_driver_observation_resume(%rip)\n jmp replay_driver_observation_bridge\n 1: popq %r14\n popq %r15\n popq %rbp\n retq\n");
}
static __attribute__((naked)) ABI void *recorded_hud_call(void *physics,float *output) {
 __asm__ volatile("pushq %r14\n movq %rdi,%r14\n leaq 1f(%rip),%rax\n movq %rax,replay_hud_observation_resume1(%rip)\n jmp replay_hud_observation_bridge1\n 1: vmovups %xmm2,(%rsi)\n popq %r14\n retq\n");
}
int recorded_test_main(void) {
    unsigned char vehicle[0x100]={0},physics[0x20]={0},state[0x500]={0},actor[0x200]={0},driver[0x400]={0},storage[768]={0};
    void *frame=storage+512;uintptr_t p=(uintptr_t)physics;memcpy(vehicle+0x68,&p,8);p=(uintptr_t)state;memcpy(physics+0x10,&p,8);
    float raw[4]={80,90,100,0},out[6],delta[4];memcpy(state+0xb0,raw,16);
    replay_recorded_motion_enabled=1;
    double time=0;
    for(unsigned i=0;i<240;++i){float dt=i%2?1.0f/150:1.0f/100;time+=dt;float target[4]={(float)(3*time),0,0,0},angular[4]={0,(float)(2*time),0,0};
        memcpy(actor+0x40,target,16);memcpy(actor+0x50,angular,16);memcpy((char *)frame-0xb0,&dt,4);
        memcpy((char *)frame-0x50,raw,16);float origin[4]={0};memcpy((char *)frame-0xc0,origin,16);
        unsigned char state_before[sizeof state],actor_before[sizeof actor];memcpy(state_before,state,sizeof state);memcpy(actor_before,actor,sizeof actor);
        recorded_feedback_call(frame,actor,vehicle);
        assert(memcmp(state_before,state,sizeof state)==0 && memcmp(actor_before,actor,sizeof actor)==0);
        if(time>.1){assert(replay_recorded_read((uintptr_t)state,dt,out));assert(fabsf(out[0]-3)<.001 && fabsf(out[4]-2)<.001);}
        memcpy((char *)frame-0xf0,&dt,4);memcpy((char *)frame-0x40,raw,16);memcpy((char *)frame-0x50,raw,16);
        recorded_driver_call(frame,driver,vehicle);memcpy(delta,(char *)frame-0x110,16);
        if(time>.1)assert(fabsf(delta[0]/dt-3)<.001);
        replay_hud_observation.dt=dt;replay_hud_observation.prepared=0;
        assert(recorded_hud_call(physics,out)==state);if(time>.1)assert(fabsf(out[0]-3)<.001);
        assert(memcmp(state_before,state,sizeof state)==0);
    }
    assert(!replay_recorded_read((uintptr_t)state,1.0f/30,out));
    assert(!replay_recorded_read((uintptr_t)state,1.0f/20,out));assert(!replay_recorded_read(0x12345,1.0f/120,out));
    float dt=1.0f/30;memcpy((char *)frame-0xf0,&dt,4);memset(driver+0x130,0,32);memcpy((char *)frame-0x40,raw,16);memcpy((char *)frame-0x50,raw,16);
    recorded_driver_call(frame,driver,vehicle);memcpy(delta,(char *)frame-0x110,16);assert(delta[0]==80);
    replay_hud_observation.dt=dt;replay_hud_observation.prepared=0;recorded_hud_call(physics,out);assert(memcmp(out,raw,16)==0);
    for(unsigned i=0;i<64;++i)replay_recorded_motion[i].stamp=GetTickCount64()-101;
    assert(!replay_recorded_read((uintptr_t)state,1.0f/120,out));
    puts("PASS: replay feedback/driver/HUD native bridges, uneven frame pacing, recorded velocity derivatives, stale/unknown/native30 bypass and no physics writes.");
    return 0;
}
static unsigned gf_updates,gf_getters;
static void *gf_vehicle;static float gf_accel[3],gf_raw[4]={70,80,90,123};
static ABI uintptr_t gf_velocity_tail(void *manager,unsigned id,float *out) {
    assert(manager==(void *)0x1234 && id==17);++gf_getters;memcpy(out,gf_raw,16);return 0xabcdef;
}
static ABI void gf_update_tail(void *self,float dt) {
    ++gf_updates;_Alignas(16) float velocity[4];
    uintptr_t result=((ReplayGForceVelocity)(image+0xd2a70c))(gf_vehicle,velocity);assert(result==0xabcdef && velocity[3]==123);
    float previous[3];memcpy(previous,(char *)self+0x40,12);
    for(unsigned j=0;j<3;++j)gf_accel[j]=(velocity[j]-previous[j])/dt;
    memcpy((char *)self+0x40,velocity,12);

}
int main(void) {
    recorded_test_main();
    runtime_image_size=0x1400000;image=VirtualAlloc(NULL,runtime_image_size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);assert(image);runtime_image_base=(uintptr_t)image;
    const unsigned char update[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x40};
    const unsigned char unwind[]={0x48,0x83,0xc4,0x40,0x5b,0x41,0x5c,0x41,0x5e,0x41,0x5f,0x5d};
    const unsigned char velocity[]={0x48,0x89,0xf0,0x48,0x8b,0x4f,0x70,0x8b,0x77,0x18,0x48,0x89,0xcf,0x48,0x89,0xc2};
    entry(0xd2a650,update,sizeof update,unwind,sizeof unwind,(uintptr_t)gf_update_tail);
    entry(0xc4bae0,velocity,sizeof velocity,NULL,0,(uintptr_t)gf_velocity_tail);
    unsigned char caller[]={0x48,0x83,0xec,0x08,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    uintptr_t target=(uintptr_t)image+0xc4bae0;memcpy(caller+6,&target,8);memcpy(image+0xd2a70c,caller,sizeof caller);
    replay_recorded_motion_enabled=1;replay_gforce_install();assert(replay_gforce_original && replay_gforce_velocity_original);
    unsigned char vehicle[0x100]={0},physics[0x20]={0},state[0x500]={0},before[sizeof state],self[0x80]={0};
    uintptr_t p=(uintptr_t)physics;memcpy(vehicle+0x68,&p,8);p=(uintptr_t)state;memcpy(physics+0x10,&p,8);p=0x1234;memcpy(vehicle+0x70,&p,8);unsigned id=17;memcpy(vehicle+0x18,&id,4);gf_vehicle=vehicle;
    memcpy(before,state,sizeof state);memset(replay_recorded_motion,0,sizeof replay_recorded_motion);
    float dt=1.0f/120;for(unsigned i=0;i<20;++i){float v[6]={i*3.0f/120,i*-2.0f/120,0,0,0,0};replay_recorded_advance((uintptr_t)state,1,dt,v);}
    float prev[4]={50,60,70,456};memcpy(self+0x40,prev,16);
    ((ReplayGForceUpdate)(image+0xd2a650))(self,dt);
    assert(gf_updates==1 && gf_getters==1 && !replay_gforce_scope);
    assert(fabsf(gf_accel[0]-3)<.002 && fabsf(gf_accel[1]+2)<.002 && gf_accel[2]==0);
    assert(!memcmp(self+0x40,gf_raw,12));assert(!memcmp(self+0x4c,prev+3,4));assert(!memcmp(before,state,sizeof state));
    float out[4];assert(((ReplayGForceVelocity)(image+0xc4bae0))(vehicle,out)==0xabcdef);assert(!memcmp(out,gf_raw,16));
    memcpy(self+0x40,prev,16);((ReplayGForceUpdate)(image+0xd2a650))(self,1.0f/30);assert(fabsf(gf_accel[0]-600)<.001);
    for(unsigned i=0;i<64;++i)replay_recorded_motion[i].stamp=GetTickCount64()-101;
    memcpy(self+0x40,prev,16);((ReplayGForceUpdate)(image+0xd2a650))(self,dt);assert(fabsf(gf_accel[0]-2400)<.001);
    assert(!memcmp(before,state,sizeof state));
    replay_gforce_enabled=0;memcpy(self+0x40,prev,16);
    ((ReplayGForceUpdate)(image+0xd2a650))(self,dt);assert(fabsf(gf_accel[0]-2400)<.001);
    assert(!memcmp(self+0x40,gf_raw,12) && !memcmp(before,state,sizeof state));
    puts("PASS: native GForce and velocity gateways, exact caller scope, return/W preservation, recorded acceleration, raw private history restored, 30 FPS/stale bypass, no physics writes.");
    return 0;
}
