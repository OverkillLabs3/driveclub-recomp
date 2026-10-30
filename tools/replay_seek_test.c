/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Windows regression test: clang -O2 tools/replay_seek_test.c -o replay_seek_test.exe */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#define ABI __attribute__((sysv_abi))
#define BB_DIAGNOSTIC_ENV(name) ((char *)0)
static unsigned char *image;
static uintptr_t runtime_image_base;
static uint64_t runtime_image_size;
static void *camera_rate_hook(size_t at,const unsigned char *p,size_t n,uintptr_t target) {
    (void)at;(void)p;(void)n;(void)target;assert(!"unexpected diagnostic hook");return NULL;
}
#include "../src/runtime_replay_seek.inc"
static float test_dt=1.0f/30;
static volatile __attribute__((used)) uintptr_t counter_entry;
static __attribute__((used)) uintptr_t counter_return;
static __attribute__((naked)) ABI uint32_t call_counter(void *self,unsigned forward,unsigned back) {
    __asm__ volatile(
        "pushq %rbp\n movq %rsp,%rbp\n pushq %r12\n pushq %r13\n subq $48,%rsp\n"
        "movq %rdi,%r12\n movl %esi,%r13d\n xorps %xmm1,%xmm1\n cvtsi2ssl %esi,%xmm1\n"
        "movss %xmm1,-44(%rbp)\n xorps %xmm0,%xmm0\n cvtsi2ssl %edx,%xmm0\n"
        "xorps %xmm2,%xmm2\n movl %edx,%eax\n leaq 1f(%rip),%r11\n"
        "movq %r11,counter_return(%rip)\n jmp *counter_entry(%rip)\n"
        "1: addq $48,%rsp\n popq %r13\n popq %r12\n popq %rbp\n retq\n");
}
static void return_stub(unsigned char *p) {
    const unsigned char load[]={0x49,0xbb},jump[]={0x41,0xff,0x23};
    uintptr_t target=(uintptr_t)&counter_return;
    memcpy(p,load,2);memcpy(p+2,&target,8);memcpy(p+10,jump,3);
}
static unsigned events(unsigned char *self,uint32_t acceleration) {
    unsigned interval=acceleration>4 ? 0 : 4-acceleration,n=0;
    int32_t f,b;memcpy(&f,self+0x130,4);memcpy(&b,self+0x134,4);
    if(f<0){memcpy(self+0x130,&interval,4);++n;}
    else if(b<0){memcpy(self+0x134,&interval,4);++n;}
    return n;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    unsigned char *reference=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(reference);
    memcpy(reference,replay_seek_counter_expected,sizeof replay_seek_counter_expected);
    return_stub(reference+sizeof replay_seek_counter_expected);
    DWORD old;assert(VirtualProtect(reference,4096,PAGE_EXECUTE_READ,&old));
    counter_entry=(uintptr_t)reference;
    unsigned char native[0x160],fixed[0x160];
    uint32_t random=12345;
    for(unsigned i=0;i<10000;++i) {
        memset(native,0xa5,sizeof native);
        for(unsigned j=0;j<3;++j){random=random*1664525u+1013904223u;memcpy(native+0x130+4*j,&random,4);}
        memcpy(fixed,native,sizeof fixed);
        unsigned forward=(i>>1)&1,back=i&1;
        uint32_t expected=call_counter(native,forward,back);
        assert(replay_seek_count(fixed,forward,back,1)==expected);
        assert(!memcmp(native,fixed,sizeof native));
    }
    puts("PASS: 10000 native machine-code comparisons, including wraparound and both/released inputs");
    runtime_image_size=0x1911108;
    image=VirtualAlloc(NULL,runtime_image_size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(image);
    runtime_image_base=(uintptr_t)image;
    memcpy(image+0x10bed4,replay_seek_counter_expected,sizeof replay_seek_counter_expected);
    return_stub(image+0x10bf48);
    const unsigned char delta[]={0x48,0x8d,0x05,0x69,0x7a,0x89,0x01,0x48,0x8b,0x00};
    memcpy(image+0x79690,delta,sizeof delta);
    unsigned char *getter=image+0x79690+sizeof delta;
    const unsigned char load[]={0x48,0xb8},tail[]={0xf3,0x0f,0x10,0x00,0xc3};
    uintptr_t dt=(uintptr_t)&test_dt;memcpy(getter,load,2);memcpy(getter+2,&dt,8);memcpy(getter+10,tail,5);
    _putenv("BB_REPLAY_SEEK_FIX=1");replay_seek_install();
    assert(!memcmp(image+0x10bed4,replay_seek_counter_expected,sizeof replay_seek_counter_expected));
    const unsigned char variable_dt[]={0xb8,0x01,0x00,0x00,0x00};memcpy(image+0xd84800,variable_dt,sizeof variable_dt);
    _putenv("BB_REPLAY_SEEK_FIX=0");replay_seek_install();
    assert(!memcmp(image+0x10bed4,replay_seek_counter_expected,sizeof replay_seek_counter_expected));
    _putenv("BB_REPLAY_SEEK_FIX=1");image[0x10bed4+80]^=1;
    unsigned char guarded[sizeof replay_seek_counter_expected+16];
    memcpy(guarded,image+0x10bed4-8,sizeof guarded);replay_seek_install();
    assert(!memcmp(guarded,image+0x10bed4-8,sizeof guarded));image[0x10bed4+80]^=1;
    replay_seek_install();assert(image[0x10bed4]==0xff && image[0x10bed4+1]==0x25);
    assert(VirtualProtect(image,runtime_image_size,PAGE_EXECUTE_READ,&old));
    counter_entry=(uintptr_t)image+0x10bed4;
    memset(native,0,sizeof native);memset(fixed,0,sizeof fixed);
    unsigned ref_events=0,fixed_events=0;
    test_dt=1.0f/30;
    for(unsigned i=0;i<30;++i){counter_entry=(uintptr_t)reference;ref_events+=events(native,call_counter(native,1,0));}
    counter_entry=(uintptr_t)image+0x10bed4;
    for(unsigned i=0;i<30;++i){fixed_events+=events(fixed,call_counter(fixed,1,0));}
    assert(ref_events==fixed_events && !memcmp(native,fixed,sizeof native));
    unsigned char fast[0x160]={0};test_dt=1.0f/240;unsigned fast_events=0;
    for(unsigned i=0;i<240;++i)fast_events+=events(fast,call_counter(fast,1,0));
    assert(fast_events==ref_events && !memcmp(native,fast,sizeof native));
    unsigned char variable[0x160]={0};unsigned variable_events=0;
    for(unsigned i=0;i<200;++i){test_dt=i%2 ? 1.0f/150 : 1.0f/300;variable_events+=events(variable,call_counter(variable,1,0));}
    assert(variable_events==ref_events && !memcmp(native,variable,sizeof native));
    unsigned char mixed[0x160]={0};
    for(unsigned i=0;i<3;++i){test_dt=.02f;call_counter(mixed,1,0);test_dt=.04f;call_counter(mixed,1,0);}
    uint32_t mixed_held;memcpy(&mixed_held,mixed+0x138,4);assert(mixed_held==5);
    test_dt=0;call_counter(mixed,1,0);memcpy(&mixed_held,mixed+0x138,4);assert(mixed_held==5);
    call_counter(fast,0,0);uint32_t held;memcpy(&held,fast+0x138,4);assert(held==0);
    call_counter(fast,1,1);memcpy(&held,fast+0x138,4);assert(held==0);
    test_dt=1.0f/240;call_counter(fast,0,1);int32_t forward,back;
    memcpy(&forward,fast+0x130,4);memcpy(&back,fast+0x134,4);assert(forward==0 && back==-1);
    puts("PASS: installed native bridge, original30 equivalence, fast/uneven updates, release/reverse/both, opt-out and signature guard");
    VirtualFree(image,0,MEM_RELEASE);VirtualFree(reference,0,MEM_RELEASE);
}
