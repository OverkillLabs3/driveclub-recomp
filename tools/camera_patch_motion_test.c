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
static unsigned impulses, updates;
static unsigned noise_updates, test_noise;
static ABI float impulse_mock(void *self,float magnitude) {
    ++impulses; float v; memcpy(&v,(char *)self+16,4); v+=magnitude; memcpy((char *)self+16,&v,4); return v;
}
static ABI void update_mock(void *self,unsigned index,void *matrix,float dt,float real) {
    assert(index==7 && matrix==(void *)0x12340 && dt==real); ++updates;
    if (test_noise) ((CameraNoiseUpdate)(image+0xce3540))(self,dt);
    else ((CameraRateImpulse)(image+0xce3849))(self,2.0f);
}
static ABI void noise_mock(void *self,float dt) {
    assert(dt==1.0f/30.0f); ++noise_updates;
    ((CameraRateImpulse)(image+0xce3849))(self,2.0f);
    /* Verify simulation receives its last actual pose, never an interpolated pose. */
    float x; memcpy(&x,(char *)self+0x160,4);
    assert(x==(float)(noise_updates-1)); ++x;
    memcpy((char *)self+0x160,&x,4);
}
static unsigned target_calls;
static ABI void target_mock(void *self,unsigned frame,float dt) {
    assert(frame==71 && dt==1.0f/120);
    memcpy((char *)self+0x44,&frame,4);++target_calls;
}
static void entry(size_t offset,const unsigned char *bytes,size_t n,const unsigned char *unwind,size_t u,uintptr_t target) {
    memcpy(image+offset,bytes,n); memcpy(image+offset+n,unwind,u);
    const unsigned char j[]={0xff,0x25,0,0,0,0};
    memcpy(image+offset+n+u,j,6); memcpy(image+offset+n+u+6,&target,8);
}
int main(void) {
    for (unsigned fps=30;fps<=240;fps+=30) {
        double credit=0;unsigned count=0;
        for(unsigned i=0;i<fps*10;++i) count+=camera_noise_ticks(&credit,1.0f/fps);
        assert(count==300);
    }
    double credit=0; assert(camera_noise_ticks(&credit,0)==0);
    assert(camera_noise_ticks(&credit,-1)==0); assert(camera_noise_ticks(&credit,1)==0);
    runtime_image_size=0xcf0000; image=VirtualAlloc(NULL,runtime_image_size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    assert(image); runtime_image_base=(uintptr_t)image;
    const unsigned char update[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xe4,0xc0,0xff,0xff,0xff};
    const unsigned char impulse[]={0x55,0x48,0x89,0xe5,0x53,0x50,0xc5,0xf8,0x28,0xc8,0x48,0x89,0xfb};
    const unsigned char noise[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    const unsigned char u[]={0x48,0x8d,0x65,0xd8,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d};
    const unsigned char v[]={0x48,0x8d,0x65,0xf8,0x5b,0x5d};
    entry(0xcc8820,update,sizeof update,u,sizeof u,(uintptr_t)update_mock);
    entry(0x6ce6f0,impulse,sizeof impulse,v,sizeof v,(uintptr_t)impulse_mock);
    entry(0xce3540,noise,sizeof noise,u,sizeof u,(uintptr_t)noise_mock);
    /* Invoke the patched impulse with the exact high-frequency camera return address. */
    const unsigned char call[]={0x48,0x83,0xec,0x08,0xe8,0,0,0,0,0x48,0x83,0xc4,0x08,0xc3};
    memcpy(image+0xce3849,call,sizeof call); int32_t rel=0x6ce6f0-0xce3852; memcpy(image+0xce384e,&rel,4);
    const unsigned char target_entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x81,0xec,0x88,0,0,0};
    const unsigned char target_unwind[]={0x48,0x8d,0x65,0xe8,0x5b,0x41,0x5e,0x41,0x5f,0x5d};
    entry(0xccd600,target_entry,sizeof target_entry,target_unwind,sizeof target_unwind,(uintptr_t)target_mock);
    _putenv("BB_CAMERA_PATCH=");camera_patch_install();assert(!memcmp(image+0xcc8820,update,sizeof update));
    _putenv("BB_CAMERA_PATCH=0");camera_patch_install();assert(!memcmp(image+0xcc8820,update,sizeof update));
    _putenv("BB_CAMERA_PATCH=1");camera_patch_install();
    assert(camera_rate_update_original && camera_rate_impulse_original);
    unsigned char self[64]={0};
    for(unsigned i=0;i<120;++i) ((CameraRateUpdate)(image+0xcc8820))(self,7,(void *)0x12340,1.0f/120,1.0f/120);
    assert(updates==120 && impulses==30);
    ((CameraRateImpulse)(image+0x6ce6f0))(self,2.0f); assert(impulses==31);
    unsigned char effect[512]={0}; test_noise=1; impulses=updates=0;
    float last=0;
    for(unsigned i=0;i<120;++i) {
        ((CameraRateUpdate)(image+0xcc8820))(effect,7,(void *)0x12340,1.0f/120,1.0f/120);
        float x; memcpy(&x,effect+0x160,4);
        assert(x>=last && x-last<=0.251f); last=x;
    }
    assert(updates==120 && noise_updates==30 && impulses==30);
    unsigned char target_state[1024]={0};
    ((CameraTargetUpdate)(image+0xccd600))(target_state,71,1.0f/120);
    assert(target_calls==1);
    unsigned char motion[1024]={0};
    float t=0;
    for(unsigned i=0;i<240;++i) {
        float dt=i%2 ? 1.0f/100 : 1.0f/150;
        t+=dt;
        /* Exact mean velocity during this interval for constant acceleration. */
        float v=3*(t-dt*0.5f),w=2*(t-dt*0.5f);
        memcpy(motion+0x120,&v,4);memcpy(motion+0x130,&w,4);
        camera_motion_filter(motion,dt,i==0);
        if(t>0.1f) {
            float a,angular;memcpy(&a,motion+0x140,4);memcpy(&angular,motion+0x150,4);
            assert(fabsf(a-3)<0.02f && fabsf(angular-2)<0.02f);
        }
    }
    float sentinel=123;
    memcpy(motion+0x140,&sentinel,4);
    camera_motion_filter(motion,1.0f/30,0);
    float unchanged;memcpy(&unchanged,motion+0x140,4);assert(unchanged==sentinel);
    camera_motion_filter(motion,1.0f/120,1);
    memcpy(&unchanged,motion+0x140,4);assert(unchanged==sentinel);
    memset(motion,0,sizeof motion);unsigned kind=1;memcpy(motion+8,&kind,4);t=0;
    for(unsigned i=0;i<240;++i) {
        float dt=i%2?1.0f/100:1.0f/150;t+=dt;
        float v=3*t,w=2*t;memcpy(motion+0x120,&v,4);memcpy(motion+0x130,&w,4);
        camera_motion_filter(motion,dt,i==0);
        if(t>.1f){float a,b;memcpy(&a,motion+0x140,4);memcpy(&b,motion+0x150,4);assert(fabsf(a-3)<.02f && fabsf(b-2)<.02f);}
    }
    puts("PASS: patch off installs no hooks; full-rate camera; 30Hz procedural noise and interpolated output; event impulse bypass; point and interval velocity derivatives; native30/history-reset bypass.");
    return 0;
}
