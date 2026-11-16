// Dark and bias calibration: master building, header matching, application.
//
// Synthetic frames are written as unsigned 16-bit FITS (BITPIX 16,
// BZERO 32768) -- the way cameras write them -- with a known bias pattern,
// known dark current, known hot pixels and a known sky signal, all integer
// ADU so every expected value is exact on the fixed 1/65535 scale.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/calibration_masters.hpp"
#include "nukex/stacker/light_calibration.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/fits_writer.hpp"

#include <fitsio.h>

#include <random>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

constexpr int   W = 64, H = 48;
constexpr float FS = 65535.0f;

struct Hdr {
    std::string imagetyp = "Light";
    std::string instrume = "ZWO ASI2400MC Pro";
    std::string bayer    = "RGGB";
    double exptime = 300.0;
    bool   has_exptime = true;
    double temp = -10.0;
    bool   has_temp = true;
    double gain = 0.0;
    bool   has_gain = true;
    double offset = 30.0;
    bool   has_offset = true;
    int    xbin = 1, ybin = 1;
};

/// Integer ADU per pixel -> unsigned 16-bit FITS, any size.
void write_u16_sized(const std::string& path, int w, int h_px, const std::function<int(int, int)>& adu, const Hdr& h) {
    std::vector<unsigned short> px(static_cast<std::size_t>(w) * h_px);
    for (int y = 0; y < h_px; ++y)
        for (int x = 0; x < w; ++x) {
            const int v = adu(x, y);
            if (v < 0 || v > 65535) throw std::runtime_error("synthetic ADU out of range");
            px[static_cast<std::size_t>(y) * w + x] = static_cast<unsigned short>(v);
        }
    fs::remove(path);
    fitsfile* fp = nullptr;
    int status = 0;
    fits_create_diskfile(&fp, path.c_str(), &status);
    long naxes[2] = {w, h_px};
    fits_create_img(fp, USHORT_IMG, 2, naxes, &status);
    fits_write_img(fp, TUSHORT, 1, static_cast<LONGLONG>(px.size()), px.data(), &status);
    fits_update_key_str(fp, "IMAGETYP", h.imagetyp.c_str(), nullptr, &status);
    if (!h.instrume.empty()) fits_update_key_str(fp, "INSTRUME", h.instrume.c_str(), nullptr, &status);
    if (!h.bayer.empty())    fits_update_key_str(fp, "BAYERPAT", h.bayer.c_str(), nullptr, &status);
    if (h.has_exptime) fits_update_key_dbl(fp, "EXPTIME", h.exptime, -10, nullptr, &status);
    if (h.has_temp)    fits_update_key_dbl(fp, "CCD-TEMP", h.temp, -10, nullptr, &status);
    if (h.has_gain)    fits_update_key_dbl(fp, "GAIN", h.gain, -10, nullptr, &status);
    if (h.has_offset)  fits_update_key_dbl(fp, "OFFSET", h.offset, -10, nullptr, &status);
    long xb = h.xbin, yb = h.ybin;
    fits_update_key_lng(fp, "XBINNING", xb, nullptr, &status);
    fits_update_key_lng(fp, "YBINNING", yb, nullptr, &status);
    fits_close_file(fp, &status);
    if (status != 0) throw std::runtime_error("write_u16 failed for " + path);
}

/// Integer ADU per pixel -> unsigned 16-bit FITS at the standard W x H.
void write_u16(const std::string& path, const std::function<int(int, int)>& adu, const Hdr& h) {
    write_u16_sized(path, W, H, adu, h);
}

// The synthetic sensor.
int bias_adu(int x, int y) { return 800 + (x % 7) + 2 * (y % 5); }
bool is_hot(int x, int y) { return (x == 10 && y == 7) || (x == 33 && y == 20) || (x == 51 && y == 40); }
/// Dark signal in ADU per 100 s: 1 everywhere, 500 on a hot pixel, and an
/// "amp glow" ramp of up to 40 at the right edge.
int dark_per_100s(int x, int y) { return 1 + (is_hot(x, y) ? 500 : 0) + (x >= 56 ? 5 * (x - 55) : 0); }
int dark_adu(int x, int y, double t) { return static_cast<int>(std::lround(dark_per_100s(x, y) * t / 100.0)); }
int sky_adu(int x, int y) { return 2000 + 3 * x + y; }
/// Vignetting, x1000 (integer so flats are exact): 600..1000.
int vignette_milli(int x, int y) {
    const double dx = (x - W / 2.0) / W, dy = (y - H / 2.0) / H;
    return static_cast<int>(1000 - 1600 * (dx * dx + dy * dy));
}
/// A symmetric, zero-sum per-frame noise pattern: frame f of 5 adds one of
/// {-2,-1,0,1,2}, permuted by pixel, so every pixel's five values are
/// symmetric about the truth.
int noise5(int f, int x, int y) { return ((f + x + 2 * y) % 5) - 2; }

