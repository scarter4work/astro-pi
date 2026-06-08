// src/lib/core/include/nukex/core/progress_observer.hpp
#pragma once

#include <string>

namespace nukex {

/// Abstract progress observer for pipeline instrumentation.
/// Phases nest: begin_phase/end_phase pairs can be called inside
/// an outer phase. The adapter decides how to render nesting.
class ProgressObserver {
public:
    virtual ~ProgressObserver() = default;

    /// Open a new phase scope with a human-readable name and total step count.
    virtual void begin_phase(const std::string& name, int total_steps) = 0;

    /// Advance the current phase by `steps` (0 = detail-only, no bar movement).
    /// Optional detail string for substep labels (e.g., "debayering").
    virtual void advance(int steps = 1, const std::string& detail = {}) = 0;

    /// Close the current phase scope. Must match a prior begin_phase().
    virtual void end_phase() = 0;

    /// Freeform log message (GPU info, warnings, timing).
    virtual void message(const std::string& text) = 0;

    /// Check if the user has requested cancellation.
    virtual bool is_cancelled() const = 0;
};

/// No-op observer for unit tests and headless use.
class NullProgressObserver : public ProgressObserver {
public:
    void begin_phase(const std::string&, int) override {}
    void advance(int, const std::string&) override {}
    void end_phase() override {}
    void message(const std::string&) override {}
    bool is_cancelled() const override { return false; }
};

/// Static instance for use as default when no observer is provided.
inline NullProgressObserver& null_progress_observer() {
    static NullProgressObserver instance;
    return instance;
}

} // namespace nukex
