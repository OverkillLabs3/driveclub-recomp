#include "../gpu/shim/bbport_motion_blur.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>
int main() {
  // Post-process layout observed in DriveClub 1.28; unrelated words carry
  // sentinels.
  std::array<float, 176> original{};
  for (unsigned i = 0; i < original.size(); ++i)
    original[i] = float(i) * .001f;
  original[128] = .7f;
  original[133] = 1.2f;
  original[142] = -1.f;
  original[143] = .1f;
  original[144] = .2f;
  assert(original[144] == 0.2f);
  unsigned cases = 0;
  for (float fps : {15.f, 30.f, 45.f, 60.f, 90.f, 120.f, 144.f, 165.f, 240.f,
                    500.f, 1000.f}) {
    auto a = original, b = original;
    float dt = 1.f / fps;
    assert(DriveMotionBlur::Correct(a.data(), sizeof a, dt, 30));
    assert(DriveMotionBlur::Correct(b.data(), sizeof b, dt, 60));
    assert(std::abs(a[144] * dt - original[144] / 30.f) < 1e-8f);
    assert(std::abs(b[144] * dt - original[144] / 60.f) < 1e-8f);
    assert(a[144] == b[144] * 2.f);
    for (unsigned i = 0; i < a.size(); i++)
      if (i != 144)
        assert(a[i] == original[i] && b[i] == original[i]);
    cases += 2;
  }
  for (float exposure : {0.f, .2f, .3f, 1.f}) {
    auto a = original;
    a[144] = exposure;
    assert(DriveMotionBlur::Correct(a.data(), sizeof a, 1.f / 120, 30));
    assert(std::abs(a[144] / 120 - exposure / 30) < 1e-8f);
  }
  for (int reference : {30, 60}) {
    auto a = original;
    assert(DriveMotionBlur::Correct(a.data(), sizeof a, 1.f / reference,
                                    reference));
    assert(a == original);
  }
  for (float dt : {0.f, -.01f, 1.f, std::numeric_limits<float>::quiet_NaN(),
                   std::numeric_limits<float>::infinity()}) {
    auto a = original;
    assert(!DriveMotionBlur::Correct(a.data(), sizeof a, dt, 30));
    assert(a == original);
    cases++;
  }
  for (float projection : {-1.0000001192092896f, -.9999999403953552f}) {
    auto a = original;
    a[142] = projection;
    assert(DriveMotionBlur::Correct(a.data(), sizeof a, 1.f / 120, 30));
    assert(std::abs(a[144] - .8f) < 1e-6f);
    assert(a[142] == projection);
  }
  for (unsigned i : {128, 133, 142, 143}) {
    auto a = original;
    a[i] = 0;
    auto before = a;
    assert(!DriveMotionBlur::Correct(a.data(), sizeof a, 1.f / 60, 30));
    assert(a == before);
  }
  for (int mode : {0, 1, 120}) {
    auto a = original;
    assert(!DriveMotionBlur::Correct(a.data(), sizeof a, 1.f / 60, mode));
    assert(a == original);
  }
  DriveMotionBlur::IntervalHistory history;
  assert(history.Observe(33333, .033333f) == .033333f);
  assert(history.Observe(41666, .008333f) == .033333f);
  assert(history.Observe(41666, .008333f) ==
         .033333f); // another binding/view, same frame
  assert(history.Observe(58332, .016666f) == .008333f);
  assert(history.Observe(65332, .007f) == .016666f);
  assert(history.Observe(300000, .01f) == .01f); // scene gap resets
  assert(history.Observe(1000, .02f) == .02f);   // clock reset
  assert(history.Observe(3000, 0) == 0);         // pause/invalid
  auto a = original;
  assert(!DriveMotionBlur::Correct(a.data(), 512, 1.f / 60, 30));
  assert(a == original);
  printf("%u cases: ", cases);
  puts("PASS: post-process exposure invariants; reference 30/60 unchanged; "
       "constant exposure at 15..1000 FPS; only MB word changes; "
       "invalid/paused/unrelated data unchanged.");
}
