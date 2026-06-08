#include "nukex/learning/rating_db.hpp"

#include <sqlite3.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>

namespace nukex::learning {

namespace {

constexpr const char* kSchemaV1 = R"SQL(
CREATE TABLE IF NOT EXISTS runs (
    run_id           BLOB PRIMARY KEY,
    created_at       INTEGER NOT NULL,
    stretch_name     TEXT NOT NULL,
    target_class     INTEGER NOT NULL,
    filter_class     INTEGER NOT NULL,

    stat_median_r REAL, stat_median_g REAL, stat_median_b REAL,
    stat_mad_r    REAL, stat_mad_g    REAL, stat_mad_b    REAL,
    stat_p50_r    REAL, stat_p50_g    REAL, stat_p50_b    REAL,
    stat_p95_r    REAL, stat_p95_g    REAL, stat_p95_b    REAL,
    stat_p99_r    REAL, stat_p99_g    REAL, stat_p99_b    REAL,
    stat_p999_r   REAL, stat_p999_g   REAL, stat_p999_b   REAL,
    stat_skew_r   REAL, stat_skew_g   REAL, stat_skew_b   REAL,
    stat_sat_frac_r REAL, stat_sat_frac_g REAL, stat_sat_frac_b REAL,

    stat_bright_concentration REAL,
    stat_color_rg  REAL, stat_color_bg REAL,
    stat_fwhm_median REAL,
    stat_star_count  INTEGER,

    params_json TEXT NOT NULL,

    rating_brightness INTEGER NOT NULL CHECK (rating_brightness BETWEEN -2 AND 2),
    rating_saturation INTEGER NOT NULL CHECK (rating_saturation BETWEEN -2 AND 2),
    rating_color      INTEGER          CHECK (rating_color      BETWEEN -2 AND 2),
    rating_star_bloat INTEGER NOT NULL CHECK (rating_star_bloat BETWEEN -2 AND 2),
    rating_overall    INTEGER NOT NULL CHECK (rating_overall    BETWEEN 1 AND 5)
);
CREATE INDEX IF NOT EXISTS idx_runs_stretch ON runs(stretch_name);
CREATE INDEX IF NOT EXISTS idx_runs_target  ON runs(target_class);
PRAGMA user_version = 1;
)SQL";

bool apply_pragmas_and_schema(sqlite3* db) {
    char* err = nullptr;
    if (sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }
    if (sqlite3_exec(db, kSchemaV1, nullptr, nullptr, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool integrity_ok(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* txt = sqlite3_column_text(stmt, 0);
        ok = (txt != nullptr) && (std::string(reinterpret_cast<const char*>(txt)) == "ok");
    }
    sqlite3_finalize(stmt);
    return ok;
}

std::string rename_corrupt(const std::string& path) {
    const auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const std::string renamed = path + ".corrupt." + std::to_string(ts);
    std::error_code ec;
    std::filesystem::rename(path, renamed, ec);
    return ec ? std::string{} : renamed;
}

} // namespace

sqlite3* open_rating_db(const std::string& path) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        sqlite3* db = nullptr;
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return nullptr;
        }
        // A garbage-bytes file opens without error but fails on the first
        // real statement (SQLITE_NOTADB). Treat schema-apply failure as
        // corruption on attempt 0 so we rename-and-retry the same way as a
        // post-integrity-check failure.
        const bool schema_ok   = apply_pragmas_and_schema(db);
        const bool integrity   = schema_ok && integrity_ok(db);
        if (schema_ok && integrity) {
            return db;
        }
        sqlite3_close(db);
        if (attempt == 0) {
            if (rename_corrupt(path).empty()) {
                return nullptr;
            }
            continue;
        }
        return nullptr;
    }
    return nullptr;
}

void close_rating_db(sqlite3* db) {
    if (db) sqlite3_close(db);
}

namespace {
int bind_double_or_null(sqlite3_stmt* s, int col, double v) {
    if (std::isnan(v)) return sqlite3_bind_null(s, col);
    return sqlite3_bind_double(s, col, v);
}
} // namespace

