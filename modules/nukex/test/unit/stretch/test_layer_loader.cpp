#include "catch_amalgamated.hpp"
#include "nukex/stretch/layer_loader.hpp"

#include <filesystem>
#include <fstream>

using namespace nukex;
namespace fs = std::filesystem;

namespace {
fs::path unique_json(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("nukex_ll_" + tag + ".json");
    fs::remove(p);
    return p;
}

void write_simple_model(const fs::path& path, const std::string& stretch) {
    ParamCoefficients c;
    c.feature_mean.assign(29, 0.0);
    c.feature_std .assign(29, 1.0);
    c.coefficients.assign(29, 0.0);
    c.intercept = 3.0;
    c.lambda = 1.0;
    c.n_train_rows = 8;
    c.cv_r_squared = 0.3;
    ParamModel m(stretch);
    m.add_param("log_D", c);
    ParamModelMap map;
    map.emplace(stretch, std::move(m));
    REQUIRE(write_param_models_json(map, path.string()));
}
} // namespace

TEST_CASE("LayerLoader: no files -> active layer is None",
          "[stretch][layer_loader]") {
    LayerLoader L("/tmp/nx_ll_no_boot.json", "/tmp/nx_ll_no_user.json");
    auto a = L.active_for_stretch("VeraLux");
    REQUIRE(a.layer == ActiveLayer::None);
    REQUIRE(a.model == nullptr);
}

TEST_CASE("LayerLoader: bootstrap present, user absent -> Layer 2",
          "[stretch][layer_loader]") {
    auto boot = unique_json("boot_only");
    write_simple_model(boot, "VeraLux");

    LayerLoader L(boot.string(), "/tmp/nx_ll_missing_user.json");
    auto a = L.active_for_stretch("VeraLux");
    REQUIRE(a.layer == ActiveLayer::CommunityBootstrap);
    REQUIRE(a.model != nullptr);
    REQUIRE(a.description.find("Layer 2") != std::string::npos);
    fs::remove(boot);
}

TEST_CASE("LayerLoader: user present wins over bootstrap -> Layer 3",
          "[stretch][layer_loader]") {
    auto boot = unique_json("both_boot");
    auto user = unique_json("both_user");
    write_simple_model(boot, "VeraLux");
    write_simple_model(user, "VeraLux");

    LayerLoader L(boot.string(), user.string());
    auto a = L.active_for_stretch("VeraLux");
    REQUIRE(a.layer == ActiveLayer::UserLearned);
    REQUIRE(a.description.find("Layer 3") != std::string::npos);
    fs::remove(boot); fs::remove(user);
}

TEST_CASE("LayerLoader: user present but no entry for this stretch falls back",
          "[stretch][layer_loader]") {
    auto boot = unique_json("fb_boot");
    auto user = unique_json("fb_user");
    write_simple_model(boot, "VeraLux");
    write_simple_model(user, "GHS");   // only GHS in user, not VeraLux

    LayerLoader L(boot.string(), user.string());
    auto v = L.active_for_stretch("VeraLux");
    REQUIRE(v.layer == ActiveLayer::CommunityBootstrap);
    auto g = L.active_for_stretch("GHS");
    REQUIRE(g.layer == ActiveLayer::UserLearned);
    fs::remove(boot); fs::remove(user);
}

TEST_CASE("LayerLoader: malformed user file falls back to bootstrap",
          "[stretch][layer_loader]") {
    auto boot = unique_json("mal_boot");
    auto user = unique_json("mal_user");
    write_simple_model(boot, "VeraLux");
    {
        std::ofstream f(user);
        f << "{ not valid ]";
    }

    LayerLoader L(boot.string(), user.string());
    auto a = L.active_for_stretch("VeraLux");
    REQUIRE(a.layer == ActiveLayer::CommunityBootstrap);
    fs::remove(boot); fs::remove(user);
}

TEST_CASE("LayerLoader: reload picks up newly-written user file",
          "[stretch][layer_loader]") {
    auto user = unique_json("rel_user");
    LayerLoader L("", user.string());
    REQUIRE(L.active_for_stretch("VeraLux").layer == ActiveLayer::None);

    write_simple_model(user, "VeraLux");
    L.reload();
    REQUIRE(L.active_for_stretch("VeraLux").layer == ActiveLayer::UserLearned);
    fs::remove(user);
}
