/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BB_RUNTIME_EXIT_H
#define BB_RUNTIME_EXIT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int open, selected_exit, exiting, device;
    char confirm[48], back[48];
} RuntimeExitView;
/* Game input thread, before sampling/returning input to the guest. */
void runtime_exit_tick(void);
/* Loader, before entering guest code: install the validated shutdown retry fix. */
void runtime_exit_initialize(void);
/* The game's regional Confirm button, before host controller remapping. */
uint32_t runtime_exit_confirm_button(void);
int runtime_exit_pad(uint32_t buttons, int lx, int ly, int neutral, int available,
                     int pad_activity, int device, const char *confirm, const char *back);
/* Window/present threads: queued actions, never call into the game here. */
int runtime_exit_captures(void);
void runtime_exit_key(unsigned action, int down, int repeat);
void runtime_exit_mouse_activity(void);
void runtime_exit_release_keys(void);
void runtime_exit_click(int exit_button);
void runtime_exit_hover(int exit_button);
RuntimeExitView runtime_exit_view(void);
#ifdef __cplusplus
}
#endif
#endif
