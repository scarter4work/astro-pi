#include "Align.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "Demosaic.h"
#include "Parallel.h"
#include "SerFile.h"

namespace planetary::stack {

namespace {

constexpr double kTaper = 1.0 / 3;   // Tukey: share of the window inside the two cosine tapers
constexpr int kMinWindow = 32;
constexpr int kNewtonSteps = 20;
constexpr double kNewtonStop = 1e-4;  // px: two orders below the accuracy the data allow
// Correlation passes: the tapered windows weight content shifted by d unequally, which pulls the
// peak toward 0 by ~2% of d (measured: 31.2 px -> 30.6); after re-centring on the integer peak the
// residual is under a pixel and the pull under ~0.02 px. Three passes cover a peak first found
// several pixels off (the second pass then lands within a pixel).
constexpr int kRecentrePasses = 3;
// Mean-square window signal below which a window counts as flat (one 16-bit step is 1.5e-5).
constexpr double kNoSignal = 1e-20;
const char* const kRefPhase = "Building reference";

void CheckLuma(const ser::FloatImage& im, const char* what) {
    if (im.width <= 0 || im.height <= 0) throw std::invalid_argument(std::string(what) + ": empty image");
    if (im.channels != 1)
        throw std::invalid_argument(std::string(what) + ": expected a 1-channel luma image, got " +
                                    std::to_string(im.channels) + " channels");
    if (im.data.size() != static_cast<size_t>(im.width) * im.height)
        throw std::invalid_argument(std::string(what) + ": " + std::to_string(im.data.size()) + " samples for a " +
                                    std::to_string(im.width) + "x" + std::to_string(im.height) + " image");
}

std::vector<double> Tukey(int n) {
    std::vector<double> w(static_cast<size_t>(n));
    const double edge = kTaper / 2;
    for (int i = 0; i < n; ++i) {
        const double u = (i + 0.5) / n;
        const double e = std::min(u, 1 - u);
        w[static_cast<size_t>(i)] = e >= edge ? 1.0 : 0.5 * (1 - std::cos(M_PI * e / edge));
    }
    return w;
}

// Signed frequency of DFT bin k of n; Nyquist (k = n / 2, n even) is reported as 0 and flagged.
int Signed(int k, int n) { return 2 * k < n ? k : k - n; }
bool Nyquist(int k, int n) { return n % 2 == 0 && 2 * k == n; }

// Value, gradient and Hessian of c(x, y) = Re sum_k C(k) e^{i (wx x + wy y)} / (W H).
struct Local {
    double v = 0, gx = 0, gy = 0, gxx = 0, gxy = 0, gyy = 0;
};

Local Evaluate(const std::vector<Complex>& c, int w, int h, double x, double y) {
    std::vector<Complex> ex(static_cast<size_t>(w));
    std::vector<double> omx(static_cast<size_t>(w));
    for (int k = 0; k < w; ++k) {
        omx[static_cast<size_t>(k)] = 2 * M_PI * Signed(k, w) / w;
        ex[static_cast<size_t>(k)] = std::polar(1.0, omx[static_cast<size_t>(k)] * x);
    }
    Local r;
    for (int ky = 0; ky < h; ++ky) {
        const Complex* row = c.data() + static_cast<size_t>(ky) * w;
        Complex s0 = 0, s1 = 0, s2 = 0;
        for (int kx = 0; kx < w; ++kx) {
            const Complex t = row[kx] * ex[static_cast<size_t>(kx)];
            const double o = omx[static_cast<size_t>(kx)];
            s0 += t;
            s1 += t * o;
            s2 += t * (o * o);
        }
        const double oy = 2 * M_PI * Signed(ky, h) / h;
        const Complex ey = std::polar(1.0, oy * y);
        const Complex a0 = ey * s0, a1 = ey * s1, a2 = ey * s2;
        // d/dx e^{i o x} = i o e^{i o x}: Re(i z) = -Im z.
        r.v += a0.real();
        r.gx += -a1.imag();
        r.gy += -oy * a0.imag();
        r.gxx += -a2.real();
        r.gyy += -oy * oy * a0.real();
        r.gxy += -oy * a1.real();
    }
    const double n = static_cast<double>(w) * h;
    r.v /= n;
    r.gx /= n;
    r.gy /= n;
    r.gxx /= n;
    r.gxy /= n;
    r.gyy /= n;
    return r;
}

// Catmull-Rom weights for fractional offset t in [0, 1).
void CubicWeights(double t, double w[4]) {
    const double t2 = t * t, t3 = t2 * t;
    w[0] = 0.5 * (-t3 + 2 * t2 - t);
    w[1] = 0.5 * (3 * t3 - 5 * t2 + 2);
    w[2] = 0.5 * (-3 * t3 + 4 * t2 + t);
    w[3] = 0.5 * (t3 - t2);
}

// Bicubic sample of `im` at (x, y), neighbours clamped to the image.
double SampleCubic(const ser::FloatImage& im, double x, double y) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    double wx[4], wy[4];
    CubicWeights(x - x0, wx);
    CubicWeights(y - y0, wy);
    double acc = 0;
    for (int j = 0; j < 4; ++j) {
        const int yy = std::clamp(y0 - 1 + j, 0, im.height - 1);
        const float* row = im.data.data() + static_cast<size_t>(yy) * im.width;
        double r = 0;
        for (int i = 0; i < 4; ++i) r += wx[i] * row[std::clamp(x0 - 1 + i, 0, im.width - 1)];
        acc += wy[j] * r;
    }
    return acc;
}

// Mean of the frames' samples at (x + dx, y + dy) that fall inside the frame; uncovered pixels take
// frame `fallback` (a frame in use) with clamped coordinates. Rows in `threads` bands; deterministic.
ser::FloatImage Average(const std::vector<ser::FloatImage>& frames, const std::vector<Shift>& shifts,
                        const std::vector<bool>& use, size_t fallback, int threads) {
    const int w = frames[0].width, h = frames[0].height;
    ser::FloatImage out{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.f)};
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                double sum = 0;
                int n = 0;
                for (size_t i = 0; i < frames.size(); ++i) {
                    if (!use[i]) continue;
                    const double sx = x + shifts[i].dx, sy = y + shifts[i].dy;
                    if (sx < 0 || sy < 0 || sx > w - 1 || sy > h - 1) continue;
                    sum += SampleCubic(frames[i], sx, sy);
                    ++n;
                }
                out.data[static_cast<size_t>(y) * w + x] = static_cast<float>(
                    n > 0 ? sum / n : SampleCubic(frames[fallback], x + shifts[fallback].dx, y + shifts[fallback].dy));
            }
    });
    return out;
}

