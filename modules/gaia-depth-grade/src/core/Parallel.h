// Dynamically scheduled parallel loop, copied from NukeX
// (modules/nukex/src/lib/gpu/include/nukex/gpu/parallel_for.hpp) into the gdg namespace so
// gdg_core does not depend on the NukeX tree. No OpenMP: libgomp is not on stock distros, and a
// dlopen'ed PixInsight module cannot link it statically.
#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace gdg {

/// The std::thread equivalent of `#pragma omp parallel for schedule(dynamic, chunk)` over
/// [0, count). `body(i, worker)` runs once per index; `worker` is a dense id in [0, workers) and
/// worker 0 is the calling thread. The first exception thrown by any body stops further chunks
/// from being claimed and is rethrown to the caller after every worker has joined.
template <typename Body>
void parallel_for_dynamic(int count, int chunk, Body&& body) {
    if (count <= 0) return;
    if (chunk < 1) chunk = 1;

    const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int chunks = (count + chunk - 1) / chunk;
    const int workers = std::min(hw, chunks);

    std::atomic<int> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::mutex error_mutex;

    auto run = [&](int worker, bool one_chunk) {
        try {
            do {
                if (failed.load(std::memory_order_relaxed)) return;
                const int begin = next.fetch_add(chunk, std::memory_order_relaxed);
                if (begin >= count) return;
                const int end = std::min(begin + chunk, count);
                for (int i = begin; i < end; ++i) body(i, worker);
            } while (!one_chunk);
        } catch (...) {
            std::lock_guard<std::mutex> lk(error_mutex);
            if (!error) error = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
        }
    };

    run(0, true);

    // A failed spawn (EAGAIN under a process limit) degrades to the workers already running
    // instead of unwinding past joinable threads (std::terminate).
    std::vector<std::thread> pool;
    try {
        pool.reserve(workers - 1);
        for (int w = 1; w < workers; ++w) pool.emplace_back(run, w, false);
    } catch (const std::exception&) {
    }
    run(0, false);
    for (auto& t : pool) t.join();

    if (error) std::rethrow_exception(error);
}

/// Runs `body(i)` over [0, count): in parallel when `work` (an estimate of the elementary
/// operations) is large enough to pay for the threads, serially otherwise. Small calls -- the
/// per-star render windows -- must not spawn a thread pool each.
template <typename Body>
void parallel_for_if(long long work, int count, Body&& body) {
    constexpr long long kParallelWork = 1LL << 20;
    if (work < kParallelWork || count < 2) {
        for (int i = 0; i < count; ++i) body(i);
        return;
    }
    parallel_for_dynamic(count, 1, [&](int i, int) { body(i); });
}

}  // namespace gdg
