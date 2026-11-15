// Cancelling in every stage lands promptly and leaves nothing behind.
//
// The engine checks for cancellation between the steps of every stage and
// inside its long parallel loops. Each case here requests a cancel at a
// chosen progress line and requires:
//   - execute() returns, within 1 s of the request;
//   - nothing the next checkpoint guards was started (deterministic: the
//     progress lines after the request say what ran);
//   - the run's cache directory is empty again (frame caches and the voxel
//     record are unlinked);
// and, run under ASan (see the perf branch notes), no leak or memory error.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/core/cube.hpp"

#include <fitsio.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace nukex;
namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

namespace {

constexpr int kW = 220, kH = 180, kFrames = 8;

std::vector<float> star_frame(float ox, float oy, unsigned seed) {
    std::vector<float> px(static_cast<std::size_t>(kW) * kH, 0.05f);
    std::uint32_t st = 991u;
    auto next = [&] { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
    for (int i = 0; i < 60; i++) {
        const float sx = static_cast<float>((next() >> 8) % 2000) / 10.0f + 10.0f - ox;
        const float sy = static_cast<float>((next() >> 8) % 1600) / 10.0f + 10.0f - oy;
        const float amp = 0.3f + 0.005f * static_cast<float>(i);
        for (int dy = -9; dy <= 9; dy++)
            for (int dx = -9; dx <= 9; dx++) {
                const int x = static_cast<int>(sx + 0.5f) + dx, y = static_cast<int>(sy + 0.5f) + dy;
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

std::vector<std::string> write_frames(const fs::path& dir) {
    fs::create_directories(dir);
    std::vector<std::string> out;
    for (int f = 0; f < kFrames; f++) {
        const std::string p = (dir / ("l_" + std::to_string(f) + ".fits")).string();
        const auto px = star_frame(2.5f * f, 1.5f * f, 700u + f);
        fitsfile* fp = nullptr;
        int status = 0;
        std::remove(p.c_str());
        fits_create_file(&fp, p.c_str(), &status);
        long naxes[2] = {kW, kH};
        fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
        fits_write_img(fp, TFLOAT, 1, static_cast<long>(kW) * kH, const_cast<float*>(px.data()), &status);
        fits_update_key_str(fp, "INSTRUME", "ASI2600MM", nullptr, &status);
        fits_update_key_str(fp, "FILTER", "L", nullptr, &status);
        fits_close_file(fp, &status);
        REQUIRE(status == 0);
        out.push_back(p);
    }
    return out;
}

// Cancels at the (skip+1)-th progress text containing `trigger`, and records
// every text that arrives after it.
struct CancelAt : ProgressObserver {
    std::string trigger;
    int skip = 0;
    bool fired = false;
    clk::time_point fired_at{};
    std::vector<std::string> after;
    void see(const std::string& s) {
        if (fired) { after.push_back(s); return; }
        if (s.find(trigger) == std::string::npos) return;
        if (skip-- > 0) return;
        fired = true;
        fired_at = clk::now();
    }
    void begin_phase(const std::string& n, int) override { see(n); }
    void advance(int, const std::string& d) override { see(d); }
    void end_phase() override {}
    void message(const std::string& t) override { see(t); }
    bool is_cancelled() const override { return fired; }
    bool saw_after(const std::string& needle) const {
        for (const auto& l : after) if (l.find(needle) != std::string::npos) return true;
        return false;
    }
};

struct Outcome {
    StackingEngine::ExecuteResult result;
    CancelAt obs;
    double latency_s = 0.0;
    bool cache_empty = false;
};

Outcome cancel_at(const std::string& name, const std::string& trigger, int skip,
                  bool file_backed = false) {
    const fs::path root = fs::temp_directory_path() / ("nukex_cancel_" + name);
    fs::remove_all(root);
    const auto lights = write_frames(root / "frames");
    const fs::path cache = root / "cache";
    fs::create_directories(cache);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    cfg.cache_dir = cache.string();
    cfg.gpu_config.force_cpu_fallback = true;   // the CPU spatial context is interruptible
    if (file_backed) cfg.file_backed_cube_fraction = 1e-12;
    else             cfg.file_backed_cube = false;

    Outcome o;
    o.obs.trigger = trigger;
    o.obs.skip = skip;
    o.result = StackingEngine(cfg).execute(lights, {}, &o.obs);
    const auto done = clk::now();
    REQUIRE(o.obs.fired);
    o.latency_s = std::chrono::duration<double>(done - o.obs.fired_at).count();
    o.result.cube.reset();   // the result holds the record; release it first
    o.cache_empty = fs::is_empty(cache);
    fs::remove_all(root);
    return o;
}

void require_clean(const Outcome& o) {
    INFO("latency " << o.latency_s << " s");
    REQUIRE(o.latency_s < 1.0);
    REQUIRE(o.cache_empty);
    REQUIRE(o.result.stacked.empty());   // no output from a cancelled run
}

} // namespace

TEST_CASE("Cancel: in the measuring pass, before any frame is measured",
          "[integration][cancel]") {
    auto o = cancel_at("measure", "Measuring frames", 0);
    require_clean(o);
    REQUIRE_FALSE(o.obs.saw_after("  frame "));          // no frame measured
    REQUIRE_FALSE(o.obs.saw_after("Phase A: Loading frames"));
    REQUIRE(o.obs.saw_after("Cancelled while measuring frames."));
    REQUIRE(o.result.n_frames_processed == 0);
}

TEST_CASE("Cancel: in Phase A after a frame is loaded, before it is aligned",
          "[integration][cancel]") {
    auto o = cancel_at("load", "Frame 3/", 0);
    require_clean(o);
    REQUIRE_FALSE(o.obs.saw_after("  aligning"));
    REQUIRE_FALSE(o.obs.saw_after("Frame 4/"));
    REQUIRE(o.obs.saw_after("Cancelled during frame loading."));
    REQUIRE(o.result.n_frames_processed == 2);
}

TEST_CASE("Cancel: in Phase A while a frame is aligned",
          "[integration][cancel]") {
    auto o = cancel_at("align", "  aligning", 2);
    require_clean(o);
    REQUIRE_FALSE(o.obs.saw_after("  aligned"));
    REQUIRE_FALSE(o.obs.saw_after("  caching"));
    REQUIRE(o.result.n_frames_processed == 2);
}

TEST_CASE("Cancel: in Phase A after a frame is cached, before it is accumulated",
          "[integration][cancel]") {
    auto o = cancel_at("accumulate", "  caching", 2);
    require_clean(o);
    REQUIRE_FALSE(o.obs.saw_after("  accumulating"));
    REQUIRE(o.result.n_frames_processed == 2);
}

TEST_CASE("Cancel: while the RAM accumulators are stored into a file-backed record",
          "[integration][cancel][file_backed]") {
#if defined(_WIN32)
    SKIP("Cube file backing is not implemented on Windows");
#else
    auto o = cancel_at("store", "frames aligned on the measuring pass", 0, /*file_backed=*/true);
    require_clean(o);
    REQUIRE(o.obs.saw_after("Cancelled during frame loading."));
    REQUIRE_FALSE(o.obs.saw_after("Estimator:"));         // Phase B never began
#endif
}

TEST_CASE("Cancel: in Phase B between a batch's kernels and its fitting",
          "[integration][cancel]") {
    auto o = cancel_at("kernels", "  kernel 1", 0);
    require_clean(o);
    REQUIRE_FALSE(o.obs.saw_after("fitting distributions"));
    REQUIRE(o.obs.saw_after("Cancelled during distribution fitting."));
}

TEST_CASE("Cancel: in Phase B while distributions are fitted",
          "[integration][cancel]") {
    auto o = cancel_at("fitting", "fitting distributions", 0);
    require_clean(o);
    REQUIRE_FALSE(o.obs.saw_after("kernel 3"));
    REQUIRE(o.obs.saw_after("Cancelled during distribution fitting."));
}

TEST_CASE("Cancel: in Phase C while spatial context is computed",
          "[integration][cancel]") {
    auto o = cancel_at("spatial", "spatial context", 0);
    require_clean(o);
    REQUIRE_FALSE(o.obs.saw_after("dominant shape"));
    REQUIRE(o.obs.saw_after("Cancelled during post-processing."));
}

TEST_CASE("Cancel: in Phase C's pass over the record",
          "[integration][cancel]") {
    auto o = cancel_at("post", "dominant shape", 0);
    require_clean(o);
    REQUIRE_FALSE(o.obs.saw_after("Coverage trim"));
    REQUIRE(o.obs.saw_after("Cancelled during post-processing."));
}
