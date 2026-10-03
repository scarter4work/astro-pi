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

// ── Gain keywords: EGAIN is e-/ADU, GAIN often is not ──────────────
//
// ZWO and QHY cameras write GAIN as the gain MENU INDEX (0-500) and EGAIN as
// the electronic gain in e-/ADU. Reading GAIN as if it were e-/ADU puts a
// number like 200 into the Poisson term, which understates shot noise by
// orders of magnitude. When the electronic gain is genuinely unknown, the
// across-frame fallback is the honest answer.

#include <fitsio.h>

namespace {

// Minimal 4x4 float frame carrying a chosen gain-keyword combination.
// egain/gain/rdnoise are written only when >= 0.
std::string write_gain_fits(const std::string& name,
                            double egain, double gain, double rdnoise,
                            const char* instrument = nullptr) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    std::filesystem::remove(path);
    fitsfile* f = nullptr;
    int status = 0;
    fits_create_file(&f, path.c_str(), &status);
    long naxes[2] = {4, 4};
    fits_create_img(f, FLOAT_IMG, 2, naxes, &status);
    float px[16];
    for (int i = 0; i < 16; i++) px[i] = 0.5f;
    fits_write_img(f, TFLOAT, 1, 16, px, &status);
    double exptime = 120.0;
    fits_update_key(f, TDOUBLE, "EXPTIME", &exptime, nullptr, &status);
    if (egain >= 0.0)   fits_update_key(f, TDOUBLE, "EGAIN",   &egain,   nullptr, &status);
    if (gain >= 0.0)    fits_update_key(f, TDOUBLE, "GAIN",    &gain,    nullptr, &status);
    if (rdnoise >= 0.0) fits_update_key(f, TDOUBLE, "RDNOISE", &rdnoise, nullptr, &status);
    if (instrument) fits_update_key(f, TSTRING, "INSTRUME", const_cast<char*>(instrument), nullptr, &status);
    fits_close_file(f, &status);
    REQUIRE(status == 0);
    return path;
}

} // namespace

TEST_CASE("FITSReader: EGAIN wins over GAIN when both are present", "[fits]") {
    const auto p = write_gain_fits("nukex_gain_egain.fits", 0.24, 200.0, 1.9);
    auto meta = FITSReader::read_headers(p);
    REQUIRE(meta.gain == Catch::Approx(0.24f));
    REQUIRE(meta.has_noise_keywords == true);
    std::filesystem::remove(p);
}

TEST_CASE("FITSReader: a GAIN menu index is not accepted as electronic gain",
          "[fits]") {
    // The ZWO case that matters: EGAIN written as 0 (unset), GAIN = 200.
    // 200 e-/ADU is not a physical electronic gain for any astro camera, so
    // the gain is unknown and the CCD noise model must not claim otherwise.
    const auto p = write_gain_fits("nukex_gain_menu.fits", 0.0, 200.0, 1.9);
    auto meta = FITSReader::read_headers(p);
    REQUIRE(meta.has_noise_keywords == false);
    std::filesystem::remove(p);
}

TEST_CASE("FITSReader: a low GAIN index on a ZWO or QHY camera is still an index, "
          "not electronic gain", "[fits]") {
    // GAIN=10 passes the plausibility ceiling, but on these cameras GAIN is
    // the menu setting whatever its value; only EGAIN is electronic gain.
    for (const char* cam : {"ZWO ASI2600MM Pro", "QHY268M", "Asi294mc Pro"}) {
        const auto p = write_gain_fits("nukex_gain_index_low.fits", -1.0, 10.0, 1.5, cam);
        auto meta = FITSReader::read_headers(p);
        INFO(cam);
        REQUIRE(meta.has_noise_keywords == false);
        std::filesystem::remove(p);
    }
    // The same header from a camera whose GAIN really is e-/ADU is accepted.
    const auto p = write_gain_fits("nukex_gain_index_ccd.fits", -1.0, 10.0, 1.5, "SBIG STL-11000M");
    auto meta = FITSReader::read_headers(p);
    REQUIRE(meta.has_noise_keywords == true);
    REQUIRE(meta.gain == Catch::Approx(10.0f));
    std::filesystem::remove(p);
}

TEST_CASE("FITSReader: a plausible GAIN is still accepted as electronic gain",
          "[fits]") {
    // Older CCD software does write GAIN in e-/ADU. A physically plausible
    // value must keep working.
    const auto p = write_gain_fits("nukex_gain_plausible.fits", -1.0, 1.9, 3.5);
    auto meta = FITSReader::read_headers(p);
    REQUIRE(meta.gain == Catch::Approx(1.9f));
    REQUIRE(meta.has_noise_keywords == true);
    std::filesystem::remove(p);
}
