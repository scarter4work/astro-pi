#pragma once
#include <algorithm>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace planetary::stack {

// Starts a thread running `fn`; ParallelBands' seam for tests that make thread creation fail.
using ThreadSpawner = std::function<std::thread(std::function<void()>)>;

// Runs body(r0, r1) over contiguous bands [r0, r1) that cover 0 .. rows exactly once: `threads`
// bands (fewer when a band would hold under kMinBandRows rows; < 1 counts as 1), the first on the
// calling thread when there is only one. Each row is in exactly one band, so a body that writes only
// its own rows gives the same result for any thread count.
// Never leaves a thread running and never reaches std::terminate (this runs inside PixInsight):
//  - if starting a thread throws partway (std::system_error when the process is out of threads),
//    the threads already started are joined, then that exception is rethrown;
//  - if a band throws, the other bands run to completion, all threads are joined, and the first
//    exception (by band order of completion) is rethrown.
constexpr int kMinBandRows = 16;

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

}  // namespace planetary::stack
