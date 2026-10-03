#include "catch_amalgamated.hpp"
#include "nukex/calibration/qe_update_state.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace nukex;
namespace fs = std::filesystem;

static std::string state_path(const char* tag) {
    fs::path p = fs::temp_directory_path() /
                 ("nukex_qe_state_" + std::string(tag) + "_" +
                  std::to_string(static_cast<long>(::getpid())));
    fs::remove_all(p);
    fs::create_directories(p);
    return (p / "qe_update_state.json").string();
}

TEST_CASE("update state: round-trips through JSON", "[qe_update_state]") {
    QEUpdateState s;
    s.enabled              = false;
    s.interval_days        = 14;
    s.last_check_unix      = 1757000000;
    s.last_result          = "up to date";
    s.installed_db_version = 14;
    s.declined_version     = 15;

    const std::string p = state_path("round");
    REQUIRE(save_update_state(p, s));

    const QEUpdateState back = load_update_state(p);
    REQUIRE(back.enabled              == false);
    REQUIRE(back.interval_days        == 14);
    REQUIRE(back.last_check_unix      == 1757000000);
    REQUIRE(back.last_result          == "up to date");
    REQUIRE(back.installed_db_version == 14);
    REQUIRE(back.declined_version     == 15);
}

TEST_CASE("update state: a missing file yields usable defaults", "[qe_update_state]") {
    const QEUpdateState s = load_update_state(state_path("missing") + ".nope");
    REQUIRE(s.enabled);
    REQUIRE(s.interval_days == 7);
    REQUIRE(s.last_check_unix == 0);
    REQUIRE(s.installed_db_version == 0);
}

TEST_CASE("update state: a corrupt file yields defaults rather than throwing", "[qe_update_state]") {
    // Losing this file costs one extra check and nothing else, so failing
    // loudly here would be noise. Contrast the QE database itself, where a
    // parse failure is reported.
    const std::string p = state_path("corrupt");
    { std::ofstream out(p); out << "{ this is not json"; }

    const QEUpdateState s = load_update_state(p);
    REQUIRE(s.enabled);
    REQUIRE(s.interval_days == 7);
}

TEST_CASE("update state: the interval gates checking", "[qe_update_state]") {
    QEUpdateState s;
    s.enabled         = true;
    s.interval_days   = 7;
    s.last_check_unix = 1000000;

    REQUIRE_FALSE(should_check_now(s, 1000000 + 6 * 86400));
    REQUIRE      (should_check_now(s, 1000000 + 7 * 86400));
}

TEST_CASE("update state: disabling suppresses checking regardless of age", "[qe_update_state]") {
    QEUpdateState s;
    s.enabled         = false;
    s.interval_days   = 7;
    s.last_check_unix = 0;
    REQUIRE_FALSE(should_check_now(s, 99999999));
}

TEST_CASE("update state: a clock that moved backwards does not wedge checking", "[qe_update_state]") {
    // A stale future timestamp -- from a clock correction or a copied config
    // -- must not disable updates until the clock catches up.
    QEUpdateState s;
    s.enabled         = true;
    s.interval_days   = 7;
    s.last_check_unix = 2000000000;
    REQUIRE(should_check_now(s, 1000000000));
}
