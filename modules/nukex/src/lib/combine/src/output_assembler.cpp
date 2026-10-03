#include "nukex/combine/output_assembler.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace nukex {

Image OutputAssembler::assemble_quality_map(const Cube& cube) {
    Image quality(cube.width, cube.height, 4);
    int n_ch = cube.channel_config.n_channels;

    for (int y = 0; y < cube.height; y++) {
        for (int x = 0; x < cube.width; x++) {
            const auto& v = cube.at(x, y);

            float avg_signal = 0.0f;
            float max_uncertainty = 0.0f;
            float min_confidence = 1.0f;

            for (int ch = 0; ch < n_ch; ch++) {
                const auto& dist = v.channel(ch).distribution;
                avg_signal += dist.true_signal_estimate;
                max_uncertainty = std::max(max_uncertainty, dist.signal_uncertainty);
                min_confidence = std::min(min_confidence, dist.confidence);
            }
            avg_signal /= n_ch;

            quality.at(x, y, 0) = avg_signal;
            quality.at(x, y, 1) = max_uncertainty;
            quality.at(x, y, 2) = min_confidence;
            quality.at(x, y, 3) = static_cast<float>(
                static_cast<uint8_t>(v.dominant_shape));
        }
    }

    return quality;
}

Image OutputAssembler::assemble_measured_noise(const Cube& cube) {
    Image measured(cube.width, cube.height, 1);
    for (int y = 0; y < cube.height; y++)
        for (int x = 0; x < cube.width; x++)
            measured.at(x, y, 0) = cube.at(x, y).local_rms;
    return measured;
}

OutputAssembler::NoiseCheck OutputAssembler::noise_check(const Image& measured,
                                                          const Image& predicted,
                                                          LuminanceSpec luminance) {
    NoiseCheck out;
    if (measured.empty() || predicted.empty()) return out;
    const LuminanceSpec lum = luminance.resolved(predicted.n_channels());
    if (lum.c0 >= predicted.n_channels() ||
        (lum.mode == LuminanceSpec::REC709 &&
         (lum.c1 >= predicted.n_channels() || lum.c2 >= predicted.n_channels()))) return out;

    auto median_positive = [](std::vector<float>& v) -> double {
        if (v.empty()) return 0.0;
        const std::size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + mid, v.end());
        return v[mid];
    };

    std::vector<float> mv;
    mv.reserve(static_cast<std::size_t>(measured.width()) * measured.height());
    for (int y = 0; y < measured.height(); y++)
        for (int x = 0; x < measured.width(); x++) {
            const float m = measured.at(x, y, 0);
            if (m > 0.0f && std::isfinite(m)) mv.push_back(m);
        }

    std::vector<float> pv;
    pv.reserve(static_cast<std::size_t>(predicted.width()) * predicted.height());
    for (int y = 0; y < predicted.height(); y++)
        for (int x = 0; x < predicted.width(); x++) {
            float p;
            if (lum.mode == LuminanceSpec::REC709) {
                // The kernel measures on a rec709 window of these planes; the
                // prediction combines the same planes in quadrature.
                const float wr = 0.2126f * predicted.at(x, y, lum.c0);
                const float wg = 0.7152f * predicted.at(x, y, lum.c1);
                const float wb = 0.0722f * predicted.at(x, y, lum.c2);
                p = std::sqrt(wr * wr + wg * wg + wb * wb);
            } else {
                p = predicted.at(x, y, lum.c0);
            }
            if (p > 0.0f && std::isfinite(p)) pv.push_back(p);
        }

    out.measured_median  = median_positive(mv);
    out.predicted_median = median_positive(pv);
    out.comparable = out.measured_median > 0.0 && out.predicted_median > 0.0;
    out.ratio = out.comparable ? out.measured_median / out.predicted_median : 0.0;
    return out;
}

