/* Trace-free production support. No files or diagnostic hooks. */

static int camera_accuracy_oracle=1; /* accepted response graph, not a trace flag */
static int camera_accuracy_trace_active(void){return 0;}
static void camera_accuracy_log(unsigned event,void*self,uintptr_t peer,uintptr_t caller,float dt,const float*data,unsigned n){(void)event;(void)self;(void)peer;(void)caller;(void)dt;(void)data;(void)n;}
