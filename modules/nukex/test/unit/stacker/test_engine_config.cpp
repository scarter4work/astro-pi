#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"

using namespace nukex;

TEST_CASE("StackingEngine::Config has QE paths with sensible defaults", "[engine_config]") {
    StackingEngine::Config c;
    REQUIRE(c.cache_dir == "/tmp");
    REQUIRE(c.qe_database_path.find("qe_database.json") != std::string::npos);
    REQUIRE(c.qe_override_path.empty()); // optional, no default
}

TEST_CASE("StackingEngine::Config QE paths are settable", "[engine_config]") {
    StackingEngine::Config c;
    c.qe_database_path = "/tmp/custom_db.json";
    c.qe_override_path = "/tmp/custom_override.json";
    REQUIRE(c.qe_database_path == "/tmp/custom_db.json");
    REQUIRE(c.qe_override_path == "/tmp/custom_override.json");
}
