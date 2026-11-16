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
#include <thread>
#include <algorithm>

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

// ── The measuring pass asks often enough, from the owning thread ────
//
// The observer may only be asked from the thread that runs execute() (the
// PixInsight console's abort runs the core's event loop). In the measuring
// pass that thread used to be worker 0 of the frame loop, so it asked only
// between the steps of the frame it was measuring itself: measured through the
// app at 24 MP, a cancel landed 0.1-4.3 s (9.4 s once) after the request.
// Here frames are large enough that one step takes long, the observer records
// when -- and from which thread -- it is asked, and cancels part-way.

namespace {

constexpr int kBigW = 4000, kBigH = 3000, kBigFrames = 6;

std::vector<std::string> write_big_frames(const fs::path& dir) {
    fs::create_directories(dir);
    std::vector<std::string> out;
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 0.003f);
    std::vector<float> base(static_cast<std::size_t>(kBigW) * kBigH, 0.05f);
    std::uint32_t st = 1234u;
    auto next = [&] { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
    for (int i = 0; i < 400; i++) {
        const int sx = 10 + static_cast<int>(next() % (kBigW - 20)), sy = 10 + static_cast<int>(next() % (kBigH - 20));
        for (int dy = -6; dy <= 6; dy++)
            for (int dx = -6; dx <= 6; dx++)
                base[static_cast<std::size_t>(sy + dy) * kBigW + (sx + dx)] += 0.4f * std::exp(-(dx * dx + dy * dy) / 6.0f);
    }
    for (int f = 0; f < kBigFrames; f++) {
        std::vector<float> px = base;
        for (auto& v : px) v += noise(rng);
        const std::string p = (dir / ("big_" + std::to_string(f) + ".fits")).string();
        fitsfile* fp = nullptr;
        int status = 0;
        std::remove(p.c_str());
        fits_create_file(&fp, p.c_str(), &status);
        long naxes[2] = {kBigW, kBigH};
        fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
        fits_write_img(fp, TFLOAT, 1, static_cast<long>(px.size()), px.data(), &status);
        fits_update_key_str(fp, "INSTRUME", "ASI2600MM", nullptr, &status);
        fits_update_key_str(fp, "FILTER", "L", nullptr, &status);
        fits_close_file(fp, &status);
        REQUIRE(status == 0);
        out.push_back(p);
    }
    return out;
}

/// Records every is_cancelled() from the owning thread while the measuring
/// pass runs; cancels `after` into the pass when `after` is positive, or at
/// the pass's first progress line when `on_first_frame` is set.
struct PollRecorder : ProgressObserver {
    std::thread::id owner = std::this_thread::get_id();
    bool in_pass = false;
    clk::time_point pass_start{};
    std::chrono::milliseconds after{0};
    bool on_first_frame = false;
    mutable bool fired = false;
    mutable clk::time_point fired_at{};
    mutable std::vector<clk::time_point> polls;
    mutable int foreign_calls = 0;
    void begin_phase(const std::string& n, int) override {
        in_pass = n.find("Measuring frames") != std::string::npos;
        if (in_pass) pass_start = clk::now();
    }
    void advance(int, const std::string&) override {
        if (in_pass && on_first_frame && !fired) {
            fired = true;
            fired_at = clk::now();
        }
    }
    void end_phase() override { in_pass = false; }
    void message(const std::string&) override {}
    bool is_cancelled() const override {
        if (std::this_thread::get_id() != owner) { ++foreign_calls; return false; }
        const auto now = clk::now();
        if (in_pass) polls.push_back(now);
        if (!fired && in_pass && after.count() > 0 && now - pass_start >= after) {
            fired = true;
            fired_at = now;
        }
        return fired;
    }
    double max_gap_s() const {
        double g = 0.0;
        for (std::size_t i = 1; i < polls.size(); ++i)
            g = std::max(g, std::chrono::duration<double>(polls[i] - polls[i - 1]).count());
        return g;
    }
};

StackingEngine::Config big_config(const fs::path& cache) {
    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    cfg.cache_dir = cache.string();
    cfg.gpu_config.force_cpu_fallback = true;
    cfg.file_backed_cube = false;
    return cfg;
}

} // namespace

