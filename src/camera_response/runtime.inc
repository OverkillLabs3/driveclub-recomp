/* DriveClub1.28 native-Q camera response, selected by existing BB_CAMERA_PATCH.
   Runtime collection is private and bounded; no diagnostic output is written. */
#ifdef _WIN32
#include <math.h>
#include "camera-fix-bootstrap.inc"
#include "head-staged-publication-api.inc"
static int camera_pose_read(uintptr_t address,void *out,size_t size) {
    SIZE_T got=0;
    return address>=0x10000 && ReadProcessMemory(GetCurrentProcess(),(void *)address,out,size,&got) && got==size;
}
static void *camera_rate_hook(size_t,const unsigned char *,size_t,uintptr_t);
/* Private source-only callbacks; no guest getters, native hooks or raw writes. */
static void lsq_runtime_init(void);
static void lsq_runtime_forget(uintptr_t,uintptr_t);
static void lsq_complete_before_restore(void*,const void*);
static void lsq_parent_status(unsigned,void*,uintptr_t,float,int64_t,int64_t,int64_t);
static void lsqt_init(void);
static void lsqt_invalidate_locked(uintptr_t,uintptr_t);
static int lsqt_commit_binding(const void*);
static void lsqt_role_event(unsigned,uintptr_t);
static void lsqm_init(void);
static void lsqm_observe(const void*,const void*,int64_t,int,int);
#include "runtime_camera_accuracy_audit.inc"
#include "runtime_camera_rate.inc"
#include "runtime_camera_motion.inc"
#include "runtime_replay_recorded_motion.inc"


static int replay_linear_q_requested(void);
static int replay_linear_q_install(void);
static void replay_linear_q_discontinuity(void);
static void replay_linear_q_body_reset(uintptr_t);
static int lfpo_enabled;
static ABI void lfpo_hsq_before(void*,float);
static ABI void lfpo_hsq_after(void*,float);
#include "runtime_replay_hud_observation.inc"
#include "replay-packet-observer.inc"
static int feedback_target_basis_enabled;
static int feedback_target_basis_requested(void){return 0;}
static ABI void feedback_target_basis_observe(void*a,void*b,void*c){(void)a;(void)b;(void)c;}
static int feedback_branch_scalar_install(void){return 0;}
#include "runtime_replay_feedback.inc"
#include "runtime_lf_transaction_tape.inc"
#include "angular-live-runtime.inc"
static void gfr_forget(uintptr_t);
#include "native-feedback32-observer.inc"
static int replay_linear_mean_rebase_requested(void);
static int replay_linear_mean_rebase_prepare(void);
static void replay_linear_mean_rebase_commit(void);
static int feedback_branch_scalar_install(void);
#include "replay-linear-native-q.inc"
#include "late-sensor-q-runtime.inc"
static void lsqm_init(void){}
static void lsqm_observe(const void*a,const void*b,int64_t t,int known,int status){(void)a;(void)b;(void)t;(void)known;(void)status;}
#include "replay-linear-mean-rebase.inc"
static void rpq_actor_invalidate(uintptr_t);
static void rpq_lifecycle_observer_ready(unsigned);
static int rpl_enabled;
#include "reference-seek.inc"
static int hud_display_trace_install(void){return 0;}
#include "gforce-output-observer.inc"
static void sdk_rebase_observer_install(void){}
static void low_fps_physics_observer_install(void){}
static ABI void lfpo_hsq_before(void*p,float dt){(void)p;(void)dt;}
static ABI void lfpo_hsq_after(void*p,float dt){(void)p;(void)dt;}
#include "runtime-lifecycle.inc"
#include "paired-query-runtime.inc"
#include "late-sensor-q-tombstone-runtime.inc"
static int hds_install(void){return 0;}
#include "runtime-head-pause.inc"
#include "head-response-primitives-runtime.inc"
#include "head-staged-publication-runtime.inc"
#include "head-temporal-group-runtime.inc"
#include "head-temporal-group-publication-binding.inc"
static void camera_patch_install(void) {
 const char*enabled=getenv("BB_CAMERA_PATCH");if(!enabled||strcmp(enabled,"1"))return;

    camera_fix_bootstrap_configure();
    hgg_runtime_prepare();
    hps_install();
    hpr_group_bind_epoch_provider(htp_source_epoch);
    hpr_install();
    hds_install();
    rpl_install();
    lsq_runtime_init();
    lsqm_init();
    lsqt_init();
    if(!replay_linear_q_requested())feedback_branch_scalar_install(); /* RLQ installs42 after its whole-region hash */
    feedback_target_basis_enabled=feedback_target_basis_requested();
    rpq_runtime_install(); /* clone ORIGINAL private controller before any detour */
    replay_packet_observer_install();paired_query_install();
    gfo_install();
    sdk_rebase_observer_install();
    lf_transaction_tape_install();
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
    hud_display_trace_install();
    hkt_install();
    camera_reference_seek_install();
    hgg_htp_bind();
    htp_install();
    hcal_install();
    hat_install();


        replay_recorded_motion_enabled=0;
        if(camera_fix_getenv("BB_CAMERA_REFERENCE_REPLAY_FEEDBACK"))replay_feedback_install();
        camera_dynamics_install();camera_motion_install();replay_hud_install();low_fps_physics_observer_install();
        if(camera_noise_lifecycle_install())camera_noise_update_original=(CameraNoiseUpdate)camera_rate_hook(0xce3540,noise,sizeof noise,(uintptr_t)camera_noise_update);
        puts("CameraPatch: experimental high-FPS camera/replay workaround enabled"); /* Existing launcher feature detection. */
        puts("CameraResponse: rigid camera noise and native-30 response enabled");
        puts("CameraResponse: shared camera/HUD phase for gameplay and replay");
}
#else
static void camera_patch_install(void) {}
#endif
