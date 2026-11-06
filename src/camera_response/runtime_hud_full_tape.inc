/* The shared native-call wrappers retain their observation scope API, but the
 * production build neither records snapshots nor opens capture resources. */
typedef struct HudTapeScope { struct HudTapeScope *previous; } HudTapeScope;
static _Thread_local HudTapeScope *hud_tape_current;
static _Thread_local int hud_tape_private_input;
static void hud_tape_begin(HudTapeScope *scope, void *self, void *subject, float dt) {
    (void)self; (void)subject; (void)dt;
    scope->previous = hud_tape_current;
}
static void hud_tape_input_raw(uintptr_t observation, uintptr_t target,
                             uintptr_t state, unsigned kind,
                             const float vectors[4][4]) {
    (void)observation; (void)target; (void)state; (void)kind; (void)vectors;
}
static void hud_tape_end(HudTapeScope *scope) {
    hud_tape_current = scope->previous;
}
