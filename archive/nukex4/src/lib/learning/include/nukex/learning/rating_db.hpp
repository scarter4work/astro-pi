#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace nukex::learning {

// Per-row record that is inserted / queried from the runs table.
// Channel-specific stats carry NaN when the filter class doesn't populate
// that channel (e.g. mono has R channel; G/B carry NaN).
struct RunRecord {
    // Identifiers
    std::array<std::uint8_t, 16> run_id{};
    std::int64_t                 created_at_unix = 0;
    std::string                  stretch_name;
    int                          target_class    = 0;
    int                          filter_class    = 0;

    // 24 per-channel stats in RGB order (median, MAD, p50, p95, p99, p999, skew, sat_frac)
    std::array<double, 24>       per_channel_stats{};

    // 5 global stats: bright_concentration, color_rg, color_bg, fwhm_median, star_count
    double bright_concentration = 0.0;
    double color_rg             = 0.0;
    double color_bg             = 0.0;
    double fwhm_median          = 0.0;
    int    star_count           = 0;

    // Applied params as JSON text
    std::string params_json;

    // Ratings (-2..2 on signed axes; 1..5 on overall)
    int                  rating_brightness = 0;
    int                  rating_saturation = 0;
    std::optional<int>   rating_color;          // NULL for mono/narrowband
    int                  rating_star_bloat = 0;
    int                  rating_overall    = 0;
};

// Opens (or creates) a SQLite DB at `path`. Applies schema v1 if the DB is
// empty. Enables WAL mode. Runs PRAGMA integrity_check; on failure renames
// the DB to `<path>.corrupt.<timestamp>` and returns a fresh DB.
//
// Returns nullptr on unrecoverable failure (e.g. dir not writable).
sqlite3* open_rating_db(const std::string& path);

// Optional: attach the read-only bootstrap DB under alias "bootstrap".
// No-op when bootstrap_path is empty or the file does not exist.
// Used by train_model to union user + bootstrap rows at fit time.
bool attach_bootstrap(sqlite3* db, const std::string& bootstrap_path);

// Insert a single row (runs table). Wrap in a transaction; return false on
// any error. Row is either fully written or not written at all.
bool insert_run(sqlite3* db, const RunRecord& rec);

// Select all rows for a given stretch_name from user DB + attached bootstrap.
std::vector<RunRecord> select_runs_for_stretch(sqlite3* db, const std::string& stretch_name);

void close_rating_db(sqlite3* db);

// ── Phase 8 file-path resolution ─────────────────────────────────────────
// Canonical locations for the Phase 8 rating DB, the user-trained model
// coefficients, and the (optional) community bootstrap files that ship with
// the module. Populated once in NukeXInstance::ExecuteGlobal so the Layer 3
// → Layer 2 → Layer 1 fallback chain can find its inputs.
struct UserDataPaths {
    std::string user_db;              // <user-data>/nukex4/phase8_user.sqlite
    std::string user_model_json;      // <user-data>/nukex4/phase8_user_model.json
    std::string bootstrap_db;         // <module-install>/share/phase8_bootstrap.sqlite
    std::string bootstrap_model_json; // <module-install>/share/phase8_bootstrap_model.json
};

// Resolves Phase 8 file paths using user_data_root (typically PCL
// File::ApplicationData()) and share_root (module install dir + "/share").
// Ensures user_data_root/nukex4/ exists (created 0700 on POSIX when a new
// directory). Safe to call repeatedly.
UserDataPaths resolve_user_data_paths(const std::string& user_data_root,
                                      const std::string& share_root);

} // namespace nukex::learning
