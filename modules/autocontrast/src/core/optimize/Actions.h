// AutoContrast core (PCL-free): the discretized action space (design §6.2) — optimize/actions.py.
//
// The menu is a bounded set; each entry carries 2-3 magnitude levels. Actions are band-limited by
// construction: one whose angular scale sits below the image's resolvable limit is never emitted.
#pragma once

#include "core/Json.h"

#include <array>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ac {

extern const std::array<const char*, 3> kLevels;   // gentle, moderate, strong
extern const std::set<std::string> kOnceOnly;      // star_split, background_neutralize

struct Action {
   std::string kind, level;
   std::optional<double> scale_arcsec;
   Json params = Json::object();

   // "<kind>@<scale %.3f or ->/<level or ->"
   std::string key() const;
   // 1.0 for the empty level; throws std::invalid_argument (Python's message) for an unknown level.
   double strength() const;
   // Identity is kind, level and scale only; params are excluded (dataclass compare=False).
   bool operator==(const Action& o) const
   {
      return kind == o.kind && level == o.level && scale_arcsec == o.scale_arcsec;
   }
};

double scale_for_layer(int layer, double pixel_scale_arcsec);

// int(round(log2(scale / pixel_scale))) with Python's round-half-even. Throws std::invalid_argument when
// scale < pixel_scale (Python's ValueError text).
int layer_for_scale(double scale_arcsec, double pixel_scale_arcsec);

// The full menu legal right now, in Python's order. palette_compatible == false withholds every chroma action;
// applied_kinds withholds spent once-only actions.
std::vector<Action> available_actions(double pixel_scale_arcsec, double psf_fwhm_arcsec, int n_scales,
                                      bool palette_compatible, const std::set<std::string>& applied_kinds);

} // namespace ac
