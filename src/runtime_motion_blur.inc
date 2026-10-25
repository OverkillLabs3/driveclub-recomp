/* Read-only DriveClub 1.28 simulation-frame interval, in seconds.
   The renderer uses this for motion-blur exposure, never to advance physics. */
#ifdef _WIN32
static int motion_blur_read(uintptr_t address, void *out, size_t size) {
  SIZE_T got = 0;
  return address >= 0x10000 &&
         ReadProcessMemory(GetCurrentProcess(), (void *)address, out, size,
                           &got) &&
         got == size;
}
int runtime_motion_blur_clock(float *dt, uint64_t *stamp) {
  /* Signature of the engine's simulation-delta getter (0x79560).
     Its +0x60 field is measured in microseconds; +0x20 is real time instead. */
  static const unsigned char signature[] = {0x48, 0x8d, 0x05, 0x99, 0x7b,
                                            0x89, 0x01, 0x48, 0x8b, 0x00};
  if (!runtime_image_base || runtime_image_size < 0x1911108 ||
      memcmp(image + 0x79560, signature, sizeof signature))
    return 0;
  uintptr_t root = 0, manager = 0, clock = 0;
  int64_t values[2] = {0, 0};
  if (!motion_blur_read(runtime_image_base + 0x1911100, &root, sizeof root) ||
      !motion_blur_read(root + 0x970, &manager, sizeof manager) ||
      !motion_blur_read(manager + 0x1e0, &clock, sizeof clock) ||
      !motion_blur_read(clock + 0x58, values, sizeof values) ||
      values[1] <= 0 || values[1] > 100000 || values[0] < 0)
    return 0;
  *dt = (float)values[1] * 0.000001f;
  *stamp = (uint64_t)values[0];
  return 1;
}
float runtime_motion_blur_delta(void) {
  float dt = 0;
  uint64_t stamp = 0;
  return runtime_motion_blur_clock(&dt, &stamp) ? dt : 0.0f;
}
#else
int runtime_motion_blur_clock(float *dt, uint64_t *stamp) {
  (void)dt;
  (void)stamp;
  return 0;
}
float runtime_motion_blur_delta(void) { return 0.0f; }
#endif
