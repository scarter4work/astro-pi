#include "catch_amalgamated.hpp"
#include "nukex/io/filter_classifier.hpp"
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
