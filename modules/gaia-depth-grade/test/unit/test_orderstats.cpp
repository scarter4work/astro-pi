// Task 4: numpy-exact percentile / median / MAD / interp / nanmedian against the Task 1 goldens
// (numpy 2.5.0) and hand-computed answers. Exact equality throughout.
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/Numerics.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using gdg::test::count_unequal;
using gdg::test::golden;

namespace {
const double kNaN = std::numeric_limits<double>::quiet_NaN();
}

TEST_CASE("percentile_matches_numpy", "[orderstats][golden]") {
    // q in {0, 0.1, 5, 20, 50, 60, 95, 99, 99.5, 99.9, 100} over 1000 values rounded to 0.1 (ties)
    // and over 1, 2 and 5 uniform values.
    const auto qs = golden("pct_q").data;
    REQUIRE(qs.size() == 11);
    for (const std::string tag : {"", "_small1", "_small2", "_small5"}) {
        const auto in = golden(tag.empty() ? "pct_input" : "pct" + tag + "_input").data;
        const auto want = golden(tag.empty() ? "percentile" : "pct" + tag).data;
        INFO("input pct" << tag << " (n=" << in.size() << ")");
        const auto got = gdg::percentiles(in, qs);
        CHECK(count_unequal(got, want) == 0);
        for (size_t k = 0; k < qs.size(); ++k) {
            INFO("q=" << qs[k]);
            CHECK(gdg::percentile(in, qs[k]) == want[k]);
        }
    }
}

TEST_CASE("percentile_hand_computed", "[orderstats]") {
    const std::vector<double> v{4.0, 1.0, 3.0, 2.0};
    CHECK(gdg::percentile(v, 0.0) == 1.0);
    CHECK(gdg::percentile(v, 25.0) == 1.75);   // vi 0.75: 2 - 1*(1-0.75)
    CHECK(gdg::percentile(v, 50.0) == 2.5);
    CHECK(gdg::percentile(v, 75.0) == 3.25);   // vi 2.25: 3 + 1*0.25
    CHECK(gdg::percentile(v, 100.0) == 4.0);
    // numpy 2.5 on [0.1, 0.7, 0.3]: q 30 -> 0.3 - 0.2*0.4 (g >= 0.5 branch), q 80, q 99.9.
    const std::vector<double> w{0.1, 0.7, 0.3};
    CHECK(gdg::percentile(w, 30.0) == 0x1.c28f5c28f5c28p-3);
    CHECK(gdg::percentile(w, 80.0) == 0x1.147ae147ae148p-1);
    CHECK(gdg::percentile(w, 99.9) == 0x1.65fd8adab9f56p-1);
    // Inputs where numpy's g >= 0.5 branch (b - d*(1-g)) differs from a + d*g in the last ulp
    // (a + d*g would give ...ef2p-1 and ...5a2p-1).
    CHECK(gdg::percentile({0.3, 0.87, 0.01}, 91.1) == 0x1.897e132b55ef1p-1);
    CHECK(gdg::percentile({0.98, 0.52, 0.52}, 94.8) == 0x1.dd441355475a3p-1);
    // Input order never matters (v is taken by value and partially selected).
    CHECK(gdg::percentiles(w, {30.0, 80.0}) == std::vector<double>{0x1.c28f5c28f5c28p-3, 0x1.147ae147ae148p-1});
}

TEST_CASE("percentile_rejects_bad_input", "[orderstats]") {
    CHECK_THROWS_AS(gdg::percentile({}, 50.0), std::invalid_argument);
    CHECK_THROWS_AS(gdg::percentile({1.0, kNaN}, 50.0), std::invalid_argument);
    CHECK_THROWS_AS(gdg::percentile({1.0, INFINITY}, 50.0), std::invalid_argument);
    CHECK_THROWS_AS(gdg::percentile({1.0, 2.0}, -0.5), std::invalid_argument);
    CHECK_THROWS_AS(gdg::percentile({1.0, 2.0}, 100.5), std::invalid_argument);
    CHECK_THROWS_AS(gdg::percentile({1.0, 2.0}, kNaN), std::invalid_argument);
    CHECK_THROWS_AS(gdg::percentiles({1.0, 2.0}, {50.0, 101.0}), std::invalid_argument);
}

TEST_CASE("median_even_odd", "[orderstats][golden]") {
    CHECK(gdg::median({3.0, 1.0, 2.0}) == 2.0);
    CHECK(gdg::median({4.0, 1.0, 3.0, 2.0}) == 2.5);
    CHECK(gdg::median({5.0}) == 5.0);
    CHECK(gdg::median({0.2, 0.1}) == 0x1.3333333333334p-3);  // (0.1 + 0.2) / 2, numpy 2.5
    const auto in = golden("order_input").data;                // 503 values (odd)
    REQUIRE(in.size() == 503);
    CHECK(gdg::median(in) == golden("order_stats").data[0]);
    // The first 502 (even): np.median(order_input[:502]) from numpy 2.5.
    CHECK(gdg::median(std::vector<double>(in.begin(), in.begin() + 502)) == 0x1.7987c80b64e15p-10);
    CHECK_THROWS_AS(gdg::median({}), std::invalid_argument);
    CHECK_THROWS_AS(gdg::median({1.0, kNaN, 2.0}), std::invalid_argument);
}

