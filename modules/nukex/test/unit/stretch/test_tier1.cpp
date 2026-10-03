#include "catch_amalgamated.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"
#include "nukex/stretch/photometric_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <filesystem>

using namespace nukex;

// ══════════════════════════════════════════════════════════
// LUPTON RGB
// ══════════════════════════════════════════════════════════

TEST_CASE("Lupton: f(0) == 0", "[lupton]") {
    LuptonStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("Lupton: monotonically increasing", "[lupton]") {
    LuptonStretch s;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("Lupton: preserves color ratios", "[lupton]") {
    LuptonStretch s;
    s.Q = 8.0f;
    s.stretch = 5.0f;

    Image img(4, 4, 3);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            img.at(x, y, 0) = 0.3f;
            img.at(x, y, 1) = 0.15f;
            img.at(x, y, 2) = 0.05f;
        }

    float ratio_rg_before = img.at(2, 2, 0) / img.at(2, 2, 1);
    float ratio_gb_before = img.at(2, 2, 1) / img.at(2, 2, 2);

    s.apply(img);

    float ratio_rg_after = img.at(2, 2, 0) / img.at(2, 2, 1);
    float ratio_gb_after = img.at(2, 2, 1) / img.at(2, 2, 2);

    REQUIRE(ratio_rg_after == Catch::Approx(ratio_rg_before).margin(0.01f));
    REQUIRE(ratio_gb_after == Catch::Approx(ratio_gb_before).margin(0.01f));
}

TEST_CASE("Lupton: output in [0, 1]", "[lupton]") {
    LuptonStretch s;
    Image img(8, 8, 3);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            for (int c = 0; c < 3; c++)
                img.at(x, y, c) = static_cast<float>(y * 8 + x) / 63.0f;

    s.apply(img);

    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            for (int c = 0; c < 3; c++) {
                REQUIRE(img.at(x, y, c) >= -1e-6f);
                REQUIRE(img.at(x, y, c) <= 1.0f + 1e-6f);
            }
}

TEST_CASE("Lupton: higher Q → more aggressive stretch at faint end", "[lupton]") {
    LuptonStretch lo, hi;
    lo.Q = 2.0f; lo.stretch = 5.0f;
    hi.Q = 20.0f; hi.stretch = 5.0f;
    REQUIRE(hi.apply_scalar(0.01f) > lo.apply_scalar(0.01f));
}

// ══════════════════════════════════════════════════════════
// VERALUX HMS
// ══════════════════════════════════════════════════════════

TEST_CASE("VeraLux: f(0) == 0", "[veralux]") {
    VeraLuxStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("VeraLux: f(1) == 1", "[veralux]") {
    VeraLuxStretch s;
    REQUIRE(s.apply_scalar(1.0f) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("VeraLux: monotonically increasing", "[veralux]") {
    VeraLuxStretch s;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("VeraLux: output in [0, 1]", "[veralux]") {
    VeraLuxStretch s;
    for (float logD : {1.0f, 2.0f, 4.0f}) {
        s.log_D = logD;
        for (int i = 0; i <= 100; i++) {
            float x = i / 100.0f;
            float y = s.apply_scalar(x);
            REQUIRE(y >= -1e-6f);
            REQUIRE(y <= 1.0f + 1e-6f);
        }
    }
}

TEST_CASE("VeraLux: convergence to white for bright pixels", "[veralux]") {
    VeraLuxStretch s;
    s.log_D = 2.0f;
    s.convergence_power = 3.5f;

    Image img(4, 4, 3);
    // Bright red pixel
    img.at(2, 2, 0) = 0.9f;
    img.at(2, 2, 1) = 0.1f;
    img.at(2, 2, 2) = 0.05f;
    // Fill rest with dark
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (x == 2 && y == 2) continue;
            img.at(x, y, 0) = 0.01f;
            img.at(x, y, 1) = 0.01f;
            img.at(x, y, 2) = 0.01f;
        }

    s.apply(img);

    // Bright pixel should have converged toward white (channels more equal)
    float r = img.at(2, 2, 0);
    float g = img.at(2, 2, 1);
    float b = img.at(2, 2, 2);

    // The G/R ratio should be closer to 1 after convergence than before
    float ratio_before = 0.1f / 0.9f;   // 0.111
    float ratio_after = g / r;
    REQUIRE(ratio_after > ratio_before);  // Converged toward white
}

TEST_CASE("VeraLux: higher log_D → more aggressive stretch", "[veralux]") {
    VeraLuxStretch lo, hi;
    lo.log_D = 1.0f;
    hi.log_D = 3.0f;
    REQUIRE(hi.apply_scalar(0.01f) > lo.apply_scalar(0.01f));
}

// ══════════════════════════════════════════════════════════
// CLAHE
// ══════════════════════════════════════════════════════════

TEST_CASE("CLAHE: output in [0, 1]", "[clahe]") {
    Image img(64, 64, 1);
    for (int i = 0; i < 64 * 64; i++)
        img.channel_data(0)[i] = static_cast<float>(i) / (64.0f * 64.0f);

    CLAHEStretch s;
    s.tile_cols = 4;
    s.tile_rows = 4;
    s.luminance_only = false;
    s.apply(img);

    for (int i = 0; i < 64 * 64; i++) {
        REQUIRE(img.channel_data(0)[i] >= -1e-6f);
        REQUIRE(img.channel_data(0)[i] <= 1.0f + 1e-6f);
    }
}

TEST_CASE("CLAHE: enhances local contrast", "[clahe]") {
    // Create image with two halves at different brightnesses
    Image img(64, 64, 1);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            if (x < 32)
                img.at(x, y, 0) = 0.1f + 0.02f * (x + y) / 96.0f;
            else
                img.at(x, y, 0) = 0.7f + 0.02f * (x + y) / 96.0f;
        }

    float dark_range_before = img.at(31, 63, 0) - img.at(0, 0, 0);

    CLAHEStretch s;
    s.tile_cols = 4;
    s.tile_rows = 4;
    s.clip_limit = 3.0f;
    s.luminance_only = false;
    s.apply(img);

    float dark_range_after = img.at(31, 63, 0) - img.at(0, 0, 0);

    // CLAHE should enhance local contrast (expand the range within each region)
    REQUIRE(dark_range_after > dark_range_before * 0.5f);
}

TEST_CASE("CLAHE: higher clip_limit → more contrast", "[clahe]") {
    Image img1(32, 32, 1), img2(32, 32, 1);
    for (int i = 0; i < 32 * 32; i++) {
        float v = static_cast<float>(i) / (32.0f * 32.0f) * 0.3f + 0.1f;
        img1.channel_data(0)[i] = v;
        img2.channel_data(0)[i] = v;
    }

    CLAHEStretch s1, s2;
    s1.clip_limit = 1.5f; s1.tile_cols = 2; s1.tile_rows = 2; s1.luminance_only = false;
    s2.clip_limit = 4.0f; s2.tile_cols = 2; s2.tile_rows = 2; s2.luminance_only = false;
    s1.apply(img1);
    s2.apply(img2);

    // Higher clip limit should produce wider range (more contrast)
    float range1 = img1.channel_data(0)[32*32 - 1] - img1.channel_data(0)[0];
    float range2 = img2.channel_data(0)[32*32 - 1] - img2.channel_data(0)[0];
    REQUIRE(range2 >= range1 - 0.01f);
}

// ══════════════════════════════════════════════════════════
// PHOTOMETRIC
// ══════════════════════════════════════════════════════════

TEST_CASE("Photometric: brighter pixel → higher output", "[photometric]") {
    PhotometricStretch s;
    REQUIRE(s.apply_scalar(0.1f) > s.apply_scalar(0.01f));
}

TEST_CASE("Photometric: output in [0, 1]", "[photometric]") {
    PhotometricStretch s;
    for (int i = 1; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= -1e-6f);
        REQUIRE(y <= 1.0f + 1e-6f);
    }
}

TEST_CASE("Photometric: wider display range → gentler stretch", "[photometric]") {
    PhotometricStretch narrow, wide;
    narrow.mu_bright = 20.0f; narrow.mu_faint = 26.0f;  // 6 mag range
    wide.mu_bright = 18.0f; wide.mu_faint = 30.0f;      // 12 mag range

    // At a mid-value, narrow range should produce higher contrast
    float n_out = narrow.apply_scalar(0.1f);
    float w_out = wide.apply_scalar(0.1f);
    // Narrow range puts more of the display range above this value
    REQUIRE(n_out != Catch::Approx(w_out).margin(0.01f));
}

// ══════════════════════════════════════════════════════════
// VISUAL OUTPUTS
// ══════════════════════════════════════════════════════════

TEST_CASE("Tier1: visual outputs on M16", "[tier1][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }
    std::filesystem::create_directories("test/output");

    // Prepare a copy for each stretch
    Image prepared = img.clone();
    test_util::prepare_for_stretch(prepared);

    // Lupton RGB (operates on the prepared data directly — multi-channel)
    {
        Image out = prepared.clone();
        LuptonStretch s;
        s.Q = 8.0f;
        s.stretch = 5.0f;
        s.apply(out);
        test_util::write_png_8bit("test/output/stretch_lupton.png", out);
    }

    // VeraLux HMS
    {
        Image out = prepared.clone();
        VeraLuxStretch s;
        s.log_D = 2.0f;
        s.protect_b = 6.0f;
        s.convergence_power = 3.5f;
        s.apply(out);
        test_util::write_png_8bit("test/output/stretch_veralux.png", out);
    }

    // CLAHE (finisher — apply after a primary stretch)
    {
        Image out = prepared.clone();
        // First apply a gentle ArcSinh as primary stretch
        ArcSinhStretch pre;
        pre.alpha = 4.0f;
        pre.luminance_only = true;
        pre.apply(out);
        // Then CLAHE for local contrast
        CLAHEStretch s;
        s.clip_limit = 2.0f;
        s.tile_cols = 8;
        s.tile_rows = 8;
        s.luminance_only = true;
        s.apply(out);
        test_util::write_png_8bit("test/output/stretch_clahe.png", out);
    }

    // Photometric
    {
        Image out = img.clone();  // Use unprepared data — photometric does its own sky subtraction
        PhotometricStretch s;
        s.mu_bright = 18.0f;
        s.mu_faint = 28.0f;
        s.luminance_only = true;
        s.apply(out);
        test_util::write_png_8bit("test/output/stretch_photometric.png", out);
    }

    REQUIRE(true);
}

TEST_CASE("VeraLux: auto_tune puts the background on the target, whatever the "
          "data's level", "[stretch][veralux]") {
    // One fixed intensity cannot serve different targets. The three
    // regression corpora have linear backgrounds spanning 0.0166 to 0.1479 --
    // nearly an order of magnitude -- so a single log_D put one stack's
    // median at 0.084 and another's at 0.431.
    for (float bg : {0.0166f, 0.0264f, 0.1479f}) {
        Image img(64, 64, 1);
        img.fill(bg);
        // A little structure above the background so the median is the
        // background rather than the only value present.
        for (int i = 0; i < 64; ++i) img.at(i, 0, 0) = bg * 4.0f;

        VeraLuxStretch v;
        const float solved = v.auto_tune(img, 0.25f);
        INFO("bg=" << bg << " solved log_D=" << solved);
        REQUIRE(solved >= 0.0f);
        REQUIRE(solved <= 7.0f);
        // The background must land on the target.
        REQUIRE(v.apply_scalar(bg) == Catch::Approx(0.25f).margin(0.01f));
    }
}

TEST_CASE("VeraLux: auto_tune leaves a degenerate image alone", "[stretch][veralux]") {
    // An all-black frame has no background to solve against; the default must
    // survive rather than the bisection running away.
    Image img(8, 8, 1);
    img.fill(0.0f);
    VeraLuxStretch v;
    const float before = v.log_D;
    REQUIRE(v.auto_tune(img, 0.25f) == Catch::Approx(before));
    REQUIRE(v.log_D == Catch::Approx(before));
}

// Deterministic, well-spread noise. A plain (x,y) hash clumps along cell
// borders, which once flattened a MAD to ~1e-4 and made a robust-statistics
// gate untestable at any threshold; splitmix64 over the flat index does not.
static float test_gauss(std::uint64_t i) {
    auto mix = [](std::uint64_t z) {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    const double u1 = (mix(2 * i) >> 11) * 0x1.0p-53 + 1e-12;
    const double u2 = (mix(2 * i + 1) >> 11) * 0x1.0p-53;
    return static_cast<float>(std::sqrt(-2.0 * std::log(u1)) *
                              std::cos(6.283185307179586 * u2));
}

// A linear deep-sky frame in miniature: a bright pedestal, tight read noise,
// faint nebulosity a few tens of sigma up, and a sparse star field. Modelled
// on the user's own 74-frame stack, where the background sat at 0.0418 and
// the 99.9th percentile only 44 sigma above it.
static Image narrow_band_on_a_pedestal(float bg, float sigma) {
    Image img(256, 256, 1);
    float* d = img.channel_data(0);
    const int n = 256 * 256;
    for (int i = 0; i < n; ++i) {
        float v = bg + sigma * test_gauss(static_cast<std::uint64_t>(i));
        if (i % 10 == 0)  v += sigma * 25.0f * (static_cast<float>(i % 997) / 997.0f);
        if (i % 1000 == 0) v += sigma * 45.0f;
        d[i] = v;
    }
    return img;
}

/// A mostly-flat background carrying a few shallow depressions -- the shape a
/// vignette or a sky gradient leaves in a deep stack. Each dip is 4 sigma deep
/// and tens of pixels wide, so the frame's LOW TAIL is far fatter than a
/// normal's while the MAD still measures the noise. That combination is what
/// makes a fixed -2.8 sigma threshold cut THROUGH the sky.
static Image background_with_a_gradient(float bg, float sigma) {
    Image img(256, 256, 1);
    float* d = img.channel_data(0);
    const struct { float cx, cy; } dip[] = {{60, 70}, {190, 110}, {120, 200}};
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const int i = y * 256 + x;
            float v = bg + sigma * test_gauss(static_cast<std::uint64_t>(i));
            for (const auto& p : dip) {
                const float dx = static_cast<float>(x) - p.cx;
                const float dy = static_cast<float>(y) - p.cy;
                v -= 4.0f * sigma * std::exp(-(dx * dx + dy * dy) / (2.0f * 20.0f * 20.0f));
            }
            d[i] = v;
        }
    return img;
}

static float percentile_of(const Image& img, double q) {
    std::vector<float> v(img.channel_data(0),
                         img.channel_data(0) + img.width() * img.height());
    auto k = v.begin() + static_cast<std::size_t>(q * (v.size() - 1));
    std::nth_element(v.begin(), k, v.end());
    return *k;
}

TEST_CASE("VeraLux: auto_tune honours the whole range of background targets",
          "[stretch][veralux]") {
    // The background target is a user control (range 0.05-0.50), not a fixed
    // 0.25. A darker target is the lever for someone who dislikes visible sky
    // grain: it lowers the noise floor into the shadows instead of lifting it
    // into plain view. This pins the contract that control depends on across
    // the range, not just at the default.
    for (float target : {0.05f, 0.10f, 0.25f, 0.40f, 0.50f}) {
        for (float bg : {0.0166f, 0.0264f, 0.1479f}) {
            Image img(128, 128, 1);
            float* d = img.channel_data(0);
            for (int i = 0; i < 128 * 128; ++i)
                d[i] = bg + 0.02f * bg * test_gauss(static_cast<std::uint64_t>(i));
            for (int i = 0; i < 128; ++i) d[i] = bg * 4.0f;

            VeraLuxStretch v;
            v.auto_tune(img, target);
            v.apply(img);

            std::vector<float> lum(d, d + 128 * 128);
            const std::size_t mid = lum.size() / 2;
            std::nth_element(lum.begin(), lum.begin() + mid, lum.end());
            INFO("target=" << target << " bg=" << bg << " -> p50=" << lum[mid]
                 << " SP=" << v.SP << " log_D=" << v.log_D);
            REQUIRE(lum[mid] == Catch::Approx(target).margin(0.03f));
        }
    }
}

TEST_CASE("VeraLux: auto_tune solves a shadow point, so a narrow signal band "
          "on a pedestal comes out with contrast", "[stretch][veralux]") {
    // Measured on the user's stack: the linear data is good -- p99.9 sits 44
    // sigma above the background -- but the stretch delivered it as 4% of the
    // output range (p50 0.2569 -> p99.9 0.2969), because the whole signal band
    // is ~1% wide sitting on a pedestal that eats 98% of the curve.
    const float bg = 0.0418f, sigma = 0.0002f;
    Image img = narrow_band_on_a_pedestal(bg, sigma);

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    v.apply(img);

    const float p50 = percentile_of(img, 0.50);
    const float p999 = percentile_of(img, 0.999);
    INFO("p50=" << p50 << " p99.9=" << p999 << " SP=" << v.SP
                << " log_D=" << v.log_D);

    // Positioning is unchanged: the background still lands on the target.
    REQUIRE(p50 == Catch::Approx(0.25f).margin(0.02f));
    // Contrast is the point. Simulated on the real stack, solving SP as well
    // as the intensity took the spread from 0.0393 to 0.2853.
    REQUIRE(p999 - p50 > 0.15f);
}

TEST_CASE("VeraLux: the shadow point sits just below the background, not on it",
          "[stretch][veralux]") {
    // STF's convention: median - 2.8 sigma. Clipping AT the median would throw
    // away half the frame; leaving it at zero is what cost the contrast.
    const float bg = 0.0418f, sigma = 0.0002f;
    Image img = narrow_band_on_a_pedestal(bg, sigma);

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    INFO("SP=" << v.SP << " bg=" << bg);
    REQUIRE(v.SP > 0.0f);
    REQUIRE(v.SP < bg);
    REQUIRE(v.SP == Catch::Approx(bg - 2.8f * sigma).margin(4.0f * sigma));
}

TEST_CASE("VeraLux: a background already at the floor gets no shadow point",
          "[stretch][veralux]") {
    // Data whose median is below 2.8 sigma has nothing to clip; SP must floor
    // at zero rather than going negative and lifting the black point.
    Image img(64, 64, 1);
    float* d = img.channel_data(0);
    for (int i = 0; i < 64 * 64; ++i)
        d[i] = std::max(0.0f, 0.0001f + 0.001f * test_gauss(static_cast<std::uint64_t>(i)));

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    REQUIRE(v.SP >= 0.0f);
    REQUIRE(v.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("VeraLux: the shadow point cannot clip away a background gradient",
          "[stretch][veralux]") {
    // STF's median - 2.8 sigma convention assumes a FLAT background: on a
    // normal one it clips 0.256% of the frame, scattered single pixels. It
    // does not survive a deep stack. Measured on the user's 114-frame M63,
    // 114 frames collapse sigma to 0.000351, so median - 2.8 sigma lands only
    // 0.4% below the median -- INSIDE the frame's own vignetting -- and takes
    // 1.437% of the sky to pure black in clumps up to 231 pixels. Those are
    // the "black circles where the stars should be": 0.000% of the brightest
    // 0.1% of pixels were zeroed and 74.5% of the darkest 2% were.
    //
    // So bound what the shadow point may clip. The bound has to sit above the
    // 0.256% a normal background loses at -2.8 sigma, or it would override the
    // convention on flat data too -- the 0.25% percentile of a normal sits at
    // exactly 2.807 sigma, which is that break-even.
    const float bg = 0.0418f, sigma = 0.0002f;
    Image img = background_with_a_gradient(bg, sigma);

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    v.apply(img);

    const float* d = img.channel_data(0);
    const std::size_t n = 256 * 256;
    const std::size_t black = static_cast<std::size_t>(std::count(d, d + n, 0.0f));
    const double clipped = static_cast<double>(black) / static_cast<double>(n);
    INFO("SP=" << v.SP << " clipped=" << (100.0 * clipped) << "% (" << black
               << " of " << n << ")");

    // Bounded, not disabled: falling back to SP = 0 would clip nothing and
    // cost all the contrast the shadow point was added for.
    REQUIRE(v.SP > 0.0f);
    REQUIRE(clipped <= 0.007);
}

/// Saturation of the BACKGROUND-SUBTRACTED signal in the top 1% of pixels.
/// Subtracting each channel's own sky first is the whole point: the colour of
/// astronomical data lives in the excess over the sky, never in the ratio of
/// the totals.
static float signal_saturation(const Image& img) {
    const int n = img.width() * img.height();
    const float* c[3] = { img.channel_data(0), img.channel_data(1), img.channel_data(2) };
    float bg[3];
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<float> v(c[ch], c[ch] + n);
        std::nth_element(v.begin(), v.begin() + n / 2, v.end());
        bg[ch] = v[n / 2];
    }
    std::vector<float> lum(n);
    for (int i = 0; i < n; ++i)
        lum[i] = 0.2126f * c[0][i] + 0.7152f * c[1][i] + 0.0722f * c[2][i];
    std::vector<float> sorted = lum;
    const std::size_t cut = static_cast<std::size_t>(0.99 * (n - 1));
    std::nth_element(sorted.begin(), sorted.begin() + cut, sorted.end());
    const float thresh = sorted[cut];

    double total = 0.0; int count = 0;
    for (int i = 0; i < n; ++i) {
        if (lum[i] < thresh) continue;
        const float r = c[0][i] - bg[0], g = c[1][i] - bg[1], b = c[2][i] - bg[2];
        const float mx = std::max(r, std::max(g, b));
        const float mn = std::min(r, std::min(g, b));
        if (mx > 1e-8f) { total += (mx - mn) / mx; ++count; }
    }
    return count ? static_cast<float>(total / count) : 0.0f;
}

TEST_CASE("VeraLux: clipping at the shadow point is neutral, not coloured",
          "[stretch][veralux]") {
    // Per-channel stretching clips each channel independently at the shadow
    // point (hms_curve returns 0 for x <= SP). Solving ONE shadow point from
    // the LUMINANCE and applying it to every channel is wrong: luminance is a
    // weighted average, so its noise is lower than any single channel's, and
    // G carries weight 0.7152 so L tracks G while R and B diverge from it.
    //
    // Measured on the user's 114-frame M63 with a luminance-derived SP:
    // R crushed 1.53% of pixels, B 1.79%, G only 0.074% -- and a pixel with R
    // and B at zero but G alive is SATURATED GREEN. 85,229 of them, 1.14% of
    // the frame, single pixels scattered everywhere.
    //
    // Each channel needs its own shadow point, which is what an unlinked STF
    // does. Then a genuinely dark pixel clips in all three at once and goes
    // neutral black, instead of turning into green confetti.
    const float sky = 0.0278f;
    const float sigma_ch[3] = { 0.00090f, 0.00060f, 0.00095f };  // R,B noisier than G
    Image img(256, 256, 3);
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 256 * 256; ++i)
            img.channel_data(ch)[i] =
                sky + sigma_ch[ch] * test_gauss(static_cast<std::uint64_t>(ch * 31337 + i));

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    v.apply(img);

    // A clipped channel does not land on exactly zero: the convergence blend
    // leaves it at L_stretched * k, which is ~0.002 against a 0.25 background.
    // In 8-bit that is (0, 62, 0) -- fully saturated green. Testing for == 0
    // misses the entire defect, which is how the first draft of this test
    // passed against the broken code.
    const float CRUSHED = 0.02f;
    const int n = 256 * 256;
    std::size_t coloured = 0, neutral_black = 0;
    for (int i = 0; i < n; ++i) {
        const int zeros = (img.channel_data(0)[i] < CRUSHED)
                        + (img.channel_data(1)[i] < CRUSHED)
                        + (img.channel_data(2)[i] < CRUSHED);
        if (zeros == 3) ++neutral_black;
        else if (zeros > 0) ++coloured;
    }
    const double coloured_frac = static_cast<double>(coloured) / n;
    INFO("coloured clip " << (100.0 * coloured_frac) << "%, neutral black "
         << neutral_black << ", SP=" << v.SP);

    // A pixel may be clipped -- that is what a shadow point is for -- but it
    // must not be clipped in SOME channels only. That is a colour artefact,
    // not a black point.
    REQUIRE(coloured_frac < 0.002);
}

TEST_CASE("VeraLux: a coloured signal on a neutral sky keeps its colour",
          "[stretch][veralux]") {
    // The bug this pins: apply() took each pixel's chromaticity as the ratio
    // of its TOTAL channel values, r/L. Astronomical signal rides on a sky
    // pedestal far larger than itself, and that pedestal is neutral -- v5.0.3.0
    // deliberately made it neutral -- so r/L is ~1:1:1 no matter what colour
    // the signal is. Every stretched image came out grey.
    //
    // Measured on a 156-frame OSC stack of M63: the linear stack carried
    // signal saturation 0.355 with ratios R 1.000 G 0.997 B 0.721 -- matching
    // the user's own PixInsight integration at 0.337 -- and the stretch
    // delivered 0.059 at 1.000 / 1.000 / 0.965. The colour was in the data
    // the whole time.
    const float sky = 0.0278f, sigma = 0.0004f;
    Image img(256, 256, 3);
    // A neutral sky, plus a disc whose excess is strongly red-weighted.
    //
    // The excess is ~11% of the sky, which is the regime that matters: on the
    // real stack the signal is a small perturbation on a large neutral
    // pedestal, so the ratio of the TOTALS is nearly 1:1:1 however colourful
    // the signal is. A fixture whose signal rivals its sky does not reproduce
    // the bug at all -- the first draft of this test used a 43% excess and
    // passed against the broken code.
    const float excess[3] = { 0.0030f, 0.0030f, 0.0015f };   // 1 : 1 : 0.5
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const int i = y * 256 + x;
            const float dx = static_cast<float>(x) - 128.0f;
            const float dy = static_cast<float>(y) - 128.0f;
            const float f = std::exp(-(dx * dx + dy * dy) / (2.0f * 45.0f * 45.0f));
            for (int ch = 0; ch < 3; ++ch)
                img.channel_data(ch)[i] =
                    sky + sigma * test_gauss(static_cast<std::uint64_t>(ch * 7919 + i))
                        + excess[ch] * f;
        }

    const float before = signal_saturation(img);

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    v.apply(img);

    const float after = signal_saturation(img);
    INFO("signal saturation before " << before << ", after " << after
         << " (ratio " << (after / before) << ")");

    // The stretch may not preserve saturation exactly -- it is a non-linear
    // map -- but it must not throw the colour away. Measured on the real
    // stack the old code retained 18% of it; per-channel retains 56%.
    REQUIRE(before > 0.2f);          // the fixture really is coloured
    REQUIRE(after > 0.4f * before);
}

TEST_CASE("VeraLux: auto_tune tunes the luminance it actually stretches, not "
          "one channel of it", "[stretch][veralux]") {
    // apply() stretches L = wR*R + wG*G + wB*B; auto_tune must solve against
    // that same quantity. Measured on the user's own stack the background is
    // strongly imbalanced -- red 0.0270, green 0.0418 -- so a shadow point
    // derived from green sits ABOVE the luminance of 99.6% of the frame and
    // clips it to black. With SP = 0 the same mismatch was merely a bias: it
    // is why the shipped stretch landed its background at 0.2569 rather than
    // on the 0.2500 it was solving for.
    const float sigma = 0.0002f;
    const float bg[3] = {0.0270f, 0.0418f, 0.0388f};
    Image img(256, 256, 3);
    for (int ch = 0; ch < 3; ++ch) {
        float* d = img.channel_data(ch);
        for (int i = 0; i < 256 * 256; ++i) {
            float v = bg[ch] + sigma * test_gauss(static_cast<std::uint64_t>(ch * 100003 + i));
            if (i % 10 == 0)   v += sigma * 25.0f * (static_cast<float>(i % 997) / 997.0f);
            if (i % 1000 == 0) v += sigma * 45.0f;
            d[i] = v;
        }
    }

    VeraLuxStretch v;
    v.auto_tune(img, 0.25f);
    v.apply(img);

    std::vector<float> lum(256 * 256);
    for (int i = 0; i < 256 * 256; ++i)
        lum[i] = v.w_R * img.channel_data(0)[i] + v.w_G * img.channel_data(1)[i]
               + v.w_B * img.channel_data(2)[i];
    const std::size_t half = lum.size() / 2;
    std::nth_element(lum.begin(), lum.begin() + half, lum.end());
    const float p50 = lum[half];
    const std::size_t black = std::count(lum.begin(), lum.end(), 0.0f);

    INFO("SP=" << v.SP << " log_D=" << v.log_D << " lum p50=" << p50
               << " black=" << black);
    REQUIRE(p50 == Catch::Approx(0.25f).margin(0.02f));
    // A shadow point at median - 2.8 sigma clips the bottom ~0.3% of a normal
    // background, not most of the frame.
    REQUIRE(black < lum.size() / 20);
}
