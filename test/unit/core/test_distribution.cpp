#include "catch_amalgamated.hpp"
#include "nukex/core/distribution.hpp"

using namespace nukex;

TEST_CASE("DistributionShape enum values are correct", "[distribution]") {
    REQUIRE(static_cast<uint8_t>(DistributionShape::GAUSSIAN) == 0);
    REQUIRE(static_cast<uint8_t>(DistributionShape::BIMODAL) == 1);
    REQUIRE(static_cast<uint8_t>(DistributionShape::HEAVY_TAILED) == 2);
    REQUIRE(static_cast<uint8_t>(DistributionShape::CONTAMINATED) == 3);
    REQUIRE(static_cast<uint8_t>(DistributionShape::SPIKE_OUTLIER) == 4);
    REQUIRE(static_cast<uint8_t>(DistributionShape::UNIFORM) == 5);
    REQUIRE(static_cast<uint8_t>(DistributionShape::UNKNOWN) == 6);
}

TEST_CASE("StudentTParams stores mu, sigma, nu", "[distribution]") {
    StudentTParams t{};
    t.mu = 0.5f; t.sigma = 0.1f; t.nu = 5.0f;
    REQUIRE(t.mu == Catch::Approx(0.5f));
    REQUIRE(t.sigma == Catch::Approx(0.1f));
    REQUIRE(t.nu == Catch::Approx(5.0f));
}

TEST_CASE("GaussianParams stores mu, sigma, amplitude", "[distribution]") {
    GaussianParams g{};
    g.mu = 0.5f; g.sigma = 0.1f; g.amplitude = 1.0f;
    REQUIRE(g.mu == Catch::Approx(0.5f));
    REQUIRE(g.sigma == Catch::Approx(0.1f));
    REQUIRE(g.amplitude == Catch::Approx(1.0f));
}

TEST_CASE("BimodalParams stores two Gaussian components + mixing ratio", "[distribution]") {
    BimodalParams b{};
    b.comp1 = {0.3f, 0.05f, 0.7f};
    b.comp2 = {0.8f, 0.08f, 0.3f};
    b.mixing_ratio = 0.7f;
    REQUIRE(b.comp1.mu == Catch::Approx(0.3f));
    REQUIRE(b.comp2.mu == Catch::Approx(0.8f));
    REQUIRE(b.mixing_ratio == Catch::Approx(0.7f));
}

TEST_CASE("ContaminationParams stores clean signal + contamination fraction", "[distribution]") {
    ContaminationParams c{};
    c.mu = 0.45f; c.sigma = 0.03f; c.contamination_frac = 0.08f;
    REQUIRE(c.mu == Catch::Approx(0.45f));
    REQUIRE(c.sigma == Catch::Approx(0.03f));
    REQUIRE(c.contamination_frac == Catch::Approx(0.08f));
}

TEST_CASE("ZDistribution: default shape is UNKNOWN", "[distribution]") {
    ZDistribution z{};
    REQUIRE(z.shape == DistributionShape::UNKNOWN);
    REQUIRE(z.used_nonparametric == false);
}

TEST_CASE("ZDistribution: Student-t signal extraction", "[distribution]") {
    ZDistribution z{};
    z.shape = DistributionShape::GAUSSIAN;
    z.params.student_t = {0.42f, 0.03f, 50.0f};
    z.true_signal_estimate = z.params.student_t.mu;
    z.signal_uncertainty = z.params.student_t.sigma;
    z.confidence = 0.95f;
    z.r_squared = 0.97f;
    z.aicc = -150.0f;
    REQUIRE(z.true_signal_estimate == Catch::Approx(0.42f));
    REQUIRE(z.signal_uncertainty == Catch::Approx(0.03f));
    REQUIRE(z.confidence == Catch::Approx(0.95f));
}

TEST_CASE("ZDistribution: heavy-tailed Student-t", "[distribution]") {
    ZDistribution z{};
    z.shape = DistributionShape::HEAVY_TAILED;
    z.params.student_t = {0.40f, 0.05f, 4.0f};
    z.true_signal_estimate = z.params.student_t.mu;
    REQUIRE(z.true_signal_estimate == Catch::Approx(0.40f));
    REQUIRE(z.params.student_t.nu == Catch::Approx(4.0f));
}

TEST_CASE("ZDistribution: contamination model", "[distribution]") {
    ZDistribution z{};
    z.shape = DistributionShape::CONTAMINATED;
    z.params.contamination = {0.42f, 0.02f, 0.05f};
    z.true_signal_estimate = z.params.contamination.mu;
    REQUIRE(z.true_signal_estimate == Catch::Approx(0.42f));
    REQUIRE(z.params.contamination.contamination_frac == Catch::Approx(0.05f));
}

TEST_CASE("ZDistribution: KDE fallback fields", "[distribution]") {
    ZDistribution z{};
    z.shape = DistributionShape::UNIFORM;
    z.used_nonparametric = true;
    z.kde_mode = 0.35f;
    z.kde_bandwidth = 0.02f;
    REQUIRE(z.used_nonparametric == true);
    REQUIRE(z.kde_mode == Catch::Approx(0.35f));
    REQUIRE(z.kde_bandwidth == Catch::Approx(0.02f));
}

TEST_CASE("distribution_shape_name returns correct strings", "[distribution]") {
    REQUIRE(distribution_shape_name(DistributionShape::GAUSSIAN) == std::string("GAUSSIAN"));
    REQUIRE(distribution_shape_name(DistributionShape::BIMODAL) == std::string("BIMODAL"));
    REQUIRE(distribution_shape_name(DistributionShape::HEAVY_TAILED) == std::string("HEAVY_TAILED"));
    REQUIRE(distribution_shape_name(DistributionShape::CONTAMINATED) == std::string("CONTAMINATED"));
    REQUIRE(distribution_shape_name(DistributionShape::SPIKE_OUTLIER) == std::string("SPIKE_OUTLIER"));
    REQUIRE(distribution_shape_name(DistributionShape::UNIFORM) == std::string("UNIFORM"));
    REQUIRE(distribution_shape_name(DistributionShape::UNKNOWN) == std::string("UNKNOWN"));
}
