#include "nukex/gpu/fit_heartbeat.hpp"
#include <string>

namespace nukex {

FitHeartbeat::FitHeartbeat(int total, long interval_ms)
    : total_(total),
      interval_ms_(interval_ms),
      t0_(std::chrono::steady_clock::now()) {}

void FitHeartbeat::tick(int thread_id, ProgressObserver& obs) {
    done_.fetch_add(1, std::memory_order_relaxed);
    if (thread_id != 0) return;
    report(obs);
}

void FitHeartbeat::report(ProgressObserver& obs) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0_).count();
    long last = last_report_ms_.load(std::memory_order_relaxed);
    if (now_ms - last < interval_ms_) return;
    if (!last_report_ms_.compare_exchange_strong(last, now_ms,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        return;  // another caller won the race
    }

    obs.advance(0,
        "    fitted " + std::to_string(done()) + "/" + std::to_string(total_)
        + " voxels (" + std::to_string(now_ms / 1000) + "s)");
}

} // namespace nukex
