#ifndef HEAD_PAUSE_CLOCK_CORE_INCLUDED
#define HEAD_PAUSE_CLOCK_CORE_INCLUDED
/* Exact original logical TimeCycle pause continuation. No guessed origin,
 * elapsed rescale, TTL, matrix relaxation or fitted phase. */
typedef struct {
 uintptr_t owner,vehicle,clock;int64_t logical,timer,time,elapsed;
 uint32_t frame;unsigned valid,paused;
} HpsAnchor;
typedef struct {
 uintptr_t owner,vehicle,clock;int64_t logical,timer,elapsed;
 uint32_t frame;unsigned valid,paused;
} HpsSample;
static int hps_add(int64_t a,int64_t b,int64_t*out){if(b>0&&a>INT64_MAX-b)return 0;if(b<0&&a<INT64_MIN-b)return 0;*out=a+b;return 1;}
static int hps_core(HpsAnchor*s,const HpsSample*x,int64_t*out){
 if(!x->valid||!x->owner||!x->clock||x->logical<0||x->timer<0||x->elapsed<=0||x->elapsed>8*33333){memset(s,0,sizeof*s);return 0;}
 if(!x->paused){*s=(HpsAnchor){x->owner,x->vehicle,x->clock,x->logical,x->timer,x->logical,x->elapsed,x->frame,1,0};*out=x->logical;return 1;}
 if(!s->valid||s->owner!=x->owner||s->vehicle!=x->vehicle||s->clock!=x->clock||s->logical!=x->logical){memset(s,0,sizeof*s);return 0;}
 if(s->paused&&s->frame==x->frame&&s->timer==x->timer&&s->elapsed==x->elapsed){*out=s->time;return 2;}
 if(x->frame!=s->frame+1u||x->timer<s->timer||x->timer-s->timer!=x->elapsed||!hps_add(s->time,x->elapsed,out)){memset(s,0,sizeof*s);return 0;}
 s->timer=x->timer;s->time=*out;s->frame=x->frame;s->elapsed=x->elapsed;s->paused=1;return 2;
}
#endif
