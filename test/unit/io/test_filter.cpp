#include "catch_amalgamated.hpp"
#include "nukex/io/filter.hpp"

using namespace nukex;

TEST_CASE("Filter: default-constructed is unknown", "[filter]") {
    Filter f;
    REQUIRE(f.cls == FilterClass::UNKNOWN);
    REQUIRE(f.name.empty());
    REQUIRE(f.camera.empty());
    REQUIRE(f.bandwidth.center_nm == 0.0);
    REQUIRE(f.bandwidth.fwhm_nm == 0.0);
}

TEST_CASE("Filter: explicit construction holds all fields", "[filter]") {
    Filter f{
        FilterClass::DUAL_NB_OSC,
        "HaO3",
        "ASI585MC",
        BandwidthSpec{578.5, 155.0}
    };
    REQUIRE(f.cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "HaO3");
    REQUIRE(f.camera == "ASI585MC");
    REQUIRE(f.bandwidth.center_nm == Catch::Approx(578.5));
    REQUIRE(f.bandwidth.fwhm_nm == Catch::Approx(155.0));
}

TEST_CASE("FilterClass enum exposes 5 + UNKNOWN values", "[filter]") {
    REQUIRE(static_cast<int>(FilterClass::UNKNOWN)            == 0);
    REQUIRE(static_cast<int>(FilterClass::BROADBAND_L)        == 1);
    REQUIRE(static_cast<int>(FilterClass::BROADBAND_RGB)      == 2);
    REQUIRE(static_cast<int>(FilterClass::BROADBAND_OSC)      == 3);
    REQUIRE(static_cast<int>(FilterClass::NARROWBAND_SINGLE)  == 4);
    REQUIRE(static_cast<int>(FilterClass::DUAL_NB_OSC)        == 5);
}

TEST_CASE("filter_class_name returns stable strings", "[filter]") {
    REQUIRE(std::string(filter_class_name(FilterClass::BROADBAND_L))       == "BROADBAND_L");
    REQUIRE(std::string(filter_class_name(FilterClass::BROADBAND_RGB))     == "BROADBAND_RGB");
    REQUIRE(std::string(filter_class_name(FilterClass::BROADBAND_OSC))     == "BROADBAND_OSC");
    REQUIRE(std::string(filter_class_name(FilterClass::NARROWBAND_SINGLE)) == "NARROWBAND_SINGLE");
    REQUIRE(std::string(filter_class_name(FilterClass::DUAL_NB_OSC))       == "DUAL_NB_OSC");
    REQUIRE(std::string(filter_class_name(FilterClass::UNKNOWN))           == "UNKNOWN");
}
