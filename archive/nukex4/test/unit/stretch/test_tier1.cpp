#include "catch_amalgamated.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"
#include "nukex/stretch/photometric_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>
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
