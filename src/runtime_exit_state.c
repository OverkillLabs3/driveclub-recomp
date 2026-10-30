/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "exit_dialog.h"
#include <math.h>

bool exit_dialog_captures(const ExitDialog *d) { return d->open || d->draining || d->exiting; }
unsigned exit_dialog_actions(uint32_t buttons) {
    unsigned actions=0;
    if(buttons & 0x2000) actions|=EXIT_BACK;
    if(buttons & 0x4000) actions|=EXIT_CONFIRM;
    if(buttons & (0x10|0x80)) actions|=EXIT_PREVIOUS;
    if(buttons & (0x20|0x40)) actions|=EXIT_NEXT;
    return actions;
}
static void close_dialog(ExitDialog *d) { d->open=false; d->draining=true; d->root_armed=false; }
void exit_dialog_click(ExitDialog *d, bool exit_button) {
    if (!d->open || d->exiting) return;
    d->selected_exit=exit_button;
    if (exit_button) d->exiting=true;
    else close_dialog(d);
}
void exit_dialog_step(ExitDialog *d, ExitDialogInput in) {
    unsigned edge=(in.held & ~d->previous) | in.pressed;
    d->previous=in.held;
    if (d->exiting) return;
    if (d->draining) {
        if (in.neutral && !in.held && !in.pressed) d->draining=false;
        return;
    }
    if (!in.root || !in.available) {
        d->root_armed=false;
        if (d->open) close_dialog(d);
        return;
    }
    if (!d->open) {
        if (!(in.held & EXIT_BACK) && !(in.pressed & EXIT_BACK)) d->root_armed=true;
        if (d->root_armed && (edge & EXIT_BACK)) {
            d->open=true; d->selected_exit=false; d->root_armed=false;
            d->stick_armed=false;
        }
        return; /* The opening press cannot also cancel/confirm. */
    }
    if (edge & EXIT_BACK) { close_dialog(d); return; }
    if (edge & EXIT_CONFIRM) { exit_dialog_click(d,d->selected_exit); return; }
    const float x=in.stick_x,y=in.stick_y;
    if (fabsf(x)<0.25f && fabsf(y)<0.25f) d->stick_armed=true;
    if (d->stick_armed && (fabsf(x)>0.65f || fabsf(y)>0.65f)) {
        const float axis=fabsf(x)>=fabsf(y) ? x : y;
        d->selected_exit=axis>0;
        d->stick_armed=false;
    }
    if ((edge & (EXIT_PREVIOUS|EXIT_NEXT))==EXIT_PREVIOUS) d->selected_exit=false;
    if ((edge & (EXIT_PREVIOUS|EXIT_NEXT))==EXIT_NEXT) d->selected_exit=true;
}