bool insert_run(sqlite3* db, const RunRecord& rec) {
    if (!db) return false;
    const char* sql = R"SQL(
INSERT INTO runs(
    run_id, created_at, stretch_name, target_class, filter_class,
    stat_median_r, stat_median_g, stat_median_b,
    stat_mad_r, stat_mad_g, stat_mad_b,
    stat_p50_r, stat_p50_g, stat_p50_b,
    stat_p95_r, stat_p95_g, stat_p95_b,
    stat_p99_r, stat_p99_g, stat_p99_b,
    stat_p999_r, stat_p999_g, stat_p999_b,
    stat_skew_r, stat_skew_g, stat_skew_b,
    stat_sat_frac_r, stat_sat_frac_g, stat_sat_frac_b,
    stat_bright_concentration,
    stat_color_rg, stat_color_bg,
    stat_fwhm_median, stat_star_count,
    params_json,
    rating_brightness, rating_saturation, rating_color,
    rating_star_bloat, rating_overall
) VALUES (?,?,?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?, ?,?, ?,?, ?, ?,?,?,?,?);
)SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    int c = 1;
    sqlite3_bind_blob(stmt, c++, rec.run_id.data(), static_cast<int>(rec.run_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, c++, rec.created_at_unix);
    sqlite3_bind_text (stmt, c++, rec.stretch_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt, c++, rec.target_class);
    sqlite3_bind_int  (stmt, c++, rec.filter_class);
    for (std::size_t i = 0; i < rec.per_channel_stats.size(); ++i) {
        bind_double_or_null(stmt, c++, rec.per_channel_stats[i]);
    }
    bind_double_or_null(stmt, c++, rec.bright_concentration);
    bind_double_or_null(stmt, c++, rec.color_rg);
    bind_double_or_null(stmt, c++, rec.color_bg);
    bind_double_or_null(stmt, c++, rec.fwhm_median);
    sqlite3_bind_int  (stmt, c++, rec.star_count);
    sqlite3_bind_text (stmt, c++, rec.params_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt, c++, rec.rating_brightness);
    sqlite3_bind_int  (stmt, c++, rec.rating_saturation);
    if (rec.rating_color.has_value()) sqlite3_bind_int(stmt, c++, *rec.rating_color);
    else                              sqlite3_bind_null(stmt, c++);
    sqlite3_bind_int  (stmt, c++, rec.rating_star_bloat);
    sqlite3_bind_int  (stmt, c++, rec.rating_overall);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool attach_bootstrap(sqlite3* db, const std::string& bootstrap_path) {
    if (!db) return false;
    if (bootstrap_path.empty()) return true;
    if (!std::filesystem::exists(bootstrap_path)) return true;

    std::string sql = "ATTACH DATABASE '" + bootstrap_path + "' AS bootstrap;";
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

std::vector<RunRecord> select_runs_for_stretch(sqlite3* db,
                                               const std::string& stretch_name) {
    std::vector<RunRecord> out;
    if (!db) return out;

    // Probe whether the bootstrap schema is attached. If yes, UNION ALL
    // across user + bootstrap rows; if no, query user DB only.
    const bool has_bootstrap = [&]() {
        sqlite3_stmt* s = nullptr;
        const int rc = sqlite3_prepare_v2(db,
            "SELECT count(*) FROM bootstrap.sqlite_master WHERE name='runs';",
            -1, &s, nullptr);
        if (rc != SQLITE_OK) { if (s) sqlite3_finalize(s); return false; }
        bool present = false;
        if (sqlite3_step(s) == SQLITE_ROW) {
            present = sqlite3_column_int(s, 0) > 0;
        }
        sqlite3_finalize(s);
        return present;
    }();

    const char* user_only =
        "SELECT * FROM runs WHERE stretch_name = ?;";
    const char* with_bootstrap =
        "SELECT * FROM runs WHERE stretch_name = ?1 "
        "UNION ALL "
        "SELECT * FROM bootstrap.runs WHERE stretch_name = ?1;";

    sqlite3_stmt* stmt = nullptr;
    const char* use_sql = has_bootstrap ? with_bootstrap : user_only;
    if (sqlite3_prepare_v2(db, use_sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, stretch_name.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RunRecord r;
        const void* blob = sqlite3_column_blob(stmt, 0);
        const int   blob_n = sqlite3_column_bytes(stmt, 0);
        if (blob_n == 16 && blob) std::memcpy(r.run_id.data(), blob, 16);
        r.created_at_unix = sqlite3_column_int64(stmt, 1);
        r.stretch_name    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        r.target_class    = sqlite3_column_int(stmt, 3);
        r.filter_class    = sqlite3_column_int(stmt, 4);
        for (int i = 0; i < 24; ++i) {
            const int col = 5 + i;
            if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
                r.per_channel_stats[i] = std::numeric_limits<double>::quiet_NaN();
            } else {
                r.per_channel_stats[i] = sqlite3_column_double(stmt, col);
            }
        }
        r.bright_concentration = sqlite3_column_double(stmt, 29);
        r.color_rg             = sqlite3_column_double(stmt, 30);
        r.color_bg             = sqlite3_column_double(stmt, 31);
        r.fwhm_median          = sqlite3_column_double(stmt, 32);
        r.star_count           = sqlite3_column_int(stmt, 33);
        r.params_json          = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 34));
        r.rating_brightness    = sqlite3_column_int(stmt, 35);
        r.rating_saturation    = sqlite3_column_int(stmt, 36);
        if (sqlite3_column_type(stmt, 37) == SQLITE_NULL) r.rating_color.reset();
        else                                              r.rating_color = sqlite3_column_int(stmt, 37);
        r.rating_star_bloat    = sqlite3_column_int(stmt, 38);
        r.rating_overall       = sqlite3_column_int(stmt, 39);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return out;
}

UserDataPaths resolve_user_data_paths(const std::string& user_data_root,
                                      const std::string& share_root) {
    namespace fs = std::filesystem;
    UserDataPaths p;
    fs::path base = fs::path(user_data_root) / "nukex4";
    std::error_code ec;
    fs::create_directories(base, ec);
    // Best-effort 0700 on POSIX — if this fails (e.g. Windows or the dir
    // already has custom perms), we still ship a working Phase 8 path.
    (void)ec;
    std::error_code perm_ec;
    fs::permissions(base,
                    fs::perms::owner_all,
                    fs::perm_options::replace,
                    perm_ec);
    (void)perm_ec;

    p.user_db              = (base / "phase8_user.sqlite").string();
    p.user_model_json      = (base / "phase8_user_model.json").string();
    p.bootstrap_db         = (fs::path(share_root) / "phase8_bootstrap.sqlite").string();
    p.bootstrap_model_json = (fs::path(share_root) / "phase8_bootstrap_model.json").string();
    return p;
}

} // namespace nukex::learning
