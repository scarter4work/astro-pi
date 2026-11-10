// DAOFIND star detection: an exact port of photutils 3.0.0 DAOStarFinder.
//
// Ported line for line from photutils 3.0.0:
//   detection/daofinder.py   DAOStarFinder._get_raw_catalog, _DAOStarFinderCatalog
//   detection/core.py        StarFinderBase._find_stars, StarFinderCatalogBase, _StarFinderKernel
//   detection/peakfinder.py  find_peaks, _fast_circular_peaks, _verify_ring_candidates
//   utils/_convolution.py    _filter_data (scipy.ndimage.convolve, mode='constant', cval=0)
//   utils/cutouts.py         _make_cutouts (fill 0 outside the image)
// keeping numpy's reduction order for every sum (pairwise over a flattened block, sequential
// down a non-innermost axis) so the results match photutils to the last bit where the
// transcendental functions agree (numpy's SIMD exp differs from libm's in the last ulp).
//
// photutils licence (3-clause BSD), reproduced as its clause 1 requires:
//
//   Copyright (c) 2011-2026, Photutils Developers
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are
//   met:
//
//   1. Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//   3. Neither the name of the copyright holder nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
//   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include "DaoFind.h"

#include "Numerics.h"
#include "Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <utility>

namespace gdg {

namespace {

constexpr double kPi = 3.141592653589793;  // NPY_PI

// Python's float `%` with a positive divisor (result in [0, d)).
double py_mod(double v, double d) {
    double m = std::fmod(v, d);
    if (m != 0.0) {
        if ((d < 0.0) != (m < 0.0)) m += d;
    } else {
        m = std::copysign(0.0, d);
    }
    return m;
}

// astropy.stats.gaussian_fwhm_to_sigma = 1.0 / (2.0 * math.sqrt(2.0 * math.log(2.0)))
double fwhm_to_sigma() { return 1.0 / (2.0 * std::sqrt(2.0 * std::log(2.0))); }

// A reduction down a non-innermost numpy axis: the first element, then each next one added.
template <typename F>
double seq_sum(int n, F&& at) {
    double s = at(0);
    for (int i = 1; i < n; ++i) s += at(i);
    return s;
}

// --- peak finding (photutils.detection.find_peaks) -------------------------------------------

// scipy.ndimage.maximum_filter1d, mode='constant', cval=0, origin 0, along one line: the
// window of output i is [i - size/2, i - size/2 + size - 1]. Monotonic-deque sliding max (max
// is exact, so any order gives scipy's value).
void max_filter_line(const double* in, size_t stride, long n, long size, double* out, size_t ostride) {
    const long lo = size / 2;
    const long hi = size - lo - 1;
    auto val = [&](long i) { return (i < 0 || i >= n) ? 0.0 : in[static_cast<size_t>(i) * stride]; };
    std::deque<std::pair<long, double>> dq;
    long next = -lo;  // next index to push
    for (long i = 0; i < n; ++i) {
        const long end = i + hi;
        for (; next <= end; ++next) {
            const double v = val(next);
            while (!dq.empty() && dq.back().second <= v) dq.pop_back();
            dq.emplace_back(next, v);
        }
        while (dq.front().first < i - lo) dq.pop_front();
        out[static_cast<size_t>(i) * ostride] = dq.front().second;
    }
}

// scipy.ndimage.maximum_filter(data, size=size, mode='constant', cval=0.0): separable, rows then
// columns.
std::vector<double> box_max(const std::vector<double>& data, long W, long H, long size) {
    std::vector<double> tmp(data.size()), out(data.size());
    parallel_for_if(static_cast<long long>(W) * H * 4, static_cast<int>(H), [&](int y) {
        max_filter_line(data.data() + static_cast<size_t>(y) * W, 1, W, size, tmp.data() + static_cast<size_t>(y) * W, 1);
    });
    parallel_for_if(static_cast<long long>(W) * H * 4, static_cast<int>(W), [&](int x) {
        max_filter_line(tmp.data() + x, static_cast<size_t>(W), H, size, out.data() + x, static_cast<size_t>(W));
    });
    return out;
}

// photutils _fast_circular_peaks(data, radius) with _verify_ring_candidates.
std::vector<char> fast_circular_peaks(const std::vector<double>& data, long W, long H, double radius) {
    // idx = np.arange(-radius, radius + 1): length ceil(stop - start), values start + i*delta with
    // delta = (start + 1) - start, as numpy's arange fill computes them.
    const double start = -radius, stop = radius + 1.0;
    const long fs = static_cast<long>(std::ceil(stop - start));
    std::vector<double> idx(static_cast<size_t>(std::max(fs, 0L)));
    if (fs > 0) idx[0] = start;
    if (fs > 1) idx[1] = start + 1.0;
    const double delta = fs > 1 ? idx[1] - idx[0] : 1.0;
    for (long i = 2; i < fs; ++i) idx[static_cast<size_t>(i)] = start + static_cast<double>(i) * delta;
    const double radius_sq = radius * radius;
    std::vector<char> fp(static_cast<size_t>(fs * fs));
    for (long i = 0; i < fs; ++i)
        for (long j = 0; j < fs; ++j)
            fp[static_cast<size_t>(i * fs + j)] =
                (idx[static_cast<size_t>(j)] * idx[static_cast<size_t>(j)] + idx[static_cast<size_t>(i)] * idx[static_cast<size_t>(i)]) <= radius_sq;
    const long half = fs / 2;

    const std::vector<double> box = box_max(data, W, H, fs);
    const long half_side = (fs % 2 == 0) ? static_cast<long>(std::floor(radius / std::sqrt(2.0) - 0.5))
                                         : static_cast<long>(std::floor(radius / std::sqrt(2.0)));
    const long side = std::max(2 * half_side + 1, 3L);
    const std::vector<double> insc = box_max(data, W, H, side);

    std::vector<char> peak(data.size());
    for (size_t i = 0; i < data.size(); ++i) peak[i] = data[i] == box[i];
    // Ring candidates: max in the inscribed box but not in the circumscribed one, verified against
    // the circle; a footprint box clipped by the image also sees cval 0 (photutils' rule).
    for (long y = 0; y < H; ++y) {
        for (long x = 0; x < W; ++x) {
            const size_t k = static_cast<size_t>(y * W + x);
            if (!(data[k] == insc[k]) || peak[k]) continue;
            const long y0 = y - half, y1 = y0 + fs, x0 = x - half, x1 = x0 + fs;
            const long dy0 = std::max(0L, y0), dy1 = std::min(H, y1);
            const long dx0 = std::max(0L, x0), dx1 = std::min(W, x1);
            bool any = false;
            double local_max = 0.0;
            for (long yy = dy0; yy < dy1; ++yy)
                for (long xx = dx0; xx < dx1; ++xx) {
                    if (!fp[static_cast<size_t>((yy - y0) * fs + (xx - x0))]) continue;
                    const double v = data[static_cast<size_t>(yy * W + xx)];
                    if (!any || v > local_max) local_max = v;
                    any = true;
                }
            if (!any) throw std::logic_error("dao_find: empty circular footprint");  // numpy: max of empty
            const bool clipped = (dy0 - y0) > 0 || (fs - (y1 - dy1)) < fs || (dx0 - x0) > 0 || (fs - (x1 - dx1)) < fs;
            if (clipped) local_max = std::max(local_max, 0.0);
            if (data[k] == local_max) peak[k] = 1;
        }
    }
    return peak;
}

// scipy.ndimage.maximum_filter(data, footprint=mask, mode='constant', cval=0.0) == data, for an
// odd footprint centred on the pixel.
std::vector<char> footprint_peaks(const std::vector<double>& data, long W, long H, const DaoKernel& k) {
    std::vector<char> peak(data.size());
    parallel_for_if(static_cast<long long>(W) * H * k.nx * k.ny, static_cast<int>(H), [&](int yi) {
        const long y = yi;
        for (long x = 0; x < W; ++x) {
            double m = -std::numeric_limits<double>::infinity();
            for (long i = 0; i < k.ny; ++i)
                for (long j = 0; j < k.nx; ++j) {
                    if (!k.mask[static_cast<size_t>(i * k.nx + j)]) continue;
                    const long yy = y + i - k.ny / 2, xx = x + j - k.nx / 2;
                    const double v = (yy < 0 || yy >= H || xx < 0 || xx >= W) ? 0.0 : data[static_cast<size_t>(yy * W + xx)];
                    m = std::max(m, v);
                }
            const size_t q = static_cast<size_t>(y * W + x);
            peak[q] = data[q] == m;
        }
    });
    return peak;
}

// StarFinderBase._find_stars -> find_peaks: (x, y) of the peaks in row-major order.
std::vector<std::pair<long, long>> find_stars(const Image& conv, const DaoKernel& k, double threshold,
                                              double min_separation, bool exclude_border) {
    const long W = conv.width, H = conv.height;
    std::vector<std::pair<long, long>> out;
    if (conv.data.empty()) return out;
    // "Input data is constant. No local peaks can be found."
    {
        const double first = conv.data[0];
        bool constant = true;
        for (double v : conv.data)
            if (!(v == first)) { constant = false; break; }
        if (constant) return out;
    }
    std::vector<double> data = conv.data;
    std::vector<char> nan_mask(data.size(), 0);
    bool any_nan = false;
    double nanmin = std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < data.size(); ++i) {
        if (std::isnan(data[i])) { nan_mask[i] = 1; any_nan = true; }
        else if (std::isnan(nanmin) || data[i] < nanmin) nanmin = data[i];
    }
    if (any_nan)
        for (size_t i = 0; i < data.size(); ++i)
            if (nan_mask[i]) data[i] = nanmin;

