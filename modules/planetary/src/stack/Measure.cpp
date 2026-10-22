#include "Measure.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "Demosaic.h"
#include "SerFile.h"

namespace planetary::stack {

namespace {

constexpr int kGridCells = 256;              // detection grid: longest side in blocks
constexpr double kBackgroundPercentile = 0.2;
constexpr double kNoisePercentile = 0.2;     // of tile noise estimates
constexpr int kMaxTile = 32;
constexpr double kNoiseTileSigmas = 3;       // a noise tile: within 3 model sd of the noise mean
constexpr int kNoiseRounds = 8;              // fixed-point rounds of the noise-tile count and kurtosis
constexpr double kMaxNoiseKurtosis = 12;     // caps kappa (8-bit noise of ~0.3 DN reads 10.5)
constexpr double kSeedSigmas = 4;            // component seeds: blocks above b + 4 s_b
constexpr double kPeakSigmas = 10;           // a target's brightest block: >= b + 10 s_b ...
constexpr double kMinContrast = 0.01;        // ... and >= b + 1% of full scale
constexpr double kExtentFraction = 0.1;      // extent: above 10% of the contrast
constexpr double kCentroidFraction = 0.5;    // centroid: above half the contrast
constexpr double kMinTargetPixels = 16;      // sensor pixels above half the contrast
constexpr double kMarginFraction = 0.1;      // quality region: box + 10%

const char* const kPhase = "Measuring frames";

// The q-quantile (lower element) of v; v is reordered.
double Quantile(std::vector<double>& v, double q) {
    const size_t k = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

double NormalCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }
double NormalPdf(double z) { return std::exp(-0.5 * z * z) / std::sqrt(2 * M_PI); }

// The pixel-noise tiles of an image (Measure.h, "Background and noise"): per tile the mean squared
// horizontal neighbour difference (white noise: E[d^2] = 2 s^2) and the mean d^4. Tiles with fewer
// than half a full tile's pairs are skipped.
struct NoiseTiles {
    // A tile shape (pairs m, pairs per row L): full tiles, the last column's, the last row's, the corner.
    struct Shape {
        int64_t m = 0;
        int len = 0;
        double n = 0, nNext = 0, tau = 0, tau0 = 0;  // tau0: a Gaussian noise's (kurtosis 3)
    };
    struct Tile {
        double d2 = 0, d4 = 0;  // mean d^2, mean d^4
        int64_t m = 0;
        size_t shape = 0;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the tile's pixels, half-open
    };
    std::vector<Shape> shapes;
    std::vector<Tile> tiles;
    size_t k = 0;        // the order statistic read: the k-th smallest (0-based) of the tiles' d^2
    double q = 0;        // its value
    int64_t pairs = 0;   // pairs in all the tiles, M
};

NoiseTiles MeasureNoiseTiles(const float* p, int w, int h) {
    NoiseTiles out;
    if (w < 2) return out;
    const int tile = std::max(1, std::min({kMaxTile, std::max(8, std::min(w, h) / 4), std::min(w, h)}));
    const int tx = (w + tile - 1) / tile, ty = (h + tile - 1) / tile;
    std::vector<double> sum(static_cast<size_t>(tx) * ty, 0.0), sum4(sum.size(), 0.0);
    std::vector<int64_t> cnt(sum.size(), 0);
    std::vector<int> rowPairs(sum.size(), 0);  // pairs per row in the tile (fewer in the last column)
    for (int y = 0; y < h; ++y) {
        const float* row = p + static_cast<size_t>(y) * w;
        const size_t trow = static_cast<size_t>(y / tile) * tx;
        for (int x0 = 0; x0 < w - 1; x0 += tile) {
            const int x1 = std::min(w - 1, x0 + tile);
            double s = 0, s4 = 0;
            for (int x = x0; x < x1; ++x) {
                const double d = static_cast<double>(row[x + 1]) - row[x], d2 = d * d;
                s += d2;
                s4 += d2 * d2;
            }
            const size_t t = trow + x0 / tile;
            sum[t] += s;
            sum4[t] += s4;
            cnt[t] += x1 - x0;
            rowPairs[t] = x1 - x0;
        }
    }
    const int64_t full = static_cast<int64_t>(std::min(tile, w - 1)) * std::min(tile, h);
    std::vector<double> est;
    out.tiles.reserve(sum.size());
    est.reserve(sum.size());
    for (size_t i = 0; i < sum.size(); ++i)
        if (cnt[i] > 0 && 2 * cnt[i] >= full) {
            size_t sh = 0;
            while (sh < out.shapes.size() && !(out.shapes[sh].m == cnt[i] && out.shapes[sh].len == rowPairs[i])) ++sh;
            if (sh == out.shapes.size()) out.shapes.push_back({cnt[i], rowPairs[i]});
            const double m = static_cast<double>(cnt[i]);
            const int x0 = static_cast<int>(i % tx) * tile, y0 = static_cast<int>(i / tx) * tile;
            out.tiles.push_back({sum[i] / m, sum4[i] / m, cnt[i], sh, x0, y0, std::min(w, x0 + tile), std::min(h, y0 + tile)});
            est.push_back(sum[i] / m);
            out.pairs += cnt[i];
        }
    if (est.empty()) return out;
    out.k = static_cast<size_t>(kNoisePercentile * static_cast<double>(est.size() - 1));
    out.q = Quantile(est, kNoisePercentile);
    return out;
}

// A target's box (luma px, half-open): the tiles overlapping it are not sky.
struct TargetBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Overlaps(const NoiseTiles::Tile& t) const { return t.x0 < x1 && t.x1 > x0 && t.y0 < y1 && t.y1 > y0; }
};

// Pixel noise from the tiles (Ruling P19; Measure.h, "Background and noise"): q is read as the k-th
// smallest of the N noise tiles -- those whose d^2 lies within kNoiseTileSigmas of the noise mean --
// and s^2 is the mean that puts it there. `target`: the sky is outside it (null: the whole image).
NoiseEstimate PixelNoise(NoiseTiles t, const TargetBox* target) {
    std::vector<NoiseTiles::Shape>& shapes = t.shapes;
    const size_t k = t.k;
    const double q = t.q;
    if (t.tiles.empty() || !(q > 0)) return {};  // no tiles, or a constant (clipped) sky: no measurable noise

    // Each tile's estimate of 2 s^2 is near-normal with relative sd tau = sqrt((kappa - (kappa - 1) / (2 L))
    // / m), kappa = mu4 / s^4 of the noise (a row of L pairs has variance (4 kappa L - 2 kappa + 2) s^4).
    // solve(): u = 2 s^2 such that q is where the k-th smallest of the counted tiles is expected (Blom),
    // and the variance of that sample quantile, pr (1 - pr) / (N f(q)^2), f their density at q.
    double kappa = 3, nTiles = 0;
    auto solve = [&](double& u, double& pr, double& varQ) {
        nTiles = 0;
        for (NoiseTiles::Shape& s : shapes) {
            nTiles += s.n;
            s.tau = std::sqrt((kappa - (kappa - 1) / (2.0 * s.len)) / static_cast<double>(s.m));
        }
        pr = std::min(0.5, (static_cast<double>(k) + 0.625) / (nTiles + 0.25));
        // x = q / u in (0, 1]: the expected share of noise tiles under q, sum n_g Phi((x - 1) / tau_g) / N,
        // rises with x and is 1/2 at x = 1, so bisection finds the x where it is pr.
        double lo = 0, hi = 1;
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo + hi);
            double share = 0;
            for (const NoiseTiles::Shape& s : shapes) share += s.n * NormalCdf((mid - 1) / s.tau);
            (share / nTiles < pr ? lo : hi) = mid;
        }
        u = q / hi;
        double density = 0;
        for (const NoiseTiles::Shape& s : shapes) density += s.n * NormalPdf((hi - 1) / s.tau) / (u * s.tau);
        density /= nTiles;
        varQ = pr * (1 - pr) / (nTiles * density * density);
    };
    // Round 0: every tile taken as noise, kappa 3 -- the centre of the first noise band.
    for (const NoiseTiles::Tile& tl : t.tiles) shapes[tl.shape].n += 1;
    for (NoiseTiles::Shape& s : shapes) s.tau0 = std::sqrt((3 - 1.0 / s.len) / static_cast<double>(s.m));
    double u = q, pr = 0.5, varQ = 0;
    solve(u, pr, varQ);
    // Then count the noise tiles in the band (Gaussian width, so texture does not widen it) and their
    // kurtosis, and solve again, to a fixed point. No correction (s^2 = q / 2) when the sky outside the
    // target holds fewer than k + 1 of them (q cannot be shown to be sky noise), or when there are at
    // most 2k + 1 in all (pr capped at 1/2: q is not in their lower half) -- the smoothest tiles are then
    // texture, not noise.
    bool corrected = false;
    for (int round = 1; round <= kNoiseRounds; ++round) {
        for (NoiseTiles::Shape& s : shapes) s.nNext = 0;
        double a2 = 0, a4 = 0, pairs = 0;
        size_t sky = 0;
        for (const NoiseTiles::Tile& tl : t.tiles) {
            NoiseTiles::Shape& s = shapes[tl.shape];
            if (tl.d2 > u * (1 + kNoiseTileSigmas * s.tau0)) continue;
            s.nNext += 1;
            const double m = static_cast<double>(tl.m);
            a2 += tl.d2 * m;
            a4 += tl.d4 * m;
            pairs += m;
            sky += !(target && target->Overlaps(tl));
        }
        corrected = sky > k;
        if (!corrected) break;
        for (NoiseTiles::Shape& s : shapes) s.n = s.nNext / NormalCdf(kNoiseTileSigmas);
        // kappa from the noise tiles: E[d^4] = 2 mu4 + 6 s^4 and E[d^2] = 2 s^2.
        kappa = std::clamp(2 * a4 * pairs / (a2 * a2) - 3, 1.0, kMaxNoiseKurtosis);
        solve(u, pr, varQ);
        corrected = pr < 0.5;
        if (!corrected) break;
    }
    NoiseEstimate n;
    if (corrected) {
        const double s2 = u / 2;
        n.sigma = std::sqrt(s2);
        n.varSigma2 = varQ * (s2 / q) * (s2 / q);  // s^2 = q / (2 x)
    } else {
        // Uncorrected, the reading as it always was: s^2 = q / 2, and the variance of the 20th percentile
        // of K tile estimates taken as Gaussian noise, 0.2 x 0.8 / (K phi(z_0.2)^2) x 12 s^4 / m, halved
        // twice: 6.12 s^4 / M (M = all pairs) (solve()'s for K like tiles at p = 0.2, u = q).
        const double s2 = q / 2;
        n.sigma = std::sqrt(s2);
        n.varSigma2 = 6.12 * s2 * s2 / static_cast<double>(t.pairs);
    }
    return n;
}

