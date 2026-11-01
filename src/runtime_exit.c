/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_exit.h"
#include "guest_cpu.h"
#include "exit_dialog.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* DRIVECLUB 1.28: Selector's foreground/overlay/sit-on-top stacks. These are
 * game UI stacks, unrelated to the disabled host settings menu. */
typedef struct { uintptr_t page; unsigned count; int transition; char name[96]; } MenuStack;
typedef struct { uintptr_t selector; bool active; MenuStack stack[3]; char state[160]; } MenuSnapshot;

static bool copy_guest(uintptr_t p, void *out, size_t n) {
    if (p < 0x10000 || n > 4096 || p + n < p) return false;
#ifdef _WIN32
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)p, out, n, &copied) && copied == n;
#else
    sigjmp_buf recovery;
    sigjmp_buf *previous = runtime_fault_recover;
    if (sigsetjmp(recovery, 1)) { runtime_fault_recover = previous; return false; }
    runtime_fault_recover = &recovery;
    memcpy(out, (const void *)p, n);
    runtime_fault_recover = previous;
    return true;
#endif
}
static uintptr_t ptr(uintptr_t p) { uintptr_t v=0; copy_guest(p,&v,sizeof(v)); return v; }
/* The game's functions are guest code: called through the guest CPU (directly on x86-64, FEXCore on ARM64). */
static uint64_t game_call(uintptr_t offset, uint64_t argument) {
    return guest_cpu_call(runtime_image_base+offset,1,&argument);
}
static bool text(uintptr_t p, char *out, size_t n) {
    for (size_t i=0;i<n;++i) {
        if (!copy_guest(p+i,out+i,1)) { out[0]=0; return false; }
        if (!out[i]) return true;
    }
    out[0]=0; return false;
}
static void page_name(uintptr_t selector, uintptr_t page, char *out, size_t n) {
    uintptr_t head=ptr(selector+0x260), node=ptr(head+8), parents[32]; unsigned depth=0, visited=0;
    /* Registry is an MSVC-style red/black tree. Name is a C string at node+0x38,
     * as used by the game's Selector.PushPage Lua binding (+0x2822ad). */
    while (node && node!=head && visited++<2048) {
        unsigned char nil=1;
        if (!copy_guest(node+0x19,&nil,1) || nil) break;
        if (ptr(node+0x28)==page) { text(node+0x38,out,n); return; }
        uintptr_t left=ptr(node), right=ptr(node+0x10);
        unsigned char ln=1,rn=1;
        copy_guest(left+0x19,&ln,1); copy_guest(right+0x19,&rn,1);
        if (!ln) {
            if (!rn) { if(depth==32)return; parents[depth++]=right; }
            node=left;
        } else if (!rn) node=right;
        else if (depth) node=parents[--depth];
        else break;
    }
}
static bool snapshot(MenuSnapshot *m) {
    memset(m,0,sizeof(*m));
    if (!runtime_image_base || runtime_image_size<0x189f8d0) return false;
    unsigned char sig[8];
    static const unsigned char expected[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56};
    if (!copy_guest(runtime_image_base+0x2821e0,sig,sizeof(sig)) || memcmp(sig,expected,sizeof(sig))) return false;
    m->selector=ptr(runtime_image_base+0x189f8c8);
    unsigned char active=0;
    if (!copy_guest(m->selector+0x78,&active,1) || active!=1) return false;
    m->active=true;
    for (unsigned i=0;i<3;++i) {
        uintptr_t stack=ptr(m->selector+0x240+i*8);
        if (!stack) return false;
        uintptr_t begin=ptr(stack+0x18), end=ptr(stack+0x20);
        if (end<begin || (end-begin)%0xb0 || end-begin>0xb0*64) return false;
        m->stack[i].count=(unsigned)((end-begin)/0xb0);
        copy_guest(stack+8,&m->stack[i].transition,4);
        if ((i==0 && m->stack[i].count!=1) || (i!=0 && m->stack[i].count)) return false;
        if (begin!=end) {
            m->stack[i].page=ptr(end-0xb0+8);
            /* Registry/page objects persist; avoid traversing hundreds of UI
             * nodes on every pad poll while the same root is displayed. */
            static uintptr_t cached_page,cached_registry;
            static char cached_name[96];
            uintptr_t registry=ptr(m->selector+0x260);
            if (cached_page!=m->stack[i].page || cached_registry!=registry) {
                cached_page=m->stack[i].page; cached_registry=registry; cached_name[0]=0;
                page_name(m->selector,cached_page,cached_name,sizeof(cached_name));
            }
            snprintf(m->stack[i].name,sizeof(m->stack[i].name),"%s",cached_name);
            if (strcmp(m->stack[i].name,"x_main.frontend")) return false;
            if (ptr(stack+0x18)!=begin || ptr(stack+0x20)!=end || ptr(end-0xb0+8)!=m->stack[i].page) return false;
        }
    }
    uintptr_t gs=ptr(runtime_image_base+0x17788c0);
    if (gs) {
        uintptr_t state=(uintptr_t)game_call(0x4f24a0,gs+8);
        text(ptr(state+8),m->state,sizeof(m->state));
    }
    return ptr(runtime_image_base+0x189f8c8)==m->selector;
}
static pthread_mutex_t exit_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tick_lock=PTHREAD_MUTEX_INITIALIZER;
static ExitDialog dialog;
static bool root;
static atomic_bool quit_dispatched;
static unsigned key_held,key_pressed;
static int active_device;
static char confirm_label[48]="A",back_label[48]="B";
static bool shutdown_retry_ready, regional_input_ready;
static _Atomic uint32_t confirm_button=0x4000;
uint32_t runtime_exit_confirm_button(void) { return atomic_load(&confirm_button); }

