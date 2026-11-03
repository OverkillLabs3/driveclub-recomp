#include "shared-phase-clock-runtime.inc"
#include "head-live-core.inc"
#include "optional-real-clock-runtime.inc"
#include "head-pause-api.inc"
static int hcl_enabled,hllq_enabled;
typedef void(ABI *HCLReset)(void*);
static HCLReset hcl_reset_original;
static int hcl_install(void){return 0;}
static int hcl_apply(const HKTRecord*r){(void)r;return 0;}
static void hcl_forget(uintptr_t owner){hps_forget(owner);}
