#include "catch_amalgamated.hpp"
#include "nukex/io/filter_classifier.hpp"
#include "nukex/io/filter_alias.hpp"
#include "nukex/core/frame_metadata.hpp"

using namespace nukex;

static FrameMetadata make_meta(const std::string& filter,
                               const std::string& bayer = "",
                               const std::string& instrument = "ASI585MC") {
    FrameMetadata m;
    m.filter        = filter;
    m.bayer_pattern = bayer;
    m.instrument    = instrument;
    return m;
}

TEST_CASE("FilterClassifier: HaO3 dual-NB on Bayer", "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("HaO3", "RGGB"));
    REQUIRE(f.cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "HaO3");
    REQUIRE(f.camera == "ASI585MC");
}

TEST_CASE("FilterClassifier: S2O3 dual-NB on Bayer", "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("S2O3", "RGGB"));
    REQUIRE(f.cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "S2O3");
}

TEST_CASE("FilterClassifier: H-alpha single-line narrowband on mono", "[filter_classifier]") {
    FilterClassifier c;
    REQUIRE(c.classify(make_meta("Ha"))      .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("Halpha"))  .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("H-alpha")) .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("OIII"))    .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("O3"))      .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("SII"))     .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("S2"))      .cls == FilterClass::NARROWBAND_SINGLE);
}

TEST_CASE("FilterClassifier: clear/broadband slot labels route as broadband, never UNKNOWN",
          "[filter_classifier]") {
    FilterClassifier c;
    // "Full" is the user's Optolong L-Pro / L-QEF broadband slot label. On a
    // Bayer frame it MUST classify as BROADBAND_OSC — not UNKNOWN, which would
    // hard-abort the whole stack (regression from v5.1.0).
    Filter full = c.classify(make_meta("Full", "RGGB"));
    REQUIRE(full.cls == FilterClass::BROADBAND_OSC);
    REQUIRE(full.name == "OSC");  // routed to the canonical broadband slot

    for (const char* name : {"Full", "Clear", "None", "Open", "L-Pro",
                             "L-QEF", "L-Quad Enhance", "UV/IR", "UV-IR-Cut"}) {
        REQUIRE(c.classify(make_meta(name, "RGGB")).cls == FilterClass::BROADBAND_OSC);
        REQUIRE(c.classify(make_meta(name)).cls == FilterClass::BROADBAND_L); // mono variant
    }
}

TEST_CASE("FilterClassifier: L / R / G / B broadband on mono", "[filter_classifier]") {
    FilterClassifier c;
    REQUIRE(c.classify(make_meta("L"))         .cls == FilterClass::BROADBAND_L);
    REQUIRE(c.classify(make_meta("Luminance")) .cls == FilterClass::BROADBAND_L);
    REQUIRE(c.classify(make_meta("R"))         .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("Red"))       .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("G"))         .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("Green"))     .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("B"))         .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("Blue"))      .cls == FilterClass::BROADBAND_RGB);
}

