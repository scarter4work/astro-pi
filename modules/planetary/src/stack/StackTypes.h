#pragma once
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace planetary::stack {

// Progress and cancel hooks for a long-running engine call. Either may be empty.
// report(phase, done, total) is called from the engine's calling thread; cancelled()
// is polled between units of work, and a true result makes the engine throw StackCancelled.
struct Progress {
    std::function<void(const std::string& phase, int64_t done, int64_t total)> report;
    std::function<bool()> cancelled;
};

// Thrown when Progress::cancelled() returned true; the engine leaves no partial result.
class StackCancelled : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace planetary::stack
