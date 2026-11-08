/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BB_EXIT_DIALOG_H
#define BB_EXIT_DIALOG_H
#include <stdbool.h>
#include <stdint.h>

enum { EXIT_BACK=1, EXIT_CONFIRM=2, EXIT_PREVIOUS=4, EXIT_NEXT=8 };
typedef struct {
    bool open, selected_exit, exiting, draining, root_armed, stick_armed;
    unsigned previous;
} ExitDialog;
typedef struct {
    bool root, available, neutral;
    unsigned held, pressed;
    float stick_x, stick_y;
} ExitDialogInput;
/* Pure state machine; the caller serializes access. No timers or key repeat. */
void exit_dialog_step(ExitDialog *d, ExitDialogInput in);
void exit_dialog_click(ExitDialog *d, bool exit_button);
bool exit_dialog_captures(const ExitDialog *d);
/* Mapped PS-style pad outputs shared with runtime_pad's existing bindings. */
unsigned exit_dialog_actions(uint32_t buttons, uint32_t confirm_button);
#endif
