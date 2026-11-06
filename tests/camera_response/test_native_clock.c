#include <assert.h>
#include <stdio.h>
#include "../../src/camera_response/lf_native_clock.h"
int main(void){
 for(int parts=1;parts<=8;++parts){int64_t t=0;int due=0;
  for(int tick=0;tick<300;++tick)for(int i=0;i<parts;++i){int64_t a=(int64_t)(i+1)*33333/parts-(int64_t)i*33333/parts;t+=a;
   LFNativeDecision d=lf_native_decision(t,a,(float)a*1e-6f);assert(d.supported);due+=d.due;
   if(parts==1)assert(d.native_q&&d.due);
  }assert(t==9999900&&due==300);
 }
 int64_t t=0;int due=0;for(int i=0;i<30000;++i){int64_t a=1000+(i*7919u)%19000;t+=a;LFNativeDecision d=lf_native_decision(t,a,(float)a*1e-6f);assert(d.supported);due+=d.due;}assert(due==t/33333);
 assert(!lf_native_decision(100000,0,0).supported);assert(!lf_native_decision(100000,50000,.05f).supported);
 assert(!lf_native_decision(100000,11111,-.011111f).supported);assert(!lf_native_decision(100000,11111,.005f).supported);
 puts("PASS native30 exact cadence; 60/90/120/150/180/210/240 integer-us schedules;30000 jitter intervals; all owners share phase; zero/scaled/negative/stall native fallback");return 0;
}
