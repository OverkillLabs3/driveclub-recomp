static _Thread_local void *camera_dynamics_self;
#include <math.h>
/* Transfer the native 30 FPS force+spring step to a shorter camera update.
   All damping/stiffness/force values come from the game. Only the two measured
   continuous chase-camera springs use this path; other spring effects bypass it. */
static int camera_reference_spring_enabled=1;
/* Strength blends each native integration result toward the reference result.
   This is an experimental response control, not a new authored spring value. */
static float camera_reference_strength=0.5f;

typedef float (ABI *CameraReferenceForce)(void *,float,float);
static CameraReferenceForce camera_reference_force_original;
typedef struct {
    void *self,*camera;float dt,force,x,v,post_v;int ready;
} CameraReferenceInput;
static _Thread_local CameraReferenceInput camera_reference_input;
static int camera_reference_step(double x,double v,double force,double dt,double damping,double stiffness,double *out) {
    const double T=1.0/30.0;
    if(!(dt>0) || dt>T || !(damping>=0) || !(stiffness>0) ||
       !isfinite(x) || !isfinite(v) || !isfinite(force) || !isfinite(damping) || !isfinite(stiffness))return 0;
    /* Native input: v += force*T, followed by one semi-implicit Euler step.
       M maps [position, velocity]; its equilibrium includes the native order
       of force injection and damping, rather than changing authored tuning. */
    double a=1-damping*T,m00=1-stiffness*T*T,m01=T*a,m10=-stiffness*T,m11=a;
    if(!(a>0))return 0;
    double half=(m00+m11)*.5,disc=half*half-a,r=dt/T,scale,identity;
    if(disc>1e-12) {
        double gap=sqrt(disc),lo=half-gap,hi=half+gap;
        if(!(lo>0) || !isfinite(hi))return 0;
        double pl=pow(lo,r),ph=pow(hi,r);
        scale=(ph-pl)/(2*gap);identity=(hi*pl-lo*ph)/(2*gap);
    }else if(disc < -1e-12) {
        double gap=sqrt(-disc),rho=sqrt(a),theta=atan2(gap,half),pr=pow(rho,r);
        scale=pr*sin(r*theta)/gap;identity=pr*cos(r*theta)-scale*half;
    }else {
        if(!(half>0))return 0;
        scale=r*pow(half,r-1);identity=(1-r)*pow(half,r);
    }
    double eq=a*force/stiffness,dx=x-eq;
    out[0]=eq+(scale*m00+identity)*dx+scale*m01*v;
    out[1]=scale*m10*dx+(scale*m11+identity)*v;
    return isfinite(out[0]) && isfinite(out[1]);
}
static int camera_reference_force_site(uintptr_t caller,void *self) {
    uintptr_t camera=(uintptr_t)camera_dynamics_self;
    return camera && ((caller==0xd1c3dd && (uintptr_t)self==camera+0x538) ||
                      (caller==0xd1c42b && (uintptr_t)self==camera+0x50c));
}
static ABI float camera_reference_force(void *self,float force,float dt) {
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-runtime_image_base;
    CameraReferenceInput input={0};
    if(camera_reference_spring_enabled && camera_reference_force_site(caller,self)) {
        input.self=self;input.camera=camera_dynamics_self;input.dt=dt;input.force=force;
        input.ready=camera_pose_read((uintptr_t)self+0x10,&input.v,4) && camera_pose_read((uintptr_t)self+0x14,&input.x,4);
    }
    float result=camera_reference_force_original(self,force,dt);
    if(input.ready) {
        input.ready=camera_pose_read((uintptr_t)self+0x10,&input.post_v,4);
        camera_reference_input=input;
    }
    return result;
}
static float camera_reference_apply(void *self,uintptr_t caller,float dt,float native_result,const unsigned char *bytes) {
    if(!camera_reference_spring_enabled || camera_reference_strength<=0)return native_result;
    uintptr_t camera=(uintptr_t)camera_dynamics_self;
    if(!camera || !((caller==0xd1c3ed && (uintptr_t)self==camera+0x538) ||
                    (caller==0xd1c43b && (uintptr_t)self==camera+0x50c)))return native_result;
    CameraReferenceInput input=camera_reference_input;camera_reference_input.ready=0;
    float before[11];memcpy(before,bytes,sizeof before);unsigned max_steps;memcpy(&max_steps,bytes+0xc,4);
    double result[2];
    if(input.ready && input.self==self && input.camera==camera_dynamics_self && input.dt==dt &&
       input.x==before[5] && input.post_v==before[4] && dt>0 && dt<1.0f/30.0f-.000001f &&
       max_steps && before[7]>1.0f/30.0f && before[8]==0 && before[10]<=0 && isfinite(before[2]) && before[2]>=0 &&
       camera_reference_step(input.x,input.v,input.force,dt,before[1],before[6],result)) {
        float x=(float)result[0],v=(float)result[1];
        if(isfinite(x) && isfinite(v)) {
            if(x>before[2]){x=before[2];v=0;}else if(x < -before[2]){x=-before[2];v=0;}
            if(camera_reference_strength<1) {
                float native[2];
                if(!camera_pose_read((uintptr_t)self+0x10,native,sizeof native) ||
                   !isfinite(native[0]) || !isfinite(native[1]))return native_result;
                v=native[0]+(v-native[0])*camera_reference_strength;
                x=native[1]+(x-native[1])*camera_reference_strength;
            }
            memcpy((char *)self+0x10,&v,4);memcpy((char *)self+0x14,&x,4);
            native_result=x;
        }
    }
    return native_result;
}

typedef float (ABI *CameraDynamicsSpring)(void *,float);
static CameraDynamicsSpring camera_dynamics_spring_original;
static void *camera_dynamics_select(void *self) {
    uintptr_t type=0,update=0;
    if(!camera_pose_read((uintptr_t)self,&type,8) ||
       !camera_pose_read(type+0x18,&update,8) || update!=runtime_image_base+0xd1b7c0)return NULL;
    return self;
}
static ABI float camera_dynamics_spring(void *self,float dt) {
    if(!camera_dynamics_self)return camera_dynamics_spring_original(self,dt);
    unsigned char before[0x2c];
    int valid=camera_pose_read((uintptr_t)self,before,sizeof before);
    float result=camera_dynamics_spring_original(self,dt);
    return valid?camera_reference_apply(self,(uintptr_t)__builtin_return_address(0)-runtime_image_base,dt,result,before):result;
}
static void camera_dynamics_install(void) {
    static const unsigned char entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50};
    camera_dynamics_spring_original=(CameraDynamicsSpring)camera_rate_hook(0x6ce5b0,entry,sizeof entry,(uintptr_t)camera_dynamics_spring);
    if(camera_dynamics_spring_original) {
        static const unsigned char force[]={0xc5,0xfa,0x59,0xc1,0xc5,0xfa,0x58,0x47,0x10,0xc5,0xfa,0x11,0x47,0x10};
        camera_reference_force_original=(CameraReferenceForce)camera_rate_hook(0x6ce740,force,sizeof force,(uintptr_t)camera_reference_force);
    }
    printf("CameraPatch: chase spring workaround %s (50%% response blend)\n",camera_reference_force_original?"on":"unavailable");
}
