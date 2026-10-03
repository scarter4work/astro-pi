// User-taught filter aliases.
//
// A filter NukeX does not recognise stops a Bayer batch at start. That is the
// right default -- guessing would mis-colour the result silently -- but filter
// names in FITS headers are whatever the capture software wrote, so the set of
// spellings is open-ended and cannot be enumerated in advance. The interface
// offers to learn one: it asks which emission lines the filter passes, and
// stores the answer against the normalised header name.
//
// These cases cover the two pieces that are not UI: which line sets map to a
// filter NukeX can actually solve, and the file the answer is kept in.

#include "catch_amalgamated.hpp"
#include "nukex/io/filter_alias.hpp"

#include <filesystem>
#include <fstream>

using namespace nukex;
namespace fs = std::filesystem;

namespace {
fs::path temp_alias_file(const char* tag) {
    auto p = fs::temp_directory_path() / (std::string("nukex_alias_") + tag + ".json");
    fs::remove(p);
    return p;
}
} // namespace

// ── which line sets NukeX can solve ──────────────────────────────────

TEST_CASE("canonical_for_lines: the sets with a calibrated entry", "[filter_alias]") {
    // Ha+OIII and SII+OIII are the two-line duals; all three is the exactly
    // determined case that L-Quad Enhance and friends land on.
    REQUIRE(canonical_for_lines({QSolveLine::Ha, QSolveLine::OIII}) == "HaO3");
    REQUIRE(canonical_for_lines({QSolveLine::SII, QSolveLine::OIII}) == "S2O3");
    REQUIRE(canonical_for_lines({QSolveLine::Ha, QSolveLine::OIII,
                                 QSolveLine::SII}) == "HaO3S2");
}

TEST_CASE("canonical_for_lines: a single line is its own narrowband filter",
          "[filter_alias]") {
    REQUIRE(canonical_for_lines({QSolveLine::Ha})   == "Ha");
    REQUIRE(canonical_for_lines({QSolveLine::OIII}) == "OIII");
    REQUIRE(canonical_for_lines({QSolveLine::SII})  == "SII");
}

TEST_CASE("canonical_for_lines: Ha and SII without OIII has no entry",
          "[filter_alias]") {
    // Not an oversight -- the shipped database has no calibrated filter for
    // that pair, and inventing one would put made-up numbers into a Q-solve.
    // The dialog must refuse it rather than accept it and fail later.
    REQUIRE(canonical_for_lines({QSolveLine::Ha, QSolveLine::SII}).empty());
}

TEST_CASE("canonical_for_lines: no lines at all is not a filter", "[filter_alias]") {
    REQUIRE(canonical_for_lines({}).empty());
}

TEST_CASE("canonical_for_lines: the answer does not depend on tick order",
          "[filter_alias]") {
    REQUIRE(canonical_for_lines({QSolveLine::SII, QSolveLine::Ha,
                                 QSolveLine::OIII}) == "HaO3S2");
    REQUIRE(canonical_for_lines({QSolveLine::OIII, QSolveLine::Ha}) == "HaO3");
}

// ── the alias file ───────────────────────────────────────────────────

TEST_CASE("FilterAliasStore: a missing file is empty, not an error",
          "[filter_alias]") {
    // First run on any machine. Nothing to load and nothing to complain about.
    FilterAliasStore s;
    REQUIRE(s.load((fs::temp_directory_path() / "nukex_no_such_alias.json").string()));
    REQUIRE(s.empty());
    REQUIRE(s.lookup("lqef").empty());
}

TEST_CASE("FilterAliasStore: an alias survives a save and load", "[filter_alias]") {
    const auto path = temp_alias_file("roundtrip");
    {
        FilterAliasStore s;
        s.set("lqef", "HaO3S2");
        REQUIRE(s.save(path.string()));
    }
    FilterAliasStore s2;
    REQUIRE(s2.load(path.string()));
    REQUIRE(s2.lookup("lqef") == "HaO3S2");
    fs::remove(path);
}

TEST_CASE("FilterAliasStore: lookup is by normalised name", "[filter_alias]") {
    // The header may say "L-QEF", "l qef" or "Lqef"; they are one filter. The
    // store normalises on the way in and on the way out, so the caller does not
    // have to remember to.
    FilterAliasStore s;
    s.set("L-QEF", "HaO3S2");
    REQUIRE(s.lookup("lqef")   == "HaO3S2");
    REQUIRE(s.lookup("L Qef")  == "HaO3S2");
    REQUIRE(s.lookup("l_q_e_f")== "HaO3S2");
    REQUIRE(s.lookup("lqe").empty());
}

TEST_CASE("FilterAliasStore: a malformed file loads as empty and says so",
          "[filter_alias]") {
    // A truncated or hand-edited file must not take the stack down with it.
    const auto path = temp_alias_file("malformed");
    { std::ofstream(path) << "{ this is not json"; }
    FilterAliasStore s;
    REQUIRE_FALSE(s.load(path.string()));
    REQUIRE(s.empty());
    fs::remove(path);
}

TEST_CASE("FilterAliasStore: saving twice keeps both aliases", "[filter_alias]") {
    const auto path = temp_alias_file("append");
    {
        FilterAliasStore s;
        s.set("lqef", "HaO3S2");
        REQUIRE(s.save(path.string()));
    }
    {
        FilterAliasStore s;
        REQUIRE(s.load(path.string()));
        s.set("duoband", "HaO3");
        REQUIRE(s.save(path.string()));
    }
    FilterAliasStore s;
    REQUIRE(s.load(path.string()));
    REQUIRE(s.lookup("lqef")    == "HaO3S2");
    REQUIRE(s.lookup("duoband") == "HaO3");
    fs::remove(path);
}
