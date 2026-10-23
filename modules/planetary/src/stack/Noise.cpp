#include "Noise.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "Measure.h"
#include "SerFile.h"

namespace planetary::stack {

namespace {

constexpr double kResponseGain = 216;  // variance of a response over the pixel variance (6^3)
constexpr int kFitRounds = 30;

void CheckLuma(const ser::FloatImage& im, const char* what) {
    if (im.channels != 1 || im.width <= 0 || im.height <= 0 || im.data.size() != static_cast<size_t>(im.width) * im.height)
        throw std::invalid_argument(std::string(what) + ": expected a non-empty 1-channel image");
}

// The median (the lower middle element) of v; v is reordered.
double Median(std::vector<double>& v) {
    const size_t k = (v.size() - 1) / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

}  // namespace

double NoiseModel::VarianceAtMean(double m) const {
    if (Zero()) return 0;
    if (knots.empty()) return Line(m);
    const auto it = std::lower_bound(knots.begin(), knots.end(), m, [](const Knot& k, double x) { return k.level < x; });
    if (it == knots.begin()) return it->variance;
    if (it == knots.end()) return knots.back().variance;
    const Knot& lo = *(it - 1);
    const Knot& hi = *it;
    const double f = hi.level > lo.level ? (m - lo.level) / (hi.level - lo.level) : 0.0;
    return std::max(0.0, lo.variance + f * (hi.variance - lo.variance));
}

void NoiseModel::Tabulate() {
    table_.assign(kNoiseTable, 0.0);
    for (int i = 0; i < kNoiseTable; ++i) table_[static_cast<size_t>(i)] = VarianceAtMean(static_cast<double>(i) / (kNoiseTable - 1));
}

bool NoiseModel::AnyClipped() const {
    return std::any_of(knots.begin(), knots.end(), [](const Knot& k) { return k.clipped; });
}

NoiseModel NoiseModel::FromSkyNoise(const NoiseEstimate& sky) {
    NoiseModel m;
    m.a = sky.sigma * sky.sigma;
    m.varA = sky.varSigma2;
    m.ownFrame = true;
    m.frames = 1;
    m.Tabulate();
    return m;
}

NoiseSamples::NoiseSamples() : n(kFineBins, 0.0), level(kFineBins, 0.0), r2(kFineBins, 0.0), r4(kFineBins, 0.0) {}

void NoiseSamples::Merge(const NoiseSamples& o) {
    for (size_t k = 0; k < n.size(); ++k) {
        n[k] += o.n[k];
        level[k] += o.level[k];
        r2[k] += o.r2[k];
        r4[k] += o.r4[k];
    }
    tiles.insert(tiles.end(), o.tiles.begin(), o.tiles.end());
    frames += o.frames;
    overlap = std::max(overlap, o.overlap);
    tileOverlap = std::max(tileOverlap, o.tileOverlap);
}

NoisePlane::NoisePlane(const ser::FloatImage& im) : w(im.width), h(im.height) {
    CheckLuma(im, "NoisePlane");
    if (w < 5 || h < 5) return;
    const size_t n = static_cast<size_t>(w) * h;
    const float* p = im.data.data();
    // [1 -2 1] along x, then along y; the level: the 3 x 3 mean, PixelVariance's (Measure.h), so the
    // curve is the noise given the very level the measures look it up by.
    std::vector<float> hd(n, 0.f), hl(n, 0.f);
    for (int y = 0; y < h; ++y) {
        const float* r = p + static_cast<size_t>(y) * w;
        float* o = hd.data() + static_cast<size_t>(y) * w;
        float* l = hl.data() + static_cast<size_t>(y) * w;
        for (int x = 1; x + 1 < w; ++x) {
            o[x] = r[x - 1] - 2 * r[x] + r[x + 1];
            l[x] = (r[x - 1] + r[x] + r[x + 1]) / 3;
        }
    }
    response.assign(n, 0.f);
    level.assign(n, 0.f);
    for (int y = 1; y + 1 < h; ++y) {
        const float* a = hd.data() + static_cast<size_t>(y - 1) * w;
        const float* b = hl.data() + static_cast<size_t>(y - 1) * w;
        float* o = response.data() + static_cast<size_t>(y) * w;
        float* l = level.data() + static_cast<size_t>(y) * w;
        for (int x = 1; x + 1 < w; ++x) {
            o[x] = a[x] - 2 * a[x + w] + a[x + 2 * w];
            l[x] = (b[x] + b[x + w] + b[x + 2 * w]) / 3;
        }
    }
}

int NoiseStep(int w, int h) {
    const double points = std::max(1.0, static_cast<double>(w - 4) * (h - 4));
    return std::max(1, static_cast<int>(std::ceil(std::sqrt(points / kNoiseResponsesPerTriple))));
}

namespace {

// [1 -2 1]'s autocorrelation is (1, -4/6, 1/6), so the variance of a mean square over responses spaced s
// apart is its independent value times 1 + 2 (rho(s)^2 + rho(2 s)^2) per axis: 1.944 at s = 1, 1.056 at
// s = 2, 1 from 3.
double OverlapAxis(int s) { return s == 1 ? 1.0 + 2.0 * (16.0 + 1.0) / 36 : s == 2 ? 1.0 + 2.0 / 36 : 1.0; }

}  // namespace

void AddTriple(const NoisePlane& p0, const NoisePlane& p1, const NoisePlane& p2, const NoisePlane& levels, NoiseSamples& out,
               const NoiseModel* first) {
    if (p0.w != p1.w || p2.w != p1.w || levels.w != p1.w || p0.h != p1.h || p2.h != p1.h || levels.h != p1.h)
        throw std::invalid_argument("AddTriple: the frames differ in size");
    const int w = p1.w, h = p1.h;
    ++out.frames;
    if (w < 5 || h < 5) return;
    const int step = NoiseStep(w, h);
    const int nx = (w - 4 + step - 1) / step, ny = (h - 4 + step - 1) / step;
    auto response = [&](int i, int j, double& level) {
        const size_t p = static_cast<size_t>(2 + j * step) * w + 2 + i * step;
        const double r = static_cast<double>(p0.response[p]) - 2.0 * p1.response[p] + p2.response[p];
        level = levels.level[p];
        return r * r / kResponseGain;
    };
    if (!first) {  // pass 1: the fine bins
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                double level;
                const double q = response(i, j, level);
                const size_t k = static_cast<size_t>(std::clamp(static_cast<int>(std::floor(level * kFineBins)), 0, kFineBins - 1));
                out.n[k] += 1;
                out.level[k] += level;
                out.r2[k] += q;
                out.r4[k] += q * q;
            }
        // In time, the triples slide by one frame (FitNoiseModel): 1.944 more.
        out.overlap = OverlapAxis(step) * OverlapAxis(step) * OverlapAxis(1);
        return;
    }
    // Pass 2: tiles of kNoiseTile x kNoiseTile responses, each read as its responses' summed square over
    // the first pass's variance summed at their levels: where the truth is c times the first pass's curve
    // over the tile's levels, c whatever their spread. (A mean of per-response ratios is not: where the
    // first pass's curve bends inside a sky's noise-spread levels, the mean of 1 / m(level) exceeds
    // 1 / m(mean level), and the synthetic shot-noise disc's sky read 1.08 of its variance.)
    const int tx = (nx + kNoiseTile - 1) / kNoiseTile, ty = (ny + kNoiseTile - 1) / kNoiseTile;
    const size_t nt = static_cast<size_t>(tx) * ty;
    std::vector<double> sq(nt, 0.0), sm(nt, 0.0), smm(nt, 0.0), sz(nt, 0.0), szz(nt, 0.0), sl(nt, 0.0), sll(nt, 0.0), sn(nt, 0.0);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            double level;
            const double q = response(i, j, level), m = first->Lookup(level);
            if (!(m > 0)) continue;  // a level the first pass found clipped flat: no noise to read
            const size_t t = static_cast<size_t>(j / kNoiseTile) * tx + i / kNoiseTile;
            sq[t] += q;
            sm[t] += m;
            smm[t] += m * m;
            sz[t] += q / m;
            szz[t] += q / m * (q / m);
            sl[t] += level;
            sll[t] += level * level;
            sn[t] += 1;
        }
    for (size_t t = 0; t < nt; ++t) {
        if (sn[t] < 0.5 * kNoiseTile * kNoiseTile) continue;  // a short edge tile, or mostly flat
        // Only tiles of one level: their levels' sd within kTileSpread of their mean, or within twice the
        // levels' own noise (a 3 x 3 mean's: a third of the pixel noise). A tile across a limb or a crater
        // rim holds the texture the seeing moves (a sharp edge's motion is no noise), and its mean level is
        // no level of its pixels.
        const double mean = sl[t] / sn[t], sd = std::sqrt(std::max(0.0, sll[t] / sn[t] - mean * mean));
        if (sd > std::max(kTileSpread * mean, 2.0 / 3 * std::sqrt(sm[t] / sn[t]))) continue;
        const double z = sz[t] / sn[t];
        // n: the effective responses of the m-weighted mean, (sum m)^2 / sum m^2.
        out.tiles.push_back({static_cast<float>(sl[t] / sn[t]), static_cast<float>(sq[t] / sm[t]),
                             static_cast<float>(z > 0 ? std::min(kMaxKurtosis, szz[t] / sn[t] / (z * z)) : kMaxKurtosis),
                             static_cast<float>(sm[t] * sm[t] / smm[t])});
    }
    out.tileOverlap = OverlapAxis(step) * OverlapAxis(step);  // within a tile: space only
}

