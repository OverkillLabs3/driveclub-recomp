/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/runtime_exit.c"
#include <assert.h>
uintptr_t runtime_image_base; uint64_t runtime_image_size;
int runtime_savedata_busy(void) { assert(!"unexpected save query"); return 0; }
static unsigned called;static unsigned char object[0x80],owners[2][0x30],tables[2][0x80];
static void ABI original(void *p) { assert(p==object);++called; }
static void reset(void) {
    memset(object,0x5a,sizeof(object)); memset(owners,0,sizeof(owners)); memset(tables,0,sizeof(tables));
    for(unsigned i=0;i<2;++i) {
        *(uintptr_t *)(owners[i]+0x18)=(uintptr_t)tables[i];
        *(uintptr_t *)(tables[i]+0x30)=0x1234;
        *(uintptr_t *)(object+0x20+i*0x10)=(uintptr_t)owners[i];
        *(uintptr_t *)(object+0x28+i*0x10)=i+1;
    }
    called=0;stream_destructor=original;
}
int main(void) {
    reset();quit_dispatched=false;*(uintptr_t *)(tables[0]+0x30)=0;
    close_stream(object);assert(called==1 && *(uintptr_t *)(object+0x20)==(uintptr_t)owners[0]);
    reset();quit_dispatched=true;close_stream(object);
    assert(called==1 && *(uintptr_t *)(object+0x28)==1 && *(uintptr_t *)(object+0x38)==2);
    reset();*(uintptr_t *)(tables[0]+0x30)=0;close_stream(object);
    assert(called==1 && !*(uintptr_t *)(object+0x20) && *(uintptr_t *)(object+0x28)==UINTPTR_MAX);
    assert(*(uintptr_t *)(object+0x30)==(uintptr_t)owners[1] && *(uintptr_t *)(object+0x38)==2);
    assert(*(uintptr_t *)(object+0x40)==UINT64_C(0x5a5a5a5a5a5a5a5a));
    reset();*(uintptr_t *)(owners[1]+0x18)=0;close_stream(object);
    assert(called==1 && *(uintptr_t *)(object+0x28)==1 && *(uintptr_t *)(object+0x38)==UINTPTR_MAX);
    reset();*(uintptr_t *)(object+0x20)=0x123;*(uintptr_t *)(object+0x38)=UINTPTR_MAX;
    close_stream(object);assert(called==1 && !*(uintptr_t *)(object+0x20));
    puts("exit_shutdown_test: PASS (ordinary gameplay unchanged; live/expired/missing subscriptions; native cleanup retained)");
}