/* The same native regional predicate selects Cross/Circle in the game's pad
 * processing (+0xd94fd5, +0xd9509d) and button help (+0x277700). It reads the
 * game's title-region state, not the host language or controller model. */
static void update_button_layout(void) {
    if (!regional_input_ready) return;
    typedef bool (ABI *CircleConfirm)(void);
    uint32_t button=((CircleConfirm)(runtime_image_base+0x5008f0))() ? 0x2000 : 0x4000;
    if (atomic_exchange(&confirm_button,button)!=button)
        printf("Exit dialog: regional Confirm=%s, Back=%s\n",
               button==0x2000 ? "Circle" : "Cross", button==0x2000 ? "Cross" : "Circle");
}

/* WebAPI_0 retries failed offline initialization inside a loop which never
 * checks the manager's stop flag (+0x21). During application teardown this
 * leaves the main thread waiting forever in its join (+0x7d6d78).
 * Preserve cleanup/initialization verbatim during play. When its owner asks
 * it to stop, leave the retry loop so its original outer loop can exit. */
static ABI bool webapi_retry(unsigned char *manager) {
    game_call(0x7d7420,(uintptr_t)manager);
    if (*(volatile unsigned char *)(manager+0x21)) return true;
    return (game_call(0x7d6810,(uintptr_t)manager)&0xff)!=0;   /* a bool: AL */
}
/* Cached Lua file wrappers may outlive their source signals after a race.
 * Disconnecting from an already destroyed subscription table enters the native
 * lazy-bucket initializer recursively with index zero. During confirmed shutdown,
 * expire only those dead connections before running the original destructor;
 * live connections, owned buffers and every other cleanup remain native. */
