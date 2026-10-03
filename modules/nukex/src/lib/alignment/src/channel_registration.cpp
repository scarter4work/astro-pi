#include "nukex/alignment/channel_registration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace nukex {
namespace {

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

struct Centroid {
    double x  = 0.0;
    double y  = 0.0;
    bool   ok = false;
};

/// Intensity-weighted centroid of `ch` in a box around (sx, sy).
///
/// The background is the MEDIAN of the box border, not its minimum. This is
/// the single most important choice in this file and it was arrived at by
/// measurement, not by taste.
///
/// The minimum of a noisy ring is an extreme order statistic: it is biased
/// low, and by an amount that varies box to box. Subtracting too little
/// leaves a pedestal under the star, and a pedestal pulls an
/// intensity-weighted centroid toward the geometric centre of the box --
/// which is the rounded integer position, so the pull depends on the star's
/// sub-pixel phase and does not cancel between channels. Measured on a
/// synthetic field with realistic noise, worst error over the fit:
///
///     background:        min      median
///     equal brightness: 0.034      0.006  px
///     red at 1/5 flux:  0.217      0.057  px
///
/// A factor of four, and on the faint channel a factor of four again.
///
/// The median is also what makes the neighbour case survivable: a companion
/// intruding on part of the ring moves fewer than half its pixels.
Centroid centroid_at(const Image& img, int ch, float sx, float sy,
                     int radius, int iterations, float min_snr) {
    const int w = img.width();
    const int h = img.height();

    double px_ = sx, py_ = sy;
    std::vector<double> ring;
    ring.reserve(8 * radius);

    for (int pass = 0; pass < std::max(1, iterations); pass++) {
        const int icx = static_cast<int>(std::lround(px_));
        const int icy = static_cast<int>(std::lround(py_));

        if (icx - radius < 0 || icx + radius >= w ||
            icy - radius < 0 || icy + radius >= h) {
            return {};   // box does not fit; unusable in every channel
        }

        ring.clear();
        for (int d = -radius; d <= radius; d++) {
            ring.push_back(img.at(icx + d, icy - radius, ch));
            ring.push_back(img.at(icx + d, icy + radius, ch));
        }
        for (int d = -radius + 1; d <= radius - 1; d++) {
            ring.push_back(img.at(icx - radius, icy + d, ch));
            ring.push_back(img.at(icx + radius, icy + d, ch));
        }

        const double bg = median_of(ring);

        // Noise from the ring's own robust spread, so the SNR gate below does
        // not need a global noise estimate the caller would have to supply.
        std::vector<double> dev;
        dev.reserve(ring.size());
        for (double v : ring) dev.push_back(std::abs(v - bg));
        const double sigma = 1.4826 * median_of(dev);

        double wsum = 0.0, wx = 0.0, wy = 0.0, peak = 0.0;
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                const int qx = icx + dx, qy = icy + dy;
                const double v = static_cast<double>(img.at(qx, qy, ch)) - bg;
                if (v <= 0.0) continue;
                wsum += v;
                wx   += v * qx;
                wy   += v * qy;
                peak  = std::max(peak, v);
            }
        }

        if (wsum <= 0.0) return {};

        // Reject a star this channel cannot see. A noiseless synthetic field
        // has sigma exactly zero, so guard that case rather than letting the
        // comparison pass by accident.
        if (sigma > 0.0 && peak < min_snr * sigma) return {};
        if (sigma <= 0.0 && peak <= 0.0)           return {};

        px_ = wx / wsum;
        py_ = wy / wsum;
    }

    return { px_, py_, true };
}

struct Pair {
    double u = 0.0, v = 0.0;      // reference-channel position, centre-relative
    double up = 0.0, vp = 0.0;    // this channel's position, centre-relative
};

