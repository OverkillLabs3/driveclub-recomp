// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_copy.h"
#include "bbport_threads.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bbport_toggles.h"
#include "common/thread.h"

namespace BbCopy {
namespace {

thread_local bool in_copy_thread = false;

class Pool {
public:
    Pool() {
        // Copies are on the critical path (fences wait for them): normal priority, ~1/4 of
        // hardware threads (2 on 8-thread CPUs, capped at 4 — avoids SMT contention on 4C/8T).
        unsigned count = std::clamp(BbThreads::Available() / 4u, 1u, 4u);
        if (const char* env = std::getenv("BB_COPY_THREADS")) {
            count = static_cast<unsigned>(std::clamp(std::atoi(env), 0, 16));
        }
        epochs.emplace_back();
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this, i] { Loop(i); });
        }
    }
    ~Pool() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    bool Enabled() const {
        return !threads.empty();
    }

    void Async(std::function<void()> task) {
        bool wake = false;
        {
            std::scoped_lock lk{mutex};
            const u64 epoch = epoch_base + epochs.size() - 1;
            ++epochs.back().pending;
            ++pending;
            pending_approx.fetch_add(1, std::memory_order_release);
            Task t;
            t.kind = Task::Kind::Func;
            t.run = std::move(task);
            t.epoch = epoch;
            async_tasks.push_back(std::move(t));
            wake = idle_workers > 0;
        }
        if (wake) {
            cv.notify_one();
        }
    }

    void AsyncItem(const Item& item) {
        bool wake = false;
        {
            std::scoped_lock lk{mutex};
            const u64 epoch = epoch_base + epochs.size() - 1;
            ++epochs.back().pending;
            ++pending;
            pending_approx.fetch_add(1, std::memory_order_release);
            Task t;
            t.kind = Task::Kind::SingleItem;
            t.item = item;
            t.epoch = epoch;
            async_tasks.push_back(std::move(t));
            wake = idle_workers > 0;
        }
        if (wake) {
            cv.notify_one();
        }
    }

    void AsyncBatch(std::vector<Item>& items) {
        bool wake = false;
        std::vector<Item> replacement;
        {
            std::scoped_lock lk{mutex};
            const u64 epoch = epoch_base + epochs.size() - 1;
            ++epochs.back().pending;
            ++pending;
            pending_approx.fetch_add(1, std::memory_order_release);
            if (!free_batches.empty()) {
                replacement = std::move(free_batches.back());
                free_batches.pop_back();
            }
            Task t;
            t.kind = Task::Kind::Batch;
            t.batch = std::move(items);
            t.epoch = epoch;
            async_tasks.push_back(std::move(t));
            wake = idle_workers > 0;
        }
        items = std::move(replacement);
        if (items.capacity() < 256) {
            items.reserve(256);
        }
        if (wake) {
            cv.notify_one();
        }
    }

    void AfterCopies(std::function<void()> callback) {
        {
            std::unique_lock lk{mutex};
            if (epochs.size() > 1 || epochs.back().pending != 0 || draining) {
                // Runs once every copy issued so far is done, in order with earlier ones.
                epochs.back().callbacks.push_back(std::move(callback));
                epochs.emplace_back();
                return;
            }
        }
        callback();
    }

    void WaitAll() {
        // Fast path when no copies or callbacks are pending.
        if (pending_approx.load(std::memory_order_acquire) == 0) {
            return;
        }
        // The waiting thread helps drain queued tasks and blocks cleanly (no mutex spin)
        // when only in-flight worker tasks remain.
        while (true) {
            Task task;
            {
                std::unique_lock lk{mutex};
                ++done_waiters;
                done_cv.wait(lk, [&] {
                    return (pending == 0 && epochs.size() == 1 && !draining) ||
                           !async_tasks.empty();
                });
                --done_waiters;
                if (pending == 0 && epochs.size() == 1 && !draining) {
                    return;
                }
                task = std::move(async_tasks.front());
                async_tasks.pop_front();
            }
            RunAsync(task);
        }
    }

    void Run(std::size_t count, const std::function<void(std::size_t)>& task) {
        auto job = std::make_shared<Job>();
        job->task = &task;
        job->count = count;
        {
            std::scoped_lock lk{mutex};
            current = job;
            ++generation;
        }
        cv.notify_all();
        Work(*job);
        // Workers only call the task for indices below count, all done before this returns.
        while (job->done.load(std::memory_order_acquire) < count) {
            std::this_thread::yield();
        }
        std::scoped_lock lk{mutex};
        if (current == job) {
            current.reset();
        }
    }

