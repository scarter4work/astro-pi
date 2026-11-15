// The measuring pass's products, reused by Phase A, change nothing.
//
// The pass runs frames in parallel and hands Phase A two things it would
// otherwise recompute: each frame's star catalogue (to the aligner) and, while
// a memory budget lasts, the debayered, flat-corrected frame itself. This
// stacks a drifting star field -- so alignment really matches stars and warps
// -- once with both reuses switched off (the path Phase A took before) and
// once with both on, and requires every output plane to agree bit for bit.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/progress_observer.hpp"

#include <fitsio.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

constexpr int kW = 220, kH = 180;

// A fixed irregular sky, panned `ox` px per frame, with noise.
std::vector<float> star_frame(float ox, float oy, unsigned seed) {
    std::vector<float> px(static_cast<std::size_t>(kW) * kH, 0.05f);
    std::uint32_t st = 777u;
    auto next = [&] { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
    for (int i = 0; i < 60; i++) {
        const float sx = static_cast<float>((next() >> 8) % 2000) / 10.0f + 10.0f - ox;
        const float sy = static_cast<float>((next() >> 8) % 1600) / 10.0f + 10.0f - oy;
        const float amp = 0.3f + 0.005f * static_cast<float>(i);
        for (int dy = -9; dy <= 9; dy++)
            for (int dx = -9; dx <= 9; dx++) {
                const int x = static_cast<int>(sx + 0.5f) + dx;
                const int y = static_cast<int>(sy + 0.5f) + dy;
                if (x < 0 || x >= kW || y < 0 || y >= kH) continue;
                const float fx = static_cast<float>(x) - sx, fy = static_cast<float>(y) - sy;
                px[static_cast<std::size_t>(y) * kW + x] += amp * std::exp(-(fx * fx + fy * fy) / 8.0f);
            }
    }
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 0.003f);
    for (auto& v : px) v += noise(rng);
    return px;
}

void write_fits(const std::string& path, const std::vector<float>& px, const std::string& bayer,
                const std::string& instrument, const std::string& filter) {
    fitsfile* fp = nullptr;
    int status = 0;
    std::remove(path.c_str());
    fits_create_file(&fp, path.c_str(), &status);
    long naxes[2] = {kW, kH};
    fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
    fits_write_img(fp, TFLOAT, 1, static_cast<long>(kW) * kH,
                   const_cast<float*>(px.data()), &status);
    if (!bayer.empty())  fits_update_key_str(fp, "BAYERPAT", bayer.c_str(), nullptr, &status);
    fits_update_key_str(fp, "INSTRUME", instrument.c_str(), nullptr, &status);
    if (!filter.empty()) fits_update_key_str(fp, "FILTER", filter.c_str(), nullptr, &status);
    fits_close_file(fp, &status);
    REQUIRE(status == 0);
}

struct MessageLog : ProgressObserver {
    std::vector<std::string> lines;
    void begin_phase(const std::string&, int) override {}
    void advance(int, const std::string&) override {}
    void end_phase() override {}
    void message(const std::string& t) override { lines.push_back(t); }
    bool is_cancelled() const override { return false; }
    bool saw(const std::string& needle) const {
        for (const auto& l : lines) if (l.find(needle) != std::string::npos) return true;
        return false;
    }
};

bool same_image(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height() || a.n_channels() != b.n_channels())
        return false;
    const std::size_t n = static_cast<std::size_t>(a.width()) * a.height() * a.n_channels();
    return std::memcmp(a.data(), b.data(), n * sizeof(float)) == 0;
}

void stack_with_and_without_reuse(const std::vector<std::string>& lights, const fs::path& cache) {
    StackingEngine::Config base;
    base.qe_database_path =
        (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    base.cache_dir = cache.string();
    base.gpu_config.force_cpu_fallback = true;

    StackingEngine::Config off = base;
    off.reuse_measured_stars = false;
    off.keep_calibrated_fraction = 0.0;
    StackingEngine::Config on = base;
    on.reuse_measured_stars = true;
    on.keep_calibrated_fraction = 1.0;   // these frames are tiny: keep all

    MessageLog log_off, log_on;
    auto a = StackingEngine(off).execute(lights, {}, &log_off);
    auto b = StackingEngine(on).execute(lights, {}, &log_on);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    const int n = static_cast<int>(lights.size());
    REQUIRE(a.n_frames_processed == n);
    REQUIRE(b.n_frames_processed == n);
    // Alignment really matched stars on every frame, so the catalogues
    // handed over were load-bearing.
    REQUIRE(a.n_frames_failed_alignment == 0);
    REQUIRE(b.n_frames_failed_alignment == 0);

    // Each run took the path it claims.
    const std::string N = std::to_string(n);
    REQUIRE(log_off.saw("0 of " + N + " calibrated frames kept"));
    REQUIRE(log_off.saw("Phase A: 0 of " + N + " frames aligned on the measuring pass's star catalogues; 0 arrived"));
    REQUIRE(log_on.saw(N + " of " + N + " calibrated frames kept"));
    REQUIRE(log_on.saw("Phase A: " + N + " of " + N + " frames aligned on the measuring pass's star catalogues; "
                       + N + " arrived"));

    REQUIRE(same_image(a.stacked, b.stacked));
    REQUIRE(same_image(a.noise_map, b.noise_map));
    REQUIRE(same_image(a.measured_noise, b.measured_noise));
    REQUIRE(same_image(a.quality_map, b.quality_map));
    REQUIRE(a.trim.x0 == b.trim.x0);
    REQUIRE(a.trim.y0 == b.trim.y0);
    REQUIRE(a.trim.x1 == b.trim.x1);
    REQUIRE(a.trim.y1 == b.trim.y1);
}

} // namespace

TEST_CASE("Measuring-pass reuse: a drifting mono star field stacks bit-identically",
          "[integration][measuring_pass]") {
    const fs::path dir = fs::temp_directory_path() / "nukex_measure_reuse_mono";
    fs::create_directories(dir);
    std::vector<std::string> lights;
    for (int f = 0; f < 8; f++) {
        const std::string p = (dir / ("l_" + std::to_string(f) + ".fits")).string();
        write_fits(p, star_frame(2.5f * f, 1.5f * f, 300u + f), "", "ASI2600MM", "L");
        lights.push_back(p);
    }
    stack_with_and_without_reuse(lights, dir);
    fs::remove_all(dir);
}

TEST_CASE("Measuring-pass reuse: a drifting OSC (Bayer) star field stacks bit-identically",
          "[integration][measuring_pass]") {
    const fs::path dir = fs::temp_directory_path() / "nukex_measure_reuse_osc";
    fs::create_directories(dir);
    std::vector<std::string> lights;
    for (int f = 0; f < 6; f++) {
        const std::string p = (dir / ("osc_" + std::to_string(f) + ".fits")).string();
        write_fits(p, star_frame(2.0f * f, 3.0f * f, 400u + f), "RGGB", "ASI585MC", "");
        lights.push_back(p);
    }
    stack_with_and_without_reuse(lights, dir);
    fs::remove_all(dir);
}
