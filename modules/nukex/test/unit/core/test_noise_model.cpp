#include "catch_amalgamated.hpp"
#include "nukex/core/noise_model.hpp"
#include <cmath>

using namespace nukex;

namespace {
FrameStats make_frame(float rn, float g, bool has_kw = true) {
    FrameStats fs;
    fs.read_noise = rn;
    fs.gain = g;
    fs.has_noise_keywords = has_kw;
    fs.frame_weight = 1.0f;
    fs.psf_weight = 1.0f;
    fs.median_luminance = 0.5f;
    return fs;
}
} // anonymous namespace

TEST_CASE("NoiseModel::sample_variance: CCD noise model in normalized units", "[core][noise]") {
    FrameStats fs = make_frame(3.0f, 1.5f);
    float var = NoiseModel::sample_variance(0.5f, fs, 0, 0.0f);
    // value_adu = 0.5 * 65535 = 32767.5
    // shot_var_adu = 32767.5 / 1.5 = 21845.0
    // read_var_adu = 9 / 2.25 = 4.0
    // total in normalized^2 = 21849 / 65535^2 ≈ 5.087e-6
    float expected = 21849.0f / (65535.0f * 65535.0f);
    REQUIRE(var == Catch::Approx(expected).margin(1e-8f));
}

TEST_CASE("NoiseModel::sample_variance: no keywords -> the caller's fallback variance",
          "[core][noise]") {
    FrameStats fs = make_frame(3.0f, 1.5f, false);
    float var = NoiseModel::sample_variance(0.5f, fs, 0, 0.02f);
    REQUIRE(var == Catch::Approx(0.02f));
}

TEST_CASE("NoiseModel::sample_variance: the identity normalisation is "
          "bit-for-bit the un-normalised model", "[core][noise][normalization]") {
    // The whole no-op argument rests on this. Every batch that needs no
    // correction carries scale 1 / offset 0, and (v - 0)/1 and 1*1*x are
    // exact in IEEE-754 -- so this is an equality, not an approximation. If
    // it ever becomes approximate, every existing golden moves.
    FrameStats fs; fs.has_noise_keywords = true; fs.gain = 1.5f; fs.read_noise = 3.2f;
    FrameStats plain = fs;   // norm_* default to the identity

    const float got = NoiseModel::sample_variance(0.5f, fs, 0, 0.0f);
    const float value_adu = 0.5f * 65535.0f;
    const float expect = (value_adu / 1.5f + (3.2f * 3.2f) / (1.5f * 1.5f))
                       / (65535.0f * 65535.0f);
    REQUIRE(got == expect);          // exact, deliberately not Approx
    REQUIRE(got == NoiseModel::sample_variance(0.5f, plain, 3, 0.0f));
}

TEST_CASE("NoiseModel::sample_variance: shot noise is read on the raw value, "
          "not the normalised one", "[core][noise][normalization]") {
    // A frame scaled up by 2 has had its noise scaled up by 2 as well, so its
    // variance is 4x -- NOT the variance of a genuinely brighter exposure,
    // which would only be 2x by Poisson. Reading the normalised value as ADU
    // would report the latter and quietly over-trust every rescaled frame.
    FrameStats fs; fs.has_noise_keywords = true; fs.gain = 1.5f; fs.read_noise = 3.2f;
    fs.norm_scale[1] = 2.0f; fs.norm_offset[1] = 0.0f;

    const float raw  = 0.25f;
    const float seen = 2.0f * raw;                    // what Phase A cached
    const float base = NoiseModel::sample_variance(raw, FrameStats{fs}, 0, 0.0f);
    const float got  = NoiseModel::sample_variance(seen, fs, 1, 0.0f);
    REQUIRE(got == Catch::Approx(4.0f * base).epsilon(1e-6));

    // The wrong answer, for contrast: treating `seen` as raw ADU.
    const float naive = NoiseModel::sample_variance(seen, FrameStats{fs}, 0, 0.0f);
    REQUIRE(naive < got);
}

TEST_CASE("NoiseModel::sample_variance: an additive offset is removed before "
          "the Poisson term", "[core][noise][normalization]") {
    // Sky pedestal differences are the additive half of normalisation. The
    // Poisson term is a function of collected photons, so a frame lifted by
    // an offset must not be charged shot noise for the lift.
    FrameStats fs; fs.has_noise_keywords = true; fs.gain = 1.5f; fs.read_noise = 3.2f;
    fs.norm_scale[0] = 1.0f; fs.norm_offset[0] = 0.10f;

    const float raw = 0.20f;
    const float got = NoiseModel::sample_variance(raw + 0.10f, fs, 0, 0.0f);
    FrameStats id;  id.has_noise_keywords = true; id.gain = 1.5f; id.read_noise = 3.2f;
    // Approx, not equality: (raw + 0.10f) - 0.10f is not raw in binary
    // floating point. Bit-exactness is claimed only for the identity, which
    // its own case above asserts.
    REQUIRE(got == Catch::Approx(NoiseModel::sample_variance(raw, id, 0, 0.0f))
                       .epsilon(1e-6));
}

TEST_CASE("NoiseModel::sample_variance: an unmeasurable scale falls back to "
          "the identity", "[core][noise][normalization]") {
    // scale <= 0 is how the solver reports "could not measure this frame".
    // Dividing by it would be division by noise, or by zero.
    FrameStats fs; fs.has_noise_keywords = true; fs.gain = 1.5f; fs.read_noise = 3.2f;
    FrameStats id = fs;
    fs.norm_scale[2] = 0.0f; fs.norm_offset[2] = 0.5f;
    REQUIRE(NoiseModel::sample_variance(0.5f, fs, 2, 0.0f)
            == NoiseModel::sample_variance(0.5f, id, 0, 0.0f));
}
