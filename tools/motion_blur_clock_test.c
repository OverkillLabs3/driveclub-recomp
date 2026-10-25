#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
static unsigned char *image;
static uintptr_t runtime_image_base;
static uint64_t runtime_image_size;
#include "../src/runtime_motion_blur.inc"
int main(void) {
  runtime_image_size = 0x1911108;
  image = calloc(1, runtime_image_size);
  assert(image);
  runtime_image_base = (uintptr_t)image;
  const unsigned char getter[] = {0x48, 0x8d, 0x05, 0x99, 0x7b,
                                  0x89, 0x01, 0x48, 0x8b, 0x00};
  memcpy(image + 0x79560, getter, sizeof getter);
  unsigned char *root = calloc(1, 0x978), *manager = calloc(1, 0x1e8),
                *clock = calloc(1, 0x80);
  assert(root && manager && clock);
  memcpy(image + 0x1911100, &root, 8);
  memcpy(root + 0x970, &manager, 8);
  memcpy(manager + 0x1e0, &clock, 8);
  int64_t elapsed = 50000;
  memcpy(clock + 0x58, &elapsed, 8);
  int64_t sim = 8333, real = 99999;
  memcpy(clock + 0x60, &sim, 8);
  memcpy(clock + 0x20, &real, 8);
  assert(runtime_motion_blur_delta() > .008332f &&
         runtime_motion_blur_delta() < .008334f);
  float dt = 0;
  uint64_t stamp = 0;
  assert(runtime_motion_blur_clock(&dt, &stamp) && stamp == 50000);
  sim = 16666;
  memcpy(clock + 0x60, &sim, 8);
  assert(runtime_motion_blur_delta() > .016665f &&
         runtime_motion_blur_delta() < .016667f);
  sim = 0;
  memcpy(clock + 0x60, &sim, 8);
  assert(runtime_motion_blur_delta() == 0);
  sim = 1000000;
  memcpy(clock + 0x60, &sim, 8);
  assert(runtime_motion_blur_delta() == 0);
  memset(image + 0x1911100, 0, 8);
  assert(runtime_motion_blur_delta() == 0);
  image[0x79560] ^= 1;
  assert(runtime_motion_blur_delta() == 0);
  free(root);
  free(manager);
  free(clock);
  free(image);
  puts("PASS: 1.28 getter signature guard and simulation timer layout; "
       "real-time field ignored; invalid pointers/pause/hitches/signature "
       "safely bypass.");
}
