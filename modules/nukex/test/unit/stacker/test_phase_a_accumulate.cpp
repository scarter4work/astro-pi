// Phase A accumulation kept apart from the voxel record (RunningAccumulators)
// and the row-parallel accumulate_frame that feeds it.
//
// The contract under test: whichever target Phase A accumulates into -- the
// record directly (CubeSink) or RAM beside a file-backed record (RamSink,
// then store_into) -- every voxel ends with exactly the Welford state,
// histogram and frame count the original serial per-pixel loop produced.
// "Exactly" means bit for bit: the reference below is that loop, written out.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/phase_a_accumulate.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/core/running_accumulators.hpp"
#include "nukex/core/cancel_poll.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/io/image.hpp"

#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f;
    f.cls  = cls;
    f.name = name;
    return ChannelConfig::from_filter(f);
}

// The serial loop Phase A ran before accumulation was split out, kept here
// verbatim as the reference.
void reference_route(SubcubeVoxel& voxel, int idx, float value, bool covered) {
    if (!covered) return;
    voxel.channel(idx).welford.update(value);
    if (voxel.channel(idx).welford.count() == 1)
        voxel.channel(idx).histogram.initialize_range(value - 0.1f, value + 0.1f);
    voxel.channel(idx).histogram.update(value);
}

void reference_accumulate(Cube& cube, const Image& img, const CoverageMask& cov,
                          AccumulateKind kind, const int s[4]) {
    auto cv = [&](int c, int y, int x) { return cov.empty() || cov.covered(c, y, x); };
    for (int y = 0; y < cube.height; y++)
        for (int x = 0; x < cube.width; x++) {
            auto& v = cube.at(x, y);
            if (kind == AccumulateKind::OSC_WITH_LUMA) {
                float r = img.at(x, y, 0), g = img.at(x, y, 1), b = img.at(x, y, 2);
                float l = 0.299f * r + 0.587f * g + 0.114f * b;
                bool cr = cv(0, y, x), cg = cv(1, y, x), cb = cv(2, y, x);
                reference_route(v, s[0], r, cr);
                reference_route(v, s[1], g, cg);
                reference_route(v, s[2], b, cb);
                reference_route(v, s[3], l, cr && cg && cb);
                if (cr || cg || cb) v.n_frames++;
            } else if (kind == AccumulateKind::THREE_PLANE) {
                bool cr = cv(0, y, x), cg = cv(1, y, x), cb = cv(2, y, x);
                reference_route(v, s[0], img.at(x, y, 0), cr);
                reference_route(v, s[1], img.at(x, y, 1), cg);
                reference_route(v, s[2], img.at(x, y, 2), cb);
                if (cr || cg || cb) v.n_frames++;
            } else {
                bool c0 = cv(0, y, x);
                reference_route(v, s[0], img.at(x, y, 0), c0);
                if (c0) v.n_frames++;
            }
        }
}

bool same_bits(const void* a, const void* b, std::size_t n) { return std::memcmp(a, b, n) == 0; }

// Every Phase A field of every voxel, compared bitwise.
void require_same_accumulators(const Cube& a, const Cube& b, int nc) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    long mismatches = 0;
    for (int y = 0; y < a.height; y++)
        for (int x = 0; x < a.width; x++) {
            const auto& va = a.at(x, y);
            const auto& vb = b.at(x, y);
            if (va.n_frames != vb.n_frames) ++mismatches;
            for (int c = 0; c < nc; c++) {
                if (!same_bits(&va.channel(c).welford, &vb.channel(c).welford,
                               sizeof(WelfordAccumulator))) ++mismatches;
                if (!same_bits(&va.channel(c).histogram, &vb.channel(c).histogram,
                               sizeof(PixelHistogram))) ++mismatches;
            }
        }
    REQUIRE(mismatches == 0);
}

// A frame of noisy values and a random partial coverage mask, so both the
// covered and uncovered branches and the L gating are exercised per pixel.
struct Frame { Image img; CoverageMask cov; };
Frame random_frame(int w, int h, int nc, std::mt19937& rng, bool empty_mask) {
    Frame f{Image(w, h, nc), CoverageMask()};
    std::uniform_real_distribution<float> val(0.0f, 1.0f);
    for (int c = 0; c < nc; c++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) f.img.at(x, y, c) = val(rng);
    if (!empty_mask) {
        f.cov = CoverageMask(w, h, nc);
        std::bernoulli_distribution keep(0.8);
        for (int c = 0; c < nc; c++)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) f.cov.set_covered(c, y, x, keep(rng));
    }
    return f;
}

struct Case { FilterClass cls; const char* name; AccumulateKind kind; int planes; };

