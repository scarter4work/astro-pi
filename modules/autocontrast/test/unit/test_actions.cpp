// AutoContrast: the action menu and recipe rendering (optimize/actions.py, recipe.py) vs the Python goldens.
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/Json.h"
#include "core/optimize/Actions.h"
#include "core/optimize/Recipe.h"

#include <cmath>
#include <fstream>
#include <set>
#include <vector>
#include <stdexcept>
#include <string>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/" + rel; }

// Python's json.dumps of the golden file's own ordered text, for byte comparison.
ac::Json load_ordered(const std::string& rel)
{
   std::ifstream f(golden(rel));
   return ac::Json::parse(f);
}

ac::Json expected_action_json(const ac::Json& g)   // golden rows also carry "key"; the dict form does not
{
   ac::Json j = ac::Json::object();
   j["kind"] = g.at("kind");
   j["level"] = g.at("level");
   j["scale_arcsec"] = g.at("scale_arcsec");
   j["params"] = g.at("params");
   return j;
}

} // namespace

TEST_CASE("Action::key format", "[actions]")
{
   CHECK(ac::Action{"local_contrast", "gentle", 2.0, {}}.key() == "local_contrast@2.000/gentle");
   CHECK(ac::Action{"star_split", "", std::nullopt, {}}.key() == "star_split@-/-");
   const auto g = load_ordered("actions/key_strength_layer.json");
   for (const auto& row : g.at("keys")) {
      ac::Action a{"local_contrast", row.at("level"), std::nullopt, {}};
      if (!row.at("scale").is_null())
         a.scale_arcsec = row.at("scale").get<double>();
      INFO(row.dump());
      CHECK(a.key() == row.at("key").get<std::string>());
   }
}

TEST_CASE("Action::strength", "[actions]")
{
   const auto g = load_ordered("actions/key_strength_layer.json");
   for (const auto& row : g.at("strengths"))
      CHECK(ac::Action{"x", row.at("level"), std::nullopt, {}}.strength() == row.at("strength").get<double>());
}

TEST_CASE("strength_rejects_unknown_level", "[actions]")
{
   const auto g = load_ordered("actions/key_strength_layer.json");
   try {
      (void)ac::Action{"x", "bogus", std::nullopt, {}}.strength();
      FAIL("must throw");
   } catch (const std::invalid_argument& e) {
      CHECK(std::string(e.what()) == g.at("bad_level_message").get<std::string>());
   }
}

TEST_CASE("scale_for_layer / layer_for_scale match Python (round-half-even)", "[actions]")
{
   const auto g = load_ordered("actions/key_strength_layer.json");
   for (const auto& r : g.at("scale_for_layer"))
      CHECK(ac::scale_for_layer(r.at("layer"), r.at("px")) == r.at("scale").get<double>());
   for (const auto& r : g.at("layer_for_scale")) {
      INFO(r.dump());
      CHECK(ac::layer_for_scale(r.at("scale"), r.at("px")) == r.at("layer").get<int>());
   }
   // log2 lands exactly on .5: Python round() goes to the even neighbour (verified against CPython).
   CHECK(ac::layer_for_scale(std::pow(2.0, 1.5), 1.0) == 2);
   CHECK(ac::layer_for_scale(std::pow(2.0, 2.5), 1.0) == 2);
   CHECK(ac::layer_for_scale(std::pow(2.0, 3.5), 1.0) == 4);
}

TEST_CASE("layer_for_scale_below_pixel_scale_throws", "[actions]")
{
   const auto g = load_ordered("actions/key_strength_layer.json");
   try {
      (void)ac::layer_for_scale(0.5, 1.0);
      FAIL("must throw");
   } catch (const std::invalid_argument& e) {
      CHECK(std::string(e.what()) == g.at("layer_below_pixel_message").get<std::string>());
   }
}

TEST_CASE("constants match Python", "[actions]")
{
   const auto g = load_ordered("actions/constants.json");
   for (size_t i = 0; i < ac::kLevels.size(); ++i)
      CHECK(std::string(ac::kLevels[i]) == g.at("LEVELS")[i].get<std::string>());
   CHECK(ac::kOnceOnly.size() == g.at("ONCE_ONLY").size());
   for (const auto& k : g.at("ONCE_ONLY"))
      CHECK(ac::kOnceOnly.count(k.get<std::string>()) == 1);
   for (const auto& [kind, proc] : g.at("PROCESS_FOR_KIND").items())
      CHECK(ac::kProcessForKind.at(kind) == proc.get<std::string>());
   CHECK(ac::kProcessForKind.size() == g.at("PROCESS_FOR_KIND").size());
}

