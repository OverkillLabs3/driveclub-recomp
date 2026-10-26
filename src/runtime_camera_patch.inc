/* Optional DriveClub 1.28 high-FPS camera/replay workaround, not an accuracy fix.
   No guest hooks are installed unless BB_CAMERA_PATCH=1. Windows x86-64 only. */
#ifdef _WIN32
#include <math.h>
static int camera_pose_read(uintptr_t address,void *out,size_t size) {
    SIZE_T got=0;
    return address>=0x10000 && ReadProcessMemory(GetCurrentProcess(),(void *)address,out,size,&got) && got==size;
}
static void *camera_rate_hook(size_t,const unsigned char *,size_t,uintptr_t);
#include "runtime_camera_rate.inc"
#include "runtime_camera_motion.inc"
#include "runtime_replay_recorded_motion.inc"
#include "runtime_replay_gforce.inc"
#include "runtime_replay_driver_observation.inc"
#include "runtime_replay_hud_observation.inc"
#include "runtime_replay_feedback.inc"
static void camera_patch_install(void) {
    const char *enabled=getenv("BB_CAMERA_PATCH");
    if(!enabled || strcmp(enabled,"1"))return;
    static const unsigned char update[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xe4,0xc0,0xff,0xff,0xff};
    static const unsigned char impulse[]={0x55,0x48,0x89,0xe5,0x53,0x50,0xc5,0xf8,0x28,0xc8,0x48,0x89,0xfb};
    static const unsigned char noise[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    if(runtime_image_size<0xcc8820+sizeof update ||
       memcmp(image+0xcc8820,update,sizeof update) || memcmp(image+0x6ce6f0,impulse,sizeof impulse) ||
       runtime_image_size<0xce3540+sizeof noise || memcmp(image+0xce3540,noise,sizeof noise)) {
        puts("CameraPatch: unsupported 1.28 signatures; skipped");return;
    }
    camera_rate_update_original=(CameraRateUpdate)camera_rate_hook(0xcc8820,update,sizeof update,(uintptr_t)camera_rate_update);
    if(!camera_rate_update_original)return;
    replay_recorded_motion_enabled=1;
    camera_dynamics_install();
    camera_motion_install();
    replay_feedback_install();
    replay_gforce_install();
    printf("CameraPatch: driver observer %s\n",replay_driver_observation_patch()?"on":"unavailable");
    replay_hud_install();
    camera_rate_impulse_original=(CameraRateImpulse)camera_rate_hook(0x6ce6f0,impulse,sizeof impulse,(uintptr_t)camera_rate_impulse);
    camera_noise_update_original=(CameraNoiseUpdate)camera_rate_hook(0xce3540,noise,sizeof noise,(uintptr_t)camera_noise_update);
    puts("CameraPatch: experimental high-FPS camera/replay workaround enabled; behavior is not reference-accurate");
}
#else
static void camera_patch_install(void) {}
#endif