// Separable Gaussian blur (sigma px, edges clamped) of a 1-channel image, in double.
std::vector<double> Blurred(const ser::FloatImage& im, double sigma) {
    const int w = im.width, h = im.height, rad = std::max(1, static_cast<int>(std::ceil(3 * sigma)));
    std::vector<double> k(static_cast<size_t>(2 * rad + 1));
    double sum = 0;
    for (int i = -rad; i <= rad; ++i) sum += k[static_cast<size_t>(i + rad)] = std::exp(-0.5 * i * i / (sigma * sigma));
    for (double& v : k) v /= sum;
    std::vector<double> tmp(static_cast<size_t>(w) * h), out(tmp.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double a = 0;
            for (int i = -rad; i <= rad; ++i)
                a += k[static_cast<size_t>(i + rad)] * im.data[static_cast<size_t>(y) * w + std::clamp(x + i, 0, w - 1)];
            tmp[static_cast<size_t>(y) * w + x] = a;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double a = 0;
            for (int i = -rad; i <= rad; ++i)
                a += k[static_cast<size_t>(i + rad)] * tmp[static_cast<size_t>(std::clamp(y + i, 0, h - 1)) * w + x];
            out[static_cast<size_t>(y) * w + x] = a;
        }
    return out;
}

// Full frame: the ww x wh window position inside the image holding the most structure -- the summed
// squared gradient of the reference low-passed like the correlation sees it -- so a crescent or a
// terminator off-centre is what the window covers. Positions on a grid of step max(1, min side / 32);
// ties go to the position nearest the frame centre. Returns the window's top-left corner.
std::pair<int, int> BestWindow(const ser::FloatImage& ref, int ww, int wh) {
    const int w = ref.width, h = ref.height;
    if (ww >= w && wh >= h) return {0, 0};
    const std::vector<double> b = Blurred(ref, kAlignLowPassSigma);
    // Integral image of the squared central-difference gradient.
    std::vector<double> integral(static_cast<size_t>(w + 1) * (h + 1), 0.0);
    for (int y = 0; y < h; ++y) {
        double row = 0;
        for (int x = 0; x < w; ++x) {
            const double gx = 0.5 * (b[static_cast<size_t>(y) * w + std::min(x + 1, w - 1)] -
                                     b[static_cast<size_t>(y) * w + std::max(x - 1, 0)]);
            const double gy = 0.5 * (b[static_cast<size_t>(std::min(y + 1, h - 1)) * w + x] -
                                     b[static_cast<size_t>(std::max(y - 1, 0)) * w + x]);
            row += gx * gx + gy * gy;
            integral[static_cast<size_t>(y + 1) * (w + 1) + x + 1] = integral[static_cast<size_t>(y) * (w + 1) + x + 1] + row;
        }
    }
    auto box = [&](int x0, int y0) {
        const size_t W = static_cast<size_t>(w + 1);
        return integral[(y0 + wh) * W + x0 + ww] - integral[y0 * W + x0 + ww] - integral[(y0 + wh) * W + x0] +
               integral[y0 * W + x0];
    };
    const int step = std::max(1, std::min(ww, wh) / 32);
    const int cx = (w - ww) / 2, cy = (h - wh) / 2;
    auto positions = [&](int span) {
        std::vector<int> p;
        for (int v = 0; v <= span; v += step) p.push_back(v);
        if (p.back() != span) p.push_back(span);
        return p;
    };
    std::pair<int, int> best{cx, cy};
    double bestE = box(cx, cy);
    for (int y0 : positions(h - wh))
        for (int x0 : positions(w - ww)) {
            const double e = box(x0, y0);
            const auto d = [&](int x, int y) { return std::abs(x - cx) + std::abs(y - cy); };
            if (e > bestE || (e == bestE && d(x0, y0) < d(best.first, best.second))) {
                bestE = e;
                best = {x0, y0};
            }
        }
    return best;
}

}  // namespace