struct TempDir {
    fs::path path;
    // A fresh directory under the system temp dir: random name (portable, no
    // pid), and create_directory() returning true proves it did not exist.
    TempDir() {
        std::random_device rd;
        for (;;) {
            path = fs::temp_directory_path() /
                ("nukex_darkbias_" + std::to_string(rd()) + "_" + std::to_string(rd()));
            if (fs::create_directory(path)) break;
        }
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    std::string file(const std::string& name) const { return (path / name).string(); }
};

MasterBuilder::Config cfg_in(const TempDir& d) {
    MasterBuilder::Config c;
    c.cache_dir = d.path.string();
    return c;
}

std::vector<std::string> write_set(const TempDir& d, const std::string& stem, int n, const Hdr& h,
                                   const std::function<int(int, int, int)>& adu) {
    std::vector<std::string> out;
    for (int f = 0; f < n; ++f) {
        const std::string p = d.file(stem + "_" + std::to_string(f) + ".fit");
        write_u16(p, [&](int x, int y) { return adu(f, x, y); }, h);
        out.push_back(p);
    }
    return out;
}

Hdr bias_hdr() { Hdr h; h.imagetyp = "Bias"; h.exptime = 0.001; return h; }
Hdr dark_hdr(double t) { Hdr h; h.imagetyp = "Dark"; h.exptime = t; return h; }

MasterFrame build_bias(const TempDir& d) {
    auto paths = write_set(d, "bias", 5, bias_hdr(), [](int f, int x, int y) { return bias_adu(x, y) + noise5(f, x, y); });
    auto r = MasterBuilder::build(MasterKind::BIAS, paths, cfg_in(d));
    REQUIRE(r.ok);
    return std::move(r.master);
}

MasterFrame build_dark(const TempDir& d, double t, const std::string& stem = "dark", Hdr h = Hdr{}) {
    if (h.imagetyp == "Light") h = dark_hdr(t);
    auto paths = write_set(d, stem, 5, h, [t](int f, int x, int y) { return bias_adu(x, y) + dark_adu(x, y, t) + noise5(f, x, y); });
    auto r = MasterBuilder::build(MasterKind::DARK, paths, cfg_in(d));
    INFO(r.error);
    REQUIRE(r.ok);
    return std::move(r.master);
}

CalibrationHeader light_header(const std::string& path) {
    const FrameMetadata m = FITSReader::read_headers(path);
    return CalibrationHeader::from_metadata(m, 1, FITSReader::full_scale_for_bitpix(m.equiv_bitpix));
}

std::string write_light(const TempDir& d, const std::string& name, const Hdr& h, bool vignetted = false) {
    const std::string p = d.file(name);
    write_u16(p, [&](int x, int y) {
        const int sky = vignetted ? sky_adu(x, y) * vignette_milli(x, y) / 1000 : sky_adu(x, y);
        return bias_adu(x, y) + dark_adu(x, y, h.exptime) + sky;
    }, h);
    return p;
}

} // namespace

// ── FITS plumbing ───────────────────────────────────────────────────

TEST_CASE("read_fixed_scale: 16-bit data on 1/65535, independent of the frame's maximum", "[darkbias][io]") {
    TempDir d;
    const std::string a = d.file("a.fit"), b = d.file("b.fit");
    write_u16(a, [](int, int) { return 1000; }, Hdr{});
    write_u16(b, [](int x, int) { return x == 0 ? 60000 : 1000; }, Hdr{});
    auto ra = FITSReader::read_fixed_scale(a), rb = FITSReader::read_fixed_scale(b);
    REQUIRE(ra.success); REQUIRE(rb.success);
    REQUIRE(ra.full_scale == 65535.0f);
    REQUIRE(ra.image.at(5, 5, 0) == rb.image.at(5, 5, 0));     // same ADU, same value
    REQUIRE(ra.image.at(5, 5, 0) == 1000.0f / FS);
    // read() divides by each frame's own maximum: the two disagree there.
    REQUIRE(FITSReader::read(a).image.at(5, 5, 0) != FITSReader::read(b).image.at(5, 5, 0));
}

TEST_CASE("FrameMetadata: raw calibration keywords with presence flags", "[darkbias][io]") {
    TempDir d;
    Hdr h; h.temp = 0.0; h.gain = 120; h.offset = 15; h.xbin = 2; h.ybin = 2;
    const std::string p = d.file("k.fit");
    write_u16(p, [](int, int) { return 1; }, h);
    const FrameMetadata m = FITSReader::read_headers(p);
    REQUIRE(m.has_ccd_temp);  REQUIRE(m.ccd_temp == 0.0f);      // 0 C is a reading, not "absent"
    REQUIRE(m.has_gain_setting); REQUIRE(m.gain_setting == 120.0f);
    REQUIRE(m.gain == 1.0f);                                     // the e-/ADU field is untouched
    REQUIRE(m.has_offset); REQUIRE(m.offset == 15.0f);
    REQUIRE(m.xbinning == 2); REQUIRE(m.ybinning == 2);
    REQUIRE(m.image_type == "Light");
    REQUIRE(m.equiv_bitpix == USHORT_IMG);
    Hdr n; n.has_temp = false; n.has_gain = false; n.has_offset = false; n.has_exptime = false;
    write_u16(p, [](int, int) { return 1; }, n);
    const FrameMetadata m2 = FITSReader::read_headers(p);
    REQUIRE_FALSE(m2.has_ccd_temp); REQUIRE_FALSE(m2.has_gain_setting);
    REQUIRE_FALSE(m2.has_offset);   REQUIRE_FALSE(m2.has_exposure);
}

// ── Masters ─────────────────────────────────────────────────────────

TEST_CASE("Master bias: the per-pixel fit recovers the bias pattern exactly", "[darkbias][master]") {
    TempDir d;
    const MasterFrame b = build_bias(d);
    REQUIRE(b.n_frames == 5);
    REQUIRE(b.header.full_scale == 65535.0);
    REQUIRE(b.estimator == "Huber");
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            REQUIRE(std::fabs(b.data.at(x, y, 0) * FS - bias_adu(x, y)) < 1e-3f);
}

TEST_CASE("Master bias: a cosmic ray in one frame does not reach the master, and nothing is rejected", "[darkbias][master]") {
    TempDir d;
    std::vector<int> values;
    auto paths = write_set(d, "bias", 7, bias_hdr(), [](int f, int x, int y) {
        const int n = ((f + x + 2 * y) % 7) - 3;             // symmetric, -3..3
        return bias_adu(x, y) + n + ((f == 2 && x == 20 && y == 20) ? 20000 : 0);
    });
    auto r = MasterBuilder::build(MasterKind::BIAS, paths, cfg_in(d));
    REQUIRE(r.ok);
    REQUIRE(r.master.n_frames == 7);
    const float got = r.master.data.at(20, 20, 0) * FS;
    // A mean would sit 20000/7 = 2857 ADU high; the Huber fit downweights the
    // hit to delta/|r| and stays within a fraction of the noise of the truth.
    REQUIRE(std::fabs(got - bias_adu(20, 20)) < 2.0f);
    REQUIRE(std::fabs(r.master.data.at(21, 20, 0) * FS - bias_adu(21, 20)) < 1e-3f);
}

