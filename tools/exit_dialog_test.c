/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "src/exit_dialog.h"
#include <assert.h>
#include <stdio.h>

static void step(ExitDialog *d, bool root, unsigned held, unsigned pulse, float x, float y) {
    exit_dialog_step(d,(ExitDialogInput){.root=root,.available=true,.held=held,
        .pressed=pulse,.neutral=!held && x==0 && y==0,.stick_x=x,.stick_y=y});
}
static void open_dialog(ExitDialog *d) {
    step(d,true,0,0,0,0); step(d,true,EXIT_BACK,0,0,0);
    assert(d->open && !d->selected_exit && !d->exiting);
}
int main(void) {
    for (uint32_t confirm=0x2000;confirm<=0x4000;confirm+=0x2000) {
        const uint32_t back=confirm ^ 0x6000;
        assert(exit_dialog_actions(confirm,confirm)==EXIT_CONFIRM);
        assert(exit_dialog_actions(back,confirm)==EXIT_BACK);
        assert(exit_dialog_actions(0x6000,confirm)==(EXIT_BACK|EXIT_CONFIRM));
        ExitDialog regional={0}; step(&regional,true,0,0,0,0);
        step(&regional,true,exit_dialog_actions(confirm,confirm),0,0,0); assert(!regional.open);
        step(&regional,true,0,0,0,0);
        step(&regional,true,exit_dialog_actions(back,confirm),0,0,0); assert(regional.open);
        step(&regional,true,0,0,0,0); step(&regional,true,EXIT_NEXT,0,0,0);
        step(&regional,true,0,0,0,0);
        step(&regional,true,exit_dialog_actions(confirm,confirm),0,0,0); assert(regional.exiting);
    }
    assert(!exit_dialog_actions(0x6000,0));
    ExitDialog d={0};
    /* A Back held across a submenu -> root transition must never open. */
    step(&d,false,0,0,0,0); step(&d,false,EXIT_BACK,0,0,0);
    for(int i=0;i<240;++i) step(&d,true,EXIT_BACK,0,0,0);
    assert(!d.open);
    step(&d,true,0,0,0,0); step(&d,true,EXIT_BACK,0,0,0);
    assert(d.open && !d.selected_exit);
    /* The opening press and every held sample leave Cancel selected. */
    for(int i=0;i<240;++i) step(&d,true,EXIT_BACK,0,0,0);
    assert(d.open && !d.selected_exit);
    step(&d,true,0,0,0,0); step(&d,true,EXIT_BACK,0,0,0);
    assert(!d.open && exit_dialog_captures(&d));
    for(int i=0;i<240;++i) step(&d,true,EXIT_BACK,0,0,0);
    assert(!d.open && exit_dialog_captures(&d));
    step(&d,true,0,0,0,0); assert(!exit_dialog_captures(&d));

    d=(ExitDialog){0}; open_dialog(&d);
    step(&d,true,0,0,0,0);
    /* Stick drift stays below activation; a single deflection selects Exit.
     * A held deflection cannot toggle again, nor can crossing to the opposite
     * side without first returning to neutral. */
    for(int i=0;i<120;++i) step(&d,true,0,0,.3f,-.2f);
    assert(!d.selected_exit);
    step(&d,true,0,0,.8f,0); assert(d.selected_exit);
    for(int i=0;i<120;++i) step(&d,true,0,0,-.8f,0);
    assert(d.selected_exit);
    step(&d,true,0,0,0,0); step(&d,true,0,0,-.8f,0);
    assert(!d.selected_exit);
    step(&d,true,0,0,0,0);
    step(&d,true,EXIT_NEXT,0,0,0); assert(d.selected_exit);
    step(&d,true,0,0,0,0); step(&d,true,EXIT_CONFIRM,0,0,0);
    assert(d.exiting && exit_dialog_captures(&d));
    for(int i=0;i<240;++i) step(&d,true,EXIT_CONFIRM,0,0,0);
    assert(d.exiting);

    d=(ExitDialog){0}; open_dialog(&d);
    step(&d,true,0,0,0,0); step(&d,true,EXIT_CONFIRM,0,0,0);
    assert(!d.open && !d.exiting && d.draining); /* default Cancel */
    for(int i=0;i<240;++i) step(&d,true,EXIT_CONFIRM,0,0,0);
    assert(d.draining);
    step(&d,true,0,0,.5f,0); assert(d.draining);
    step(&d,true,0,0,0,0); assert(!d.draining);

    /* Short keyboard taps survive until the game's next sample. */
    d=(ExitDialog){0}; step(&d,true,0,0,0,0);
    step(&d,true,0,EXIT_BACK,0,0); assert(d.open);
    step(&d,true,0,0,0,0);
    step(&d,true,0,EXIT_BACK,0,0); assert(!d.open);

    /* No scene other than a positively identified root can open it. */
    d=(ExitDialog){0}; step(&d,false,0,0,0,0);
    step(&d,false,EXIT_BACK,EXIT_BACK,0,0); assert(!d.open);
    d=(ExitDialog){0}; open_dialog(&d);
    step(&d,false,0,0,0,0); assert(!d.open && d.draining);
    d=(ExitDialog){0}; step(&d,true,0,0,0,0);
    exit_dialog_step(&d,(ExitDialogInput){.root=true,.available=false,.held=EXIT_BACK});
    assert(!d.open);

    /* Mouse and keyboard/controller activation share the same action. */
    d=(ExitDialog){0}; open_dialog(&d); exit_dialog_click(&d,false);
    assert(!d.open && d.draining && !d.exiting);
    d=(ExitDialog){0}; open_dialog(&d); exit_dialog_click(&d,true);
    assert(d.exiting);
    puts("exit_dialog_test: PASS");
}
