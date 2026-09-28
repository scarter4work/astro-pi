#include "catch_amalgamated.hpp"

#include "nukex/gpu/parallel_for.hpp"

#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using nukex::parallel_for_dynamic;

TEST_CASE("parallel_for_dynamic: visits every index exactly once", "[parallel_for]") {
    const int n = 100003;  // not a multiple of the chunk
    std::vector<std::atomic<int>> hits(n);
    parallel_for_dynamic(n, 256, [&](int i, int) { hits[i].fetch_add(1); });
    for (int i = 0; i < n; ++i)
        REQUIRE(hits[i].load() == 1);
}

TEST_CASE("parallel_for_dynamic: empty and tiny ranges", "[parallel_for]") {
    std::atomic<int> calls{0};
    parallel_for_dynamic(0, 256, [&](int, int) { calls++; });
    REQUIRE(calls == 0);
    parallel_for_dynamic(3, 256, [&](int, int) { calls++; });
    REQUIRE(calls == 3);
}

TEST_CASE("parallel_for_dynamic: worker ids are dense and include 0", "[parallel_for]") {
    // FitHeartbeat only reports from worker 0, so worker 0 must always run
    // iterations, even when the range is large enough to use every thread.
    std::mutex m;
    std::set<int> ids;
    parallel_for_dynamic(50000, 16, [&](int, int worker) {
        std::lock_guard<std::mutex> lk(m);
        ids.insert(worker);
    });
    REQUIRE(ids.count(0) == 1);
    const int max_workers = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    REQUIRE(*ids.rbegin() < max_workers);
}

TEST_CASE("parallel_for_dynamic: runs on more than one thread", "[parallel_for]") {
    if (std::thread::hardware_concurrency() < 2) SKIP("single-core host");
    std::mutex m;
    std::set<std::thread::id> threads;
    parallel_for_dynamic(20000, 1, [&](int, int) {
        std::this_thread::yield();
        std::lock_guard<std::mutex> lk(m);
        threads.insert(std::this_thread::get_id());
    });
    REQUIRE(threads.size() > 1);
}

TEST_CASE("parallel_for_dynamic: an exception in the body is rethrown to the caller", "[parallel_for]") {
    // Under OpenMP an exception escaping the parallel region calls
    // std::terminate -- inside PixInsight that takes the whole application
    // down. The replacement must surface it as an ordinary exception.
    std::atomic<int> after{0};
    REQUIRE_THROWS_AS(
        parallel_for_dynamic(10000, 8, [&](int i, int) {
            if (i == 4321) throw std::runtime_error("boom");
            after++;
        }),
        std::runtime_error);
    REQUIRE(after.load() < 10000);
}
