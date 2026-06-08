#include "catch_amalgamated.hpp"
#include "nukex/core/welford.hpp"
#include <cmath>
#include <vector>
#include <numeric>

using nukex::WelfordAccumulator;

TEST_CASE("WelfordAccumulator: default state is zero/empty", "[welford]") {
    WelfordAccumulator w;
    REQUIRE(w.count() == 0);
    REQUIRE(w.mean == 0.0f);
    REQUIRE(w.variance() == 0.0f);
    REQUIRE(w.std_dev() == 0.0f);
    REQUIRE(w.min_val == FLT_MAX);
    REQUIRE(w.max_val == -FLT_MAX);
}

TEST_CASE("WelfordAccumulator: single value", "[welford]") {
    WelfordAccumulator w;
    w.update(42.0f);
    REQUIRE(w.count() == 1);
    REQUIRE(w.mean == Catch::Approx(42.0f));
    REQUIRE(w.variance() == 0.0f);
    REQUIRE(w.std_dev() == 0.0f);
    REQUIRE(w.min_val == Catch::Approx(42.0f));
    REQUIRE(w.max_val == Catch::Approx(42.0f));
}

TEST_CASE("WelfordAccumulator: two values — known answer", "[welford]") {
    WelfordAccumulator w;
    w.update(2.0f);
    w.update(4.0f);
    REQUIRE(w.count() == 2);
    REQUIRE(w.mean == Catch::Approx(3.0f));
    REQUIRE(w.variance() == Catch::Approx(2.0f));
    REQUIRE(w.std_dev() == Catch::Approx(std::sqrt(2.0f)));
    REQUIRE(w.min_val == Catch::Approx(2.0f));
    REQUIRE(w.max_val == Catch::Approx(4.0f));
}

TEST_CASE("WelfordAccumulator: known dataset {1,2,3,4,5}", "[welford]") {
    WelfordAccumulator w;
    for (float v : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}) {
        w.update(v);
    }
    REQUIRE(w.count() == 5);
    REQUIRE(w.mean == Catch::Approx(3.0f));
    REQUIRE(w.variance() == Catch::Approx(2.5f));
    REQUIRE(w.std_dev() == Catch::Approx(std::sqrt(2.5f)));
    REQUIRE(w.min_val == Catch::Approx(1.0f));
    REQUIRE(w.max_val == Catch::Approx(5.0f));
}

TEST_CASE("WelfordAccumulator: identical values → zero variance", "[welford]") {
    WelfordAccumulator w;
    for (int i = 0; i < 100; i++) {
        w.update(7.5f);
    }
    REQUIRE(w.count() == 100);
    REQUIRE(w.mean == Catch::Approx(7.5f));
    REQUIRE(w.variance() == Catch::Approx(0.0f).margin(1e-10f));
    REQUIRE(w.min_val == Catch::Approx(7.5f));
    REQUIRE(w.max_val == Catch::Approx(7.5f));
}

TEST_CASE("WelfordAccumulator: numerical stability — large offset", "[welford]") {
    WelfordAccumulator w;
    const float offset = 1.0e6f;
    std::vector<float> values = {
        offset + 1.0f, offset + 2.0f, offset + 3.0f,
        offset + 4.0f, offset + 5.0f
    };
    for (float v : values) {
        w.update(v);
    }
    REQUIRE(w.count() == 5);
    REQUIRE(w.mean == Catch::Approx(offset + 3.0f).epsilon(1e-6f));
    REQUIRE(w.variance() == Catch::Approx(2.5f).epsilon(1e-4f));
}

TEST_CASE("WelfordAccumulator: negative values", "[welford]") {
    WelfordAccumulator w;
    w.update(-3.0f);
    w.update(-1.0f);
    w.update(1.0f);
    w.update(3.0f);
    REQUIRE(w.count() == 4);
    REQUIRE(w.mean == Catch::Approx(0.0f));
    REQUIRE(w.variance() == Catch::Approx(20.0f / 3.0f).epsilon(1e-5f));
    REQUIRE(w.min_val == Catch::Approx(-3.0f));
    REQUIRE(w.max_val == Catch::Approx(3.0f));
}

TEST_CASE("WelfordAccumulator: large N (1000 samples)", "[welford]") {
    WelfordAccumulator w;
    for (int i = 0; i < 1000; i++) {
        w.update(static_cast<float>(i));
    }
    REQUIRE(w.count() == 1000);
    REQUIRE(w.mean == Catch::Approx(499.5f).epsilon(1e-5f));
    // Sample variance of {0,1,...,999}: (N+1)*(N-1)/12 * N/(N-1) = N*(N+1)/12
    // = 1000*1001/12 = 83416.667
    REQUIRE(w.variance() == Catch::Approx(83416.667f).epsilon(1e-3f));
    REQUIRE(w.min_val == Catch::Approx(0.0f));
    REQUIRE(w.max_val == Catch::Approx(999.0f));
}

TEST_CASE("WelfordAccumulator: reset clears state", "[welford]") {
    WelfordAccumulator w;
    w.update(10.0f);
    w.update(20.0f);
    w.reset();
    REQUIRE(w.count() == 0);
    REQUIRE(w.mean == 0.0f);
    REQUIRE(w.variance() == 0.0f);
    REQUIRE(w.min_val == FLT_MAX);
    REQUIRE(w.max_val == -FLT_MAX);

    // Verify re-use after reset gives correct results (same as fresh accumulator)
    w.update(5.0f);
    w.update(15.0f);
    REQUIRE(w.mean == Catch::Approx(10.0f));
    REQUIRE(w.variance() == Catch::Approx(50.0f));
    REQUIRE(w.count() == 2);
}
