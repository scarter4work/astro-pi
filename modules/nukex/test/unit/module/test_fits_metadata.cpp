#include "catch_amalgamated.hpp"
#include "fits_metadata.hpp"
#include <cstdlib>

using nukex::FITSMetadata;
using nukex::read_fits_metadata;

TEST_CASE("read_fits_metadata: missing file returns read_ok=false", "[module][fits_metadata]") {
    auto meta = read_fits_metadata("/nonexistent/path/does_not_exist.fits");
    REQUIRE(meta.read_ok == false);
    REQUIRE(meta.filter.empty());
    REQUIRE(meta.bayer_pat.empty());
    REQUIRE(meta.naxis3 == 1);
}

TEST_CASE("read_fits_metadata: live FITS file parses OK", "[module][fits_metadata]") {
    const char* env = std::getenv("NUKEX_TEST_FITS_LRGB_MONO");
    if (!env) {
        WARN("NUKEX_TEST_FITS_LRGB_MONO unset — skipping live-read test");
        return;
    }
    auto meta = read_fits_metadata(env);
    REQUIRE(meta.read_ok == true);
    REQUIRE(meta.naxis3 >= 1);
    REQUIRE(meta.naxis3 <= 3);
}
