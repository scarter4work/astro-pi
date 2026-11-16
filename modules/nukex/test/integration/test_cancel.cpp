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
    // Unique per run: two runs at once (ctest -j, other worktrees) must not
    // share frames or a cache directory.
    std::random_device rd;
    const fs::path root = fs::temp_directory_path() / ("nukex_cancel_" + name + "_" + std::to_string(rd()));
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

TEST_CASE("Cancel: after the GPU is set up, before Phase B's first batch",
          "[integration][cancel]") {
    // Routing, the output images and the OpenCL context come between the last
    // check of Phase A and the first of Phase B: ~1 s on 24 MP colour.
    auto o = cancel_at("gpu_setup", "GPU: ", 0);
    require_clean(o);
    REQUIRE_FALSE(o.obs.saw_after("Batch 1/"));
    REQUIRE(o.obs.saw_after("Cancelled during distribution fitting."));
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

/// Records every is_cancelled() from the owning thread while the phase named
/// `phase` runs; cancels `after` into the phase when `after` is positive, or at
/// the phase's first progress line when `on_first_frame` is set. With
/// `bounded`, the phase's begin_phase and end_phase count as the first and last
/// poll, so a stretch before the first check or after the last one is a gap too.
///
/// A GPU kernel dispatch cannot check for a cancel: Phase B announces one with
/// a "kernel 2: ... [GPU]" or "kernel 3: ... [GPU]" line and checks right after
/// it. That stretch (the line to the next poll) is recorded as a dispatch, not
/// a gap; GPUContext's staging cap bounds it, not a check.
struct PollRecorder : ProgressObserver {
    std::thread::id owner = std::this_thread::get_id();
    std::string phase = "Measuring frames";
    bool bounded = false;
    bool in_pass = false;
    clk::time_point pass_start{};
    std::chrono::milliseconds after{0};
    bool on_first_frame = false;
    mutable bool fired = false;
    mutable clk::time_point fired_at{};
    mutable long polls = 0;
    mutable int foreign_calls = 0;
    mutable bool have_last = false;
    mutable clk::time_point last_poll{};
    mutable double max_gap = 0.0;
    mutable std::string last_line, max_gap_at;
    mutable bool in_dispatch = false;
    mutable clk::time_point dispatch_start{};
    mutable double max_dispatch = 0.0;
    mutable std::vector<std::pair<clk::time_point, clk::time_point>> dispatches;
    void gap_to(clk::time_point now) const {
        if (in_dispatch) {
            in_dispatch = false;
            dispatches.push_back({dispatch_start, now});
            max_dispatch = std::max(max_dispatch,
                                    std::chrono::duration<double>(now - dispatch_start).count());
        } else if (have_last) {
            const double g = std::chrono::duration<double>(now - last_poll).count();
            if (g > max_gap) { max_gap = g; max_gap_at = last_line; }
        }
        have_last = true;
        last_poll = now;
    }
    /// Seconds of [a, b] spent inside GPU dispatches.
    double dispatch_overlap_s(clk::time_point a, clk::time_point b) const {
        double s = 0.0;
        for (const auto& [d0, d1] : dispatches) {
            const auto lo = std::max(a, d0), hi = std::min(b, d1);
            if (hi > lo) s += std::chrono::duration<double>(hi - lo).count();
        }
        return s;
    }
    void begin_phase(const std::string& n, int) override {
        in_pass = n.find(phase) != std::string::npos;
        if (in_pass) {
            pass_start = clk::now();
            last_line = n;
            if (bounded) gap_to(pass_start);
        }
    }
    void advance(int, const std::string& d) override {
        if (in_pass) last_line = d;
        if (in_pass && d.find("[GPU]") != std::string::npos
            && (d.find("kernel 2:") != std::string::npos || d.find("kernel 3:") != std::string::npos)) {
            const auto now = clk::now();
            gap_to(now);   // the host work up to this line is a gap like any other
            in_dispatch = true;
            dispatch_start = now;
        }
        if (in_pass && on_first_frame && !fired) {
            fired = true;
            fired_at = clk::now();
        }
    }
    clk::time_point ended_at{};   // when the watched phase ended
    void end_phase() override {
        if (in_pass) ended_at = clk::now();
        if (in_pass && bounded) gap_to(ended_at);
        in_pass = false;
    }
    mutable clk::time_point stopped_at{};   // the engine's own "Cancelled ..." line
    void message(const std::string& t) override {
        if (fired && stopped_at == clk::time_point{} && t.rfind("Cancelled", 0) == 0) stopped_at = clk::now();
    }
    bool is_cancelled() const override {
        if (std::this_thread::get_id() != owner) { ++foreign_calls; return false; }
        const auto now = clk::now();
        if (in_pass) { ++polls; gap_to(now); }
        if (!fired && in_pass && after.count() > 0 && now - pass_start >= after) {
            fired = true;
            fired_at = now;
        }
        return fired;
    }
    double max_gap_s() const { return max_gap; }
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
        INFO(rec.polls << " polls during the pass, longest gap " << rec.max_gap_s() << " s");
        REQUIRE(rec.polls > 5);
        REQUIRE(rec.max_gap_s() < 0.05);
        REQUIRE(rec.foreign_calls == 0);
    }
    SECTION("a cancel part-way through lands within 0.75 s") {
        PollRecorder rec;
        rec.after = std::chrono::milliseconds(150);
        auto r = StackingEngine(big_config(root / "cache")).execute(lights, {}, &rec);
        const double latency = std::chrono::duration<double>(clk::now() - rec.fired_at).count();
        REQUIRE(rec.fired);
        const double stop = rec.stopped_at == clk::time_point{} ? -1.0 : std::chrono::duration<double>(rec.stopped_at - rec.fired_at).count();
        INFO("engine stopped " << stop << " s after the request; returned after " << latency << " s");
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
    const double stop = rec.stopped_at == clk::time_point{} ? -1.0 : std::chrono::duration<double>(rec.stopped_at - rec.fired_at).count();
        INFO("engine stopped " << stop << " s after the request; returned after " << latency << " s");
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    REQUIRE(latency < 0.75);
#else
    REQUIRE(latency < 15.0);
#endif
    REQUIRE(r.stacked.empty());
    REQUIRE(rec.foreign_calls == 0);
    fs::remove_all(root);
}

// ── Phase B asks often enough, in every step of a batch ─────────────
//
// Phase B checked for a cancel only between the steps of a batch (and inside
// the fit loop), and its batches are sized by memory: up to 2 GiB of staging,
// 4.8M voxels for ten 24 MP colour frames. The steps around the fit -- copying
// the batch out of the cube, kernels 1-2, writing the classification back,
// reading the fits, kernel 3, writing the selection back -- each ran over the
// whole batch without a check: measured on the IC1805 lights (10 x 24 MP,
// file-backed record, load ~30 on 32 threads) 0.1-0.9 s each, and 6.7 s once
// for the classification writeback under memory pressure. A cancel landing in
// one of them waited for it.
//
// Here the whole of Phase B is watched, from begin_phase to end_phase.

namespace {

void phase_b_gaps(bool gpu, bool file_backed, const char* tag) {
    std::random_device rd;
    const fs::path root = fs::temp_directory_path() / (std::string("nukex_cancel_phase_b_") + tag + "_" + std::to_string(rd()));
    struct RemoveOnExit {
        fs::path p;
        ~RemoveOnExit() { std::error_code ec; fs::remove_all(p, ec); }
    } remove_on_exit{root};
    const auto lights = write_big_frames(root / "frames");
    fs::create_directories(root / "cache");
    StackingEngine::Config cfg = big_config(root / "cache");
    cfg.gpu_config.force_cpu_fallback = !gpu;
    if (file_backed) { cfg.file_backed_cube = true; cfg.file_backed_cube_fraction = 1e-12; }

    SECTION("no cancel: every step of every batch is watched") {
        PollRecorder rec;
        rec.phase = "Phase B";
        rec.bounded = true;
        auto r = StackingEngine(cfg).execute(lights, {}, &rec);
        REQUIRE_FALSE(r.stacked.empty());
        INFO(rec.polls << " polls in Phase B, longest gap " << rec.max_gap_s()
             << " s, ending in \"" << rec.max_gap_at << "\"; longest GPU dispatch "
             << rec.max_dispatch << " s");
        REQUIRE(rec.polls > 5);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
        REQUIRE(rec.max_gap_s() < 0.2);
#endif
        REQUIRE(rec.max_dispatch < 5.0);
        REQUIRE(rec.foreign_calls == 0);
    }
    SECTION("a cancel part-way through lands promptly") {
        PollRecorder rec;
        rec.phase = "Phase B";
        rec.after = std::chrono::milliseconds(300);
        auto r = StackingEngine(cfg).execute(lights, {}, &rec);
        const auto returned = clk::now();
        REQUIRE(rec.fired);
        REQUIRE(rec.ended_at > rec.fired_at);
        // Three different costs: Phase B's host work after the request (what
        // the checks bound), a GPU dispatch already under way (the staging cap
        // bounds it), and releasing the file-backed record and caches
        // afterwards (0.4-2.1 s at 24 MP under load; no check can shorten it).
        const double stop = std::chrono::duration<double>(rec.ended_at - rec.fired_at).count();
        const double in_gpu = rec.dispatch_overlap_s(rec.fired_at, rec.ended_at);
        const double total = std::chrono::duration<double>(returned - rec.fired_at).count();
        INFO("Phase B ended " << stop << " s after the request (" << in_gpu
             << " s of it in a GPU dispatch); execute() returned after " << total << " s");
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
        REQUIRE(stop - in_gpu < 0.25);
#endif
        REQUIRE(total < 15.0);
        REQUIRE(r.stacked.empty());
        REQUIRE(rec.foreign_calls == 0);
    }
}

} // namespace

TEST_CASE("Cancel: Phase B asks for a cancel inside every step (CPU kernels, cube in RAM)",
          "[integration][cancel][phase_b]") {
    phase_b_gaps(/*gpu=*/false, /*file_backed=*/false, "cpu");
}

TEST_CASE("Cancel: Phase B asks for a cancel inside every step (GPU kernels, file-backed record)",
          "[integration][cancel][phase_b][file_backed]") {
#if defined(_WIN32)
    SKIP("Cube file backing is not implemented on Windows");
#else
    phase_b_gaps(/*gpu=*/true, /*file_backed=*/true, "gpu");
#endif
}

// ── Finishing asks for a cancel as often as the stages before it ─────
//
// Everything after Phase B -- the coverage rectangle, the sky-gradient fit and
// subtraction, the spatial context, the pass over the record, the dual-NB
// Q-solve, the trim, background match and noise check -- ran with checks only
// between those steps, and some not even then. On ten 24 MP HaO3 frames under
// load the longest stretches were 34.6 s (coverage), 36.3 s (Q-solve) and
// 18.6 s (spatial context).
//
// Here a 12 MP HaO3 Bayer frame (so the Q-solve runs) is stacked and the
// observer is watched from Phase B's closing line until execute() returns.

#include "synthetic_fits.hpp"

namespace {

struct TailRecorder : ProgressObserver {
    std::thread::id owner = std::this_thread::get_id();
    bool watching = false;
    mutable long polls = 0;
    mutable int foreign_calls = 0;
    mutable clk::time_point last{};
    mutable double max_gap = 0.0;
    mutable std::string line, max_gap_at;
    std::vector<std::string> lines;
    void gap_to(clk::time_point now) const {
        const double g = std::chrono::duration<double>(now - last).count();
        if (g > max_gap) { max_gap = g; max_gap_at = line; }
        last = now;
    }
    void note(const std::string& s) { if (watching) { line = s; lines.push_back(s); } }
    void begin_phase(const std::string& n, int) override { note(n); }
    void advance(int, const std::string& d) override { note(d); }
    void end_phase() override {}
    void message(const std::string& t) override {
        if (!watching && t.rfind("Phase B: ", 0) == 0) {
            watching = true;
            last = clk::now();
        }
        note(t);
    }
    bool is_cancelled() const override {
        if (std::this_thread::get_id() != owner) { ++foreign_calls; return false; }
        if (watching) { ++polls; gap_to(clk::now()); }
        return false;
    }
};

void finishing_gaps(bool gpu, bool file_backed, const char* tag) {
    std::random_device rd;
    const fs::path root = fs::temp_directory_path() / (std::string("nukex_cancel_finishing_") + tag + "_" + std::to_string(rd()));
    struct RemoveOnExit {
        fs::path p;
        ~RemoveOnExit() { std::error_code ec; fs::remove_all(p, ec); }
    } remove_on_exit{root};
    fs::create_directories(root / "cache");
    const std::string frame = (root / "hao3.fits").string();
    test_util::write_synthetic_q_solved_hao3(frame, kBigW, kBigH, "ASI585MC", 0.5f, 0.3f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    cfg.cache_dir = (root / "cache").string();
    cfg.gpu_config.force_cpu_fallback = !gpu;
    if (file_backed) { cfg.file_backed_cube = true; cfg.file_backed_cube_fraction = 1e-12; }
    else             cfg.file_backed_cube = false;

    TailRecorder rec;
    auto r = StackingEngine(cfg).execute({frame}, {}, &rec);
    REQUIRE(r.ok);
    REQUIRE(rec.watching);
    rec.gap_to(clk::now());   // from the last check to execute() returning
    REQUIRE(r.derived.slots.count("Ha") == 1);   // the Q-solve ran
    INFO(rec.polls << " polls after Phase B, longest gap " << rec.max_gap
         << " s, ending after \"" << rec.max_gap_at << "\"");
    REQUIRE(rec.polls > 5);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    REQUIRE(rec.max_gap < 0.2);
#endif
    REQUIRE(rec.foreign_calls == 0);
}

} // namespace

TEST_CASE("Cancel: finishing asks for a cancel inside every step (CPU, cube in RAM)",
          "[integration][cancel][finishing]") {
    finishing_gaps(/*gpu=*/false, /*file_backed=*/false, "cpu");
}

TEST_CASE("Cancel: finishing asks for a cancel inside every step (GPU, file-backed record)",
          "[integration][cancel][finishing][file_backed]") {
#if defined(_WIN32)
    SKIP("Cube file backing is not implemented on Windows");
#else
    finishing_gaps(/*gpu=*/true, /*file_backed=*/true, "gpu");
#endif
}
