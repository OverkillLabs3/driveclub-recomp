/* Optional integration check against a locally flattened, unpatched 1.28 image.
   No guest code is executed. No game data is distributed with this test. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define ABI __attribute__((sysv_abi))
static unsigned char *image;
static uintptr_t runtime_image_base;
static uint64_t runtime_image_size;
#include "../src/runtime_camera_patch.inc"

int main(int argc,char **argv) {
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>0x1400000);rewind(f);
    runtime_image_size=(uint64_t)size;
    image=VirtualAlloc(NULL,size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);assert(image);
    runtime_image_base=(uintptr_t)image;
    assert(fread(image,1,size,f)==(size_t)size);fclose(f);
    unsigned char *before=malloc(size);assert(before);memcpy(before,image,size);
    _putenv("BB_CAMERA_PATCH=");camera_patch_install();assert(!memcmp(before,image,size));
    _putenv("BB_CAMERA_PATCH=0");camera_patch_install();assert(!memcmp(before,image,size));
    _putenv("BB_CAMERA_PATCH=1");camera_patch_install();
    assert(camera_rate_update_original && camera_rate_impulse_original && camera_noise_update_original);
    assert(camera_target_update_original && camera_dynamics_spring_original && camera_reference_force_original);
    assert(replay_feedback_resume && replay_rotation_resume && replay_gforce_original && replay_gforce_velocity_original);
    assert(replay_driver_observation_resume && replay_hud_update_original);
    assert(replay_hud_observation_resume1 && replay_hud_observation_resume2);
    assert(camera_reference_strength==0.5f);
    puts("PASS: opt-out leaves whole 1.28 image identical; every bundled hook matches its real guest signature and installs, with chase blend 0.5.");
    free(before);VirtualFree(image,0,MEM_RELEASE);
}