TEST_CASE("Master dark: hot pixels and glow are kept (they are signal, not outliers)", "[darkbias][master]") {
    TempDir d;
    const MasterFrame dk = build_dark(d, 300.0);
    REQUIRE(dk.header.exposure == Catch::Approx(300.0));
    REQUIRE(dk.header.temperature == Catch::Approx(-10.0));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            REQUIRE(std::fabs(dk.data.at(x, y, 0) * FS - (bias_adu(x, y) + dark_adu(x, y, 300))) < 1e-3f);
    REQUIRE(dk.data.at(10, 7, 0) * FS == Catch::Approx(bias_adu(10, 7) + 1503).margin(1e-3));
}

TEST_CASE("Master build: the result does not depend on the order the files were given", "[darkbias][master]") {
    TempDir d;
    auto paths = write_set(d, "bias", 6, bias_hdr(), [](int f, int x, int y) { return bias_adu(x, y) + (f * 7 + x * 3 + y) % 11; });
    auto a = MasterBuilder::build(MasterKind::BIAS, paths, cfg_in(d));
    std::reverse(paths.begin(), paths.end());
    auto b = MasterBuilder::build(MasterKind::BIAS, paths, cfg_in(d));
    REQUIRE(a.ok); REQUIRE(b.ok);
    REQUIRE(std::memcmp(a.master.data.data(), b.master.data.data(), a.master.data.data_size() * sizeof(float)) == 0);
    REQUIRE(a.master.signature == b.master.signature);
}

TEST_CASE("Master build refuses inconsistent sets, naming the frame", "[darkbias][master][mismatch]") {
    TempDir d;
    SECTION("fewer than three frames") {
        auto p = write_set(d, "b", 2, bias_hdr(), [](int, int x, int y) { return bias_adu(x, y); });
        auto r = MasterBuilder::build(MasterKind::BIAS, p, cfg_in(d));
        REQUIRE_FALSE(r.ok);
        REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("at least 3"));
    }
    SECTION("a dark of a different exposure in a dark set") {
        auto p = write_set(d, "d", 4, dark_hdr(300), [](int, int x, int y) { return bias_adu(x, y); });
        write_u16(p[1], [](int x, int y) { return bias_adu(x, y); }, dark_hdr(120));
        auto r = MasterBuilder::build(MasterKind::DARK, p, cfg_in(d));
        REQUIRE_FALSE(r.ok);
        REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("d_1.fit"));
        REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("exposure 120"));
    }
    SECTION("a different gain") {
        auto p = write_set(d, "d", 4, dark_hdr(300), [](int, int x, int y) { return bias_adu(x, y); });
        Hdr g = dark_hdr(300); g.gain = 100;
        write_u16(p[3], [](int x, int y) { return bias_adu(x, y); }, g);
        auto r = MasterBuilder::build(MasterKind::DARK, p, cfg_in(d));
        REQUIRE_FALSE(r.ok);
        REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("gain 100 vs 0"));
    }
    SECTION("temperatures spread beyond one master") {
        auto p = write_set(d, "d", 4, dark_hdr(300), [](int, int x, int y) { return bias_adu(x, y); });
        Hdr t = dark_hdr(300); t.temp = -2.0;
        write_u16(p[2], [](int x, int y) { return bias_adu(x, y); }, t);
        auto r = MasterBuilder::build(MasterKind::DARK, p, cfg_in(d));
        REQUIRE_FALSE(r.ok);
        REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("temperatures span -10.0 to -2.0"));
    }
    SECTION("a light given as a bias") {
        auto p = write_set(d, "b", 3, bias_hdr(), [](int, int x, int y) { return bias_adu(x, y); });
        write_u16(p[0], [](int x, int y) { return bias_adu(x, y); }, Hdr{});
        auto r = MasterBuilder::build(MasterKind::BIAS, p, cfg_in(d));
        REQUIRE_FALSE(r.ok);
        REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("IMAGETYP='Light'"));
    }
    SECTION("a master flat with no pedestal") {
        auto p = write_set(d, "f", 3, Hdr{}, [](int, int x, int y) { return bias_adu(x, y); });
        auto r = MasterBuilder::build(MasterKind::FLAT, p, cfg_in(d));
        REQUIRE_FALSE(r.ok);
        REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("pedestal"));
    }
}

