// A file-backed voxel record stacks to exactly what an in-memory one does.
//
// The file-backed path accumulates Phase A in RAM (RunningAccumulators) and
// copies the result into the record once, instead of rewriting the record
// per frame. File backing is forced here with a tiny memory budget
// (file_backed_cube_fraction), the way a 24 MP stack on a 30 GB machine gets
// it, and every output plane plus every voxel's Phase A state is compared bit
// for bit against the same batch held in memory.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/progress_observer.hpp"

#include <fitsio.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

void write_noisy(const std::string& path, int w, int h, const std::string& bayer,
                 const std::string& instrument, const std::string& filter,
                 float level, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 0.02f);
    std::vector<float> px(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            // A gentle gradient plus noise: every pixel and frame differs.
            px[static_cast<std::size_t>(y) * w + x] =
                level + 0.001f * static_cast<float>(x + y) + noise(rng);
    fitsfile* fp = nullptr;
    int status = 0;
    std::remove(path.c_str());
    fits_create_file(&fp, path.c_str(), &status);
    long naxes[2] = {w, h};
    fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
    fits_write_img(fp, TFLOAT, 1, static_cast<long>(w) * h, px.data(), &status);
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

void stack_both_ways(const std::vector<std::string>& lights, const fs::path& cache) {
    StackingEngine::Config base;
    base.qe_database_path =
        (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    base.cache_dir = cache.string();
    base.gpu_config.force_cpu_fallback = true;

    StackingEngine::Config in_mem = base;
    in_mem.file_backed_cube = false;
    StackingEngine::Config on_file = base;
    on_file.file_backed_cube = true;
    on_file.file_backed_cube_fraction = 1e-12;   // any record "crowds" this budget

    MessageLog mem_log, file_log;
    auto a = StackingEngine(in_mem).execute(lights, {}, &mem_log);
    auto b = StackingEngine(on_file).execute(lights, {}, &file_log);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    REQUIRE(a.n_frames_processed == static_cast<int>(lights.size()));
    REQUIRE(b.n_frames_processed == static_cast<int>(lights.size()));

    // The two runs really did take the two paths.
    REQUIRE_FALSE(a.cube->file_backed());
    REQUIRE(b.cube->file_backed());
    REQUIRE_FALSE(mem_log.saw("Phase A accumulators"));
    REQUIRE(file_log.saw("Phase A accumulators:"));
    REQUIRE_FALSE(file_log.saw("accumulating into the file instead"));

    REQUIRE(same_image(a.stacked, b.stacked));
    REQUIRE(same_image(a.noise_map, b.noise_map));
    REQUIRE(same_image(a.measured_noise, b.measured_noise));
    REQUIRE(same_image(a.quality_map, b.quality_map));

    // And the record itself: every voxel's Phase A state, bit for bit.
    const Cube& ca = *a.cube;
    const Cube& cb = *b.cube;
    REQUIRE(ca.width == cb.width);
    REQUIRE(ca.height == cb.height);
    const int nc = ca.channel_config.n_channels;
    long mismatches = 0, with_samples = 0;
    for (int y = 0; y < ca.height; y++)
        for (int x = 0; x < ca.width; x++) {
            const auto& va = ca.at(x, y);
            const auto& vb = cb.at(x, y);
            if (va.n_frames != vb.n_frames) ++mismatches;
            if (va.n_frames > 0) ++with_samples;
            for (int c = 0; c < nc; c++) {
                if (std::memcmp(&va.channel(c).welford, &vb.channel(c).welford,
                                sizeof(WelfordAccumulator)) != 0) ++mismatches;
                if (std::memcmp(&va.channel(c).histogram, &vb.channel(c).histogram,
                                sizeof(PixelHistogram)) != 0) ++mismatches;
            }
        }
    REQUIRE(mismatches == 0);
    REQUIRE(with_samples == static_cast<long>(ca.width) * ca.height);

    // Phase C wrote the spatial context into the record, each field where it
    // belongs: on these flat, gently graded frames the local background is
    // the sky level and the Sobel gradient is small, and the measured-noise
    // map is the record's local_rms at the trimmed coordinates.
    std::vector<float> bgs, grads, rmss;
    for (int y = 0; y < cb.height; y++)
        for (int x = 0; x < cb.width; x++) {
            bgs.push_back(cb.at(x, y).local_background);
            grads.push_back(cb.at(x, y).gradient_mag);
            rmss.push_back(cb.at(x, y).local_rms);
        }
    std::nth_element(bgs.begin(), bgs.begin() + bgs.size() / 2, bgs.end());
    std::nth_element(grads.begin(), grads.begin() + grads.size() / 2, grads.end());
    const float bg_med = bgs[bgs.size() / 2], grad_med = grads[grads.size() / 2];
    INFO("median local_background " << bg_med << ", median gradient_mag " << grad_med);
    REQUIRE(bg_med > 0.2f);
    REQUIRE(bg_med < 0.5f);
    REQUIRE(grad_med < 0.1f);
    // And the measured noise is a measurement: the frames carry 0.02 of
    // noise, so the stack's local scatter is positive and below that.
    std::nth_element(rmss.begin(), rmss.begin() + rmss.size() / 2, rmss.end());
    const float rms_med = rmss[rmss.size() / 2];
    INFO("median local_rms " << rms_med);
    REQUIRE(rms_med > 1e-4f);
    REQUIRE(rms_med < 0.02f);
    const int x0 = b.trim.applied ? b.trim.x0 : 0, y0 = b.trim.applied ? b.trim.y0 : 0;
    long rms_mismatch = 0;
    for (int y = 0; y < b.measured_noise.height(); y++)
        for (int x = 0; x < b.measured_noise.width(); x++)
            if (b.measured_noise.at(x, y, 0) != cb.at(x + x0, y + y0).local_rms) ++rms_mismatch;
    REQUIRE(rms_mismatch == 0);
}

} // namespace

TEST_CASE("File-backed record, RAM accumulators: OSC batch is bit-identical to in-memory",
          "[integration][phase_a][file_backed]") {
#if defined(_WIN32)
    SKIP("Cube file backing is not implemented on Windows");
#else
    const fs::path dir = fs::temp_directory_path() / "nukex_fb_accumulate_osc";
    fs::create_directories(dir);
    std::vector<std::string> lights;
    for (int f = 0; f < 6; f++) {
        const std::string p = (dir / ("osc_" + std::to_string(f) + ".fits")).string();
        write_noisy(p, 48, 36, "RGGB", "ASI585MC", "", 0.25f + 0.01f * f, 100u + f);
        lights.push_back(p);
    }
    stack_both_ways(lights, dir);
    fs::remove_all(dir);
#endif
}

TEST_CASE("File-backed record, RAM accumulators: mono batch is bit-identical to in-memory",
          "[integration][phase_a][file_backed]") {
#if defined(_WIN32)
    SKIP("Cube file backing is not implemented on Windows");
#else
    const fs::path dir = fs::temp_directory_path() / "nukex_fb_accumulate_mono";
    fs::create_directories(dir);
    std::vector<std::string> lights;
    for (int f = 0; f < 7; f++) {
        const std::string p = (dir / ("l_" + std::to_string(f) + ".fits")).string();
        write_noisy(p, 40, 30, "", "ASI2600MM", "L", 0.3f + 0.005f * f, 200u + f);
        lights.push_back(p);
    }
    stack_both_ways(lights, dir);
    fs::remove_all(dir);
#endif
}
