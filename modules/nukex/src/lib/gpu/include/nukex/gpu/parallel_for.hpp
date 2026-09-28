#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace nukex {

/// Dynamically scheduled parallel loop over [0, count): the std::thread
/// equivalent of `#pragma omp parallel for schedule(dynamic, chunk)`.
///
/// Why not OpenMP: NukeX ships as a PixInsight module, and OpenMP made it
/// NEED libgomp.so.1, which stock Debian/Ubuntu installs do not provide -- the
/// module then fails to load at all. Linking libgomp statically into a
/// dlopen'ed module is not an option either (initial-exec TLS). This loop
/// needs no runtime beyond libstdc++.
///
/// `body(i, worker)` runs once per index. `worker` is a dense id in
/// [0, workers); worker 0 is the calling thread and always participates
/// (FitHeartbeat reports only from worker 0). Chunks of `chunk` indices are
/// claimed from a shared atomic counter, so heterogeneous per-index cost
/// stays balanced.
///
/// Unlike OpenMP (where an escaping exception calls std::terminate and takes
/// PixInsight down), the first exception thrown by any body stops further
/// chunks from being claimed and is rethrown to the caller after all workers
/// have joined.
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

    auto run = [&](int worker) {
        try {
            for (;;) {
                if (failed.load(std::memory_order_relaxed)) return;
                const int begin = next.fetch_add(chunk, std::memory_order_relaxed);
                if (begin >= count) return;
                const int end = std::min(begin + chunk, count);
                for (int i = begin; i < end; ++i) body(i, worker);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lk(error_mutex);
            if (!error) error = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (int w = 1; w < workers; ++w) pool.emplace_back(run, w);
    run(0);
    for (auto& t : pool) t.join();

    if (error) std::rethrow_exception(error);
}

} // namespace nukex