double OutputAssembler::plane_noise_lag8(const Image& plane, int channel) {
    if (plane.empty() || channel < 0 || channel >= plane.n_channels()) return 0.0;
    const int w = plane.width(), h = plane.height();
    constexpr int LAG = 8;
    if (w <= LAG || h <= LAG) return 0.0;
    const float* d = plane.channel_data(channel);
    const std::size_t n = static_cast<std::size_t>(w) * h;
    const int step = static_cast<int>(std::max<std::size_t>(1, n / 2000000));   // ~2M pairs per direction at most
    std::vector<float> dh, dv;
    dh.reserve(n / step + 1); dv.reserve(n / step + 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x + LAG < w; x += step)
            dh.push_back(d[static_cast<std::size_t>(y) * w + x + LAG] - d[static_cast<std::size_t>(y) * w + x]);
    for (int x = 0; x < w; x++)
        for (int y = 0; y + LAG < h; y += step)
            dv.push_back(d[static_cast<std::size_t>(y + LAG) * w + x] - d[static_cast<std::size_t>(y) * w + x]);
    auto median_of = [](std::vector<float>& a) {
        const std::size_t k = a.size() / 2;
        std::nth_element(a.begin(), a.begin() + k, a.end());
        return a[k];
    };
    if (dh.size() < 16 || dv.size() < 16) return 0.0;
    std::vector<float> th = dh, tv = dv;
    const float mh = median_of(th), mv = median_of(tv);
    std::vector<float> dev;
    dev.reserve(dh.size() + dv.size());
    for (float x : dh) dev.push_back(std::fabs(x - mh));
    for (float x : dv) dev.push_back(std::fabs(x - mv));
    return 1.4826 * median_of(dev) / std::sqrt(2.0);
}

OutputAssembler::NoiseDecomposition OutputAssembler::noise_decomposition(
    double measured_median, const Image& half_even, const Image& half_odd,
    LuminanceSpec luminance) {
    NoiseDecomposition out;
    out.measured = measured_median;
    if (half_even.empty() || half_odd.empty() ||
        half_even.width() != half_odd.width() || half_even.height() != half_odd.height() ||
        half_even.n_channels() != half_odd.n_channels() || !(measured_median > 0.0)) return out;
    const int w = half_even.width(), h = half_even.height(), nc = half_even.n_channels();
    const LuminanceSpec lum = luminance.resolved(nc);
    if (lum.c0 >= nc || (lum.mode == LuminanceSpec::REC709 && (lum.c1 >= nc || lum.c2 >= nc))) return out;

    // The difference of the halves on the same luminance the kernel measured.
    Image diff(w, h, 1);
    float* dd = diff.channel_data(0);
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (lum.mode == LuminanceSpec::REC709) {
        const float *e0 = half_even.channel_data(lum.c0), *e1 = half_even.channel_data(lum.c1), *e2 = half_even.channel_data(lum.c2);
        const float *o0 = half_odd.channel_data(lum.c0),  *o1 = half_odd.channel_data(lum.c1),  *o2 = half_odd.channel_data(lum.c2);
        for (std::size_t i = 0; i < n; i++)
            dd[i] = 0.2126f * (e0[i] - o0[i]) + 0.7152f * (e1[i] - o1[i]) + 0.0722f * (e2[i] - o2[i]);
    } else {
        const float *e = half_even.channel_data(lum.c0), *o = half_odd.channel_data(lum.c0);
        for (std::size_t i = 0; i < n; i++) dd[i] = e[i] - o[i];
    }
    const double sigma_diff = plane_noise_lag8(diff, 0);
    if (!(sigma_diff > 0.0)) return out;
    // Each half has sqrt(2) x the full stack's stochastic noise; the difference
    // of two independent halves has sqrt(2) x a half's: 2 x the full stack's.
    out.stochastic  = sigma_diff / 2.0;
    const double fixed2 = measured_median * measured_median - out.stochastic * out.stochastic;
    out.fixed       = fixed2 > 0.0 ? std::sqrt(fixed2) : 0.0;
    out.fixed_share = fixed2 > 0.0 ? fixed2 / (measured_median * measured_median) : 0.0;
    out.valid = true;
    return out;
}

} // namespace nukex
