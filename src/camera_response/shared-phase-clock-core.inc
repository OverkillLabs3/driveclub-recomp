#ifndef SHARED_CAMERA_PHASE_CLOCK_CORE_INCLUDED
#define SHARED_CAMERA_PHASE_CLOCK_CORE_INCLUDED
/* Actual native clock identity+integer endpoints; no wall-time inference. */
typedef struct {uintptr_t clock;int64_t time_us,dt_us;uint32_t frame;int valid;unsigned domain;} HCSClock;
#endif
