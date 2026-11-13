// AutoContrast core (PCL-free): np.interp for one point.
#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace ac {

// np.interp(x, xp, fp) (numpy/_core/src/multiarray/compiled_base.c::arr_interp): xp increasing, no NaN;
// x < xp[0] -> fp[0], x > xp[n-1] -> fp[n-1], NaN -> NaN. Interior: j with xp[j] <= x < xp[j+1];
//   x == xp[j] -> fp[j];  else slope = (fp[j+1]-fp[j])/(xp[j+1]-xp[j]);  r = slope*(x-xp[j]) + fp[j];
//   if r is NaN: r = slope*(x-xp[j+1]) + fp[j+1]; if still NaN and fp[j]==fp[j+1]: r = fp[j].
inline double interp(double x, const std::vector<double>& xp, const std::vector<double>& fp)
{
   const size_t n = xp.size();
   if (n == 0 || fp.size() != n)
      throw std::invalid_argument("interp: xp and fp must be non-empty and equally long");
   if (std::isnan(x))
      return x;
   if (x < xp.front())
      return fp.front();
   if (x > xp.back())
      return fp.back();
   const size_t j = static_cast<size_t>(std::upper_bound(xp.begin(), xp.end(), x) - xp.begin()) - 1;
   if (j == n - 1)
      return fp[j];
   if (xp[j] == x)
      return fp[j];
   const double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
   double r = slope * (x - xp[j]) + fp[j];
   if (std::isnan(r)) {
      r = slope * (x - xp[j + 1]) + fp[j + 1];
      if (std::isnan(r) && fp[j] == fp[j + 1])
         r = fp[j];
   }
   return r;
}

} // namespace ac