struct Grid {
    int d = 1, cw = 0, ch = 0;
    std::vector<double> mean;   // block means
    std::vector<int64_t> area;  // pixels per block (edge blocks may be partial)
};

// One pass of a 3-wide min or max filter along rows (step 1) or columns (step w); the window is
// cut at the image border, so a target cut by the edge stays at the edge.
template <bool kMax>
void Filter3(const float* in, float* out, int w, int h, bool alongRows) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            float v = in[i];
            const bool lo = alongRows ? x > 0 : y > 0, hi = alongRows ? x + 1 < w : y + 1 < h;
            const size_t step = alongRows ? 1 : static_cast<size_t>(w);
            if (lo) v = kMax ? std::max(v, in[i - step]) : std::min(v, in[i - step]);
            if (hi) v = kMax ? std::max(v, in[i + step]) : std::min(v, in[i + step]);
            out[i] = v;
        }
}

// Grey-scale opening by a 3x3 square (erosion, then dilation): every structure that a 3x3 square
// does not fit inside -- a hot pixel, pair, column or row segment, a 1-2 px wide cosmic-ray
// streak -- is flattened to its surroundings; anything at least 3x3 keeps its shape. Result in `a`.
void Open3x3(const float* p, int w, int h, std::vector<float>& a, std::vector<float>& b) {
    const size_t n = static_cast<size_t>(w) * h;
    a.resize(n);
    b.resize(n);
    Filter3<false>(p, a.data(), w, h, true);
    Filter3<false>(a.data(), b.data(), w, h, false);
    Filter3<true>(b.data(), a.data(), w, h, true);
    Filter3<true>(a.data(), b.data(), w, h, false);
    a.swap(b);
}