TEST_CASE("Master files: round trip and provenance-checked cache", "[darkbias][master][io]") {
    TempDir d;
    auto paths = write_set(d, "dark", 4, dark_hdr(300), [](int f, int x, int y) { return bias_adu(x, y) + dark_adu(x, y, 300) + f; });
    const std::string cache = d.file("master_dark.fit");
    auto a = MasterBuilder::build_or_load(MasterKind::DARK, paths, cache, cfg_in(d));
    REQUIRE(a.ok); REQUIRE_FALSE(a.reused);
    auto r = MasterBuilder::read(cache);
    REQUIRE(r.ok);
    REQUIRE(r.master.kind == MasterKind::DARK);
    REQUIRE(r.master.n_frames == 4);
    REQUIRE(r.master.signature == a.master.signature);
    REQUIRE(r.master.header.exposure == Catch::Approx(300.0));
    REQUIRE(r.master.header.temperature == Catch::Approx(-10.0));
    REQUIRE(r.master.header.gain == 0.0); REQUIRE(r.master.header.has_gain);
    REQUIRE(r.master.header.offset == 30.0);
    REQUIRE(r.master.header.instrument == "ZWO ASI2400MC Pro");
    REQUIRE(r.master.header.bayer_pattern == "RGGB");
    REQUIRE(r.master.header.full_scale == 65535.0);
    REQUIRE(r.master.sources.size() == 4);
    REQUIRE(std::memcmp(r.master.data.data(), a.master.data.data(), a.master.data.data_size() * sizeof(float)) == 0);
    auto keys = FITSReader::read_keywords(cache);
    REQUIRE(keys["IMAGETYP"] == "Master Dark");
    REQUIRE(keys["NCOMBINE"] == "4");

    auto b = MasterBuilder::build_or_load(MasterKind::DARK, paths, cache, cfg_in(d));
    REQUIRE(b.ok); REQUIRE(b.reused);

    // Change one input: the cache must not be trusted.
    // Same size; a modification time moved explicitly, so the test does not
    // depend on the filesystem's timestamp resolution.
    const auto before = fs::last_write_time(paths[0]);
    write_u16(paths[0], [](int x, int y) { return bias_adu(x, y) + dark_adu(x, y, 300) + 9; }, dark_hdr(300));
    fs::last_write_time(paths[0], before + std::chrono::seconds(10));
    auto c = MasterBuilder::build_or_load(MasterKind::DARK, paths, cache, cfg_in(d));
    REQUIRE(c.ok); REQUIRE_FALSE(c.reused);
    REQUIRE(c.master.signature != a.master.signature);

    // A file that is not a master is refused, not misread.
    REQUIRE_FALSE(MasterBuilder::read(paths[1]).ok);
}

// ── Matching ────────────────────────────────────────────────────────

TEST_CASE("Matching: same exposure and temperature -> the dark is subtracted as it is", "[darkbias][match]") {
    TempDir d;
    std::vector<MasterFrame> biases{build_bias(d)};
    std::vector<MasterFrame> darks{build_dark(d, 300.0)};
    const auto lh = light_header(write_light(d, "l.fit", Hdr{}));
    auto pr = CalibrationMatcher::plan(lh, biases, darks, MatchTolerance{}, true, true);
    REQUIRE(pr.ok);
    REQUIRE(pr.plan.dark == &darks[0]);
    REQUIRE_FALSE(pr.plan.dark_scaled);
    REQUIRE(pr.plan.bias == nullptr);     // an unscaled dark carries the bias
    REQUIRE(pr.plan.dark_scale == 1.0);
}

TEST_CASE("Matching: picks the dark needing the least correction", "[darkbias][match]") {
    TempDir d;
    std::vector<MasterFrame> biases{build_bias(d)};
    std::vector<MasterFrame> darks;
    darks.push_back(build_dark(d, 120.0, "d120"));
    darks.push_back(build_dark(d, 600.0, "d600"));
    darks.push_back(build_dark(d, 300.0, "d300"));
    const auto lh = light_header(write_light(d, "l.fit", Hdr{}));
    auto pr = CalibrationMatcher::plan(lh, biases, darks, MatchTolerance{}, true, true);
    REQUIRE(pr.ok);
    REQUIRE(pr.plan.dark == &darks[2]);                     // exact exposure first
    darks.pop_back();
    pr = CalibrationMatcher::plan(lh, biases, darks, MatchTolerance{}, true, true);
    REQUIRE(pr.ok);
    REQUIRE(pr.plan.dark == &darks[1]);                     // |ln(300/600)| < |ln(300/120)|
    REQUIRE(pr.plan.dark_scaled);
    REQUIRE(pr.plan.dark_scale == Catch::Approx(0.5));
    REQUIRE(pr.plan.bias == &biases[0]);
}

TEST_CASE("Matching: mismatches are loud and say why", "[darkbias][match][mismatch]") {
    TempDir d;
    std::vector<MasterFrame> biases{build_bias(d)};
    std::vector<MasterFrame> darks{build_dark(d, 300.0)};
    std::vector<MasterFrame> none;
    auto plan_for = [&](const Hdr& h, const std::vector<MasterFrame>& b) {
        return CalibrationMatcher::plan(light_header(write_light(d, "l.fit", h)), b, darks, MatchTolerance{}, true, !b.empty());
    };
    SECTION("wrong exposure and no bias to scale with") {
        Hdr h; h.exptime = 180;
        auto pr = plan_for(h, none);
        REQUIRE_FALSE(pr.ok);
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("No master dark matches"));
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("needs a compatible master bias"));
    }
    SECTION("wrong exposure with scaling switched off") {
        Hdr h; h.exptime = 180;
        MatchTolerance t; t.allow_exposure_scaling = false;
        auto pr = CalibrationMatcher::plan(light_header(write_light(d, "l.fit", h)), biases, darks, t, true, true);
        REQUIRE_FALSE(pr.ok);
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("exposure scaling is off"));
    }
    SECTION("temperature beyond tolerance") {
        Hdr h; h.temp = -4.0;
        auto pr = plan_for(h, biases);
        REQUIRE_FALSE(pr.ok);
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("6.0 C from the frame's -4.0 C"));
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("temperature scaling is off"));
    }
    SECTION("temperature just inside tolerance is accepted unscaled") {
        Hdr h; h.temp = -8.1;
        auto pr = plan_for(h, biases);
        REQUIRE(pr.ok);
        REQUIRE_FALSE(pr.plan.dark_scaled);
    }
    SECTION("gain") {
        Hdr h; h.gain = 100;
        auto pr = plan_for(h, biases);
        REQUIRE_FALSE(pr.ok);
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("No master bias matches"));
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("gain 100 vs 0"));
        auto dark_only = plan_for(h, none);
        REQUIRE_FALSE(dark_only.ok);
        REQUIRE_THAT(dark_only.error, Catch::Matchers::ContainsSubstring("gain 100 vs 0"));
    }
    SECTION("offset") {
        Hdr h; h.offset = 50;
        auto pr = plan_for(h, none);
        REQUIRE_FALSE(pr.ok);
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("offset 50 vs 30"));
    }
    SECTION("camera") {
        Hdr h; h.instrume = "ZWO ASI585MC Pro";
        auto pr = plan_for(h, none);
        REQUIRE_FALSE(pr.ok);
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("camera 'ZWO ASI585MC Pro' vs 'ZWO ASI2400MC Pro'"));
    }
    SECTION("binning") {
        Hdr h; h.xbin = 2; h.ybin = 2;
        auto pr = plan_for(h, none);
        REQUIRE_FALSE(pr.ok);
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("binning 2x2 vs 1x1"));
    }
    SECTION("a keyword in only one header cannot be checked") {
        Hdr h; h.has_gain = false;
        auto pr = plan_for(h, none);
        REQUIRE_FALSE(pr.ok);
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("gain is in only one header"));
    }
    SECTION("no exposure on the light") {
        Hdr h; h.has_exptime = false;
        auto pr = plan_for(h, none);
        REQUIRE_FALSE(pr.ok);
        REQUIRE_THAT(pr.error, Catch::Matchers::ContainsSubstring("exposure is not recorded"));
    }
}

