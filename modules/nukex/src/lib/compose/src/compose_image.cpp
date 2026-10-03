#include "nukex/compose/compose_image.hpp"
#include <algorithm>
#include <cmath>

namespace nukex {

bool slots_have_colour(
    const std::unordered_map<std::string, std::vector<float>>& slots) {
    for (const char* name : {"R", "G", "B", "Ha", "OIII", "SII"})
        if (slots.find(name) != slots.end()) return true;
    return false;
}

Image compose_slots_to_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    ColorComposer& composer,
    const Image* gate_plane) {

    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || slots.empty() || n == 0) return Image{};

    // Resolve every slot pointer ONCE. Looking a name up per pixel would
    // re-hash the map seven times for each of 24 million pixels.
    auto plane = [&](const char* name) -> const float* {
        auto it = slots.find(name);
        if (it == slots.end() || it->second.size() < n) return nullptr;
        return it->second.data();
    };
    const float* L    = plane("L");
    const float* R    = plane("R");
    const float* G    = plane("G");
    const float* B    = plane("B");
    const float* Ha   = plane("Ha");
    const float* OIII = plane("OIII");
    const float* SII  = plane("SII");

    if (!L && !R && !G && !B && !Ha && !OIII && !SII) return Image{};

    const bool gate_ok = gate_plane && !gate_plane->empty() && gate_plane->n_channels() == 1
                      && gate_plane->width() == width && gate_plane->height() == height;
    const float* gate = gate_ok ? gate_plane->channel_data(0) : nullptr;

    Image out(width, height, 3);
    float* dst_r = out.channel_data(0);
    float* dst_g = out.channel_data(1);
    float* dst_b = out.channel_data(2);

    for (std::size_t p = 0; p < n; ++p) {
        DerivedSlots ds;
        if (L)    ds.L    = static_cast<double>(L[p]);
        if (R)    ds.R    = static_cast<double>(R[p]);
        if (G)    ds.G    = static_cast<double>(G[p]);
        if (B)    ds.B    = static_cast<double>(B[p]);
        if (Ha)   ds.Ha   = static_cast<double>(Ha[p]);
        if (OIII) ds.OIII = static_cast<double>(OIII[p]);
        if (SII)  ds.SII  = static_cast<double>(SII[p]);
        const sRGBPixel c = gate_ok
            ? composer.map_to_srgb(composer.compose_lab(ds, static_cast<double>(gate[p])))
            : composer.compose_pixel(ds);
        dst_r[p] = static_cast<float>(c.r);
        dst_g[p] = static_cast<float>(c.g);
        dst_b[p] = static_cast<float>(c.b);
    }
    return out;
}

namespace {

// Resolve every slot pointer ONCE; shared by the three composers below.
struct SlotPlanes {
    const float *L = nullptr, *R = nullptr, *G = nullptr, *B = nullptr;
    const float *Ha = nullptr, *OIII = nullptr, *SII = nullptr;
    bool any() const { return L || R || G || B || Ha || OIII || SII; }
    DerivedSlots at(std::size_t p) const {
        DerivedSlots ds;
        if (L)    ds.L    = static_cast<double>(L[p]);
        if (R)    ds.R    = static_cast<double>(R[p]);
        if (G)    ds.G    = static_cast<double>(G[p]);
        if (B)    ds.B    = static_cast<double>(B[p]);
        if (Ha)   ds.Ha   = static_cast<double>(Ha[p]);
        if (OIII) ds.OIII = static_cast<double>(OIII[p]);
        if (SII)  ds.SII  = static_cast<double>(SII[p]);
        return ds;
    }
};

SlotPlanes resolve(const std::unordered_map<std::string, std::vector<float>>& slots,
                   std::size_t n) {
    auto plane = [&](const char* name) -> const float* {
        auto it = slots.find(name);
        if (it == slots.end() || it->second.size() < n) return nullptr;
        return it->second.data();
    };
    SlotPlanes sp;
    sp.L = plane("L"); sp.R = plane("R"); sp.G = plane("G"); sp.B = plane("B");
    sp.Ha = plane("Ha"); sp.OIII = plane("OIII"); sp.SII = plane("SII");
    return sp;
}

} // namespace