// Block means of the opened image `o`.
Grid BlockMeans(const float* o, int w, int h) {
    Grid g;
    g.d = DetectionBlock(w, h);
    g.cw = (w + g.d - 1) / g.d;
    g.ch = (h + g.d - 1) / g.d;
    g.mean.assign(static_cast<size_t>(g.cw) * g.ch, 0.0);
    g.area.assign(g.mean.size(), 0);
    for (int y = 0; y < h; ++y) {
        const float* row = o + static_cast<size_t>(y) * w;
        const size_t grow = static_cast<size_t>(y / g.d) * g.cw;
        for (int x = 0; x < w; ++x) g.mean[grow + x / g.d] += row[x];
    }
    for (int gy = 0; gy < g.ch; ++gy)
        for (int gx = 0; gx < g.cw; ++gx) {
            const size_t i = static_cast<size_t>(gy) * g.cw + gx;
            g.area[i] = static_cast<int64_t>(std::min(g.d, w - gx * g.d)) * std::min(g.d, h - gy * g.d);
            g.mean[i] /= static_cast<double>(g.area[i]);
        }
    return g;
}

// Block noise: MAD of neighbouring block differences / sqrt(2), as a Gaussian sigma.
double BlockNoise(const Grid& g) {
    std::vector<double> diffs;
    diffs.reserve(g.mean.size() * 2);
    for (int gy = 0; gy < g.ch; ++gy)
        for (int gx = 0; gx < g.cw; ++gx) {
            const size_t i = static_cast<size_t>(gy) * g.cw + gx;
            if (gx + 1 < g.cw) diffs.push_back(std::fabs(g.mean[i + 1] - g.mean[i]));
            if (gy + 1 < g.ch) diffs.push_back(std::fabs(g.mean[i + g.cw] - g.mean[i]));
        }
    if (diffs.empty()) return 0;
    return 1.4826 * Quantile(diffs, 0.5) / std::sqrt(2.0);
}

