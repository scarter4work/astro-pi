// LRGB-mono: a batch of separate L / R / G / B mono filters.
//
// This configuration does not produce correct output today, and these cases
// pin that it fails loudly rather than quietly.
//
// Why it is broken. Every mono frame debayers to the same (W, H, 1) geometry,
// and FrameCache is keyed on geometry alone, so all four filters land in one
// cache file. Phase B then cannot read a given slot's own frames:
//
//   - The L slot reads that shared cache and fits a mixture of every filter's
//     samples.
//   - R, G and B get `cache = nullptr` (stacking_engine.cpp guards against
//     routing three slots at channel 0 of one cache) and Phase B fits a buffer
//     of zeros, which the code comments describe as a "welford-only" fallback
//     that does not in fact exist.
//
// On real data -- M27 2025, ATR585M, L24 R12 G12 B24 -- that produced one
// populated channel out of four and three exactly-zero ones: a solid blue
// frame that looked like a colour-balance problem and was not.
//
// Phase A is not the problem: the per-slot Welford accumulators are correct,
// which the routing test below shows. The gap is that Phase B's per-frame read
// path, its shadow buffers and its weight kernels all assume every channel
// shares one frame set. Separating them is an architectural change, not a
// cache key. Until it lands, a batch that would hit this refuses to run.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "synthetic_fits.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

struct FilterLevel { const char* filter; float value; };

std::vector<std::string> write_mono_batch(const std::string& tag,
                                          const std::vector<FilterLevel>& levels,
                                          int frames_each) {
    std::vector<std::string> paths;
    for (const auto& fl : levels) {
        for (int i = 0; i < frames_each; i++) {
            auto p = fs::temp_directory_path()
                   / ("lrgb_" + tag + "_" + fl.filter + std::to_string(i) + ".fits");
            test_util::write_synthetic_mono(p.string(), 16, 16, "ATR585M",
                                            fl.filter, fl.value);
            paths.push_back(p.string());
        }
    }
    return paths;
}

StackingEngine::Config test_config() {
    StackingEngine::Config cfg;
    cfg.qe_database_path =
        (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    return cfg;
}

const std::vector<FilterLevel> kLRGB = {
    { "L", 0.60f }, { "R", 0.20f }, { "G", 0.35f }, { "B", 0.50f },
};

} // namespace

TEST_CASE("LRGB-mono: a four-filter mono batch stacks every channel",
          "[integration][lrgb]") {
    // This batch used to be refused, and before that it produced ONE
    // populated channel and three exactly-zero ones -- an image that looked
    // like a colour-balance problem and was not. Every mono frame is
    // (W, H, 1), so a geometry-keyed cache collapsed all four filters into
    // one file and no slot could read only its own frames.
    auto paths = write_mono_batch("lrgb", kLRGB, 3);
    StackingEngine engine(test_config());
    auto result = engine.execute(paths, {}, nullptr);

    INFO("error: " << result.error);
    REQUIRE(result.ok);
    REQUIRE(result.cube != nullptr);
    REQUIRE(result.cube->channel_config.n_channels == 4);

    // Every frame here is identical and unshifted, so every frame covers every
    // pixel and the intersection trim must keep the whole field.
    REQUIRE_FALSE(result.trim.applied);

    // Each slot must carry its OWN filter's level, not another filter's and
    // not zero. These are the four values write_mono_batch wrote.
    //
    // Read the routing off the cube rather than the stacked image: these
    // fixtures are UNIFORM, so the whole frame is background, and the chroma
    // match that runs before the stack is emitted legitimately pulls R, G and
    // B onto the dimmest of them. The per-slot accumulator is the routing
    // truth and no colour balance touches it.
    for (const auto& fl : kLRGB) {
        const int idx = result.cube->channel_config.slot_index(fl.filter);
        INFO("slot " << fl.filter << " index " << idx);
        REQUIRE(idx >= 0);
        REQUIRE(result.cube->at(8, 8).channel(idx).welford.count() == 3);
        REQUIRE(result.cube->at(8, 8).channel(idx).welford.mean
                == Catch::Approx(fl.value).margin(0.02f));
    }

    // Luminance is not part of the chroma set, so it reaches the output at its
    // own level; the three colour slots land together on the dimmest of them.
    const auto slot_of = [&](const char* n) {
        return result.cube->channel_config.slot_index(n);
    };
    REQUIRE(result.stacked.at(8, 8, slot_of("L")) == Catch::Approx(0.60f).margin(0.02f));
    for (const char* n : {"R", "G", "B"})
        REQUIRE(result.stacked.at(8, 8, slot_of(n)) == Catch::Approx(0.20f).margin(0.02f));
}

