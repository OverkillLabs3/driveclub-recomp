#ifndef LF_NATIVE_CLOCK_H
#define LF_NATIVE_CLOCK_H
#include <stdint.h>
#include <string.h>
/* Shared native logical clock phase, not per-owner wall clocks. Inputs and
 * subsequent owner steps make the same decision without accumulating drift.
 * Unsupported/scaled clocks pass through; no guessed replay timestamp. */
typedef struct {int supported,due,native_q;int64_t elapsed_us,tick;} LFNativeDecision;
static LFNativeDecision lf_native_decision(int64_t logical,int64_t clock_dt,float dt){
 LFNativeDecision d={0};if(logical<0||clock_dt<=0||clock_dt>33333||logical<clock_dt)return d;
 float forward=(float)clock_dt*1e-6f;uint32_t a,b;memcpy(&a,&forward,4);memcpy(&b,&dt,4);
 if(a!=b)return d;
 d.supported=1;d.elapsed_us=clock_dt;d.tick=logical/33333;
 d.due=d.tick!=(logical-clock_dt)/33333;d.native_q=clock_dt==33333;return d;
}
#endif