// 8-connected flood fill over blocks with mean > thr, from `seed`; marks `label` in `labels`.
template <class Visit>
void Flood(const Grid& g, double thr, size_t seed, int label, std::vector<int>& labels, Visit visit) {
    std::vector<size_t> stack{seed};
    labels[seed] = label;
    while (!stack.empty()) {
        const size_t i = stack.back();
        stack.pop_back();
        visit(i);
        const int gx = static_cast<int>(i % g.cw), gy = static_cast<int>(i / g.cw);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = gx + dx, ny = gy + dy;
                if (nx < 0 || ny < 0 || nx >= g.cw || ny >= g.ch) continue;
                const size_t j = static_cast<size_t>(ny) * g.cw + nx;
                if (labels[j] == 0 && g.mean[j] > thr) {
                    labels[j] = label;
                    stack.push_back(j);
                }
            }
    }
}

}  // namespace

bool UndeclaredMosaic(const ser::FileInfo& info) {
    return info.colorId == ser::ColorId::Mono && info.avi && info.avi->bayerSource == "none";
}

double LumaScale(const ser::FileInfo& info) { return ser::IsBayer(info.colorId) || UndeclaredMosaic(info) ? 2.0 : 1.0; }

ser::FloatImage MeasurementLuma(const ser::Frame& frame, const ser::FileInfo& info) {
    // LumaForMeasure's Bayer luma is the 2 x 2 cell mean whatever the pattern, so any Bayer id bins.
    return LumaForMeasure(frame, UndeclaredMosaic(info) ? ser::ColorId::BayerRGGB : info.colorId, info.normDepth);
}

