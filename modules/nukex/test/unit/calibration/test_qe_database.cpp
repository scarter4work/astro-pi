#include "catch_amalgamated.hpp"
#include "nukex/calibration/qe_database.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

static fs::path fixture(const char* name) {
    return fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / name;
}

TEST_CASE("QEDatabase: load shipped DB, lookup camera QE", "[qe_database]") {
    QEDatabase db;
    auto result = db.load_shipped(fixture("minimal_db.json").string());
    REQUIRE(result.ok);
    REQUIRE(db.has_camera("ASI585MC"));
    REQUIRE(db.has_camera("ASI2600MC"));
    REQUIRE_FALSE(db.has_camera("DoesNotExist"));

    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R) == Catch::Approx(0.73).margin(0.01));
    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::G) == Catch::Approx(0.32).margin(0.01));
    REQUIRE(db.lookup_camera_qe("ASI585MC", 500.7, Photosite::B) == Catch::Approx(0.50).margin(0.02));
}

TEST_CASE("QEDatabase: filter passband lookup", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.has_filter("HaO3"));
    auto bw = db.lookup_filter("HaO3");
    REQUIRE(bw.lines.size() == 2);
    REQUIRE(bw.lines[0].name == "Ha");
    REQUIRE(bw.lines[0].wavelength_nm == Catch::Approx(656.3));
    REQUIRE(bw.lines[1].name == "OIII");
    REQUIRE(bw.lines[1].wavelength_nm == Catch::Approx(500.7));
}

TEST_CASE("QEDatabase: override merge, override wins on collision", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);

    // Pre-override
    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R) == Catch::Approx(0.73).margin(0.01));

    auto ov = db.load_override(fixture("override.json").string());
    REQUIRE(ov.ok);

    // Override wins on collision
    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R) == Catch::Approx(0.99).margin(0.01));

    // New camera from override is added
    REQUIRE(db.has_camera("Custom_Cam_1"));
    REQUIRE(db.lookup_camera_qe("Custom_Cam_1", 656.3, Photosite::R) == Catch::Approx(0.50).margin(0.01));
}

TEST_CASE("QEDatabase: missing shipped DB → loud fail", "[qe_database]") {
    QEDatabase db;
    auto r = db.load_shipped("/nonexistent/path/to/qe.json");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("missing") != std::string::npos);
}

TEST_CASE("QEDatabase: malformed override → loud fail with line/col", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    auto r = db.load_override(fixture("malformed.json").string());
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("malformed") != std::string::npos);
    // nlohmann json reports a byte offset, surfaced as "at line N, col M"
    REQUIRE(r.error.find("line") != std::string::npos);
}

TEST_CASE("QEDatabase: confidence enum reads from JSON", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.confidence("ASI585MC") == QEConfidence::HIGH);
    REQUIRE(db.confidence("Unknown")  == QEConfidence::UNKNOWN);
}

TEST_CASE("QEDatabase: camera keys normalise (case, punctuation)", "[qe_database]") {
    REQUIRE(QEDatabase::normalize_camera_key("ZWO ASI2400MC Pro") == "zwoasi2400mcpro");
    REQUIRE(QEDatabase::normalize_camera_key("asi_585-MC")        == "asi585mc");
}

TEST_CASE("QEDatabase: lookups are case-insensitive", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.has_camera("asi585mc"));
    REQUIRE(db.has_camera("ASI585MC"));
    REQUIRE(db.lookup_camera_qe("asi585mc", 656.3, Photosite::R) ==
            db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R));
}

TEST_CASE("QEDatabase: resolve_camera maps a real INSTRUME onto a DB key", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.resolve_camera("ASI585MC")            == "asi585mc");   // exact
    REQUIRE(db.resolve_camera("ZWO ASI2600MC Pro")   == "asi2600mc");  // key is a substring
    REQUIRE(db.resolve_camera("asi2600mc-pro")       == "asi2600mc");
    REQUIRE(db.resolve_camera("ATR585M")             == "");           // no key contained
    REQUIRE(db.resolve_camera("ASI585")              == "");           // partial key does not count
    REQUIRE(db.resolve_camera("")                    == "");
}