    std::vector<char> good = min_separation > 0.0 ? fast_circular_peaks(data, W, H, min_separation)
                                                  : footprint_peaks(data, W, H, k);
    for (size_t i = 0; i < data.size(); ++i)
        if (nan_mask[i]) good[i] = 0;
    if (exclude_border) {
        const long by = k.y_radius, bx = k.x_radius;
        // as_pair(..., lower_bound=(0, 1), upper_bound=data.shape): 0 <= width < axis length.
        if (by >= H || bx >= W) throw std::invalid_argument("dao_find: border_width must be smaller than the image");
        for (long y = 0; y < H; ++y)
            for (long x = 0; x < W; ++x) {
                const bool border = (by > 0 && (y < by || y >= H - by)) || (bx > 0 && (x < bx || x >= W - bx));
                if (border) good[static_cast<size_t>(y * W + x)] = 0;
            }
    }
    for (long y = 0; y < H; ++y)
        for (long x = 0; x < W; ++x) {
            const size_t i = static_cast<size_t>(y * W + x);
            if (good[i] && data[i] > threshold) out.emplace_back(x, y);
        }
    return out;
}

// --- marginal fits (_DAOStarFinderCatalog.daofind_marginal_fit) ------------------------------

struct Marginal {
    int axis = 0, size = 0, center = 0;
    double sigma = 0.0;
    std::vector<long> wt;    // 1-D weights
    std::vector<long> wts;   // 2-D weights (ky*kx)
    std::vector<long> dxx;   // pixel offsets used by data_dx_sum
    long wt_sum = 0;
    std::vector<double> kern_sum_1d, dkern_dx;
    double kern_sum = 0, kern2_sum = 0, dkern_dx_sum = 0, dkern_dx2_sum = 0, kern_dkern_dx_sum = 0;
};

Marginal marginal_kernel(const DaoKernel& k, int axis) {
    const int ky = k.ny, kx = k.nx;
    const int ycen = (ky - 1) / 2, xcen = (kx - 1) / 2;
    std::vector<long> xx(static_cast<size_t>(kx)), yy(static_cast<size_t>(ky));
    for (int j = 0; j < kx; ++j) xx[static_cast<size_t>(j)] = xcen - std::abs(j - xcen) + 1;
    for (int i = 0; i < ky; ++i) yy[static_cast<size_t>(i)] = ycen - std::abs(i - ycen) + 1;
    Marginal m;
    m.axis = axis;
    m.wts.resize(static_cast<size_t>(ky * kx));
    if (axis == 0) {
        m.wt = yy;
        for (int i = 0; i < ky; ++i)
            for (int j = 0; j < kx; ++j) m.wts[static_cast<size_t>(i * kx + j)] = xx[static_cast<size_t>(j)];  // xwt
        m.size = ky;
        m.center = ycen;
        m.sigma = k.y_sigma;
        for (int t = 0; t < m.size; ++t) m.dxx.push_back(t - m.center);
    } else {
        m.wt = xx;
        for (int i = 0; i < ky; ++i)
            for (int j = 0; j < kx; ++j) m.wts[static_cast<size_t>(i * kx + j)] = yy[static_cast<size_t>(i)];  // ywt
        m.size = kx;
        m.center = xcen;
        m.sigma = k.x_sigma;
        for (int t = 0; t < m.size; ++t) m.dxx.push_back(m.center - t);
    }
    const size_t n = static_cast<size_t>(m.size);
    // kern_sum_1d = np.sum(gaussian_kernel_unmasked * wts, axis=1 - axis)
    m.kern_sum_1d.resize(n);
    std::vector<double> row(static_cast<size_t>(kx));
    if (axis == 0) {  // over axis 1 (innermost): pairwise per row
        for (int i = 0; i < ky; ++i) {
            for (int j = 0; j < kx; ++j)
                row[static_cast<size_t>(j)] = k.unmasked[static_cast<size_t>(i * kx + j)] * static_cast<double>(m.wts[static_cast<size_t>(i * kx + j)]);
            m.kern_sum_1d[static_cast<size_t>(i)] = pairwise_sum(row);
        }
    } else {  // over axis 0: sequential down each column
        for (int j = 0; j < kx; ++j)
            m.kern_sum_1d[static_cast<size_t>(j)] = seq_sum(ky, [&](int i) {
                return k.unmasked[static_cast<size_t>(i * kx + j)] * static_cast<double>(m.wts[static_cast<size_t>(i * kx + j)]);
            });
    }
    for (long w : m.wt) m.wt_sum += w;  // integer sum
    std::vector<double> t(n);
    auto wsum = [&](auto&& f) {
        for (size_t q = 0; q < n; ++q) t[q] = f(q);
        return pairwise_sum(t);
    };
    const auto& ks = m.kern_sum_1d;
    auto wt = [&](size_t q) { return static_cast<double>(m.wt[q]); };
    m.kern_sum = wsum([&](size_t q) { return ks[q] * wt(q); });
    m.kern2_sum = wsum([&](size_t q) { return (ks[q] * ks[q]) * wt(q); });
    m.dkern_dx.resize(n);
    for (size_t q = 0; q < n; ++q) m.dkern_dx[q] = ks[q] * static_cast<double>(m.center - static_cast<long>(q));
    const auto& dk = m.dkern_dx;
    m.dkern_dx_sum = wsum([&](size_t q) { return dk[q] * wt(q); });
    m.dkern_dx2_sum = wsum([&](size_t q) { return (dk[q] * dk[q]) * wt(q); });
    m.kern_dkern_dx_sum = wsum([&](size_t q) { return (ks[q] * dk[q]) * wt(q); });
    return m;
}

// _marginal_data_sums + _marginal_lstsq for one source: returns (dx, hx).
std::pair<double, double> marginal_fit(const Marginal& m, const std::vector<double>& cd, int ky, int kx) {
    const size_t n = static_cast<size_t>(m.size);
    std::vector<double> d1d(n), t(n);
    if (m.axis == 0) {  // np.sum(cutout * wts, axis=2): pairwise per row
        std::vector<double> row(static_cast<size_t>(kx));
        for (int i = 0; i < ky; ++i) {
            for (int j = 0; j < kx; ++j)
                row[static_cast<size_t>(j)] = cd[static_cast<size_t>(i * kx + j)] * static_cast<double>(m.wts[static_cast<size_t>(i * kx + j)]);
            d1d[static_cast<size_t>(i)] = pairwise_sum(row);
        }
    } else {  // np.sum(cutout * wts, axis=1): sequential down each column
        for (int j = 0; j < kx; ++j)
            d1d[static_cast<size_t>(j)] = seq_sum(ky, [&](int i) {
                return cd[static_cast<size_t>(i * kx + j)] * static_cast<double>(m.wts[static_cast<size_t>(i * kx + j)]);
            });
    }
    auto wsum = [&](auto&& f) {
        for (size_t q = 0; q < n; ++q) t[q] = f(q);
        return pairwise_sum(t);
    };
    auto wt = [&](size_t q) { return static_cast<double>(m.wt[q]); };
    const double data_sum = wsum([&](size_t q) { return d1d[q] * wt(q); });
    const double data_kern_sum = wsum([&](size_t q) { return (d1d[q] * m.kern_sum_1d[q]) * wt(q); });
    const double data_dkern_dx_sum = wsum([&](size_t q) { return (d1d[q] * m.dkern_dx[q]) * wt(q); });
    const double data_dx_sum = wsum([&](size_t q) { return (d1d[q] * static_cast<double>(m.dxx[q])) * wt(q); });

    const double wt_sum = static_cast<double>(m.wt_sum);
    const double hx_numer = data_kern_sum - (data_sum * m.kern_sum) / wt_sum;
    const double hx_denom = m.kern2_sum - (m.kern_sum * m.kern_sum / wt_sum);
    const bool mask1 = (hx_numer <= 0.0) || (hx_denom <= 0.0);
    double hx = hx_numer / hx_denom;
    double dx = (m.kern_dkern_dx_sum - (data_dkern_dx_sum - m.dkern_dx_sum * data_sum)) /
                (hx * m.dkern_dx2_sum / (m.sigma * m.sigma));
    const double dx2 = data_dx_sum / data_sum;
    const double hsize = static_cast<double>(m.size) / 2.0;
    if (std::fabs(dx) > hsize) dx = (data_sum == 0.0) ? 0.0 : dx2;
    if (std::fabs(dx) > hsize) dx = 0.0;
    if (mask1) {
        hx = std::numeric_limits<double>::quiet_NaN();
        dx = std::numeric_limits<double>::quiet_NaN();
    }
    return {dx, hx};
}

// _make_cutouts: (ky, kx) block centred on (xc, yc), 0 outside the image.
void cutout(const std::vector<double>& data, long W, long H, long xc, long yc, int ky, int kx, std::vector<double>& out) {
    const long hy = ky / 2, hx = kx / 2;
    out.resize(static_cast<size_t>(ky * kx));
    for (int i = 0; i < ky; ++i)
        for (int j = 0; j < kx; ++j) {
            const long y = yc + i - hy, x = xc + j - hx;
            out[static_cast<size_t>(i * kx + j)] =
                (y >= 0 && y < H && x >= 0 && x < W) ? data[static_cast<size_t>(y * W + x)] : 0.0;
        }
}

// pairwise sum of the (r0..r1) x (c0..c1) block of a (ky, kx) cutout, flattened row-major (numpy
// buffers a non-contiguous multi-axis reduction and sums it pairwise).
double block_sum(const std::vector<double>& a, int kx, int r0, int r1, int c0, int c1) {
    std::vector<double> b;
    b.reserve(static_cast<size_t>(std::max(0, (r1 - r0) * (c1 - c0))));
    for (int i = r0; i < r1; ++i)
        for (int j = c0; j < c1; ++j) b.push_back(a[static_cast<size_t>(i * kx + j)]);
    return pairwise_sum(b);
}

}  // namespace