namespace {

struct Point {
    double x, v, n, var;  // level, variance, weight (responses or tiles), the variance's sampling variance
};

// Pass 1's points: the fine bins merged from the dark end until each holds kMinBinResponses (a short last
// group joins the one before it); per bin the plain mean square over 216 and its sampling variance from
// the bin's own fourth moment, times the overlapping responses' correlation.
std::vector<Point> FineBinPoints(const NoiseSamples& s) {
    std::vector<Point> pts;
    double gn = 0, gl = 0, g2 = 0, g4 = 0;
    for (size_t k = 0; k < s.n.size(); ++k) {
        gn += s.n[k];
        gl += s.level[k];
        g2 += s.r2[k];
        g4 += s.r4[k];
        const bool last = k + 1 == s.n.size();
        if (gn >= kMinBinResponses || (last && gn > 0)) {
            if (gn < kMinBinResponses && !pts.empty()) {  // the short last group joins the one before it
                const Point& p = pts.back();
                gn += p.n;
                gl += p.x * p.n;
                g2 += p.v * p.n;
                g4 += (p.var / s.overlap * p.n + p.v * p.v) * p.n;
                pts.pop_back();
            }
            const double v = g2 / gn;
            pts.push_back({gl / gn, v, gn, s.overlap * std::max(0.0, g4 / gn - v * v) / gn});
            gn = gl = g2 = g4 = 0;
        }
    }
    return pts;
}

// Pass 2's points: level bins of kBinTiles tiles (by the tiles' mean levels; a short last one joins the one
// before), each the bin's ratio, read at the kNoiseQuantile-quantile of its tiles' ratios: texture the
// time difference left only raises a tile, and the median needs only half the tiles at a level to be
// clean. A tile's ratio is a weighted mean of n correlated standardised squares, relative sd
// tau = sqrt((kappa - 1) overlap / n) (kappa their kurtosis, the bin's median: 3 for Gaussian noise), near
// chi^2 with 2 / tau^2 degrees of freedom, whose quantile at the Blom position sits at
// (1 - c + z sqrt(c))^3 of the mean, c = 2 / (9 dof) (Wilson-Hilferty), by which the reading is divided.
// (The 25% quantile was tried: its correction leans on kappa 7x harder, and a tile's sample kurtosis
// reads low -- 2.88 for Gaussian noise -- which put white noise at 0.984 and the shot-noise discs'
// quality at 0.96.) Where the median kurtosis reaches kMaxKurtosis the tiles' ratios are no chi^2 -- a sky
// clipped to one value whose rare flips are spikes (the 2026-01-30 Jupiter 195830 AVI's 8-bit sky: median
// kurtosis at the cap, its tiles' median ratio to pass 1's mean square 0.24) -- and the bin's plain mean
// ratio is taken.
std::vector<Point> TilePoints(const NoiseSamples& s) {
    std::vector<Point> pts;
    std::vector<size_t> order(s.tiles.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t i, size_t j) {
        return s.tiles[i].level != s.tiles[j].level ? s.tiles[i].level < s.tiles[j].level : i < j;
    });
    for (size_t b0 = 0; b0 < order.size();) {
        size_t b1 = std::min(order.size(), b0 + kBinTiles);
        if (order.size() - b1 < static_cast<size_t>(kBinTiles)) b1 = order.size();  // the short last one joins
        std::vector<double> ratio, kap, cnt;
        double lev = 0, mean = 0;
        for (size_t i = b0; i < b1; ++i) {
            const NoiseSamples::Tile& t = s.tiles[order[i]];
            ratio.push_back(t.ratio);
            kap.push_back(t.kurtosis);
            cnt.push_back(t.n);
            lev += t.level;
            mean += t.ratio;
        }
        const double N = static_cast<double>(ratio.size());
        lev /= N;
        mean /= N;
        const double kappa = std::max(1.5, Median(kap)), n = Median(cnt);
        const double tau2 = (std::min(kappa, kMaxKurtosis) - 1) * s.tileOverlap / n;
        double c = mean, var = tau2 * mean * mean / N;
        if (kappa < kMaxKurtosis) {
            std::sort(ratio.begin(), ratio.end());
            const size_t k = static_cast<size_t>(kNoiseQuantile * (N - 1));
            const double pr = (static_cast<double>(k) + 0.625) / (N + 0.25);
            double lo = -8, hi = 8;  // z = Phi^-1(pr)
            for (int it = 0; it < 80; ++it) {
                const double mid = 0.5 * (lo + hi);
                (0.5 * std::erfc(-mid / std::sqrt(2.0)) < pr ? lo : hi) = mid;
            }
            const double z = 0.5 * (lo + hi), cw = tau2 / 9;  // 2 / (9 dof), dof = 2 / tau2
            c = ratio[k] / std::pow(std::max(1e-3, 1 - cw + z * std::sqrt(cw)), 3);
            const double dens = std::exp(-0.5 * z * z) / std::sqrt(2 * M_PI);
            var = pr * (1 - pr) / (N * dens * dens) * tau2 * c * c;  // the quantile's sampling variance
        }
        pts.push_back({lev, c, N, var});
        b0 = b1;
    }
    return pts;
}