const Case kCases[] = {
    {FilterClass::BROADBAND_OSC, "OSC",  AccumulateKind::OSC_WITH_LUMA, 3},
    {FilterClass::DUAL_NB_OSC,   "HaO3", AccumulateKind::THREE_PLANE,   3},
    {FilterClass::BROADBAND_L,   "L",    AccumulateKind::MONO,          1},
};

void slots_for(const Case& k, const ChannelConfig& cfg, int s[4]) {
    s[0] = s[1] = s[2] = s[3] = -1;
    if (k.kind == AccumulateKind::OSC_WITH_LUMA) {
        s[0] = cfg.slot_index("R"); s[1] = cfg.slot_index("G");
        s[2] = cfg.slot_index("B"); s[3] = cfg.slot_index("L");
    } else if (k.kind == AccumulateKind::THREE_PLANE) {
        s[0] = cfg.slot_index("R_HaO3"); s[1] = cfg.slot_index("G_HaO3");
        s[2] = cfg.slot_index("B_HaO3");
    } else {
        s[0] = cfg.slot_index("L");
    }
}

} // namespace

TEST_CASE("accumulate_sample: an uncovered sample changes nothing; the first covered one seeds the histogram",
          "[phase_a][accumulate]") {
    WelfordAccumulator w;
    PixelHistogram h;
    accumulate_sample(w, h, 0.7f, false);
    REQUIRE(w.count() == 0);
    REQUIRE(h.total_count() == 0);
    REQUIRE(h.range_min == 0.0f);   // still the default range

    accumulate_sample(w, h, 0.4f, true);
    REQUIRE(w.count() == 1);
    REQUIRE(w.mean == 0.4f);
    REQUIRE(h.total_count() == 1);
    // initialize_range(0.3, 0.5) widens by 5% of the 0.2 span either side.
    REQUIRE(h.range_min == Catch::Approx(0.29f));
    REQUIRE(h.range_max == Catch::Approx(0.51f));

    accumulate_sample(w, h, 0.45f, true);
    REQUIRE(w.count() == 2);
    REQUIRE(h.total_count() == 2);
    REQUIRE(h.range_min == Catch::Approx(0.29f));   // not re-seeded
}

TEST_CASE("accumulate_frame: CubeSink and RamSink both match the serial loop bit for bit",
          "[phase_a][accumulate]") {
    const int w = 37, h = 29;   // odd sizes: no row or chunk lines up
    for (const Case& k : kCases) {
        for (bool empty_mask : {false, true}) {
            DYNAMIC_SECTION(k.name << (empty_mask ? " cloned" : " warped")) {
                const ChannelConfig cfg = cfg_for(k.cls, k.name);
                int s[4];
                slots_for(k, cfg, s);
                Cube ref(w, h, cfg), direct(w, h, cfg), backed(w, h, cfg);
                RunningAccumulators ram(w, h, cfg.n_channels);

                std::mt19937 rng(1234);
                for (int f = 0; f < 9; f++) {
                    Frame fr = random_frame(w, h, k.planes, rng, empty_mask);
                    reference_accumulate(ref, fr.img, fr.cov, k.kind, s);
                    accumulate_frame(CubeSink{direct}, fr.img, fr.cov, w, h, k.kind, s);
                    accumulate_frame(RamSink{ram}, fr.img, fr.cov, w, h, k.kind, s);
                }
                ram.store_into(backed);
                require_same_accumulators(ref, direct, cfg.n_channels);
                require_same_accumulators(ref, backed, cfg.n_channels);
                // Sanity: the mask really did leave samples out somewhere.
                if (!empty_mask) {
                    long short_counts = 0;
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++)
                            if (ref.at(x, y).channel(s[0]).welford.count() < 9) ++short_counts;
                    REQUIRE(short_counts > 0);
                }
            }
        }
    }
}

TEST_CASE("RunningAccumulators::store_into fills a FILE-BACKED cube exactly",
          "[phase_a][accumulate][file_backed]") {
#if defined(_WIN32)
    SKIP("Cube file backing is not implemented on Windows");
#else
    const int w = 41, h = 23;
    const ChannelConfig cfg = cfg_for(FilterClass::BROADBAND_OSC, "OSC");
    const int s[4] = {cfg.slot_index("R"), cfg.slot_index("G"),
                      cfg.slot_index("B"), cfg.slot_index("L")};
    const fs::path dir = fs::temp_directory_path();
    Cube ref(w, h, cfg);
    Cube backed(w, h, cfg, dir.string());
    REQUIRE(backed.file_backed());
    RunningAccumulators ram(w, h, cfg.n_channels);
    REQUIRE(ram.bytes_allocated() ==
            static_cast<std::size_t>(w) * h * (4 * sizeof(RunningAccumulators::Channel) + 2));

    std::mt19937 rng(99);
    for (int f = 0; f < 6; f++) {
        Frame fr = random_frame(w, h, 3, rng, false);
        reference_accumulate(ref, fr.img, fr.cov, AccumulateKind::OSC_WITH_LUMA, s);
        accumulate_frame(RamSink{ram}, fr.img, fr.cov, w, h, AccumulateKind::OSC_WITH_LUMA, s);
    }
    // Until store_into, the file-backed record is untouched by Phase A.
    REQUIRE(backed.at(5, 5).n_frames == 0);
    REQUIRE(backed.at(5, 5).channel(0).welford.count() == 0);

    ram.store_into(backed);
    require_same_accumulators(ref, backed, cfg.n_channels);

    ram.release();
    REQUIRE(ram.empty());
    REQUIRE(ram.bytes_allocated() == 0);
#endif
}