int DetectionBlock(int width, int height) {
    return std::max(1, (std::max(width, height) + kGridCells - 1) / kGridCells);
}

// MeasureFrame, also handing out the noise its quality used (EstimatePixelNoise).
static FrameStats MeasureWithNoise(const ser::FloatImage& luma, int64_t index, double lumaScale, NoiseEstimate* qualityNoise) {
    if (luma.width <= 0 || luma.height <= 0)
        throw std::invalid_argument("MeasureFrame: empty image");
    if (luma.channels != 1)
        throw std::invalid_argument("MeasureFrame: expected a 1-channel luma image, got " +
                                    std::to_string(luma.channels) + " channels");
    const int w = luma.width, h = luma.height;
    const size_t npix = static_cast<size_t>(w) * h;
    if (luma.data.size() != npix)
        throw std::invalid_argument("MeasureFrame: " + std::to_string(luma.data.size()) + " samples for a " +
                                    std::to_string(w) + "x" + std::to_string(h) + " image");
    if (!std::isfinite(lumaScale) || lumaScale < 1)
        throw std::invalid_argument("MeasureFrame: luma scale " + std::to_string(lumaScale) + " is not >= 1");
    const float* p = luma.data.data();
    auto at = [&](int x, int y) { return static_cast<double>(p[static_cast<size_t>(y) * w + x]); };

    FrameStats s;
    s.index = index;

    // Per-thread scratch for the opened image (a 1080p mono frame is 8 MB as float; MeasureAll calls this
    // per frame). Two buffers (openA, openB), so ~16 MB per calling thread at 1080p, held until the
    // thread exits -- per MeasureAll worker, see its memory note in Measure.h.
    thread_local std::vector<float> openA, openB;
    Open3x3(p, w, h, openA, openB);
    const float* opened = openA.data();
    const Grid g = BlockMeans(opened, w, h);
    const NoiseTiles tiles = MeasureNoiseTiles(p, w, h);
    // Detection's floor s / d needs s before there is a box, so the whole image is taken as sky. On a
    // frame-filling target that corrects texture as if it were noise, but the floor does not hold
    // there: the opened grid's MAD estimate averages 8.7x / 21x s / d on the 2026-01-30 193142 / 2023-02-02
    // 202030 Moons, and the floor holds in none of their first 300 frames (measured 2026-10-22).
    const NoiseEstimate noiseImage = PixelNoise(tiles, nullptr);
    const double sigma = noiseImage.sigma;
    std::vector<double> sorted = g.mean;
    const double b = Quantile(sorted, kBackgroundPercentile);
    const double sigmaB = std::max(BlockNoise(g), sigma / g.d);

    // Components above the seed threshold; the candidate with the most flux is the target.
    std::vector<int> labels(g.mean.size(), 0);
    const double seedThr = b + kSeedSigmas * sigmaB;
    size_t bestPeak = 0;
    double bestFlux = -1;
    int next = 1;
    std::vector<size_t> blocks;
    for (size_t i = 0; i < g.mean.size(); ++i) {
        if (labels[i] != 0 || g.mean[i] <= seedThr) continue;
        size_t peak = i;
        double flux = 0;
        blocks.clear();
        Flood(g, seedThr, i, next++, labels, [&](size_t j) {
            if (g.mean[j] > g.mean[peak] || (g.mean[j] == g.mean[peak] && j < peak)) peak = j;
            flux += (g.mean[j] - b) * static_cast<double>(g.area[j]);
            blocks.push_back(j);
        });
        const double contrast = g.mean[peak] - b;
        if (contrast < std::max(kPeakSigmas * sigmaB, kMinContrast) || flux <= bestFlux) continue;
        // Size in real pixels: opened-image pixels of the component above half its contrast.
        const double half = b + kCentroidFraction * contrast;
        int64_t core = 0;
        for (size_t j : blocks) {
            const int x0 = static_cast<int>(j % g.cw) * g.d, y0 = static_cast<int>(j / g.cw) * g.d;
            for (int y = y0; y < std::min(h, y0 + g.d); ++y)
                for (int x = x0; x < std::min(w, x0 + g.d); ++x)
                    if (opened[static_cast<size_t>(y) * w + x] > half) ++core;
        }
        if (static_cast<double>(core) * lumaScale * lumaScale >= kMinTargetPixels) {
            bestFlux = flux;
            bestPeak = peak;
        }
    }

    if (bestFlux < 0) {
        double sum = 0;
        int64_t clipped = 0;
        for (size_t i = 0; i < npix; ++i) {
            sum += p[i];
            if (p[i] >= kClippedLuma) ++clipped;
        }
        s.brightness = sum / static_cast<double>(npix);
        s.clippedFraction = static_cast<double>(clipped) / static_cast<double>(npix);
        if (qualityNoise) *qualityNoise = noiseImage;  // no target: the whole image is sky
        return s;
    }
    s.targetFound = true;

    // Extent: regrow from the brightest block over blocks above 10% of the contrast.
    const double contrast = g.mean[bestPeak] - b;
    const double extThr = b + std::max(kSeedSigmas * sigmaB, kExtentFraction * contrast);
    std::fill(labels.begin(), labels.end(), 0);
    int gx0 = g.cw, gy0 = g.ch, gx1 = -1, gy1 = -1;
    Flood(g, extThr, bestPeak, 1, labels, [&](size_t j) {
        const int gx = static_cast<int>(j % g.cw), gy = static_cast<int>(j / g.cw);
        gx0 = std::min(gx0, gx);
        gx1 = std::max(gx1, gx);
        gy0 = std::min(gy0, gy);
        gy1 = std::max(gy1, gy);
    });
    s.bx0 = std::max(0, (gx0 - 1) * g.d);
    s.by0 = std::max(0, (gy0 - 1) * g.d);
    s.bx1 = std::min(w, (gx1 + 2) * g.d);
    s.by1 = std::min(h, (gy1 + 2) * g.d);

    // Target pixels, brightness, clipping, centroid, and the half-contrast extent.
    const double cenThr = b + kCentroidFraction * contrast;
    double sum = 0, wsum = 0, wx = 0, wy = 0;
    int64_t n = 0, clipped = 0;
    int hx0 = w, hy0 = h, hx1 = -1, hy1 = -1;
    for (int y = s.by0; y < s.by1; ++y)
        for (int x = s.bx0; x < s.bx1; ++x) {
            const double v = at(x, y);
            if (v > extThr) {
                sum += v;
                ++n;
                if (v >= kClippedLuma) ++clipped;
            }
            if (opened[static_cast<size_t>(y) * w + x] > cenThr) {
                hx0 = std::min(hx0, x);
                hx1 = std::max(hx1, x);
                hy0 = std::min(hy0, y);
                hy1 = std::max(hy1, y);
            }
            if (v > cenThr) {
                const double wt = v - cenThr;
                wsum += wt;
                wx += wt * x;
                wy += wt * y;
            }
        }
    if (n > 0) {
        s.brightness = sum / static_cast<double>(n);
        s.clippedFraction = static_cast<double>(clipped) / static_cast<double>(n);
    }
    if (wsum > 0) {
        s.cx = wx / wsum;
        s.cy = wy / wsum;
    }
    if (hx1 >= hx0) s.halfRadius = 0.5 * std::max(hx1 - hx0 + 1, hy1 - hy0 + 1);

    // Quality over the box plus a margin.
    const int margin = std::max(1, static_cast<int>(std::lround(kMarginFraction * std::max(s.bx1 - s.bx0, s.by1 - s.by0))));
    const int rx0 = std::max(0, s.bx0 - margin), ry0 = std::max(0, s.by0 - margin);
    const int rx1 = std::min(w, s.bx1 + margin), ry1 = std::min(h, s.by1 + margin);
    // The quality's noise: corrected only when the sky outside the target's box holds more than k noise
    // tiles ("Background and noise"; a frame-filling target leaves none, and its reading stays as it was).
    const TargetBox box{s.bx0, s.by0, s.bx1, s.by1};
    const NoiseEstimate noise = PixelNoise(tiles, &box);
    if (qualityNoise) *qualityNoise = noise;
    const QualityEstimate q = RegionQuality(p, w, rx0, ry0, rx1, ry1, b, noise);
    s.quality = q.quality;
    s.qualitySigma = q.sigma;
    return s;
}