// Each point's value averaged with its neighbours' (weighted by their weight) until they hold `weight`, and
// that average's sd.
struct Smooth {
    std::vector<double> v, sd;
};
Smooth Smoothed(const std::vector<Point>& pts, double weight) {
    Smooth out{std::vector<double>(pts.size()), std::vector<double>(pts.size())};
    for (size_t j = 0; j < pts.size(); ++j) {
        size_t lo = j, hi = j;
        double n = pts[j].n;
        while (n < weight && (lo > 0 || hi + 1 < pts.size())) {
            if (lo > 0) n += pts[--lo].n;
            if (n < weight && hi + 1 < pts.size()) n += pts[++hi].n;
        }
        double sv = 0, svar = 0;
        for (size_t i = lo; i <= hi; ++i) {
            sv += pts[i].n * pts[i].v;
            svar += pts[i].n * pts[i].n * pts[i].var;
        }
        out.v[j] = sv / n;
        out.sd[j] = std::sqrt(svar) / n;
    }
    return out;
}

// The line and the knots through a curve's points (FineBinPoints).
NoiseModel CurveFromPoints(const std::vector<Point>& pts, int sites, int frames) {
    NoiseModel model;
    model.frames = frames;
    // A bin with no noise at all (a level clipped flat), or within kClipSigmas of a sample's noise of black
    // (a sample of a luma pixel of `sites` has `sites` times its variance), is a clipped knot, not part of
    // the line: a sky 2 noise sd above black reads 0.960 of the line, inside the fit's slack.
    std::vector<char> nearBlack(pts.size(), 0);
    std::vector<size_t> live;
    for (size_t i = 0; i < pts.size(); ++i) {
        nearBlack[i] = pts[i].x < kClipSigmas * std::sqrt(std::max(1, sites) * pts[i].v);
        if (pts[i].v > 0 && !nearBlack[i]) live.push_back(i);
    }
    if (live.empty())  // every level is near black: the line goes through them
        for (size_t i = 0; i < pts.size(); ++i)
            if (pts[i].v > 0) {
                live.push_back(i);
                nearBlack[i] = 0;
            }
    if (live.empty()) {
        model.Tabulate();
        return model;
    }

    // A slope only where the bins' levels spread wider than kSlopeSpread level-noise sd (a level is one
    // frame's weighted 3 x 3 mean: sd 0.5 of the pixel noise, sqrt(sum w^4) / 36): within a narrow range --
    // a sky alone -- the observed level is mostly its noise, and a slope there is the noise's, not the
    // signal's (the 2026-01-30 Saturn AVI's sky-only bins read a slope of 9.2e-4 over 0.006 of level, and
    // the line it gave sat 30% under the sky's own variance).
    bool slope = false;
    {
        double m1 = 0, m2 = 0, mv = 0;
        for (size_t i : live) {
            m1 += pts[i].x;
            m2 += pts[i].x * pts[i].x;
            mv += pts[i].v;
        }
        const double nl = static_cast<double>(live.size()), spread = m2 / nl - (m1 / nl) * (m1 / nl);
        slope = spread > kSlopeSpread * kSlopeSpread * 0.25 * (mv / nl);
    }
    // Start (Theil-Sen): the median of the slopes between bins, then the median intercept.
    double a = 0, b = 0;
    if (slope) {
        std::vector<double> slopes, icpt;
        for (size_t i = 0; i < live.size(); ++i)
            for (size_t j = i + 1; j < live.size(); ++j) {
                const Point& p = pts[live[i]];
                const Point& q = pts[live[j]];
                if (q.x > p.x) slopes.push_back((q.v - p.v) / (q.x - p.x));
            }
        b = slopes.empty() ? 0.0 : std::max(0.0, Median(slopes));
        for (size_t i : live) icpt.push_back(pts[i].v - b * pts[i].x);
        a = Median(icpt);
    } else {
        std::vector<double> v;
        for (size_t i : live) v.push_back(pts[i].v);
        a = Median(v);
    }
    // Weighted least squares, iterated with Tukey's biweight on each bin's relative error.
    std::vector<double> w(pts.size(), 0.0);
    for (int round = 0; round < kFitRounds; ++round) {
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i : live) {
            const double f = std::max(a + b * pts[i].x, 1e-30);
            const double rel2 = pts[i].var / (pts[i].v * pts[i].v) + kFitSlack * kFitSlack;
            const double z = (pts[i].v - f) / (f * std::sqrt(rel2)) / kTukey;
            w[i] = std::fabs(z) < 1 ? (1 - z * z) * (1 - z * z) / (f * f * rel2) : 0;
            sw += w[i];
            sx += w[i] * pts[i].x;
            sy += w[i] * pts[i].v;
            sxx += w[i] * pts[i].x * pts[i].x;
            sxy += w[i] * pts[i].x * pts[i].v;
        }
        if (!(sw > 0)) break;
        double na = sy / sw, nb = 0;
        const double det = sw * sxx - sx * sx;
        if (det > 1e-9 * sw * sxx) {
            nb = (sw * sxy - sx * sy) / det;
            na = (sy - nb * sx) / sw;
        }
        if (nb < 0 || !slope) {  // shot noise cannot lower the variance; no slope to be had: the level alone
            nb = 0;
            na = sy / sw;
        } else if (na < 0 && sxx > 0) {  // nor is a read noise negative: the line through 0
            // (the 2026-01-30 Moon 193142 read a = -2.8e-6, its dark levels' curve concave under the line)
            na = 0;
            nb = sxy / sxx;
        }
        const bool done = std::fabs(na - a) <= 1e-9 * std::fabs(a) && std::fabs(nb - b) <= 1e-9 * std::fabs(b) + 1e-15;
        a = na;
        b = nb;
        if (done) break;
    }
    model.a = a;
    model.b = b;
    // The line's own (co)variance, from the bins that carried it, weighted by their sampling variance.
    double haa = 0, hab = 0, hbb = 0;
    for (size_t i : live) {
        if (!(w[i] > 0) || !(pts[i].var > 0)) continue;
        const double wi = 1 / pts[i].var;
        haa += wi;
        hab += wi * pts[i].x;
        hbb += wi * pts[i].x * pts[i].x;
    }
    const double det = haa * hbb - hab * hab;
    if (b > 0 && det > 0) {
        model.varA = hbb / det;
        model.varB = haa / det;
        model.covAB = -hab / det;
    } else if (haa > 0) {
        model.varA = 1 / haa;
    }
    // The knots: per bin, the curve smoothed over the neighbouring bins until they hold kKnotResponses, and
    // whether it lies under the line by more than kClipSigmas of its sd or the bin is near black (a clip).
    const Smooth smooth = Smoothed(pts, kKnotResponses);
    for (size_t j = 0; j < pts.size(); ++j) {
        NoiseModel::Knot k;
        k.level = pts[j].x;
        k.variance = smooth.v[j];
        k.clipped = nearBlack[j] || smooth.v[j] < model.Line(pts[j].x) - kClipSigmas * smooth.sd[j];
        model.knots.push_back(k);
    }
    model.Tabulate();
    return model;
}

