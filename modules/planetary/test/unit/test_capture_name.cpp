#include <catch_amalgamated.hpp>

#include "CaptureName.h"

using namespace planetary::ser;

namespace {
void Expect(const char* name, Target t, const char* filter, FilterClass fc) {
    const CaptureName c = ParseCaptureName(name);
    INFO(name);
    CHECK(c.parsed);
    CHECK(c.target == t);
    CHECK(c.filterToken == filter);
    CHECK(c.filter == fc);
}
FileInfo Info(ColorId id, int planes) {
    FileInfo i;
    i.colorId = id;
    i.planes = planes;
    return i;
}
}  // namespace

TEST_CASE("ParseCaptureName reads ASIAIR names", "[capture_name]") {
    Expect("2026-10-23-203401-Moon-Bin1 Lqef 64.6F.avi", Target::Moon, "Lqef", FilterClass::Broadband);
    Expect("2024-02-01-013058-Moon-Bin1 HaO3 16.2F.avi", Target::Moon, "HaO3", FilterClass::Narrowband);
    Expect("2023-08-30-220451-Moon-Bin1 LPro 14.0F.avi", Target::Moon, "LPro", FilterClass::Broadband);
    Expect("2026-01-30-193142-Moon-Bin1 Lqef -4.0F_pipp.ser", Target::Moon, "Lqef", FilterClass::Broadband);
    Expect("2026-01-30-195137-Jupiter-Bin1 Lqef -4.2F.avi", Target::Jupiter, "Lqef", FilterClass::Broadband);
    Expect("2026-01-30-195137-Saturn-Bin1 Lqef -4.2F.avi", Target::Saturn, "Lqef", FilterClass::Broadband);
    Expect("2026-01-30-195137-Neptune-Bin1 Lqef -4.2F.avi", Target::Neptune, "Lqef", FilterClass::Broadband);
    Expect("2026-01-30-195137-Mars-Bin1 Lqef -4.2F.avi", Target::Mars, "Lqef", FilterClass::Broadband);
    Expect("2026-01-30-195137-moon-Bin1 L-eXtreme 4.2F.avi", Target::Moon, "L-eXtreme", FilterClass::Narrowband);
    Expect("2026-01-30-195137-Moon-Bin1 Weird 4.2F.avi", Target::Moon, "Weird", FilterClass::Unknown);
    Expect("/mnt/qnap/astro/2026-01-30-195137-Moon-Bin1 Lqef -4.2F.avi", Target::Moon, "Lqef",
           FilterClass::Broadband);
}

TEST_CASE("ParseCaptureName: unknown target is Other, still parsed", "[capture_name]") {
    const CaptureName c = ParseCaptureName("2026-02-10-215957-Betelgeuse-Bin1 Lqef -4.0F.avi");
    CHECK(c.parsed);
    CHECK(c.target == Target::Other);
    CHECK(c.targetToken == "Betelgeuse");
}

TEST_CASE("ParseCaptureName: non-matching names", "[capture_name]") {
    for (const char* n : {"moon_stack_final.ser", ""}) {
        const CaptureName c = ParseCaptureName(n);
        CHECK_FALSE(c.parsed);
        CHECK(c.target == Target::Other);
    }
}

TEST_CASE("TargetName", "[capture_name]") {
    CHECK(std::string(TargetName(Target::Moon)) == "Moon");
    CHECK(std::string(TargetName(Target::Jupiter)) == "Jupiter");
    CHECK(std::string(TargetName(Target::Other)) == "Other");
}

TEST_CASE("MineralMoonRefusal", "[capture_name]") {
    const FileInfo bayer = Info(ColorId::BayerRGGB, 1);
    CHECK_FALSE(MineralMoonRefusal(Target::Moon, FilterClass::Broadband, bayer, "Lqef").has_value());
    CHECK_FALSE(MineralMoonRefusal(Target::Moon, FilterClass::Unknown, bayer, "").has_value());
    CHECK_FALSE(MineralMoonRefusal(Target::Moon, FilterClass::Broadband, Info(ColorId::RGB, 3), "Lqef").has_value());
    CHECK(*MineralMoonRefusal(Target::Moon, FilterClass::Narrowband, bayer, "HaO3") ==
          "Mineral moon needs a broadband filter; HaO3 is narrowband, so its colours come from the filter.");
    // The token as the file name writes it.
    CHECK(*MineralMoonRefusal(Target::Moon, FilterClass::Narrowband, bayer, "hao3") ==
          "Mineral moon needs a broadband filter; hao3 is narrowband, so its colours come from the filter.");
    CHECK(*MineralMoonRefusal(Target::Saturn, FilterClass::Broadband, bayer, "Lqef") ==
          "Mineral moon is for the Moon; the target is Saturn.");
    CHECK(*MineralMoonRefusal(Target::Moon, FilterClass::Broadband, Info(ColorId::Mono, 1), "Lqef") ==
          "Mineral moon needs a colour capture; this one is mono (set the Bayer pattern if it is a colour camera).");
}

