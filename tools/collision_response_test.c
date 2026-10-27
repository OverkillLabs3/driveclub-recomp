/* Investigation only. Execute a few functions from the user's own 1.28 ELF in
 * private memory. No DriveClub process, SDL/FFB, file patch or installed
 * change. Collision event consumer is a recorder, not the game's score/UI
 * backend. */
#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define ABI __attribute__((sysv_abi))
static unsigned char *image;
static size_t image_size;
static void check(int ok, const char *s) {
  if (!ok) {
    fprintf(stderr, "FAIL %s\n", s);
    exit(1);
  }
  printf("PASS %s\n", s);
}
static uint64_t u64(const void *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}
static uint32_t u32(const void *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}
static uint16_t u16(const void *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
static float f32(const void *p) {
  float v;
  memcpy(&v, p, 4);
  return v;
}
static void pf(void *p, float v) { memcpy(p, &v, 4); }
static void pp(void *p, const void *v) { memcpy(p, &v, 8); }
static void jump(unsigned char *p, uintptr_t to) {
  const unsigned char op[] = {0xff, 0x25, 0, 0, 0, 0};
  memcpy(p, op, 6);
  memcpy(p + 6, &to, 8);
}
static uint64_t hash(const unsigned char *p, size_t n) {
  uint64_t h = UINT64_C(0xcbf29ce484222325);
  while (n--)
    h = (h ^ *p++) * UINT64_C(0x100000001b3);
  return h;
}
typedef void(ABI *Setter)(void *, float, float, float);
typedef void(ABI *Update)(void *, void *, float);
typedef float(ABI *Getter)(void *);
typedef void(ABI *RecordStep)(void *, float);
typedef void(ABI *Ctor)(void *);
typedef struct {
  _Alignas(16) unsigned char setup[0x1000], state[0x4000], body[0x300],
      iface[0x40], actor[0x20], stats[0x100];
  uintptr_t motion_vtable[40], actor_vtable[120];
  unsigned velocity_writes, events[6];
  float amount[6];
} Fixture;
static Fixture *current;
static ABI void wake(void *body) { (void)body; }
static ABI void velocity(void *motion, const void *v) {
  unsigned char *body = (unsigned char *)motion - 0x150;
  memcpy(body + 0x230, v, 16);
  ++current->velocity_writes;
}
static ABI float guest_pow(float base, float exponent) {
  return powf(base, exponent);
}
static ABI float zero(void *actor) {
  (void)actor;
  return 0;
}
static ABI void *actor_interface(void *actor) {
  (void)actor;
  return current->iface;
}
static ABI void event_record(void *stats, int type, float amount) {
  (void)stats;
  if (type < 0 || type > 5)
    abort();
  ++current->events[type];
  current->amount[type] = amount;
}
static void fixture(Fixture *s) {
  memset(s, 0, sizeof(*s));
  current = s;
  pp(s->setup + 8, s->state);
  pp(s->setup + 16, s->body);
  pp(s->body + 0x150, s->motion_vtable);
  s->motion_vtable[0x88 / 8] = (uintptr_t)velocity;
  pf(s->body + 0x230, 20);
  pf(s->state + 0x2adc, 20);
  pf(s->state + 0x2958, 1);
  ((Ctor)(image + 0x644430))(s->setup + 0x57c);
  pp(s->iface + 0x20, s->setup);
  pp(s->actor, s->actor_vtable);
  pp(s->stats + 0xa0, s->actor);
  s->actor_vtable[0x180 / 8] = (uintptr_t)zero;
  s->actor_vtable[0x250 / 8] = (uintptr_t)zero;
  s->actor_vtable[0x350 / 8] = (uintptr_t)actor_interface;
}
static void load(const char *path) {
  FILE *f = fopen(path, "rb");
  check(f != NULL, "own ELF opened");
  fseek(f, 0, SEEK_END);
  size_t n = ftell(f);
  rewind(f);
  unsigned char *e = malloc(n);
  check(e && fread(e, 1, n, f) == n, "own ELF read");
  fclose(f);
  uint64_t off = u64(e + 0x20);
  unsigned stride = u16(e + 0x36), count = u16(e + 0x38);
  check(off + (uint64_t)stride * count <= n, "headers bounded");
  for (unsigned i = 0; i < count; ++i) {
    unsigned char *p = e + off + i * stride;
    if (u32(p) == 1 && u64(p + 16) + u64(p + 40) > image_size)
      image_size = u64(p + 16) + u64(p + 40);
  }
  image = VirtualAlloc(NULL, image_size, MEM_RESERVE | MEM_COMMIT,
                       PAGE_EXECUTE_READWRITE);
  check(image != NULL, "private image");
  for (unsigned i = 0; i < count; ++i) {
    unsigned char *p = e + off + i * stride;
    if (u32(p) != 1)
      continue;
    check(u64(p + 8) + u64(p + 32) <= n, "segment bounded");
    memcpy(image + u64(p + 16), e + u64(p + 8), u64(p + 32));
  }
  free(e);
}
static void patches(const char *path) {
  FILE *f = fopen(path, "rb");
  check(f != NULL, "private patch file opened");
  unsigned char head[24];
  check(fread(head, 1, 24, f) == 24 && !memcmp(head, "BBPATCH2", 8),
        "patch header");
  uint64_t count = u64(head + 16);
  for (uint64_t i = 0; i < count; ++i) {
    unsigned char h[16];
    check(fread(h, 1, 16, f) == 16, "patch record");
    uint64_t a = u64(h), n = u64(h + 8);
    check(n && n <= 4096 && a + n <= image_size, "patch bounds");
    check(fread(image + a, 1, n, f) == n, "patch bytes");
  }
  check(fgetc(f) == EOF, "no trailing patch data");
  fclose(f);
}
static void scenario_final(int kind) {
  Fixture s;
  fixture(&s);
  unsigned char *c = s.setup + 0x57c;
  uint64_t seed = UINT64_C(0x8765432112345678);
  memcpy(image + 0x191b2a0, &seed, 8);
  if (kind == 1)
    ((Setter)(image + 0x644a70))(c, 1000000, 100, 0);
  if (kind == 2)
    ((Setter)(image + 0x644a30))(c, 1000000, 100, 0);
  if (kind == 3) {
    typedef void(ABI * Wall)(void *, float);
    ((Wall)(image + 0x644a20))(c, 25.f);
  }
  if (kind == 4) {
    pf(c + 0x30, 1.5f);
    c[0x42] = 1;
    pf(c + 0x34, 1.5f);
  }
  bool started = false, expired = true;
  unsigned writes_after = 0, previous_writes = 0;
  for (int n = 0; n < 260; ++n) {
    ((RecordStep)(image + 0xc39750))(s.stats, 1.f / 60);
    ((Update)(image + 0x644520))(c, s.setup, 1.f / 60);
    float t = f32(c + (kind == 1   ? 0x28
                       : kind == 2 ? 0x20
                       : kind == 3 ? 0x18
                                   : 0x30));
    if (n == 0)
      started = t > 0 && t <= 1.00001f;
    if (kind != 4 && n >= 61) {
      expired &= t <= 0 && ((Getter)(image + 0x6449f0))(c) == 0;
      writes_after += s.velocity_writes - previous_writes;
    }
    previous_writes = s.velocity_writes;
    if (kind == 4 && n == 65)
      check(t > 0 && s.velocity_writes > 0,
            "corner-cut still acts beyond one second");
  }
  if (kind != 4) {
    check(started && expired && s.velocity_writes > 0 && writes_after == 0,
          "native collision slows, lasts one second, then stops writing "
          "velocity");
  }
  if (kind == 1)
    check(s.events[4] == 1, "vehicle collision event retained");
  if (kind == 2)
    check(s.events[5] == 1, "world collision event retained");
}
int main(int argc, char **argv) {
  check(argc == 3, "usage: test_one_second eboot.elf private-patches.bin");
  load(argv[1]);
  check(hash(image + 0x644520, 0x4b0) == UINT64_C(0xf38286adc43c3a97),
        "original native updater signature");
  check(hash(image + 0xc39750, 0x210) == UINT64_C(0x1bfd07399c1916e2),
        "native event producer signature");
  patches(argv[2]);
  check(image[0x644a20] == 0xc6 && image[0x644a70] == 0xc5,
        "native setter entries restored");
  uint64_t canary = 12345;
  pp(image + 0x166e250, &canary);
  jump(image + 0x938af0, (uintptr_t)wake);
  jump(image + 0x123e7b0, (uintptr_t)guest_pow);
  jump(image + 0xc39960, (uintptr_t)event_record);
  for (int kind = 1; kind <= 4; ++kind)
    scenario_final(kind);
  puts("PASS one-second vehicle/world/wall response and original CornerCut; no "
       "game/SDL/FFB started");
  return 0;
}