// Pass 2's correction of pass 1's points: each times the tiles' ratio at its level (TilePoints smoothed
// over kKnotTiles tiles, linear between their levels, held beyond the ends).
std::vector<Point> Corrected(std::vector<Point> pts, const std::vector<Point>& ratios) {
    const std::vector<double> c = Smoothed(ratios, kKnotTiles).v;
    for (Point& p : pts) {
        const auto it = std::lower_bound(ratios.begin(), ratios.end(), p.x, [](const Point& r, double x) { return r.x < x; });
        double f = 1;  // beyond the levels one-level tiles cover: no clean reading, pass 1's own
        if (it != ratios.begin() && it != ratios.end()) {
            const size_t i = static_cast<size_t>(it - ratios.begin());
            const double t = (p.x - ratios[i - 1].x) / std::max(1e-12, ratios[i].x - ratios[i - 1].x);
            f = c[i - 1] + t * (c[i] - c[i - 1]);
        }
        p.v *= f;
        p.var *= f * f;
    }
    return pts;
}

}  // namespace

NoiseModel FitNoise(const NoiseSamples& s, int sites) {
    NoiseModel m = CurveFromPoints(FineBinPoints(s), sites, s.frames);
    for (double c : s.n) m.responses += static_cast<int64_t>(c);
    return m;
}