TEST_CASE("QEDatabase: resolve_camera prefers the longest contained key", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.load_override(fixture("override_camera_pro.json").string()).ok);
    REQUIRE(db.resolve_camera("ZWO ASI2600MC Pro") == "asi2600mcpro");
    REQUIRE(db.resolve_camera("ZWO ASI2600MC")     == "asi2600mc");
}

TEST_CASE("QEDatabase: resolve_camera breaks equal-length ties lexicographically", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    // "ZWO-ABD1" -> "zwoabd1" and "ZWO-ABC1" -> "zwoabc1" are both 7 chars
    // and both contained in the instrume below; the lexicographically
    // smaller key ("zwoabc1") must win regardless of map iteration order.
    REQUIRE(db.load_override(fixture("override_equal_length.json").string()).ok);
    REQUIRE(db.resolve_camera("ZWO ABC1 ZWO ABD1") == "zwoabc1");
}

TEST_CASE("QEDatabase: resolve_camera falls back to sensor + mono/OSC for a rebadged camera", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.load_override(fixture("override_sensor_rebadge.json").string()).ok);

    // ATR585M is an IMX585 *mono* camera whose product name contains no DB
    // key, so both product-name tiers miss. The sensor number and the mono
    // marker still identify the sensor exactly, and its QE curve is already
    // in the DB under another vendor's product name.
    REQUIRE(db.resolve_camera("ATR585M")            == "asi585mm");
    REQUIRE(db.resolve_camera("Player One 585MM")   == "asi585mm");

    // The same sensor rebadged as a colour camera must land on the OSC row,
    // not the mono one.
    REQUIRE(db.resolve_camera("Foo 585MC")          == "asi585mc");

    // A camera that names the SENSOR directly (as Altair and QHY do)
    // resolves even though the product names share nothing at all.
    REQUIRE(db.resolve_camera("Altair IMX571M")     == "asi2600mm");

    // ...and so does one that names the ZWO PRODUCT number. That "2600
    // means IMX571" is not derivable from the digits -- it is a fact the
    // database already stores, and resolution reads it rather than
    // guessing. Getting this from a rule over the number alone is not
    // merely hard, it is impossible: ASI294MC is an IMX294 while ASI294MM
    // is an IMX492, so the same number denotes different silicon on each
    // side of the mono/colour split.
    REQUIRE(db.resolve_camera("SomeCam-2600M Pro")  == "asi2600mm");

    // Vendor noise after the marker does not defeat the match.
    REQUIRE(db.resolve_camera("Rising 585M Pro")    == "asi585mm");
}

TEST_CASE("QEDatabase: sensor fallback refuses ambiguous or unknown sensors", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.load_override(fixture("override_sensor_rebadge.json").string()).ok);

    // No mono/OSC marker after the digits: IMX585 has both a mono and an OSC
    // row, so guessing would be a coin flip. Stay unresolved and let the
    // caller decide between failing loud and the generic fallback.
    REQUIRE(db.resolve_camera("ASI585")   == "");

    // Sensor number matches nothing in the DB.
    REQUIRE(db.resolve_camera("XYZ999M")  == "");

    // The generic fallback row must never be reachable by sensor matching --
    // it is a caller's explicit choice, not a resolution result.
    REQUIRE(db.resolve_camera("generic")  == "");
}

TEST_CASE("QEDatabase: colliding normalised camera keys within one document -> loud fail", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R) == Catch::Approx(0.73).margin(0.01));

    // "ASI-585MC" and "ASI585 MC" both normalise to "asi585mc" within the
    // SAME document — that is an ambiguous DB, not an override, so it must
    // fail loud rather than silently pick whichever the map visits last.
    auto r = db.load_override(fixture("collision.json").string());
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("ASI-585MC") != std::string::npos);
    REQUIRE(r.error.find("ASI585 MC") != std::string::npos);

    // The failed override must not have half-applied: the shipped value
    // for asi585mc is untouched.
    REQUIRE(db.has_camera("asi585mc"));
    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R) == Catch::Approx(0.73).margin(0.01));
}
