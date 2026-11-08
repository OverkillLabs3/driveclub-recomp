/* Opt-in READ-ONLY per-helper primitive transaction. Native getters/calls and
 * coefficients are unchanged. Separate from frozen HDPRIM01. */
typedef struct{unsigned char magic[8];uint32_t version,record_size,capacity,count,dropped,invalid,unpaired,skipped;uint64_t frequency,reserved[2];} HprHeader;
typedef struct{
 uint64_t sequence,qpc_before,qpc_after,call_id,owner,helper_frame,main_frame,entry_rsp,delay_pointer,vector_pointer,caller,clock;
 int64_t logical_T,logical_dt;
 uint32_t frame,thread,flags,mxcsr_before,mxcsr_after,source_flags,gate,parent_mode_bl;
 uint64_t result_rax;
 float raw_axes[2],look_old,look_target,look_cache[4],xmm[8][4];
 unsigned char stack_arguments[40],head_before[0x380],head_after[0x380];
 float vector_before[4],vector_after[4],delay_before,delay_after;
 uint64_t source_call_id;
 float accum_before[2];uint64_t input_rax;
 /* Explicit additional snapshots: CE-before/after above retain their v1 meaning.
  * For direct-service rows CE fields/flags remain absent, never relabeled. */
 unsigned char head_entry[0x380],head_exit[0x380];float helper_xmm[2][4];
 uint64_t helper_qpc_before,helper_qpc_after;
 uint32_t helper_mxcsr_before,helper_mxcsr_after;
 uint64_t helper_clock;int64_t helper_logical_T,helper_logical_dt;
 uint32_t helper_frame_number,helper_thread;float kinematic_angle;uint32_t kinematic_valid;
} HprRecord;
_Static_assert(sizeof(HprHeader)==64,"HPR2 header64");
_Static_assert(sizeof(HprRecord)==4096,"HPR2 record4096");
enum{HPR_SCOPE=1,HPR_CLOCK=2,HPR_BEFORE=4,HPR_AFTER=8,HPR_RAW_AXES=16,HPR_LOOK=32,HPR_ENTRY=64,HPR_VECTOR_OLD200=128,HPR_CE_PRESENT=256,HPR_DIRECT_SOURCE=512,HPR_HELPER_EXIT=1024,HPR_HEAD_ENTRY=2048,HPR_HEAD_EXIT=4096,HPR_PARENT_MODE=8192,HPR_KINEMATIC_ANGLE=16384};
typedef struct{uint64_t id;uintptr_t actor,helper,main,clock;int64_t T,dt;uint32_t frame,thread,flags,consumed,ce_seen,ce_ready,parent_mode_bl,mxcsr;uint64_t group_head_serial;float axes[2],accum[2],args[2][4],kinematic_angle;uint64_t qpc;unsigned char head_entry[0x380];HprRecord completed;} HprEntry;
typedef struct{uintptr_t actor,main,clock;int64_t T;uint32_t frame,thread,valid;float old,target,cache[4];} HprLook;
static HprHeader*hpr_header;static HprRecord*hpr_records;static SRWLOCK hpr_tape_lock=SRWLOCK_INIT;static volatile LONG64 hpr_serial;
static _Thread_local HprEntry hpr_entries[4];static _Thread_local unsigned hpr_entry_count;
static _Thread_local HprLook hpr_looks[4];
static _Thread_local HprRecord hpr_pending[4];static _Thread_local unsigned hpr_depth;
static __attribute__((used))uintptr_t hpr_ce_original,hpr_entry_original,hpr_raw_original,hpr_look_original,hpr_common_original;
static __attribute__((naked,used))ABI void hpr_post_bridge(void);
#include "head-group-functional-primitives.inc"
#ifdef HPR_TEST_CLOBBER
static uint32_t hpr_test_csr=0x7fa0;
static __attribute__((noinline))ABI void hpr_test_clobber(void){__asm__ volatile("vzeroall\nfninit\nfld1\nldmxcsr %0"::"m"(hpr_test_csr):"xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15","cc","memory");}
#else
#define hpr_test_clobber() ((void)0)
#endif
static uint64_t hpr_qpc(void){LARGE_INTEGER v;QueryPerformanceCounter(&v);return(uint64_t)v.QuadPart;}
static void hpr_count(uint32_t*p){if(p)InterlockedIncrement((volatile LONG*)p);}
#define HPR_DIAG_COUNT(member) do{if(hpr_header)hpr_count(&hpr_header->member);}while(0)
static int hpr_clock(uintptr_t*clock,int64_t*T,int64_t*dt,uint32_t*frame){uintptr_t p=0,c=0;int64_t a[2],b[2];uint32_t f=0,g=0;
 if(!camera_pose_read((uintptr_t)image+0x1911100,&p,8)||!camera_pose_read(p+0x968,&p,8)||!camera_pose_read(p+0x1e0,&c,8)||!camera_pose_read(c+0x58,a,16)||!camera_pose_read(c+0x21c,&f,4)||!camera_pose_read(c+0x58,b,16)||!camera_pose_read(c+0x21c,&g,4)||memcmp(a,b,16)||f!=g)return 0;
 *clock=c;*T=a[0];*dt=a[1];*frame=f;return 1;
}
static int hpr_main(uintptr_t helper,uintptr_t actor,uintptr_t*main){uintptr_t caller=0,saved_actor=0,parent_caller=0;
 return helper>=0x10000&&actor>=0x10000&&camera_pose_read(helper,main,8)&&camera_pose_read(helper+8,&caller,8)&&caller==(uintptr_t)image+0xb921d2&&camera_pose_read(helper-8,&saved_actor,8)&&saved_actor==actor&&camera_pose_read(*main+8,&parent_caller,8)&&parent_caller==(uintptr_t)image+0xb959f9;
}
static HprEntry*hpr_find_entry(uintptr_t actor,uintptr_t helper){for(unsigned i=hpr_entry_count;i>0;--i)if(hpr_entries[i-1].actor==actor&&hpr_entries[i-1].helper==helper)return hpr_entries+i-1;return NULL;}
static void hpr_helper_entry(unsigned char*c){uintptr_t actor=0,main=0,caller=0,saved_actor=0,pc=0;memcpy(&actor,c+1096,8);memcpy(&main,c+1112,8);memcpy(&caller,c+1160,8);memcpy(&saved_actor,c+1032,8);uintptr_t helper=(uintptr_t)c+1160-8;
 if(caller!=(uintptr_t)image+0xb921d2||actor!=saved_actor||!camera_pose_read(main+8,&pc,8)||pc!=(uintptr_t)image+0xb959f9)return;
 while(hpr_entry_count&&hpr_entries[hpr_entry_count-1].helper<=helper)--hpr_entry_count;
 if(hpr_entry_count>=4){HPR_DIAG_COUNT(skipped);return;}HprEntry*e=hpr_entries+hpr_entry_count++;memset(e,0,sizeof*e);e->id=(uint64_t)InterlockedIncrement64(&hpr_serial);e->actor=actor;e->helper=helper;e->main=main;e->thread=GetCurrentThreadId();e->qpc=hpr_qpc();
 memcpy(&e->parent_mode_bl,c+1120,1);e->flags|=HPR_PARENT_MODE;memcpy(&e->mxcsr,c+24,4);for(unsigned i=0;i<2;++i)memcpy(e->args[i],c+512+i*32,16);
 if(hpr_clock(&e->clock,&e->T,&e->dt,&e->frame))e->flags|=HPR_CLOCK;e->group_head_serial=hpr_group_helper_token(actor,main,e->frame,e->thread);if(camera_pose_read(actor+0x354,e->accum,8))e->flags|=HPR_ENTRY;
 /* Native B920A2 stores original atan2f output here, before response1E4/2B0
  * assembly at B92160. Capture, do not invert the rounded derived response. */
 if(camera_pose_read(main-0x110,&e->kinematic_angle,4))e->flags|=HPR_KINEMATIC_ANGLE;else HPR_DIAG_COUNT(invalid);
 if(camera_pose_read(actor,e->head_entry,sizeof e->head_entry))e->flags|=HPR_HEAD_ENTRY;else HPR_DIAG_COUNT(invalid);
}
static void hpr_raw(unsigned char*c){uintptr_t actor=0,helper=0,main=0;memcpy(&actor,c+1040,8);memcpy(&helper,c+1120,8);HprEntry*e=hpr_find_entry(actor,helper);
 if(!e||e->consumed||!hpr_main(helper,actor,&main)||main!=e->main){HPR_DIAG_COUNT(invalid);return;}
 if(camera_pose_read(helper-0x48,e->axes,4)&&camera_pose_read(helper-0x44,e->axes+1,4))e->flags|=HPR_RAW_AXES;else HPR_DIAG_COUNT(invalid);
}
static void hpr_look(unsigned char*c){uintptr_t actor=0,helper=0,main=0,pc=0;memcpy(&actor,c+1064,8);memcpy(&helper,c+1120,8);
 if(!camera_pose_read(helper,&main,8)||!camera_pose_read(helper+8,&pc,8)||pc!=(uintptr_t)image+0xb9219c||!camera_pose_read(main+8,&pc,8)||pc!=(uintptr_t)image+0xb959f9)return;
 HprLook*slot=NULL,*empty=NULL;for(unsigned i=0;i<4;++i){if(hpr_looks[i].actor==actor){slot=hpr_looks+i;break;}if(!hpr_looks[i].valid&&!empty)empty=hpr_looks+i;}if(!slot)slot=empty;if(!slot){HPR_DIAG_COUNT(skipped);return;}
 memset(slot,0,sizeof*slot);slot->actor=actor;slot->main=main;slot->thread=GetCurrentThreadId();int64_t dt=0;
 if(hpr_clock(&slot->clock,&slot->T,&dt,&slot->frame)&&camera_pose_read(actor+0x2b0,&slot->old,4)&&camera_pose_read(actor+0x2b4,&slot->target,4)&&camera_pose_read(actor+0x2c0,slot->cache,16))slot->valid=1;else HPR_DIAG_COUNT(invalid);
}
static void hpr_attach_look(HprRecord*r,unsigned char enabled){if(!enabled)return;for(unsigned i=0;i<4;++i){HprLook*x=hpr_looks+i;if(x->valid&&x->actor==r->owner&&x->main==r->main_frame&&x->thread==r->thread&&x->clock==r->clock&&x->T==r->logical_T&&x->frame==r->frame){r->look_old=x->old;r->look_target=x->target;memcpy(r->look_cache,x->cache,16);r->flags|=HPR_LOOK;x->valid=0;break;}}}
static void hpr_ce(unsigned char*c){uintptr_t caller=0,delay=0,vector=0,actor=0,helper=0,main=0;memcpy(&caller,c+1160,8);if(caller!=(uintptr_t)image+0xb92f94)return;
 memcpy(&delay,c+1096,8);memcpy(&vector,c+1104,8);memcpy(&actor,c+1032,8);memcpy(&helper,c+1112,8);uintptr_t entry_rsp=(uintptr_t)c+1160;HprEntry*e=hpr_find_entry(actor,helper);if(e&&!e->consumed)e->ce_seen=1;
 if(actor>UINTPTR_MAX-0x1e0||delay!=actor+0x1e0||vector!=helper-0x40||entry_rsp!=helper-0x98||!hpr_main(helper,actor,&main)){HPR_DIAG_COUNT(invalid);return;}
 if(hpr_depth>=4){HPR_DIAG_COUNT(skipped);return;}HprRecord*r=hpr_pending+hpr_depth++;memset(r,0,sizeof*r);r->qpc_before=hpr_qpc();r->owner=actor;r->helper_frame=helper;r->main_frame=main;r->entry_rsp=entry_rsp;r->delay_pointer=delay;r->vector_pointer=vector;r->caller=caller;r->thread=GetCurrentThreadId();r->flags=HPR_SCOPE|HPR_CE_PRESENT;memcpy(&r->mxcsr_before,c+24,4);memcpy(&r->gate,c+1040,1);memcpy(&r->input_rax,c+1144,8);
 if(hpr_clock(&r->clock,&r->logical_T,&r->logical_dt,&r->frame))r->flags|=HPR_CLOCK;
 for(unsigned i=0;i<8;++i)memcpy(r->xmm[i],c+512+i*32,16);
 if(camera_pose_read(entry_rsp+8,r->stack_arguments,40)&&camera_pose_read(actor,r->head_before,sizeof r->head_before)&&camera_pose_read(vector,r->vector_before,16)&&camera_pose_read(delay,&r->delay_before,4))r->flags|=HPR_BEFORE;
 if((r->flags&HPR_BEFORE)&&!memcmp(r->head_before+0x200,r->vector_before,16))r->flags|=HPR_VECTOR_OLD200;
 if(e&&!e->consumed&&e->main==main&&e->thread==r->thread){r->call_id=e->id;r->source_call_id=e->id;r->source_flags=e->flags;memcpy(r->accum_before,e->accum,8);if(e->flags&HPR_ENTRY)r->flags|=HPR_ENTRY;
  if((e->flags&(HPR_CLOCK|HPR_RAW_AXES))==(HPR_CLOCK|HPR_RAW_AXES)&&e->clock==r->clock&&e->T==r->logical_T&&e->frame==r->frame){memcpy(r->raw_axes,e->axes,8);r->flags|=HPR_RAW_AXES;}
 }else HPR_DIAG_COUNT(unpaired);
 hpr_attach_look(r,(r->flags&HPR_BEFORE)?r->head_before[0x120]:0);uintptr_t post=(uintptr_t)hpr_post_bridge;memcpy(c+1160,&post,8);
}
static void hpr_common(unsigned char*c){uintptr_t actor=0,helper=0,main=0;memcpy(&actor,c+1040,8);memcpy(&helper,c+1120,8);HprEntry*e=hpr_find_entry(actor,helper);
 if(!e||e->consumed||!hpr_main(helper,actor,&main)||e->main!=main||e->thread!=GetCurrentThreadId()){HPR_DIAG_COUNT(unpaired);return;}
 if(e->ce_seen&&!e->ce_ready){HPR_DIAG_COUNT(unpaired);e->consumed=1;return;}
 HprRecord*r=&e->completed;if(!e->ce_seen){memset(r,0,sizeof*r);r->owner=actor;r->helper_frame=helper;r->main_frame=main;r->thread=e->thread;r->call_id=e->id;r->source_call_id=e->id;r->clock=e->clock;r->logical_T=e->T;r->logical_dt=e->dt;r->frame=e->frame;r->flags=HPR_SCOPE|HPR_DIRECT_SOURCE;
  if(e->flags&HPR_CLOCK)r->flags|=HPR_CLOCK;hpr_attach_look(r,e->head_entry[0x120]);}
 r->source_flags=e->flags;r->parent_mode_bl=e->parent_mode_bl;r->flags|=HPR_HELPER_EXIT|HPR_PARENT_MODE;r->helper_qpc_before=e->qpc;r->helper_qpc_after=hpr_qpc();r->helper_mxcsr_before=e->mxcsr;memcpy(&r->helper_mxcsr_after,c+24,4);memcpy(r->helper_xmm,e->args,sizeof e->args);memcpy(r->accum_before,e->accum,8);r->helper_clock=e->clock;r->helper_logical_T=e->T;r->helper_logical_dt=e->dt;r->helper_frame_number=e->frame;r->helper_thread=e->thread;
 if(e->flags&HPR_KINEMATIC_ANGLE){r->flags|=HPR_KINEMATIC_ANGLE;r->kinematic_angle=e->kinematic_angle;r->kinematic_valid=1;}
 if(e->flags&HPR_ENTRY)r->flags|=HPR_ENTRY;if(e->flags&HPR_HEAD_ENTRY){memcpy(r->head_entry,e->head_entry,sizeof r->head_entry);r->flags|=HPR_HEAD_ENTRY;}
 if(camera_pose_read(actor,r->head_exit,sizeof r->head_exit))r->flags|=HPR_HEAD_EXIT;else HPR_DIAG_COUNT(invalid);e->consumed=1;
 hpr_group_record_complete(e,r);
 if(hpr_header){ AcquireSRWLockExclusive(&hpr_tape_lock);unsigned i=hpr_header->count;if(i<hpr_header->capacity){r->sequence=i;hpr_records[i]=*r;MemoryBarrier();hpr_header->count=i+1;}else ++hpr_header->dropped;ReleaseSRWLockExclusive(&hpr_tape_lock);}
}
static __attribute__((used,noinline))ABI void hpr_before(unsigned stage,unsigned char*c){if(!hpr_header&&!hpr_group_enabled)return;if(stage==0)hpr_helper_entry(c);else if(stage==1)hpr_raw(c);else if(stage==2)hpr_look(c);else if(stage==3)hpr_ce(c);else if(stage==4)hpr_common(c);hpr_test_clobber();}
static __attribute__((used,noinline))ABI void hpr_after(unsigned char*c){uintptr_t native=(uintptr_t)image+0xb92f94;memcpy(c+1160,&native,8);if(!hpr_header&&!hpr_group_enabled)return;
 if(!hpr_depth){HPR_DIAG_COUNT(unpaired);return;}HprRecord*r=hpr_pending+hpr_depth-1;uintptr_t helper=0,actor=0;memcpy(&helper,c+1112,8);memcpy(&actor,c+1032,8);
 if(helper!=r->helper_frame||actor!=r->owner){HPR_DIAG_COUNT(unpaired);return;}--hpr_depth;r->qpc_after=hpr_qpc();memcpy(&r->mxcsr_after,c+24,4);memcpy(&r->result_rax,c+1144,8);
 if(camera_pose_read(r->owner,r->head_after,sizeof r->head_after)&&camera_pose_read(r->delay_pointer,&r->delay_after,4)&&camera_pose_read(r->vector_pointer,r->vector_after,16))r->flags|=HPR_AFTER;else HPR_DIAG_COUNT(invalid);
 HprEntry*e=hpr_find_entry(actor,helper);if(!e||e->consumed||!e->ce_seen||e->id!=r->call_id){HPR_DIAG_COUNT(unpaired);return;}e->completed=*r;e->ce_ready=1;hpr_test_clobber();
}
#include "head-response-primitives-bridges.inc"
#include "head-response-primitives-install.inc"
