/* Isolated shared-Q HUD candidate. Include immediately before the old HUD
 * update_body after HudReference helpers and diagnostic tape definitions.
 * BB_HUD_SHARED_Q=1 only. No native source/RNG/parameter writes.
 * Explicitly instrumented startup/reset holds are NOT claimed to reconstruct
 * unobserved pre-epoch physics. Native30 goes through the verified old path. */
#include "hud-shared-q-core.inc"
typedef struct {
 uintptr_t owner,state,clock;uint64_t generation,source_epoch,angular_order;
 unsigned busy,active,source_busy,adapted;int64_t last_time,last_q,reset_time;
 unsigned char canonical[0x84];HudReferenceInput identity;
 HsqVelocityCurve velocity;HsqInputCurve inputs;
} HsqConsumer;
static HsqConsumer hsq_consumers[HUD_REFERENCE_CAPACITY];
static SRWLOCK hsq_lock=SRWLOCK_INIT;static uint64_t hsq_generation;
static int hsq_installed;
static int hsq_requested(void){const char*p=camera_fix_getenv("BB_HUD_SHARED_Q");return p&&!strcmp(p,"1");}
static HsqConsumer*hsq_find(uintptr_t owner,int create){
 HsqConsumer*empty=NULL;for(unsigned i=0;i<HUD_REFERENCE_CAPACITY;++i){HsqConsumer*s=hsq_consumers+i;if(s->owner==owner)return s;if(!s->owner&&!empty)empty=s;}
 if(create&&empty){memset(empty,0,sizeof *empty);empty->owner=owner;empty->generation=++hsq_generation;empty->source_epoch=empty->generation;return empty;}return NULL;
}
static void hsq_forget(uintptr_t owner){
 AcquireSRWLockExclusive(&hsq_lock);for(unsigned i=0;i<HUD_REFERENCE_CAPACITY;++i)if(!owner||hsq_consumers[i].owner==owner){memset(hsq_consumers+i,0,sizeof hsq_consumers[i]);++hsq_generation;}ReleaseSRWLockExclusive(&hsq_lock);
}
static int hsq_clock(uintptr_t*clock,int64_t*time,int64_t*us,unsigned*frame){
 uintptr_t p=0;return camera_pose_read((uintptr_t)image+0x1911100,&p,8)&&camera_pose_read(p+0x970,&p,8)&&camera_pose_read(p+0x1e0,clock,8)&&
 camera_pose_read(*clock+0x58,time,8)&&camera_pose_read(*clock+0x60,us,8)&&camera_pose_read(*clock+0x21c,frame,4);
}
static uintptr_t hsq_setter_state(void*manager,uint32_t handle){
 int32_t index=(int32_t)(handle+UINT32_C(0xff0f3f01));uintptr_t owner=0,raw=0;
 uintptr_t slot=(uintptr_t)manager+(uintptr_t)((int64_t)index*INT64_C(0xb8))+0x38;
 return camera_pose_read(slot,&owner,8)&&owner&&camera_pose_read(owner+8,&raw,8)&&raw?raw+0x2ad0:0;
}
static void hsq_source_publish(uintptr_t state,uintptr_t clock,int64_t time,unsigned frame,const float velocity[3]){
 AcquireSRWLockExclusive(&hsq_lock);
 for(unsigned i=0;i<HUD_REFERENCE_CAPACITY;++i){HsqConsumer*s=hsq_consumers+i;if(!s->owner||s->state!=state)continue;
  if(s->clock!=clock||(s->velocity.count&&time<s->velocity.knots[s->velocity.count-1].t)){
   s->clock=clock;s->velocity.count=0;s->inputs.count=0;s->active=0;s->source_epoch=++hsq_generation;
  }
  int status=hsq_velocity_push(&s->velocity,time,velocity);
  float data[24]={(float)status};memcpy(data+1,&time,8);memcpy(data+3,velocity,12);memcpy(data+6,&s->source_epoch,8);data[8]=(float)frame;
  camera_accuracy_log(24,(void*)s->owner,state,0x7cb150,0,data,9);
 }
 ReleaseSRWLockExclusive(&hsq_lock);
}
typedef uintptr_t(ABI*HsqPublication)(void*,float);
typedef uintptr_t(ABI*HsqSetter)(void*,uint32_t,const void*);
static HsqPublication hsq_publication_original;static HsqSetter hsq_linear_original,hsq_angular_original;
static ABI uintptr_t hsq_publication(void*owner,float dt){
 if(lfpo_enabled)lfpo_hsq_before(owner,dt);
 uintptr_t result=hsq_publication_original(owner,dt);uint32_t fp;__asm__ volatile("stmxcsr %0":"=m"(fp));
 if(lfpo_enabled)lfpo_hsq_after(owner,dt);
 uintptr_t raw=0,clock=0;int64_t time=0,us=0;unsigned frame=0;float velocity[3];
 /* Original7CB150 reads10 for derivative and stores previous40;10 is NOT
  * changed by its derivative math. Capture here precedes later640900 feedback. */
 if(camera_pose_read((uintptr_t)owner+8,&raw,8)&&raw&&hsq_clock(&clock,&time,&us,&frame)&&camera_pose_read(raw+0x2ae0,velocity,12))
  hsq_source_publish(raw+0x2ad0,clock,time,frame,velocity);
 __asm__ volatile("ldmxcsr %0"::"m"(fp):"memory");return result;
}
static void hsq_setter_begin(uintptr_t state,unsigned bit){
 if(!state)return;AcquireSRWLockExclusive(&hsq_lock);
 for(unsigned i=0;i<HUD_REFERENCE_CAPACITY;++i)if(hsq_consumers[i].owner&&hsq_consumers[i].state==state)hsq_consumers[i].source_busy|=bit;
 ReleaseSRWLockExclusive(&hsq_lock);
}
static void hsq_setter_end(uintptr_t state,unsigned bit){
 uintptr_t clock=0;int64_t time=0,us=0;unsigned frame=0;float velocity[3]={0};
 int valid=state&&hsq_clock(&clock,&time,&us,&frame)&&camera_pose_read(state+0x10,velocity,12);
 AcquireSRWLockExclusive(&hsq_lock);
 for(unsigned i=0;i<HUD_REFERENCE_CAPACITY;++i){HsqConsumer*s=hsq_consumers+i;if(!s->owner||s->state!=state)continue;
  s->source_busy&=~bit;
  if(bit==1){
   s->velocity.count=0;s->inputs.count=0;s->source_epoch=++hsq_generation;s->reset_time=time;
   /* A real native641220 reset rebases velocity/history and zeroes B0. Never
    * connect the old moving velocity to its new zero across this epoch. */
   if(valid){s->clock=clock;hsq_velocity_push(&s->velocity,time,velocity);}
  }else ++s->angular_order; /* authored90zero; neverderive a replacement. */
  float data[24]={(float)bit,(float)valid};memcpy(data+2,&time,8);memcpy(data+4,&s->source_epoch,8);memcpy(data+6,&s->angular_order,8);
  camera_accuracy_log(25,(void*)s->owner,state,bit==1?0x641220:0x6415f0,0,data,8);
 }
 ReleaseSRWLockExclusive(&hsq_lock);
}
static ABI uintptr_t hsq_linear_setter(void*manager,uint32_t handle,const void*vector){
 uint32_t before,after;__asm__ volatile("stmxcsr %0":"=m"(before));uintptr_t state=hsq_setter_state(manager,handle);hsq_setter_begin(state,1);
 __asm__ volatile("ldmxcsr %0"::"m"(before):"memory");uintptr_t result=hsq_linear_original(manager,handle,vector);
 __asm__ volatile("stmxcsr %0":"=m"(after));hsq_setter_end(state,1);replay_linear_q_body_reset(state);__asm__ volatile("ldmxcsr %0"::"m"(after):"memory");return result;
}
static ABI uintptr_t hsq_angular_setter(void*manager,uint32_t handle,const void*vector){
 uint32_t before,after;__asm__ volatile("stmxcsr %0":"=m"(before));uintptr_t state=hsq_setter_state(manager,handle);hsq_setter_begin(state,2);
 __asm__ volatile("ldmxcsr %0"::"m"(before):"memory");uintptr_t result=hsq_angular_original(manager,handle,vector);
 __asm__ volatile("stmxcsr %0":"=m"(after));hsq_setter_end(state,2);__asm__ volatile("ldmxcsr %0"::"m"(after):"memory");return result;
}
static int hsq_install(void){
 if(!hsq_requested())return 0;
 static const unsigned char publication[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x38,0xc5,0xfa,0x11,0x45,0xbc};
 static const unsigned char linear[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38};
 static const unsigned char angular[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x20};
 if(runtime_image_size<0x7cb150+sizeof publication||memcmp(image+0x7cb150,publication,sizeof publication)||memcmp(image+0x641220,linear,sizeof linear)||memcmp(image+0x6415f0,angular,sizeof angular)){
  puts("HUDSharedQ: unsupported/conflicting native source signatures; unavailable");return 0;
 }
 hsq_publication_original=(HsqPublication)camera_rate_hook(0x7cb150,publication,sizeof publication,(uintptr_t)hsq_publication);
 hsq_linear_original=(HsqSetter)camera_rate_hook(0x641220,linear,sizeof linear,(uintptr_t)hsq_linear_setter);
 hsq_angular_original=(HsqSetter)camera_rate_hook(0x6415f0,angular,sizeof angular,(uintptr_t)hsq_angular_setter);
 hsq_installed=hsq_publication_original&&hsq_linear_original&&hsq_angular_original;
 printf("HUDSharedQ: source7CB150 + reset641220 + authored90setter6415F0 %s; nativeQ passthrough\n",hsq_installed?"on":"unavailable");return hsq_installed;
}
/* Returns1 when this route consumed/held a VRR update,0 for strictnativeQ. */
static int hsq_update_impl(void*self,void*subject,float dt,uint32_t entry_fp,uint32_t*exit_fp){
 if(!hsq_requested()||!hsq_installed)return 0;
 int64_t dtus=0;
 if(!hud_reference_native_us(dt,&dtus)){hsq_forget((uintptr_t)self);return 0;}
 AcquireSRWLockExclusive(&hsq_lock);HsqConsumer*mode=hsq_find((uintptr_t)self,1);
 if(!mode){ReleaseSRWLockExclusive(&hsq_lock);return 0;}
 /* A Q-sized frame can be an ordinary VRR stall. Once this real object epoch
  * has seen nonQ time, preserve shared-Q history on all laterQ callbacks.
  * An allQ native30 epoch stays on the original path without any HUD writes. */
 if(dtus==HSQ_Q_US&&!mode->adapted){ReleaseSRWLockExclusive(&hsq_lock);return 0;}
 mode->adapted=1;ReleaseSRWLockExclusive(&hsq_lock);
 uintptr_t clock=0;int64_t time=0,us=0;unsigned frame=0;HudReferenceInput input;
 if(!hsq_clock(&clock,&time,&us,&frame)||dtus!=us||!hud_reference_capture(subject,dt,&input)){
  hsq_forget((uintptr_t)self);return 0;
 }
 if(input.kind!=4||time<0){hsq_forget((uintptr_t)self);return 0;}
 /* Source input geometry records retain actual native selectedB0, not the
  * synthetic supplied Q descriptor that the original kernel will consume. */
 HudTapeScope observe;hud_tape_begin(&observe,self,subject,dt);hud_tape_input_raw(input.observation,input.target_ptr,input.state_ptr,input.kind,input.vectors);hud_tape_current=observe.previous;
 AcquireSRWLockExclusive(&hsq_lock);HsqConsumer*s=hsq_find((uintptr_t)self,1);
 if(!s||s->busy||s->source_busy){ReleaseSRWLockExclusive(&hsq_lock);return 1;}
 int reset=!s->active||s->state!=input.state_ptr||s->clock!=clock||!hud_reference_same_input(&s->identity,&input)||time<s->last_time||memcmp(s->canonical,(char*)self+8,0x84);
 if(reset){
  if(s->state!=input.state_ptr||s->clock!=clock){s->velocity.count=0;s->reset_time=0;s->source_epoch=++hsq_generation;}
  s->state=input.state_ptr;s->clock=clock;s->identity=input;s->active=1;s->inputs.count=0;s->generation=++hsq_generation;
  memcpy(s->canonical,(char*)self+8,0x84);
  /* First canonical state has no authoritative pre-epoch Q input. Observe
   * through the next full boundary; no fabricated earlier velocity or dt. */
  s->last_q=((time+HSQ_Q_US-1)/HSQ_Q_US)*HSQ_Q_US;
 }
 s->last_time=time;hsq_input_push(&s->inputs,time,s->angular_order,input.vectors);
 if(s->velocity.count&&s->inputs.count){
  int64_t earliest=s->velocity.knots[0].t>s->inputs.knots[0].t?s->velocity.knots[0].t:s->inputs.knots[0].t;
  if(s->last_q<earliest)s->last_q=((earliest+HSQ_Q_US-1)/HSQ_Q_US)*HSQ_Q_US;
 }
 uint64_t generation=s->generation,epoch=s->source_epoch;int status=reset?2:0;unsigned ticks=0;
 s->busy=1;
 while(s->last_q+HSQ_Q_US<=time){
  int64_t q=s->last_q+HSQ_Q_US;float vectors[4][4],accel[3];
  if(!hsq_inputs_at(&s->inputs,q,vectors)||!hsq_linear_acceleration(&s->velocity,q,accel)){status=3;break;}
  memcpy(vectors[0],accel,12);
  HudReferencePrivate supplied;memset(&supplied,0,sizeof supplied);memcpy(supplied.kind,&input.kind,4);
  memcpy(supplied.state+0xb0,vectors[0],16);memcpy(supplied.target+0x140,vectors[1],16);memcpy(supplied.state+0x90,vectors[2],16);memcpy(supplied.state+0x110,vectors[3],16);
  supplied.descriptor[0]=(uintptr_t)supplied.kind;supplied.descriptor[1]=(uintptr_t)supplied.target;supplied.descriptor[2]=(uintptr_t)supplied.state;
  memcpy((char*)self+8,s->canonical,0x84);ReleaseSRWLockExclusive(&hsq_lock);
  uintptr_t*saved=hud_reference_supplied_observation;hud_reference_supplied_observation=supplied.descriptor;
  __asm__ volatile("ldmxcsr %0"::"m"(entry_fp):"memory");
  HudTapeScope native_q_tape;hud_tape_begin(&native_q_tape,self,subject,hud_reference_Q);
  ++hud_tape_private_input;hud_tape_input_raw(input.observation,(uintptr_t)supplied.target,(uintptr_t)supplied.state,input.kind,vectors);--hud_tape_private_input;
  __asm__ volatile("ldmxcsr %0"::"m"(entry_fp):"memory");
  replay_hud_update_original(self,subject,hud_reference_Q);
  __asm__ volatile("stmxcsr %0":"=m"(*exit_fp));
  hud_tape_end(&native_q_tape);hud_reference_supplied_observation=saved;
  unsigned char after[0x84];memcpy(after,(char*)self+8,0x84);
  AcquireSRWLockExclusive(&hsq_lock);s=hsq_find((uintptr_t)self,0);
  if(!s||s->generation!=generation||s->source_epoch!=epoch||s->source_busy){if(s)s->busy=0;ReleaseSRWLockExclusive(&hsq_lock);return 1;}
  memcpy(s->canonical,after,0x84);s->last_q=q;++ticks;status=1;
 }
 /* True native reset epoch: discard unavailable old-Q backlog and re-prime
  * at the next complete post-reset interval. This hold is diagnosed, not
  * claimed exact reproduction of unobserved constructor/reset inputs. */
 if(status==3&&s->reset_time&&s->last_q<s->reset_time){s->last_q=((s->reset_time+HSQ_Q_US-1)/HSQ_Q_US)*HSQ_Q_US;status=4;}
 s->busy=0;
 float data[24]={(float)status,(float)ticks,(float)reset};memcpy(data+3,&time,8);memcpy(data+5,&s->last_q,8);memcpy(data+7,&epoch,8);data[9]=(float)s->velocity.count;data[10]=(float)s->inputs.count;
 camera_accuracy_log(26,self,input.state_ptr,frame,dt,data,11);
 ReleaseSRWLockExclusive(&hsq_lock);return 1;
}
static int hsq_update(void*self,void*subject,float dt){
 uint32_t entry_fp,exit_fp;__asm__ volatile("stmxcsr %0":"=m"(entry_fp));exit_fp=entry_fp;
 int result=hsq_update_impl(self,subject,dt,entry_fp,&exit_fp);
 __asm__ volatile("ldmxcsr %0"::"m"(exit_fp):"memory");return result;
}
