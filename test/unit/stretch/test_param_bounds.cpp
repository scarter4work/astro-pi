#include "catch_amalgamated.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/ghs_stretch.hpp"
#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "nukex/stretch/log_stretch.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"

using namespace nukex;

TEST_CASE("VeraLux: param_bounds covers log_D / protect_b / convergence_power",
          "[stretch][param_bounds]") {
    VeraLuxStretch op;
    auto b = op.param_bounds();
    REQUIRE(b.size() == 3);
    REQUIRE(b.at("log_D").first  == 0.0f);
    REQUIRE(b.at("log_D").second == 7.0f);
    REQUIRE(b.at("protect_b").first == 0.1f);
    REQUIRE(b.at("convergence_power").second == 10.0f);
}

TEST_CASE("VeraLux: set_param + get_param round-trip", "[stretch][param_bounds]") {
    VeraLuxStretch op;
    REQUIRE(op.set_param("log_D", 3.5f));
    REQUIRE(op.get_param("log_D").value() == 3.5f);
    REQUIRE_FALSE(op.set_param("nonsense_param", 0.0f));
    REQUIRE_FALSE(op.get_param("nonsense_param").has_value());
}

TEST_CASE("All primary stretches expose at least one bounded param",
          "[stretch][param_bounds]") {
    REQUIRE_FALSE(VeraLuxStretch().param_bounds().empty());
    REQUIRE_FALSE(GHSStretch().param_bounds().empty());
    REQUIRE_FALSE(MTFStretch().param_bounds().empty());
    REQUIRE_FALSE(ArcSinhStretch().param_bounds().empty());
    REQUIRE_FALSE(LogStretch().param_bounds().empty());
    REQUIRE_FALSE(LuptonStretch().param_bounds().empty());
    REQUIRE_FALSE(CLAHEStretch().param_bounds().empty());
}
