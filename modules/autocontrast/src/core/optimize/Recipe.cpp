#include "core/optimize/Recipe.h"

#include <stdexcept>

namespace ac {

// tools/autocontrast/src/autocontrast/optimize/recipe.py:15-24
const std::map<std::string, std::string> kProcessForKind = {
   {"local_contrast", "MultiscaleLinearTransform"}, {"local_equalize", "LocalHistogramEqualization"},
   {"core_hdr", "HDRMultiscaleTransform"},          {"tonal_reshape", "CurvesTransformation"},
   {"black_point", "HistogramTransformation"},      {"chroma", "ColorSaturation"},
   {"background_neutralize", "BackgroundNeutralization"}, {"star_split", "StarXTerminator"},
};

Json action_to_json(const Action& a)
{
   Json j = Json::object();
   j["kind"] = a.kind;
   j["level"] = a.level;
   j["scale_arcsec"] = a.scale_arcsec ? Json(*a.scale_arcsec) : Json(nullptr);
   j["params"] = a.params;
   return j;
}

Action action_from_json(const Json& j)
{
   Action a;
   a.kind = j.at("kind").get<std::string>();
   a.level = j.at("level").get<std::string>();
   const Json& s = j.at("scale_arcsec");
   if (!s.is_null())
      a.scale_arcsec = s.get<double>();
   a.params = j.contains("params") ? j.at("params") : Json::object();
   return a;
}

Json pixinsight_step(const Action& a)
{
   const auto it = kProcessForKind.find(a.kind);
   if (it == kProcessForKind.end()) {
      std::string known;
      for (const auto& kv : kProcessForKind)
         known += (known.empty() ? "" : ", ") + kv.first;
      throw std::invalid_argument("unmapped action kind '" + a.kind + "'; known kinds: " + known);
   }
   Json params = a.params;
   params["strength"] = a.strength();
   if (a.scale_arcsec)
      params["scale_arcsec"] = *a.scale_arcsec;
   Json j = Json::object();
   j["process"] = it->second;
   j["action"] = a.key();
   j["params"] = std::move(params);
   return j;
}

Recipe Recipe::extend(const Action& a) const
{
   Recipe r = *this;
   r.actions.push_back(a);
   return r;
}

std::string Recipe::key() const
{
   std::string s;
   for (size_t i = 0; i < actions.size(); ++i)
      s += (i ? " | " : "") + actions[i].key();
   return s;
}

std::set<std::string> Recipe::applied_kinds() const
{
   std::set<std::string> s;
   for (const auto& a : actions)
      s.insert(a.kind);
   return s;
}

Json Recipe::to_json() const
{
   Json arr = Json::array();
   for (const auto& a : actions)
      arr.push_back(action_to_json(a));
   Json j = Json::object();
   j["actions"] = std::move(arr);
   return j;
}

Recipe Recipe::from_json(const Json& j)
{
   Recipe r;
   for (const auto& a : j.at("actions"))
      r.actions.push_back(action_from_json(a));
   return r;
}

Json Recipe::to_pixinsight_steps() const
{
   Json arr = Json::array();
   for (const auto& a : actions)
      arr.push_back(pixinsight_step(a));
   return arr;
}

} // namespace ac
