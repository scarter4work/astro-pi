#pragma once

#include "nukex/core/progress_observer.hpp"
#include <atomic>
#include <chrono>

namespace nukex {

/// Rate-limited, thread-safe progress emitter for the OpenMP-parallelised
/// Ceres fit loop in GPUExecutor::execute_phase_b.
///
/// Every worker thread calls tick() after completing a voxel.  Only thread 0
/// ever touches the observer, and only once per `interval_ms`, keeping the
/// observer's mutex off the hot path and log noise bounded (~one line per
/// interval regardless of core count).  The rate limit is enforced with an
/// atomic compare-exchange on the "last report time" so simultaneous
/// thread-0 iterations (theoretically impossible under OpenMP static/dynamic
/// scheduling, but defensive) emit at most once.
///
/// Message shape: `    fitted K/N voxels (Ts)` where T is whole seconds
/// since construction.
class FitHeartbeat {
public:
    FitHeartbeat(int total, long interval_ms);

    /// Records one completed voxel.  If `thread_id == 0` and the interval
    /// has elapsed since the last emission, emits via `obs.advance(0, ...)`.
    void tick(int thread_id, ProgressObserver& obs);

    int done() const { return done_.load(std::memory_order_relaxed); }

private:
    std::atomic<int>  done_{0};
    std::atomic<long> last_report_ms_{0};
    int  total_;
    long interval_ms_;
    std::chrono::steady_clock::time_point t0_;
};

} // namespace nukex
