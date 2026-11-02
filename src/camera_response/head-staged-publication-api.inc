#include "head-staged-publication-core.inc"
typedef struct {
 HtpKey key;int64_t logical_T,logical_dt;const void*hkt;const void*primitives;
 uint64_t head_serial;
} HtpRequest;
typedef int(*HtpProducer)(void*,const HtpRequest*,HtpSelection*);
typedef void(*HtpForget)(void*,uintptr_t);
typedef int(*HtpAccessor)(uintptr_t,uintptr_t,uint32_t,uint32_t,const void**,const void**,uint64_t*,uint64_t*);
enum{HTP_PROOF_NATIVE_Q=1,HTP_PROOF_SOURCE_Q=2,HTP_PROOF_RESET_COVERAGE=4};
static uint64_t htp_source_epoch(uintptr_t);
static void htp_reset_begin(uintptr_t);
static void htp_reset_end(void);
static int htp_requested(void);
static int htp_install(void);
static int htp_register_producer(HtpProducer,HtpForget,HtpAccessor,void*,unsigned);
