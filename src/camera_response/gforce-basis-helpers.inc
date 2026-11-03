#include <float.h>
/* Same rigid SO3 sampled-input math already used by the camera reference. */
static int gfr_rigid_quaternion(const float*m,float*q){
 double error=0,det=(double)m[0]*(m[5]*m[10]-m[6]*m[9])-(double)m[1]*(m[4]*m[10]-m[6]*m[8])+(double)m[2]*(m[4]*m[9]-m[5]*m[8]);
 for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c){double dot=0;for(unsigned j=0;j<3;++j){if(!isfinite(m[r*4+j]))return 0;dot+=(double)m[r*4+j]*m[c*4+j];}double e=fabs(dot-(r==c));if(e>error)error=e;}
 /* A float32 numeric validity bound, not an artistic spring coefficient. */
 if(error>512*FLT_EPSILON||fabs(det-1)>1024*FLT_EPSILON)return 0;
 double x,y,z,w,s,trace=(double)m[0]+m[5]+m[10];
 if(trace>0){s=sqrt(trace+1)*2;w=s*.25;x=(m[9]-m[6])/s;y=(m[2]-m[8])/s;z=(m[4]-m[1])/s;}
 else if(m[0]>m[5]&&m[0]>m[10]){s=sqrt(1+(double)m[0]-m[5]-m[10])*2;w=(m[9]-m[6])/s;x=s*.25;y=(m[1]+m[4])/s;z=(m[2]+m[8])/s;}
 else if(m[5]>m[10]){s=sqrt(1+(double)m[5]-m[0]-m[10])*2;w=(m[2]-m[8])/s;x=(m[1]+m[4])/s;y=s*.25;z=(m[6]+m[9])/s;}
 else{s=sqrt(1+(double)m[10]-m[0]-m[5])*2;w=(m[4]-m[1])/s;x=(m[2]+m[8])/s;y=(m[6]+m[9])/s;z=s*.25;}
 double norm=sqrt(x*x+y*y+z*z+w*w);if(!(norm>0)||!isfinite(norm))return 0;
 q[0]=(float)(x/norm);q[1]=(float)(y/norm);q[2]=(float)(z/norm);q[3]=(float)(w/norm);return 1;
}
static void gfr_slerp(const float*a,const float*b,double u,float*out){
 double dot=0;for(unsigned i=0;i<4;++i)dot+=(double)a[i]*b[i];double sign=dot<0?-1:1;dot=fmin(1.,fabs(dot));
 double l=1-u,r=u;
 if(1-dot>32*DBL_EPSILON){double theta=acos(dot),den=sin(theta);l=sin((1-u)*theta)/den;r=sin(u*theta)/den;}
 double norm=0;for(unsigned i=0;i<4;++i){out[i]=(float)(l*a[i]+r*sign*b[i]);norm+=(double)out[i]*out[i];}
 norm=sqrt(norm);for(unsigned i=0;i<4;++i)out[i]=(float)(out[i]/norm);
}