TEST_CASE("Cancel: the measuring pass asks for a cancel every few tens of ms, only from its own thread",
          "[integration][cancel][measuring]") {
    std::random_device rd;
    const fs::path root = fs::temp_directory_path() / ("nukex_cancel_poll_" + std::to_string(rd()));
    const auto lights = write_big_frames(root / "frames");
    fs::create_directories(root / "cache");

    SECTION("no cancel: the whole pass is watched") {
        PollRecorder rec;
        auto r = StackingEngine(big_config(root / "cache")).execute(lights, {}, &rec);
        (void)r;
        INFO(rec.polls.size() << " polls during the pass, longest gap " << rec.max_gap_s() << " s");
        REQUIRE(rec.polls.size() > 5);
        REQUIRE(rec.max_gap_s() < 0.05);
        REQUIRE(rec.foreign_calls == 0);
    }
    SECTION("a cancel part-way through lands within 0.75 s") {
        PollRecorder rec;
        rec.after = std::chrono::milliseconds(150);
        auto r = StackingEngine(big_config(root / "cache")).execute(lights, {}, &rec);
        const double latency = std::chrono::duration<double>(clk::now() - rec.fired_at).count();
        REQUIRE(rec.fired);
        INFO("latency " << latency << " s");
        // The 0.75 s target is a property of an optimised build: unoptimised
        // and under ASan one 12 MP step takes seconds. The poll cadence above
        // is asserted in every build; here a sanitizer/debug build only has
        // to land.
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
        REQUIRE(latency < 0.75);
#else
        REQUIRE(latency < 15.0);
#endif
        REQUIRE(r.stacked.empty());
        REQUIRE(rec.foreign_calls == 0);
    }
    fs::remove_all(root);
}

// ── A cancel while the workers are detecting stars on a crowded field ──
//
// Star detection finds every local maximum above median + 5 sigma, then
// drops those within the exclusion radius of a brighter one. On a frame
// whose nebula sits wholly above that threshold, every noise peak in the
// nebula is a candidate: the real IC1805 HaO3 lights (24 MP) give 125k-260k,
// and the exclusion test against every kept candidate took 4-15 s per frame
// with no checkpoint. A cancel landed only when the slowest detection in
// flight finished: 14-18 s through the app 3 s into the pass under 5.3.1.0,
// which starts every worker at once (5.3.0.0 measured frame 0 alone first, so
// 3 s in only one detection was running: ~4 s).
//
// Here frame 0's nebula is narrow, so it is measured first, and every other
// frame's covers 45% of it. The cancel comes at frame 0's progress line,
// while the other workers are inside detection.

namespace {

std::vector<std::string> write_nebula_frames(const fs::path& dir) {
    fs::create_directories(dir);
    std::vector<std::string> out;
    std::mt19937 rng(11);
    std::normal_distribution<float> noise(0.0f, 0.003f);
    for (int f = 0; f < kBigFrames; f++) {
        const int nebula_w = f == 0 ? kBigW / 20 : kBigW * 9 / 20;
        std::vector<float> px(static_cast<std::size_t>(kBigW) * kBigH);
        for (int y = 0; y < kBigH; y++)
            for (int x = 0; x < kBigW; x++)
                px[static_cast<std::size_t>(y) * kBigW + x] = (x < nebula_w ? 0.2f : 0.05f) + noise(rng);
        const std::string p = (dir / ("nebula_" + std::to_string(f) + ".fits")).string();
        fitsfile* fp = nullptr;
        int status = 0;
        std::remove(p.c_str());
        fits_create_file(&fp, p.c_str(), &status);
        long naxes[2] = {kBigW, kBigH};
        fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
        fits_write_img(fp, TFLOAT, 1, static_cast<long>(px.size()), px.data(), &status);
        fits_update_key_str(fp, "INSTRUME", "ASI2600MM", nullptr, &status);
        fits_update_key_str(fp, "FILTER", "L", nullptr, &status);
        fits_close_file(fp, &status);
        REQUIRE(status == 0);
        out.push_back(p);
    }
    return out;
}

} // namespace

TEST_CASE("Cancel: in the measuring pass while every worker detects stars on a crowded field",
          "[integration][cancel][measuring][crowded]") {
    std::random_device rd;
    const fs::path root = fs::temp_directory_path() / ("nukex_cancel_crowded_" + std::to_string(rd()));
    const auto lights = write_nebula_frames(root / "frames");
    fs::create_directories(root / "cache");

    PollRecorder rec;
    rec.on_first_frame = true;
    auto r = StackingEngine(big_config(root / "cache")).execute(lights, {}, &rec);
    const double latency = std::chrono::duration<double>(clk::now() - rec.fired_at).count();
    REQUIRE(rec.fired);
    INFO("latency " << latency << " s");
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    REQUIRE(latency < 0.75);
#else
    REQUIRE(latency < 15.0);
#endif
    REQUIRE(r.stacked.empty());
    REQUIRE(rec.foreign_calls == 0);
    fs::remove_all(root);
}
