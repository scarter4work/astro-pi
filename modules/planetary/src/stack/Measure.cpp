#include "Measure.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
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
constexpr double kSeedSigmas = 4;            // component seeds: blocks above b + 4 s_b
constexpr double kPeakSigmas = 10;           // a target's brightest block: >= b + 10 s_b ...
constexpr double kMinContrast = 0.01;        // ... and >= b + 1% of full scale
constexpr double kExtentFraction = 0.1;      // extent: above 10% of the contrast
constexpr double kCentroidFraction = 0.5;    // centroid: above half the contrast
constexpr int64_t kMinTargetPixels = 9;
constexpr double kMarginFraction = 0.1;      // quality region: box + 10%

const char* const kPhase = "Measuring frames";

// The q-quantile (lower element) of v; v is reordered.
double Quantile(std::vector<double>& v, double q) {
    const size_t k = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

// Pixel noise: from the smoothest tiles' mean squared horizontal neighbour difference
// (white noise: E[d^2] = 2 s^2). Tiles with fewer than half a full tile's pairs are skipped.
double PixelNoise(const float* p, int w, int h) {
    if (w < 2) return 0;
    const int tile = std::max(1, std::min({kMaxTile, std::max(8, std::min(w, h) / 4), std::min(w, h)}));
    const int tx = (w + tile - 1) / tile, ty = (h + tile - 1) / tile;
    std::vector<double> sum(static_cast<size_t>(tx) * ty, 0.0);
    std::vector<int64_t> cnt(sum.size(), 0);
    for (int y = 0; y < h; ++y) {
        const float* row = p + static_cast<size_t>(y) * w;
        const size_t trow = static_cast<size_t>(y / tile) * tx;
        for (int x0 = 0; x0 < w - 1; x0 += tile) {
            const int x1 = std::min(w - 1, x0 + tile);
            double s = 0;
            for (int x = x0; x < x1; ++x) {
                const double d = static_cast<double>(row[x + 1]) - row[x];
                s += d * d;
            }
            sum[trow + x0 / tile] += s;
            cnt[trow + x0 / tile] += x1 - x0;
        }
    }
    const int64_t full = static_cast<int64_t>(std::min(tile, w - 1)) * std::min(tile, h);
    std::vector<double> est;
    est.reserve(sum.size());
    for (size_t i = 0; i < sum.size(); ++i)
        if (cnt[i] > 0 && 2 * cnt[i] >= full) est.push_back(sum[i] / static_cast<double>(cnt[i]));
    if (est.empty()) return 0;
    return std::sqrt(Quantile(est, kNoisePercentile) / 2);
}

struct Grid {
    int d = 1, cw = 0, ch = 0;
    std::vector<double> mean;   // block means
    std::vector<int64_t> area;  // pixels per block (edge blocks may be partial)
};

// Block means of the despiked image: each pixel is first held to the largest of its 4
// neighbours, so a hot pixel (or a lone noise spike) cannot lift a block into a "target".
Grid BlockMeans(const float* p, int w, int h) {
    Grid g;
    g.d = DetectionBlock(w, h);
    g.cw = (w + g.d - 1) / g.d;
    g.ch = (h + g.d - 1) / g.d;
    g.mean.assign(static_cast<size_t>(g.cw) * g.ch, 0.0);
    g.area.assign(g.mean.size(), 0);
    const bool alone = w == 1 && h == 1;
    for (int y = 0; y < h; ++y) {
        const float* row = p + static_cast<size_t>(y) * w;
        const float* up = y > 0 ? row - w : nullptr;
        const float* down = y + 1 < h ? row + w : nullptr;
        const size_t grow = static_cast<size_t>(y / g.d) * g.cw;
        for (int x = 0; x < w; ++x) {
            float v = row[x];
            if (!alone) {
                float m = -std::numeric_limits<float>::infinity();
                if (x > 0) m = std::max(m, row[x - 1]);
                if (x + 1 < w) m = std::max(m, row[x + 1]);
                if (up) m = std::max(m, up[x]);
                if (down) m = std::max(m, down[x]);
                v = std::min(v, m);
            }
            g.mean[grow + x / g.d] += v;
        }
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

double LumaScale(const ser::FileInfo& info) { return ser::IsBayer(info.colorId) ? 2.0 : 1.0; }

int DetectionBlock(int width, int height) {
    return std::max(1, (std::max(width, height) + kGridCells - 1) / kGridCells);
}

FrameStats MeasureFrame(const ser::FloatImage& luma, int64_t index) {
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
    const float* p = luma.data.data();
    auto at = [&](int x, int y) { return static_cast<double>(p[static_cast<size_t>(y) * w + x]); };

    FrameStats s;
    s.index = index;

    const Grid g = BlockMeans(p, w, h);
    const double sigma = PixelNoise(p, w, h);
    std::vector<double> sorted = g.mean;
    const double b = Quantile(sorted, kBackgroundPercentile);
    const double sigmaB = std::max(BlockNoise(g), sigma / g.d);

    // Components above the seed threshold; the candidate with the most flux is the target.
    std::vector<int> labels(g.mean.size(), 0);
    const double seedThr = b + kSeedSigmas * sigmaB;
    size_t bestPeak = 0;
    double bestFlux = -1;
    int next = 1;
    for (size_t i = 0; i < g.mean.size(); ++i) {
        if (labels[i] != 0 || g.mean[i] <= seedThr) continue;
        size_t peak = i;
        double flux = 0;
        int64_t area = 0;
        Flood(g, seedThr, i, next++, labels, [&](size_t j) {
            if (g.mean[j] > g.mean[peak] || (g.mean[j] == g.mean[peak] && j < peak)) peak = j;
            flux += (g.mean[j] - b) * static_cast<double>(g.area[j]);
            area += g.area[j];
        });
        const double contrast = g.mean[peak] - b;
        if (contrast >= std::max(kPeakSigmas * sigmaB, kMinContrast) && area >= kMinTargetPixels && flux > bestFlux) {
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

    // Target pixels, brightness, clipping, centroid.
    const double cenThr = b + kCentroidFraction * contrast;
    double sum = 0, wsum = 0, wx = 0, wy = 0;
    int64_t n = 0, clipped = 0;
    for (int y = s.by0; y < s.by1; ++y)
        for (int x = s.bx0; x < s.bx1; ++x) {
            const double v = at(x, y);
            if (v > extThr) {
                sum += v;
                ++n;
                if (v >= kClippedLuma) ++clipped;
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

    // Quality over the box plus a margin.
    const int margin = std::max(1, static_cast<int>(std::lround(kMarginFraction * std::max(s.bx1 - s.bx0, s.by1 - s.by0))));
    const int rx0 = std::max(0, s.bx0 - margin), ry0 = std::max(0, s.by0 - margin);
    const int rx1 = std::min(w, s.bx1 + margin), ry1 = std::min(h, s.by1 + margin);
    double gh = 0, gv = 0, e = 0;
    for (int y = ry0; y < ry1; ++y) {
        const float* row = p + static_cast<size_t>(y) * w;
        const float* below = y + 1 < ry1 ? row + w : nullptr;
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
        }
    }
    const double nh = static_cast<double>(rx1 - rx0 - 1) * (ry1 - ry0);
    const double nv = static_cast<double>(rx1 - rx0) * (ry1 - ry0 - 1);
    const double var = sigma * sigma;
    double num = 0;
    if (nh > 0) num += gh / nh - 2 * var;
    if (nv > 0) num += gv / nv - 2 * var;
    const double den = e / (static_cast<double>(rx1 - rx0) * (ry1 - ry0)) - var;
    s.quality = num > 0 && den > 0 ? std::sqrt(num / den) : 0.0;
    return s;
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
            if (si.frameCount != info.frameCount || si.header.width != info.header.width ||
                si.header.height != info.header.height)
                throw ser::SerError("MeasureAll: a reader opened " + std::to_string(si.header.width) + "x" +
                                    std::to_string(si.header.height) + " with " + std::to_string(si.frameCount) +
                                    " frames, expected " + std::to_string(info.header.width) + "x" +
                                    std::to_string(info.header.height) + " with " +
                                    std::to_string(info.frameCount));
            while (!stop.load(std::memory_order_relaxed)) {
                const int64_t first = nextFrame.fetch_add(kRun);
                if (first >= total) break;
                const int64_t last = std::min(total, first + kRun);
                for (int64_t i = first; i < last && !stop.load(std::memory_order_relaxed); ++i) {
                    out[static_cast<size_t>(i)] =
                        MeasureFrame(LumaForMeasure(src->ReadFrame(i), info.colorId, info.normDepth), i);
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
