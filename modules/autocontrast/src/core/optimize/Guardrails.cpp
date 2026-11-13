#include "core/optimize/Guardrails.h"

#include "core/Json.h"
#include "core/Parallel.h"
#include "core/fingerprint/Color.h"
#include "core/fingerprint/Starlet.h"
#include "core/num/Histogram.h"
#include "core/num/Label.h"
#include "core/num/Percentile.h"
#include "core/num/Sum.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace ac {

namespace {

// Python's format(x, '.<prec>f') -- correctly rounded fixed notation (glibc printf and CPython agree).
std::string fmt_f(double x, int prec)
{
   char buf[512];
   const int n = std::snprintf(buf, sizeof buf, "%.*f", prec, x);
   if (n < 0 || static_cast<size_t>(n) >= sizeof buf)
      throw std::runtime_error("guardrails: formatting failed");
   return std::string(buf, static_cast<size_t>(n));
}

// f'{x:.<prec>%}': x * 100 rounded to a double, then fixed notation, then '%'.
std::string fmt_pct(double x, int prec) { return fmt_f(x * 100.0, prec) + "%"; }

// f'{x!r}' for a float.
std::string py_repr(double x) { return dump_python(Json(x)); }

GuardrailVerdict make(const char* name, bool ok, std::string reason, double value, double limit)
{
   GuardrailVerdict v;
   v.name = name;
   v.ok = ok;
   v.reason = std::move(reason);
   v.value = value;
   v.limit = limit;
   return v;
}

// Python float % : result takes the sign of the divisor (numpy npy_divmod / CPython float_rem).
double py_mod(double x, double y)
{
   double mod = std::fmod(x, y);
   if (mod != 0.0) {
      if ((y < 0.0) != (mod < 0.0))
         mod += y;
   } else {
      mod = std::copysign(0.0, y);
   }
   return mod;
}

// np.degrees: x * (180 / pi).
constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

} // namespace

Plane gray_of(const Image& rgb, unsigned threads)
{
   if (rgb.channels < 1)
      throw std::invalid_argument("gray_of: image has no channels");
   Plane g(rgb.width, rgb.height);
   const size_t n = g.data.size();
   const size_t ch = static_cast<size_t>(rgb.channels);
   const double* p = rgb.data.data();
   // rgb.mean(axis=-1): add.reduce over the channel axis, then / count.
   parallel_for(n, [&](size_t i) {
      g.data[i] = pairwise_sum(i * ch, ch, [p](size_t k) { return p[k]; }) / static_cast<double>(ch);
   }, threads);
   return g;
}

double mrs_noise_sigma(const Plane& gray, unsigned threads)
{
   const Starlet s = starlet_transform(gray, 1, threads);
   const std::vector<double>& finest = s.planes[0].data;
   const double med = median(finest);
   std::vector<double> dev(finest.size());
   for (size_t i = 0; i < dev.size(); ++i)
      dev[i] = std::fabs(finest[i] - med);
   return median(std::move(dev)) * kMadToSigma;
}

std::vector<double> clipped_fraction(const Image& rgb, double at)
{
   std::vector<double> out;
   const size_t n = static_cast<size_t>(rgb.width) * rgb.height;
   for (int c = 0; c < rgb.channels; ++c) {
      size_t count = 0;
      for (size_t i = 0; i < n; ++i)
         count += rgb.data[i * rgb.channels + c] == at;
      // np.mean of a bool array: exact integer count / n.
      out.push_back(static_cast<double>(count) / static_cast<double>(n));
   }
   return out;
}