TEST_CASE("LRGB-mono: filters with different frame counts do not borrow "
          "each other's frames", "[integration][lrgb]") {
    // The asymmetric case is the one that would expose a shared frame set:
    // if L's slot read R's cache it would see the wrong count as well as the
    // wrong values. This is exactly the shape of the real M27 2025 corpus
    // (L24 R12 G12 B24).
    std::vector<std::string> paths;
    for (const auto& p : write_mono_batch("asym_l", { { "L", 0.60f } }, 4))
        paths.push_back(p);
    for (const auto& p : write_mono_batch("asym_r", { { "R", 0.20f } }, 2))
        paths.push_back(p);

    StackingEngine engine(test_config());
    auto result = engine.execute(paths, {}, nullptr);

    INFO("error: " << result.error);
    REQUIRE(result.ok);
    REQUIRE(result.cube != nullptr);

    const int li = result.cube->channel_config.slot_index("L");
    const int ri = result.cube->channel_config.slot_index("R");
    REQUIRE(li >= 0);
    REQUIRE(ri >= 0);
    REQUIRE(result.stacked.at(8, 8, li) == Catch::Approx(0.60f).margin(0.02f));
    REQUIRE(result.stacked.at(8, 8, ri) == Catch::Approx(0.20f).margin(0.02f));
}

TEST_CASE("LRGB-mono: a single mono filter still stacks", "[integration][lrgb]") {
    // The guard must catch only the case it is for. One mono filter per batch
    // is the whole NGC7635 corpus and must be untouched.
    auto paths = write_mono_batch("single", { { "L", 0.60f } }, 3);
    StackingEngine engine(test_config());
    auto result = engine.execute(paths, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube != nullptr);
    REQUIRE(result.cube->channel_config.n_channels == 1);
    REQUIRE(result.stacked.at(8, 8, 0) == Catch::Approx(0.60f).margin(0.02f));
}

TEST_CASE("LRGB-mono: Phase A routes each filter into its own slot",
          "[integration][lrgb]") {
    // Phase A was always innocent -- the per-slot Welford accumulators were
    // correct even when Phase B could not read them. This pins that end to
    // end now that the read path works too.
    auto paths = write_mono_batch("routing", { { "L", 0.60f }, { "R", 0.20f } }, 2);
    StackingEngine engine(test_config());
    auto result = engine.execute(paths, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube != nullptr);
    REQUIRE(result.cube->channel_config.n_channels == 2);

    const auto& vox = result.cube->at(8, 8);
    const int li = result.cube->channel_config.slot_index("L");
    const int ri = result.cube->channel_config.slot_index("R");
    REQUIRE(vox.channel(li).welford.count() == 2);
    REQUIRE(vox.channel(ri).welford.count() == 2);
    REQUIRE(vox.channel(li).welford.mean == Catch::Approx(0.60f).margin(0.02f));
    REQUIRE(vox.channel(ri).welford.mean == Catch::Approx(0.20f).margin(0.02f));
}

TEST_CASE("LRGB-mono: the background match is reported for provenance",
          "[integration][lrgb]") {
    // What came off each channel is the sky level this session's filters and
    // light pollution put there. It reaches the FITS header so a user can see
    // it -- and add it back if they ever want the raw colour.
    auto paths = write_mono_batch("prov", kLRGB, 3);
    StackingEngine engine(test_config());
    auto result = engine.execute(paths, {}, nullptr);

    INFO("error: " << result.error);
    REQUIRE(result.ok);
    REQUIRE(result.background_match.size() == 3);

    std::map<std::string, float> got;
    for (const auto& [name, off] : result.background_match) got[name] = off;
    // R is the dimmest of the three, so it is the reference and loses nothing.
    REQUIRE(got.at("R") == Catch::Approx(0.00f).margin(0.02f));
    REQUIRE(got.at("G") == Catch::Approx(0.15f).margin(0.02f));
    REQUIRE(got.at("B") == Catch::Approx(0.30f).margin(0.02f));
    // Luminance is not a chroma slot and must not appear.
    REQUIRE(got.find("L") == got.end());
}

TEST_CASE("LRGB-mono: the composed slots and the stacked image agree",
          "[integration][lrgb]") {
    // Three windows come out of a run and they must not disagree about the
    // background. The derived slots feed the composed window and the stacked
    // image feeds the stretched one, so both are matched in the same place
    // over the same pixels. Matching them where the slots are BUILT would
    // measure across the thin, noisy rim that the coverage trim then removes,
    // and the two windows would part company by exactly that difference.
    auto paths = write_mono_batch("agree", kLRGB, 3);
    StackingEngine engine(test_config());
    auto result = engine.execute(paths, {}, nullptr);

    INFO("error: " << result.error);
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.derived.slots.empty());

    const int N = result.stacked.width() * result.stacked.height();
    REQUIRE(result.derived.width * result.derived.height == N);

    for (const char* nm : {"R", "G", "B"}) {
        auto it = result.derived.slots.find(nm);
        INFO("slot " << nm);
        REQUIRE(it != result.derived.slots.end());
        const int idx = result.cube->channel_config.slot_index(nm);
        REQUIRE(idx >= 0);
        // Same pixel, same value: both planes were matched together.
        REQUIRE(it->second[8 * result.derived.width + 8]
                == Catch::Approx(result.stacked.at(8, 8, idx)).margin(0.02f));
    }
}
