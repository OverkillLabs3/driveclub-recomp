/* GForceComponent feeds the character's Float_GForce/Float_GForceJolt animation
   variables. Its private velocity difference must not observe replay feedback. */
typedef void (ABI *ReplayGForceUpdate)(void *,float);
typedef uintptr_t (ABI *ReplayGForceVelocity)(void *,void *);
static ReplayGForceUpdate replay_gforce_original;
static ReplayGForceVelocity replay_gforce_velocity_original;
static int replay_gforce_enabled;
typedef struct {
    void *self;float dt,previous[3],raw[3];int restored;
} ReplayGForceScope;
static _Thread_local ReplayGForceScope *replay_gforce_scope;
static int replay_gforce_observe(void *vehicle,float *out) {
    ReplayGForceScope *s=replay_gforce_scope;float accel[6];
    if(!replay_gforce_enabled || !s || s->restored ||
       !replay_recorded_read(replay_recorded_state(vehicle),s->dt,accel)) return 0;
    float observed[3];
    for(unsigned j=0;j<3;++j) {
        observed[j]=s->previous[j]+accel[j]*s->dt;
        if(!isfinite(out[j]) || !isfinite(observed[j]))return 0;
    }
    memcpy(s->raw,out,12);memcpy(out,observed,12);s->restored=1;
    return 1;
}
static ABI uintptr_t replay_gforce_velocity(void *vehicle,void *out) {
    uintptr_t caller=(uintptr_t)__builtin_return_address(0);
    uintptr_t result=replay_gforce_velocity_original(vehicle,out);
    if(caller==runtime_image_base+0xd2a71c) {
        replay_gforce_observe(vehicle,out);
    }
    return result;
}
static ABI void replay_gforce_update(void *self,float dt) {
    ReplayGForceScope scope={0},*previous=replay_gforce_scope;
    scope.self=self;scope.dt=dt;
    int readable=camera_pose_read((uintptr_t)self+0x40,scope.previous,12);
    replay_gforce_scope=readable?&scope:NULL;
    replay_gforce_original(self,dt);
    /* The synthetic sample is used solely by the native derivative. Keep its
       history in native velocity space, including cache-expiry/30 FPS bypass. */
    if(scope.restored)memcpy((char *)self+0x40,scope.raw,12);
    replay_gforce_scope=previous;

}
static void replay_gforce_install(void) {
    replay_gforce_enabled=replay_recorded_motion_enabled;
    static const unsigned char update[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x40};
    /* Includes mov rax,rdx before the gateway's RAX scratch jump. */
    static const unsigned char velocity[]={0x48,0x89,0xf0,0x48,0x8b,0x4f,0x70,0x8b,0x77,0x18,0x48,0x89,0xcf,0x48,0x89,0xc2};
    if(runtime_image_size<0xd2a650+sizeof update || memcmp(image+0xd2a650,update,sizeof update) ||
       runtime_image_size<0xc4bae0+sizeof velocity || memcmp(image+0xc4bae0,velocity,sizeof velocity)) {
        puts("ReplayGForce: unsupported signatures; skipped");return;
    }
    replay_gforce_velocity_original=(ReplayGForceVelocity)camera_rate_hook(0xc4bae0,velocity,sizeof velocity,(uintptr_t)replay_gforce_velocity);
    if(!replay_gforce_velocity_original)return;
    replay_gforce_original=(ReplayGForceUpdate)camera_rate_hook(0xd2a650,update,sizeof update,(uintptr_t)replay_gforce_update);
    printf("ReplayGForce: character animation observer %s; physics unchanged\n",replay_gforce_original?"on":"unavailable");
}
