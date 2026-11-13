#include "core/num/Gaussian.h"

#include "core/num/Convolve.h"

#include <cmath>
#include <cstddef>

namespace ac {

namespace {

// numpy/_core/src/umath/loops_utils.h.src::pairwise_sum (what np.add.reduce uses on contiguous float64).
double pairwise_sum(const double* a, size_t n)
{
   if (n < 8) {
      double res = 0.0;
      for (size_t i = 0; i < n; ++i)
         res += a[i];
      return res;
   }
   if (n <= 128) {
      double r[8];
      for (int j = 0; j < 8; ++j)
         r[j] = a[j];
      size_t i;
      for (i = 8; i < n - (n % 8); i += 8)
         for (int j = 0; j < 8; ++j)
            r[j] += a[i + j];
      double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
      for (; i < n; ++i)
         res += a[i];
      return res;
   }
   size_t n2 = n / 2;
   n2 -= n2 % 8;
   return pairwise_sum(a, n2) + pairwise_sum(a + n2, n - n2);
}

} // namespace

// _filters.py 656-666 (order 0): sigma2 = sigma*sigma; x = arange(-radius, radius+1);
// phi_x = exp(-0.5 / sigma2 * x**2); phi_x = phi_x / phi_x.sum().
std::vector<double> gaussian_kernel1d(double sigma, int radius)
{
   const double sigma2 = sigma * sigma;
   const size_t n = static_cast<size_t>(2 * radius + 1);
   std::vector<double> phi(n);
   for (size_t i = 0; i < n; ++i) {
      const long x = static_cast<long>(i) - radius;
      phi[i] = std::exp(-0.5 / sigma2 * static_cast<double>(x * x));
   }
   // np.sum: the first element seeds the accumulator, the rest is pairwise-summed.
   const double sum = phi[0] + pairwise_sum(phi.data() + 1, n - 1);
   for (double& v : phi)
      v = v / sum;
   return phi;
}

// _filters.py gaussian_filter 852-861 and gaussian_filter1d 745-754: lw = int(truncate*sd + 0.5);
// weights = _gaussian_kernel1d(...)[::-1]; correlate1d(..., mode='reflect'); axes in order 0, 1.
Plane gaussian_filter(const Plane& in, double sigma, unsigned threads)
{
   if (!(sigma > 1e-15))
      return in;
   const int lw = static_cast<int>(4.0 * sigma + 0.5);
   std::vector<double> w = gaussian_kernel1d(sigma, lw);
   std::vector<double> rev(w.rbegin(), w.rend());
   const Plane tmp = correlate1d(in, rev, 0, EdgeMode::Reflect, threads);
   return correlate1d(tmp, rev, 1, EdgeMode::Reflect, threads);
}

} // namespace ac
