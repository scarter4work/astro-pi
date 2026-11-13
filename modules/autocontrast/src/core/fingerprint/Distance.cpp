// AutoContrast core (PCL-free): the fingerprint distance function — fingerprint/distance.py, line for line.
#include "core/fingerprint/Distance.h"

#include "core/fingerprint/BandLimit.h"
#include "core/fingerprint/Emd.h"
#include "core/fingerprint/Palette.h"
#include "core/num/Histogram.h"
#include "core/num/Sum.h"

#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ac {

namespace {

constexpr double kLMax = 100.0;   // CIELAB L* upper bound, for normalising the tonal quantile distance

// np.linalg.norm of a short 1-D float64 vector: sqrt(x.dot(x)), the bundled OpenBLAS ddot accumulating serially
// with fused multiply-adds below 16 elements (see l2_normalize in BandLimit.cpp, verified bitwise there).
// Callers here pass at most n_scales - 1 (spectrum) or 3 (channel balance) elements.
double blas_norm(const std::vector<double>& x)
{
   double sq = 0.0;
   for (double v : x)
      sq = std::fma(v, v, sq);
   return std::sqrt(sq);
}

} // namespace

double combine_distance(const DistanceComponents& c, const DistanceWeights& w, bool palette_match)
{
   // Option (b): gate chroma out on a palette mismatch (§2.3), then redistribute its weight across the
   // surviving components so matched and mismatched references stay on one comparable [0, 1] scale.
   const double w_c = palette_match ? w.chroma : 0.0;
   const double used_weight = w.spectrum + w.tonal + w_c + w.background;
   const double total = w.spectrum * c.spectrum + w.tonal * c.tonal + w_c * c.chroma + w.background * c.background;
   if (used_weight == 0.0) {
      std::ostringstream msg;
      msg << "combine_distance: the surviving weights sum to zero (spectrum=" << w.spectrum << ", tonal=" << w.tonal
          << ", chroma=" << w.chroma << (palette_match ? "" : " [gated out: palette mismatch]")
          << ", background=" << w.background << "); there is nothing to measure the distance with";
      throw std::invalid_argument(msg.str());
   }
   return total / used_weight;
}

std::pair<double, bool> spectrum_distance(const FingerprintData& ref, const FingerprintData& target)
{
   const auto bl = band_limit(ref.energy, target.energy, ref.psf_fwhm_arcsec, target.psf_fwhm_arcsec);
   if (!bl.usable)
      return {0.0, false};
   std::vector<double> diff(bl.ref_shape.size());
   for (size_t k = 0; k < diff.size(); ++k)
      diff[k] = bl.ref_shape[k] - bl.target_shape[k];
   return {blas_norm(diff) / std::sqrt(2.0), true};
}

double tonal_distance(const FingerprintData& ref, const FingerprintData& target)
{
   // np.sqrt(np.mean(diff ** 2)), diff = (ref - target) / L_MAX
   std::array<double, 10> sq{};
   for (size_t k = 0; k < sq.size(); ++k) {
      const double d = (ref.tonal_quantiles[k] - target.tonal_quantiles[k]) / kLMax;
      sq[k] = d * d;
   }
   return std::sqrt(pairwise_sum(sq.data(), sq.size()) / double(sq.size()));
}

double chroma_distance(const FingerprintData& ref, const FingerprintData& target, double extent)
{
   const auto& h_ref = ref.chroma_hist;
   const auto& h_tgt = target.chroma_hist;
   const int bins = int(std::lround(std::sqrt(double(h_ref.size()))));
   if (h_ref.empty() || size_t(bins) * size_t(bins) != h_ref.size() || h_tgt.size() != h_ref.size())
      throw std::invalid_argument("chroma_distance: both chroma histograms must be the same non-empty square size (" +
                                  std::to_string(h_ref.size()) + " vs " + std::to_string(h_tgt.size()) +
                                  " entries)");
   if (pairwise_sum(h_ref.data(), h_ref.size()) == 0.0 || pairwise_sum(h_tgt.data(), h_tgt.size()) == 0.0)
      return 0.0;   // a neutral image has no chroma to compare

   // bin centres of np.linspace(-extent, extent, bins + 1); coords = meshgrid(indexing="ij") raveled, so point
   // k sits at (centre[k / bins], centre[k % bins]); ground cost = scipy cdist's Euclidean sqrt(sum d*d)
   const auto edges = linspace(-extent, extent, bins + 1);
   std::vector<double> centre(bins);
   for (int k = 0; k < bins; ++k)
      centre[k] = 0.5 * (edges[k] + edges[k + 1]);
   const size_t n = h_ref.size();
   std::vector<double> cost(n * n);
   for (size_t p = 0; p < n; ++p)
      for (size_t q = 0; q < n; ++q) {
         const double d0 = centre[p / bins] - centre[q / bins], d1 = centre[p % bins] - centre[q % bins];
         double s = 0.0;
         s += d0 * d0;
         s += d1 * d1;
         cost[p * n + q] = std::sqrt(s);
      }
   const double emd = emd_exact(h_ref, h_tgt, cost).cost;
   return emd / std::hypot(2.0 * extent, 2.0 * extent);
}

double squash(double x)
{
   if (!std::isfinite(x))
      return 1.0;
   return x / (1.0 + x);
}

double background_distance(const FingerprintData& ref, const FingerprintData& target)
{
   const auto& rb = ref.background;
   const auto& tb = target.background;
   if (rb.channel_balance.size() != tb.channel_balance.size() || rb.channel_balance.empty())
      throw std::invalid_argument("background_distance: channel_balance lengths differ (" +
                                  std::to_string(rb.channel_balance.size()) + " vs " +
                                  std::to_string(tb.channel_balance.size()) + ") or are empty");
   const double d_floor = std::abs(rb.floor_to_peak_ratio - tb.floor_to_peak_ratio);
   const double d_grad = std::abs(squash(rb.structure_to_gradient_ratio) - squash(tb.structure_to_gradient_ratio));
   std::vector<double> diff(rb.channel_balance.size());
   for (size_t k = 0; k < diff.size(); ++k)
      diff[k] = rb.channel_balance[k] - tb.channel_balance[k];
   const double d_balance = blas_norm(diff) / std::sqrt(double(diff.size()));
   const double parts[3] = {d_floor, d_grad, d_balance};
   return pairwise_sum(parts, 3) / 3.0;
}

double fingerprint_distance(const FingerprintData& ref, const FingerprintData& target, const DistanceWeights& weights)
{
   const bool palette_match = palette_chroma_compatible(ref.palette_class, target.palette_class);
   const auto [d_spectrum, spectrum_usable] = spectrum_distance(ref, target);
   DistanceWeights effective = weights;
   if (!spectrum_usable)
      effective.spectrum = 0.0;   // no comparable band: drop the spectrum, combine_distance rescales the rest
   const DistanceComponents components{d_spectrum, tonal_distance(ref, target), chroma_distance(ref, target),
                                       background_distance(ref, target)};
   return combine_distance(components, effective, palette_match);
}

} // namespace ac
