#include "nukex/calibration/qe_update_state.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace nukex {

QEUpdateState load_update_state(const std::string& path) {
    QEUpdateState s;   // defaults stand unless the file supplies better

    std::ifstream in(path, std::ios::binary);
    if (!in) return s;

    std::ostringstream ss;
    ss << in.rdbuf();

    try {
        const auto doc = nlohmann::json::parse(ss.str());
        s.enabled              = doc.value("enabled", s.enabled);
        s.interval_days        = doc.value("interval_days", s.interval_days);
        s.last_check_unix      = doc.value("last_check_unix", s.last_check_unix);
        s.last_result          = doc.value("last_result", s.last_result);
        s.installed_db_version = doc.value("installed_db_version", s.installed_db_version);
        s.declined_version     = doc.value("declined_version", s.declined_version);
    } catch (const std::exception&) {
        return QEUpdateState{};   // corrupt: defaults, not an error
    }

    // A nonsensical interval would either hammer the endpoint or disable
    // checks outright; clamp rather than trust the file.
    if (s.interval_days < 1)   s.interval_days = 1;
    if (s.interval_days > 365) s.interval_days = 365;
    return s;
}

bool save_update_state(const std::string& path, const QEUpdateState& state) {
    namespace fs = std::filesystem;

    nlohmann::json doc;
    doc["enabled"]              = state.enabled;
    doc["interval_days"]        = state.interval_days;
    doc["last_check_unix"]      = state.last_check_unix;
    doc["last_result"]          = state.last_result;
    doc["installed_db_version"] = state.installed_db_version;
    doc["declined_version"]     = state.declined_version;

    std::error_code ec;
    const fs::path dest(path);
    if (dest.has_parent_path()) {
        fs::create_directories(dest.parent_path(), ec);
        if (ec) return false;
    }

    // Same temp-then-rename discipline as the database install, so an
    // interrupted write cannot leave state that parses to something wrong.
    const fs::path tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << doc.dump(2) << "\n";
        out.flush();
        if (!out) { fs::remove(tmp, ec); return false; }
    }

    fs::rename(tmp, dest, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

bool should_check_now(const QEUpdateState& state, long long now_unix) {
    if (!state.enabled) return false;
    if (now_unix < state.last_check_unix) return true;   // clock moved back
    return (now_unix - state.last_check_unix) >=
           static_cast<long long>(state.interval_days) * 86400LL;
}

} // namespace nukex