FrameStats MeasureFrame(const ser::FloatImage& luma, int64_t index, double lumaScale) {
    return MeasureWithNoise(luma, index, lumaScale, nullptr);
}

NoiseEstimate EstimatePixelNoise(const ser::FloatImage& luma) {
    if (luma.channels != 1 || luma.width <= 0 || luma.height <= 0 ||
        luma.data.size() != static_cast<size_t>(luma.width) * luma.height)
        throw std::invalid_argument("EstimatePixelNoise: expected a non-empty 1-channel image");
    NoiseEstimate n;
    MeasureWithNoise(luma, 0, 1, &n);  // the target's box gates the correction, as for the quality
    return n;
}

QualityEstimate RegionQuality(const float* p, int w, int rx0, int ry0, int rx1, int ry1, double b,
                              const NoiseEstimate& noise) {
    QualityEstimate out;
    if (rx1 <= rx0 || ry1 <= ry0) return out;
    const double sigma = noise.sigma;
    double gh = 0, gv = 0, e = 0, lap2 = 0;
    int64_t nLap = 0;
    for (int y = ry0; y < ry1; ++y) {
        const float* row = p + static_cast<size_t>(y) * w;
        const float* below = y + 1 < ry1 ? row + w : nullptr;
        const float* above = y > ry0 ? row - w : nullptr;
        for (int x = rx0; x < rx1; ++x) {
            const double v = row[x];
            e += (v - b) * (v - b);
            if (x + 1 < rx1) {
                const double d = static_cast<double>(row[x + 1]) - v;
                gh += d * d;
            }
            if (below) {
                const double d = static_cast<double>(below[x]) - v;
                gv += d * d;
            }
            if (above && below && x > rx0 && x + 1 < rx1) {  // 5-point Laplacian, for qualitySigma
                const double l = static_cast<double>(row[x - 1]) + row[x + 1] + above[x] + below[x] - 4 * v;
                lap2 += l * l;
                ++nLap;
            }
        }
    }
    const double nReg = static_cast<double>(rx1 - rx0) * (ry1 - ry0);
    const double nh = static_cast<double>(rx1 - rx0 - 1) * (ry1 - ry0);
    const double nv = static_cast<double>(rx1 - rx0) * (ry1 - ry0 - 1);
    const double var = sigma * sigma;
    double num = 0;
    if (nh > 0) num += gh / nh - 2 * var;
    if (nv > 0) num += gv / nv - 2 * var;
    const double den = e / nReg - var;
    if (!(num > 0 && den > 0)) return out;
    out.quality = std::sqrt(num / den);

    // qualitySigma: first-order propagation of the pixel noise (derivation in Measure.h), on the
    // region sums SN = nReg * num (gradient energy) and SD = nReg * den (signal energy).
    if (var > 0) {
        const double sn = nReg * num, sd = nReg * den;
        const double var2 = var * var;
        const double lapTrue = std::max(0.0, lap2 - 20.0 * var * static_cast<double>(nLap)) * nReg /
                               std::max<double>(1.0, static_cast<double>(nLap));
        const double vs = noise.varSigma2;
        const double varSN = 4 * var * lapTrue + 40 * nReg * var2 + 16 * nReg * nReg * vs;
        const double varSD = 4 * var * sd + 2 * nReg * var2 + nReg * nReg * vs;
        const double cov = 4 * var * sn + 8 * nReg * var2 + 4 * nReg * nReg * vs;
        const double rel2 = 0.25 * (varSN / (sn * sn) + varSD / (sd * sd) - 2 * cov / (sn * sd));
        out.sigma = out.quality * std::sqrt(std::max(0.0, rel2));
    }
    return out;
}