TEST_CASE("Matching: dark-current temperature scaling only when enabled, within its limit", "[darkbias][match]") {
    TempDir d;
    std::vector<MasterFrame> biases{build_bias(d)};
    std::vector<MasterFrame> darks{build_dark(d, 300.0)};
    Hdr h; h.temp = -4.0;
    const auto lh = light_header(write_light(d, "l.fit", h));
    MatchTolerance t; t.dark_doubling_temperature_c = 6.0;
    auto pr = CalibrationMatcher::plan(lh, biases, darks, t, true, true);
    REQUIRE(pr.ok);
    REQUIRE(pr.plan.dark_scaled);
    REQUIRE(pr.plan.temperature_factor == Catch::Approx(2.0));        // +6 C = one doubling
    REQUIRE(pr.plan.dark_scale == Catch::Approx(2.0));
    REQUIRE(pr.plan.bias == &biases[0]);
    t.temperature_scaling_limit_c = 5.0;
    auto beyond = CalibrationMatcher::plan(lh, biases, darks, t, true, true);
    REQUIRE_FALSE(beyond.ok);
    REQUIRE_THAT(beyond.error, Catch::Matchers::ContainsSubstring("scaling limit"));
}

// ── Application ─────────────────────────────────────────────────────

namespace {
void require_sky_recovered(const Image& img) {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            INFO("pixel " << x << "," << y);
            REQUIRE(std::fabs(img.at(x, y, 0) * FS - sky_adu(x, y)) < 2e-3f);
        }
}
}

TEST_CASE("Calibration: unscaled dark recovers the sky exactly, hot pixels and glow gone", "[darkbias][apply]") {
    TempDir d;
    std::vector<MasterFrame> biases{build_bias(d)};
    std::vector<MasterFrame> darks{build_dark(d, 300.0)};
    const std::string lp = write_light(d, "l.fit", Hdr{});
    auto pr = CalibrationMatcher::plan(light_header(lp), biases, darks, MatchTolerance{}, true, true);
    REQUIRE(pr.ok);
    auto rr = FITSReader::read_fixed_scale(lp);
    REQUIRE(rr.image.at(10, 7, 0) * FS == Catch::Approx(bias_adu(10, 7) + 1503 + sky_adu(10, 7)));
    REQUIRE(LightCalibrator::apply(rr.image, rr.full_scale, pr.plan, nullptr).empty());
    require_sky_recovered(rr.image);
}

TEST_CASE("Calibration: a dark of another exposure is scaled after the bias comes off", "[darkbias][apply]") {
    TempDir d;
    std::vector<MasterFrame> biases{build_bias(d)};
    std::vector<MasterFrame> darks{build_dark(d, 100.0)};
    const std::string lp = write_light(d, "l.fit", Hdr{});
    auto pr = CalibrationMatcher::plan(light_header(lp), biases, darks, MatchTolerance{}, true, true);
    REQUIRE(pr.ok);
    REQUIRE(pr.plan.dark_scaled);
    REQUIRE(pr.plan.dark_scale == Catch::Approx(3.0));
    auto rr = FITSReader::read_fixed_scale(lp);
    REQUIRE(LightCalibrator::apply(rr.image, rr.full_scale, pr.plan, nullptr).empty());
    require_sky_recovered(rr.image);
}

TEST_CASE("Calibration: bias only removes the pedestal; a held-out bias calibrates to zero", "[darkbias][apply]") {
    TempDir d;
    std::vector<MasterFrame> biases{build_bias(d)};
    const std::string p = d.file("held_out_bias.fit");
    write_u16(p, [](int x, int y) { return bias_adu(x, y) + noise5(3, x, y); }, bias_hdr());
    auto pr = CalibrationMatcher::plan(light_header(p), biases, {}, MatchTolerance{}, false, true);
    REQUIRE(pr.ok);
    REQUIRE(pr.plan.bias == &biases[0]);
    REQUIRE(pr.plan.dark == nullptr);
    auto rr = FITSReader::read_fixed_scale(p);
    REQUIRE(LightCalibrator::apply(rr.image, rr.full_scale, pr.plan, nullptr).empty());
    double sum = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            REQUIRE(std::fabs(rr.image.at(x, y, 0) * FS - noise5(3, x, y)) < 1e-3f);
            sum += rr.image.at(x, y, 0) * FS;
        }
    REQUIRE(std::fabs(sum / (W * H)) < 0.05);      // pedestal of ~800 ADU gone
}

