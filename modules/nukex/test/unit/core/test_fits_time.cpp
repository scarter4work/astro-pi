#include "catch_amalgamated.hpp"
#include "nukex/core/fits_time.hpp"

using namespace nukex;

TEST_CASE("parse_fits_datetime: ISO 8601 as cameras write it", "[core][time]") {
    REQUIRE(parse_fits_datetime("1970-01-01T00:00:00") == 0.0);
    REQUIRE(parse_fits_datetime("1970-01-02T00:00:00") == 86400.0);
    REQUIRE(parse_fits_datetime("2025-08-31T21:59:46.123") == Catch::Approx(1756677586.123));
    REQUIRE(parse_fits_datetime("2025-08-31 21:59:46") == Catch::Approx(1756677586.0));
    // The session in the corpus: 21:59:46 on the 31st to 02:51:24 on the 1st.
    const double a = parse_fits_datetime("2025-08-31T21:59:46");
    const double b = parse_fits_datetime("2025-09-01T02:51:24");
    REQUIRE(b - a == Catch::Approx(4.0 * 3600 + 51 * 60 + 38));
}

TEST_CASE("parse_fits_datetime: absent or malformed is 0, never a guess", "[core][time]") {
    REQUIRE(parse_fits_datetime("") == 0.0);
    REQUIRE(parse_fits_datetime("2025-08-31") == 0.0);
    REQUIRE(parse_fits_datetime("31/08/2025 21:59:46") == 0.0);
    REQUIRE(parse_fits_datetime("2025-13-01T00:00:00") == 0.0);
}