static void (ABI *stream_destructor)(void *);
static ABI void close_stream(void *object) {
    if (atomic_load(&quit_dispatched)) {
        unsigned expired=0;
        for (unsigned offset=0x20;offset<=0x30;offset+=0x10) {
            uintptr_t connection=(uintptr_t)object+offset;
            uintptr_t owner=ptr(connection), key=ptr(connection+8);
            if (!owner || key==UINTPTR_MAX) continue;
            uintptr_t table=ptr(owner+0x18);
            if (!table || !ptr(table+0x30)) {
                *(uintptr_t *)connection=0;
                *(uintptr_t *)(connection+8)=UINTPTR_MAX;
                ++expired;
            }
        }
        static atomic_bool reported;
        if (expired && !atomic_exchange(&reported,true))
            puts("Exit dialog: expired file-stream subscriptions skipped during shutdown");
    }
    stream_destructor(object);
}
static bool install_stream_shutdown_guard(void) {
    unsigned char *site=(unsigned char *)(runtime_image_base+0xea24d0);
    /* Complete, RIP-independent prologue. Keep it in the trampoline verbatim. */
    static const unsigned char prologue[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x68};
    static const unsigned char first_connection[]={0x48,0x8b,0x43,0x28,0x48,0x83,0xf8,0xff,0x74,0x3a};
    static const unsigned char second_connection[]={0x48,0x8b,0x43,0x38,0x48,0x83,0xf8,0xff,0x74,0x3a};
    if (memcmp(site,prologue,sizeof(prologue)) ||
        memcmp((const void *)(runtime_image_base+0xea24f3),first_connection,sizeof(first_connection)) ||
        memcmp((const void *)(runtime_image_base+0xea2537),second_connection,sizeof(second_connection))) return false;
#ifdef _WIN32
    unsigned char *trampoline=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    unsigned char *trampoline=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (trampoline==MAP_FAILED) trampoline=NULL;
#endif
    if (!trampoline) return false;
    memcpy(trampoline,prologue,sizeof(prologue));
    unsigned char jump[12]={0x48,0xb8};
    uintptr_t resume=(uintptr_t)site+sizeof(prologue);
    memcpy(jump+2,&resume,8);jump[10]=0xff;jump[11]=0xe0;
    memcpy(trampoline+sizeof(prologue),jump,sizeof(jump));
    unsigned char patch[sizeof(prologue)];memset(patch,0x90,sizeof(patch));
    uintptr_t target=(uintptr_t)close_stream;
    memcpy(jump+2,&target,8);memcpy(patch,jump,sizeof(jump));
#ifdef _WIN32
    DWORD old,ignored;
    if (!VirtualProtect(trampoline,4096,PAGE_EXECUTE_READ,&old) ||
        !VirtualProtect(site,sizeof(patch),PAGE_EXECUTE_READWRITE,&old)) {
        VirtualFree(trampoline,0,MEM_RELEASE);return false;
    }
    stream_destructor=(void (ABI *)(void *))trampoline;
    memcpy(site,patch,sizeof(patch));
    VirtualProtect(site,sizeof(patch),old,&ignored);
    FlushInstructionCache(GetCurrentProcess(),trampoline,sizeof(prologue)+sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(),site,sizeof(patch));
#else
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    void *start=(void *)((uintptr_t)site&~(uintptr_t)(page-1));
    if (mprotect(trampoline,4096,PROT_READ|PROT_EXEC) ||
        mprotect(start,page,PROT_READ|PROT_WRITE|PROT_EXEC)) {
        munmap(trampoline,4096);return false;
    }
    stream_destructor=(void (ABI *)(void *))trampoline;
    memcpy(site,patch,sizeof(patch));mprotect(start,page,PROT_READ|PROT_EXEC);
    __builtin___clear_cache((char *)trampoline,(char *)trampoline+sizeof(prologue)+sizeof(jump));
    __builtin___clear_cache((char *)site,(char *)site+sizeof(patch));
#endif
    return true;
}
void runtime_exit_initialize(void) {
    if (!runtime_image_base || runtime_image_size<0x189f8d0) return;
    static const unsigned char regional_predicate[]={
        0x48,0x8d,0x05,0xcd,0x7c,0x05,0x01,0x8b,0x00,0x83,0xf8,0x02,0x74,0x05,
        0x83,0xf8,0x04,0x75,0x03,0xb0,0x01,0xc3,0x83,0xf8,0x05,0x0f,0x94,0xc0,0xc3};
    if (memcmp((const void *)(runtime_image_base+0x5008f0),regional_predicate,sizeof(regional_predicate))) {
        puts("Exit dialog: unsupported regional input code; dialog disabled"); return;
    }
    regional_input_ready=true;
    unsigned char *site=(unsigned char *)(runtime_image_base+0x7d6e70);
    static const unsigned char expected[]={0x4c,0x89,0xff,0xe8,0xa8,0x05,0,0,
        0x4c,0x89,0xff,0xe8,0x90,0xf9,0xff,0xff};
    if (memcmp(site,expected,sizeof(expected))) {
        puts("Exit dialog: unsupported WebAPI shutdown code; dialog disabled"); return;
    }
    if (!install_stream_shutdown_guard()) {
        puts("Exit dialog: unsupported file-stream destructor; dialog disabled");return;
    }
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect(site,sizeof(expected),PAGE_EXECUTE_READWRITE,&old)) return;
#else
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    void *start=(void *)((uintptr_t)site&~(uintptr_t)(page-1));
    if (mprotect(start,page,PROT_READ|PROT_WRITE|PROT_EXEC)) return;
