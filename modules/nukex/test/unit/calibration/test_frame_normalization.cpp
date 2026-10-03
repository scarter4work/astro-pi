#include "catch_amalgamated.hpp"
#include "nukex/calibration/frame_normalization.hpp"

#include <cmath>
#include <limits>
#include <vector>

using namespace nukex;

// A frame's sky as the measuring pass reports it: a robust location and a
// robust scale.
static FrameSky sky(double loc, double scale) { return FrameSky{loc, scale, true}; }

TEST_CASE("normalization: a stable session is left alone", "[normalization]") {
    // THE property that matters most. Most sessions are stable, and on those
    // this must be an exact no-op -- if it is not, every existing stack moves
    // for no reason. Measured on bayer_rgb_m27_2023 the model race and a plain
    // robust estimator agree to 1.00x, i.e. that corpus has nothing to correct.
    std::vector<FrameSky> frames(20, sky(0.0278, 0.00090));
    const auto c = solve_frame_normalization(frames);
    REQUIRE(c.size() == frames.size());
    for (const auto& k : c) {
        REQUIRE(k.scale  == Catch::Approx(1.0).margin(1e-12));
        REQUIRE(k.offset == Catch::Approx(0.0).margin(1e-12));
    }
}

TEST_CASE("normalization: a transparency dip is scaled back up",
          "[normalization]") {
    // Cloud or haze: the sky level AND the signal both drop. Measured on the
    // user's M63, per-frame sky offsets run -0.8 to +3.2 sigma across a
    // session -- real variation the stacker currently sees as per-pixel
    // bimodality instead of a per-frame level.
    std::vector<FrameSky> frames(9, sky(0.0300, 0.00100));
    frames[4] = sky(0.0150, 0.00050);            // half the level and the scale
    const auto c = solve_frame_normalization(frames);

    // The majority is untouched...
    REQUIRE(c[0].scale  == Catch::Approx(1.0).margin(1e-9));
    REQUIRE(c[0].offset == Catch::Approx(0.0).margin(1e-9));
    // ...and the dim frame is brought onto the same level and spread.
    REQUIRE(c[4].scale == Catch::Approx(2.0).margin(1e-9));
    const double mapped_loc   = c[4].scale * 0.0150 + c[4].offset;
    const double mapped_upper = c[4].scale * (0.0150 + 0.00050) + c[4].offset;
    REQUIRE(mapped_loc   == Catch::Approx(0.0300).margin(1e-9));
    REQUIRE(mapped_upper == Catch::Approx(0.0300 + 0.00100).margin(1e-9));
}

TEST_CASE("normalization: a longer exposure is brought onto the same scale",
          "[normalization]") {
    // The user's own M63 batch mixes 120 s and 300 s frames: sky 2685 vs 6813
    // ADU, a ratio of 2.54 against the 2.50 exposure ratio at identical gain.
    // Nothing in the pipeline normalises for it today, so the per-voxel fit
    // sees a bimodal population and rejects the minority.
    std::vector<FrameSky> frames(10, sky(2685.0, 30.0));
    for (int i = 6; i < 10; ++i) frames[i] = sky(6813.0, 76.2);   // 300 s
    const auto c = solve_frame_normalization(frames);
    for (int i = 6; i < 10; ++i) {
        const double mapped = c[i].scale * 6813.0 + c[i].offset;
        INFO("frame " << i << " scale " << c[i].scale << " offset " << c[i].offset);
        REQUIRE(mapped == Catch::Approx(2685.0).margin(1e-6));
    }
}

TEST_CASE("normalization: one wild frame does not move the reference",
          "[normalization]") {
    // The reference must be robust. A single blown or fogged frame that
    // dragged the reference would rescale every good frame to match it.
    std::vector<FrameSky> frames(15, sky(0.0300, 0.00100));
    frames[7] = sky(0.9000, 0.05000);
    const auto c = solve_frame_normalization(frames);
    for (int i = 0; i < 15; ++i) {
        if (i == 7) continue;
        REQUIRE(c[i].scale  == Catch::Approx(1.0).margin(1e-9));
        REQUIRE(c[i].offset == Catch::Approx(0.0).margin(1e-9));
    }
}

TEST_CASE("normalization: unusable and degenerate frames are no-ops",
          "[normalization]") {
    std::vector<FrameSky> frames(5, sky(0.0300, 0.00100));
    frames[1].usable = false;
    frames[2] = sky(0.0300, 0.0);      // zero scale: nothing to measure
    const auto c = solve_frame_normalization(frames);
    for (int i : {1, 2}) {
        REQUIRE(c[i].scale  == Catch::Approx(1.0).margin(1e-12));
        REQUIRE(c[i].offset == Catch::Approx(0.0).margin(1e-12));
    }
}

// ─────────────────────────────────────────────────────────────────────
// measure_channel_sky — turning one channel of pixels into a FrameSky
// ─────────────────────────────────────────────────────────────────────

#include <random>

