/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_exit.h"
#include "exit_dialog.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
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
        typedef uintptr_t (ABI *Current)(uintptr_t);
        uintptr_t state=((Current)(runtime_image_base+0x4f24a0))(gs+8);
        text(ptr(state+8),m->state,sizeof(m->state));
    }
    return ptr(runtime_image_base+0x189f8c8)==m->selector;
}
static pthread_mutex_t exit_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tick_lock=PTHREAD_MUTEX_INITIALIZER;
static ExitDialog dialog;
static bool root, quit_dispatched;
static unsigned key_held,key_pressed;
static int active_device;
static char confirm_label[48]="A",back_label[48]="B";
static bool shutdown_retry_ready;

/* WebAPI_0 retries failed offline initialization inside a loop which never
 * checks the manager's stop flag (+0x21). During application teardown this
 * leaves the main thread waiting forever in its join (+0x7d6d78).
 * Preserve cleanup/initialization verbatim during play. When its owner asks
 * it to stop, leave the retry loop so its original outer loop can exit. */
static ABI bool webapi_retry(unsigned char *manager) {
    typedef bool (ABI *NetworkOperation)(void *);
    ((NetworkOperation)(runtime_image_base+0x7d7420))(manager);
    if (*(volatile unsigned char *)(manager+0x21)) return true;
    return ((NetworkOperation)(runtime_image_base+0x7d6810))(manager);
}
void runtime_exit_initialize(void) {
    if (!runtime_image_base || runtime_image_size<0x189f8d0) return;
    unsigned char *site=(unsigned char *)(runtime_image_base+0x7d6e70);
    static const unsigned char expected[]={0x4c,0x89,0xff,0xe8,0xa8,0x05,0,0,
        0x4c,0x89,0xff,0xe8,0x90,0xf9,0xff,0xff};
    if (memcmp(site,expected,sizeof(expected))) {
        puts("Exit dialog: unsupported WebAPI shutdown code; dialog disabled"); return;
    }
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect(site,sizeof(expected),PAGE_EXECUTE_READWRITE,&old)) return;
#else
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    void *start=(void *)((uintptr_t)site&~(uintptr_t)(page-1));
    if (mprotect(start,page,PROT_READ|PROT_WRITE|PROT_EXEC)) return;
#endif
    /* mov rdi,r15; movabs rax,webapi_retry; call rax; nop */
    unsigned char patch[16]={0x4c,0x89,0xff,0x48,0xb8};
    uintptr_t target=(uintptr_t)webapi_retry;
    memcpy(patch+5,&target,8); patch[13]=0xff; patch[14]=0xd0; patch[15]=0x90;
    memcpy(site,patch,sizeof(patch));
#ifdef _WIN32
    DWORD ignored; VirtualProtect(site,sizeof(patch),old,&ignored);
    FlushInstructionCache(GetCurrentProcess(),site,sizeof(patch));
#else
    mprotect(start,page,PROT_READ|PROT_EXEC);
    __builtin___clear_cache((char *)site,(char *)site+sizeof(patch));
#endif
    shutdown_retry_ready=true;
    puts("Exit dialog: WebAPI offline retry now observes the game's stop request");
}

static bool supported_layout(void) {
    /* Check both the UI binding and quit binding, not just a common prologue.
     * Fail closed on another game revision; never try guessed offsets. */
    return shutdown_retry_ready && runtime_image_base && runtime_image_size>0x189f8d0 &&
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
        typedef int (ABI *Quit)(void *);
        ((Quit)(runtime_image_base+0x1404d0))(NULL);
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
    unsigned held=key_held | exit_dialog_actions(buttons);
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
