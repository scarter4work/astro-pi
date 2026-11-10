#include "Numerics.h"

#include "Parallel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace gdg {

namespace {

// numpy's DOUBLE_pairwise_sum (numpy/_core/src/umath/loops_utils.h.src), contiguous stride.
double pairwise_sum_raw(const double* a, size_t n) {
    constexpr size_t kBlock = 128;  // PW_BLOCKSIZE
    if (n < 8) {
        double res = -0.0;  // numpy starts at -0.0 to preserve -0.0 inputs
        for (size_t i = 0; i < n; ++i) res += a[i];
        return res;
    }
    if (n <= kBlock) {
        double r[8];
        for (int k = 0; k < 8; ++k) r[k] = a[k];
        size_t i = 8;
        for (; i < n - (n % 8); i += 8)
            for (int k = 0; k < 8; ++k) r[k] += a[i + k];
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i) res += a[i];
        return res;
    }
    size_t n2 = n / 2;
    n2 -= n2 % 8;
    return pairwise_sum_raw(a, n2) + pairwise_sum_raw(a + n2, n - n2);
}

// scipy NI_Correlate1D for an odd, symmetric kernel `w` of radius r (w[r] is the centre) over one
// line of n samples read with `stride`, written to `out` with the same stride. `ext` is a
// caller-owned buffer of n + 2r samples. Loop order is scipy's symmetric branch:
//   out = x[0]*w[0]; for j = -r .. -1: out += (x[j] + x[-j]) * w[j].
void correlate_line_symmetric(const double* in, size_t stride, long n, const std::vector<double>& w, long r,
                              double* ext, double* out) {
    for (long i = -r; i < n + r; ++i) ext[i + r] = in[static_cast<size_t>(reflect_index(i, n)) * stride];
    const double* fw = w.data() + r;
    for (long i = 0; i < n; ++i) {
        const double* x = ext + i + r;
        double o = x[0] * fw[0];
        for (long j = -r; j < 0; ++j) o += (x[j] + x[-j]) * fw[j];
        out[static_cast<size_t>(i) * stride] = o;
    }
}

}  // namespace

double pairwise_sum(const double* a, size_t n) {
    // np.add.reduce initialises the result to 0.0 and adds the pairwise sum of the whole array
    // (verified against np.sum for 300 random lengths up to 40000).
    return 0.0 + pairwise_sum_raw(a, n);
}

int gaussian_radius(double sigma) { return static_cast<int>(4.0 * sigma + 0.5); }

std::vector<double> gaussian_kernel1d(double sigma, int radius) {
    if (radius < 0) throw std::invalid_argument("gaussian_kernel1d: radius must be >= 0");
    // scipy 1.18 _filters._gaussian_kernel1d: phi = exp(-0.5 / sigma2 * x**2) with integer x**2,
    // then phi / phi.sum(). numpy's exp is its own SIMD routine (AVX-512 here), libm's may differ
    // from it in the last ulp; that is the whole residual against the goldens.
    const double c2 = -0.5 / (sigma * sigma);
    std::vector<double> phi(static_cast<size_t>(2 * radius + 1));
    for (int i = -radius; i <= radius; ++i) {
        const double x2 = static_cast<double>(static_cast<long long>(i) * i);
        phi[static_cast<size_t>(i + radius)] = std::exp(c2 * x2);
    }
    const double s = pairwise_sum(phi);
    for (double& v : phi) v /= s;
    return phi;
}

Image gaussian_filter(const Image& in, double sigma) {
    if (!std::isfinite(sigma) || sigma < 0.0)
        throw std::invalid_argument("gaussian_filter: sigma must be finite and >= 0");
    Image out = in;
    if (sigma <= 1e-15 || in.data.empty()) return out;  // scipy drops axes with sigma <= 1e-15

    const long r = gaussian_radius(sigma);
    // scipy reverses the weights before correlating; the kernel is exactly symmetric, so the
    // reversal is the identity and scipy takes its symmetric branch.
    const std::vector<double> w = gaussian_kernel1d(sigma, static_cast<int>(r));
    const long W = in.width, H = in.height, C = in.channels;
    const long long taps = r + 1;

    // Axis 0 (y): one line per (x, c), stride W*C. Reads `in`, writes `out`.
    {
        const int lines = static_cast<int>(W * C);
        parallel_for_if(static_cast<long long>(lines) * H * taps, lines, [&](int line) {
            const long x = line / C, c = line % C;
            std::vector<double> ext(static_cast<size_t>(H + 2 * r));
            correlate_line_symmetric(in.data.data() + x * C + c, static_cast<size_t>(W * C), H, w, r, ext.data(),
                                     out.data.data() + x * C + c);
        });
    }
    // Axis 1 (x): one line per (y, c), stride C, in place on the axis-0 result (each line is
    // copied into its extension buffer before it is overwritten).
    {
        const int lines = static_cast<int>(H * C);
        parallel_for_if(static_cast<long long>(lines) * W * taps, lines, [&](int line) {
            const long y = line / C, c = line % C;
            std::vector<double> ext(static_cast<size_t>(W + 2 * r));
            double* row = out.data.data() + y * W * C + c;
            correlate_line_symmetric(row, static_cast<size_t>(C), W, w, r, ext.data(), row);
        });
    }
    return out;
}