TEST_CASE("mad_matches", "[orderstats][golden]") {
    CHECK(gdg::mad({1.0, 2.0, 3.0, 4.0, 100.0}) == 1.0);  // median 3; |dev| {2,1,0,1,97}
    CHECK(gdg::mad({2.0, 2.0, 2.0}) == 0.0);
    const auto stats = golden("order_stats").data;          // [median, mad, 1.4826*mad]
    const double m = gdg::mad(golden("order_input").data);
    CHECK(m == stats[1]);
    CHECK(1.4826 * m == stats[2]);
    CHECK_THROWS_AS(gdg::mad({}), std::invalid_argument);
}

TEST_CASE("interp_left_right_clamp", "[orderstats][golden]") {
    const auto xp = golden("interp_xp").data;  // [0, 1, 2.5, 4]
    const auto fp = golden("interp_fp").data;  // [1, 0.5, 0.2, 0]
    const auto x = golden("interp_x").data;    // linspace(-1, 5, 31)
    CHECK(count_unequal(gdg::interp(x, xp, fp), golden("interp_clamp").data) == 0);
    CHECK(count_unequal(gdg::interp(x, xp, fp, fp[0], 0.0), golden("interp_left_fp0_right0").data) == 0);
    CHECK(count_unequal(gdg::interp(golden("interp_xq").data, xp, fp, fp[0], 0.0),
                        golden("interp_q_left_right").data) == 0);

    // Hand: 1.75 lies in [1, 2.5]: 0.5 + (0.2-0.5)/1.5 * 0.75 = 0.35 (numpy: 0x1.6666666666666p-2).
    CHECK(gdg::interp({1.75}, xp, fp)[0] == 0x1.6666666666666p-2);
    CHECK(gdg::interp({-3.0, 9.0}, xp, fp) == std::vector<double>{1.0, 0.0});         // default clamp
    CHECK(gdg::interp({-3.0, 9.0}, xp, fp, -7.0, 8.0) == std::vector<double>{-7.0, 8.0});
    CHECK(gdg::interp({0.0, 4.0, 2.5}, xp, fp) == std::vector<double>{1.0, 0.0, 0.2});  // knots exact
    // Duplicate knots take the rightmost (numpy: interp(1, [0,1,1,2], [0,1,3,4]) == 3).
    CHECK(gdg::interp({1.0}, {0.0, 1.0, 1.0, 2.0}, {0.0, 1.0, 3.0, 4.0})[0] == 3.0);
    // One knot: left / fp / right.
    CHECK(gdg::interp({-1.0, 0.5, 2.0}, {0.5}, {7.0}) == std::vector<double>{7.0, 7.0, 7.0});
    CHECK(gdg::interp({-1.0, 2.0}, {0.5}, {7.0}, -3.0, 9.0) == std::vector<double>{-3.0, 9.0});
    CHECK(std::isnan(gdg::interp({kNaN}, {0.0, 1.0}, {0.0, 1.0})[0]));
    CHECK_THROWS_AS(gdg::interp({1.0}, {}, {}), std::invalid_argument);
    CHECK_THROWS_AS(gdg::interp({1.0}, {0.0, 1.0}, {0.0}), std::invalid_argument);
}

TEST_CASE("nanmedian_ignores_nan", "[orderstats][golden]") {
    const auto in = golden("nanmedian_input");  // (7, 17), ~25% NaN, column 3 all NaN
    REQUIRE(in.shape == std::vector<size_t>{7, 17});
    std::vector<std::vector<double>> stack(7);
    for (size_t r = 0; r < 7; ++r) stack[r].assign(in.data.begin() + r * 17, in.data.begin() + (r + 1) * 17);
    const auto got = gdg::nanmedian_rows(stack);
    const auto want = golden("nanmedian").data;
    REQUIRE(std::isnan(want[3]));
    CHECK(count_unequal(got, want) == 0);  // NaN == NaN counts as equal

    // Hand: columns {1, 3, 2} -> 2, {NaN, NaN, 5} -> 5, all NaN -> NaN, {4, 1} (even) -> 2.5.
    const auto h = gdg::nanmedian_rows({{1.0, kNaN, kNaN, 4.0}, {3.0, kNaN, kNaN, kNaN}, {2.0, 5.0, kNaN, 1.0}});
    REQUIRE(h.size() == 4);
    CHECK(h[0] == 2.0);
    CHECK(h[1] == 5.0);
    CHECK(std::isnan(h[2]));
    CHECK(h[3] == 2.5);
    CHECK(gdg::nanmedian_rows({}).empty());
    CHECK_THROWS_AS(gdg::nanmedian_rows({{1.0, 2.0}, {3.0}}), std::invalid_argument);
}
