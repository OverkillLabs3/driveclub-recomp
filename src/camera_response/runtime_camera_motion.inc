static void lf_shared_q_reset_target(void*);
#include <math.h>
/* Camera-only derivatives over the original 30 Hz observation interval.
   Vehicle physics and camera transforms continue updating at render cadence. */
typedef void (ABI *CameraTargetUpdate)(void *,unsigned,float);
static CameraTargetUpdate camera_target_update_original;
static int camera_motion_point_enabled;
typedef struct { double end; float dt; float v[6]; } CameraMotionSample;
typedef struct {
    void *self;
    double time;
    ULONGLONG last;
    unsigned count;
    CameraMotionSample samples[128];
} CameraMotionHistory;
#include "target-derivative-live.inc"
static void camera_motion_filter(void*self,float dt,int reset){qtarget_filter(self,dt,reset);}
static ABI void camera_target_update(void *self,unsigned frame,float dt) {
    unsigned previous;
    memcpy(&previous,(char *)self+0x44,4);
    int reset=*((unsigned char *)self+0x40)!=0 || previous+1!=frame;
    float audit[24]={0};int trace=camera_accuracy_trace_active();uintptr_t binding_before=0,binding_after=0;
    camera_pose_read((uintptr_t)self+0x28,&binding_before,8);
    if(trace)camera_pose_read((uintptr_t)self+0x140,audit,12);
    if(trace)camera_pose_read((uintptr_t)self+0x150,audit+3,12);
    unsigned tape_kind_before=0;camera_pose_read((uintptr_t)self+8,&tape_kind_before,4);
    camera_target_update_original(self,frame,dt);
    camera_pose_read((uintptr_t)self+0x28,&binding_after,8);
    if(trace)camera_pose_read((uintptr_t)self+0x140,audit+6,12);
    if(trace)camera_pose_read((uintptr_t)self+0x150,audit+9,12);
    audit[18]=(float)reset;audit[19]=(float)(binding_before!=binding_after);
    unsigned tape_kind_after=0;camera_pose_read((uintptr_t)self+8,&tape_kind_after,4);
    if(previous!=frame&&(reset||binding_before!=binding_after||tape_kind_before!=tape_kind_after))lf_shared_q_reset_target(self);
    if(previous!=frame)camera_spring_native_observation(self,frame,dt,reset||binding_before!=binding_after||tape_kind_before!=tape_kind_after,previous+1u==frame);
    if(previous!=frame&&(!camera_accuracy_oracle||qtarget_requested())) camera_motion_filter(self,dt,reset||binding_before!=binding_after||tape_kind_before!=tape_kind_after);
    if(trace){
    camera_pose_read((uintptr_t)self+0x140,audit+12,12);
    camera_pose_read((uintptr_t)self+0x150,audit+15,12);
    audit[20]=(float)frame;unsigned actual_frame=0,desired=0,active=0;
    camera_pose_read((uintptr_t)self+0x44,&actual_frame,4);
    camera_pose_read((uintptr_t)self+4,&desired,4);camera_pose_read((uintptr_t)self+8,&active,4);
    audit[21]=(float)actual_frame;audit[22]=(float)desired;audit[23]=(float)active;
    if(previous!=frame)camera_accuracy_log(2,self,binding_after,previous,dt,audit,24);
    }
}
static void camera_motion_install(void) {
    camera_motion_point_enabled=1;
    static const unsigned char entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x81,0xec,0x88,0,0,0};
    camera_target_update_original=(CameraTargetUpdate)camera_rate_hook(0xccd600,entry,sizeof entry,(uintptr_t)camera_target_update);
    printf("CameraMotion: camera acceleration at reference observation interval: %s\n",camera_target_update_original?"on":"unavailable");
    if(camera_motion_point_enabled)puts("CameraMotion: direct vehicle velocity sampled across 1/30 s; other target types unchanged");
}
