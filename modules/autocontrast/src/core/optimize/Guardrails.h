// AutoContrast core (PCL-free): the six §7 guardrails — optimize/guardrails.py. Hard filters, all evaluated
// every iteration (no short-circuit); a verdict that differs from Python's changes which candidates survive.
#pragma once

#include "core/Image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ac {

// tools/autocontrast/src/autocontrast/optimize/guardrails.py:32-40 (GuardrailLimits defaults)
struct GuardrailLimits {
   double noise_growth = 0.15;
   double shadow_clip_fraction = 1e-4;
   double highlight_clip_fraction = 1e-4;
   double star_count_drop = 0.05;
   double star_fwhm_growth = 0.20;
   double star_ecc_growth = 0.20;
   double hue_invention_mass = 1e-3;
   double hue_tolerance_deg = 20.0;
   double channel_ratio_drift = 0.10;
};

struct GuardrailVerdict {
   std::string name;
   bool ok = true;
   std::string reason;   // Python's f-string, byte for byte; empty when ok (except the starless note)
   double value = 0.0, limit = 0.0;
};

// guardrails.py:24 _MAD_TO_SIGMA
constexpr double kMadToSigma = 1.4826;
// guardrails.py:231 NUM_HUE_BINS (a measurement grid, 1 degree) and :240 HUE_CHROMA_FLOOR
constexpr int kNumHueBins = 360;
constexpr double kHueChromaFloor = 1.0;

// rgb.mean(axis=-1) (guardrails.py:_gray), numpy summation order. Needs channels >= 1.
Plane gray_of(const Image& rgb, unsigned threads = 0);

// MAD noise sigma of the finest starlet plane: median(|f - median(f)|) * 1.4826 (guardrails.py:mrs_noise_sigma).
double mrs_noise_sigma(const Plane& gray, unsigned threads = 0);

// Fraction of pixels exactly equal to `at`, per channel (np.mean(rgb[..., c] == at)).
std::vector<double> clipped_fraction(const Image& rgb, double at);

struct StarStats {
   int count = 0;
   double median_fwhm = 0.0, median_ecc = 0.0;
};

// Threshold at median + k_sigma * max(sigma, 1e-9), 4-connected labels, bounding-box FWHM/eccentricity
// (single-pixel-wide boxes skipped). guardrails.py:detect_stars.
StarStats detect_stars(const Plane& gray, double k_sigma = 5.0, unsigned threads = 0);

// Chroma-weighted 1-degree hue histogram (np.histogram with an explicit edge array: sorted cumulative-sum
// path), pixels with chroma <= kHueChromaFloor excluded. Always kNumHueBins entries.
std::vector<double> hue_mass_by_bin(const Image& rgb, unsigned threads = 0);

// OR of `occupied` with itself rolled by +-1..radius_bins, wrapping (guardrails.py:_dilate_circular).
std::vector<uint8_t> dilate_circular(const std::vector<uint8_t>& occupied, int radius_bins);

// Mean level per channel, normalised to sum 1 (guardrails.py:_channel_ratios).
std::vector<double> channel_ratios(const Image& rgb);

GuardrailVerdict check_noise_floor(const Image& candidate, const Image& baseline, const GuardrailLimits& lim, unsigned threads = 0);
GuardrailVerdict check_star_integrity(const Image& candidate, const Image& baseline, const GuardrailLimits& lim, unsigned threads = 0);
GuardrailVerdict check_shadow_clipping(const Image& candidate, const GuardrailLimits& lim);
GuardrailVerdict check_highlight_clipping(const Image& candidate, const GuardrailLimits& lim);
GuardrailVerdict check_hue_invention(const Image& candidate, const Image& source, const GuardrailLimits& lim, unsigned threads = 0);
GuardrailVerdict check_channel_ratio_drift(const Image& candidate, const Image& source, const GuardrailLimits& lim);

// noise_floor, star_integrity, shadow_clipping, highlight_clipping, hue_invention, channel_ratio_drift —
// all six, in that order. Bit-identical at any thread count.
std::vector<GuardrailVerdict> evaluate_guardrails(const Image& candidate, const Image& baseline, const Image& source,
                                                  const GuardrailLimits& lim = {}, unsigned threads = 0);

} // namespace ac