// --- order statistics -------------------------------------------------------------------------

namespace {

void require_finite(const std::vector<double>& v, const char* fn) {
    if (v.empty()) throw std::invalid_argument(std::string(fn) + ": empty input");
    for (double x : v)
        if (!std::isfinite(x)) throw std::invalid_argument(std::string(fn) + ": non-finite input (filter first)");
}

void require_q(double q) {
    if (!(q >= 0.0 && q <= 100.0)) throw std::invalid_argument("percentile: q must be in [0, 100]");
}

// numpy _quantile, method "linear", on a working copy that may be reordered.
double percentile_select(std::vector<double>& v, double q) {
    const size_t n = v.size();
    const double vi = static_cast<double>(n - 1) * (q / 100.0);  // (n-1) * true_divide(q, 100)
    if (vi >= static_cast<double>(n - 1)) return *std::max_element(v.begin(), v.end());  // index -1
    const double prev_d = std::floor(vi);
    const size_t prev = static_cast<size_t>(prev_d);
    std::nth_element(v.begin(), v.begin() + static_cast<long>(prev), v.end());
    const double a = v[prev];
    const double b = *std::min_element(v.begin() + static_cast<long>(prev) + 1, v.end());
    // numpy _lerp: a + (b-a)*g, replaced by b - (b-a)*(1-g) where g >= 0.5.
    const double g = vi - prev_d;
    const double d = b - a;
    return g >= 0.5 ? b - d * (1.0 - g) : a + d * g;
}

// np.median on non-empty input; the even case is np.mean of the two middle values.
double median_select(std::vector<double>& v) {
    const size_t n = v.size(), h = n / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(h), v.end());
    const double hi = v[h];
    if (n % 2 == 1) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<long>(h));
    return (lo + hi) / 2.0;
}

}  // namespace

double percentile(std::vector<double> v, double q) {
    require_finite(v, "percentile");
    require_q(q);
    return percentile_select(v, q);
}

std::vector<double> percentiles(std::vector<double> v, const std::vector<double>& qs) {
    require_finite(v, "percentiles");
    for (double q : qs) require_q(q);
    std::vector<double> out;
    out.reserve(qs.size());
    for (double q : qs) out.push_back(percentile_select(v, q));  // selection keeps the multiset
    return out;
}

double median(std::vector<double> v) {
    require_finite(v, "median");
    return median_select(v);
}

double mad(const std::vector<double>& v) {
    const double m = median(v);
    std::vector<double> dev(v.size());
    for (size_t i = 0; i < v.size(); ++i) dev[i] = std::fabs(v[i] - m);
    return median_select(dev);
}

std::vector<double> interp(const std::vector<double>& xq, const std::vector<double>& xp,
                           const std::vector<double>& fp, double left, double right) {
    if (xp.empty()) throw std::invalid_argument("interp: array of sample points is empty");
    if (xp.size() != fp.size()) throw std::invalid_argument("interp: fp and xp are not of the same length");
    std::vector<double> out(xq.size());
    const size_t n = xp.size();
    if (n == 1) {  // numpy's single-knot branch
        for (size_t i = 0; i < xq.size(); ++i)
            out[i] = xq[i] < xp[0] ? left : (xq[i] > xp[0] ? right : fp[0]);
        return out;
    }
    for (size_t i = 0; i < xq.size(); ++i) {
        const double x = xq[i];
        if (std::isnan(x)) {
            out[i] = x;
            continue;
        }
        if (x < xp[0]) {
            out[i] = left;
            continue;
        }
        if (x > xp[n - 1]) {
            out[i] = right;
            continue;
        }
        // binary_search_with_guess: the rightmost j with xp[j] <= x.
        const size_t j = static_cast<size_t>(std::upper_bound(xp.begin(), xp.end(), x) - xp.begin()) - 1;
        if (j == n - 1 || xp[j] == x) {
            out[i] = fp[j];
            continue;
        }
        const double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
        double r = slope * (x - xp[j]) + fp[j];
        if (std::isnan(r)) {  // numpy retries from the right knot, then takes fp[j] on a flat run
            r = slope * (x - xp[j + 1]) + fp[j + 1];
            if (std::isnan(r) && fp[j] == fp[j + 1]) r = fp[j];
        }
        out[i] = r;
    }
    return out;
}

std::vector<double> interp(const std::vector<double>& xq, const std::vector<double>& xp,
                           const std::vector<double>& fp) {
    if (fp.empty()) throw std::invalid_argument("interp: array of sample points is empty");
    return interp(xq, xp, fp, fp.front(), fp.back());
}

std::vector<double> nanmedian_rows(const std::vector<std::vector<double>>& stack) {
    if (stack.empty()) return {};
    const size_t cols = stack[0].size();
    for (const auto& row : stack)
        if (row.size() != cols) throw std::invalid_argument("nanmedian_rows: rows differ in length");
    std::vector<double> out(cols);
    std::vector<double> col;
    col.reserve(stack.size());
    for (size_t c = 0; c < cols; ++c) {
        col.clear();
        for (const auto& row : stack)
            if (!std::isnan(row[c])) col.push_back(row[c]);
        out[c] = col.empty() ? std::numeric_limits<double>::quiet_NaN() : median_select(col);
    }
    return out;
}

}  // namespace gdg
