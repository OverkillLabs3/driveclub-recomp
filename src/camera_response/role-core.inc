/* Bounded PRIVATE observed-native-birth roles. Entry observations do NOT mean
 * construction completed. Eligibility is queried only on a coherent original
 * consumer-selected actor/provider/reader with the native class guards. */
#define RLR_CAPACITY 4096u
typedef struct{uintptr_t ptr;uint64_t birth,mutation;unsigned kind,alive,busy,source_attempt;} RlrSlot;
typedef struct{RlrSlot slots[RLR_CAPACITY];uint64_t serial;unsigned overflow;} RlrRegistry;
static RlrSlot*rlr_find(RlrRegistry*r,unsigned kind,uintptr_t ptr,int create){
 if(!ptr||!kind)return NULL;unsigned at=(unsigned)(((ptr>>4)^(ptr>>23)^((uintptr_t)kind*UINT32_C(2654435761)))&(RLR_CAPACITY-1));
 for(unsigned i=0;i<RLR_CAPACITY;++i){RlrSlot*s=r->slots+((at+i)&(RLR_CAPACITY-1));if(s->ptr==ptr&&s->kind==kind)return s;if(!s->ptr){if(!create)return NULL;memset(s,0,sizeof*s);s->ptr=ptr;s->kind=kind;return s;}}
 if(create)r->overflow=1;return NULL;
}
static unsigned rlr_kind(unsigned stage){return stage<8?1:stage<15?2:stage<17?3:4;}
static int rlr_birth(unsigned stage){return stage==0||stage==8||stage==9||stage==15||stage==17;}
static int rlr_death(unsigned stage){return stage==1||stage==2||stage==3||stage==10||stage==16;}
static uint64_t rlr_begin(RlrRegistry*r,unsigned stage,uintptr_t owner){
 if(stage>=18||!owner)return 0;uint64_t tag=++r->serial;if(!tag){r->overflow=1;return 0;}
 RlrSlot*s=rlr_find(r,rlr_kind(stage),owner,rlr_birth(stage));if(!s)return tag;
 s->mutation=tag;s->busy=1;
 if(rlr_birth(stage)){s->birth=tag;s->alive=0;s->source_attempt=0;}
 if(rlr_death(stage))s->alive=0;
 if(stage==5)s->source_attempt=1;
 if(stage==16){for(unsigned i=0;i<2;++i){RlrSlot*t=rlr_find(r,4,owner+(i?0x28:0),0);if(t){t->alive=0;t->busy=0;t->mutation=tag;}}}
 return tag;
}
static void rlr_end(RlrRegistry*r,unsigned stage,uintptr_t owner,uint64_t tag){
 if(!tag||stage>=18)return;RlrSlot*s=rlr_find(r,rlr_kind(stage),owner,0);if(!s||s->mutation!=tag)return;
 s->busy=0;if(rlr_birth(stage))s->alive=1;
}
static int rlr_binding(RlrRegistry*r,uintptr_t actor,uintptr_t provider,uintptr_t reader,uint64_t*token){
 if(token)*token=0;if(r->overflow||reader>UINTPTR_MAX-0x28)return 0;
 RlrSlot*a=rlr_find(r,1,actor,0),*p=rlr_find(r,2,provider,0),*v=rlr_find(r,3,reader,0),*s=rlr_find(r,4,reader,0),*t=rlr_find(r,4,reader+0x28,0);
 if(!a||!p||!v||!s||!t||!a->source_attempt)return 0;
 RlrSlot*all[]={a,p,v,s,t};for(unsigned i=0;i<5;++i)if(!all[i]->birth||!all[i]->alive||all[i]->busy)return 0;
 /* Native reader creation/publication follows its owning provider construction;
  * both embedded stream bindings occur inside the reader constructor. */
 if(v->birth<=p->birth||s->birth<=v->birth||t->birth<=v->birth)return 0;
 if(token)*token=r->serial;return 1;
}
