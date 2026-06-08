#include "catch_amalgamated.hpp"
#include "nukex/gpu/fit_heartbeat.hpp"
#include "nukex/core/progress_observer.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace nukex;

namespace {

/// Spy observer that records every advance() detail message.
/// Thread-safe so the same spy can be passed to a parallel region.
class SpyObserver : public ProgressObserver {
public:
    void begin_phase(const std::string&, int) override {}
    void advance(int, const std::string& detail) override {
        std::lock_guard<std::mutex> g(mtx_);
        messages_.push_back(detail);
    }
    void end_phase() override {}
    void message(const std::string&) override {}
    bool is_cancelled() const override { return false; }

    std::vector<std::string> fit_messages() const {
        std::lock_guard<std::mutex> g(mtx_);
        std::vector<std::string> out;
        for (const auto& m : messages_) {
            if (m.find("fitted") != std::string::npos) out.push_back(m);
        }
        return out;
    }
private:
    mutable std::mutex mtx_;
    std::vector<std::string> messages_;
};

} // namespace

TEST_CASE("FitHeartbeat: does not emit before interval elapses",
          "[gpu][heartbeat]") {
    FitHeartbeat hb(100, 1000);  // 1 s interval
    SpyObserver obs;

    for (int i = 0; i < 10; i++) hb.tick(0, obs);

    REQUIRE(hb.done() == 10);
    REQUIRE(obs.fit_messages().empty());
}

TEST_CASE("FitHeartbeat: emits from thread 0 after interval elapses",
          "[gpu][heartbeat]") {
    FitHeartbeat hb(100, 50);  // 50 ms interval
    SpyObserver obs;

    hb.tick(0, obs);
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    hb.tick(0, obs);

    auto fits = obs.fit_messages();
    REQUIRE(fits.size() == 1);
    REQUIRE(fits[0].find("fitted 2/100 voxels") != std::string::npos);
}

TEST_CASE("FitHeartbeat: non-zero threads never emit",
          "[gpu][heartbeat]") {
    FitHeartbeat hb(10, 10);
    SpyObserver obs;

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    hb.tick(1, obs);
    hb.tick(2, obs);
    hb.tick(3, obs);

    REQUIRE(hb.done() == 3);
    REQUIRE(obs.fit_messages().empty());
}

TEST_CASE("FitHeartbeat: rate-limited to one emit per interval",
          "[gpu][heartbeat]") {
    FitHeartbeat hb(100, 100);
    SpyObserver obs;

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    for (int i = 0; i < 50; i++) hb.tick(0, obs);  // all inside one interval

    REQUIRE(obs.fit_messages().size() == 1);
}

TEST_CASE("FitHeartbeat: multiple intervals produce multiple emits",
          "[gpu][heartbeat]") {
    FitHeartbeat hb(10, 30);
    SpyObserver obs;

    for (int round = 0; round < 3; round++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        hb.tick(0, obs);
    }

    auto fits = obs.fit_messages();
    REQUIRE(fits.size() >= 2);
    // Done counts are monotonic across the emits.
    int last_k = -1;
    for (const auto& m : fits) {
        auto pos = m.find("fitted ");
        REQUIRE(pos != std::string::npos);
        int k = std::stoi(m.substr(pos + 7));
        REQUIRE(k > last_k);
        last_k = k;
    }
}

TEST_CASE("FitHeartbeat: concurrent ticks count every worker",
          "[gpu][heartbeat]") {
    // Many threads hammering tick() — done() must match total work.
    // Also confirms no data race under TSan (if enabled).
    FitHeartbeat hb(4000, 1000);
    SpyObserver obs;

    std::vector<std::thread> workers;
    const int n_threads = 4;
    const int per_thread = 1000;
    for (int t = 0; t < n_threads; t++) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < per_thread; i++) hb.tick(t, obs);
        });
    }
    for (auto& w : workers) w.join();

    REQUIRE(hb.done() == n_threads * per_thread);
}
