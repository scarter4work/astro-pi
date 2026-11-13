#include "core/optimize/Actions.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace ac {

const std::array<const char*, 3> kLevels = {"gentle", "moderate", "strong"};
const std::set<std::string> kOnceOnly = {"star_split", "background_neutralize"};

std::string Action::key() const
{
   std::string scale = "-";
   if (scale_arcsec) {
      char buf[512];
      std::snprintf(buf, sizeof buf, "%.3f", *scale_arcsec);
      scale = buf;
   }
   return kind + "@" + scale + "/" + (level.empty() ? std::string("-") : level);
}

double Action::strength() const
{
   if (level.empty())
      return 1.0;
   if (level == "gentle")
      return 0.25;
   if (level == "moderate")
      return 0.5;
   if (level == "strong")
      return 0.85;
   throw std::invalid_argument("level '" + level +
                               "' is not recognized; valid levels are: gentle, moderate, strong");
}

double scale_for_layer(int layer, double pixel_scale_arcsec)
{
   return std::pow(2.0, layer) * pixel_scale_arcsec;
}

int layer_for_scale(double scale_arcsec, double pixel_scale_arcsec)
{
   if (scale_arcsec < pixel_scale_arcsec)
      throw std::invalid_argument("scale " + dump_python(Json(scale_arcsec)) + "\" is below the pixel scale " +
                                  dump_python(Json(pixel_scale_arcsec)) +
                                  "\"/px; there is no wavelet plane there");
   const double l = std::log2(scale_arcsec / pixel_scale_arcsec);
   // Python's round(): half to even, independent of the ambient rounding mode.
   const double fl = std::floor(l);
   const double diff = l - fl;
   double r;
   if (diff < 0.5)
      r = fl;
   else if (diff > 0.5)
      r = fl + 1.0;
   else
      r = std::fmod(fl, 2.0) == 0.0 ? fl : fl + 1.0;
   return static_cast<int>(r);
}

std::vector<Action> available_actions(double px, double psf, int n_scales, bool palette_compatible,
                                      const std::set<std::string>& applied_kinds)
{
   std::vector<Action> out;
   std::vector<double> bands;
   for (int i = 0; i < n_scales; ++i)
      if (scale_for_layer(i, px) >= psf)
         bands.push_back(scale_for_layer(i, px));

   for (double band : bands)
      for (const char* level : kLevels) {
         Action lc{"local_contrast", level, band, Json{{"layer", layer_for_scale(band, px)}}};
         out.push_back(std::move(lc));
         out.push_back(Action{"local_equalize", level, band, Json{{"radius_arcsec", band}}});
      }

   for (const char* level : kLevels) {
      const std::string lv = level;
      out.push_back(Action{"tonal_reshape", lv, std::nullopt, Json{{"monotone", true}}});
      out.push_back(Action{"black_point", lv, std::nullopt, Json{{"clip_limited", true}}});
      const int layers = lv == "gentle" ? 2 : lv == "moderate" ? 3 : 4;
      out.push_back(Action{"core_hdr", lv, std::nullopt, Json{{"layers", layers}}});
      if (palette_compatible)
         out.push_back(Action{"chroma", lv, std::nullopt, Json::object()});
   }

   for (const char* kind : {"background_neutralize", "star_split"})
      if (!applied_kinds.count(kind))
         out.push_back(Action{kind, "", std::nullopt, Json::object()});
   return out;
}

} // namespace ac
