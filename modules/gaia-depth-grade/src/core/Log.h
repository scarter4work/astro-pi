// Logging sink for the core (PCL-free). The module routes these to the PixInsight console; the
// tests capture them. Either callback may be empty (the message is then dropped).
#pragma once

#include <functional>
#include <string>

namespace gdg {

struct Log {
    std::function<void(const std::string&)> info, warning;
};

inline void log_info(const Log& log, const std::string& msg) {
    if (log.info) log.info(msg);
}

inline void log_warning(const Log& log, const std::string& msg) {
    if (log.warning) log.warning(msg);
}

}  // namespace gdg