TEST_CASE("menu_matches_python", "[actions]")
{
   const auto g = load_ordered("actions/available_actions.json");
   REQUIRE(g.at("cases").size() == 9);
   for (const auto& c : g.at("cases")) {
      std::set<std::string> applied;
      for (const auto& k : c.at("applied_kinds"))
         applied.insert(k.get<std::string>());
      const auto menu = ac::available_actions(c.at("pixel_scale"), c.at("psf"), c.at("n_scales"),
                                              c.at("palette_compatible"), applied);
      INFO(c.dump());
      REQUIRE(menu.size() == c.at("actions").size());
      for (size_t i = 0; i < menu.size(); ++i) {
         const auto want = expected_action_json(c.at("actions")[i]);
         CHECK(ac::dump_python(ac::action_to_json(menu[i])) == ac::dump_python(want));
         CHECK(menu[i].key() == c.at("actions")[i].at("key").get<std::string>());
      }
   }
}

TEST_CASE("menu serialises byte-identically to Python json.dumps (insertion order)", "[actions]")
{
   const auto d = load_ordered("recipe/dumps.json");
   const auto menu = ac::available_actions(1.0, 2.0, 6, true, {});
   REQUIRE(menu.size() == d.at("menu").size());
   for (size_t i = 0; i < menu.size(); ++i)
      CHECK(ac::dump_python(ac::action_to_json(menu[i])) == d.at("menu")[i].get<std::string>());
}

TEST_CASE("pixinsight_step_matches_python / recipe_json_round_trip", "[actions][recipe]")
{
   const auto g = load_ordered("recipe/recipes.json");
   ac::Recipe built;
   const auto& snaps = g.at("snapshots");
   const auto dumps = load_ordered("recipe/dumps.json");
   REQUIRE(dumps.at("snapshots").size() == snaps.size());
   for (size_t s = 0; s < snaps.size(); ++s) {
      const auto& snap = snaps[s];
      if (s > 0)
         built = built.extend(ac::action_from_json(snap.at("dict").at("actions").back()));
      CHECK(built.key() == snap.at("key").get<std::string>());
      std::vector<std::string> want_kinds;
      for (const auto& k : snap.at("applied_kinds"))
         want_kinds.push_back(k.get<std::string>());
      const auto got = built.applied_kinds();
      CHECK(std::vector<std::string>(got.begin(), got.end()) == want_kinds);   // golden list is sorted
      CHECK(ac::dump_python(built.to_json()) == dumps.at("snapshots")[s].at("dict").get<std::string>());
      CHECK(ac::dump_python(built.to_pixinsight_steps()) == dumps.at("snapshots")[s].at("steps").get<std::string>());
      // same values as the key-sorted golden (compared order-insensitively)
      CHECK(nlohmann::json::parse(ac::dump_python(built.to_json())) == nlohmann::json::parse(snap.at("dict").dump()));
      CHECK(nlohmann::json::parse(ac::dump_python(built.to_pixinsight_steps())) ==
            nlohmann::json::parse(snap.at("steps").dump()));
      const auto rt = ac::Recipe::from_json(ac::Json::parse(ac::dump_python(built.to_json())));
      CHECK(rt.actions == built.actions);
      CHECK(ac::dump_python(rt.to_json()) == ac::dump_python(built.to_json()));
   }
   try {
      ac::Recipe r;
      r.actions.push_back(ac::Action{"nonsense", "gentle", std::nullopt, {}});
      (void)r.to_pixinsight_steps();
      FAIL("must throw");
   } catch (const std::invalid_argument& e) {
      CHECK(std::string(e.what()) == g.at("unmapped_kind_message").get<std::string>());
   }
}

TEST_CASE("Action equality ignores params", "[actions]")
{
   ac::Action a{"core_hdr", "gentle", std::nullopt, ac::Json{{"layers", 2}}};
   ac::Action b{"core_hdr", "gentle", std::nullopt, ac::Json{{"layers", 9}}};
   CHECK(a == b);
   b.scale_arcsec = 1.0;
   CHECK_FALSE(a == b);
}
