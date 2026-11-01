// DriveClub 1.28 motion-blur exposure. This only edits a private post-process
// constant copy.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace DriveMotionBlur {
inline constexpr std::size_t Bytes = 704;
inline constexpr std::size_t ExposureWord = 144;
// The immutable camera/motion matrices belong to the preceding simulation step.
// An engine-frame identity prevents multiple views/bindings from shifting
// twice.
struct IntervalHistory {
  std::uint64_t stamp{};
  float latest{}, interval{};
  float Observe(std::uint64_t current_stamp, float current_dt) {
    if (!std::isfinite(current_dt) || current_dt < 0.001f ||
        current_dt > 0.1f) {
      stamp = 0;
      latest = interval = 0;
      return 0;
    }
    if (stamp && current_stamp == stamp)
      return interval;
    interval = stamp && current_stamp > stamp && current_stamp - stamp <= 100000
                   ? latest
                   : current_dt;
    stamp = current_stamp;
    latest = current_dt;
    return interval;
  }
};
// The same constants with the exposure at 0: no blur. VR with alternate frames: the camera moves by the eyes'
// distance every frame, and the blur drew it as fast sideways motion (everything near smeared across). The blur
// shares its shader with the game's anti-aliasing, which FSR and DLSS start from, so it is not patched out there.
inline bool Zero(void *data, std::size_t size) {
  if (size != Bytes || !data)
    return false;
  std::array<float, Bytes / sizeof(float)> constants;
  std::memcpy(constants.data(), data, size);
  if (!(constants[128] > 0.0f && constants[133] > 0.0f &&
        std::abs(constants[142] + 1.0f) < 0.00001f && constants[143] > 0.0f &&
        constants[143] < 1.0f))
    return false;
  const float zero = 0.0f;
  std::memcpy(static_cast<unsigned char *>(data) + ExposureWord * sizeof(float),
              &zero, sizeof(zero));
  return true;
}
inline bool Correct(void *data, std::size_t size, float simulated_dt,
                    int reference_fps) {
  if (size != Bytes || !data || (reference_fps != 30 && reference_fps != 60) ||
      !std::isfinite(simulated_dt) || simulated_dt < 0.001f ||
      simulated_dt > 0.1f)
    return false;
  std::array<float, Bytes / sizeof(float)> constants;
  std::memcpy(constants.data(), data, size);
  // Post-process projection followed by MotionBlur's exposure multiplier.
  // Camera constants (including those used by DLAA/FSR) have another layout.
  // The engine rebuilds this matrix: its -1 term can differ by a few ULPs.
  if (!(constants[128] > 0.0f && constants[133] > 0.0f &&
        std::abs(constants[142] + 1.0f) < 0.00001f && constants[143] > 0.0f &&
        constants[143] < 1.0f))
    return false;
  const float original = constants[ExposureWord];
  if (!std::isfinite(original) || original < 0.0f || original > 1.0f)
    return false;
  const float corrected = original / (float(reference_fps) * simulated_dt);
  std::memcpy(static_cast<unsigned char *>(data) + ExposureWord * sizeof(float),
              &corrected, sizeof(corrected));
  return true;
}
} // namespace DriveMotionBlur
