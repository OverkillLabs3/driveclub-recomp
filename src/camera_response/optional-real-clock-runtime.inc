#ifndef CAMERA_HEAD_REAL_DOMAIN_INCLUDED
#define CAMERA_HEAD_REAL_DOMAIN_INCLUDED
#include "optional-real-clock-core.inc"
/* Verified original TimeCycle layout: master+08 initialized by787D0->662F40,
 * updated by78EC0->663040. TC+10 cumulative, TC+18 elapsed, TC+38 paused.
 * B91789..B917A1 explicitly consumes master+20 during native replay pause.
 * This reader is HEAD-only and only called after the logical reader rejects.
 * Equal float dt alone is insufficient: logical raw60 must be nearzero under
 * the original threshold, while the real TimeCycle must be unpaused. */
static HCSClock hcs_read_head_clock(unsigned frame,float dt){
 HCSClock out=hcs_read_clock(frame,dt);if(out.valid)return out;
 const char*flag=camera_fix_getenv("BB_CAMERA_HEAD_REAL_CLOCK");if(!flag||atoi(flag)!=1)return out;
 uintptr_t root=0,service=0,clock=0;unsigned char a[0x240],b[0x240];int64_t us;
 if(!crs_native_us(dt,&us)||us>8*33333||!camera_pose_read(runtime_image_base+0x1911100,&root,8)||!camera_pose_read(root+0x968,&service,8)||!camera_pose_read(service+0x1e0,&clock,8)||!camera_pose_read(clock,a,sizeof a)||!camera_pose_read(clock,b,sizeof b))return out;
 int64_t t,d,logical;uint32_t f;memcpy(&t,a+0x18,8);memcpy(&d,a+0x20,8);memcpy(&logical,a+0x60,8);memcpy(&f,a+0x21c,4);
 if(memcmp(a+8,b+8,0x40)||memcmp(a+0x58,b+0x58,16)||memcmp(a+0x21c,b+0x21c,4)||f!=frame||a[0x40]||d!=us||t<d||fabsf((float)logical*1e-6f)>=1e-4f)return out;
 return(HCSClock){clock,t,d,f,1,1};
}
#endif
