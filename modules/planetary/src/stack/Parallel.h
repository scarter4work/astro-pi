#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace planetary::stack {

// Starts a thread running `fn`; ParallelBands' seam for tests that make thread creation fail.
using ThreadSpawner = std::function<std::thread(std::function<void()>)>;

// Fewest rows a band holds before it is worth a thread of its own.
constexpr int kMinBandRows = 16;

// Runs body(r0, r1) over contiguous bands [r0, r1) that cover 0 .. rows exactly once: `threads`
// bands (fewer when a band would hold under kMinBandRows rows; < 1 counts as 1). A single band runs
// on the calling thread; with several, every band runs on a thread started here and the calling
// thread only waits for them. Each row is in exactly one band, so a body that writes only
// its own rows gives the same result for any thread count.
// Never leaves a thread running and never reaches std::terminate (this runs inside PixInsight):
//  - if starting a thread throws partway (std::system_error when the process is out of threads),
//    the threads already started are joined, then that exception is rethrown;
//  - if a band throws, the other bands run to completion, all threads are joined, and the first
//    exception (by band order of completion) is rethrown.
inline void ParallelBands(int rows, int threads, const std::function<void(int, int)>& body,
                          const ThreadSpawner& spawn = {}) {
    if (rows <= 0) return;
    const int bands = std::max(1, std::min(threads, rows / kMinBandRows));
    if (bands == 1) {
        body(0, rows);
        return;
    }
    std::mutex m;
    std::exception_ptr error;
    auto run = [&](int b) {
        try {
            body(static_cast<int>(static_cast<long long>(rows) * b / bands),
                 static_cast<int>(static_cast<long long>(rows) * (b + 1) / bands));
        } catch (...) {
            std::lock_guard<std::mutex> lock(m);
            if (!error) error = std::current_exception();
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(bands));
    try {
        for (int b = 0; b < bands; ++b) {
            std::function<void()> task = [&run, b] { run(b); };
            pool.push_back(spawn ? spawn(std::move(task)) : std::thread(std::move(task)));
        }
    } catch (...) {
        for (std::thread& t : pool)
            if (t.joinable()) t.join();
        throw;
    }
    for (std::thread& t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

// The worker pools that read frames (MeasureAll, FitNoiseModel, RunStack's passes): `count` threads run
// work(w) for w = 0 .. count - 1, each claiming its own share and returning when `stop` is set or nothing
// is left; a worker that throws keeps the first exception and sets `stop`. The calling thread waits,
// calling poll() at least every 10 ms (its progress reports); poll() returning true -- a cancel -- sets
// `stop`. Every started thread is joined on every way out, a thread that cannot be started
// (std::system_error) included, which then leaves through here. The caller decides what to throw:
// cancelled first, then the first worker error.
struct PoolOutcome {
    bool cancelled = false;
    std::exception_ptr error;
};
template <class Work, class Poll>
PoolOutcome RunPool(int count, std::atomic<bool>& stop, Work work, Poll poll) {
    PoolOutcome out;
    std::mutex m;
    std::condition_variable cv;
    int finished = 0;
    auto run = [&](int w) {
        try {
            work(w);
        } catch (...) {
            std::lock_guard<std::mutex> lock(m);
            if (!out.error) out.error = std::current_exception();
            stop = true;
        }
        std::lock_guard<std::mutex> lock(m);
        ++finished;
        cv.notify_all();
    };
    std::vector<std::thread> pool;
    struct Joiner {
        std::vector<std::thread>& pool;
        std::atomic<bool>& stop;
        ~Joiner() {
            stop = true;
            for (std::thread& t : pool)
                if (t.joinable()) t.join();
        }
    } joiner{pool, stop};
    pool.reserve(static_cast<size_t>(std::max(0, count)));
    for (int k = 0; k < count; ++k) pool.emplace_back(run, k);
    std::unique_lock<std::mutex> lock(m);
    while (finished < count) {
        cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return finished == count; });
        lock.unlock();
        if (poll() && !out.cancelled) {  // poll runs every time (its reports), a cancel counts once
            out.cancelled = true;
            stop = true;
        }
        lock.lock();
    }
    lock.unlock();
    for (std::thread& t : pool) t.join();
    return out;
}

}  // namespace planetary::stack
