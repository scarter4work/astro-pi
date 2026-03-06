#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/QualityWeights.h"
#include <vector>

TEST_CASE("Quality weights computation", "[weights]") {
    std::vector<nukex::SubMetadata> metas(3);
    metas[0].fwhm = 2.0; metas[0].eccentricity = 0.1;
    metas[1].fwhm = 3.0; metas[1].eccentricity = 0.2;
    metas[2].fwhm = 5.0; metas[2].eccentricity = 0.5;

    nukex::WeightConfig cfg;
    cfg.fwhmWeight = 1.0;
    cfg.eccentricityWeight = 1.0;
    cfg.skyBackgroundWeight = 0.0;  // disable
    cfg.hfrWeight = 0.0;            // disable
    cfg.altitudeWeight = 0.0;       // disable

    auto weights = nukex::ComputeQualityWeights(metas, cfg);

    SECTION("weights are normalized to sum to 1") {
        double sum = 0;
        for (auto w : weights) sum += w;
        REQUIRE(sum == Catch::Approx(1.0));
    }

    SECTION("better subs get higher weights") {
        REQUIRE(weights[0] > weights[1]);
        REQUIRE(weights[1] > weights[2]);
    }

    SECTION("all-equal metadata gives equal weights") {
        for (auto& m : metas) { m.fwhm = 2.0; m.eccentricity = 0.1; }
        auto eq = nukex::ComputeQualityWeights(metas, cfg);
        REQUIRE(eq[0] == Catch::Approx(eq[1]).margin(1e-10));
        REQUIRE(eq[1] == Catch::Approx(eq[2]).margin(1e-10));
    }

    SECTION("single sub gets weight 1.0") {
        std::vector<nukex::SubMetadata> single(1);
        single[0].fwhm = 3.0;
        auto w = nukex::ComputeQualityWeights(single, cfg);
        REQUIRE(w[0] == Catch::Approx(1.0));
    }
}

TEST_CASE("Quality weights edge cases", "[weights]") {
    SECTION("empty input returns empty vector") {
        std::vector<nukex::SubMetadata> empty;
        nukex::WeightConfig cfg;
        auto w = nukex::ComputeQualityWeights(empty, cfg);
        REQUIRE(w.empty());
    }

    SECTION("all config weights zero gives equal weights") {
        std::vector<nukex::SubMetadata> metas(3);
        metas[0].fwhm = 2.0;
        metas[1].fwhm = 3.0;
        metas[2].fwhm = 5.0;

        nukex::WeightConfig cfg;
        cfg.fwhmWeight = 0.0;
        cfg.eccentricityWeight = 0.0;
        cfg.skyBackgroundWeight = 0.0;
        cfg.hfrWeight = 0.0;
        cfg.altitudeWeight = 0.0;

        auto w = nukex::ComputeQualityWeights(metas, cfg);
        REQUIRE(w[0] == Catch::Approx(1.0 / 3.0));
        REQUIRE(w[1] == Catch::Approx(1.0 / 3.0));
        REQUIRE(w[2] == Catch::Approx(1.0 / 3.0));
    }

    SECTION("altitude scoring uses sin for airmass proxy") {
        std::vector<nukex::SubMetadata> metas(2);
        metas[0].altitude = 90.0;  // zenith — best
        metas[1].altitude = 30.0;  // 30 degrees — worse

        nukex::WeightConfig cfg;
        cfg.fwhmWeight = 0.0;
        cfg.eccentricityWeight = 0.0;
        cfg.skyBackgroundWeight = 0.0;
        cfg.hfrWeight = 0.0;
        cfg.altitudeWeight = 1.0;

        auto w = nukex::ComputeQualityWeights(metas, cfg);
        REQUIRE(w[0] > w[1]);

        double sum = w[0] + w[1];
        REQUIRE(sum == Catch::Approx(1.0));
    }

    SECTION("missing metrics (zero values) are skipped gracefully") {
        std::vector<nukex::SubMetadata> metas(2);
        metas[0].fwhm = 2.0;  metas[0].eccentricity = 0.0;  // eccentricity missing
        metas[1].fwhm = 2.0;  metas[1].eccentricity = 0.3;

        nukex::WeightConfig cfg;
        cfg.fwhmWeight = 1.0;
        cfg.eccentricityWeight = 1.0;
        cfg.skyBackgroundWeight = 0.0;
        cfg.hfrWeight = 0.0;
        cfg.altitudeWeight = 0.0;

        auto w = nukex::ComputeQualityWeights(metas, cfg);
        double sum = w[0] + w[1];
        REQUIRE(sum == Catch::Approx(1.0));
    }
}