DaoKernel dao_kernel(double fwhm, double ratio, double theta, double sigma_radius) {
    if (!(fwhm > 0.0)) throw std::invalid_argument("dao_kernel: fwhm must be positive");
    if (ratio <= 0.0 || ratio > 1.0) throw std::invalid_argument("dao_kernel: ratio must be > 0 and <= 1.0");
    if (!(sigma_radius > 0.0)) throw std::invalid_argument("dao_kernel: sigma_radius must be positive");
    DaoKernel k;
    const double th = py_mod(theta, 360.0);
    k.x_sigma = fwhm * fwhm_to_sigma();
    k.y_sigma = k.x_sigma * ratio;
    const double theta_radians = th * (kPi / 180.0);  // np.deg2rad
    const double cost = std::cos(theta_radians), sint = std::sin(theta_radians);
    const double x_sigma2 = k.x_sigma * k.x_sigma, y_sigma2 = k.y_sigma * k.y_sigma;
    const double a = (cost * cost / (2.0 * x_sigma2)) + (sint * sint / (2.0 * y_sigma2));
    const double b = 0.5 * cost * sint * ((1.0 / x_sigma2) - (1.0 / y_sigma2));
    const double c = (sint * sint / (2.0 * x_sigma2)) + (cost * cost / (2.0 * y_sigma2));
    const double f = sigma_radius * sigma_radius / 2.0;
    const double denom = (a * c) - b * b;
    // nx = 2 * int(max(2, sqrt(c*f/denom))) + 1 (Python max keeps the int 2 on ties)
    const double sx = std::sqrt(c * f / denom), sy = std::sqrt(a * f / denom);
    k.nx = 2 * static_cast<int>(sx > 2.0 ? sx : 2.0) + 1;
    k.ny = 2 * static_cast<int>(sy > 2.0 ? sy : 2.0) + 1;
    k.x_radius = k.nx / 2;
    k.y_radius = k.ny / 2;

    const size_t n = static_cast<size_t>(k.nx * k.ny);
    k.mask.resize(n);
    k.unmasked.resize(n);
    std::vector<double> gk(n);
    const double two_b = 2.0 * b;
    for (int i = 0; i < k.ny; ++i)
        for (int j = 0; j < k.nx; ++j) {
            const long dx = j - k.x_radius, dy = i - k.y_radius;
            const double circular = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            const double elliptical = (a * static_cast<double>(dx * dx) + two_b * static_cast<double>(dx) * static_cast<double>(dy)) +
                                      c * static_cast<double>(dy * dy);
            const size_t q = static_cast<size_t>(i * k.nx + j);
            k.mask[q] = (elliptical <= f) || (circular <= 2.0) ? 1 : 0;
            k.unmasked[q] = std::exp(-elliptical);
            gk[q] = k.unmasked[q] * static_cast<double>(k.mask[q]);
            k.n_pixels += k.mask[q];
        }
    std::vector<double> gk2(n);
    for (size_t q = 0; q < n; ++q) gk2[q] = gk[q] * gk[q];
    const double gsum = pairwise_sum(gk);
    const double np_ = static_cast<double>(k.n_pixels);
    const double var = pairwise_sum(gk2) - (gsum * gsum / np_);  // (variance * n_pixels)
    k.rel_err = 1.0 / std::sqrt(var);
    k.data.resize(n);
    const double mean = gsum / np_;
    for (size_t q = 0; q < n; ++q) k.data[q] = ((gk[q] - mean) / var) * static_cast<double>(k.mask[q]);
    return k;
}

