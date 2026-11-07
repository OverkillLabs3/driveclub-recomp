/* Production HAI native transaction adapters; diagnostic-only proof removed. */
static void haip_label(unsigned phase,int64_t Q,int64_t duration){(void)phase;(void)Q;(void)duration;}
static uint64_t haip_next_serial(void){return 0;}
static int haip_copy(unsigned char camera[0x3a0],const HSIInput*in){return hsi_native_copy(&hai_copy_kernel,camera,in,NULL);}
static int haip_solver(unsigned char camera[0x3a0],const HACInputs*in){return hac_native_step(&hcal_kernel,camera,in,NULL);}
static void haip_commit(const HAIManagerCall*c,const HAIResult*result,const unsigned char*native,unsigned applied,unsigned unchanged){(void)c;(void)result;(void)native;(void)applied;(void)unchanged;}
