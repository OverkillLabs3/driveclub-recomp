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
static unsigned native_spring_calls;
static ABI float native_spring(void *self,float dt) {
    ++native_spring_calls;float f[11];memcpy(f,self,44);unsigned maximum;memcpy(&maximum,(char *)self+12,4);
    unsigned n=(unsigned)(dt/f[7])+1;if(n>maximum)n=maximum;
    if(n){float step=dt/n;for(unsigned i=0;i<n;++i){float accel=-f[6]*f[5]-f[1]*f[4];f[4]+=accel*step;f[5]+=f[4]*step;}}
    if(f[5]>f[2]){f[5]=f[2];f[4]=0;}else if(f[5]<-f[2]){f[5]=-f[2];f[4]=0;}
    memcpy((char *)self+16,f+4,8);return f[5];
}
static void init(float *state,float x,float v) {
    memset(state,0,44);state[1]=9.42477798461914f;state[2]=10;unsigned maximum=8;memcpy(state+3,&maximum,4);
    state[4]=v;state[5]=x;state[6]=22.20661163330078f;state[7]=1.0f/12;state[9]=1;
}
static void gateway(size_t offset,const unsigned char *prefix,unsigned n,const unsigned char *unwind,unsigned count,uintptr_t target) {
    unsigned char *p=image+offset;memcpy(p,prefix,n);p+=n;memcpy(p,unwind,count);p+=count;
    const unsigned char jump[]={0xff,0x25,0,0,0,0};memcpy(p,jump,6);memcpy(p+6,&target,8);
}
static void callsite(uintptr_t start,size_t force_return,size_t spring_return) {
    unsigned char *p=image+start;memset(p,0x90,80);
    /* Save self and dt in callee-saved registers across both SysV calls. */
    const unsigned char entry[]={0x53,0x41,0x54,0x48,0x83,0xec,0x08,0x49,0x89,0xfc,0x66,0x0f,0x7e,0xcb};
    memcpy(p,entry,sizeof entry);p=image+force_return-5;*p++=0xe8;int32_t relative=(int32_t)(0x6ce740-force_return);memcpy(p,&relative,4);
    p=image+force_return;const unsigned char args[]={0x4c,0x89,0xe7,0x66,0x0f,0x6e,0xc3};memcpy(p,args,sizeof args);
    p=image+spring_return-5;*p++=0xe8;relative=(int32_t)(0x6ce5b0-spring_return);memcpy(p,&relative,4);
    const unsigned char tail[]={0x48,0x83,0xc4,0x08,0x41,0x5c,0x5b,0xc3};memcpy(image+spring_return,tail,sizeof tail);
}
int main(void) {
    double out[2],c=9.42477798461914,k=22.20661163330078,T=1.0/30;
    for(unsigned hz=60;hz<=240;hz+=30){double x=.2,v=.1;for(unsigned i=0;i<hz;++i){assert(camera_reference_step(x,v,3,1.0/hz,c,k,out));x=out[0];v=out[1];}
        double rx=.2,rv=.1;for(unsigned i=0;i<30;++i){rv=(1-c*T)*(rv+3*T)-k*T*rx;rx+=T*rv;}assert(fabs(x-rx)<1e-10 && fabs(v-rv)<1e-10);}
    const double extra_cases[2][2]={{.5,200},{10.8,36}};
    for(unsigned which=0;which<2;++which){double dc=extra_cases[which][0],dk=extra_cases[which][1],x=.2,v=.1,rx=x,rv=v;
        for(unsigned i=0;i<120;++i){assert(camera_reference_step(x,v,3,1.0/120,dc,dk,out));x=out[0];v=out[1];}
        for(unsigned i=0;i<30;++i){rv=(1-dc*T)*(rv+3*T)-dk*T*rx;rx+=T*rv;}assert(fabs(x-rx)<1e-10 && fabs(v-rv)<1e-10);}
    assert(!camera_reference_step(0,0,1,0,c,k,out));assert(!camera_reference_step(0,0,1,.01,40,k,out));
    runtime_image_size=0xd30000;image=VirtualAlloc(NULL,runtime_image_size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);assert(image);runtime_image_base=(uintptr_t)image;
    const unsigned char spring[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50};
    const unsigned char unwind[]={0x48,0x83,0xc4,0x08,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d};
    gateway(0x6ce5b0,spring,sizeof spring,unwind,sizeof unwind,(uintptr_t)native_spring);
    const unsigned char force[]={0xc5,0xfa,0x59,0xc1,0xc5,0xfa,0x58,0x47,0x10,0xc5,0xfa,0x11,0x47,0x10,0xc3};memcpy(image+0x6ce740,force,sizeof force);
    callsite(0xd1c3c0,0xd1c3dd,0xd1c3ed);callsite(0xd1c40e,0xd1c42b,0xd1c43b);
    _putenv("BB_CAMERA_REFERENCE_SPRING_STRENGTH=1");_putenv("BB_CAMERA_REFERENCE_SPRING_FIX=1");_putenv("BB_CAMERA_DYNAMICS_TRACE=");camera_dynamics_install();camera_reference_strength=1;assert(camera_reference_force_original && camera_dynamics_spring_original);
    _Alignas(16) unsigned char camera[0x600]={0};camera_dynamics_self=camera;
    const unsigned offsets[]={0x538,0x50c},sites[]={0xd1c3c0,0xd1c40e};
    for(unsigned axis=0;axis<2;++axis){float state[11];init(state,-.1f,.2f);memcpy(camera+offsets[axis],state,44);
        float dt=1.0f/120;assert(camera_reference_step(state[5],state[4],3,dt,state[1],state[6],out));
        unsigned before_calls=native_spring_calls;float result=((CameraReferenceForce)(image+sites[axis]))(camera+offsets[axis],3,dt);
        float actual[11];memcpy(actual,camera+offsets[axis],44);assert(native_spring_calls==before_calls+1 && result==actual[5]);assert(fabs(actual[5]-out[0])<1e-7 && fabs(actual[4]-out[1])<1e-7);
        memcpy(state+4,actual+4,8);assert(!memcmp(state,actual,44));
        /* Native 30 FPS bypass remains bit-identical through both real gateways. */
        init(state,-.1f,.2f);memcpy(camera+offsets[axis],state,44);
        ((CameraReferenceForce)(image+0x6ce740))(state,3,1.0f/30);float expected=camera_dynamics_spring_original(state,1.0f/30);
        result=((CameraReferenceForce)(image+sites[axis]))(camera+offsets[axis],3,1.0f/30);
        assert(result==expected && !memcmp(state,camera+offsets[axis],44));
        /* Unsupported active noise uses the exact native path. */
        init(state,-.1f,.2f);state[8]=1;memcpy(camera+offsets[axis],state,44);
        ((CameraReferenceForce)(image+0x6ce740))(state,3,dt);expected=camera_dynamics_spring_original(state,dt);
        result=((CameraReferenceForce)(image+sites[axis]))(camera+offsets[axis],3,dt);assert(result==expected && !memcmp(state,camera+offsets[axis],44));
    }
    float other[11];init(other,-.1f,.2f);memcpy(camera+0x480,other,44);
    ((CameraReferenceForce)(image+0x6ce740))(other,3,1.0f/120);float native_result=camera_dynamics_spring_original(other,1.0f/120);
    float result=((CameraReferenceForce)(image+0xd1c3c0))(camera+0x480,3,1.0f/120);
    assert(result==native_result && !memcmp(other,camera+0x480,44));
    /* Intermediate strength through actual native gateways: endpoints and sustained response. */
    camera_reference_strength=.5f;
    for(unsigned axis=0;axis<2;++axis) {
        float state[11],native[11];init(state,-.1f,.2f);memcpy(native,state,44);memcpy(camera+offsets[axis],state,44);
        float dt=1.0f/120;
        assert(camera_reference_step(state[5],state[4],3,dt,state[1],state[6],out));
        ((CameraReferenceForce)(image+0x6ce740))(native,3,dt);camera_dynamics_spring_original(native,dt);
        float result=((CameraReferenceForce)(image+sites[axis]))(camera+offsets[axis],3,dt);
        float actual[11];memcpy(actual,camera+offsets[axis],44);
        assert(fabs(actual[5]-(native[5]+(float)out[0])*.5)<1e-7);
        assert(fabs(actual[4]-(native[4]+(float)out[1])*.5)<1e-7 && result==actual[5]);
        memcpy(state+4,actual+4,8);assert(!memcmp(state,actual,44));
        camera_reference_strength=0;init(state,-.1f,.2f);memcpy(camera+offsets[axis],state,44);
        ((CameraReferenceForce)(image+0x6ce740))(state,3,dt);float expected=camera_dynamics_spring_original(state,dt);
        result=((CameraReferenceForce)(image+sites[axis]))(camera+offsets[axis],3,dt);
        assert(result==expected && !memcmp(state,camera+offsets[axis],44));
        camera_reference_strength=.5f;
    }
    for(unsigned hz=60;hz<=240;hz+=30) {
        float final_x[3];
        for(unsigned blend=0;blend<3;++blend) {
            camera_reference_strength=blend*.5f;float state[11];init(state,0,0);memcpy(camera+0x538,state,44);
            for(unsigned i=0;i<hz*5;++i)((CameraReferenceForce)(image+0xd1c3c0))(camera+0x538,3,1.0f/hz);
            memcpy(&final_x[blend],camera+0x538+0x14,4);
        }
        assert(final_x[2]<final_x[1] && final_x[1]<final_x[0]);
        double native_eq=(1-c/hz)*3/k,ref_eq=(1-c/30)*3/k;
        assert(fabs(final_x[0]-native_eq)<1e-5 && fabs(final_x[2]-ref_eq)<1e-5);
    }
    puts("PASS: strength 0 gives bit-identical native state; 0.5 midpoint per update on both springs; 1 native30 constant-force response; sustained 60..240Hz response strictly between endpoints; authored parameters unchanged.");
    return 0;
}
