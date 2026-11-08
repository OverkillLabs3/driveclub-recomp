// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: two-stage draw pipeline (docs/parallel_gpu.md, "Two-stage draw pipeline").
//
// The GPU command thread (stage A) decodes PM4, keeps the register file and selects pipelines;
// for a direct draw it writes a packet (the register blocks changed since the previous packet,
// the stages' user data, the draw parameters) into this ring and goes on decoding. The draw
// recording thread (stage B) owns the caches, the barriers and the command recording while
// packets are in flight: it applies each packet to its own copy of the registers and runs the
// rest of the draw. Anything else stage A does on that state waits for stage B to run dry
// (Drain) and then runs on stage A as before.

#pragma once

#include <cstdlib>
#include "bbport_threads.h"
#include "bbport_copy.h"
#include "bbport_toggles.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include "bbport_cpu.h"

#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class DrawPipe {
public:
    /// Runs a packet on stage B.
    using Handler = void (*)(void* context, const u8* packet, u32 size);

    DrawPipe(Handler handler_, void* context_) : handler{handler_}, context{context_} {
        ring = std::make_unique<u8[]>(Capacity);
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~DrawPipe() {
        Drain();
        thread.request_stop();
        published.fetch_add(0, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// True on the draw recording thread.
    [[nodiscard]] static bool OnStageB() noexcept {
        return on_stage_b;
    }

    [[nodiscard]] u32 StageBThreadId() const noexcept {
        return stage_b_tid.load(std::memory_order_acquire);
    }

    /// Stage A: space for a packet of `size` bytes (8-byte aligned), valid until Commit().
    u8* Begin(u32 size) {
        size = Align(size + sizeof(Header));
        ASSERT(size <= Capacity / 4);
        u64 at = head;
        const u64 offset = at % Capacity;
        if (offset + size > Capacity) {
            // Wrap: the rest of the ring is skipped (a header with zero size marks it).
            WaitForSpace(at, Capacity - offset);
            reinterpret_cast<Header*>(ring.get() + offset)->size = 0;
            at += Capacity - offset;
            head = at;
            published.store(at, std::memory_order_release);
        }
        WaitForSpace(at, size);
        pending_size = size;
        auto* header = reinterpret_cast<Header*>(ring.get() + at % Capacity);
        header->size = size;
        return reinterpret_cast<u8*>(header + 1);
    }

    /// Stage A: hands the packet from Begin() to stage B.
    void Commit(u32 payload_size) {
        reinterpret_cast<Header*>(ring.get() + head % Capacity)->payload = payload_size;
        head += pending_size;
        published.store(head, std::memory_order_seq_cst);
        ++packets;
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    /// Why stage A drained (statistics): a PM4 opcode, or one of these.
    enum Reason : u32 {
        ReasonConstRam = 256,
        ReasonCommands,
        ReasonCompute,
        ReasonSubmissionEnd,
        ReasonDraw,
        ReasonRasterizer,
        NumReasons,
    };
    std::array<u64, NumReasons> drains_by_reason{};
    std::array<u64, NumReasons> cycles_by_reason{};

    /// Stage A: waits until stage B has run every committed packet.
    void Drain(u32 reason = ReasonRasterizer) {
        if (consumed.load(std::memory_order_acquire) == head) {
            return;
        }
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = BbCpu::Cycles();
        const auto wall = std::chrono::steady_clock::now();
        bool reported = false;
        for (u32 spins = 0; consumed.load(std::memory_order_acquire) != head; ++spins) {
            if (spins < 4096) {
                BbCpu::Pause();
            } else {
                std::this_thread::yield();
                // bbport: a recording thread stuck for 2 s (a GPU hang inside it) is reported once, with the
                // reason of this drain, so a freeze leaves a trace in the log.
                if (!reported && !(spins & 1023) && std::chrono::steady_clock::now() - wall > std::chrono::seconds(2)) {
                    reported = true;
                    std::printf("Draw pipe: the recording thread has not finished its packets for 2 s (drain reason "
                                "%u, %llu ring bytes queued)\n",
                                reason, static_cast<unsigned long long>(head - consumed.load()));
                    std::fflush(stdout);
                }
            }
        }
        const u64 waited = BbCpu::Cycles() - start;
        drain_cycles += waited;
        cycles_by_reason[slot] += waited;
    }

    /// Stage A: position after the last committed packet; Reached(position) once B ran it.
    [[nodiscard]] u64 Head() const noexcept {
        return head;
    }
    [[nodiscard]] bool Reached(u64 position) const noexcept {
        return consumed.load(std::memory_order_acquire) >= position;
    }

    /// Stage A: waits until stage B has run everything before `position` (it keeps going after it).
    void WaitFor(u64 position) {
        if (Reached(position)) {
            return;
        }
        for (u32 spins = 1; !Reached(position); ++spins) {
            BbCpu::Pause();
            if (!(spins & 1023)) {
                std::this_thread::yield();
            }
        }
    }

    /// Packets published that stage B has not finished (any thread; BB_PIPE_CHECK).
    [[nodiscard]] bool InFlight() const noexcept {
        return consumed.load(std::memory_order_acquire) != published.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Idle() const noexcept {
        return consumed.load(std::memory_order_acquire) == head;
    }

    /// Statistics (stage A): packets, drains that had to wait, cycles waited, cycles stage B
    /// spent running packets.
    u64 packets = 0, drains = 0, drain_cycles = 0;
    /// Stage B adds to it after every packet: a cache line of its own, apart from stage A's counters above and its
    /// write position below (sharing one cost both stages a cache miss per packet).
    alignas(64) std::atomic<u64> busy_cycles{0};
    char busy_cycles_line[56]{};

private:
    struct Header {
        u32 size;    ///< bytes to the next packet (0: wrap to the ring start)
        u32 payload; ///< bytes written by the producer
    };
    static constexpr u64 Capacity = 16ull << 20;

    static constexpr u32 Align(u32 size) {
        return (size + 63) & ~63u;
    }

    void WaitForSpace(u64 at, u64 size) {
        while (at + size - consumed.load(std::memory_order_acquire) > Capacity) {
            BbCpu::Pause();
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:DrawRec");
        // The renderer's bottleneck: a core of its own and the GPU threads' priority (BB_CORE_OWNERS bit 2).
        if (BbThreads::CoreOwners() & BbThreads::kDrawThread) {
            BbThreads::MakeImportant();
            BbThreads::ReserveCoreForCurrentThread(BbThreads::kDrawThread);
        }
        on_stage_b = true;
        stage_b_tid.store(static_cast<u32>(gettid()), std::memory_order_release);
        BbStats::pipe_stage_b.store(static_cast<u32>(gettid()), std::memory_order_relaxed);
        u64 at = 0;
        while (true) {
            // Packets follow each other within microseconds while a frame is decoded: spin,
            // and sleep only after a longer pause (between frames).
            u64 available = published.load(std::memory_order_acquire);
            if (available == at) {
                // With few hardware threads (Steam Deck: 8) a long spin takes time from the
                // game's own threads. BB_PIPE_SPIN_US overrides.
                static const auto spin_time = std::chrono::microseconds([] {
                    if (const char* env = std::getenv("BB_PIPE_SPIN_US")) {
                        return std::max(0, std::atoi(env));
                    }
                    return BbThreads::Available() >= 12 ? 200 : 50;
                }());
                const auto spin_until = std::chrono::steady_clock::now() + spin_time;
                for (u32 spins = 1; available == at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    BbCpu::Pause();
                    if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.store(true, std::memory_order_seq_cst);
                        if (published.load(std::memory_order_seq_cst) == at &&
                            !stop.stop_requested()) {
                            wake.wait(seen, std::memory_order_seq_cst);
                        }
                        sleeping.store(false, std::memory_order_relaxed);
                    }
                    available = published.load(std::memory_order_acquire);
                }
            }
            const auto* header = reinterpret_cast<const Header*>(ring.get() + at % Capacity);
            if (header->size == 0) {
                at += Capacity - at % Capacity;
                consumed.store(at, std::memory_order_release);
                continue;
            }
            const u64 start = BbCpu::Cycles();
            const bool check = BbStats::PipeCheckEnabled();
            if (check) {
                BbStats::pipe_owner.store(static_cast<u32>(gettid()), std::memory_order_release);
            }
            handler(context, reinterpret_cast<const u8*>(header + 1), header->payload);
            if (check) {
                BbStats::pipe_owner.store(0, std::memory_order_release);
            }
            busy_cycles.fetch_add(BbCpu::Cycles() - start, std::memory_order_relaxed);
            at += header->size;
            // bbport: guest copies this thread queued (BbCopy batches are per thread) must reach the copy pool
            // before stage A can see the ring empty: A drains, then submits or signals the guest, and waits only
            // for its own batch and the pool. Unpublished, the GPU read staging memory not yet written (missing
            // geometry and noise textures in DriveClub). Published when the ring runs dry, batched otherwise.
            if (published.load(std::memory_order_acquire) == at) {
                BbCopy::FlushBatch();
            }
            consumed.store(at, std::memory_order_release);
        }
    }

    static inline thread_local bool on_stage_b = false;
    Handler handler;
    void* context;
    std::unique_ptr<u8[]> ring;
    u64 head = 0; ///< stage A's write position
    u32 pending_size = 0;
    alignas(64) std::atomic<u64> published{0};
    alignas(64) std::atomic<u64> consumed{0};
    alignas(64) std::atomic<u32> wake{0};
    std::atomic<bool> sleeping{false};
    std::atomic<u32> stage_b_tid{0};
    std::jthread thread;
};

} // namespace Vulkan
