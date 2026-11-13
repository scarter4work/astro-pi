#include "core/fingerprint/Metrics.h"

#include "core/Parallel.h"
#include "core/num/Histogram.h"
#include "core/num/Percentile.h"
#include "core/num/Sum.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace ac {

const std::array<double, 10> kQuantileLevels = {1, 5, 10, 25, 50, 75, 90, 95, 99, 99.9};

namespace {

std::array<double, 10> quantiles_of(const Plane& p)
{
   const std::vector<double> q =
      percentiles(p.data, std::vector<double>(kQuantileLevels.begin(), kQuantileLevels.end()));
   std::array<double, 10> out{};
   for (size_t i = 0; i < out.size(); ++i)
      out[i] = q[i];
   return out;
}

} // namespace

std::array<double, 10> tonal_quantiles(const Plane& L)
{
   return quantiles_of(L);
}

std::array<double, 10> saturation_quantiles(const Plane& C)
{
   return quantiles_of(C);
}

std::vector<double> chroma_histogram(const Plane& a, const Plane& b, int bins, double extent)
{
   Plane weights(a.width, a.height);
   parallel_for(a.data.size(), [&](size_t i) { weights.data[i] = std::hypot(a.data[i], b.data[i]); });
   std::vector<double> hist = histogram2d(a, b, weights, bins, extent);
   const double total = pairwise_sum(hist.data(), hist.size());
   if (total > 0)
      for (double& h : hist)
         h /= total;
   return hist;
}

double floor_to_peak_ratio(const Plane& L)
{
   const double floor_ = percentile(L.data, 1.0);
   const double peak = percentile(L.data, 99.9);
   if (peak == 0)
      return 0.0;
   return floor_ / peak;
}

double structure_to_gradient_ratio_from_transform(const Starlet& st)
{
   // structure = float(np.sum(planes**2)): planes is one C-contiguous (n, H, W) array, so np.sum is a single
   // pairwise reduction over all n*H*W squares.
   const size_t hw = st.residual.data.size();
   std::vector<const double*> ptr;
   for (const Plane& p : st.planes) {
      if (p.data.size() != hw)
         throw std::invalid_argument("structure_to_gradient_ratio: planes and residual must share one shape");
      ptr.push_back(p.data.data());
   }
   const double structure = hw == 0 ? 0.0 : pairwise_sum(0, ptr.size() * hw, [&](size_t i) {
      const double v = ptr[i / hw][i % hw];
      return v * v;
   });
   // gradient = float(np.sum((residual - residual.mean()) ** 2)); mean = add.reduce / n
   const double* r = st.residual.data.data();
   const double mean = pairwise_sum(r, hw) / static_cast<double>(hw);
   const double gradient = pairwise_sum(0, hw, [&](size_t i) {
      const double d = r[i] - mean;
      return d * d;
   });
   if (gradient == 0)
      return structure > 0 ? std::numeric_limits<double>::infinity() : 0.0;
   return structure / gradient;
}

std::vector<double> background_channel_balance(const Image& rgb, double pct)
{
   const size_t n = static_cast<size_t>(rgb.width) * rgb.height;
   const size_t C = static_cast<size_t>(rgb.channels);
   // luminance = rgb.mean(axis=-1): add.reduce over the last axis in element order, then / C
   std::vector<double> lum(n);
   parallel_for(n, [&](size_t i) {
      lum[i] = pairwise_sum(&rgb.data[i * C], C) / static_cast<double>(C);
   });
   const double threshold = percentile(lum, pct);
   std::vector<double> means(C);
   std::vector<double> picked;
   for (size_t c = 0; c < C; ++c) {
      picked.clear();
      for (size_t i = 0; i < n; ++i)
         if (lum[i] <= threshold)
            picked.push_back(rgb.data[i * C + c]);
      // empty selection: numpy's mean of nothing is 0.0 / 0 = NaN
      means[c] = picked.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : pairwise_sum(picked.data(), picked.size()) / static_cast<double>(picked.size());
   }
   const double overall = pairwise_sum(means.data(), C) / static_cast<double>(C);
   std::vector<double> out(C);
   for (size_t c = 0; c < C; ++c)
      out[c] = overall == 0 ? 1.0 : means[c] / overall;
   return out;
}

} // namespace ac