/// Closed-form least squares for u' = s*u + tx, v' = s*v + ty with a single
/// shared s. Minimising the summed squared error in both axes gives
///
///     s  = [Suu' + Svv'] / [Suu + Svv]        (about the means)
///     tx = mean(u') - s * mean(u)
///
/// which needs no matrix solver, so this file has no Eigen dependency.
ChannelTransform fit_affine(const std::vector<Pair>& p) {
    ChannelTransform t;
    const double n = static_cast<double>(p.size());

    double su = 0, sv = 0, sup = 0, svp = 0;
    for (const auto& q : p) { su += q.u; sv += q.v; sup += q.up; svp += q.vp; }
    const double mu = su / n, mv = sv / n, mup = sup / n, mvp = svp / n;

    double num = 0, den = 0;
    for (const auto& q : p) {
        num += (q.u - mu) * (q.up - mup) + (q.v - mv) * (q.vp - mvp);
        den += (q.u - mu) * (q.u  - mu)  + (q.v - mv) * (q.v  - mv);
    }

    // den is the spread of the stars about their own centroid, in pixels
    // squared summed over the stars. It vanishes only if every star sits at
    // one point, which the caller's star count makes impossible in practice,
    // but a division by it must still be safe.
    //
    // The magnitude is chosen against those units, not as a generic epsilon:
    // real inputs put den in the 1e4-1e8 range (hundreds of stars spread
    // over a frame thousands of pixels wide), so 1e-9 is some thirteen
    // orders of magnitude below anything legitimate. It rejects only a
    // genuinely degenerate configuration and can never fire on real data,
    // which is what a guard on a physically-impossible case should do.
    t.s  = (den > 1e-9) ? (num / den) : 1.0;
    t.tx = mup - t.s * mu;
    t.ty = mvp - t.s * mv;
    t.fit = ChannelTransform::Fit::Affine;
    return t;
}

ChannelTransform fit_translation(const std::vector<Pair>& p) {
    ChannelTransform t;
    const double n = static_cast<double>(p.size());
    double dx = 0, dy = 0;
    for (const auto& q : p) { dx += q.up - q.u; dy += q.vp - q.v; }
    t.s  = 1.0;
    t.tx = dx / n;
    t.ty = dy / n;
    t.fit = ChannelTransform::Fit::TranslationOnly;
    return t;
}

std::vector<double> residuals_of(const ChannelTransform& t,
                                 const std::vector<Pair>& p) {
    std::vector<double> r;
    r.reserve(p.size());
    for (const auto& q : p) {
        const double mx = t.s * q.u + t.tx;
        const double my = t.s * q.v + t.ty;
        r.push_back(std::hypot(q.up - mx, q.vp - my));
    }
    return r;
}

/// The fallback ladder. Every rung is named in the returned Fit so the console
/// can report which one a frame landed on.
///
///   enough stars       -> affine, sigma-clipped once
///   fewer than that    -> translation only
///   fewer still        -> identity
///   implausible result -> identity
///
/// Rejection is always to identity, never to a partial correction. A wrong
/// transform is worse than none: it moves every pixel of a channel.
ChannelTransform fit_channel(const std::vector<Pair>& pairs,
                             const ChannelRegistrationConfig& config) {
    ChannelTransform t;   // identity
    const int n = static_cast<int>(pairs.size());

    if (n < config.min_stars_translation) return t;

    auto plausible = [&](const ChannelTransform& c) {
        return std::abs(c.s - 1.0)  <= config.max_scale_deviation
            && std::abs(c.tx)       <= config.max_translation_px
            && std::abs(c.ty)       <= config.max_translation_px
            && std::isfinite(c.s) && std::isfinite(c.tx) && std::isfinite(c.ty);
    };

    if (n >= config.min_stars_affine) {
        ChannelTransform a = fit_affine(pairs);

        // One sigma-clip pass. A second buys nothing measurable and risks
        // eating real signal at the field edges, which is precisely where the
        // correction is largest and least redundant.
        std::vector<double> r = residuals_of(a, pairs);
        const double med = median_of(r);
        std::vector<double> dev;
        dev.reserve(r.size());
        for (double v : r) dev.push_back(std::abs(v - med));
        // 1.4826 * MAD estimates sigma for a normal distribution.
        const double sigma = 1.4826 * median_of(dev);

        if (sigma > 0.0) {
            const double cut = med + config.clip_sigma * sigma;
            std::vector<Pair> kept;
            kept.reserve(pairs.size());
            for (size_t i = 0; i < pairs.size(); i++)
                if (r[i] <= cut) kept.push_back(pairs[i]);

            if (static_cast<int>(kept.size()) >= config.min_stars_affine
                && kept.size() < pairs.size()) {
                a = fit_affine(kept);
                a.n_stars  = static_cast<int>(kept.size());
                a.residual = median_of(residuals_of(a, kept));
                return plausible(a) ? a : ChannelTransform{};
            }
        }

        a.n_stars  = n;
        a.residual = med;
        if (plausible(a)) return a;
        // An implausible affine fit does not earn a translation-only retry:
        // the same bad centroids feed it. Fall through to identity.
        return ChannelTransform{};
    }

    ChannelTransform tr = fit_translation(pairs);
    tr.n_stars  = n;
    tr.residual = median_of(residuals_of(tr, pairs));
    return plausible(tr) ? tr : ChannelTransform{};
}

} // namespace