GlobalAligner::GlobalAligner(const ser::FloatImage& reference, bool fullFrame)
    : fullFrame_(fullFrame),
      width_(reference.width),
      height_(reference.height),
      ww_(0),
      wh_(0),
      refCx_(0),
      refCy_(0),
      fft_(1, 1) {
    CheckLuma(reference, "GlobalAligner");
    if (fullFrame_) {
        ww_ = PrevFastSize(std::min(width_, kMaxAlignWindow));
        wh_ = PrevFastSize(std::min(height_, kMaxAlignWindow));
        const std::pair<int, int> at = BestWindow(reference, ww_, wh_);
        refCx_ = at.first + ww_ / 2;
        refCy_ = at.second + wh_ / 2;
    } else {
        const FrameStats rs = MeasureFrame(reference, 0);
        if (!rs.targetFound) throw std::invalid_argument("GlobalAligner: the reference has no target to align on");
        const int box = std::max(rs.bx1 - rs.bx0, rs.by1 - rs.by0);
        int n = NextFastSize(static_cast<int>(std::ceil(1.5 * box)) + 16);
        n = std::min(std::max(n, kMinWindow), PrevFastSize(kMaxAlignWindow));
        ww_ = wh_ = n;
        refCx_ = static_cast<int>(std::lround(rs.cx));
        refCy_ = static_cast<int>(std::lround(rs.cy));
    }
    fft_ = Fft2D(ww_, wh_);
    taperX_ = Tukey(ww_);
    taperY_ = Tukey(wh_);
    lowPass2_.assign(static_cast<size_t>(ww_) * wh_, 0.0);
    const double k = 4 * M_PI * M_PI * kAlignLowPassSigma * kAlignLowPassSigma;
    for (int ky = 0; ky < wh_; ++ky)
        for (int kx = 0; kx < ww_; ++kx) {
            if (Nyquist(kx, ww_) || Nyquist(ky, wh_)) continue;
            const double fx = static_cast<double>(Signed(kx, ww_)) / ww_, fy = static_cast<double>(Signed(ky, wh_)) / wh_;
            const double f2 = fx * fx + fy * fy;
            lowPass2_[static_cast<size_t>(ky) * ww_ + kx] = 4 * M_PI * M_PI * f2 * std::exp(-k * f2);
        }
    // Extra blurs of the reference for the confidence: separable Gaussian factors per axis.
    const size_t nb = std::size(kConfidenceBlurs);
    blurX_.assign(nb * static_cast<size_t>(ww_), 0.0);
    blurY_.assign(nb * static_cast<size_t>(wh_), 0.0);
    for (size_t j = 0; j < nb; ++j) {
        const double s2 = 2 * M_PI * M_PI * kConfidenceBlurs[j] * kConfidenceBlurs[j];
        for (int kx = 0; kx < ww_; ++kx) {
            const double fx = static_cast<double>(Signed(kx, ww_)) / ww_;
            blurX_[static_cast<size_t>(kx) * nb + j] = std::exp(-s2 * fx * fx);
        }
        for (int ky = 0; ky < wh_; ++ky) {
            const double fy = static_cast<double>(Signed(ky, wh_)) / wh_;
            blurY_[j * wh_ + ky] = std::exp(-s2 * fy * fy);
        }
    }
    Window(reference, refCx_, refCy_, refSpec_);
    fft_.Forward(refSpec_);
    refEnergy_.assign(nb, 0.0);
    for (int ky = 0; ky < wh_; ++ky)
        for (int kx = 0; kx < ww_; ++kx) {
            const size_t i = static_cast<size_t>(ky) * ww_ + kx;
            const double p = std::norm(refSpec_[i]) * lowPass2_[i];
            for (size_t j = 0; j < nb; ++j) {
                const double b = blurX_[static_cast<size_t>(kx) * nb + j] * blurY_[j * wh_ + ky];
                refEnergy_[j] += p * b * b;
            }
            refSpec_[i] = std::conj(refSpec_[i]) * lowPass2_[i];
        }
    for (double& e : refEnergy_) e /= static_cast<double>(ww_) * wh_;
}

