/* Time-of-day telemetry and optional environment timelapse at the game's own time-of-day update boundary
 * (eboot 1.28). */
#ifdef _WIN32
typedef void(ABI *SceneTimeUpdate)(void *, float);
static SceneTimeUpdate scene_time_original;
static uint64_t scene_time_values, scene_time_stamp;
static uint32_t mod_hours_bits = 0xffffffffu;   /* an hour a mod script asked for (dcmod.set_time); all ones (a NaN) means none */
static ABI void scene_time_update(void *self, float dt) {
  const uint32_t asked = __atomic_exchange_n(&mod_hours_bits, 0xffffffffu, __ATOMIC_ACQ_REL);
  if (asked != 0xffffffffu) memcpy((unsigned char *)self + 0xc, &asked, 4);   /* the clock's hour, before the game advances it */
  scene_time_original(self, env_time_delta(self,dt));
  uint64_t values;
  memcpy(&values, (const unsigned char *)self + 0xc, sizeof(values));
  /* Publish the clock and its effective signed rate as one sample. The reader
   * must not combine an earlier clock with a newer keyboard/frame-scale value. */
  if (env_timelapse_enabled && __atomic_load_n(&env_action, __ATOMIC_ACQUIRE)) {
    float compression;
    memcpy(&compression, (unsigned char *)&values + 4, 4);
    compression *= (float)env_load_double(&env_frame_scale_bits);
    memcpy((unsigned char *)&values + 4, &compression, 4);
  }
  __atomic_store_n(&scene_time_values, values, __ATOMIC_RELAXED);
  __atomic_store_n(&scene_time_stamp, GetTickCount64(), __ATOMIC_RELEASE);
}
void runtime_mod_set_hours(float hours) {
  uint32_t bits;
  memcpy(&bits, &hours, 4);
  __atomic_store_n(&mod_hours_bits, bits, __ATOMIC_RELEASE);
}
int runtime_scene_debug_time(float *hours, float *compression) {
  uint64_t stamp = __atomic_load_n(&scene_time_stamp, __ATOMIC_ACQUIRE);
  if (!stamp || GetTickCount64() - stamp > 2000)
    return 0;
  uint64_t values = __atomic_load_n(&scene_time_values, __ATOMIC_RELAXED);
  memcpy(hours, &values, 4);
  memcpy(compression, (unsigned char *)&values + 4, 4);
  return 1;
}
static void scene_debug_jump(unsigned char *code, uintptr_t target) {
  const unsigned char op[6] = {0xff, 0x25, 0, 0, 0, 0};
  memcpy(code, op, 6);
  memcpy(code + 6, &target, 8);
}
static void scene_debug_install(void) {
  const char *enabled = getenv("BB_SCENE_OVERLAY");
  if (enabled && atoi(enabled) == 0 && !env_hooks_ready)
    return;
  const size_t offset = 0x75a00;
  /* Whole instructions only; no PC-relative operands in this prologue. */
  static const unsigned char expected[] = {
      0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
      0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xf8, 0,    0,    0};
  if (runtime_image_size < offset + sizeof expected ||
      memcmp(image + offset, expected, sizeof expected)) {
    env_timelapse_time_ready(0);
    puts("Scene overlay: time update signature unavailable");
    return;
  }
  unsigned char *code =
      VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!code) {env_timelapse_time_ready(0);return;}
  memcpy(code, expected, sizeof expected);
  scene_debug_jump(code + sizeof expected,
                   (uintptr_t)image + offset + sizeof expected);
  DWORD old;
  if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old)) {
    VirtualFree(code, 0, MEM_RELEASE);
    env_timelapse_time_ready(0);return;
  }
  FlushInstructionCache(GetCurrentProcess(), code, 4096);
  scene_time_original = (SceneTimeUpdate)code;
  scene_debug_jump(image + offset, (uintptr_t)scene_time_update);
  memset(image + offset + 14, 0x90, sizeof expected - 14);
  FlushInstructionCache(GetCurrentProcess(), image + offset, sizeof expected);
  env_timelapse_time_ready(1);
  puts("Scene overlay: current time-of-day telemetry enabled");
}
#else
void runtime_mod_set_hours(float hours) { (void)hours; }
int runtime_scene_debug_time(float *hours, float *compression) { return 0; }
static void scene_debug_install(void) {}
#endif
/* The game's clock for mod scripts (dcmod/state/env.lua.bin): hour of the day 0..24 and the game's time compression;
 * 0 when the clock is not running (menus). */
int runtime_mod_env_state(double *v, int max) {
    float hours, compression;
    if (max < 2 || !runtime_scene_debug_time(&hours, &compression)) return 0;
    v[0] = hours;
    v[1] = compression;
    return 2;
}
