typedef struct {double t;float v[6];} ReplayObservationSample;
/* Experimental replay visual inertia from the recorded velocities, before the
   resimulation controller adds its position/velocity corrections. No physics writes. */
static int replay_recorded_motion_enabled;
typedef struct {
    uintptr_t state,actor;ULONGLONG stamp;unsigned count;int ready;double time;
    ReplayObservationSample samples[128];float derivative[6];
} ReplayRecordedMotion;
static ReplayRecordedMotion replay_recorded_motion[64];
static SRWLOCK replay_recorded_motion_lock=SRWLOCK_INIT;
static void replay_recorded_advance(uintptr_t state,uintptr_t actor,float dt,const float *velocity) {
    if(!replay_recorded_motion_enabled || !state) return;
    for(unsigned j=0;j<6;++j) if(!isfinite(velocity[j])) return;
    ULONGLONG now=GetTickCount64();
    AcquireSRWLockExclusive(&replay_recorded_motion_lock);
    ReplayRecordedMotion *slot=NULL;ULONGLONG oldest=~(ULONGLONG)0;
    for(unsigned i=0;i<64;++i) {
        if(replay_recorded_motion[i].state==state || !replay_recorded_motion[i].state){slot=&replay_recorded_motion[i];break;}
        if(replay_recorded_motion[i].stamp<oldest){oldest=replay_recorded_motion[i].stamp;slot=&replay_recorded_motion[i];}
    }
    if(slot->state!=state || slot->actor!=actor || now-slot->stamp>250 || !(dt>0) || dt>=1.0f/30.0f-0.000001f) {
        memset(slot,0,sizeof *slot);slot->state=state;slot->actor=actor;
    }
    slot->stamp=now;slot->ready=0;
    if(dt>0 && dt<1.0f/30.0f-0.000001f) {
        if(slot->count) slot->time+=dt;
        if(slot->count==128){memmove(slot->samples,slot->samples+1,127*sizeof slot->samples[0]);--slot->count;}
        ReplayObservationSample *sample=&slot->samples[slot->count++];sample->t=slot->time;memcpy(sample->v,velocity,24);
        double target=slot->time-1.0/30.0;
        for(unsigned i=slot->count-1;i>0;--i) {
            ReplayObservationSample *a=&slot->samples[i-1],*b=&slot->samples[i];
            if(a->t>target || b->t<=a->t) continue;
            double alpha=(target-a->t)/(b->t-a->t);int finite=1;
            for(unsigned j=0;j<6;++j){slot->derivative[j]=(float)((velocity[j]-(a->v[j]+(b->v[j]-a->v[j])*alpha))*30.0);finite &= isfinite(slot->derivative[j]);}
            slot->ready=finite;break;
        }
    }
    ReleaseSRWLockExclusive(&replay_recorded_motion_lock);
}
static int replay_recorded_read(uintptr_t state,float dt,float *out) {
    if(!replay_recorded_motion_enabled || !(dt>0) || dt>=1.0f/30.0f-0.000001f) return 0;
    int found=0;ULONGLONG now=GetTickCount64();
    AcquireSRWLockShared(&replay_recorded_motion_lock);
    for(unsigned i=0;i<64;++i) if(replay_recorded_motion[i].state==state && replay_recorded_motion[i].ready && now-replay_recorded_motion[i].stamp<100) {
        memcpy(out,replay_recorded_motion[i].derivative,24);found=1;break;
    }
    ReleaseSRWLockShared(&replay_recorded_motion_lock);return found;
}
static uintptr_t replay_recorded_state(void *vehicle) {
    uintptr_t physics=0,state=0;
    if(!camera_pose_read((uintptr_t)vehicle+0x68,&physics,8) || !camera_pose_read(physics+0x10,&state,8)) return 0;
    return state;
}
static void replay_recorded_publish(void *vehicle,void *actor,float dt,const float *linear) {
    if(!replay_recorded_motion_enabled) return;
    uintptr_t state=replay_recorded_state(vehicle);float velocity[6];memcpy(velocity,linear,12);
    if(!camera_pose_read((uintptr_t)actor+0x50,velocity+3,12)) return;
    replay_recorded_advance(state,(uintptr_t)actor,dt,velocity);
}