TEST_CASE("Calibration: flats calibrated with the bias divide out vignetting", "[darkbias][apply]") {
    TempDir d;
    MasterFrame bias = build_bias(d);
    Hdr fh; fh.imagetyp = "Flat"; fh.exptime = 0.3;
    auto flats = write_set(d, "flat", 5, fh, [](int f, int x, int y) {
        return bias_adu(x, y) + (20 + f) * vignette_milli(x, y);          // levels differ per flat
    });
    auto fr = MasterBuilder::build(MasterKind::FLAT, flats, cfg_in(d), &bias.data, "master bias", bias.signature);
    INFO(fr.error);
    REQUIRE(fr.ok);
    std::vector<MasterFrame> biases{std::move(bias)};
    std::vector<MasterFrame> darks{build_dark(d, 300.0)};
    const std::string lp = write_light(d, "l.fit", Hdr{}, /*vignetted=*/true);
    auto pr = CalibrationMatcher::plan(light_header(lp), biases, darks, MatchTolerance{}, true, true);
    REQUIRE(pr.ok);
    auto rr = FITSReader::read_fixed_scale(lp);
    REQUIRE(LightCalibrator::apply(rr.image, rr.full_scale, pr.plan, &fr.master).empty());
    // light/flat = sky * v / (v / level(v)) = sky * level(v): one constant.
    const double k = rr.image.at(W / 2, H / 2, 0) * FS / sky_adu(W / 2, H / 2);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            INFO("pixel " << x << "," << y);
            // The synthetic light holds T = floor(sky * v / 1000) ADU of sky,
            // so calibrated * v / 1000 / k must give T back exactly.
            const double t = sky_adu(x, y) * vignette_milli(x, y) / 1000;
            REQUIRE(std::fabs(rr.image.at(x, y, 0) * FS / k * vignette_milli(x, y) / 1000.0 - t) < 2e-3);
        }
}

TEST_CASE("Calibration: masters of another scale or geometry are refused", "[darkbias][apply][mismatch]") {
    TempDir d;
    std::vector<MasterFrame> biases{build_bias(d)};
    CalibrationPlan plan; plan.bias = &biases[0];
    Image wrong(W + 2, H, 1);
    REQUIRE_THAT(LightCalibrator::apply(wrong, 65535.0, plan, nullptr), Catch::Matchers::ContainsSubstring("frame is 66x48x1"));
    Image right(W, H, 1);
    REQUIRE_THAT(LightCalibrator::apply(right, 255.0, plan, nullptr), Catch::Matchers::ContainsSubstring("full scale"));
}

TEST_CASE("Calibration: an unscaled dark is never combined with a second bias subtraction", "[darkbias][apply]") {
    // The contract of CalibrationPlan, independent of how the plan was made:
    // an unscaled dark already holds the bias, so a plan that also names a
    // bias must still give L - D, not L - D - B.
    TempDir d;
    std::vector<MasterFrame> biases{build_bias(d)};
    std::vector<MasterFrame> darks{build_dark(d, 300.0)};
    CalibrationPlan plan;
    plan.bias = &biases[0];
    plan.dark = &darks[0];
    plan.dark_scaled = false;
    const std::string lp = write_light(d, "l.fit", Hdr{});
    auto rr = FITSReader::read_fixed_scale(lp);
    REQUIRE(LightCalibrator::apply(rr.image, rr.full_scale, plan, nullptr).empty());
    require_sky_recovered(rr.image);
    // The flats' pedestal from the same plan is the dark alone, too.
    const Image ped = LightCalibrator::pedestal(plan);
    REQUIRE(ped.at(10, 7, 0) == darks[0].data.at(10, 7, 0));
}

// ── Fitted dark scale ───────────────────────────────────────────────

namespace {

constexpr int BW = 256, BH = 192;

/// Hot-pixel and glow amplitude at 100 s (ADU): 2% of pixels hot (60..900),
/// plus a glow wedge at the right edge.
int structure_100s(int x, int y) {
    const unsigned h = static_cast<unsigned>(x) * 73856093u ^ static_cast<unsigned>(y) * 19349663u;
    int a = (h % 50u == 0u) ? 60 + static_cast<int>((h / 50u) % 841u) : 0;
    if (x >= BW - 24) a += 4 * (x - (BW - 24));
    return a;
}
/// Non-linear dark signal: the structure grows as t^0.8, the uniform floor
/// linearly. A 100 s dark scaled by the header ratio 3 to 300 s therefore
/// over-subtracts the structure by (3 - 3^0.8) = 0.59 of its 100 s value.
double dark_signal(int x, int y, double t, bool structured) {
    return 1.0 * t / 100.0 + (structured ? structure_100s(x, y) * std::pow(t / 100.0, 0.8) : 0.0);
}
int big_bias(int x, int y) { return 900 + (x % 5) + 3 * (y % 3); }

struct BigSet {
    MasterFrame bias, dark;
    std::string light;
};

BigSet big_set(const TempDir& d, bool structured, double light_t = 300.0) {
    BigSet out;
    std::vector<std::string> bp, dp;
    for (int f = 0; f < 5; ++f) {
        bp.push_back(d.file("bb_" + std::to_string(f) + ".fit"));
        write_u16_sized(bp.back(), BW, BH, [f](int x, int y) { return big_bias(x, y) + ((f + x + 2 * y) % 5) - 2; }, bias_hdr());
        dp.push_back(d.file("bd_" + std::to_string(f) + ".fit"));
        write_u16_sized(dp.back(), BW, BH, [&, f](int x, int y) {
            return big_bias(x, y) + static_cast<int>(std::lround(dark_signal(x, y, 100.0, structured))) + ((f + 2 * x + y) % 5) - 2;
        }, dark_hdr(100.0));
    }
    auto b = MasterBuilder::build(MasterKind::BIAS, bp, cfg_in(d));
    auto k = MasterBuilder::build(MasterKind::DARK, dp, cfg_in(d));
    REQUIRE(b.ok); REQUIRE(k.ok);
    out.bias = std::move(b.master);
    out.dark = std::move(k.master);
    // Light: smooth sky, stars, Gaussian read+shot noise.
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 6.0);
    Hdr lh; lh.exptime = light_t;
    out.light = d.file("big_light.fit");
    write_u16_sized(out.light, BW, BH, [&](int x, int y) {
        double sky = 2500 + 2.0 * x + 1.5 * y;
        for (int sidx = 0; sidx < 12; ++sidx) {
            const double sx = 20 + (sidx * 37) % (BW - 40), sy = 15 + (sidx * 53) % (BH - 30);
            const double r2 = (x - sx) * (x - sx) + (y - sy) * (y - sy);
            sky += 20000.0 * std::exp(-r2 / 4.0);
        }
        return static_cast<int>(std::lround(big_bias(x, y) + dark_signal(x, y, light_t, structured) + sky + noise(rng)));
    }, lh);
    return out;
}