private:
    using u64 = unsigned long long;
    struct Task {
        enum class Kind : unsigned char { Func, SingleItem, Batch };
        Kind kind = Kind::Func;
        std::function<void()> run;
        Item item{};
        std::vector<Item> batch;
        u64 epoch{};
    };
    /// Copies issued between two AfterCopies() calls, and the callbacks waiting for them.
    struct Epoch {
        std::size_t pending = 0;
        std::vector<std::function<void()>> callbacks;
    };
    struct Job {
        const std::function<void(std::size_t)>* task{};
        std::size_t count{};
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
    };

    void RunAsync(Task& task) {
        const bool was = in_copy_thread;
        in_copy_thread = true;
        if (task.kind == Task::Kind::SingleItem) {
            task.item.run(task.item);
        } else if (task.kind == Task::Kind::Batch) {
            for (const auto& item : task.batch) {
                item.run(item);
            }
            task.batch.clear();
        } else {
            task.run();
        }
        in_copy_thread = was;
        Complete(task.epoch, task.kind == Task::Kind::Batch ? &task.batch : nullptr);
    }

    /// Retires a task and runs the callbacks of epochs with nothing left, oldest first. One
    /// thread drains at a time so the callbacks keep their order.
    void Complete(u64 epoch, std::vector<Item>* recycled_batch = nullptr) {
        std::unique_lock lk{mutex};
        if (recycled_batch && free_batches.size() < 16) {
            free_batches.push_back(std::move(*recycled_batch));
        }
        --epochs[epoch - epoch_base].pending;
        --pending;
        if (draining) {
            return;
        }
        draining = true;
        while (true) {
            std::vector<std::function<void()>> ready;
            while (epochs.size() > 1 && epochs.front().pending == 0) {
                for (auto& callback : epochs.front().callbacks) {
                    ready.push_back(std::move(callback));
                }
                epochs.pop_front();
                ++epoch_base;
            }
            if (ready.empty()) {
                break;
            }
            lk.unlock();
            for (auto& callback : ready) {
                callback();
            }
            lk.lock();
        }
        draining = false;
        const bool all_done = (pending == 0 && epochs.size() == 1);
        const bool notify_waiters = all_done && done_waiters > 0;
        if (all_done) {
            pending_approx.store(0, std::memory_order_release);
        }
        lk.unlock();
        if (notify_waiters) {
            done_cv.notify_all();
        }
    }

    static void Work(Job& job) {
        const bool was = in_copy_thread;
        in_copy_thread = true;
        for (std::size_t i; (i = job.next.fetch_add(1, std::memory_order_relaxed)) < job.count;) {
            (*job.task)(i);
            job.done.fetch_add(1, std::memory_order_release);
        }
        in_copy_thread = was;
    }

    void Loop(unsigned index) {
        Common::SetCurrentThreadName(("bb:Copy" + std::to_string(index)).c_str());
        u64 seen = 0;
        std::unique_lock lk{mutex};
        while (true) {
            if (!stop && async_tasks.empty() && (generation == seen || !current)) {
                ++idle_workers;
                cv.wait(lk, [&] {
                    return stop || !async_tasks.empty() || (generation != seen && current);
                });
                --idle_workers;
            }
            if (stop) {
                return;
            }
            if (!async_tasks.empty()) {
                Task task = std::move(async_tasks.front());
                async_tasks.pop_front();
                lk.unlock();
                RunAsync(task);
                lk.lock();
                continue;
            }
            seen = generation;
            const auto job = current;
            lk.unlock();
            Work(*job);
            lk.lock();
        }
    }

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable done_cv;
    std::shared_ptr<Job> current;
    u64 generation = 0;
    bool stop = false;
    std::deque<Task> async_tasks;
    std::vector<std::vector<Item>> free_batches;
    std::deque<Epoch> epochs; ///< back() is open for new copies
    u64 epoch_base = 0;       ///< epoch number of epochs.front()
    std::size_t pending = 0;
    std::atomic<std::size_t> pending_approx{0};
    unsigned idle_workers = 0;
    unsigned done_waiters = 0;
    bool draining = false;
};

Pool& GetPool() {
    static Pool pool;
    return pool;
}

struct Batch {
    std::vector<Item> items;
    unsigned long long bytes = 0;
};
thread_local Batch batch;

} // namespace

bool Enabled() {
    return GetPool().Enabled() && !BbToggle::Disabled(BbToggle::ParallelCopies);
}

void Async(std::function<void()> task) {
    if (!Enabled()) {
        task();
        return;
    }
    GetPool().Async(std::move(task));
}

void AsyncItem(const Item& item) {
    if (!Enabled()) {
        item.run(item);
        return;
    }
    GetPool().AsyncItem(item);
}

void FlushBatch() {
    if (batch.items.empty()) {
        return;
    }
    batch.bytes = 0;
    if (!Enabled()) {
        for (const auto& item : batch.items) {
            item.run(item);
        }
        batch.items.clear();
        return;
    }
    GetPool().AsyncBatch(batch.items);
}

void QueueCopy(const Item& item) {
    if (!Enabled()) {
        item.run(item);
        return;
    }
    // Smaller batches (32 items / 64 KiB, done before the next fence) measured slower.
    batch.items.push_back(item);
    batch.bytes += item.size;
    if (batch.bytes >= 512 * 1024 || batch.items.size() >= 256) {
        FlushBatch();
    }
}

void AfterCopies(std::function<void()> callback) {
    FlushBatch();
    GetPool().AfterCopies(std::move(callback));
}

void WaitAsync() {
    // The caller's own batch runs here: handing it over only to wait for it costs a wakeup.
    if (!batch.items.empty()) {
        for (const auto& item : batch.items) {
            item.run(item);
        }
        batch.items.clear();
        batch.bytes = 0;
    }
    GetPool().WaitAll();
}

void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task) {
    if (count <= 1 || in_copy_thread || !Enabled()) {
        for (std::size_t i = 0; i < count; ++i) {
            task(i);
        }
        return;
    }
    GetPool().Run(count, task);
}

} // namespace BbCopy
