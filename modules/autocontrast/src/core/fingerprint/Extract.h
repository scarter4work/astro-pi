// AutoContrast core (PCL-free): top-level fingerprint extraction and the serialisable record —
// fingerprint/extract.py.
#pragma once

#include "core/Image.h"
#include "core/Json.h"
#include "core/fingerprint/Energy.h"

#include <array>
#include <string>
#include <vector>

namespace ac {

constexpr int kChromaBins = 16;
constexpr double kChromaExtent = 100.0;

struct Background {
   double floor_to_peak_ratio = 0.0;
   double structure_to_gradient_ratio = 0.0;
   std::vector<double> channel_balance;
};

// The measured statistics block of a fingerprint record (§4.1).
struct FingerprintData {
   EnergySpectrum energy;
   std::array<double, 10> tonal_quantiles{};
   std::array<double, 10> saturation_quantiles{};
   std::vector<double> chroma_hist;   // kChromaBins x kChromaBins, row-major [a-bin * bins + b-bin]
   Background background;
   double pixel_scale_arcsec = 0.0;
   double psf_fwhm_arcsec = 0.0;
   std::string palette_class;

   // FingerprintData.to_dict(): keys and order exactly as Python (energy_spectrum, tonal_quantiles,
   // saturation_quantiles, chroma_hist as a list of rows, background, pixel_scale_arcsec, psf_fwhm_arcsec,
   // palette_class). Non-finite doubles stay doubles; dump_python writes them as Infinity / NaN.
   Json to_json() const;
   // from_dict: throws std::invalid_argument on a missing key, wrong length or a chroma_hist that is not a
   // square list of rows.
   static FingerprintData from_json(const Json& j);
};

// extract(rgb, ...): fingerprint an sRGB image in [0, 1] (3 channels, non-empty). One starlet decomposition
// of L* feeds both the energy spectrum and the structure/gradient ratio. Per-pixel work is threaded
// (`threads`, 0 = default); every reduction is a pairwise_sum over a materialised array, so the result is
// bit-identical at any thread count.
FingerprintData extract(const Image& rgb, double pixel_scale_arcsec, int n_scales, double psf_fwhm_arcsec,
                        const std::string& palette_class, unsigned threads = 0);

} // namespace ac