double true_k() { return std::pow(3.0, 0.8); }   // 2.4082

} // namespace

TEST_CASE("Dark scale fit: non-linear dark current is fitted, not assumed", "[darkbias][fit]") {
    TempDir d;
    BigSet s = big_set(d, true);
    std::vector<MasterFrame> biases{std::move(s.bias)};
    std::vector<MasterFrame> darks{std::move(s.dark)};
    auto pr = CalibrationMatcher::plan(light_header(s.light), biases, darks, MatchTolerance{}, true, true);
    REQUIRE(pr.ok);
    REQUIRE(pr.plan.dark_scaled);
    REQUIRE(pr.plan.dark_scale == Catch::Approx(3.0));      // the header ratio: the seed only
    LightCalibrator::prepare(pr.plan);
    REQUIRE(pr.plan.structure);
    REQUIRE(pr.plan.structure->index.size() > 500);

    auto rr = FITSReader::read_fixed_scale(s.light);
    const Image raw = rr.image.clone();
    DarkScaleFit fit;
    REQUIRE(LightCalibrator::apply(rr.image, rr.full_scale, pr.plan, nullptr, 0.01f, &fit).empty());
    INFO("fitted " << fit.fitted << " +/- " << fit.standard_error << " (true " << true_k() << ")");
    REQUIRE(fit.well_conditioned);
    REQUIRE(fit.warning.empty());
    REQUIRE(fit.scale == fit.fitted);
    REQUIRE(std::fabs(fit.scale - true_k()) < 0.01 * true_k());
    REQUIRE(fit.standard_error < 0.01 * fit.scale);

    // Hot pixels: with the fitted scale their residual is noise; with the
    // header ratio they are over-subtracted by 0.59 x their 100 s value.
    std::vector<double> fitted_res, header_res;
    const float* bb = biases[0].data.data();
    const float* dd = darks[0].data.data();
    for (int y = 2; y < BH - 2; ++y)
        for (int x = 2; x < BW - 26; ++x) {
            const int a = structure_100s(x, y);
            if (a < 200) continue;
            const std::size_t i = static_cast<std::size_t>(y) * BW + x;
            // Compare with the pixel's own truth: sky + uniform floor residual.
            double sky = 2500 + 2.0 * x + 1.5 * y;
            for (int sidx = 0; sidx < 12; ++sidx) {
                const double sx = 20 + (sidx * 37) % (BW - 40), sy = 15 + (sidx * 53) % (BH - 30);
                sky += 20000.0 * std::exp(-((x - sx) * (x - sx) + (y - sy) * (y - sy)) / 4.0);
            }
            const double floor_res = 3.0 - fit.scale * 1.0;
            fitted_res.push_back(rr.image.data()[i] * FS - sky - floor_res);
            const double hdr = raw.data()[i] - bb[i] - 3.0f * (dd[i] - bb[i]);
            header_res.push_back(hdr * FS - sky - (3.0 - 3.0));
        }
    REQUIRE(fitted_res.size() > 100);
    std::sort(fitted_res.begin(), fitted_res.end());
    std::sort(header_res.begin(), header_res.end());
    const double fmed = fitted_res[fitted_res.size() / 2], hmed = header_res[header_res.size() / 2];
    INFO("hot-pixel residual median: fitted " << fmed << " ADU, header ratio " << hmed << " ADU");
    REQUIRE(std::fabs(fmed) < 3.0);          // within the light's 6 ADU noise
    REQUIRE(hmed < -100.0);                  // the header ratio over-subtracts
}

TEST_CASE("Dark scale fit: a linear dark gives back the header ratio", "[darkbias][fit]") {
    TempDir d;
    BigSet s = big_set(d, true);
    // A light whose structure really is 3 x the 100 s dark (linear).
    Hdr lh; lh.exptime = 300.0;
    const std::string lp = d.file("linear_light.fit");
    write_u16_sized(lp, BW, BH, [](int x, int y) {
        return big_bias(x, y) + 3 + 3 * structure_100s(x, y) + 3000;
    }, lh);
    std::vector<MasterFrame> biases{std::move(s.bias)};
    std::vector<MasterFrame> darks{std::move(s.dark)};
    auto pr = CalibrationMatcher::plan(light_header(lp), biases, darks, MatchTolerance{}, true, true);
    REQUIRE(pr.ok);
    auto rr = FITSReader::read_fixed_scale(lp);
    DarkScaleFit fit;
    REQUIRE(LightCalibrator::apply(rr.image, rr.full_scale, pr.plan, nullptr, 0.01f, &fit).empty());
    REQUIRE(fit.well_conditioned);
    // At 100 s, a * (t/100)^0.8 is a: the dark holds the structure exactly,
    // and this light holds exactly three times it.
    REQUIRE(fit.scale == Catch::Approx(3.0).epsilon(1e-4));
}

