// gdg::parse_double_exact / parse_float_exact: exact and portable across libstdc++ versions
// (GCC 11's from_chars reports result_out_of_range for subnormal results).
#include <catch_amalgamated.hpp>

#include "core/ParseNumber.h"
#include "core/Catalog.h"

#include <cstring>
#include <limits>
#include <string>

using gdg::parse_double_exact;
using gdg::parse_float_exact;

namespace {
bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }
}  // namespace

TEST_CASE("parse_double_exact_subnormals_and_extremes", "[parse]") {
    struct Case {
        const char* text;
        double want;
    };
    const Case cases[] = {{"5e-324", std::numeric_limits<double>::denorm_min()},
                          {"4.9406564584124654e-324", std::numeric_limits<double>::denorm_min()},
                          {"2.2250738585072009e-308", 2.2250738585072009e-308},  // largest subnormal
                          {"2.2250738585072014e-308", std::numeric_limits<double>::min()},
                          {"-5e-324", -std::numeric_limits<double>::denorm_min()},
                          {"1.7976931348623157e308", std::numeric_limits<double>::max()},
                          {"-0", -0.0},
                          {"0.1", 0.1},
                          {"123456789012345.0", 123456789012345.0}};
    for (const Case& c : cases) {
        INFO("text " << c.text);
        double v = 1.0;
        REQUIRE(parse_double_exact(c.text, v));
        CHECK(same_bits(v, c.want));
    }
}

TEST_CASE("parse_double_exact_rejections", "[parse]") {
    for (const char* bad : {"1e999", "-1e999", "1e-999", "", "-", "+1", " 1", "1 ", "1x", "0x10", "1e", ".e1", "abc"}) {
        INFO("text '" << bad << "'");
        double v = 7.0;
        CHECK_FALSE(parse_double_exact(bad, v));
    }
}

TEST_CASE("parse_float_exact_subnormals_and_overflow", "[parse]") {
    float v = 0.0f;
    REQUIRE(parse_float_exact("1e-45", v));
    CHECK(v == std::numeric_limits<float>::denorm_min());
    REQUIRE(parse_float_exact("1.17549421e-38", v));  // a subnormal just under FLT_MIN
    CHECK(v > 0.0f);
    REQUIRE(parse_float_exact("3.4028235e38", v));
    CHECK(v == std::numeric_limits<float>::max());
    CHECK_FALSE(parse_float_exact("1e39", v));
    CHECK_FALSE(parse_float_exact("1e-60", v));  // underflow to zero
}
