#include "Numerics.h"

#include "Parallel.h"

#include <cmath>
#include <stdexcept>

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

}  // namespace gdg
