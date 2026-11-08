#include "video_core/texture_cache/cmask_metadata.h"
#include <array>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <thread>
using namespace std::chrono_literals;
enum class Kind { CMask, FMask, HTile, Revoked };
struct Entry {
    Kind type;
    int clear_mask = -1;
};
using Map = std::map<std::uint64_t, Entry>;
int main(int argc, char** argv) {
    using namespace BbCmaskMetadata;
    std::mutex tc, pending, meta;
    Map entries;
    std::atomic<std::uint64_t> gen{7};
    constexpr std::uint64_t a = 0x400655f000;
    Register(meta, entries, a, Entry{Kind::CMask, -2}, Kind::CMask,
             Kind::Revoked);
    Register(meta, entries, a + 64, Entry{Kind::FMask, -3}, Kind::CMask,
             Kind::Revoked);
    Register(meta, entries, a + 128, Entry{Kind::HTile, -4}, Kind::CMask,
             Kind::Revoked);
    if (argc > 1) { // Isolated expected-hang negative control, killed by the
                    // Python watchdog.
        std::scoped_lock held{tc};
        std::scoped_lock deferred{pending};
        std::scoped_lock recursive{tc};
        return 99;
    }
    // Actual v2 deadlock schedule: GC holds a real nonrecursive TC mutex,
    // Finish executes a FaultManager-like upload callback under pending_ops
    // mutex.
    {
        std::scoped_lock gc{tc};
        std::scoped_lock deferred{pending};
        const auto r = Revoke(meta, entries, a, std::uint64_t{4096},
                              Kind::CMask, Kind::Revoked, gen);
        assert(r.count == 1 && r.first == a && gen == 8);
    }
    assert(entries.at(a).clear_mask == -2 &&
           !Query(meta, entries, a, Kind::Revoked));
    assert(!Cleared(meta, entries, a, 0, Kind::Revoked));
    assert(!Clear(meta, entries, a, Kind::Revoked) &&
           !Touch(meta, entries, a, 0, true, Kind::Revoked));
    assert(Revoke(meta, entries, a, std::uint64_t{4096}, Kind::CMask,
                  Kind::Revoked, gen)
                   .count == 0 &&
           gen == 8);
    assert(entries.at(a).clear_mask ==
           -2); // Tombstone and idempotency preserve slice state.
    assert(entries.at(a + 64).type == Kind::FMask &&
           entries.at(a + 64).clear_mask == -3);
    assert(entries.at(a + 128).type == Kind::HTile &&
           entries.at(a + 128).clear_mask == -4);
    Register(meta, entries, a, Entry{Kind::CMask}, Kind::CMask, Kind::Revoked);
    assert(Query(meta, entries, a, Kind::Revoked) == Kind::CMask &&
           entries.at(a).clear_mask == -2);
    assert(!Cleared(meta, entries, a, 0, Kind::Revoked) &&
           Cleared(meta, entries, a, 1, Kind::Revoked));
    assert(Touch(meta, entries, a, 0, true, Kind::Revoked) &&
           Clear(meta, entries, a, Kind::Revoked));
    assert(entries.at(a).clear_mask == -1);
    // Real overlap: B owns Meta, A owns TC and reaches the deferred upload. B's
    // leaf query exits without acquiring TC/pending; both threads must
    // complete.
    std::barrier both{2};
    std::promise<void> a_waiting;
    auto waiting = a_waiting.get_future();
    std::thread b([&] {
        std::unique_lock leaf{meta};
        both.arrive_and_wait();
        waiting.wait();
        std::this_thread::sleep_for(20ms);
    });
    std::thread t([&] {
        std::scoped_lock gc{tc};
        both.arrive_and_wait();
        a_waiting.set_value();
        std::scoped_lock p{pending};
        Revoke(meta, entries, a, std::uint64_t{4096}, Kind::CMask,
               Kind::Revoked, gen);
    });
    b.join();
    t.join();
    assert(gen == 9);
    // Concurrent registry operations use the SAME production leaf helpers.
    std::thread registrar([&] {
        for (unsigned i = 0; i < 2000; ++i) {
            Register(meta, entries, a, Entry{Kind::CMask}, Kind::CMask,
                     Kind::Revoked);
            Touch(meta, entries, a, 0, false, Kind::Revoked);
            Erase(meta, entries, std::array<std::uint64_t, 1>{a});
        }
    });
    std::thread reader([&] {
        for (unsigned i = 0; i < 2000; ++i) {
            Query(meta, entries, a, Kind::Revoked);
            Clear(meta, entries, a, Kind::Revoked);
            Revoke(meta, entries, a, std::uint64_t{4096}, Kind::CMask,
                   Kind::Revoked, gen);
        }
    });
    registrar.join();
    reader.join();
    // Explicitly characterize conservative initialization-before-bind behavior:
    // a later upload revokes eligibility, and real RT rebind preserves clear
    // state.
    Register(meta, entries, a, Entry{Kind::CMask, -2}, Kind::CMask,
             Kind::Revoked);
    assert(Query(meta, entries, a, Kind::Revoked));
    assert(Revoke(meta, entries, a - 16, std::uint64_t{16}, Kind::CMask,
                  Kind::Revoked, gen)
               .count == 0);
    assert(Revoke(meta, entries, a - 16, std::uint64_t{17}, Kind::CMask,
                  Kind::Revoked, gen)
               .count == 1);
    Register(meta, entries, a, Entry{Kind::CMask}, Kind::CMask, Kind::Revoked);
    assert(entries.at(a).clear_mask == -2);
    // Conflicting kinds are never retagged by CMask registration.
    Register(meta, entries, a + 64, Entry{Kind::CMask}, Kind::CMask,
             Kind::Revoked);
    assert(entries.at(a + 64).type == Kind::FMask);
    std::puts("PASS: forced GC/deferred callback, two-thread leaf ordering, "
              "concurrent registry stress, tombstone queries, clear-state "
              "preservation, idempotent generation, RT restore, conservative "
              "initialization boundary");
}
