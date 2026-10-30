// DriveClub intro pacing: presentation rate only, never the video or game clock.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace BbIntroPacing {
inline std::atomic<unsigned> players{0};

inline bool IsIntro(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto name = slash == path.npos ? path : path.substr(slash + 1);
    return name == "dc_attract_video_30fps_11.v3_256.mp4";
}

inline std::uint32_t EffectiveLimit(std::uint32_t configured) {
    if (!players.load(std::memory_order_relaxed)) return configured;
    // Do not raise a player's lower cap. Zero is the uncapped/legacy setting.
    return configured ? std::min(configured, 30u) : 30u;
}

class Playback {
public:
    Playback() = default;
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;
    ~Playback() { SetActive(false); }
    void SetActive(bool active) {
        std::lock_guard lock(mutex);
        if (active == counted) return;
        if (active) players.fetch_add(1, std::memory_order_relaxed);
        else players.fetch_sub(1, std::memory_order_relaxed);
        counted = active;
    }
private:
    std::mutex mutex;
    bool counted{};
};
} // namespace BbIntroPacing
