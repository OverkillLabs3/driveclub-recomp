#ifndef SHARED_CAMERA_PHASE_CLOCK_RUNTIME_INCLUDED
#define SHARED_CAMERA_PHASE_CLOCK_RUNTIME_INCLUDED
#include "shared-phase-clock-core.inc"
static HCSClock hcs_read_clock(unsigned expected_frame,float dt){
 HCSClock result={0};uintptr_t root=0,service=0,clock=0;int64_t us=0;unsigned char before[0x240],after[0x240];
 if(!crs_native_us(dt,&us)||!camera_pose_read(runtime_image_base+0x1911100,&root,8)||!camera_pose_read(root+0x968,&service,8)||!camera_pose_read(service+0x1e0,&clock,8)||!camera_pose_read(clock,before,sizeof before)||!camera_pose_read(clock,after,sizeof after))return result;
 int64_t t=0,d=0,t2=0,d2=0;uint32_t f=0,f2=0;memcpy(&t,before+0x58,8);memcpy(&d,before+0x60,8);memcpy(&f,before+0x21c,4);memcpy(&t2,after+0x58,8);memcpy(&d2,after+0x60,8);memcpy(&f2,after+0x21c,4);
 if(t!=t2||d!=d2||f!=f2||d!=us||f!=expected_frame||t<d)return result;
 result=(HCSClock){clock,t,d,f,1,0};return result;
}
#endif
