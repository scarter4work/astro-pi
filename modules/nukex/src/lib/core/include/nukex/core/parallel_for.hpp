#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
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
///
/// `max_workers` (> 0) caps the thread count below hardware_concurrency, for
/// bodies whose per-worker memory is large -- a 24 MP frame being read,
/// debayered and star-detected holds about a gigabyte. <= 0 means no cap.
template <typename Body>
void parallel_for_dynamic(int count, int chunk, int max_workers, Body&& body) {
    if (count <= 0) return;
    if (chunk < 1) chunk = 1;

    const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int chunks = (count + chunk - 1) / chunk;
    const int cap = max_workers > 0 ? std::min(hw, max_workers) : hw;
    const int workers = std::min(cap, chunks);

    std::atomic<int> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::mutex error_mutex;

    // Claims and runs chunks until the range is exhausted (or, with
    // `one_chunk`, after a single chunk). Never throws.
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

    // Worker 0 takes the first chunk before any thread exists, so it always
    // participates (FitHeartbeat) even when spawned workers would otherwise
    // drain a small range before the caller got to it.
    run(0, true);

    // A spawn can fail (std::system_error: EAGAIN under a process or cgroup
    // limit, or memory pressure). Degrade to the workers already running --
    // as OpenMP does -- instead of letting the exception unwind past joinable
    // threads, which is std::terminate (and PixInsight going down with it).
    std::vector<std::thread> pool;
    try {
        pool.reserve(workers - 1);
        for (int w = 1; w < workers; ++w) pool.emplace_back(run, w, false);
    } catch (const std::exception&) {
        // fewer workers; worker 0 below finishes whatever is left
    }
    run(0, false);
    for (auto& t : pool) t.join();

    if (error) std::rethrow_exception(error);
}

/// parallel_for_dynamic for loops whose iterations are long (a frame read,
/// debayered and star-detected: seconds at 24 MP), where the CALLING thread
/// must stay responsive -- to ask the host for a cancel, which it may only do
/// from its own thread, and to report progress.
///
/// The calling thread runs no iteration: it calls `poll()` every `interval`
/// until every worker has finished. `body(i, worker)` runs on spawned threads
/// with `worker` in [1, workers]; worker 0 is never used, so a body must not
/// treat any worker as the calling thread. `poll` runs once more after the
/// workers have joined.
///
/// If no thread at all can be started (a process or cgroup limit), the
/// calling thread runs the iterations itself as worker 0, calling `poll()`
/// between them -- the old, coarser behaviour rather than none.
///
/// Exceptions: as parallel_for_dynamic -- the first one stops further
/// iterations from being claimed and is rethrown after the join; one thrown
/// by `poll` is rethrown the same way.
template <typename Body, typename Poll>
void parallel_for_polled(int count, int chunk, int max_workers, Body&& body, Poll&& poll,
                         std::chrono::milliseconds interval = std::chrono::milliseconds(20)) {
    if (count <= 0) { poll(); return; }
    if (chunk < 1) chunk = 1;

    const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int chunks = (count + chunk - 1) / chunk;
    const int cap = max_workers > 0 ? std::min(hw, max_workers) : hw;
    const int workers = std::max(1, std::min(cap, chunks));

    std::atomic<int> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::mutex m;
    std::condition_variable cv;
    int running = 0;

    auto run = [&](int worker) {
        try {
            for (;;) {
                if (failed.load(std::memory_order_relaxed)) break;
                const int begin = next.fetch_add(chunk, std::memory_order_relaxed);
                if (begin >= count) break;
                const int end = std::min(begin + chunk, count);
                for (int i = begin; i < end; ++i) body(i, worker);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lk(m);
            if (!error) error = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> pool;
    try {
        pool.reserve(static_cast<std::size_t>(workers));
        for (int w = 1; w <= workers; ++w) {
            {
                std::lock_guard<std::mutex> lk(m);
                ++running;
            }
            try {
                pool.emplace_back([&, w] {
                    run(w);
                    std::lock_guard<std::mutex> lk(m);
                    --running;
                    cv.notify_all();
                });
            } catch (...) {
                std::lock_guard<std::mutex> lk(m);
                --running;
                throw;
            }
        }
    } catch (const std::exception&) {
        // fewer workers than asked; with none, the caller works below
    }

    if (pool.empty()) {
        // No thread could be started: the calling thread does the work.
        try {
            for (;;) {
                if (failed.load(std::memory_order_relaxed)) break;
                const int begin = next.fetch_add(chunk, std::memory_order_relaxed);
                if (begin >= count) break;
                const int end = std::min(begin + chunk, count);
                for (int i = begin; i < end; ++i) { body(i, 0); poll(); }
            }
        } catch (...) {
            if (!error) error = std::current_exception();
        }
    } else {
        std::unique_lock<std::mutex> lk(m);
        while (running > 0) {
            lk.unlock();
            try {
                poll();
            } catch (...) {
                std::lock_guard<std::mutex> g(m);
                if (!error) error = std::current_exception();
                failed.store(true, std::memory_order_relaxed);
            }
            lk.lock();
            if (running > 0) cv.wait_for(lk, interval);
        }
    }
    for (auto& t : pool) t.join();
    if (!error) {
        try { poll(); } catch (...) { error = std::current_exception(); }
    }
    if (error) std::rethrow_exception(error);
}

/// Uncapped: one worker per hardware thread.
template <typename Body>
void parallel_for_dynamic(int count, int chunk, Body&& body) {
    parallel_for_dynamic(count, chunk, 0, std::forward<Body>(body));
}

} // namespace nukex
