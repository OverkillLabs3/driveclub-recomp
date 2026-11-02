#include "target-derivative-core.inc"
static QTargetHistory qtarget_histories[64];static SRWLOCK qtarget_lock=SRWLOCK_INIT;
static void qtarget_forget_all(void){AcquireSRWLockExclusive(&qtarget_lock);memset(qtarget_histories,0,sizeof qtarget_histories);ReleaseSRWLockExclusive(&qtarget_lock);}
static int qtarget_requested(void){const char*p=camera_fix_getenv("BB_CAMERA_TARGET_Q_DERIVATIVE");return p&&!strcmp(p,"1");}
static void qtarget_filter_impl(void*self,float dt,int reset){
 int64_t us,t=0,clock_dt=0;uintptr_t clock=0,binding=0;unsigned kind=0,frame=0;float velocity[6],derivative[6];
 if(!crs_native_us(dt,&us)||!camera_pose_read(runtime_image_base+0x1911100,&clock,8)||!camera_pose_read(clock+0x970,&clock,8)||!camera_pose_read(clock+0x1e0,&clock,8)||!camera_pose_read(clock+0x58,&t,8)||!camera_pose_read(clock+0x60,&clock_dt,8)||us!=clock_dt||
    !camera_pose_read((uintptr_t)self+8,&kind,4)||!camera_pose_read((uintptr_t)self+0x28,&binding,8)||!camera_pose_read((uintptr_t)self+0x44,&frame,4)||!camera_pose_read((uintptr_t)self+0x120,velocity,12)||!camera_pose_read((uintptr_t)self+0x130,velocity+3,12))return;
 if(kind!=1)return;
 AcquireSRWLockExclusive(&qtarget_lock);QTargetHistory*h=NULL;
 for(unsigned i=0;i<64;++i)if(qtarget_histories[i].owner==(uintptr_t)self){h=qtarget_histories+i;break;}
 if(!h)for(unsigned i=0;i<64;++i)if(!qtarget_histories[i].owner){h=qtarget_histories+i;break;}
 int status=h?qtarget_advance(h,(uintptr_t)self,binding,clock,frame,kind,reset,t,us,velocity,derivative):-1;
 ReleaseSRWLockExclusive(&qtarget_lock);
 if(status==1){memcpy((char*)self+0x140,derivative,12);memcpy((char*)self+0x150,derivative+3,12);}
 float data[24]={(float)status,(float)us};memcpy(data+2,&t,8);memcpy(data+4,velocity,24);if(status==1)memcpy(data+10,derivative,24);camera_accuracy_log(17,self,binding,frame,dt,data,16);
}
static void qtarget_filter(void*self,float dt,int reset){uint32_t csr;__asm__ volatile("stmxcsr %0":"=m"(csr));qtarget_filter_impl(self,dt,reset);__asm__ volatile("ldmxcsr %0"::"m"(csr):"memory");}