StarStats detect_stars(const Plane& gray, double k_sigma, unsigned threads)
{
   const double sigma = mrs_noise_sigma(gray, threads);
   const double background = median(gray.data);
   // guardrails.py:144 -- max(sigma, 1e-9); 1e-9 is the floor that keeps a flat plane from thresholding at 0.
   const double floor_sigma = (1e-9 > sigma) ? 1e-9 : sigma;
   const double thr = background + k_sigma * floor_sigma;

   std::vector<uint8_t> mask(gray.data.size());
   parallel_for(mask.size(), [&](size_t i) { mask[i] = gray.data[i] > thr; }, threads);

   const Labels L = label4(mask, gray.width, gray.height);
   if (L.count == 0)
      return {};

   std::vector<double> fwhms, eccs;
   for (const Box& b : L.boxes) {
      const int h = b.y1 - b.y0, w = b.x1 - b.x0;
      if (h < 2 || w < 2)
         continue;   // single-pixel hits are cosmic rays / hot pixels, not stars (guardrails.py:155)
      fwhms.push_back(std::sqrt(static_cast<double>(h * w)));
      const int major = std::max(h, w), minor = std::min(h, w);
      const double r = static_cast<double>(minor) / static_cast<double>(major);
      eccs.push_back(std::sqrt(1.0 - r * r));
   }
   if (fwhms.empty())
      return {};
   StarStats s;
   s.count = static_cast<int>(fwhms.size());
   s.median_fwhm = median(fwhms);
   s.median_ecc = median(eccs);
   return s;
}

std::vector<double> hue_mass_by_bin(const Image& rgb, unsigned threads)
{
   Plane L, a, b;
   rgb_to_lab(rgb, L, a, b, threads);
   const size_t n = a.data.size();
   std::vector<double> chroma(n), hue(n);
   parallel_for(n, [&](size_t i) {
      chroma[i] = std::hypot(a.data[i], b.data[i]);
      hue[i] = py_mod(std::atan2(b.data[i], a.data[i]) * kRadToDeg, 360.0);   // np.degrees(arctan2(b, a)) % 360.0
   }, threads);

   std::vector<double> sh, sw;   // hue[supported], chroma[supported] in raster order
   for (size_t i = 0; i < n; ++i)
      if (chroma[i] > kHueChromaFloor) {
         sh.push_back(hue[i]);
         sw.push_back(chroma[i]);
      }

   // np.histogram(x, bins=edges, weights=w) with an explicit edge array takes numpy's non-uniform path
   // (_histograms_impl.py 883-893): per 65536-block, argsort, cumulative weight, searchsorted(left) on the
   // edges (last edge: right), differences of the summed cumulative table.
   const std::vector<double> edges = linspace(0.0, 360.0, kNumHueBins + 1);
   std::vector<double> cum_n(edges.size(), 0.0);
   constexpr size_t kBlock = 65536;
   for (size_t i0 = 0; i0 < sh.size(); i0 += kBlock) {
      const size_t cnt = std::min(kBlock, sh.size() - i0);
      std::vector<size_t> idx(cnt);
      std::iota(idx.begin(), idx.end(), size_t{0});
      // np.argsort(default quicksort) is not stable; a stable order is one valid permutation of the ties
      // (ties only change the last bits of a bin's cumulative sum, never which bins are occupied).
      std::stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return sh[i0 + x] < sh[i0 + y]; });
      std::vector<double> sa(cnt), cw(cnt + 1);
      cw[0] = 0.0;
      double run = 0.0;
      for (size_t k = 0; k < cnt; ++k) {
         sa[k] = sh[i0 + idx[k]];
         run += sw[i0 + idx[k]];       // sw.cumsum(): sequential
         cw[k + 1] = run;
      }
      for (size_t e = 0; e < edges.size(); ++e) {
         const auto pos = (e + 1 == edges.size())
                             ? std::upper_bound(sa.begin(), sa.end(), edges[e])
                             : std::lower_bound(sa.begin(), sa.end(), edges[e]);
         cum_n[e] += cw[static_cast<size_t>(pos - sa.begin())];
      }
   }
   std::vector<double> hist(kNumHueBins);
   for (int k = 0; k < kNumHueBins; ++k)
      hist[k] = cum_n[k + 1] - cum_n[k];
   return hist;
}

std::vector<uint8_t> dilate_circular(const std::vector<uint8_t>& occupied, int radius_bins)
{
   const long n = static_cast<long>(occupied.size());
   std::vector<uint8_t> out = occupied;
   for (int shift = 1; shift <= radius_bins; ++shift)
      for (long i = 0; i < n; ++i) {
         // np.roll(occupied, +s)[i] = occupied[i - s]; np.roll(occupied, -s)[i] = occupied[i + s]
         const long a = ((i - shift) % n + n) % n, b = ((i + shift) % n + n) % n;
         out[i] = out[i] | occupied[a] | occupied[b];
      }
   return out;
}

