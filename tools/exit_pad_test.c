/* SPDX-License-Identifier: GPL-2.0-or-later */
/* In-process virtual SDL controller, never an OS input device. Unused runtime
 * entry points are discarded by the linker; only the real sampler is tested. */
#include "../src/runtime_pad.c"
#include "../src/exit_dialog.h"
#include <assert.h>

uintptr_t runtime_image_base;
uint64_t runtime_image_size;
const char *runtime_file_user_dir(void) { return "out/exit-pad-test-user"; }
int bbgpu_overlay_captures_input(void) { return 0; }
void bbgpu_overlay_note(const char *s) { (void)s; }
void bbgpu_mouse_stick_capture(int c) { (void)c; }
static uint32_t test_confirm=0x4000;
static char sampled_confirm[48],sampled_back[48];
uint32_t runtime_exit_confirm_button(void) { return test_confirm; }
int runtime_exit_captures(void) { return 0; }
void compat_sleep_ns(uint64_t ns) { (void)ns; assert(!"unexpected FFB worker in sampler test"); }
uintptr_t runtime_lookup(const RuntimeExport *e,size_t n,const char *name) {
    (void)e;(void)n;(void)name;assert(!"unexpected runtime export lookup");return 0;
}
void runtime_exit_tick(void) { assert(!"this test calls the sampler, not the guest read service"); }
int bbgpu_exit_dialog_available(void) { return 1; }
int runtime_exit_pad(uint32_t b,int x,int y,int n,int a,int p,int d,const char *c,const char *r) {
    (void)b;(void)x;(void)y;(void)n;(void)a;(void)p;(void)d;
    snprintf(sampled_confirm,sizeof(sampled_confirm),"%s",c);
    snprintf(sampled_back,sizeof(sampled_back),"%s",r); return 0;
}

static void feed(ExitDialog *d, SDL_Joystick *joystick, int physical, bool down, bool root) {
    if(physical>=0) assert(SDL_SetJoystickVirtualButton(joystick,physical,down));
    SDL_UpdateJoysticks();
    PadData p; sample_buttons(&p); exit_input(&p);
    exit_dialog_step(d,(ExitDialogInput){.root=root,.available=true,.neutral=!p.buttons,
        .held=exit_dialog_actions(p.buttons,test_confirm),.stick_x=(p.left_x-128)/127.0f,.stick_y=(p.left_y-128)/127.0f});
}
int main(void) {
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc={.version=sizeof(desc),.type=SDL_JOYSTICK_TYPE_GAMEPAD,
        .naxes=SDL_GAMEPAD_AXIS_COUNT,.nbuttons=SDL_GAMEPAD_BUTTON_COUNT,
        .button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1,.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1,
        .name="Exit dialog test controller"};
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc); assert(id);
    SDL_Joystick *j=SDL_OpenJoystick(id); assert(j);
    assert(SDL_IsGamepad(id)); gamepad=SDL_OpenGamepad(id); assert(gamepad);
    input_loaded=1; /* Avoid reading/writing the user's configuration. */
    ExitDialog d={0}; feed(&d,j,-1,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,true,true); assert(d.open && !d.selected_exit);
    for(int i=0;i<30;++i) feed(&d,j,-1,false,true);
    assert(d.open);
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,false,true);
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTX,10000));
    feed(&d,j,-1,false,true); assert(!d.selected_exit);
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTX,32767));
    feed(&d,j,-1,false,true); assert(d.selected_exit);
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTX,0));
    feed(&d,j,-1,false,true);
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTX,-32768));
    feed(&d,j,-1,false,true); assert(!d.selected_exit);
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTX,0));
    feed(&d,j,-1,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,true,true); assert(!d.open && d.draining);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,false,true);

    /* Swap physical Confirm/Back exactly as a user controller remap does. */
    pad_remap_count=2;
    pad_remap[0][0]=BTN_CROSS; pad_remap[0][1]=SDL_GAMEPAD_BUTTON_EAST;
    pad_remap[1][0]=BTN_CIRCLE; pad_remap[1][1]=SDL_GAMEPAD_BUTTON_SOUTH;
    d=(ExitDialog){0}; feed(&d,j,-1,false,false);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,true,false);
    feed(&d,j,-1,false,true); assert(!d.open);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,true,true); assert(d.open);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,true,true); assert(d.selected_exit);
    feed(&d,j,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,true,true); assert(d.exiting);
    assert(!strcmp(exit_button_label(SDL_GAMEPAD_BUTTON_EAST,false),"B"));
    assert(!strcmp(exit_button_label(SDL_GAMEPAD_BUTTON_EAST,true),"Circle"));
    assert(!strcmp(exit_button_label(SDL_GAMEPAD_BUTTON_SOUTH,false),"A"));
    assert(!strcmp(exit_button_label(SDL_GAMEPAD_BUTTON_SOUTH,true),"Cross"));
    assert(!exit_dialog_actions(BTN_OPTIONS|BTN_TOUCHPAD|BTN_R1,0x4000));
    /* Both regional layouts through the real SDL sampler and prompt routing. */
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,false,true);
    test_confirm=BTN_CIRCLE; pad_remap_count=0;
    d=(ExitDialog){0}; feed(&d,j,-1,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,true,true); assert(!d.open);
    assert(!strcmp(sampled_confirm,"B") && !strcmp(sampled_back,"A"));
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,true,true); assert(d.open);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,true,true); assert(d.selected_exit);
    feed(&d,j,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,true,true); assert(d.exiting);
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,false,true);
    pad_remap_count=2; /* Circle-confirm plus custom swapped physical buttons. */
    d=(ExitDialog){0}; feed(&d,j,-1,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,true,true); assert(!d.open);
    assert(!strcmp(sampled_confirm,"A") && !strcmp(sampled_back,"B"));
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,true,true); assert(d.open);
    feed(&d,j,SDL_GAMEPAD_BUTTON_EAST,false,true);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,true,true); assert(!d.open && d.draining);
    feed(&d,j,SDL_GAMEPAD_BUTTON_SOUTH,false,true);
    if(gamepad) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    SDL_CloseJoystick(j); SDL_DetachVirtualJoystick(id); SDL_Quit();
    puts("exit_pad_test: PASS (SDL virtual controller, both regional layouts, remapped Confirm/Back and prompts)");
}