Image dao_convolve(const Image& mono, const DaoKernel& k) {
    if (mono.channels != 1) throw std::invalid_argument("dao_convolve: mono image required");
    const long W = mono.width, H = mono.height;
    Image out(mono.width, mono.height, 1, 0.0);
    // scipy convolve = correlate with the weights reversed on both axes (odd shape: no origin
    // shift). NI_Correlate keeps the weights with |w| > DBL_EPSILON, in C order, and sums
    // tmp = 0.0; tmp += w * x (w * cval outside the image).
    struct Tap {
        long dy, dx;
        double w;
    };
    std::vector<Tap> taps;
    const long cy = k.ny / 2, cx = k.nx / 2;
    for (long i = 0; i < k.ny; ++i)
        for (long j = 0; j < k.nx; ++j) {
            const double w = k.data[static_cast<size_t>((k.ny - 1 - i) * k.nx + (k.nx - 1 - j))];
            if (std::fabs(w) > std::numeric_limits<double>::epsilon()) taps.push_back({i - cy, j - cx, w});
        }
    const double cval = 0.0;
    parallel_for_if(static_cast<long long>(W) * H * static_cast<long long>(taps.size()), static_cast<int>(H), [&](int yi) {
        const long y = yi;
        for (long x = 0; x < W; ++x) {
            double tmp = 0.0;
            for (const Tap& t : taps) {
                const long yy = y + t.dy, xx = x + t.dx;
                if (yy < 0 || yy >= H || xx < 0 || xx >= W)
                    tmp += t.w * cval;
                else
                    tmp += t.w * mono.data[static_cast<size_t>(yy * W + xx)];
            }
            out.data[static_cast<size_t>(y * W + x)] = tmp;
        }
    });
    return out;
}