std::vector<FrameStats> MeasureAll(const SourceFactory& factory, const ser::FileInfo& info, int threads,
                                   const Progress& progress) {
    const int64_t total = std::max<int64_t>(0, info.frameCount);
    if (total == 0) return {};
    if (!factory) throw std::invalid_argument("MeasureAll: no reader factory");
    if (progress.cancelled && progress.cancelled()) throw StackCancelled("cancelled while measuring frames");

    constexpr int64_t kRun = 8;  // consecutive frames per claim: each reader reads forward
    const int workers = static_cast<int>(std::min<int64_t>(std::max(1, threads), (total + kRun - 1) / kRun));
    std::vector<FrameStats> out(static_cast<size_t>(total));
    const double scale = LumaScale(info);

    std::atomic<int64_t> nextFrame{0}, done{0};
    std::atomic<bool> stop{false};
    std::mutex m;
    std::condition_variable cv;
    std::exception_ptr error;
    int finished = 0;

    auto work = [&] {
        try {
            const std::unique_ptr<ser::FrameSource> src = factory();
            if (!src) throw ser::SerError("MeasureAll: the reader factory returned no reader");
            const ser::FileInfo& si = src->Info();
            auto describe = [](const ser::FileInfo& f) {
                return std::to_string(f.header.width) + "x" + std::to_string(f.header.height) + ", " +
                       std::to_string(f.frameCount) + " frames, " + ser::ColorIdName(f.colorId) + ", depth " +
                       std::to_string(f.normDepth);
            };
            if (si.frameCount != info.frameCount || si.header.width != info.header.width ||
                si.header.height != info.header.height || si.colorId != info.colorId || si.normDepth != info.normDepth)
                throw ser::SerError("MeasureAll: a reader opened " + describe(si) + "; expected " + describe(info));
            while (!stop.load(std::memory_order_relaxed)) {
                const int64_t first = nextFrame.fetch_add(kRun);
                if (first >= total) break;
                const int64_t last = std::min(total, first + kRun);
                for (int64_t i = first; i < last && !stop.load(std::memory_order_relaxed); ++i) {
                    out[static_cast<size_t>(i)] =
                        MeasureFrame(MeasurementLuma(src->ReadFrame(i), info), i, scale);
                    done.fetch_add(1);
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(m);
            if (!error) error = std::current_exception();
            stop = true;
        }
        std::lock_guard<std::mutex> lock(m);
        ++finished;
        cv.notify_all();
    };

    // Joins every started worker on every way out (including a throwing callback).
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

    pool.reserve(static_cast<size_t>(workers));
    for (int k = 0; k < workers; ++k) pool.emplace_back(work);

    if (progress.report) progress.report(kPhase, 0, total);
    int64_t reported = 0;
    bool cancelled = false;
    std::unique_lock<std::mutex> lock(m);
    while (finished < workers) {
        cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return finished == workers; });
        lock.unlock();
        const int64_t d = done.load();
        if (progress.report && d != reported) {
            progress.report(kPhase, d, total);
            reported = d;
        }
        if (!cancelled && progress.cancelled && progress.cancelled()) {
            cancelled = true;
            stop = true;
        }
        lock.lock();
    }
    lock.unlock();
    for (std::thread& t : pool) t.join();

    if (cancelled) throw StackCancelled("cancelled while measuring frames");
    if (error) std::rethrow_exception(error);
    if (progress.report && reported != total) progress.report(kPhase, total, total);
    return out;
}

}  // namespace planetary::stack
