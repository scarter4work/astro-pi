#include "catch_amalgamated.hpp"
#include "nukex/io/fits_reader.hpp"
#include <filesystem>

using namespace nukex;

// Path to real test FITS file
static const std::string TEST_FITS =
    "/home/scarter4work/projects/processing/M16/"
    "Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit";

TEST_CASE("FITSReader: read headers from real file", "[fits][integration]") {
    if (!std::filesystem::exists(TEST_FITS)) {
        SKIP("Test FITS file not available");
    }

    auto meta = FITSReader::read_headers(TEST_FITS);

    REQUIRE(meta.width == 6072);
    REQUIRE(meta.height == 4042);
    REQUIRE(meta.exposure == Catch::Approx(300.0f));
    REQUIRE(meta.gain == Catch::Approx(6.2f).margin(0.1f));  // EGAIN
    REQUIRE(meta.filter == "HaO3");
    REQUIRE(meta.bayer_pattern == "RGGB");
    REQUIRE(meta.instrument == "ZWO ASI2400MC Pro");
    REQUIRE(meta.focal_length == Catch::Approx(1215.0f));
    REQUIRE(meta.pixel_size == Catch::Approx(5.94f).margin(0.01f));
    REQUIRE(meta.has_plate_scale == true);
    // plate_scale = 206.265 * 5.94 / 1215 ≈ 1.008 arcsec/pixel
    REQUIRE(meta.plate_scale == Catch::Approx(1.008f).margin(0.01f));
    REQUIRE(meta.has_wcs == true);
    REQUIRE(meta.date_obs == "2023-09-02T03:09:59.682813");
}

TEST_CASE("FITSReader: read full image from real file", "[fits][integration]") {
    if (!std::filesystem::exists(TEST_FITS)) {
        SKIP("Test FITS file not available");
    }

    auto result = FITSReader::read(TEST_FITS);

    REQUIRE(result.success == true);
    REQUIRE(result.error.empty());
    REQUIRE(result.image.width() == 6072);
    REQUIRE(result.image.height() == 4042);
    REQUIRE(result.image.n_channels() == 1);  // Raw Bayer = single channel

    // Pixel values should be normalized to [0, 1]
    float min_val = 1.0f, max_val = 0.0f;
    const float* data = result.image.data();
    for (size_t i = 0; i < result.image.data_size(); i++) {
        if (data[i] < min_val) min_val = data[i];
        if (data[i] > max_val) max_val = data[i];
    }
    REQUIRE(min_val >= 0.0f);
    REQUIRE(max_val <= 1.0f);
    REQUIRE(max_val == Catch::Approx(1.0f).margin(0.001f));  // max should normalize to ~1.0
}

TEST_CASE("FITSReader: nonexistent file returns error", "[fits]") {
    auto result = FITSReader::read("/nonexistent/file.fits");
    REQUIRE(result.success == false);
    REQUIRE(!result.error.empty());
}

TEST_CASE("FITSReader: read_headers from nonexistent file returns default", "[fits]") {
    auto meta = FITSReader::read_headers("/nonexistent/file.fits");
    REQUIRE(meta.width == 0);
    REQUIRE(meta.height == 0);
}