TEST_CASE("RunningAccumulators::store_into refuses a cube of another shape",
          "[phase_a][accumulate]") {
    const ChannelConfig osc = cfg_for(FilterClass::BROADBAND_OSC, "OSC");
    const ChannelConfig mono = cfg_for(FilterClass::BROADBAND_L, "L");
    RunningAccumulators ram(8, 6, osc.n_channels);
    Cube wrong_size(8, 7, osc);
    Cube too_few_channels(8, 6, mono);
    REQUIRE_THROWS_AS(ram.store_into(wrong_size), std::invalid_argument);
    REQUIRE_THROWS_AS(ram.store_into(too_few_channels), std::invalid_argument);
}

namespace {
struct FlagObserver : ProgressObserver {
    bool flag = false;
    void begin_phase(const std::string&, int) override {}
    void advance(int, const std::string&) override {}
    void end_phase() override {}
    void message(const std::string&) override {}
    bool is_cancelled() const override { return flag; }
};
} // namespace

TEST_CASE("store_into constructs a DEFERRED file-backed record in the same pass, exactly",
          "[phase_a][accumulate][file_backed]") {
#if defined(_WIN32)
    SKIP("Cube file backing is not implemented on Windows");
#else
    const int w = 43, h = 31;
    const ChannelConfig cfg = cfg_for(FilterClass::BROADBAND_OSC, "OSC");
    const int s[4] = {cfg.slot_index("R"), cfg.slot_index("G"),
                      cfg.slot_index("B"), cfg.slot_index("L")};
    Cube ref(w, h, cfg);   // constructed now, accumulated the old way
    Cube deferred(w, h, cfg, fs::temp_directory_path().string(), Cube::Construction::DEFERRED);
    REQUIRE(deferred.file_backed());
    REQUIRE_FALSE(deferred.constructed());
    RunningAccumulators ram(w, h, cfg.n_channels);
    std::mt19937 rng(17);
    for (int f = 0; f < 5; f++) {
        Frame fr = random_frame(w, h, 3, rng, false);
        reference_accumulate(ref, fr.img, fr.cov, AccumulateKind::OSC_WITH_LUMA, s);
        accumulate_frame(RamSink{ram}, fr.img, fr.cov, w, h, AccumulateKind::OSC_WITH_LUMA, s);
    }
    ram.store_into(deferred);
    REQUIRE(deferred.constructed());
    require_same_accumulators(ref, deferred, cfg.n_channels);
    // Everything Phase A does not write is exactly what construction gives.
    long bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const auto& a = ref.at(x, y);
            const auto& b = deferred.at(x, y);
            if (b.n_channels != a.n_channels || b.flags != a.flags
                || b.dominant_shape != a.dominant_shape
                || b.quality_score != a.quality_score || b.local_rms != a.local_rms
                || b.cloud_frame_count != a.cloud_frame_count) ++bad;
            for (int c = 0; c < cfg.n_channels; c++)
                if (b.channel(c).distribution.shape != a.channel(c).distribution.shape
                    || b.channel(c).snr != a.channel(c).snr
                    || b.channel(c).mad != a.channel(c).mad) ++bad;
        }
    REQUIRE(bad == 0);
#endif
}

TEST_CASE("store_into stops when cancelled, leaving a DEFERRED record unconstructed",
          "[phase_a][accumulate][cancel]") {
#if defined(_WIN32)
    SKIP("Cube file backing is not implemented on Windows");
#else
    const ChannelConfig cfg = cfg_for(FilterClass::BROADBAND_L, "L");
    Cube deferred(64, 600, cfg, fs::temp_directory_path().string(), Cube::Construction::DEFERRED);
    RunningAccumulators ram(64, 600, cfg.n_channels);
    FlagObserver obs;
    obs.flag = true;
    CancelPoll cancel(obs);
    ram.store_into(deferred, &cancel);
    REQUIRE(cancel.cancelled());
    REQUIRE_FALSE(deferred.constructed());
    // Construct now and the record is whole, so nothing was half-built.
    deferred.construct();
    REQUIRE(deferred.constructed());
    REQUIRE(deferred.at(10, 500).n_channels == 1);
#endif
}
