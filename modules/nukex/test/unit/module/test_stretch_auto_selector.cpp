#include "catch_amalgamated.hpp"
#include "stretch_auto_selector.hpp"
#include "fits_metadata.hpp"
#include "nukex/stretch/veralux_stretch.hpp"

using namespace nukex;

TEST_CASE("select_auto: BROADBAND_L picks VeraLux and logs class name", "[module][auto_selector]") {
    auto sel = select_auto(FilterClass::BROADBAND_L);
    REQUIRE(sel.op != nullptr);
    REQUIRE(dynamic_cast<VeraLuxStretch*>(sel.op.get()) != nullptr);
    REQUIRE(sel.log_line.find("BROADBAND_L") != std::string::npos);
    REQUIRE(sel.log_line.find("VeraLux") != std::string::npos);
}

TEST_CASE("select_auto: all classes produce non-null op + non-empty log", "[module][auto_selector]") {
    for (FilterClass c : {FilterClass::UNKNOWN, FilterClass::BROADBAND_L,
                          FilterClass::BROADBAND_RGB, FilterClass::BROADBAND_OSC,
                          FilterClass::NARROWBAND_SINGLE, FilterClass::DUAL_NB_OSC}) {
        auto sel = select_auto(c);
        REQUIRE(sel.op != nullptr);
        REQUIRE(!sel.log_line.empty());
    }
}

TEST_CASE("select_auto(meta): log line includes FITS FILTER/BAYERPAT/NAXIS3",
          "[module][auto_selector]") {
    FITSMetadata m;
    m.filter    = "L";
    m.bayer_pat = "";
    m.naxis3    = 1;
    m.read_ok   = true;

    auto sel = select_auto(m);
    REQUIRE(sel.op != nullptr);
    REQUIRE(dynamic_cast<VeraLuxStretch*>(sel.op.get()) != nullptr);

    // The enriched rationale must carry the actual FITS values that drove
    // the classification, so a user can trace "why BROADBAND_L?" back to
    // the source of truth.
    REQUIRE(sel.log_line.find("FILTER='L'")    != std::string::npos);
    REQUIRE(sel.log_line.find("BAYERPAT=''")   != std::string::npos);
    REQUIRE(sel.log_line.find("NAXIS3=1")      != std::string::npos);
    REQUIRE(sel.log_line.find("BROADBAND_L")   != std::string::npos);
    REQUIRE(sel.log_line.find("VeraLux")       != std::string::npos);
}

TEST_CASE("select_auto(meta): Bayer/OSC classification shown in log",
          "[module][auto_selector]") {
    FITSMetadata m;
    m.filter    = "";
    m.bayer_pat = "RGGB";
    m.naxis3    = 1;
    m.read_ok   = true;

    auto sel = select_auto(m);
    REQUIRE(sel.log_line.find("BAYERPAT='RGGB'") != std::string::npos);
    REQUIRE(sel.log_line.find("BROADBAND_OSC")   != std::string::npos);
}

TEST_CASE("select_auto(meta): Narrowband classification via FILTER name",
          "[module][auto_selector]") {
    FITSMetadata m;
    m.filter    = "Ha";
    m.bayer_pat = "";
    m.naxis3    = 1;
    m.read_ok   = true;

    auto sel = select_auto(m);
    REQUIRE(sel.log_line.find("FILTER='Ha'")      != std::string::npos);
    REQUIRE(sel.log_line.find("NARROWBAND_SINGLE") != std::string::npos);
}