ChannelTransforms measure_channel_transforms(
    const Image& image, const StarCatalog& stars, int reference_channel,
    const ChannelRegistrationConfig& config)
{
    ChannelTransforms out;

    const int nch = image.n_channels();
    if (nch < 2 || image.empty() || stars.empty()) return out;
    if (reference_channel < 0 || reference_channel >= nch) return out;

    out.cx = (image.width()  - 1) / 2.0;
    out.cy = (image.height() - 1) / 2.0;
    out.reference_channel = reference_channel;
    out.per_channel.assign(nch, ChannelTransform{});

    // Isolation. A neighbour inside the centroid box drags the centroid, and
    // by a different amount in each channel, so a crowded star is worse than
    // no star. StarDetector's exclusion_radius (5 by default) is smaller than
    // the centroid box, so it does NOT already guarantee this -- the filter
    // has to be here.
    //
    // O(n^2) over the catalog, which caps at max_stars (200 by default), so
    // 40000 comparisons per frame against tens of millions of pixels. Not
    // worth a spatial index.
    const double min_sep2 = static_cast<double>(config.min_neighbour_separation)
                          * config.min_neighbour_separation;
    std::vector<bool> isolated(stars.stars.size(), true);
    for (size_t i = 0; i < stars.stars.size(); i++) {
        for (size_t j = i + 1; j < stars.stars.size(); j++) {
            const double dx = stars.stars[i].x - stars.stars[j].x;
            const double dy = stars.stars[i].y - stars.stars[j].y;
            if (dx * dx + dy * dy < min_sep2) {
                isolated[i] = false;
                isolated[j] = false;
            }
        }
    }

    // Reference positions, centroided on the reference channel itself rather
    // than taken from the catalog. The catalog's centroids came from a
    // different estimator with a different aperture; using this one for both
    // sides means any bias it has cancels instead of leaking into the fit.
    struct RefPos { double x, y; bool ok; };
    std::vector<RefPos> ref(stars.stars.size());
    for (size_t i = 0; i < stars.stars.size(); i++) {
        if (!isolated[i]) { ref[i] = {0.0, 0.0, false}; continue; }
        const Star& s = stars.stars[i];
        const Centroid c = centroid_at(image, reference_channel, s.x, s.y,
                                       config.centroid_radius,
                                       config.centroid_iterations,
                                       config.min_star_snr);
        ref[i] = { c.x, c.y, c.ok };
    }

    for (int ch = 0; ch < nch; ch++) {
        if (ch == reference_channel) {
            out.per_channel[ch] = ChannelTransform{};   // identity by construction
            out.per_channel[ch].n_stars =
                static_cast<int>(std::count_if(ref.begin(), ref.end(),
                                               [](const RefPos& r) { return r.ok; }));
            continue;
        }

        std::vector<Pair> pairs;
        pairs.reserve(stars.stars.size());
        for (size_t i = 0; i < stars.stars.size(); i++) {
            if (!ref[i].ok) continue;
            const Star& s = stars.stars[i];
            const Centroid c = centroid_at(image, ch, s.x, s.y,
                                           config.centroid_radius,
                                           config.centroid_iterations,
                                           config.min_star_snr);
            if (!c.ok) continue;
            pairs.push_back({ ref[i].x - out.cx, ref[i].y - out.cy,
                              c.x      - out.cx, c.y      - out.cy });
        }

        out.per_channel[ch] = fit_channel(pairs, config);
    }

    return out;
}

std::string describe_channel_transforms(const ChannelTransforms& ct,
                                        double radius) {
    if (ct.empty()) return {};

    std::string out;
    for (size_t ch = 0; ch < ct.per_channel.size(); ch++) {
        if (static_cast<int>(ch) == ct.reference_channel) continue;
        const ChannelTransform& t = ct.per_channel[ch];

        char buf[192];
        switch (t.fit) {
        case ChannelTransform::Fit::Affine:
            std::snprintf(buf, sizeof(buf),
                          "ch%zu affine %+.0f ppm t=(%+.2f,%+.2f) "
                          "n=%d res=%.3fpx max=%.3fpx",
                          ch, (t.s - 1.0) * 1e6, t.tx, t.ty,
                          t.n_stars, t.residual, t.max_displacement(radius));
            break;
        case ChannelTransform::Fit::TranslationOnly:
            std::snprintf(buf, sizeof(buf),
                          "ch%zu translation t=(%+.2f,%+.2f) n=%d res=%.3fpx",
                          ch, t.tx, t.ty, t.n_stars, t.residual);
            break;
        case ChannelTransform::Fit::Identity:
            std::snprintf(buf, sizeof(buf),
                          "ch%zu identity (nothing measurable)", ch);
            break;
        }
        if (!out.empty()) out += "; ";
        out += buf;
    }
    return out;
}

} // namespace nukex