Image compose_luminance_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots) {
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || slots.empty() || n == 0) return Image{};
    const SlotPlanes sp = resolve(slots, n);
    if (!sp.any()) return Image{};
    Image out(width, height, 1);
    float* dst = out.channel_data(0);
    for (std::size_t p = 0; p < n; ++p)
        dst[p] = static_cast<float>(ColorComposer::luminance_of(sp.at(p)));
    return out;
}

Image compose_slots_with_luminance(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    ColorComposer& composer,
    const Image& luminance,
    const Image* gate_plane) {
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || slots.empty() || n == 0) return Image{};
    if (luminance.empty() || luminance.n_channels() != 1 ||
        luminance.width() != width || luminance.height() != height) return Image{};
    const SlotPlanes sp = resolve(slots, n);
    if (!sp.any()) return Image{};
    const bool gate_ok = gate_plane && !gate_plane->empty() && gate_plane->n_channels() == 1
                      && gate_plane->width() == width && gate_plane->height() == height;
    const float* gate = gate_ok ? gate_plane->channel_data(0) : nullptr;

    Image out(width, height, 3);
    float* dst_r = out.channel_data(0);
    float* dst_g = out.channel_data(1);
    float* dst_b = out.channel_data(2);
    const float* lum = luminance.channel_data(0);
    for (std::size_t p = 0; p < n; ++p) {
        LabColor lab = gate_ok ? composer.compose_lab(sp.at(p), static_cast<double>(gate[p]))
                               : composer.compose_lab(sp.at(p));
        lab.L = ColorComposer::lab_L_from_luminance(static_cast<double>(lum[p]));
        const sRGBPixel c = composer.map_to_srgb(lab);
        dst_r[p] = static_cast<float>(c.r);
        dst_g[p] = static_cast<float>(c.g);
        dst_b[p] = static_cast<float>(c.b);
    }
    return out;
}

Image emission_total_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    const ColorComposer& composer) {
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || slots.empty() || n == 0) return Image{};
    const SlotPlanes sp = resolve(slots, n);
    if (!sp.Ha && !sp.OIII && !sp.SII) return Image{};
    const double sha = composer.line_background_ha(), so3 = composer.line_background_oiii(),
                 ss2 = composer.line_background_sii();
    Image out(width, height, 1);
    float* dst = out.channel_data(0);
    for (std::size_t p = 0; p < n; ++p) {
        double t = 0.0;
        if (sp.Ha)   t += static_cast<double>(sp.Ha[p])   - sha;
        if (sp.OIII) t += static_cast<double>(sp.OIII[p]) - so3;
        if (sp.SII)  t += static_cast<double>(sp.SII[p])  - ss2;
        dst[p] = static_cast<float>(t);
    }
    return out;
}

Image box_smooth(const Image& plane, int radius) {
    if (plane.empty() || plane.n_channels() != 1 || radius <= 0) return plane;
    const int w = plane.width(), h = plane.height();
    const float* src = plane.channel_data(0);
    std::vector<float> tmp(static_cast<std::size_t>(w) * h);
    // rows: running sum with clamped edges
    for (int y = 0; y < h; y++) {
        const float* row = src + static_cast<std::size_t>(y) * w;
        float* out = tmp.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; x++) {
            double acc = 0.0;
            for (int k = -radius; k <= radius; k++) {
                const int xx = std::min(w - 1, std::max(0, x + k));
                acc += row[xx];
            }
            out[x] = static_cast<float>(acc / (2 * radius + 1));
        }
    }
    Image res(w, h, 1);
    float* dst = res.channel_data(0);
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) {
            double acc = 0.0;
            for (int k = -radius; k <= radius; k++) {
                const int yy = std::min(h - 1, std::max(0, y + k));
                acc += tmp[static_cast<std::size_t>(yy) * w + x];
            }
            dst[static_cast<std::size_t>(y) * w + x] = static_cast<float>(acc / (2 * radius + 1));
        }
    }
    return res;
}

