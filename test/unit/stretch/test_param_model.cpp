#include "catch_amalgamated.hpp"
#include "nukex/stretch/param_model.hpp"
#include "nukex/stretch/veralux_stretch.hpp"

#include <limits>

using namespace nukex;

namespace {
ImageStats trivial_stats() {
    ImageStats s;
    s.median.fill(0.5); s.mad.fill(0.1);
    s.p50.fill(0.5);    s.p95.fill(0.9); s.p99.fill(0.95); s.p999.fill(0.99);
    s.skew.fill(0.0);   s.sat_frac.fill(0.0);
    s.bright_concentration = 0.2;
    s.color_rg = 1.0; s.color_bg = 1.0;
    s.fwhm_median = 3.0; s.star_count = 100;
    return s;
}
} // namespace

TEST_CASE("ParamModel: apply with zero-coefficient predicts intercept",
          "[stretch][param_model]") {
    ParamModel m("VeraLux");
    ParamCoefficients c;
    c.feature_mean.assign(29, 0.5);
    c.feature_std .assign(29, 1.0);
    c.coefficients.assign(29, 0.0);
    c.intercept = 3.5;
    m.add_param("log_D", c);

    VeraLuxStretch op;
    REQUIRE(m.predict_and_apply(trivial_stats(), op));
    REQUIRE(op.get_param("log_D").value() == Catch::Approx(3.5f));
}

TEST_CASE("ParamModel: clamps prediction to param_bounds",
          "[stretch][param_model]") {
    ParamModel m("VeraLux");
    ParamCoefficients c;
    c.feature_mean.assign(29, 0.0);
    c.feature_std .assign(29, 1.0);
    c.coefficients.assign(29, 0.0);
    c.intercept = 99.0;   // above VeraLux log_D max of 7.0
    m.add_param("log_D", c);

    VeraLuxStretch op;
    REQUIRE(m.predict_and_apply(trivial_stats(), op));
    REQUIRE(op.get_param("log_D").value() == Catch::Approx(7.0f));
}

TEST_CASE("ParamModel: drops unknown param names",
          "[stretch][param_model]") {
    ParamModel m("VeraLux");
    ParamCoefficients c;
    c.feature_mean.assign(29, 0.0);
    c.feature_std .assign(29, 1.0);
    c.coefficients.assign(29, 0.0);
    c.intercept = 1.0;
    m.add_param("bogus_param_name", c);

    VeraLuxStretch op;
    // Nothing applied because VeraLux has no "bogus_param_name". Not an error.
    REQUIRE_FALSE(m.predict_and_apply(trivial_stats(), op));
    REQUIRE(op.log_D == 2.0f);  // default unchanged
}

TEST_CASE("ParamModel: NaN stats row falls through",
          "[stretch][param_model]") {
    ParamModel m("VeraLux");
    ParamCoefficients c;
    c.feature_mean.assign(29, 0.0);
    c.feature_std .assign(29, 1.0);
    c.coefficients.assign(29, 0.5);
    c.intercept = 1.0;
    m.add_param("log_D", c);

    ImageStats s = trivial_stats();
    s.median[0] = std::numeric_limits<double>::quiet_NaN();

    VeraLuxStretch op;
    // Non-finite input -> model refuses to predict -> false. Op unchanged.
    REQUIRE_FALSE(m.predict_and_apply(s, op));
    REQUIRE(op.log_D == 2.0f);
}

#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

TEST_CASE("ParamModelMap round-trip through JSON", "[stretch][param_model][json]") {
    const auto path = fs::temp_directory_path() / "nukex_param_model_rt.json";
    fs::remove(path);

    ParamCoefficients c;
    c.feature_mean = std::vector<double>(29, 0.5);
    c.feature_std  = std::vector<double>(29, 1.0);
    c.coefficients = std::vector<double>(29, 0.0);
    c.coefficients[0] = 1.5;
    c.intercept = 2.0;
    c.lambda = 1.0;
    c.n_train_rows = 42;
    c.cv_r_squared = 0.45;

    ParamModel veralux("VeraLux");
    veralux.add_param("log_D", c);

    ParamModelMap in;
    in.emplace("VeraLux", std::move(veralux));

    REQUIRE(write_param_models_json(in, path.string()));

    ParamModelMap out;
    REQUIRE(read_param_models_json(path.string(), out));
    REQUIRE(out.count("VeraLux") == 1);
    const auto& loaded = out.at("VeraLux");
    REQUIRE(loaded.has_param("log_D"));
    const auto& lc = loaded.per_param().at("log_D");
    REQUIRE(lc.intercept == Catch::Approx(2.0));
    REQUIRE(lc.n_train_rows == 42);
    REQUIRE(lc.coefficients[0] == Catch::Approx(1.5));
    fs::remove(path);
}

TEST_CASE("read_param_models_json: missing file returns false",
          "[stretch][param_model][json]") {
    ParamModelMap out;
    REQUIRE_FALSE(read_param_models_json("/tmp/does_not_exist_12345.json", out));
    REQUIRE(out.empty());
}

TEST_CASE("read_param_models_json: malformed file returns false",
          "[stretch][param_model][json]") {
    const auto path = fs::temp_directory_path() / "nukex_malformed_model.json";
    {
        std::ofstream f(path);
        f << "{ not valid json";
    }
    ParamModelMap out;
    REQUIRE_FALSE(read_param_models_json(path.string(), out));
    fs::remove(path);
}
