// cavity_meta_from_json: the strict inverse of cavity_meta_json (Task 25 reads a saved model's
// GaiaDepthGrade:Recon:Meta property back with it).
#include <catch_amalgamated.hpp>

#include "Blister.h"
#include "TestFs.h"
#include "core/recon/Cavity.h"
#include "core/recon/Reconstruct.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

using namespace gdg::recon;
using gdg::test::thrown_message;

namespace {

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

void check_identical(const CavityMeta& a, const CavityMeta& b) {
    CHECK(same_bits(a.w_geometry, b.w_geometry));
    CHECK(same_bits(a.w_ionization_max, b.w_ionization_max));
    CHECK(same_bits(a.w_ionization_effective, b.w_ionization_effective));
    CHECK(same_bits(a.w_emission, b.w_emission));
    CHECK(same_bits(a.emission_sign, b.emission_sign));
    CHECK(same_bits(a.w_dust, b.w_dust));
    CHECK(same_bits(a.ionization_confidence, b.ionization_confidence));
    CHECK(same_bits(a.ratio_confidence, b.ratio_confidence));
    CHECK(a.is_linear == b.is_linear);
    CHECK(a.sii_present == b.sii_present);
    CHECK(a.capture_mode == b.capture_mode);
    CHECK(a.ionizer_source == b.ionizer_source);
    CHECK(same_bits(a.footprint_frac, b.footprint_frac));
    CHECK(a.footprint_px == b.footprint_px);
}

CavityMeta blister_meta() {
    const gdg::test::Blister b = gdg::test::make_blister();
    ReconInputs in;
    in.mode = "sho";
    in.channels = {{"ha", b.ha}, {"oiii", b.oiii}, {"sii", b.sii}};
    in.ionizer_override = ionizer_from_xy(b.truth.apex_x, b.truth.apex_y, std::nullopt);
    return reconstruct(in, ReconConfig{}, gdg::Log{}).model.meta;
}

/// The default meta's JSON with `from` replaced by `to` (REQUIREd present once).
std::string mutated(const std::string& from, const std::string& to) {
    std::string j = cavity_meta_json(CavityMeta{});
    const size_t at = j.find(from);
    REQUIRE(at != std::string::npos);
    j.replace(at, from.size(), to);
    return j;
}

void rejects(const std::string& json, const std::string& needle) {
    INFO("json: " << json);
    const std::string msg = thrown_message([&] { cavity_meta_from_json(json); });
    CHECK_FALSE(msg.empty());
    CHECK(msg.find(needle) != std::string::npos);
    CHECK_THROWS_AS(cavity_meta_from_json(json), std::invalid_argument);
}

}  // namespace

TEST_CASE("cavity_meta_from_json_round_trips_a_blister_model", "[recon][cavity][meta]") {
    const CavityMeta m = blister_meta();
    const std::string j = cavity_meta_json(m);
    check_identical(cavity_meta_from_json(j), m);
    CHECK(cavity_meta_json(cavity_meta_from_json(j)) == j);
}

TEST_CASE("cavity_meta_from_json_round_trips_edge_values", "[recon][cavity][meta]") {
    CavityMeta m;
    m.w_geometry = -0.0;
    m.w_ionization_max = 5e-324;  // smallest subnormal
    m.w_ionization_effective = std::numeric_limits<double>::max();
    m.w_emission = 1e-05;
    m.emission_sign = -2.5e-07;
    m.w_dust = 1.5e+300;
    m.ionization_confidence = 0.1 + 0.2;  // not exactly 0.3
    m.ratio_confidence = 123456789012345.0;
    m.is_linear = true;
    m.sii_present = true;
    m.capture_mode = std::string("a\"b\\c\b\f\n\r\t\x01\x1f/\x7f", 15) + "\xc3\xa9 \xe2\x9c\x93";
    m.ionizer_source = "";
    m.footprint_frac = 1e16;
    m.footprint_px = std::numeric_limits<long long>::min();
    const CavityMeta back = cavity_meta_from_json(cavity_meta_json(m));
    check_identical(back, m);
    CHECK(std::signbit(back.w_geometry));
    m.footprint_px = std::numeric_limits<long long>::max();
    CHECK(cavity_meta_from_json(cavity_meta_json(m)).footprint_px == m.footprint_px);
}

