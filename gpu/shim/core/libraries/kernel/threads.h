// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"
#include "core/libraries/kernel/threads/pthread.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        Stop();
        std::scoped_lock lk{mutex};
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("bb:hle");
            func(stop);
            done->store(true);
        });
    }
    // A thread may stop its own Thread object (AvPlayer's decoder threads end that way); it detaches
    // instead of joining. Other threads may join the same object at that moment (the demuxer thread,
    // sceAvPlayerClose): the handle is taken out under the lock and joined outside it, so a join never
    // runs on a handle another thread detached (that read freed thread memory: a crash in
    // _pthread_join when the intro video closed).
    // bbport: these are called from guest code, which a C++ exception cannot unwind: a failed
    // join is logged and the thread detached instead.
    void Join() {
        std::jthread joining;
        std::shared_ptr<std::atomic<bool>> done;
        {
            std::scoped_lock lk{mutex};
            if (!thread.joinable()) return;
            if (thread.get_id() == std::this_thread::get_id()) {
                thread.detach();
                return;
            }
            joining = std::move(thread);
            done = finished;
        }
        try {
            joining.join();
        } catch (const std::system_error& error) {
            // Windows (libc++ on Win32 threads): the join can fail although the thread is fine.
            // Wait for its function to return, so nothing it uses is freed under it, then let go.
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            for (int waited = 0; done && !done->load() && waited < 5000; ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (joining.joinable()) joining.detach();
        }
    }
    bool Joinable() const {
        std::scoped_lock lk{mutex};
        return thread.joinable();
    }
    void Stop() {
        {
            std::scoped_lock lk{mutex};
            if (!thread.joinable()) return;
            thread.request_stop();
        }
        Join();
    }

private:
    mutable std::mutex mutex;
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
} // namespace Libraries::Kernel