NoiseModel FitNoise(const NoiseSamples& first, const NoiseSamples& second, int sites) {
    const NoiseModel one = FitNoise(first, sites);
    if (second.tiles.empty()) return one;
    NoiseModel m = CurveFromPoints(Corrected(FineBinPoints(first), TilePoints(second)), sites, first.frames);
    m.responses = one.responses;
    return m;
}

namespace {

// Sliding triples over consecutive frames, each frame's plane made once: (f, f + 1, f + 2) with the levels
// of f + 3, for f = first .. first + triples - 1 (frames first .. first + triples + 2).
void AddRun(const std::function<ser::FloatImage(int64_t)>& frame, int64_t first, int64_t triples, NoiseSamples& out,
            const NoiseModel* model) {
    std::vector<NoisePlane> planes;
    for (int64_t i = 0; i < triples + 3; ++i) {
        planes.emplace_back(frame(first + i));
        if (planes.size() == 4) {
            AddTriple(planes[0], planes[1], planes[2], planes[3], out, model);
            planes.erase(planes.begin());
        }
    }
}

}  // namespace

std::vector<NoiseRun> NoiseRuns(const ser::FileInfo& info) {
    const int64_t total = std::max<int64_t>(0, info.frameCount);
    const double scale = LumaScale(info);
    const int lw = static_cast<int>(info.header.width / scale), lh = static_cast<int>(info.header.height / scale);
    const int step = NoiseStep(lw, lh);
    const double perTriple = std::max(1.0, std::ceil(std::max(0, lw - 4) / static_cast<double>(step)) *
                                               std::ceil(std::max(0, lh - 4) / static_cast<double>(step)));
    const int64_t possible = std::max<int64_t>(0, total - 3);
    const int64_t triples =
        std::min<int64_t>(possible, std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(kNoiseResponses) / perTriple)));
    const int64_t runs = triples > 0 ? (triples + kNoiseRun - 1) / kNoiseRun : 0;
    std::vector<NoiseRun> plan;
    for (int64_t r = 0; r < runs; ++r) {
        const int64_t n = triples / runs + (r < triples % runs ? 1 : 0);
        const int64_t first = runs > 1 ? r * (possible - n) / (runs - 1) : (possible - n) / 2;
        plan.push_back({first, n});
    }
    return plan;
}