std::vector<DaoStar> dao_find(const Image& mono, const DaoParams& p) {
    if (mono.channels != 1) throw std::invalid_argument("dao_find: mono image required");
    const DaoKernel k = dao_kernel(p.fwhm, p.ratio, py_mod(p.theta, 360.0), p.sigma_radius);
    const double threshold_eff = p.scale_threshold ? p.threshold * k.rel_err : p.threshold;
    const double min_sep = p.min_separation < 0.0 ? 2.5 * p.fwhm : p.min_separation;

    const Image conv = dao_convolve(mono, k);
    const auto xy = find_stars(conv, k, threshold_eff, min_sep, p.exclude_border);
    std::vector<DaoStar> out;
    if (xy.empty()) return out;

    const long W = mono.width, H = mono.height;
    const int ky = k.ny, kx = k.nx;
    const int cy = (ky - 1) / 2, cx = (kx - 1) / 2;
    const Marginal mx = marginal_kernel(k, 1), my = marginal_kernel(k, 0);
    const bool skip_flux = threshold_eff == 0.0;

    struct Row {
        DaoStar s;
        double hx, hy;
        bool keep;
    };
    std::vector<Row> rows(xy.size());
    parallel_for_if(static_cast<long long>(xy.size()) * ky * kx * 16, static_cast<int>(xy.size()), [&](int si) {
        const long xp = xy[static_cast<size_t>(si)].first, yp = xy[static_cast<size_t>(si)].second;
        std::vector<double> cd, cc;
        cutout(mono.data, W, H, xp, yp, ky, kx, cd);
        cutout(conv.data, W, H, xp, yp, ky, kx, cc);
        Row& r = rows[static_cast<size_t>(si)];
        const size_t c0 = static_cast<size_t>(cy * kx + cx);
        const double peak = cd[c0];
        const double convdata_peak = cc[c0];

        // roundness1: four quadrants of the convolved cutout with the peak zeroed.
        cc[c0] = 0.0;
        const double quad1 = block_sum(cc, kx, 0, cy + 1, cx + 1, kx);
        const double quad2 = block_sum(cc, kx, 0, cy, 0, cx + 1);
        const double quad3 = block_sum(cc, kx, cy, ky, 0, cx);
        const double quad4 = block_sum(cc, kx, cy + 1, ky, cx, kx);
        const double sum2 = ((-quad1 + quad2) - quad3) + quad4;
        std::vector<double> absc(cc.size());
        for (size_t q = 0; q < cc.size(); ++q) absc[q] = std::fabs(cc[q]);
        const double sum4 = pairwise_sum(absc);
        const double roundness1 = 2.0 * sum2 / sum4;

        // sharpness
        std::vector<double> masked(cd.size());
        for (size_t q = 0; q < cd.size(); ++q) masked[q] = cd[q] * static_cast<double>(k.mask[q]);
        const double data_mean = (pairwise_sum(masked) - peak) / static_cast<double>(k.n_pixels - 1);
        const double sharpness = (peak - data_mean) / convdata_peak;

        const auto [dx, hx] = marginal_fit(mx, cd, ky, kx);
        const auto [dy, hy] = marginal_fit(my, cd, ky, kx);
        const double roundness2 = 2.0 * (hx - hy) / (hx + hy);
        const double flux = pairwise_sum(cd);

        r.s = {static_cast<double>(xp) + dx, static_cast<double>(yp) + dy, flux, peak, sharpness, roundness1, roundness2};
        r.hx = hx;
        r.hy = hy;
        const bool finite = std::isfinite(r.s.x) && std::isfinite(r.s.y) && std::isfinite(hx) && std::isfinite(hy) &&
                            std::isfinite(sharpness) && std::isfinite(roundness1) && std::isfinite(roundness2) &&
                            std::isfinite(peak) && (skip_flux || std::isfinite(flux));
        r.keep = finite;
    });

    for (const Row& r : rows) {
        if (!r.keep) continue;
        const DaoStar& s = r.s;
        if (!(s.sharpness >= p.sharp_lo && s.sharpness <= p.sharp_hi)) continue;
        if (!(s.roundness1 >= p.round_lo && s.roundness1 <= p.round_hi)) continue;
        if (!(s.roundness2 >= p.round_lo && s.roundness2 <= p.round_hi)) continue;
        out.push_back(s);
    }
    return out;
}

}  // namespace gdg
