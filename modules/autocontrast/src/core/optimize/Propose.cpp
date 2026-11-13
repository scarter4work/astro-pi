#include "core/optimize/Propose.h"

#include "core/fingerprint/BandLimit.h"
#include "core/fingerprint/Distance.h"
#include "core/fingerprint/Palette.h"

#include <algorithm>
#include <stdexcept>

namespace ac {

std::string level_for_gap(double gap)
{
   if (gap >= kGapModerateMax)
      return "strong";
   if (gap >= kGapGentleMax)
      return "moderate";
   return "gentle";
}

std::map<std::string, double> component_gaps(const FingerprintData& ref, const FingerprintData& target)
{
   const auto [spectrum, usable] = spectrum_distance(ref, target);
   return {{"spectrum", usable ? spectrum : 0.0},
           {"tonal", tonal_distance(ref, target)},
           {"chroma", chroma_distance(ref, target)},
           {"background", background_distance(ref, target)}};
}

std::vector<double> band_deficits(const FingerprintData& ref, const FingerprintData& target)
{
   std::vector<double> deficits(target.energy.energy.size(), 0.0);
   const auto bl = band_limit(ref.energy, target.energy, ref.psf_fwhm_arcsec, target.psf_fwhm_arcsec);
   if (!bl.usable)
      return deficits;
   size_t k = 0;
   for (size_t i = 0; i < target.energy.centers_arcsec.size(); ++i) {
      const double c = target.energy.centers_arcsec[i];
      if (c >= bl.lo_arcsec && c <= bl.hi_arcsec) {
         if (k >= bl.ref_shape.size() || k >= bl.target_shape.size())
            throw std::logic_error("band_deficits: band-limited shapes shorter than the comparable window");
         deficits[i] = bl.ref_shape[k] - bl.target_shape[k];
         ++k;
      }
   }
   if (k != bl.ref_shape.size())
      throw std::logic_error("band_deficits: comparable window and band-limited shapes disagree in length");
   return deficits;
}

namespace {

struct GroupKey {
   double neg_gap, scale;
   std::string kind;
};

// Python tuple order: float compare (-0.0 == 0.0), then str compare (byte order).
struct GroupLess {
   bool operator()(const GroupKey& a, const GroupKey& b) const
   {
      if (a.neg_gap < b.neg_gap) return true;
      if (b.neg_gap < a.neg_gap) return false;
      if (a.scale < b.scale) return true;
      if (b.scale < a.scale) return false;
      return a.kind < b.kind;
   }
};

double remedy_gap(const std::string& kind, const std::map<std::string, double>& gaps)
{
   double gap = 0.0;
   auto take = [&](const char* component) { gap = std::max(gap, gaps.at(component)); };
   if (kind == "core_hdr") take("spectrum");
   if (kind == "tonal_reshape" || kind == "black_point") take("tonal");
   if (kind == "chroma") take("chroma");
   if (kind == "background_neutralize" || kind == "black_point") take("background");
   return gap;
}

} // namespace

std::vector<Action> propose_actions(const FingerprintData& ref, const FingerprintData& target,
                                    const std::set<std::string>& applied_kinds, int n_scales,
                                    std::optional<int> top_k)
{
   const bool compatible = palette_chroma_compatible(ref.palette_class, target.palette_class);
   auto gaps = component_gaps(ref, target);
   if (!compatible)
      gaps["chroma"] = 0.0;
   const auto deficits = band_deficits(ref, target);

   const auto menu = available_actions(target.pixel_scale_arcsec,
                                       std::max(ref.psf_fwhm_arcsec, target.psf_fwhm_arcsec), n_scales,
                                       compatible, applied_kinds);

   auto gap_for = [&](const Action& a) {
      if (a.kind == "local_contrast" || a.kind == "local_equalize") {
         const int layer = layer_for_scale(*a.scale_arcsec, target.pixel_scale_arcsec);
         return layer >= 0 && layer < static_cast<int>(deficits.size()) ? deficits[layer] : 0.0;
      }
      return remedy_gap(a.kind, gaps);
   };

   std::map<GroupKey, std::vector<Action>, GroupLess> groups;
   for (const auto& a : menu)
      groups[GroupKey{-gap_for(a), a.scale_arcsec ? *a.scale_arcsec : -1.0, a.kind}].push_back(a);

   std::vector<Action> proposed;
   for (const auto& [gk, members] : groups) {
      if (top_k && static_cast<int>(proposed.size()) >= *top_k)
         break;
      const auto levelless = std::find_if(members.begin(), members.end(), [](const Action& a) { return a.level.empty(); });
      if (levelless != members.end()) {
         proposed.push_back(*levelless);
         continue;
      }
      const std::string level = level_for_gap(-gk.neg_gap);
      const auto chosen = std::find_if(members.begin(), members.end(), [&](const Action& a) { return a.level == level; });
      if (chosen == members.end())
         throw std::logic_error("no '" + level + "' action found for kind '" + members.front().kind +
                                "'; available_actions should have emitted all three LEVELS");
      proposed.push_back(*chosen);
   }
   return proposed;
}

} // namespace ac