NoiseModel FitNoiseModel(const std::vector<ser::FloatImage>& frames, int sites) {
    NoiseSamples s1, s2;
    if (frames.size() < 4) return FitNoise(s1, sites);
    const auto frame = [&](int64_t i) { return frames[static_cast<size_t>(i)]; };
    const auto triples = static_cast<int64_t>(frames.size()) - 3;
    AddRun(frame, 0, triples, s1, nullptr);
    const NoiseModel first = FitNoise(s1, sites);
    AddRun(frame, 0, triples, s2, &first);
    return FitNoise(s1, s2, sites);
}

int LumaSites(const ser::FileInfo& info) {
    if (ser::IsBayer(info.colorId) || UndeclaredMosaic(info)) return 4;
    return info.planes == 3 ? 3 : 1;
}

NoiseModel FitNoiseModel(const std::function<std::unique_ptr<ser::FrameSource>()>& factory, const ser::FileInfo& info,
                         int threads, const Progress& progress) {
    const char* const phase = "Measuring noise";
    const std::vector<NoiseRun> plan = NoiseRuns(info);
    const auto runs = static_cast<int64_t>(plan.size());
    const int64_t total = kNoisePasses * runs;
    if (progress.report) progress.report(phase, 0, total);
    if (plan.empty()) return FitNoise(NoiseSamples{}, LumaSites(info));
    if (!factory) throw std::invalid_argument("FitNoiseModel: no reader factory");
    // One pass over every run on up to `threads` workers, its samples merged in the plan's order.
    auto pass = [&](const NoiseModel* model, int64_t base) {
        std::vector<NoiseSamples> per(plan.size());
        if (progress.cancelled && progress.cancelled()) throw StackCancelled("cancelled while measuring the noise");
        const int workers = static_cast<int>(std::min<int64_t>(std::max(1, threads), runs));
        std::atomic<size_t> next{0};
        std::atomic<int64_t> done{0};
        std::atomic<bool> stop{false};
        std::mutex m;
        std::exception_ptr error;
        auto work = [&] {
            try {
                const std::unique_ptr<ser::FrameSource> src = factory();
                if (!src) throw ser::SerError("FitNoiseModel: the reader factory returned no reader");
                for (size_t i; !stop.load() && (i = next.fetch_add(1)) < plan.size();) {
                    AddRun([&](int64_t f) {
                        if (stop.load()) throw StackCancelled("cancelled while measuring the noise");
                        return MeasurementLuma(src->ReadFrame(f), info);
                    }, plan[i].first, plan[i].triples, per[i], model);
                    done.fetch_add(1);
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(m);
                if (!error) error = std::current_exception();
                stop = true;
            }
        };
        std::vector<std::thread> pool;
        struct Joiner {
            std::vector<std::thread>& pool;
            std::atomic<bool>& stop;
            ~Joiner() {
                stop = true;
                for (std::thread& t : pool)
                    if (t.joinable()) t.join();
            }
        } joiner{pool, stop};
        for (int w = 0; w < workers; ++w) pool.emplace_back(work);
        // Report and poll on this thread until every run is in, a worker failed, or cancelled.
        bool cancelled = false;
        int64_t reported = 0;
        for (;;) {
            const int64_t d = done.load();
            if (progress.report && d != reported) progress.report(phase, base + (reported = d), total);
            if (!cancelled && progress.cancelled && progress.cancelled()) {
                cancelled = true;
                stop = true;
            }
            if (d >= runs || stop.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (std::thread& t : pool) t.join();
        if (cancelled) throw StackCancelled("cancelled while measuring the noise");
        if (error) std::rethrow_exception(error);
        NoiseSamples all;
        for (const NoiseSamples& s : per) all.Merge(s);
        return all;
    };
    const int sites = LumaSites(info);
    const NoiseSamples s1 = pass(nullptr, 0);
    const NoiseModel first = FitNoise(s1, sites);
    return FitNoise(s1, pass(&first, runs), sites);
}

}  // namespace planetary::stack
