#include "core/num/Histogram.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ac {

std::vector<double> linspace(double start, double stop, int num)
{
   std::vector<double> y(static_cast<size_t>(std::max(num, 0)));
   if (num <= 0)
      return y;
   const int div = num - 1;
   const double delta = stop - start;
   if (div > 0) {
      const double step = delta / div;
      if (step == 0) {                       // denormal handling, gh-5437 (function_base.py 157-161)
         for (int i = 0; i < num; ++i)
            y[static_cast<size_t>(i)] = (static_cast<double>(i) / div) * delta;
      } else {
         for (int i = 0; i < num; ++i)
            y[static_cast<size_t>(i)] = static_cast<double>(i) * step;
      }
   } else {
      for (int i = 0; i < num; ++i)
         y[static_cast<size_t>(i)] = static_cast<double>(i) * delta;
   }
   for (double& v : y)
      v += start;
   if (num > 1)
      y.back() = stop;
   return y;
}

std::vector<double> histogram_uniform(const double* x, const double* w, size_t n, int bins, double lo, double hi)
{
   if (bins < 1)
      throw std::invalid_argument("histogram_uniform: bins must be >= 1");
   if (!(lo <= hi) || !std::isfinite(lo) || !std::isfinite(hi))
      throw std::invalid_argument("histogram_uniform: bad range");
   if (lo == hi) {                            // _get_outer_edges (320-323)
      lo -= 0.5;
      hi += 0.5;
   }
   const std::vector<double> edges = linspace(lo, hi, bins + 1);
   const double norm_denom = hi - lo;         // _unsigned_subtract on floats is a plain subtraction
   const double nb = static_cast<double>(bins);
   constexpr size_t BLOCK = 65536;

   std::vector<double> total(static_cast<size_t>(bins), 0.0), blk(static_cast<size_t>(bins));
   for (size_t i0 = 0; i0 < n; i0 += BLOCK) {
      const size_t i1 = std::min(n, i0 + BLOCK);
      std::fill(blk.begin(), blk.end(), 0.0);
      for (size_t i = i0; i < i1; ++i) {
         const double a = x[i];
         if (!(a >= lo && a <= hi))           // keep mask (836-837); NaN fails both
            continue;
         long idx = static_cast<long>(((a - lo) / norm_denom) * nb);       // f_indices.astype(intp) (851-853)
         if (idx == bins)
            --idx;                                                          // 854
         if (a < edges[static_cast<size_t>(idx)])                           // decrement (858-859)
            --idx;
         if (a >= edges[static_cast<size_t>(idx + 1)] && idx != bins - 1)  // increment (861-863)
            ++idx;
         blk[static_cast<size_t>(idx)] += w ? w[i] : 1.0;                   // np.bincount (872)
      }
      for (int k = 0; k < bins; ++k)          // n += bincount(...)
         total[static_cast<size_t>(k)] += blk[static_cast<size_t>(k)];
   }
   return total;
}

std::vector<double> histogram2d(const Plane& a, const Plane& b, const Plane& w, int bins, double extent)
{
   if (bins < 1)
      throw std::invalid_argument("histogram2d: bins must be >= 1");
   if (a.data.size() != b.data.size() || (!w.data.empty() && w.data.size() != a.data.size()))
      throw std::invalid_argument("histogram2d: plane sizes differ");
   const std::vector<double> edges = linspace(-extent, extent, bins + 1);
   const size_t nb = static_cast<size_t>(bins);
   // histogramdd: searchsorted(edges, v, 'right') in [0, bins+1]; on the last edge -> bin bins-1 (1051-1053);
   // 1..bins are real bins, 0 and bins+1 are outliers.
   auto locate = [&](double v) -> long {
      long k = static_cast<long>(std::upper_bound(edges.begin(), edges.end(), v) - edges.begin());   // NaN -> n
      if (v == edges.back())
         --k;
      return k;
   };
   std::vector<double> hist(nb * nb, 0.0);
   for (size_t i = 0; i < a.data.size(); ++i) {
      const long ka = locate(a.data[i]), kb = locate(b.data[i]);
      if (ka < 1 || ka > bins || kb < 1 || kb > bins)
         continue;
      hist[static_cast<size_t>(ka - 1) * nb + static_cast<size_t>(kb - 1)] += w.data.empty() ? 1.0 : w.data[i];
   }
   return hist;
}

} // namespace ac
