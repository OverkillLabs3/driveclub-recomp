/* Event45 is the EXACT original parent B959BD logicalTC+20/master68 fetch.
 *43 remains the separate child fallback. Snapshot is original native data. */
#include "head-pause-clock-core.inc"
static __attribute__((used))uintptr_t hps_entry_gateway,hps_scale,hps_selected_resume;
static int hps_enabled;static volatile LONG64 hps_epoch=1;
typedef struct{uintptr_t manager,parent,tc;uint64_t entry,epoch;unsigned valid;uint32_t frame,thread;int64_t selected;uint32_t dtbits;unsigned char tuple[64];} HpsNative;
static _Thread_local HpsNative hps_native;
static _Thread_local uint64_t hps_entries;
static SRWLOCK hps_lock=SRWLOCK_INIT;static HpsAnchor hps_anchors[64];
static uint64_t hps_u64(const unsigned char*p){uint64_t x;memcpy(&x,p,8);return x;}
static int64_t hps_i64(const unsigned char*p){int64_t x;memcpy(&x,p,8);return x;}
static uint32_t hps_u32(const unsigned char*p){uint32_t x;memcpy(&x,p,4);return x;}
static void hps_jump(unsigned char*p,uintptr_t t){memcpy(p,"\xff\x25\0\0\0\0",6);memcpy(p+6,&t,8);}
static __attribute__((used,noinline))ABI void hps_begin(const unsigned char*c){
 memset(&hps_native,0,sizeof hps_native);hps_native.manager=hps_u64(c+1032+64);hps_native.parent=(uintptr_t)c+1032+120;hps_native.entry=++hps_entries;hps_native.epoch=(uint64_t)InterlockedCompareExchange64(&hps_epoch,0,0);hps_native.thread=GetCurrentThreadId();
 #ifdef HPS_TEST_CLOBBER
 HPS_TEST_CLOBBER();
 #endif
}
static __attribute__((used,noinline))ABI void hps_capture(const unsigned char*c){
 uintptr_t manager=hps_u64(c+1280),parent=hps_u64(c+1360),tc=hps_u64(c+1392);int64_t selected=hps_i64(c+1152);uint32_t flags=0;
 if(hps_native.manager==manager&&hps_native.parent==parent&&hps_native.epoch==(uint64_t)InterlockedCompareExchange64(&hps_epoch,0,0))flags|=1;
 if(!memcmp(c+1024,c+1088,64)&&hps_u32(c+1216)==hps_u32(c+1220)&&selected==hps_i64(c+1088+32))flags|=2;
 hps_native.valid=(flags&3)==3;hps_native.tc=tc;hps_native.frame=hps_u32(c+1216);hps_native.selected=selected;hps_native.dtbits=hps_u32(c+512);memcpy(hps_native.tuple,c+1024,64);

}
static void hps_forget(uintptr_t owner){
 if(!hps_enabled)return;AcquireSRWLockExclusive(&hps_lock);for(unsigned i=0;i<64;++i)if(!owner||hps_anchors[i].owner==owner)memset(hps_anchors+i,0,sizeof hps_anchors[i]);InterlockedIncrement64(&hps_epoch);ReleaseSRWLockExclusive(&hps_lock);
}
static void hps_discard_anchor(uintptr_t owner){AcquireSRWLockExclusive(&hps_lock);for(unsigned i=0;i<64;++i)if(hps_anchors[i].owner==owner)memset(hps_anchors+i,0,sizeof hps_anchors[i]);ReleaseSRWLockExclusive(&hps_lock);}
#include "head-pause-producer.inc"
static int hps_tuple_units(const unsigned char*t){return hps_effective_producer(t)!=0;}
static int hps_selected_for_head(const HKTRecord*r,HpsSample*out){
 HpsNative*s=&hps_native;uintptr_t parent=0,ret=0;uint32_t bits=0;memcpy(&bits,r->input.dt,4);
 if(!s->valid||s->epoch!=(uint64_t)InterlockedCompareExchange64(&hps_epoch,0,0)||s->thread!=GetCurrentThreadId()||r->thread!=s->thread||r->frame!=s->frame||(uintptr_t)r->frame_address+0x30!=s->parent||bits!=s->dtbits||!hps_tuple_units(s->tuple)||!s->tuple[0x38]||hps_i64(s->tuple+0x18)!=0||s->selected!=hps_i64(s->tuple+0x20)||!camera_pose_read((uintptr_t)r->frame_address,&parent,8)||parent!=s->parent||!camera_pose_read((uintptr_t)r->frame_address+8,&ret,8)||ret!=(uintptr_t)image+0xb959f9)return 0;
 uint32_t stored=0;if(!camera_pose_read(parent-0x1c,&stored,4)||stored!=bits)return 0;
 *out=(HpsSample){(uintptr_t)r->actor,(uintptr_t)r->vehicle,s->tc-0x48,hps_i64(s->tuple+0x10),hps_i64(s->tuple+8),s->selected,s->frame,1,1};return 1;
}
static HCSClock hps_head_clock(const HKTRecord*r){
 if(!hps_enabled)return hllq_enabled?hcs_read_clock(r->frame,r->input.dt[0]):hcs_read_head_clock(r->frame,r->input.dt[0]);uint64_t epoch=(uint64_t)InterlockedCompareExchange64(&hps_epoch,0,0);HCSClock clock=hcs_read_clock(r->frame,r->input.dt[0]);HpsSample x={0};
 if(clock.valid){unsigned char before[64],after[64];uintptr_t tc=clock.clock+0x48;if(camera_pose_read(tc,before,64)&&camera_pose_read(tc,after,64)&&!memcmp(before,after,64)&&!before[0x38]&&hps_tuple_units(before)&&hps_i64(before+0x10)==clock.time_us&&hps_i64(before+0x18)==clock.dt_us)x=(HpsSample){(uintptr_t)r->actor,(uintptr_t)r->vehicle,clock.clock,clock.time_us,hps_i64(before+8),clock.dt_us,r->frame,1,0};}
 else if(!hps_selected_for_head(r,&x)){hps_discard_anchor((uintptr_t)r->actor);return clock;}
 AcquireSRWLockExclusive(&hps_lock);HpsAnchor*slot=NULL,*empty=NULL;for(unsigned i=0;i<64;++i){if(hps_anchors[i].owner==(uintptr_t)r->actor&&r->actor){slot=hps_anchors+i;break;}if(!hps_anchors[i].owner&&!empty)empty=hps_anchors+i;}if(!slot)slot=empty;int64_t t=0;int mode=slot&&epoch==(uint64_t)InterlockedCompareExchange64(&hps_epoch,0,0)?hps_core(slot,&x,&t):0;ReleaseSRWLockExclusive(&hps_lock);
 if(clock.valid)return clock;if(mode==2)return(HCSClock){x.clock,t,x.elapsed,r->frame,1,1};return(HCSClock){0};
}
#include "head-pause-bridges.inc"
static int hps_install(void){const char*e=camera_fix_getenv("BB_HEAD_PAUSED_CLOCK");if(!e||strcmp(e,"1"))return 0;
 static const unsigned char entry[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0xc5,0xfa,0x11,0x45,0xe4};
 static const unsigned char selected[]={0xc4,0xe1,0xfa,0x2a,0x40,0x20,0xc5,0xfa,0x59,0x05,0x49,0xbd,0x7b,0x00};
 if(runtime_image_size<0xb959bd+14||memcmp(image+0xb958d0,entry,15)||memcmp(image+0xb959bd,selected,14))return 0;unsigned char*g=VirtualAlloc(NULL,29,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);if(!g)return 0;memcpy(g,entry,15);hps_jump(g+15,(uintptr_t)image+0xb958df);hps_entry_gateway=(uintptr_t)g;hps_scale=(uintptr_t)image+0x1351714;hps_selected_resume=(uintptr_t)image+0xb959cb;
 DWORD old;if(!VirtualProtect(image+0xb958d0,0xfb,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(g,0,MEM_RELEASE);return 0;}hps_jump(image+0xb958d0,(uintptr_t)hps_entry_bridge);image[0xb958d0+14]=0x90;hps_jump(image+0xb959bd,(uintptr_t)hps_selected_bridge);DWORD ignored;VirtualProtect(image+0xb958d0,0xfb,old,&ignored);FlushInstructionCache(GetCurrentProcess(),image+0xb958d0,0xfb);hps_enabled=1;puts("HeadPausedClock: exact parent selected master68/event45 + original-chain TLS; unit/no-fixed anchored timer50 continuation only, unknown pause stays native");return 1;
}
