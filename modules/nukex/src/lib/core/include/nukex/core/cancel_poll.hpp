#pragma once

#include "nukex/core/progress_observer.hpp"
#include <atomic>

namespace nukex {

/// Cancellation that long loops -- parallel ones included -- can check often.
///
/// The engine used to ask its observer only at progress calls, so a cancel
/// landed when the current stage next reported: measured 4.7 s into the
/// measuring pass, 15.9 s into frame loading and alignment (which reported
/// nothing at all), 1.5 s into fitting. Checkpoints inside those loops fix
/// that, and this is what they check.
///
/// The observer is the host's (PixInsight's console, the app's progress
/// sink) and is only ever asked from the thread that owns it. Inside
/// parallel_for_dynamic that is worker 0 -- always the calling thread -- so
/// poll(0) asks the observer and every other worker reads the shared flag.
/// Code on a thread of its own (a background loader) uses cancelled(), which
/// never calls the observer. Once set the flag stays set.
class CancelPoll {
public:
    explicit CancelPoll(ProgressObserver& obs) : obs_(obs) {}
    CancelPoll(const CancelPoll&)            = delete;
    CancelPoll& operator=(const CancelPoll&) = delete;

    /// From the calling thread: ask the observer.
    bool check() {
        if (!flag_.load(std::memory_order_relaxed) && obs_.is_cancelled())
            flag_.store(true, std::memory_order_relaxed);
        return flag_.load(std::memory_order_relaxed);
    }

    /// From inside a parallel_for_dynamic body.
    bool poll(int worker) {
        return worker == 0 ? check() : flag_.load(std::memory_order_relaxed);
    }

    /// From any thread, without asking the observer.
    bool cancelled() const { return flag_.load(std::memory_order_relaxed); }

private:
    ProgressObserver& obs_;
    std::atomic<bool> flag_{false};
};

} // namespace nukex
