#ifndef HUD_SHARED_Q_CORE_INCLUDED
#define HUD_SHARED_Q_CORE_INCLUDED
/* Time-stamped source sampling only. No engine memory, RNG, fitted gain,
 * invented angular derivative or wall-clock heuristic. Ring size is a bounded
 * storage limit; inability to bracket is explicit, never extrapolated. */
#define HSQ_Q_US INT64_C(33333)
#define HSQ_SAMPLES 256
typedef struct {int64_t t;float v[3];} HsqVelocityKnot;
typedef struct {unsigned count;HsqVelocityKnot knots[HSQ_SAMPLES];} HsqVelocityCurve;
static int hsq_velocity_push(HsqVelocityCurve*h,int64_t t,const float v[3]){
 for(unsigned j=0;j<3;++j)if(!isfinite(v[j]))return 0;
 if(h->count&&t<h->knots[h->count-1].t)return 0;
 if(h->count&&t==h->knots[h->count-1].t){memcpy(h->knots[h->count-1].v,v,12);return 2;}
 if(h->count==HSQ_SAMPLES){memmove(h->knots,h->knots+1,(HSQ_SAMPLES-1)*sizeof h->knots[0]);--h->count;}
 HsqVelocityKnot*k=h->knots+h->count++;k->t=t;memcpy(k->v,v,12);return 1;
}
static int hsq_velocity_at(const HsqVelocityCurve*h,int64_t t,float v[3]){
 if(!h->count||t<h->knots[0].t||t>h->knots[h->count-1].t)return 0;
 for(unsigned i=0;i<h->count;++i){const HsqVelocityKnot*b=h->knots+i;
  if(t==b->t){memcpy(v,b->v,12);return 1;}
  if(b->t>t){const HsqVelocityKnot*a=b-1;double u=(double)(t-a->t)/(double)(b->t-a->t);
   for(unsigned j=0;j<3;++j)v[j]=(float)((double)a->v[j]+((double)b->v[j]-a->v[j])*u);return 1;}
 }return 0;
}
static int hsq_linear_acceleration(const HsqVelocityCurve*h,int64_t q,float a[3]){
 float before[3],after[3];if(!hsq_velocity_at(h,q-HSQ_Q_US,before)||!hsq_velocity_at(h,q,after))return 0;
 /* Actual7CB150 uses physicsfloat1/30, not camera33333usfloat. */
 volatile float physics_q=1.f/30.f;volatile float inverse=1.f/physics_q;
 for(unsigned j=0;j<3;++j){volatile float delta=after[j]-before[j];a[j]=delta*inverse;if(!isfinite(a[j]))return 0;}return 1;
}
typedef struct {int64_t t;uint64_t angular_order;float vectors[4][4];} HsqInputKnot;
typedef struct {unsigned count;HsqInputKnot knots[HSQ_SAMPLES];} HsqInputCurve;
static int hsq_input_push(HsqInputCurve*h,int64_t t,uint64_t angular_order,const float vectors[4][4]){
 if(h->count&&t<h->knots[h->count-1].t)return 0;
 HsqInputKnot*k;
 if(h->count&&t==h->knots[h->count-1].t)k=h->knots+h->count-1;
 else {if(h->count==HSQ_SAMPLES){memmove(h->knots,h->knots+1,(HSQ_SAMPLES-1)*sizeof h->knots[0]);--h->count;}k=h->knots+h->count++;}
 k->t=t;k->angular_order=angular_order;memcpy(k->vectors,vectors,sizeof k->vectors);return 1;
}
static int hsq_inputs_at(const HsqInputCurve*h,int64_t q,float vectors[4][4]){
 if(!h->count||q<h->knots[0].t||q>h->knots[h->count-1].t)return 0;
 for(unsigned i=0;i<h->count;++i){const HsqInputKnot*b=h->knots+i;
  if(q==b->t){memcpy(vectors,b->vectors,sizeof b->vectors);return 1;}
  if(b->t>q){const HsqInputKnot*a=b-1;double u=(double)(q-a->t)/(double)(b->t-a->t);
   for(unsigned v=0;v<4;++v){for(unsigned j=0;j<3;++j){
    /* Native angular setter is an ordered discontinuity, never smear its zero
     * backwards to the preceding publication. The actual90 vector is retained. */
    if(v==2&&a->angular_order!=b->angular_order)vectors[v][j]=a->vectors[v][j];
    else vectors[v][j]=(float)((double)a->vectors[v][j]+((double)b->vectors[v][j]-a->vectors[v][j])*u);
   }vectors[v][3]=b->vectors[v][3];}return 1;
  }
 }return 0;
}
#endif
