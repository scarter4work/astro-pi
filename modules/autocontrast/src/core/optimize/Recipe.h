// AutoContrast core (PCL-free): the recipe, an ordered replayable audit log (design §12) — optimize/recipe.py.
#pragma once

#include "core/Json.h"
#include "core/optimize/Actions.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ac {

// §6.2's action -> stock PixInsight process mapping.
extern const std::map<std::string, std::string> kProcessForKind;

// {"kind","level","scale_arcsec"(null or number),"params"} — Python's action_to_dict, key order included.
Json action_to_json(const Action& a);
Action action_from_json(const Json& j);   // throws nlohmann exceptions on a missing key, as KeyError would
// {"process","action","params"} with strength (and scale_arcsec) appended to the params. Throws
// std::invalid_argument for an unmapped kind (Python's ValueError text).
Json pixinsight_step(const Action& a);

struct Recipe {
   std::vector<Action> actions;

   Recipe extend(const Action& a) const;
   std::string key() const;                    // " | "-joined action keys
   std::set<std::string> applied_kinds() const;
   Json to_json() const;                       // {"actions": [...]}
   static Recipe from_json(const Json& j);
   Json to_pixinsight_steps() const;           // array
};

} // namespace ac