std::vector<double> channel_ratios(const Image& rgb)
{
   const size_t n = static_cast<size_t>(rgb.width) * rgb.height;
   const size_t ch = static_cast<size_t>(rgb.channels);
   const double* p = rgb.data.data();
   std::vector<double> means;
   for (size_t c = 0; c < ch; ++c)
      means.push_back(pairwise_sum(0, n, [p, ch, c](size_t i) { return p[i * ch + c]; }) / static_cast<double>(n));
   const double total = pairwise_sum(means.data(), means.size());
   if (total > 0.0)
      for (double& m : means)
         m = m / total;
   return means;
}

GuardrailVerdict check_noise_floor(const Image& candidate, const Image& baseline, const GuardrailLimits& lim, unsigned threads)
{
   const double base = mrs_noise_sigma(gray_of(baseline, threads), threads);
   const double cand = mrs_noise_sigma(gray_of(candidate, threads), threads);
   if (base <= 0.0) {
      // A non-positive baseline sigma makes growth undefined: fail closed (guardrails.py:78-90).
      return make("noise_floor", false,
                  "baseline noise sigma was non-positive (" + py_repr(base) + "); noise growth could not be assessed",
                  std::numeric_limits<double>::infinity(), lim.noise_growth);
   }
   const double growth = (cand - base) / base;
   const bool ok = growth <= lim.noise_growth;
   return make("noise_floor", ok,
               ok ? "" : "noise sigma grew " + fmt_pct(growth, 1) + " from checkpoint (limit " + fmt_pct(lim.noise_growth, 1) + ")",
               growth, lim.noise_growth);
}

GuardrailVerdict check_shadow_clipping(const Image& candidate, const GuardrailLimits& lim)
{
   const auto f = clipped_fraction(candidate, 0.0);
   const double worst = *std::max_element(f.begin(), f.end());
   const bool ok = worst <= lim.shadow_clip_fraction;
   return make("shadow_clipping", ok,
               ok ? "" : fmt_pct(worst, 4) + " of pixels crushed to zero (limit " + fmt_pct(lim.shadow_clip_fraction, 4) + ")",
               worst, lim.shadow_clip_fraction);
}

GuardrailVerdict check_highlight_clipping(const Image& candidate, const GuardrailLimits& lim)
{
   const auto f = clipped_fraction(candidate, 1.0);
   const double worst = *std::max_element(f.begin(), f.end());
   const bool ok = worst <= lim.highlight_clip_fraction;
   return make("highlight_clipping", ok,
               ok ? "" : fmt_pct(worst, 4) + " of pixels blown to white (limit " + fmt_pct(lim.highlight_clip_fraction, 4) + ")",
               worst, lim.highlight_clip_fraction);
}

GuardrailVerdict check_star_integrity(const Image& candidate, const Image& baseline, const GuardrailLimits& lim, unsigned threads)
{
   const StarStats base = detect_stars(gray_of(baseline, threads), 5.0, threads);
   const StarStats cand = detect_stars(gray_of(candidate, threads), 5.0, threads);

   if (base.count == 0) {
      // A pass, but not a silent one: a starless baseline is legitimate, yet the verdict says it was not assessed.
      return make("star_integrity", true, "star integrity not assessed: baseline had no detectable stars", 0.0, 0.0);
   }

   const double lost = static_cast<double>(base.count - cand.count) / static_cast<double>(base.count);
   if (lost > lim.star_count_drop) {
      return make("star_integrity", false,
                  "star count fell " + fmt_pct(lost, 1) + " (" + std::to_string(base.count) + " -> " + std::to_string(cand.count) +
                     "), limit " + fmt_pct(lim.star_count_drop, 1),
                  lost, lim.star_count_drop);
   }

   if (base.median_fwhm > 0.0) {
      const double growth = (cand.median_fwhm - base.median_fwhm) / base.median_fwhm;
      if (growth > lim.star_fwhm_growth) {
         return make("star_integrity", false,
                     "star FWHM ballooned " + fmt_pct(growth, 1) + " (" + fmt_f(base.median_fwhm, 2) + " -> " +
                        fmt_f(cand.median_fwhm, 2) + " px), limit " + fmt_pct(lim.star_fwhm_growth, 1),
                     growth, lim.star_fwhm_growth);
      }
   }

   const double ecc_growth = cand.median_ecc - base.median_ecc;
   if (ecc_growth > lim.star_ecc_growth) {
      return make("star_integrity", false,
                  "star eccentricity grew " + fmt_f(ecc_growth, 2) + ", limit " + fmt_f(lim.star_ecc_growth, 2),
                  ecc_growth, lim.star_ecc_growth);
   }

   return make("star_integrity", true, "", 0.0, lim.star_count_drop);
}

