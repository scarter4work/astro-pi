#include "core/num/Convolve.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <stdexcept>

namespace ac {

// Source index for an out-of-range position i on a line of length n (periodic bounce, so any distance works).
long extend_index(long i, long n, EdgeMode mode)
{
   if (n == 1)
      return 0;
   if (mode == EdgeMode::Mirror) {
      const long period = 2 * (n - 1);
      long m = i % period;
      if (m < 0)
         m += period;
      return m < n ? m : period - m;
   }
   const long period = 2 * n;
   long m = i % period;
   if (m < 0)
      m += period;
   return m < n ? m : period - 1 - m;
}

Plane correlate1d(const Plane& in, const std::vector<double>& weights, int axis, EdgeMode mode, unsigned threads)
{
   const long K = static_cast<long>(weights.size());
   if (K == 0 || K % 2 == 0)
      throw std::invalid_argument("correlate1d: kernel length must be odd");
   if (axis != 0 && axis != 1)
      throw std::invalid_argument("correlate1d: axis must be 0 or 1");
   Plane out(in.width, in.height);
   if (in.data.empty())
      return out;

   const long r = K / 2;                 // size1 == size2 for odd K
   const double* fw = weights.data() + r;   // centred: fw[jj], jj in [-r, r]

   // NI_Correlate1D: symmetric (+1) / anti-symmetric (-1) detection with a DBL_EPSILON tolerance.
   int symmetric = 1;
   for (long ii = 1; ii <= r; ++ii)
      if (std::fabs(fw[ii] - fw[-ii]) > DBL_EPSILON) {
         symmetric = 0;
         break;
      }
   if (symmetric == 0) {
      symmetric = -1;
      for (long ii = 1; ii <= r; ++ii)
         if (std::fabs(fw[ii] + fw[-ii]) > DBL_EPSILON) {
            symmetric = 0;
            break;
         }
   }

   const long L = axis == 1 ? in.width : in.height;        // line length
   const size_t lines = static_cast<size_t>(axis == 1 ? in.height : in.width);

   parallel_for(lines, [&](size_t line) {
      std::vector<double> ext(static_cast<size_t>(L + 2 * r));
      auto src = [&](long k) -> double {
         const long m = (k < 0 || k >= L) ? extend_index(k, L, mode) : k;
         return axis == 1 ? in.at(static_cast<int>(line), static_cast<int>(m))
                          : in.at(static_cast<int>(m), static_cast<int>(line));
      };
      for (long k = -r; k < L + r; ++k)
         ext[static_cast<size_t>(k + r)] = src(k);
      const double* il = ext.data() + r;           // il[0] is the centre of the first output sample
      for (long p = 0; p < L; ++p, ++il) {
         double acc;
         if (symmetric > 0) {
            acc = il[0] * fw[0];
            for (long jj = -r; jj < 0; ++jj)
               acc += (il[jj] + il[-jj]) * fw[jj];
         } else if (symmetric < 0) {
            acc = 0.0;
            for (long jj = -r; jj < 0; ++jj)
               acc += (il[jj] - il[-jj]) * fw[jj];
         } else {
            acc = il[r] * fw[r];
            for (long jj = -r; jj < r; ++jj)
               acc += il[jj] * fw[jj];
         }
         if (axis == 1)
            out.at(static_cast<int>(line), static_cast<int>(p)) = acc;
         else
            out.at(static_cast<int>(p), static_cast<int>(line)) = acc;
      }
   }, threads);
   return out;
}

Plane convolve1d_mirror(const Plane& in, const std::vector<double>& kernel, int axis, unsigned threads)
{
   std::vector<double> flipped(kernel.rbegin(), kernel.rend());   // convolve1d: weights = weights[::-1]
   return correlate1d(in, flipped, axis, EdgeMode::Mirror, threads);
}

} // namespace ac