TEST_CASE("sky: a flat field reports its level and no spread",
          "[normalization][sky]") {
    // Degenerate but reachable: a fully clipped or synthetic frame. Scale 0
    // is the signal solve_frame_normalization uses to hand back the identity,
    // so it must be reported honestly rather than fudged to a small epsilon.
    std::vector<float> px(1000, 0.25f);
    const FrameSky s = measure_channel_sky(px.data(), 50, 20);
    REQUIRE(s.location == Catch::Approx(0.25).margin(1e-9));
    REQUIRE(s.scale    == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("sky: location and scale recover a known sky",
          "[normalization][sky]") {
    // MAD x 1.4826 is a consistent estimator of sigma for a Gaussian, which
    // is what a background-dominated astronomical frame is away from stars.
    std::mt19937 rng(12345);
    std::normal_distribution<double> nd(0.0300, 0.00100);
    std::vector<float> px(200000);
    for (auto& v : px) v = static_cast<float>(nd(rng));

    const FrameSky s = measure_channel_sky(px.data(), 500, 400);
    REQUIRE(s.location == Catch::Approx(0.0300).margin(2e-5));
    REQUIRE(s.scale    == Catch::Approx(0.00100).margin(3e-5));
    REQUIRE(s.usable);
}

TEST_CASE("sky: stars do not drag the sky", "[normalization][sky]") {
    // THE reason this is median/MAD and not mean/stddev. A frame is sky plus
    // a small bright minority; a mean-and-sigma estimator would read the
    // stars as level and the pipeline would normalise on how many stars a
    // frame happens to contain -- i.e. on the seeing -- instead of on its sky.
    std::mt19937 rng(999);
    std::normal_distribution<double> nd(0.0300, 0.00100);
    std::vector<float> px(200000);
    for (auto& v : px) v = static_cast<float>(nd(rng));
    // 2% of the frame is a star at 100x the sky.
    for (std::size_t i = 0; i < px.size(); i += 50) px[i] = 3.0f;

    const FrameSky s = measure_channel_sky(px.data(), 500, 400);

    // The claim is robustness, not immunity: a 2% bright minority does move a
    // median, by a fraction of a sigma. What matters is the CONTRAST with the
    // mean the naive estimator would have used on the same pixels.
    double sum = 0.0;
    for (double v : px) sum += v;
    const double naive_mean = sum / static_cast<double>(px.size());

    const double med_err  = std::abs(s.location   - 0.0300) / 0.00100;
    const double mean_err = std::abs(naive_mean   - 0.0300) / 0.00100;
    INFO("median off by " << med_err << " sigma, mean off by "
                          << mean_err << " sigma");
    REQUIRE(med_err  < 0.1);     // measured 0.022 sigma
    REQUIRE(mean_err > 20.0);    // measured 59 sigma -- the estimator we avoid

    // The scale is likewise robust rather than immune: 2% of pairs are
    // contaminated at 100x the sky and the estimate moves 4%. A sample
    // standard deviation over the same pixels lands near 0.42 -- roughly
    // 420x the true sigma.
    double s2 = 0.0;
    for (double v : px) s2 += (v - naive_mean) * (v - naive_mean);
    const double naive_sd = std::sqrt(s2 / double(px.size()));
    INFO("scale " << s.scale << " vs naive sd " << naive_sd);
    REQUIRE(s.scale  == Catch::Approx(0.00100).epsilon(0.10));
    REQUIRE(naive_sd > 100.0 * 0.00100);
}

TEST_CASE("sky: an empty channel is not usable", "[normalization][sky]") {
    const FrameSky s = measure_channel_sky(nullptr, 500, 400);
    REQUIRE_FALSE(s.usable);
    REQUIRE(s.scale == Catch::Approx(0.0).margin(1e-12));
}

TEST_CASE("sky: non-finite samples are ignored, not propagated",
          "[normalization][sky]") {
    // A NaN reaching the median would poison nth_element's ordering and the
    // reference for the whole batch with it. Flats divide, so NaN and Inf are
    // both reachable from a bad calibration frame.
    std::vector<float> px(1001, 0.25f);
    px[10]  = std::numeric_limits<float>::quiet_NaN();
    px[20]  = std::numeric_limits<float>::infinity();
    px[30]  = -std::numeric_limits<float>::infinity();
    const FrameSky s = measure_channel_sky(px.data(), 7, 143);
    REQUIRE(s.location == Catch::Approx(0.25).margin(1e-9));
    REQUIRE(s.usable);
}

// ─────────────────────────────────────────────────────────────────────
// mix_coefficients — the effective map for a SYNTHESIZED slot
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("mix: a synthesized slot inherits its inputs' normalisation",
          "[normalization][mix]") {
    // OSC's L slot is 0.299R + 0.587G + 0.114B, built in Phase A from planes
    // that have ALREADY been normalised. It must not be normalised a second
    // time -- but Phase B's noise model still needs to know what map was
    // effectively applied to it, to convert a sample back to raw ADU.
    const NormalizationCoefficients c[3] = {{1.10, -0.0030},
                                            {1.00,  0.0000},
                                            {0.80,  0.0060}};
    const double w[3] = {0.299, 0.587, 0.114};
    const auto m = mix_coefficients(c, w, 3);
    REQUIRE(m.scale  == Catch::Approx(0.299*1.10 + 0.587*1.00 + 0.114*0.80));
    REQUIRE(m.offset == Catch::Approx(0.299*-0.0030 + 0.114*0.0060));
}

TEST_CASE("mix: identical inputs mix to exactly that map",
          "[normalization][mix]") {
    // The case that must be exact, because it is the common one: when every
    // input plane got the same correction, the synthesized plane got it too,
    // and the mixture is not an approximation at all.
    const NormalizationCoefficients c[3] = {{1.25, -0.004},
                                            {1.25, -0.004},
                                            {1.25, -0.004}};
    const double w[3] = {0.299, 0.587, 0.114};
    const auto m = mix_coefficients(c, w, 3);
    // Exact, deliberately. rec709's weights sum to 0.9999999999999999, so
    // the long way round returns a scale that is not 1 -- and mixing three
    // IDENTITIES that way would move every OSC stack with nothing to correct.
    REQUIRE(m.scale  == 1.25);
    REQUIRE(m.offset == -0.004);
}

TEST_CASE("mix: three identities mix to the exact identity",
          "[normalization][mix]") {
    // The case that actually ships on a stable session, and the one a
    // weighted sum gets subtly wrong.
    const NormalizationCoefficients c[3] = {{1.0, 0.0}, {1.0, 0.0}, {1.0, 0.0}};
    const double w[3] = {0.299, 0.587, 0.114};
    const auto m = mix_coefficients(c, w, 3);
    REQUIRE(m.scale  == 1.0);
    REQUIRE(m.offset == 0.0);
}

TEST_CASE("sky: a smooth gradient does not inflate the scale",
          "[normalization][sky]") {
    // THE defect this estimator exists to avoid, and the reason the first
    // wiring of normalisation made a real stack WORSE rather than better.
    //
    // `scale` is what the solver divides by, so it has to be noise. MAD about
    // the median cannot tell noise from structure: on the user's own M27 2025
    // B frames the seven clouded exposures reported a MAD of 375-1159 where
    // their pixel noise was 120-169, so normalising on it crushed them by a
    // factor of eight and pixel noise in the stack rose 8%.
    //
    // Here: identical noise, one frame flat and one with a strong smooth
    // gradient across it. A correct scale estimator cannot tell them apart.
    std::mt19937 rng(4242);
    std::normal_distribution<double> nd(0.0, 0.00100);
    const int W = 500, H = 400;
    std::vector<float> flat(W * H), sloped(W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double noise = nd(rng);
            const std::size_t i = static_cast<std::size_t>(y) * W + x;
            flat[i]   = static_cast<float>(0.0300 + noise);
            // A gradient far larger than the noise: 0.03 -> 0.09 across the
            // frame, i.e. 60 sigma of structure.
            sloped[i] = static_cast<float>(0.0300 + noise
                                           + 0.0600 * (double(x) / W));
        }

    const FrameSky a = measure_channel_sky(flat.data(),   W, H);
    const FrameSky b = measure_channel_sky(sloped.data(), W, H);

    INFO("flat scale " << a.scale << ", sloped scale " << b.scale);
    REQUIRE(a.scale == Catch::Approx(0.00100).margin(5e-5));
    // The gradient may not move the noise estimate by more than a few
    // percent. MAD about the median would report roughly 17x here.
    REQUIRE(b.scale == Catch::Approx(a.scale).epsilon(0.05));
}

TEST_CASE("sky: too few pairs reports no scale, not a guess",
          "[normalization][sky]") {
    // A degenerate strip has nothing to difference. Scale 0 routes it to the
    // identity, which is the honest answer; an invented spread would license
    // the solver to rescale a frame it never measured.
    std::vector<float> px(40, 0.9f);
    const FrameSky s = measure_channel_sky(px.data(), 1, 40);  // no neighbours
    REQUIRE(s.scale == Catch::Approx(0.0).margin(1e-12));
}

TEST_CASE("sky: screening on background would bias the scale",
          "[normalization][sky]") {
    // Pins why the estimate is taken over ALL adjacent pairs. Selecting pairs
    // whose members both fall below the median conditions the estimate on the
    // noise it is measuring and shrinks it -- and only when there is no
    // gradient, so two frames of identical noise disagree by 1.7x depending
    // on whether one of them is clouded. Unscreened, the same noise reads the
    // same either way; the case above proves the gradient half.
    std::mt19937 rng(777);
    std::normal_distribution<double> nd(0.0300, 0.00100);
    const int W = 500, H = 400;
    std::vector<float> px(static_cast<std::size_t>(W) * H);
    for (auto& v : px) v = static_cast<float>(nd(rng));
    const FrameSky s = measure_channel_sky(px.data(), W, H);
    REQUIRE(s.scale == Catch::Approx(0.00100).epsilon(0.03));
}