GuardrailVerdict check_hue_invention(const Image& candidate, const Image& source, const GuardrailLimits& lim, unsigned threads)
{
   const std::vector<double> src_hist = hue_mass_by_bin(source, threads);
   const std::vector<double> cand_hist = hue_mass_by_bin(candidate, threads);

   const double bin_width_deg = 360.0 / kNumHueBins;
   const int radius_bins = static_cast<int>(std::ceil(lim.hue_tolerance_deg / bin_width_deg));
   std::vector<uint8_t> occ(kNumHueBins);
   for (int k = 0; k < kNumHueBins; ++k)
      occ[k] = src_hist[k] > 0.0;
   const std::vector<uint8_t> supported = dilate_circular(occ, radius_bins);

   const double cand_total = pairwise_sum(cand_hist.data(), cand_hist.size());
   std::vector<double> unsupported;   // cand_norm[~supported]
   for (int k = 0; k < kNumHueBins; ++k)
      if (!supported[k])
         unsupported.push_back(cand_total > 0.0 ? cand_hist[k] / cand_total : cand_hist[k]);

   const double mass = pairwise_sum(unsupported.data(), unsupported.size());
   const bool ok = mass <= lim.hue_invention_mass;
   return make("hue_invention", ok,
               ok ? "" : "hue invention: " + fmt_f(mass, 4) + " chroma mass appeared more than " + fmt_f(lim.hue_tolerance_deg, 0) +
                            " degrees from any hue the source supports (limit " + fmt_f(lim.hue_invention_mass, 4) + ")",
               mass, lim.hue_invention_mass);
}

GuardrailVerdict check_channel_ratio_drift(const Image& candidate, const Image& source, const GuardrailLimits& lim)
{
   const auto rc = channel_ratios(candidate), rs = channel_ratios(source);
   // np.linalg.norm(a - b): sqrt of the sum of squares (BLAS ddot in numpy; plain sequential sum here).
   double ss = 0.0;
   for (size_t i = 0; i < rc.size(); ++i) {
      const double d = rc[i] - rs[i];
      ss += d * d;
   }
   const double drift = std::sqrt(ss);
   const bool ok = drift <= lim.channel_ratio_drift;
   return make("channel_ratio_drift", ok,
               ok ? "" : "channel ratios drifted " + fmt_f(drift, 3) + " from the source (limit " + fmt_f(lim.channel_ratio_drift, 3) + ")",
               drift, lim.channel_ratio_drift);
}

std::vector<GuardrailVerdict> evaluate_guardrails(const Image& candidate, const Image& baseline, const Image& source,
                                                  const GuardrailLimits& lim, unsigned threads)
{
   // All six, no short-circuit: a declined run reports everything that was wrong.
   std::vector<GuardrailVerdict> v;
   v.push_back(check_noise_floor(candidate, baseline, lim, threads));
   v.push_back(check_star_integrity(candidate, baseline, lim, threads));
   v.push_back(check_shadow_clipping(candidate, lim));
   v.push_back(check_highlight_clipping(candidate, lim));
   v.push_back(check_hue_invention(candidate, source, lim, threads));
   v.push_back(check_channel_ratio_drift(candidate, source, lim));
   return v;
}

} // namespace ac
