#ifndef CAMERA_HEAD_LIVE_CORE_INCLUDED
#define CAMERA_HEAD_LIVE_CORE_INCLUDED
#include "shared-phase-clock-core.inc"
/* Native coupled torso/head transaction at authored33333us. Only the64-byte
 * coupled state is persistent. World composition/animation and physics still
 * run once per real render. Input between observations is an explicit sampled
 * model (linear velocity, rigid shortest-arc basis), not unobserved PS4 truth.
 * HKTInputs/HKTRecord, crs_native_us, crr quaternion helpers precede this file. */
typedef struct {
 uintptr_t owner,vehicle;uint32_t frame;unsigned valid,adapted;
 int64_t credit_us,canonical_us,time_us;uintptr_t clock;unsigned primed;uint64_t ticks,resets;
 unsigned char canonical[64],display[64];
 float linear[4],angular[4],q_linear[4],q_angular[4],matrix[16];
} HCLState;
typedef int(*HCLStep)(void*,unsigned char[0x380],const HKTInputs*,uint32_t,uint32_t*);
enum {HCL_NATIVE=0,HCL_APPLIED=1,HCL_UNSUPPORTED=-1,HCL_BACKEND_FAILED=-2};

static int hcl_finite(const void*p,unsigned n){const unsigned char*b=p;for(unsigned i=0;i<n;++i){float x;memcpy(&x,b+4*i,4);if(!isfinite(x))return 0;}return 1;}
static void hcl_basis(const float*a,const float*b,double u,float*out){
 memcpy(out,b,64);
 /* A held native basis must remain BITWISE held. Reconstructing its quaternion
    would needlessly normalize native fast-trig roundoff at an interior Q. */
 if(u>=1||(!memcmp(a,b,12)&&!memcmp(a+4,b+4,12)&&!memcmp(a+8,b+8,12)))return;
 float qa[4],qb[4],q[4];crr_rigid_quaternion(a,qa);crr_rigid_quaternion(b,qb);crr_slerp(qa,qb,u,q);
 double x=q[0],y=q[1],z=q[2],w=q[3];
 out[0]=(float)(1-2*(y*y+z*z));out[1]=(float)(2*(x*y-z*w));out[2]=(float)(2*(x*z+y*w));
 out[4]=(float)(2*(x*y+z*w));out[5]=(float)(1-2*(x*x+z*z));out[6]=(float)(2*(y*z-x*w));
 out[8]=(float)(2*(x*z-y*w));out[9]=(float)(2*(y*z+x*w));out[10]=(float)(1-2*(x*x+y*y));
}



#endif