// The window of `im` centred on (cx, cy) (top-left cx - ww/2, cy - wh/2), edge-repeated outside the
// image, weighted mean removed, tapered.
void GlobalAligner::Window(const ser::FloatImage& im, int cx, int cy, std::vector<Complex>& out) const {
    const int x0 = cx - ww_ / 2, y0 = cy - wh_ / 2;
    out.assign(static_cast<size_t>(ww_) * wh_, Complex(0, 0));
    double sw = 0, swv = 0;
    for (int j = 0; j < wh_; ++j) {
        const int y = std::clamp(y0 + j, 0, height_ - 1);
        const float* row = im.data.data() + static_cast<size_t>(y) * width_;
        for (int i = 0; i < ww_; ++i) {
            const double v = row[std::clamp(x0 + i, 0, width_ - 1)];
            const double wt = taperX_[static_cast<size_t>(i)] * taperY_[static_cast<size_t>(j)];
            out[static_cast<size_t>(j) * ww_ + i] = Complex(v, 0);
            sw += wt;
            swv += wt * v;
        }
    }
    const double mean = swv / sw;
    for (int j = 0; j < wh_; ++j)
        for (int i = 0; i < ww_; ++i) {
            Complex& c = out[static_cast<size_t>(j) * ww_ + i];
            c = Complex((c.real() - mean) * taperX_[static_cast<size_t>(i)] * taperY_[static_cast<size_t>(j)], 0);
        }
}

Shift GlobalAligner::Align(const ser::FloatImage& frameLuma, const FrameStats& frameStats) const {
    CheckLuma(frameLuma, "GlobalAligner::Align");
    if (frameLuma.width != width_ || frameLuma.height != height_)
        throw std::invalid_argument("GlobalAligner::Align: frame is " + std::to_string(frameLuma.width) + "x" +
                                    std::to_string(frameLuma.height) + ", the reference " + std::to_string(width_) +
                                    "x" + std::to_string(height_));
    int fcx = refCx_, fcy = refCy_;
    if (!fullFrame_ && frameStats.targetFound) {
        fcx = static_cast<int>(std::lround(frameStats.cx));
        fcy = static_cast<int>(std::lround(frameStats.cy));
    }
    // Correlate; while the integer peak is more than a pixel off, move the frame's window onto it
    // and correlate again, so the final residual is sub-pixel (see kRecentrePasses). Only the last
    // correlation is refined.
    for (int pass = 0;; ++pass) {
        const Peak p = Correlate(frameLuma, fcx, fcy, pass + 1 < kRecentrePasses);
        if (p.recentre) {
            fcx += p.ix;
            fcy += p.iy;
            continue;
        }
        Shift s;
        s.dx = fcx - refCx_ + p.x;
        s.dy = fcy - refCy_ + p.y;
        s.confidence = p.confidence;
        return s;
    }
}

