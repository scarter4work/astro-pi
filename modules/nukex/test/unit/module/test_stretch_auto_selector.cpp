#include "catch_amalgamated.hpp"
#include "stretch_auto_selector.hpp"
#include "nukex/core/frame_metadata.hpp"
#include "nukex/stretch/veralux_stretch.hpp"

using namespace nukex;

static FrameMetadata meta_of(const std::string& filter, const std::string& bayer,
                             const std::string& instrument) {
    FrameMetadata m;
    m.filter        = filter;
    m.bayer_pattern = bayer;
    m.instrument    = instrument;
    return m;
}

TEST_CASE("select_auto: BROADBAND_L picks VeraLux and logs the class name", "[module][auto_selector]") {
    auto sel = select_auto(FilterClass::BROADBAND_L);
    REQUIRE(sel.op != nullptr);
    REQUIRE(dynamic_cast<VeraLuxStretch*>(sel.op.get()) != nullptr);
    REQUIRE(sel.log_line.find("BROADBAND_L") != std::string::npos);
    REQUIRE(sel.log_line.find("VeraLux")     != std::string::npos);
}

TEST_CASE("select_auto: every class produces a non-null op + non-empty log", "[module][auto_selector]") {
    for (FilterClass c : {FilterClass::UNKNOWN, FilterClass::BROADBAND_L, FilterClass::BROADBAND_RGB,
                          FilterClass::BROADBAND_OSC, FilterClass::NARROWBAND_SINGLE,
                          FilterClass::DUAL_NB_OSC}) {
        auto sel = select_auto(c);
        REQUIRE(sel.op != nullptr);
        REQUIRE(!sel.log_line.empty());
    }
}

TEST_CASE("select_auto(meta): log line carries FILTER/BAYERPAT/INSTRUME + class", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("L", "", "ASI2600MM"));
    REQUIRE(dynamic_cast<VeraLuxStretch*>(sel.op.get()) != nullptr);
    REQUIRE(sel.log_line.find("FILTER='L'")           != std::string::npos);
    REQUIRE(sel.log_line.find("BAYERPAT=''")          != std::string::npos);
    REQUIRE(sel.log_line.find("INSTRUME='ASI2600MM'") != std::string::npos);
    REQUIRE(sel.log_line.find("BROADBAND_L")          != std::string::npos);
    REQUIRE(sel.log_line.find("VeraLux")              != std::string::npos);
}

TEST_CASE("select_auto(meta): Bayer without FILTER classifies as BROADBAND_OSC", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("", "RGGB", "ZWO ASI2400MC Pro"));
    REQUIRE(sel.log_line.find("BAYERPAT='RGGB'") != std::string::npos);
    REQUIRE(sel.log_line.find("BROADBAND_OSC")   != std::string::npos);
}

TEST_CASE("select_auto(meta): dual-NB FILTER on Bayer classifies as DUAL_NB_OSC", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("HaO3", "RGGB", "ZWO ASI2400MC Pro"));
    REQUIRE(sel.log_line.find("FILTER='HaO3'") != std::string::npos);
    REQUIRE(sel.log_line.find("DUAL_NB_OSC")   != std::string::npos);
}

TEST_CASE("select_auto(meta): single-line FILTER on mono classifies as NARROWBAND_SINGLE", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("Ha", "", "ASI2600MM"));
    REQUIRE(sel.log_line.find("FILTER='Ha'")       != std::string::npos);
    REQUIRE(sel.log_line.find("NARROWBAND_SINGLE") != std::string::npos);
}

TEST_CASE("select_auto(meta): unknown FILTER on mono appends the classifier warning", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("5", "", "Asi294mc Pro"));
    REQUIRE(sel.log_line.find("BROADBAND_L")       != std::string::npos);
    REQUIRE(sel.log_line.find("Unknown filter '5'") != std::string::npos);
}
