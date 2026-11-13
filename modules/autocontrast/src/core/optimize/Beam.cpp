#include "core/optimize/Beam.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace ac {

std::vector<Branch> prune(std::vector<Branch> candidates, int width)
{
   struct Ranked {
      size_t index;
      Branch* b;
   };
   std::vector<Ranked> live;
   live.reserve(candidates.size());
   for (size_t i = 0; i < candidates.size(); ++i) {
      Branch& b = candidates[i];
      if (!b.alive)
         continue;
      if (std::isnan(b.distance))
         throw std::invalid_argument("prune: branch '" + b.recipe.key() + "' has a NaN distance");
      live.push_back({i, &b});
   }
   // sorted(..., key=distance): stable.
   std::stable_sort(live.begin(), live.end(),
                    [](const Ranked& a, const Ranked& b) { return a.b->distance < b.b->distance; });
   // Noise-level ties: runs anchored at their first (smallest) distance, each holding every following branch within
   // kPruneTieTolerance of that anchor, keep input order (what Python's stable sort does for an exact tie). The
   // anchor bounds a run's span to the tolerance, so no pair further apart is ever reordered. See Beam.h.
   for (size_t i = 0; i < live.size();) {
      size_t j = i + 1;
      while (j < live.size() && live[j].b->distance - live[i].b->distance <= kPruneTieTolerance)
         ++j;
      if (j - i > 1)
         std::sort(live.begin() + static_cast<std::ptrdiff_t>(i), live.begin() + static_cast<std::ptrdiff_t>(j),
                   [](const Ranked& a, const Ranked& b) { return a.index < b.index; });
      i = j;
   }

   std::set<std::string> seen;
   std::vector<Branch> kept;
   for (const Ranked& r : live) {
      std::string key = r.b->recipe.key();
      if (seen.count(key) != 0)
         continue;
      seen.insert(std::move(key));
      kept.push_back(std::move(*r.b));
      if (static_cast<int>(kept.size()) == width)
         break;
   }
   return kept;
}

} // namespace ac