GlobalAligner::Peak GlobalAligner::Correlate(const ser::FloatImage& frameLuma, int fcx, int fcy,
                                             bool mayRecentre) const {
    Peak result;
    std::vector<Complex> c;
    Window(frameLuma, fcx, fcy, c);
    fft_.Forward(c);
    const double n = static_cast<double>(ww_) * wh_;
    double e = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        e += std::norm(c[i]) * lowPass2_[i];
        c[i] *= refSpec_[i];
    }
    const double energy = e / n;
    // A window with no variation (mean square far below one 16-bit step) has nothing to align on.
    if (!(energy > kNoSignal * n) || !(refEnergy_[0] > kNoSignal * n)) return result;
    std::vector<Complex> cc = c;
    fft_.Inverse(cc);

    // Integer peak within the search range.
    const int limX = fullFrame_ ? ww_ / 2 - 1 : ww_ / 4, limY = fullFrame_ ? wh_ / 2 - 1 : wh_ / 4;
    int px = 0, py = 0;
    double best = -1e300;
    for (int ky = 0; ky < wh_; ++ky) {
        const int sy = Signed(ky, wh_);
        if (std::abs(sy) > limY) continue;
        for (int kx = 0; kx < ww_; ++kx) {
            const int sx = Signed(kx, ww_);
            if (std::abs(sx) > limX) continue;
            const double v = cc[static_cast<size_t>(ky) * ww_ + kx].real();
            if (v > best) {
                best = v;
                px = sx;
                py = sy;
            }
        }
    }
    if (mayRecentre && (std::abs(px) > 1 || std::abs(py) > 1)) {
        result.ix = px;
        result.iy = py;
        result.recentre = true;
        return result;
    }
    auto at = [&](int sx, int sy) {
        const int kx = (sx % ww_ + ww_) % ww_, ky = (sy % wh_ + wh_) % wh_;
        return cc[static_cast<size_t>(ky) * ww_ + kx].real();
    };
    auto parabola = [](double m, double z, double p) {
        const double d = m - 2 * z + p;
        return d < 0 ? std::clamp(0.5 * (m - p) / d, -0.5, 0.5) : 0.0;
    };
    // Parabolic start, then Newton on the exact trigonometric interpolant.
    const double x0 = px + parabola(at(px - 1, py), best, at(px + 1, py));
    const double y0 = py + parabola(at(px, py - 1), best, at(px, py + 1));
    double x = x0, y = y0;
    Local l = Evaluate(c, ww_, wh_, x, y);
    for (int it = 0; it < kNewtonSteps; ++it) {
        const double det = l.gxx * l.gyy - l.gxy * l.gxy;
        if (!(l.gxx < 0 && det > 0)) break;  // not a maximum's neighbourhood: keep the current point
        double dx = -(l.gyy * l.gx - l.gxy * l.gy) / det;
        double dy = -(l.gxx * l.gy - l.gxy * l.gx) / det;
        const double len = std::hypot(dx, dy);
        if (len > 0.5) {
            dx *= 0.5 / len;
            dy *= 0.5 / len;
        }
        x += dx;
        y += dy;
        l = Evaluate(c, ww_, wh_, x, y);
        if (len < kNewtonStop) break;
    }
    if (std::fabs(x - px) > 1.5 || std::fabs(y - py) > 1.5) {  // wandered off: the parabolic estimate
        x = x0;
        y = y0;
        l = Evaluate(c, ww_, wh_, x, y);
    }
    result.ix = px;
    result.iy = py;
    result.x = x;
    result.y = y;
    // Confidence: the best normalised correlation at (x, y) over the reference blurred by each of
    // kConfidenceBlurs (blur multiplies the reference spectrum by a separable Gaussian B_j), so a
    // frame blurred by seeing is compared with a reference as blurred as it is:
    //   v_j = Re sum_k C(k) B_j(k) e^{i w.(x, y)} / (W H),  ncc_j = v_j / sqrt(E_frame E_ref,j).
    const size_t nb = refEnergy_.size();
    std::vector<Complex> ex(static_cast<size_t>(ww_));
    for (int kx = 0; kx < ww_; ++kx) ex[static_cast<size_t>(kx)] = std::polar(1.0, 2 * M_PI * Signed(kx, ww_) * x / ww_);
    std::vector<double> v(nb, 0.0);
    std::vector<Complex> rowSum(nb);
    for (int ky = 0; ky < wh_; ++ky) {
        std::fill(rowSum.begin(), rowSum.end(), Complex(0, 0));
        const Complex* row = c.data() + static_cast<size_t>(ky) * ww_;
        for (int kx = 0; kx < ww_; ++kx) {
            const Complex t = row[kx] * ex[static_cast<size_t>(kx)];
            const double* bx = blurX_.data() + static_cast<size_t>(kx) * nb;
            for (size_t j = 0; j < nb; ++j) rowSum[j] += t * bx[j];
        }
        const Complex ey = std::polar(1.0, 2 * M_PI * Signed(ky, wh_) * y / wh_);
        for (size_t j = 0; j < nb; ++j) v[j] += (ey * rowSum[j]).real() * blurY_[j * wh_ + ky];
    }
    double conf = 0;
    for (size_t j = 0; j < nb; ++j) conf = std::max(conf, v[j] / n / std::sqrt(energy * refEnergy_[j]));
    result.confidence = std::clamp(conf, 0.0, 1.0);
    return result;
}

