/* Functional input collector, independent of all diagnostic maps/files. It
 * never selects a response itself. External epoch source must be registered. */
typedef uint64_t(*HprGroupEpochProvider)(uintptr_t actor);
static HprGroupEpochProvider hpr_group_epoch_provider;
static int hpr_group_enabled;
static int hpr_group_active_for_head(void){return hpr_group_enabled;}
typedef struct{const HKTRecord*slot;uint64_t serial,epoch;uintptr_t actor,main;uint32_t frame,thread;} HprGroupHeadPending;
typedef struct{HKTRecord head;HprRecord primitives;uint64_t serial,epoch;unsigned head_valid,ready,consumed;} HprGroupInputs;
static _Thread_local HprGroupHeadPending hpr_group_heads[4];
static _Thread_local unsigned hpr_group_head_depth;
static _Thread_local uint64_t hpr_group_serial;
static _Thread_local HprGroupInputs hpr_group_inputs;
static __attribute__((used))void hpr_group_bind_epoch_provider(HprGroupEpochProvider provider){hpr_group_epoch_provider=provider;}
static int hpr_group_requested(void){const char*camera=camera_fix_getenv("BB_CAMERA_PATCH"),*group=camera_fix_getenv("BB_CAMERA_HEAD_GROUP");return camera&&!strcmp(camera,"1")&&group&&!strcmp(group,"1");}
static void hpr_group_head_before(const HKTRecord*r){
 if(!hpr_group_enabled)return;hpr_group_inputs.head_valid=0;hpr_group_inputs.ready=0;hpr_group_inputs.consumed=0;
 uint64_t serial=++hpr_group_serial,epoch=hpr_group_epoch_provider?hpr_group_epoch_provider((uintptr_t)r->actor):0;
 for(unsigned i=0;i<hpr_group_head_depth;++i)if(hpr_group_heads[i].slot==r){memmove(hpr_group_heads+i,hpr_group_heads+i+1,(hpr_group_head_depth-i-1)*sizeof(HprGroupHeadPending));--hpr_group_head_depth;return;}
 if(!epoch||!(r->flags&HKT_FRAME_VALID)||hpr_group_head_depth>=4)return;
 HprGroupHeadPending*p=hpr_group_heads+hpr_group_head_depth++;*p=(HprGroupHeadPending){r,serial,epoch,(uintptr_t)r->actor,(uintptr_t)r->frame_address,r->frame,r->thread};
}
static void hpr_group_head_after(const HKTRecord*r){
 if(!hpr_group_enabled)return;unsigned at=hpr_group_head_depth;for(unsigned i=hpr_group_head_depth;i>0;--i)if(hpr_group_heads[i-1].slot==r){at=i-1;break;}if(at==hpr_group_head_depth)return;
 HprGroupHeadPending p=hpr_group_heads[at];memmove(hpr_group_heads+at,hpr_group_heads+at+1,(hpr_group_head_depth-at-1)*sizeof(HprGroupHeadPending));--hpr_group_head_depth;
 uint64_t epoch=hpr_group_epoch_provider?hpr_group_epoch_provider((uintptr_t)r->actor):0;
 if(epoch!=p.epoch||!epoch||p.actor!=r->actor||p.main!=r->frame_address||p.frame!=r->frame||p.thread!=r->thread||r->thread!=GetCurrentThreadId()||!(r->flags&HKT_READ_COMPLETE))return;
 hpr_group_inputs.head=*r;hpr_group_inputs.serial=p.serial;hpr_group_inputs.epoch=p.epoch;hpr_group_inputs.head_valid=1;hpr_group_inputs.ready=0;hpr_group_inputs.consumed=0;
}
static uint64_t hpr_group_helper_token(uintptr_t actor,uintptr_t main,uint32_t frame,uint32_t thread){
 HprGroupInputs*s=&hpr_group_inputs;if(!hpr_group_enabled||!s->head_valid||s->head.actor!=actor||s->head.frame_address!=main||s->head.frame!=frame||s->head.thread!=thread||thread!=GetCurrentThreadId())return 0;
 uint64_t epoch=hpr_group_epoch_provider?hpr_group_epoch_provider(actor):0;return epoch&&epoch==s->epoch?s->serial:0;
}
static void hpr_group_record_complete(const HprEntry*e,const HprRecord*r){
 HprGroupInputs*s=&hpr_group_inputs;if(!hpr_group_enabled||!e->group_head_serial||!s->head_valid||e->group_head_serial!=s->serial||r->owner!=s->head.actor||r->main_frame!=s->head.frame_address||r->helper_frame_number!=s->head.frame||r->thread!=s->head.thread||r->thread!=GetCurrentThreadId())return;
 uint64_t epoch=hpr_group_epoch_provider?hpr_group_epoch_provider((uintptr_t)r->owner):0;
 if(!epoch||epoch!=s->epoch||(r->flags&(HPR_SCOPE|HPR_CLOCK|HPR_HELPER_EXIT|HPR_HEAD_ENTRY|HPR_HEAD_EXIT|HPR_PARENT_MODE|HPR_KINEMATIC_ANGLE))!=(HPR_SCOPE|HPR_CLOCK|HPR_HELPER_EXIT|HPR_HEAD_ENTRY|HPR_HEAD_EXIT|HPR_PARENT_MODE|HPR_KINEMATIC_ANGLE))return;
 s->primitives=*r;s->ready=1;s->consumed=0;
}
/* Call under the publisher's shared reset lease. The provider itself must be
 * a non-locking monotonic epoch read; no duplicate epoch table or TTL here. */
static __attribute__((used))int hpr_group_current(uintptr_t actor,uintptr_t main,uint32_t frame,uint32_t thread,const HKTRecord**head,const HprRecord**primitives,uint64_t*epoch,uint64_t*serial){
 HprGroupInputs*s=&hpr_group_inputs;if(!hpr_group_enabled||!s->head_valid||!s->ready||s->consumed||s->head.actor!=actor||s->head.frame_address!=main||s->head.frame!=frame||s->head.thread!=thread||thread!=GetCurrentThreadId())return 0;
 uint64_t now=hpr_group_epoch_provider?hpr_group_epoch_provider(actor):0;if(!now||now!=s->epoch)return 0;
 *head=&s->head;*primitives=&s->primitives;*epoch=s->epoch;*serial=s->serial;s->consumed=1;return 1;
}