TEST_CASE("Dark scale fit: no structure to fit -> header ratio, loudly", "[darkbias][fit][mismatch]") {
    TempDir d;
    BigSet s = big_set(d, false);
    std::vector<MasterFrame> biases{std::move(s.bias)};
    std::vector<MasterFrame> darks{std::move(s.dark)};
    auto pr = CalibrationMatcher::plan(light_header(s.light), biases, darks, MatchTolerance{}, true, true);
    REQUIRE(pr.ok);
    auto rr = FITSReader::read_fixed_scale(s.light);
    DarkScaleFit fit;
    REQUIRE(LightCalibrator::apply(rr.image, rr.full_scale, pr.plan, nullptr, 0.01f, &fit).empty());
    REQUIRE_FALSE(fit.well_conditioned);
    REQUIRE(fit.scale == 3.0);
    REQUIRE_THAT(fit.warning, Catch::Matchers::ContainsSubstring("header ratio 3.0000 is used instead"));
}

TEST_CASE("Dark scale fit: a fit far from the seed is refused", "[darkbias][fit][mismatch]") {
    TempDir d;
    BigSet s = big_set(d, true);
    // A light whose "structure" is 20 x the dark's: no plausible exposure scaling.
    Hdr lh; lh.exptime = 300.0;
    const std::string lp = d.file("odd_light.fit");
    write_u16_sized(lp, BW, BH, [](int x, int y) {
        return std::min(65535, big_bias(x, y) + 20 * structure_100s(x, y) + 3000);
    }, lh);
    auto st = LightCalibrator::dark_structure(s.bias, s.dark);
    auto rr = FITSReader::read_fixed_scale(lp);
    const DarkScaleFit fit = LightCalibrator::fit_dark_scale(rr.image, s.bias, s.dark, *st, 3.0);
    REQUIRE_FALSE(fit.well_conditioned);
    REQUIRE(fit.scale == 3.0);
    REQUIRE(fit.fitted > 12.0);
    REQUIRE_THAT(fit.warning, Catch::Matchers::ContainsSubstring("not within a factor 4"));
}

TEST_CASE("Dark scale fit: refuses below its structure count and above its error bound", "[darkbias][fit][mismatch]") {
    TempDir d;
    BigSet s = big_set(d, true);
    auto st = LightCalibrator::dark_structure(s.bias, s.dark);
    auto rr = FITSReader::read_fixed_scale(s.light);
    const int n = static_cast<int>(st->index.size());
    const DarkScaleFit ok = LightCalibrator::fit_dark_scale(rr.image, s.bias, s.dark, *st, 3.0);
    REQUIRE(ok.well_conditioned);
    const DarkScaleFit few = LightCalibrator::fit_dark_scale(rr.image, s.bias, s.dark, *st, 3.0, n + 1);
    REQUIRE_FALSE(few.well_conditioned);
    REQUIRE(few.scale == 3.0);
    REQUIRE_THAT(few.warning, Catch::Matchers::ContainsSubstring("pixels of the dark carry structure"));
    const DarkScaleFit loose = LightCalibrator::fit_dark_scale(rr.image, s.bias, s.dark, *st, 3.0, 200,
                                                               0.1 * ok.standard_error / ok.fitted);
    REQUIRE_FALSE(loose.well_conditioned);
    REQUIRE(loose.scale == 3.0);
    REQUIRE(loose.fitted == Catch::Approx(ok.fitted));
    REQUIRE_THAT(loose.warning, Catch::Matchers::ContainsSubstring("poorly conditioned"));
}

TEST_CASE("Dark scale fit: on a CFA it compares each photosite with its own colour", "[darkbias][fit]") {
    // Red photosites carry 2 ADU per 100 s more dark signal than the rest (a
    // CFA-level pattern), and the light's sky is red-heavy. Measured against
    // ALL eight neighbours, every red photosite would look like structure and
    // carry the sky's colour into the fit; against its same-colour neighbours
    // only the hot pixels do.
    TempDir d;
    auto red = [](int x, int y) { return (x % 2 == 0) && (y % 2 == 0); };
    std::vector<std::string> bp, dp;
    for (int f = 0; f < 5; ++f) {
        bp.push_back(d.file("cb_" + std::to_string(f) + ".fit"));
        write_u16_sized(bp.back(), BW, BH, [f](int x, int y) { return big_bias(x, y) + ((f + x + 2 * y) % 5) - 2; }, bias_hdr());
        dp.push_back(d.file("cd_" + std::to_string(f) + ".fit"));
        write_u16_sized(dp.back(), BW, BH, [&, f](int x, int y) {
            return big_bias(x, y) + (red(x, y) ? 2 : 0) + static_cast<int>(std::lround(dark_signal(x, y, 100.0, true)))
                 + ((f + 2 * x + y) % 5) - 2;
        }, dark_hdr(100.0));
    }
    auto b = MasterBuilder::build(MasterKind::BIAS, bp, cfg_in(d));
    auto k = MasterBuilder::build(MasterKind::DARK, dp, cfg_in(d));
    REQUIRE(b.ok); REQUIRE(k.ok);
    Hdr lh; lh.exptime = 300.0;
    const std::string lp = d.file("cfa_light.fit");
    std::mt19937 rng(11);
    std::normal_distribution<double> noise(0.0, 4.0);
    write_u16_sized(lp, BW, BH, [&](int x, int y) {
        const double sky = red(x, y) ? 6000.0 : 2500.0;
        return static_cast<int>(std::lround(big_bias(x, y) + (red(x, y) ? 2.0 * std::pow(3.0, 0.8) : 0.0)
                                            + dark_signal(x, y, 300.0, true) + sky + noise(rng)));
    }, lh);
    auto st = LightCalibrator::dark_structure(b.master, k.master);
    REQUIRE(st->stride == 2);
    auto rr = FITSReader::read_fixed_scale(lp);
    const DarkScaleFit fit = LightCalibrator::fit_dark_scale(rr.image, b.master, k.master, *st, 3.0);
    INFO("fitted " << fit.fitted << " +/- " << fit.standard_error);
    REQUIRE(fit.well_conditioned);
    REQUIRE(std::fabs(fit.scale - true_k()) < 0.01 * true_k());
}