Shift GlobalShift(const ser::FloatImage& reference, const ser::FloatImage& frameLuma, const FrameStats& frameStats) {
    CheckLuma(reference, "GlobalShift");
    const FrameStats rs = MeasureFrame(reference, 0);
    const bool full = !rs.targetFound || FillsFrame(rs, reference.width, reference.height);
    return GlobalAligner(reference, full).Align(frameLuma, frameStats);
}

ser::FloatImage BuildReference(const SourceFactory& factory, const ser::FileInfo& info, const Selection& selection,
                               const std::vector<FrameStats>& stats, int threads, const Progress& progress) {
    if (selection.frames.empty())
        throw std::invalid_argument("BuildReference: the selection holds no frames (" + selection.diagnosis.message + ")");
    if (!factory) throw std::invalid_argument("BuildReference: no reader factory");
    std::map<int64_t, const FrameStats*> byIndex;
    for (const FrameStats& s : stats) byIndex[s.index] = &s;
    std::vector<const FrameStats*> ranked;
    ranked.reserve(selection.frames.size());
    for (int64_t i : selection.frames) {
        const auto it = byIndex.find(i);
        if (it == byIndex.end())
            throw std::invalid_argument("BuildReference: selected frame " + std::to_string(i) + " has no statistics");
        ranked.push_back(it->second);
    }
    std::sort(ranked.begin(), ranked.end(), [](const FrameStats* a, const FrameStats* b) {
        return a->quality != b->quality ? a->quality > b->quality : a->index < b->index;
    });
    ranked.resize(std::min(ranked.size(), static_cast<size_t>(kReferenceFrames)));
    const size_t k = ranked.size();
    int64_t total = 3 * static_cast<int64_t>(k);  // grows by k for each anchor that is turned down
    int64_t done = 0;
    auto tick = [&] {
        if (progress.cancelled && progress.cancelled()) throw StackCancelled("cancelled while building the reference");
        if (progress.report) progress.report(kRefPhase, done, total);
    };
    tick();

    // Read in frame order (the reader reads forward); frames[i] is ranked[i], frames[0] the sharpest.
    std::vector<size_t> order(k);
    for (size_t i = 0; i < k; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ranked[a]->index < ranked[b]->index; });
    const std::unique_ptr<ser::FrameSource> src = factory();
    if (!src) throw ser::SerError("BuildReference: the reader factory returned no reader");
    const ser::FileInfo& si = src->Info();
    if (si.frameCount != info.frameCount || si.header.width != info.header.width ||
        si.header.height != info.header.height || si.colorId != info.colorId || si.normDepth != info.normDepth)
        throw ser::SerError("BuildReference: the reader does not match the capture it was asked for");
    std::vector<ser::FloatImage> frames(k);
    for (size_t i : order) {
        frames[i] = MeasurementLuma(src->ReadFrame(ranked[i]->index), info);
        ++done;
        tick();
    }
    if (k == 1) {
        if (progress.report) progress.report(kRefPhase, total, total);
        return frames[0];
    }

    const bool full = selection.fullFrameTarget;
    std::vector<Shift> shifts(k);
    std::vector<bool> use(k);
    // Pass 1: onto an anchor the other frames confirm. The sharpest frame first; it is turned down
    // when fewer than half of the others reach AlignmentCut against it (cut over the others only --
    // its self-match is not evidence), and the next sharpest is tried -- through every frame read until
    // one is confirmed (a sharp group of foreign frames confirms itself only if it is at least half of
    // the frames read). None confirmed: the candidate most others agreed with (the sharper on a tie).
    size_t anchor = 0;
    {
        int bestCount = -1;
        std::vector<Shift> bestShifts;
        bool tried = false;
        for (size_t a = 0; a < k; ++a) {
            if (!full && !ranked[a]->targetFound) continue;  // a disc anchor needs a target
            if (tried) {
                total += static_cast<int64_t>(k);
                tick();
            }
            tried = true;
            const GlobalAligner aligner(frames[a], full);
            std::vector<Shift> s(k), others;
            for (size_t i = 0; i < k; ++i) {
                if (i == a) continue;
                s[i] = aligner.Align(frames[i], *ranked[i]);
                others.push_back(s[i]);
                ++done;
                tick();
            }
            s[a] = Shift{0, 0, 1};
            const double cut = AlignmentCut(others);
            int count = 0;
            for (const Shift& o : others) count += o.confidence >= cut ? 1 : 0;
            if (count > bestCount) {
                bestCount = count;
                bestShifts = s;
                anchor = a;
            }
            ++done;  // the anchor's own slot
            tick();
            if (2 * static_cast<size_t>(count) >= others.size()) break;
        }
        if (bestCount < 0) throw std::invalid_argument("BuildReference: no selected frame has a target to align on");
        shifts = bestShifts;
        std::vector<Shift> others;
        for (size_t i = 0; i < k; ++i)
            if (i != anchor) others.push_back(shifts[i]);
        const double cut = AlignmentCut(others);
        for (size_t i = 0; i < k; ++i) use[i] = i == anchor || shifts[i].confidence >= cut;
    }
    const ser::FloatImage first = Average(frames, shifts, use, anchor, threads);

    // Pass 2: onto that average, re-centred on the mean shift. The frames' stats centroids still
    // place the windows (they are positions in each frame, not relative to any reference).
    const GlobalAligner aligner(first, full);
    for (size_t i = 0; i < k; ++i) {
        shifts[i] = aligner.Align(frames[i], *ranked[i]);
        ++done;
        tick();
    }
    const double cut = AlignmentCut(shifts);
    double mx = 0, my = 0;
    int used = 0;
    size_t bestKept = k;  // the sharpest frame kept: covers pixels no shifted frame reaches
    for (size_t i = 0; i < k; ++i) {
        use[i] = shifts[i].confidence >= cut;
        if (use[i]) {
            mx += shifts[i].dx;
            my += shifts[i].dy;
            ++used;
            bestKept = std::min(bestKept, i);
        }
    }
    if (used == 0) return first;
    mx /= used;
    my /= used;
    for (Shift& s : shifts) {
        s.dx -= mx;
        s.dy -= my;
    }
    return Average(frames, shifts, use, bestKept, threads);
}


double AlignmentCut(const std::vector<Shift>& shifts) {
    std::vector<double> ok;
    for (const Shift& s : shifts)
        if (s.confidence >= kMinAlignConfidence) ok.push_back(s.confidence);
    if (ok.empty()) return kMinAlignConfidence;
    const size_t mid = (ok.size() - 1) / 2;
    std::nth_element(ok.begin(), ok.begin() + static_cast<std::ptrdiff_t>(mid), ok.end());
    return std::max(kMinAlignConfidence, kAlignRelative * ok[mid]);
}

}  // namespace planetary::stack
