#include "catch_amalgamated.hpp"
#include "stretch_factory.hpp"
#include "fits_metadata.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/ghs_stretch.hpp"
#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "nukex/stretch/log_stretch.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"
#include "nukex/stretch/layer_loader.hpp"
#include "nukex/stretch/image_stats.hpp"

using namespace nukex;

TEST_CASE("build_primary: Auto fills log_line", "[module][stretch_factory]") {
    FITSMetadata meta;
    meta.filter = "L";
    std::string log;
    auto op = build_primary(PrimaryStretch::Auto, meta, log);
    REQUIRE(op != nullptr);
    REQUIRE(!log.empty());
}

TEST_CASE("build_primary: explicit values return empty log_line", "[module][stretch_factory]") {
    FITSMetadata meta;
    std::string log = "prior";
    auto op = build_primary(PrimaryStretch::GHS, meta, log);
    REQUIRE(op != nullptr);
    REQUIRE(log.empty());
    REQUIRE(dynamic_cast<GHSStretch*>(op.get()) != nullptr);
}

TEST_CASE("build_primary: all named enums produce the correct op type", "[module][stretch_factory]") {
    FITSMetadata meta;
    std::string log;
    REQUIRE(dynamic_cast<VeraLuxStretch*>(build_primary(PrimaryStretch::VeraLux, meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<GHSStretch*>    (build_primary(PrimaryStretch::GHS,     meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<MTFStretch*>    (build_primary(PrimaryStretch::MTF,     meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<ArcSinhStretch*>(build_primary(PrimaryStretch::ArcSinh, meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<LogStretch*>    (build_primary(PrimaryStretch::Log,     meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<LuptonStretch*> (build_primary(PrimaryStretch::Lupton,  meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<CLAHEStretch*>  (build_primary(PrimaryStretch::CLAHE,   meta, log).get()) != nullptr);
}

TEST_CASE("build_finishing: None returns nullptr", "[module][stretch_factory]") {
    REQUIRE(build_finishing(FinishingStretch::None) == nullptr);
}

TEST_CASE("build_primary Auto: no Phase 8 context leaves op at factory defaults",
          "[module][stretch_factory][phase8]") {
    FITSMetadata meta; meta.filter = "L";
    std::string log;
    auto op = build_primary(PrimaryStretch::Auto, meta, log, nullptr);
    REQUIRE(op != nullptr);
    // Log does not mention Phase 8 when no context is passed
    REQUIRE(log.find("Phase 8") == std::string::npos);
}

TEST_CASE("build_primary Auto: empty LayerLoader falls through to factory",
          "[module][stretch_factory][phase8]") {
    LayerLoader empty_loader("", "");
    ImageStats stats;
    Phase8Context ctx{&empty_loader, &stats};

    FITSMetadata meta; meta.filter = "L";
    std::string log;
    auto op = build_primary(PrimaryStretch::Auto, meta, log, &ctx);
    REQUIRE(op != nullptr);
    REQUIRE(log.find("Layer 1") != std::string::npos);
    // VeraLux factory default log_D is 2.0 -- unchanged
    auto* v = dynamic_cast<VeraLuxStretch*>(op.get());
    if (v) REQUIRE(v->log_D == 2.0f);
}

TEST_CASE("build_primary: explicit enum ignores Phase 8 context",
          "[module][stretch_factory][phase8]") {
    LayerLoader empty_loader("", "");
    ImageStats stats;
    Phase8Context ctx{&empty_loader, &stats};

    FITSMetadata meta;
    std::string log;
    auto op = build_primary(PrimaryStretch::GHS, meta, log, &ctx);
    REQUIRE(op != nullptr);
    REQUIRE(log.empty());  // explicit path never logs Phase 8 metadata
    REQUIRE(dynamic_cast<GHSStretch*>(op.get()) != nullptr);
}