GateStats gate_statistics(const Image& plane) {
    GateStats g;
    if (plane.empty() || plane.n_channels() != 1) return g;
    const int w = plane.width(), h = plane.height();
    const float* d = plane.channel_data(0);
    const std::size_t n = static_cast<std::size_t>(w) * h;
    const std::size_t stride = std::max<std::size_t>(1, n / 400000);

    // Sky: the lower quartile of the plane.
    std::vector<float> v;
    v.reserve(n / stride + 1);
    for (std::size_t i = 0; i < n; i += stride) v.push_back(d[i]);
    if (v.size() < 64) return g;
    const std::size_t q = v.size() / 4;
    std::nth_element(v.begin(), v.begin() + q, v.end());
    g.sky = v[q];

    // Noise: lag-16 differences, each direction centred on its own median,
    // then the pooled MAD. Anything smooth cancels; the lag clears the
    // smoothing window and the pixel correlation of a resampled stack.
    constexpr int LAG = 16;
    if (w <= LAG || h <= LAG) return g;
    std::vector<float> dh, dv;
    dh.reserve(n / stride + 1); dv.reserve(n / stride + 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x + LAG < w; x += static_cast<int>(std::max<std::size_t>(1, stride / 4)))
            dh.push_back(d[static_cast<std::size_t>(y) * w + x + LAG] - d[static_cast<std::size_t>(y) * w + x]);
    for (int x = 0; x < w; x++)
        for (int y = 0; y + LAG < h; y += static_cast<int>(std::max<std::size_t>(1, stride / 4)))
            dv.push_back(d[static_cast<std::size_t>(y + LAG) * w + x] - d[static_cast<std::size_t>(y) * w + x]);
    auto median_of = [](std::vector<float>& a) {
        const std::size_t k = a.size() / 2;
        std::nth_element(a.begin(), a.begin() + k, a.end());
        return a[k];
    };
    if (dh.empty() || dv.empty()) return g;
    std::vector<float> th = dh, tv = dv;
    const float mh = median_of(th), mv = median_of(tv);
    std::vector<float> dev;
    dev.reserve(dh.size() + dv.size());
    for (float x : dh) dev.push_back(std::fabs(x - mh));
    for (float x : dv) dev.push_back(std::fabs(x - mv));
    const double mad = median_of(dev);
    g.sigma = 1.4826 * mad / std::sqrt(2.0);
    if (!(g.sigma > 0.0)) return g;

    // Sky: seed at the lower quartile, then an asymmetrically clipped median
    // -- keep [s - 3 sigma, s + 1 sigma] and re-take the median -- which
    // walks onto the sky's own peak. A fixed quantile cannot: with an object
    // over 70% of the frame the lower quartile sits a full sigma into the
    // sky's upper tail, and with an object over 90% it is object.
    for (int it = 0; it < 6; ++it) {
        std::vector<float> kept;
        kept.reserve(v.size());
        const float lo = static_cast<float>(g.sky - 3.0 * g.sigma);
        const float hi = static_cast<float>(g.sky + 1.0 * g.sigma);
        for (float x : v) if (x > lo && x < hi) kept.push_back(x);
        if (kept.size() < 32) break;
        const double next = median_of(kept);
        const bool done = std::fabs(next - g.sky) < 0.01 * g.sigma;
        g.sky = next;
        if (done) break;
    }
    g.start = g.sky + 3.0 * g.sigma;
    g.full  = g.sky + 6.0 * g.sigma;
    g.valid = true;
    return g;
}

} // namespace nukex
