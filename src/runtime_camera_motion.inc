#include <math.h>
/* Camera-only derivatives over the original 30 Hz observation interval.
   Vehicle physics and camera transforms continue updating at render cadence. */
typedef void (ABI *CameraTargetUpdate)(void *,unsigned,float);
static CameraTargetUpdate camera_target_update_original;
static int camera_motion_point_enabled;
typedef struct { double end; float dt; float v[6]; } CameraMotionSample;
typedef struct {
    void *self;
    double time;
    ULONGLONG last;
    unsigned count;
    CameraMotionSample samples[128];
} CameraMotionHistory;
static void camera_motion_filter(void *self,float dt,int reset) {
    static _Thread_local CameraMotionHistory histories[64];
    CameraMotionHistory *h=NULL;
    for(unsigned i=0;i<64;++i) {
        if(!histories[i].self) histories[i].self=self;
        if(histories[i].self==self) { h=&histories[i];break; }
    }
    if(!h) return;
    ULONGLONG now=GetTickCount64();
    if(reset || now-h->last>250 || !(dt>0) || dt>0.1f) { h->count=0;h->time=0; }
    h->last=now;
    if(!(dt>0) || dt>0.1f) return;
    if(h->count==128) { memmove(h->samples,h->samples+1,127*sizeof h->samples[0]);--h->count; }
    h->time+=dt;
    CameraMotionSample *s=&h->samples[h->count++];
    s->end=h->time;s->dt=dt;
    memcpy(s->v,(char *)self+0x120,12);
    memcpy(s->v+3,(char *)self+0x130,12);
    const double interval=1.0/30.0;
    /* Keep native behavior at the reference rate and below it. */
    if(dt>=interval-0.000001) return;
    unsigned kind;memcpy(&kind,(char *)self+8,4);
    if(camera_motion_point_enabled && kind==1) {
        /* Native CCDDA0 gets vehicle v100/v150 directly: these are point
           velocities, not position differences averaged over a render frame.
           Reproduce the native 30 FPS difference over one reference interval. */
        double target=h->time-interval;
        float derivative[6];
        for(unsigned i=h->count-1;i>0;--i) {
            CameraMotionSample *a=&h->samples[i-1],*b=&h->samples[i];
            if(a->end>target || b->end<=a->end)continue;
            double alpha=(target-a->end)/(b->end-a->end);
            for(unsigned axis=0;axis<6;++axis) {
                derivative[axis]=(float)((s->v[axis]-(a->v[axis]+(b->v[axis]-a->v[axis])*alpha))/interval);
                if(!isfinite(derivative[axis]))return;
            }
            memcpy((char *)self+0x140,derivative,12);
            memcpy((char *)self+0x150,derivative+3,12);
            return;
        }
        return;
    }
    if(h->time<2*interval)return;
    double integral[2][6]={{0}}, covered[2]={0};
    for(unsigned i=h->count;i>0;--i) {
        CameraMotionSample *a=&h->samples[i-1];
        double start=a->end-a->dt;
        if(a->end<=h->time-2*interval) break;
        for(unsigned w=0;w<2;++w) {
            double hi=h->time-w*interval,lo=hi-interval;
            double left=start>lo?start:lo,right=a->end<hi?a->end:hi;
            double weight=right-left;
            if(weight<=0) continue;
            covered[w]+=weight;
            for(unsigned axis=0;axis<6;++axis) integral[w][axis]+=weight*a->v[axis];
        }
    }
    if(covered[0]<interval-0.000001 || covered[1]<interval-0.000001) return;
    for(unsigned axis=0;axis<6;++axis) {
        float derivative=(float)((integral[0][axis]-integral[1][axis])/(interval*interval));
        if(!isfinite(derivative)) return;
        memcpy((char *)self+(axis<3?0x140:0x150)+(axis%3)*4,&derivative,4);
    }
}
static ABI void camera_target_update(void *self,unsigned frame,float dt) {
    unsigned previous;
    memcpy(&previous,(char *)self+0x44,4);
    int reset=*((unsigned char *)self+0x40)!=0 || previous+1!=frame;
    camera_target_update_original(self,frame,dt);
    if(previous!=frame) camera_motion_filter(self,dt,reset);
}
static void camera_motion_install(void) {
    camera_motion_point_enabled=1;
    static const unsigned char entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x81,0xec,0x88,0,0,0};
    camera_target_update_original=(CameraTargetUpdate)camera_rate_hook(0xccd600,entry,sizeof entry,(uintptr_t)camera_target_update);
    printf("CameraMotion: camera acceleration at reference observation interval: %s\n",camera_target_update_original?"on":"unavailable");
    if(camera_motion_point_enabled)puts("CameraMotion: direct vehicle velocity sampled across 1/30 s; other target types unchanged");
}