TEST_CASE("cavity_meta_from_json_rejects_malformed_input", "[recon][cavity][meta]") {
    const std::string good = cavity_meta_json(CavityMeta{});
    SECTION("key set") {
        rejects(mutated("\"w_dust\"", "\"w_dusty\""), "w_dust");  // an unknown key and a missing one
        rejects(mutated("\"w_dust\"", "\"w_geometry\""), "duplicate");
        rejects("{}", "w_geometry");
        std::string extra = good;
        extra.insert(extra.size() - 1, ", \"extra\": 1.0");
        rejects(extra, "extra");
        std::string missing = good;
        missing.erase(missing.find(", \"footprint_px\""), std::string(", \"footprint_px\": 0").size());
        rejects(missing, "footprint_px");
    }
    SECTION("types") {
        rejects(mutated("\"is_linear\": false", "\"is_linear\": 1"), "is_linear");
        rejects(mutated("\"is_linear\": false", "\"is_linear\": \"true\""), "is_linear");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": true"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": \"0.0\""), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": null"), "w_dust");
        rejects(mutated("\"capture_mode\": \"\"", "\"capture_mode\": 3"), "capture_mode");
        rejects(mutated("\"footprint_px\": 0", "\"footprint_px\": 1.5"), "footprint_px");
        rejects(mutated("\"footprint_px\": 0", "\"footprint_px\": 1e3"), "footprint_px");
        rejects(mutated("\"footprint_px\": 0", "\"footprint_px\": 9223372036854775808"), "footprint_px");
    }
    SECTION("numbers") {
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": NaN"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": Infinity"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": -Infinity"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": 1e999"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": 1.2.3"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": 0x10"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": +1.0"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": .5"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": 01.0"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": 1e"), "w_dust");
        rejects(mutated("\"w_dust\": 0.0", "\"w_dust\": "), "w_dust");
    }
    SECTION("strings") {
        rejects(mutated("\"capture_mode\": \"\"", "\"capture_mode\": \"abc"), "capture_mode");
        rejects(mutated("\"capture_mode\": \"\"", "\"capture_mode\": \"a\\qb\""), "capture_mode");
        rejects(mutated("\"capture_mode\": \"\"", "\"capture_mode\": \"a\\/b\""), "capture_mode");
        rejects(mutated("\"capture_mode\": \"\"", "\"capture_mode\": \"a\\u0100b\""), "capture_mode");
        rejects(mutated("\"capture_mode\": \"\"", "\"capture_mode\": \"a\\u00zzb\""), "capture_mode");
        rejects(mutated("\"capture_mode\": \"\"", "\"capture_mode\": \"a\nb\""), "capture_mode");
        rejects(mutated("\"capture_mode\": \"\"", "\"capture_mode\": \"a\\u00\""), "capture_mode");
    }
    SECTION("structure") {
        rejects("", "object");
        rejects("[]", "object");
        rejects(good + " x", "trailing");
        rejects(good + "}", "trailing");
        rejects(good.substr(0, good.size() - 1), "end");
        rejects(mutated(", \"w_emission\"", " \"w_emission\""), "','");  // missing comma
        std::string trailing_comma = good;
        trailing_comma.insert(trailing_comma.size() - 1, ",");
        rejects(trailing_comma, "key");
        rejects(mutated("\"w_geometry\": ", "\"w_geometry\" "), "w_geometry");  // missing colon
        rejects(mutated("{\"w_geometry\"", "{w_geometry"), "key");
    }
    SECTION("whitespace between tokens is allowed, the values are unchanged") {
        const CavityMeta m = blister_meta();
        std::string j = cavity_meta_json(m);
        for (size_t at = j.find(", "); at != std::string::npos; at = j.find(", ", at + 5)) j.replace(at, 2, " ,\n\t");
        check_identical(cavity_meta_from_json(" \n" + j + " \r\n"), m);
    }
}
