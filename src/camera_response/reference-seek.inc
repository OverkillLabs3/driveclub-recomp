static void lf_shared_q_forget_all(void);
/* Actual native seek boundary. No save-file loader, driving input or physics
 * write. Clear PRIVATE response epochs on both sides of the original call. */
typedef uintptr_t(ABI*CameraReferenceSeek)(void*,int64_t);
static CameraReferenceSeek camera_reference_seek_original;
static void camera_reference_forget_all(void){
 replay_linear_q_discontinuity();
 rpq_actor_invalidate(0);
 gfr_forget(0);
 lf_shared_q_forget_all(); qtarget_forget_all(); hud_reference_discontinuity();hcl_forget(0);hcal_invalidate_all(0);
 uintptr_t owners[CRS_STATE_CAPACITY];unsigned count=0;
 AcquireSRWLockShared(&camera_reference_store_lock);
 for(unsigned i=0;i<CRS_STATE_CAPACITY;++i)if(camera_reference_store.slots[i].owner)owners[count++]=camera_reference_store.slots[i].owner;
 ReleaseSRWLockShared(&camera_reference_store_lock);
 for(unsigned i=0;i<count;++i)camera_reference_invalidate(owners[i]);
 AcquireSRWLockExclusive(&crr_lock);memset(crr_histories,0,sizeof crr_histories);memset(crr_observations,0,sizeof crr_observations);ReleaseSRWLockExclusive(&crr_lock);
 camera_helmet_reference_discontinuity();
 /* Noise transactions serialize through this same lock. Preserve the original
    current matrices/RNG; only discard fractional private cadence and ownership. */
 AcquireSRWLockExclusive(&camera_noise_state_lock);memset(camera_noise_states,0,sizeof camera_noise_states);++camera_noise_generation;ReleaseSRWLockExclusive(&camera_noise_state_lock);
}
static ABI uintptr_t camera_reference_seek(void*self,int64_t time){
 htp_reset_begin(0);camera_reference_forget_all();uintptr_t result=camera_reference_seek_original(self,time);camera_reference_forget_all();htp_reset_end();
 float data[24]={0};memcpy(data,&time,8);camera_accuracy_log(12,self,0,0,0,data,2);return result;
}
static int camera_reference_seek_install(void){
 static const unsigned char entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x49,0x89,0xfe};
 if(!hcl_enabled&&!rpl_enabled)return 0;
 camera_reference_seek_original=(CameraReferenceSeek)camera_rate_hook(0x762340,entry,sizeof entry,(uintptr_t)camera_reference_seek);
 if(camera_reference_seek_original)rpq_lifecycle_observer_ready(1);
 if(camera_reference_seek_original)puts("CameraReference: actual native seek clears private HUD/chase/body/noise epochs");
 return camera_reference_seek_original!=NULL;
}
