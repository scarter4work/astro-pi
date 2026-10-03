#ifndef NUKEX_CALIBRATION_QE_UPDATE_STATE_HPP
#define NUKEX_CALIBRATION_QE_UPDATE_STATE_HPP

#include <string>

namespace nukex {

// Persisted beside the Phase 8 user state in <user-data>/nukex4/.
struct QEUpdateState {
    bool        enabled              = true;
    int         interval_days        = 7;
    long long   last_check_unix      = 0;
    std::string last_result;
    int         installed_db_version = 0;
    // The version the user declined, so the prompt does not reappear every
    // interval for an answer already given -- without disabling updates.
    int         declined_version     = 0;
};

// Never fails: a missing or unparseable file yields defaults. Losing this
// file costs one extra check and nothing else, so treating it as an error
// would be noise rather than signal.
QEUpdateState load_update_state(const std::string& path);

bool save_update_state(const std::string& path, const QEUpdateState& state);

// True when a check is due. A last_check_unix in the FUTURE (clock
// correction, or a config copied from another machine) counts as due, so a
// bad timestamp cannot wedge updates off until the clock catches up.
bool should_check_now(const QEUpdateState& state, long long now_unix);

// The camera database actually in effect, and the version the updater must
// compare a published manifest against.
struct ActiveQEDatabase {
    bool use_downloaded = false;   // false: the database built into the module
    int  version        = 0;
};

// Decides between the database compiled into the module (embedded_version,
// the db_version it was published as) and one the updater downloaded
// (downloaded_present, at state.installed_db_version).
ActiveQEDatabase active_qe_database(const QEUpdateState& state,
                                    bool downloaded_present,
                                    int embedded_version);

} // namespace nukex

#endif