#endif
    /* mov rdi,r15; movabs rax,webapi_retry; call rax; nop. The guest-callable address of the host function: on
     * ARM64 the guest cannot run host code, FEXCore enters it through a thunk. */
    unsigned char patch[16]={0x4c,0x89,0xff,0x48,0xb8};
    uintptr_t target=guest_cpu_host_function((void *)webapi_retry);
    memcpy(patch+5,&target,8); patch[13]=0xff; patch[14]=0xd0; patch[15]=0x90;
    memcpy(site,patch,sizeof(patch));
#ifdef _WIN32
    DWORD ignored; VirtualProtect(site,sizeof(patch),old,&ignored);
    FlushInstructionCache(GetCurrentProcess(),site,sizeof(patch));
#else
    mprotect(start,page,PROT_READ|PROT_EXEC);
    __builtin___clear_cache((char *)site,(char *)site+sizeof(patch));
#endif
    guest_cpu_code_changed((uintptr_t)site,sizeof(patch));
    shutdown_retry_ready=true;
    puts("Exit dialog: WebAPI offline retry now observes the game's stop request");
}

static bool supported_layout(void) {
    /* Check both the UI binding and quit binding, not just a common prologue.
     * Fail closed on another game revision; never try guessed offsets. */
    return shutdown_retry_ready && regional_input_ready && runtime_image_base && runtime_image_size>0x189f8d0 &&
        ptr(runtime_image_base+0x15b4878)==runtime_image_base+0x1404d0 &&
        ptr(runtime_image_base+0x15c1978)==runtime_image_base+0x2821e0 &&
        !memcmp((const void *)(runtime_image_base+0x12ad1b7),"x_main.frontend",15);
}
void runtime_exit_tick(void) {
    pthread_mutex_lock(&tick_lock);
    pthread_mutex_lock(&exit_lock);
    const bool dispatched=quit_dispatched;
    pthread_mutex_unlock(&exit_lock);
    if (dispatched) { pthread_mutex_unlock(&tick_lock); return; }
    update_button_layout();
    MenuSnapshot m;
    bool at_root=supported_layout() && snapshot(&m) && m.stack[0].count==1 &&
        !strcmp(m.stack[0].name,"x_main.frontend") && !m.stack[1].count && !m.stack[2].count &&
        (!strcmp(m.state,"/game/Game/inner/Lite/frontendflow/Front") ||
         !strcmp(m.state,"/game/Game/inner/Lite/frontendflow/NoDiorama"));
    pthread_mutex_lock(&exit_lock);
    if (root!=at_root) {
        printf("Exit dialog: root menu %s\n",at_root ? "ready" : "inactive");
        root=at_root;
    }
    bool quit=dialog.exiting && !quit_dispatched && at_root && !runtime_savedata_busy();
    if (quit) quit_dispatched=true;
    pthread_mutex_unlock(&exit_lock);
    /* Same game input thread used by game_options_apply(), outside the pad lock.
     * This is the real GameState.Quit Lua implementation (L is unused): it sets
     * the game manager's shutdown request, consumed by the application loop.
     * EndGame is a different gameplay event and does not close the application.
     * Never use SDL_QUIT/_Exit here. */
    if (quit) {
        puts("Exit dialog: requesting GameState.Quit (save transactions complete)");
        game_call(0x1404d0,0);
    }
    pthread_mutex_unlock(&tick_lock);
}
int runtime_exit_captures(void) {
    pthread_mutex_lock(&exit_lock);
    int result=exit_dialog_captures(&dialog);
    pthread_mutex_unlock(&exit_lock); return result;
}
void runtime_exit_key(unsigned action, int down, int repeat) {
    if (down && repeat) return;
    pthread_mutex_lock(&exit_lock);
    if (down && !repeat && !(key_held & action)) key_pressed|=action;
    if (down) key_held|=action; else key_held&=~action;
    if (down && !repeat) active_device=0;
    pthread_mutex_unlock(&exit_lock);
}
void runtime_exit_mouse_activity(void) {
    pthread_mutex_lock(&exit_lock); active_device=0; pthread_mutex_unlock(&exit_lock);
}
void runtime_exit_release_keys(void) {
    pthread_mutex_lock(&exit_lock); key_held=key_pressed=0; pthread_mutex_unlock(&exit_lock);
}
int runtime_exit_pad(uint32_t buttons, int lx, int ly, int neutral, int available,
                     int pad_activity, int device, const char *confirm, const char *back) {
    pthread_mutex_lock(&exit_lock);
    unsigned held=key_held | exit_dialog_actions(buttons,runtime_exit_confirm_button());
    if (pad_activity) {
        active_device=device;
        snprintf(confirm_label,sizeof(confirm_label),"%s",confirm);
        snprintf(back_label,sizeof(back_label),"%s",back);
    }
    bool was_open=dialog.open, was_capture=exit_dialog_captures(&dialog);
    exit_dialog_step(&dialog,(ExitDialogInput){.root=root,.available=available!=0,
        .neutral=neutral && !key_held,.held=held,.pressed=key_pressed,
        .stick_x=(lx-128)/127.0f,.stick_y=(ly-128)/127.0f});
    key_pressed=0;
    if (was_open!=dialog.open) printf("Exit dialog: %s\n",dialog.open ? "opened (Cancel selected)" : "cancelled");
    int capture=was_capture || exit_dialog_captures(&dialog);
    pthread_mutex_unlock(&exit_lock); return capture;
}
void runtime_exit_click(int exit_button) {
    pthread_mutex_lock(&exit_lock);
    exit_dialog_click(&dialog,exit_button!=0);
    pthread_mutex_unlock(&exit_lock);
}
void runtime_exit_hover(int exit_button) {
    pthread_mutex_lock(&exit_lock);
    if (dialog.open && !dialog.exiting) dialog.selected_exit=exit_button!=0;
    pthread_mutex_unlock(&exit_lock);
}
RuntimeExitView runtime_exit_view(void) {
    pthread_mutex_lock(&exit_lock);
    RuntimeExitView view={.open=dialog.open,.selected_exit=dialog.selected_exit,
        .exiting=dialog.exiting,.device=active_device};
    snprintf(view.confirm,sizeof(view.confirm),"%s",active_device ? confirm_label : "Enter");
    snprintf(view.back,sizeof(view.back),"%s",active_device ? back_label : "Esc");
    pthread_mutex_unlock(&exit_lock); return view;
}
