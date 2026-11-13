#include "core/fingerprint/Extract.h"

#include "core/Parallel.h"
#include "core/fingerprint/Color.h"
#include "core/fingerprint/Metrics.h"
#include "core/fingerprint/Starlet.h"

#include <cmath>
#include <stdexcept>

namespace ac {

namespace {

const Json& need(const Json& j, const char* key)
{
   if (!j.is_object() || !j.contains(key))
      throw std::invalid_argument(std::string("fingerprint JSON: missing key '") + key + "'");
   return j.at(key);
}

std::vector<double> doubles(const Json& j, const char* what)
{
   if (!j.is_array())
      throw std::invalid_argument(std::string("fingerprint JSON: '") + what + "' must be a list");
   std::vector<double> v;
   v.reserve(j.size());
   for (const Json& x : j) {
      if (!x.is_number())
         throw std::invalid_argument(std::string("fingerprint JSON: '") + what + "' holds a non-number");
      v.push_back(x.get<double>());
   }
   return v;
}

std::array<double, 10> quantile_array(const Json& j, const char* what)
{
   const std::vector<double> v = doubles(j, what);
   if (v.size() != 10)
      throw std::invalid_argument(std::string("fingerprint JSON: '") + what + "' must have 10 entries");
   std::array<double, 10> out{};
   for (size_t i = 0; i < 10; ++i)
      out[i] = v[i];
   return out;
}

} // namespace

Json FingerprintData::to_json() const
{
   const int bins = static_cast<int>(std::lround(std::sqrt(static_cast<double>(chroma_hist.size()))));
   if (static_cast<size_t>(bins) * bins != chroma_hist.size())
      throw std::logic_error("FingerprintData::to_json: chroma_hist is not square");
   Json rows = Json::array();
   for (int r = 0; r < bins; ++r)
      rows.push_back(Json(std::vector<double>(chroma_hist.begin() + static_cast<long>(r) * bins,
                                              chroma_hist.begin() + static_cast<long>(r + 1) * bins)));
   Json j = Json::object();
   j["energy_spectrum"] = Json::object({{"centers_arcsec", energy.centers_arcsec}, {"energy", energy.energy}});
   j["tonal_quantiles"] = std::vector<double>(tonal_quantiles.begin(), tonal_quantiles.end());
   j["saturation_quantiles"] = std::vector<double>(saturation_quantiles.begin(), saturation_quantiles.end());
   j["chroma_hist"] = std::move(rows);
   j["background"] = Json::object({{"floor_to_peak_ratio", background.floor_to_peak_ratio},
                                   {"structure_to_gradient_ratio", background.structure_to_gradient_ratio},
                                   {"channel_balance", background.channel_balance}});
   j["pixel_scale_arcsec"] = pixel_scale_arcsec;
   j["psf_fwhm_arcsec"] = psf_fwhm_arcsec;
   j["palette_class"] = palette_class;
   return j;
}

FingerprintData FingerprintData::from_json(const Json& j)
{
   FingerprintData fp;
   const Json& es = need(j, "energy_spectrum");
   fp.energy.centers_arcsec = doubles(need(es, "centers_arcsec"), "centers_arcsec");
   fp.energy.energy = doubles(need(es, "energy"), "energy");
   if (fp.energy.centers_arcsec.size() != fp.energy.energy.size())
      throw std::invalid_argument("fingerprint JSON: centers_arcsec and energy differ in length");
   fp.tonal_quantiles = quantile_array(need(j, "tonal_quantiles"), "tonal_quantiles");
   fp.saturation_quantiles = quantile_array(need(j, "saturation_quantiles"), "saturation_quantiles");
   const Json& hist = need(j, "chroma_hist");
   if (!hist.is_array())
      throw std::invalid_argument("fingerprint JSON: 'chroma_hist' must be a list of rows");
   for (const Json& row : hist) {
      const std::vector<double> r = doubles(row, "chroma_hist row");
      if (r.size() != hist.size())
         throw std::invalid_argument("fingerprint JSON: 'chroma_hist' must be square");
      fp.chroma_hist.insert(fp.chroma_hist.end(), r.begin(), r.end());
   }
   const Json& bg = need(j, "background");
   fp.background.floor_to_peak_ratio = need(bg, "floor_to_peak_ratio").get<double>();
   fp.background.structure_to_gradient_ratio = need(bg, "structure_to_gradient_ratio").get<double>();
   fp.background.channel_balance = doubles(need(bg, "channel_balance"), "channel_balance");
   fp.pixel_scale_arcsec = need(j, "pixel_scale_arcsec").get<double>();
   fp.psf_fwhm_arcsec = need(j, "psf_fwhm_arcsec").get<double>();
   fp.palette_class = need(j, "palette_class").get<std::string>();
   return fp;
}

FingerprintData extract(const Image& rgb, double pixel_scale_arcsec, int n_scales, double psf_fwhm_arcsec,
                        const std::string& palette_class, unsigned threads)
{
   if (rgb.channels != 3)
      throw std::invalid_argument("extract expects an (H, W, 3) sRGB image, got " + std::to_string(rgb.height) + "x" +
                                  std::to_string(rgb.width) + "x" + std::to_string(rgb.channels));
   if (rgb.width <= 0 || rgb.height <= 0)
      throw std::invalid_argument("extract: empty image");

   Plane L, a, b;
   rgb_to_lab(rgb, L, a, b, threads);
   Plane chroma(L.width, L.height);
   parallel_for(chroma.data.size(), [&](size_t i) { chroma.data[i] = std::hypot(a.data[i], b.data[i]); }, threads);

   // One starlet decomposition, used twice (energy spectrum + structure/gradient ratio), as in Python.
   const Starlet st = starlet_transform(L, n_scales, threads);

   FingerprintData fp;
   fp.energy = energy_spectrum_from_planes(st.planes, pixel_scale_arcsec, threads);
   fp.tonal_quantiles = tonal_quantiles(L);
   fp.saturation_quantiles = saturation_quantiles(chroma);
   fp.chroma_hist = chroma_histogram(a, b, kChromaBins, kChromaExtent);
   fp.background.floor_to_peak_ratio = floor_to_peak_ratio(L);
   fp.background.structure_to_gradient_ratio = structure_to_gradient_ratio_from_transform(st);
   fp.background.channel_balance = background_channel_balance(rgb);
   fp.pixel_scale_arcsec = pixel_scale_arcsec;
   fp.psf_fwhm_arcsec = psf_fwhm_arcsec;
   fp.palette_class = palette_class;
   return fp;
}

} // namespace ac
