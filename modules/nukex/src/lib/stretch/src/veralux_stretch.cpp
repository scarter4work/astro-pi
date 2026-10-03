#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include <limits>

namespace nukex {

namespace {

/// The HMS core curve, normalised to [0,1] over [SP,1].
///   f(x) = [asinh(D*(x-SP)+b) - asinh(b)] / [asinh(D*(1-SP)+b) - asinh(b)]
/// `den` is the denominator, hoisted because apply() evaluates this 72
/// million times on a 24 MP colour frame and it does not vary per pixel.
inline float hms_curve(float x, float D, float b, float SP, float den) {
    if (x <= SP) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return std::clamp((std::asinh(D * (x - SP) + b) - std::asinh(b)) / den,
                      0.0f, 1.0f);
}

inline float hms_den(float D, float b, float SP) {
    return std::asinh(D * (1.0f - SP) + b) - std::asinh(b);
}

/// The largest fraction of a frame the auto-solved shadow point may send to
/// black. Chosen from two independent measurements: it is above the 0.256% a
/// NORMAL background loses at -2.8 sigma, so the STF convention still governs
/// flat data (the break-even is 0.25%), and on the user's 114-frame M63 it
/// takes the largest clipped clump from 231 pixels to 42 and leaves none at
/// all above 50. The contrast it costs is nothing: SP moves 0.026725 ->
/// 0.026607 out of a range approaching 1.
constexpr double kMaxShadowClip = 0.005;

/// The largest fraction of any ONE COLOUR CHANNEL the shadow point may crush.
///
/// Far tighter than kMaxShadowClip, and it has to be. apply() runs the curve
/// on each channel separately, and hms_curve returns 0 below SP, so a pixel
/// crushed in R and B but not G is not "dark" -- it is SATURATED GREEN. Noise
/// is independent per channel, so any hard threshold produces some of it; the
/// only defence is to put the threshold where essentially nothing crosses it.
///
/// Measured on the user's 114-frame M63 with a shadow point taken from the
/// LUMINANCE: because luminance is a weighted average its noise is lower than
/// any single channel's, and G carries weight 0.7152 so L tracks G while R and
/// B diverge. R was crushed on 1.53% of pixels, B on 1.79%, G on only 0.074%
/// -- 85,229 pixels reading exactly (0, 62, 0) in 8-bit, scattered frame-wide.
constexpr double kMaxChannelClip = 1.0e-4;

} // namespace

float VeraLuxStretch::apply_scalar(float x) const {
    const float D = std::pow(10.0f, log_D);
    const float den = hms_den(D, protect_b, SP);
    if (den < 1e-10f) return x;  // Degenerate case
    return hms_curve(x, D, protect_b, SP, den);
}

float VeraLuxStretch::auto_tune(const Image& img, float target_background) {
    // Robust background of the quantity apply() actually stretches: the
    // sensor-weighted luminance for a colour image, channel 0 for a mono one.
    // Tuning against a single channel instead is not a rounding difference --
    // on the user's own stack the background is imbalanced (red 0.0270, green
    // 0.0418), so a shadow point taken from green sits above the luminance of
    // 99.6% of the frame and blacks it out. Under the old SP = 0 curve the
    // same mismatch was a quiet bias: it is why that stretch put its
    // background at 0.2569 while solving for 0.2500.
    //
    // Sampled on a stride -- a full sort of a 24 MP frame costs more than the
    // whole stretch, to produce one number.
    const int n = img.width() * img.height();
    if (n <= 0) return log_D;
    const bool colour = img.n_channels() >= 3;
    const float* c0 = img.channel_data(0);
    const float* c1 = colour ? img.channel_data(1) : nullptr;
    const float* c2 = colour ? img.channel_data(2) : nullptr;

    const int stride = std::max(1, n / 200000);
    std::vector<float> sample;
    sample.reserve(static_cast<std::size_t>(n / stride) + 1);
    for (int i = 0; i < n; i += stride)
        sample.push_back(colour ? (w_R * c0[i] + w_G * c1[i] + w_B * c2[i])
                                : c0[i]);
    if (sample.empty()) return log_D;

    // The per-channel background too. apply() stretches each channel with the
    // curve and mixes the RESULTS, and the curve is concave, so the mean of
    // the stretched channels is not the stretched mean -- on an imbalanced
    // background they differ by a third of the output range. Solving against
    // hms(L) alone would put the background wherever Jensen's inequality
    // happened to leave it. Tune what apply() actually produces.
    float bg_ch[3] = {0.0f, 0.0f, 0.0f};
    // The lowest shadow point any channel can tolerate. See kMaxChannelClip.
    float channel_floor = std::numeric_limits<float>::max();
    if (colour) {
        const float* ch[3] = {c0, c1, c2};
        std::vector<float> cs;
        cs.reserve(sample.size());
        for (int k = 0; k < 3; ++k) {
            cs.clear();
            for (int i = 0; i < n; i += stride) cs.push_back(ch[k][i]);
            const std::size_t m = cs.size() / 2;
            std::nth_element(cs.begin(), cs.begin() + m, cs.end());
            bg_ch[k] = cs[m];

            // This channel's own tail: the level below which at most
            // kMaxChannelClip of it sits.
            const std::size_t q = static_cast<std::size_t>(
                kMaxChannelClip * static_cast<double>(cs.size() - 1));
            std::nth_element(cs.begin(), cs.begin() + q, cs.begin() + m);
            channel_floor = std::min(channel_floor, cs[q]);

            // And its own noise, so the STF convention is judged per channel
            // rather than against the quieter luminance.
            for (float& v : cs) v = std::abs(v - bg_ch[k]);
            std::nth_element(cs.begin(), cs.begin() + m, cs.end());
            const float sig_k = 1.4826f * cs[m];
            if (sig_k > 0.0f)
                channel_floor = std::min(channel_floor, bg_ch[k] - 2.8f * sig_k);
        }
    }

    const std::size_t mid = sample.size() / 2;
    std::nth_element(sample.begin(), sample.begin() + mid, sample.end());
    const float bg = sample[mid];
    if (!(bg > 0.0f) || bg >= 1.0f) return log_D;

    // Solve the SHADOW POINT as well as the intensity. Positioning and
    // contrast are separate problems: solving the intensity MOVES the
    // histogram, only a shadow point WIDENS it. Measured on a 74-frame stack
    // whose p99.9 sits 44 sigma above the background, SP = 0 delivered that
    // signal as 4% of the output range (p50 0.2569 -> p99.9 0.2969) because
    // the whole band rides on a pedestal that eats 98% of the curve. Solving
    // both took the spread from 0.0393 to 0.2853 with the background still
    // exactly on target.
    //
    // median - 2.8 sigma is the convention PixInsight's own STF autostretch
    // uses, with sigma from the MAD (x1.4826 for consistency with a normal)
    // so a bright nebula cannot inflate the noise estimate.
    //
    // But that convention assumes a FLAT background, and a deep stack has no
    // such thing. On the user's 114-frame M63 the sigma collapses to 0.000351,
    // which puts median - 2.8 sigma only 0.4% below the median -- inside the
    // frame's own vignetting -- and 1.437% of the SKY went to pure black in
    // clumps up to 231 pixels wide. So bound how much may be clipped: take
    // whichever of the two candidates is lower. The bound sits above the
    // 0.256% a normal background loses at -2.8 sigma (its 0.25th percentile is
    // at exactly 2.807 sigma, the break-even), so on flat data the convention
    // still governs and only a fat, structured low tail can pull SP down.
    //
    // Taken here, while `sample` still holds levels rather than deviations.
    // [begin, mid) already holds the mid smallest after the median's
    // nth_element, so the percentile only has to partition that half.
    const std::size_t k =
        static_cast<std::size_t>(kMaxShadowClip * static_cast<double>(sample.size() - 1));
    std::nth_element(sample.begin(), sample.begin() + k, sample.begin() + mid);
    const float clip_bound = sample[k];

    for (float& v : sample) v = std::abs(v - bg);
    std::nth_element(sample.begin(), sample.begin() + mid, sample.end());
    const float sigma = 1.4826f * sample[mid];

    const float saved_SP = SP;
    const float saved    = log_D;
    SP = (sigma > 0.0f)
             ? std::max(0.0f, std::min(bg - 2.8f * sigma, clip_bound))
             : 0.0f;
    // Never above what the noisiest colour channel can survive.
    if (colour && channel_floor < SP) SP = std::max(0.0f, channel_floor);

    // The luminance apply() will actually emit for the background, given the
    // current SP and log_D. Mono is the plain curve; colour mirrors the
    // per-channel blend exactly.
    const auto background_out = [&]() -> float {
        const float Ls = apply_scalar(bg);
        if (!colour) return Ls;
        const float kk = std::pow(Ls, convergence_power);
        const float mixed = w_R * apply_scalar(bg_ch[0])
                          + w_G * apply_scalar(bg_ch[1])
                          + w_B * apply_scalar(bg_ch[2]);
        return mixed * (1.0f - kk) + Ls * kk;
    };

    // A band too tight to clip: if no intensity in range can still put the
    // background on target -- which happens when the noise floor is so narrow
    // that (bg - SP) underflows the curve -- clipping buys nothing and costs
    // the positioning. Fall back to the shadow point at black.
    log_D = 7.0f;
    if (background_out() < target_background) SP = 0.0f;

    // background_out is monotonically increasing in log_D at fixed input, so a
    // bisection is exact enough in a handful of steps.
    float lo = 0.0f, hi = 7.0f;
    for (int i = 0; i < 40; ++i) {
        const float mid_ld = 0.5f * (lo + hi);
        log_D = mid_ld;
        if (background_out() < target_background) lo = mid_ld; else hi = mid_ld;
    }
    log_D = 0.5f * (lo + hi);
    if (!std::isfinite(log_D)) log_D = saved;
    if (!std::isfinite(SP) || SP < 0.0f || SP >= 1.0f) SP = saved_SP;
    return log_D;
}

void VeraLuxStretch::apply(Image& img) const {
    int n = img.width() * img.height();
    int nch = img.n_channels();

    float D = std::pow(10.0f, log_D);
    float b = protect_b;
    float den = hms_den(D, b, SP);
    if (den < 1e-10f) return;

    if (nch < 3) {
        // Single channel: apply scalar stretch. Inlined against the hoisted
        // D/den rather than calling apply_scalar, which would recompute a pow
        // and two asinh for every one of 24 million pixels.
        float* data = img.channel_data(0);
        for (int i = 0; i < n; i++)
            data[i] = hms_curve(data[i], D, b, SP, den);
        clamp_image(img);
        return;
    }

    // Multi-channel: stretch each channel with the SAME curve, then converge
    // the brightest pixels toward neutral.
    //
    // This used to take each pixel's colour as the ratio of its TOTAL channel
    // values, r/L, and rescale that by the stretched luminance. It threw the
    // colour away. Astronomical signal rides on a sky pedestal far larger than
    // itself, and that pedestal is neutral -- v5.0.3.0 deliberately made it
    // neutral -- so r/L is ~1:1:1 however colourful the signal is. Measured on
    // a 156-frame OSC stack of M63: the linear stack carried signal saturation
    // 0.355 at R 1.000 G 0.997 B 0.721, agreeing with the user's own
    // PixInsight integration (0.337), and this loop delivered 0.059 at
    // 1.000 / 1.000 / 0.965. Grey, from data that was never grey.
    //
    // Subtracting the pedestal before taking the ratio is NOT the fix: the
    // denominator goes to zero at sky level, so background noise is amplified
    // into violent chroma speckle (measured 0.63 "saturation", almost all of
    // it noise). Stretching each channel independently has no division at all,
    // which is exactly what STF does and why STF images have colour.
    float* R = img.channel_data(0);
    float* G = img.channel_data(1);
    float* B = img.channel_data(2);

    constexpr float eps = 1e-9f;

    for (int i = 0; i < n; i++) {
        float r = R[i], g = G[i], bv = B[i];

        // Sensor-weighted luminance
        float L = w_R * r + w_G * g + w_B * bv;
        // At or below the shadow point the pixel is background: it must go to
        // black, not keep its linear value.
        if (L <= SP || L < eps) { R[i] = G[i] = B[i] = 0.0f; continue; }

        // The same curve, applied to each channel and to the luminance.
        float L_stretched = hms_curve(L,  D, b, SP, den);
        float r_stretched = hms_curve(r,  D, b, SP, den);
        float g_stretched = hms_curve(g,  D, b, SP, den);
        float b_stretched = hms_curve(bv, D, b, SP, den);

        // Convergence factor: bright pixels transition toward white
        // k approaches 1 for bright pixels, 0 for faint pixels
        float k = std::pow(L_stretched, convergence_power);

        // Reconstruct: blend each stretched channel toward the stretched
        // luminance, which is neutral. k -> 1 at a star core, so cores still
        // go white; k -> 0 over the faint signal, which keeps its colour.
        R[i] = std::clamp(r_stretched * (1.0f - k) + L_stretched * k, 0.0f, 1.0f);
        G[i] = std::clamp(g_stretched * (1.0f - k) + L_stretched * k, 0.0f, 1.0f);
        B[i] = std::clamp(b_stretched * (1.0f - k) + L_stretched * k, 0.0f, 1.0f);
    }
}

std::map<std::string, std::pair<float, float>> VeraLuxStretch::param_bounds() const {
    return {
        {"SP",                {0.0f,  1.0f}},
        {"log_D",             {0.0f,  7.0f}},
        {"protect_b",         {0.1f, 15.0f}},
        {"convergence_power", {1.0f, 10.0f}},
    };
}

bool VeraLuxStretch::set_param(const std::string& n, float v) {
    if (n == "SP")                { SP = v;                return true; }
    if (n == "log_D")             { log_D = v;             return true; }
    if (n == "protect_b")         { protect_b = v;         return true; }
    if (n == "convergence_power") { convergence_power = v; return true; }
    return false;
}

std::optional<float> VeraLuxStretch::get_param(const std::string& n) const {
    if (n == "SP")                return SP;
    if (n == "log_D")             return log_D;
    if (n == "protect_b")         return protect_b;
    if (n == "convergence_power") return convergence_power;
    return std::nullopt;
}

} // namespace nukex