TEST_CASE("ParseCaptureName: IR-pass filters are a class of their own, and Mineral moon refuses them (final review B, m4)",
          "[capture_name]") {
    // Lunar imagers use IR-pass filters against seeing; a colour camera behind one records the Bayer dyes' infrared
    // response, near-monochrome, which the colour step would amplify into false colour.
    for (const char* f : {"IR685", "IR850", "IR642", "IR742", "IR807", "ir850", "IR-685", "IR_850", "IR685nm", "IRPass",
                          "IR-Pass", "IRPass685", "PP642", "ProPlanet742", "ProPlanet-807", "IR1000"})
        Expect(("2026-10-23-203401-Moon-Bin1 " + std::string(f) + " 64.6F.avi").c_str(), Target::Moon, f, FilterClass::IrPass);
    // IR-cut filters stay broadband; a methane band is narrowband; "IR" alone (cut or pass?) and a wavelength outside
    // the near infrared are not named.
    for (const char* f : {"UVIR", "UV/IR", "IRCut"})
        Expect(("2026-10-23-203401-Moon-Bin1 " + std::string(f) + " 64.6F.avi").c_str(), Target::Moon, f, FilterClass::Broadband);
    for (const char* f : {"CH4", "Methane"})
        Expect(("2026-10-23-203401-Moon-Bin1 " + std::string(f) + " 64.6F.avi").c_str(), Target::Moon, f, FilterClass::Narrowband);
    for (const char* f : {"IR", "IR450", "IR85", "PP", "IRX685"})
        Expect(("2026-10-23-203401-Moon-Bin1 " + std::string(f) + " 64.6F.avi").c_str(), Target::Moon, f, FilterClass::Unknown);
    const FileInfo bayer = Info(ColorId::BayerRGGB, 1);
    CHECK(*MineralMoonRefusal(Target::Moon, FilterClass::IrPass, bayer, "IR685") ==
          "Mineral moon needs a broadband filter; IR685 is IR-pass, so its colours come from the filter.");
    CHECK(*MineralMoonRefusal(Target::Moon, FilterClass::IrPass, bayer, "") ==
          "Mineral moon needs a broadband filter; the filter is IR-pass, so its colours come from the filter.");
}

TEST_CASE("ParseCaptureName: a filter holding a separator, Windows paths, any case (B1 review m3)", "[capture_name]") {
    // UV/IR holds a '/': in a full path the base name still starts at the timestamp.
    Expect("/mnt/qnap/astro_data/2026-10-23/2026-10-23-203401-Moon-Bin1 UV/IR 5.0F.avi", Target::Moon, "UV/IR",
           FilterClass::Broadband);
    Expect("C:\\Users\\scott\\Pictures\\ASIAIR\\2026-10-23-203401-Moon-Bin1 Lqef 64.6F.avi", Target::Moon, "Lqef",
           FilterClass::Broadband);
    Expect("D:\\captures\\2026-10-23-203401-Moon-Bin1 UV/IR 5.0F.ser", Target::Moon, "UV/IR", FilterClass::Broadband);
    // Filter tokens match whatever their case; the token is kept as written.
    Expect("2026-10-23-203401-Moon-Bin1 hao3 16.2F.avi", Target::Moon, "hao3", FilterClass::Narrowband);
    Expect("2026-10-23-203401-MOON-Bin1 LQEF 64.6F.avi", Target::Moon, "LQEF", FilterClass::Broadband);
    Expect("2026-10-23-203401-Moon-Bin1 uv/ir 5.0F.avi", Target::Moon, "uv/ir", FilterClass::Broadband);
}