TEST_CASE("FilterClassifier: alias normalization", "[filter_classifier]") {
    FilterClassifier c;
    REQUIRE(c.classify(make_meta("L-eXtreme")).cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(c.classify(make_meta("L_eXtreme")).cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(c.classify(make_meta("L Extreme")).cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(c.classify(make_meta("LExtreme") ).cls == FilterClass::DUAL_NB_OSC);
}

TEST_CASE("FilterClassifier: missing FILTER + Bayer -> BROADBAND_OSC silent", "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("", "RGGB"));
    REQUIRE(f.cls == FilterClass::BROADBAND_OSC);
    REQUIRE(f.name == "OSC");
}

TEST_CASE("FilterClassifier: missing FILTER + mono -> BROADBAND_L L_unnamed silent",
          "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("", ""));
    REQUIRE(f.cls == FilterClass::BROADBAND_L);
    REQUIRE(f.name == "L_unnamed");
}

TEST_CASE("FilterClassifier: unknown FILTER + Bayer -> UNKNOWN sentinel",
          "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("ALP-T-fake-2026", "RGGB"));
    REQUIRE(f.cls == FilterClass::UNKNOWN);
    REQUIRE(f.name == "ALP-T-fake-2026");
}

TEST_CASE("FilterClassifier: unknown FILTER + mono -> BROADBAND_L preserve name + warn",
          "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("custom_Hbeta", ""));
    REQUIRE(f.cls == FilterClass::BROADBAND_L);
    REQUIRE(f.name == "custom_Hbeta");
    REQUIRE(c.last_warning().find("Unknown filter") != std::string::npos);
    REQUIRE(c.last_warning().find("custom_Hbeta")   != std::string::npos);
}

TEST_CASE("FilterClassifier: broadband names on Bayer resolve to BROADBAND_OSC", "[filter_classifier]") {
    FilterClassifier c;
    for (const char* name : {"L", "Luminance", "LPro", "L-Pro", "UV/IR Cut", "CLS-CCD", "LPS-D1", "L1"}) {
        Filter f = c.classify(make_meta(name, "RGGB", "ZWO ASI2400MC Pro"));
        INFO(name);
        REQUIRE(f.cls  == FilterClass::BROADBAND_OSC);
        REQUIRE(f.name == "OSC");
        REQUIRE(c.last_warning().empty());
    }
}

TEST_CASE("FilterClassifier: broadband LPR names on mono resolve to BROADBAND_L named L", "[filter_classifier]") {
    FilterClassifier c;
    for (const char* name : {"LPro", "UV-IR-Cut", "CLS", "LPS-D2"}) {
        Filter f = c.classify(make_meta(name, "", "ASI2600MM"));
        INFO(name);
        REQUIRE(f.cls  == FilterClass::BROADBAND_L);
        REQUIRE(f.name == "L");
        REQUIRE(c.last_warning().empty());
    }
}

TEST_CASE("FilterClassifier: quad-band glass is broadband, not narrowband",
          "[filter_classifier]") {
    // v5.0.0.2 put the L-Quad family in the narrowband table so that a batch
    // of FILTER='Lqef' frames would stack at all instead of stopping at start.
    // It stacked -- as an emission-line target. On M63, a galaxy, the Q-solve
    // was asked to decompose pure continuum into Ha, OIII and SII: one line
    // came out at or below zero across the whole frame and was clamped, so the
    // red channel of a 114-frame stack held 7,540 non-zero pixels out of
    // 7,547,618 and the picture came out teal.
    //
    // The filter says what light gets through, not what the target emits. A
    // 175 nm passband is not narrowband by any definition -- compare
    // L-Ultimate at 3 nm, ALP-T at 5, L-eXtreme at 7 -- and this is light
    // pollution glass used for RGB imaging. It belongs with the other LPR
    // filters already in the broadband set (lpro, lpr, lps, lpsd1...).
    FilterClassifier c;
    for (const char* spelling : { "Lqef", "L-QEF", "LQEF", "L-Quad", "L Quad Enhance",
                                  "Optolong L-Quad Enhance", "L-Synergy",
                                  "HaO3S2", "HaOIIISII" }) {
        INFO("FILTER = " << spelling);
        Filter f = c.classify(make_meta(spelling, "RGGB"));
        REQUIRE(f.cls  == FilterClass::BROADBAND_OSC);
        REQUIRE(f.name == "OSC");
        // Still no warning: the batch stacks, which is what the v5.0.0.2 entry
        // was added to achieve. Only the interpretation changed.
        REQUIRE(c.last_warning().empty());
    }
}

TEST_CASE("FilterClassifier: genuine dual-narrowband glass is untouched",
          "[filter_classifier]") {
    // The rule is about passband width, and these are 3 to 25 nm. Moving the
    // quad-band names must not take the real narrowband filters with them.
    FilterClassifier c;
    for (auto [spelling, canonical] : std::vector<std::pair<const char*, const char*>>{
             {"L-eXtreme", "L-eXtreme"}, {"L-Ultimate", "L-Ultimate"},
             {"L-eNhance", "L-eNhance"}, {"ALP-T", "ALP-T"},
             {"HaO3", "HaO3"},           {"S2O3", "S2O3"}}) {
        INFO("FILTER = " << spelling);
        Filter f = c.classify(make_meta(spelling, "RGGB"));
        REQUIRE(f.cls  == FilterClass::DUAL_NB_OSC);
        REQUIRE(f.name == canonical);
    }
}

TEST_CASE("FilterClassifier: quad-band glass on a mono sensor is luminance",
          "[filter_classifier]") {
    // Same rule, other sensor: a broadband filter on a mono frame is a
    // luminance channel, and it must not raise the unknown-filter warning.
    FilterClassifier c;
    Filter f = c.classify(make_meta("Lqef"));
    REQUIRE(f.cls  == FilterClass::BROADBAND_L);
    REQUIRE(f.name == "L");
    REQUIRE(c.last_warning().empty());
}

TEST_CASE("FilterClassifier: a taught alias resolves a name the table does not know",
          "[filter_classifier]") {
    // The open-ended half of the problem: FITS FILTER values are whatever the
    // capture software wrote, so no shipped table can list them all. When the
    // interface learns one, the classifier must honour it.
    FilterAliasStore aliases;
    aliases.set("MyDuoBand", "HaO3");

    FilterClassifier c;
    REQUIRE(c.classify(make_meta("MyDuoBand", "RGGB")).cls == FilterClass::UNKNOWN);

    c.set_aliases(aliases);
    Filter f = c.classify(make_meta("MyDuoBand", "RGGB"));
    REQUIRE(f.cls  == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "HaO3");
}

TEST_CASE("FilterClassifier: an alias cannot shadow a shipped name",
          "[filter_classifier]") {
    // Consulted only after the built-in table, so a user file can add
    // spellings but never redefine what Ha or HaO3 mean.
    FilterAliasStore aliases;
    aliases.set("Ha", "S2O3");

    FilterClassifier c;
    c.set_aliases(aliases);
    Filter f = c.classify(make_meta("Ha"));
    REQUIRE(f.name == "Ha");
}

TEST_CASE("FilterClassifier: an alias is matched on the normalised name",
          "[filter_classifier]") {
    // The fixture used to teach "lqef", which is no longer a name the table
    // does not know -- quad-band glass is classified as the broadband LPR
    // filter it is, and the shipped table is consulted BEFORE taught aliases.
    // Any capture-software spelling still works; this one just has to be a
    // name NukeX genuinely has not heard of, which is what the case is about.
    FilterAliasStore aliases;
    aliases.set("mydualband", "HaO3");

    FilterClassifier c;
    c.set_aliases(aliases);
    REQUIRE(c.classify(make_meta("My-DualBand", "RGGB")).name == "HaO3");
    REQUIRE(c.classify(make_meta("my dual band", "RGGB")).name == "HaO3");
}

TEST_CASE("FilterClassifier: a taught alias cannot pull quad-band glass back "
          "into the narrowband path", "[filter_classifier]") {
    // Worth stating rather than discovering: aliases are consulted only when
    // the shipped table has no answer, so teaching "lqef" a narrowband
    // canonical does NOT restore the three-line solve. The three-line Q-solve
    // has no shipped spelling any more. ChannelDecomposer still supports it,
    // and reaching it again needs a deliberate opt-in, not an alias.
    FilterAliasStore aliases;
    aliases.set("lqef", "HaO3");

    FilterClassifier c;
    c.set_aliases(aliases);
    Filter f = c.classify(make_meta("Lqef", "RGGB"));
    REQUIRE(f.cls  == FilterClass::BROADBAND_OSC);
    REQUIRE(f.name == "OSC");
}
