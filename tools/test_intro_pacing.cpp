#include "bbport_intro_pacing.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    using namespace BbIntroPacing;
    assert(IsIntro("newui\\art\\dc_attract_video_30fps_11.v3_256.mp4"));
    assert(IsIntro("/app0/newui/art/dc_attract_video_30fps_11.v3_256.mp4"));
    assert(!IsIntro("newui/art/unrelated.mp4"));
    assert(!IsIntro("dc_attract_video_30fps_11.v3_256.mp4.backup"));
    for (auto cap : {0u, 15u, 30u, 60u, 72u, 90u, 120u}) {
        assert(EffectiveLimit(cap) == cap);
        {
            Playback intro;
            intro.SetActive(true);
            intro.SetActive(true); // repeated start must not leak ownership
            assert(EffectiveLimit(cap) == (cap ? std::min(cap, 30u) : 30u));
            intro.SetActive(false); // pause/stop, including repeated stop
            intro.SetActive(false);
            assert(EffectiveLimit(cap) == cap);
            intro.SetActive(true); // resume
        } // close/error destruction restores the configured cap
        assert(EffectiveLimit(cap) == cap);
    }
    {
        Playback first, second;
        first.SetActive(true); second.SetActive(true);
        first.SetActive(false);
        assert(EffectiveLimit(90) == 30);
        second.SetActive(false);
        assert(EffectiveLimit(90) == 90);
    }
    {
        Playback shared;
        std::vector<std::thread> threads;
        for (int t=0; t<4; ++t) threads.emplace_back([&] {
            for (int i=0; i<10000; ++i) { shared.SetActive(true); shared.SetActive(false); }
        });
        for (auto& thread : threads) thread.join();
        shared.SetActive(false);
    }
    assert(players.load() == 0);
    assert(EffectiveLimit(72) == 72);
    std::cout << "PASS intro classification, cap preservation, pause/resume, stop/close, overlapping players and concurrent lifecycle\n";
}
