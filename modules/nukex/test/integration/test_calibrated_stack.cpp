// Dark/bias calibration as the stacker's optional pre-step.
//
//   1. No masters: execute(lights, flats, {}) is execute(lights, flats), bit
//      for bit -- the PixInsight module's path cannot move.
//   2. Masters: the stack loses the pedestal and the hot pixels, and the log
//      says what was subtracted.
//   3. A light the masters do not fit stops the batch before anything is
//      stacked, naming the light and the reason.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/stacker/calibration_masters.hpp"
#include "nukex/core/progress_observer.hpp"

#include <fitsio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

constexpr int kW = 200, kH = 160;

int bias_adu(int x, int y) { return 800 + (x % 7) + 2 * (y % 5); }
bool is_hot(int x, int y) { return (x % 37 == 11) && (y % 29 == 13); }
int dark_adu(int x, int y, double t) { return static_cast<int>(std::lround((1.0 + (is_hot(x, y) ? 500.0 : 0.0)) * t / 100.0)); }

struct Hdr {
    std::string imagetyp = "Light";
    double exptime = 300.0, temp = -10.0, gain = 0.0, offset = 30.0;
};

void write_u16(const std::string& path, const std::function<double(int, int)>& adu, const Hdr& h) {
    std::vector<unsigned short> px(static_cast<std::size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            px[static_cast<std::size_t>(y) * kW + x] =
                static_cast<unsigned short>(std::clamp(std::lround(adu(x, y)), 0L, 65535L));
    fs::remove(path);
    fitsfile* fp = nullptr;
    int status = 0;
    fits_create_diskfile(&fp, path.c_str(), &status);
    long naxes[2] = {kW, kH};
    fits_create_img(fp, USHORT_IMG, 2, naxes, &status);
    fits_write_img(fp, TUSHORT, 1, static_cast<LONGLONG>(px.size()), px.data(), &status);
    fits_update_key_str(fp, "IMAGETYP", h.imagetyp.c_str(), nullptr, &status);
    fits_update_key_str(fp, "INSTRUME", "ZWO ASI2400MM Pro", nullptr, &status);
    fits_update_key_dbl(fp, "EXPTIME", h.exptime, -10, nullptr, &status);
    fits_update_key_dbl(fp, "CCD-TEMP", h.temp, -10, nullptr, &status);
    fits_update_key_dbl(fp, "GAIN", h.gain, -10, nullptr, &status);
    fits_update_key_dbl(fp, "OFFSET", h.offset, -10, nullptr, &status);
    fits_close_file(fp, &status);
    REQUIRE(status == 0);
}

/// A fixed star field (no drift: hot pixels stay on the same sky pixels, so a
/// stack that did not remove them would keep them).
double stars(int x, int y) {
    static std::vector<double> field = [] {
        std::vector<double> f(static_cast<std::size_t>(kW) * kH, 0.0);
        std::uint32_t st = 777u;
        auto next = [&] { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
        for (int i = 0; i < 50; ++i) {
            const double sx = 8 + (next() >> 8) % (kW - 16), sy = 8 + (next() >> 8) % (kH - 16);
            const double amp = 15000 + 300 * i;
            for (int dy = -7; dy <= 7; ++dy)
                for (int dx = -7; dx <= 7; ++dx) {
                    const int px = static_cast<int>(sx) + dx, py = static_cast<int>(sy) + dy;
                    f[static_cast<std::size_t>(py) * kW + px] += amp * std::exp(-(dx * dx + dy * dy) / 6.0);
                }
        }
        return f;
    }();
    return field[static_cast<std::size_t>(y) * kW + x];
}

struct Log : ProgressObserver {
    std::vector<std::string> lines;
    void begin_phase(const std::string&, int) override {}
    void advance(int, const std::string&) override {}
    void end_phase() override {}
    void message(const std::string& t) override { lines.push_back(t); }
    bool is_cancelled() const override { return false; }
    bool saw(const std::string& s) const {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(s) != std::string::npos; });
    }
};

struct Fixture {
    fs::path dir;
    std::vector<std::string> lights, flats, biases, darks;
    Fixture() {
        // Random name (portable, no pid); create_directory() proves it is new.
        std::random_device rd;
        for (;;) {
            dir = fs::temp_directory_path() /
                ("nukex_calstack_" + std::to_string(rd()) + "_" + std::to_string(rd()));
            if (fs::create_directory(dir)) break;
        }
        std::mt19937 rng(42);
        std::normal_distribution<double> n(0.0, 6.0);
        for (int f = 0; f < 5; ++f) {
            lights.push_back((dir / ("light_" + std::to_string(f) + ".fit")).string());
            write_u16(lights.back(), [&](int x, int y) { return bias_adu(x, y) + dark_adu(x, y, 300) + 3000 + stars(x, y) + n(rng); }, Hdr{});
            Hdr b; b.imagetyp = "Bias"; b.exptime = 0.001;
            biases.push_back((dir / ("bias_" + std::to_string(f) + ".fit")).string());
            write_u16(biases.back(), [&](int x, int y) { return bias_adu(x, y) + n(rng) * 0.5; }, b);
            Hdr d; d.imagetyp = "Dark";
            darks.push_back((dir / ("dark_" + std::to_string(f) + ".fit")).string());
            write_u16(darks.back(), [&](int x, int y) { return bias_adu(x, y) + dark_adu(x, y, 300) + n(rng) * 0.5; }, d);
            Hdr fl; fl.imagetyp = "Flat"; fl.exptime = 0.3;
            flats.push_back((dir / ("flat_" + std::to_string(f) + ".fit")).string());
            write_u16(flats.back(), [&](int x, int y) { return bias_adu(x, y) + 20000.0 * (1.0 - 0.2 * ((x - kW / 2.0) * (x - kW / 2.0)) / (kW * kW)); }, fl);
        }
    }
    ~Fixture() { std::error_code ec; fs::remove_all(dir, ec); }
    StackingEngine::Config config() const {
        StackingEngine::Config c;
        c.cache_dir = dir.string();
        c.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
        return c;
    }
    MasterFrame master(MasterKind k, const std::vector<std::string>& p) const {
        MasterBuilder::Config mc; mc.cache_dir = dir.string();
        auto r = MasterBuilder::build(k, p, mc);
        INFO(r.error);
        REQUIRE(r.ok);
        return std::move(r.master);
    }
};

bool same_bits(const Image& a, const Image& b) {
    return a.width() == b.width() && a.height() == b.height() && a.n_channels() == b.n_channels() &&
           std::memcmp(a.data(), b.data(), a.data_size() * sizeof(float)) == 0;
}

/// Hot pixels' excess over their 3x3 neighbourhood in a stack, in ADU.
double worst_hot_excess(const Image& s, int x0, int y0) {
    double worst = 0.0;
    for (int y = 2; y < s.height() - 2; ++y)
        for (int x = 2; x < s.width() - 2; ++x) {
            if (!is_hot(x + x0, y + y0) || stars(x + x0, y + y0) > 50.0) continue;
            std::vector<float> nb;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy) nb.push_back(s.at(x + dx, y + dy, 0));
            std::nth_element(nb.begin(), nb.begin() + 4, nb.end());
            worst = std::max(worst, std::fabs(static_cast<double>(s.at(x, y, 0) - nb[4])) * 65535.0);
        }
    return worst;
}

} // namespace

TEST_CASE("Calibrated stack: an empty calibration set is the flat-only path, bit for bit", "[integration][darkbias]") {
    Fixture fx;
    auto a = StackingEngine(fx.config()).execute(fx.lights, fx.flats, nullptr);
    auto b = StackingEngine(fx.config()).execute(fx.lights, fx.flats, CalibrationSet{}, nullptr);
    REQUIRE(a.ok); REQUIRE(b.ok);
    REQUIRE(same_bits(a.stacked, b.stacked));
    REQUIRE(same_bits(a.noise_map, b.noise_map));
    REQUIRE(same_bits(a.measured_noise, b.measured_noise));
}

TEST_CASE("Calibrated stack: bias and dark leave the stack without pedestal or hot pixels", "[integration][darkbias]") {
    Fixture fx;
    CalibrationSet bias_only;
    bias_only.biases.push_back(fx.master(MasterKind::BIAS, fx.biases));
    CalibrationSet full = bias_only;
    full.darks.push_back(fx.master(MasterKind::DARK, fx.darks));

    Log log_b, log_f;
    auto rb = StackingEngine(fx.config()).execute(fx.lights, fx.flats, bias_only, &log_b);
    auto rf = StackingEngine(fx.config()).execute(fx.lights, fx.flats, full, &log_f);
    INFO(rb.error << " | " << rf.error);
    REQUIRE(rb.ok); REQUIRE(rf.ok);
    REQUIRE(rf.n_frames_processed == 5);
    REQUIRE(log_f.saw("Calibration: 5 lights: dark"));
    REQUIRE(log_f.saw("subtracted as is: same exposure"));
    REQUIRE(log_f.saw("Calibration: flats -- bias"));

    // Hot pixels: ~1500 ADU with the bias alone, gone with the dark.
    const int x0 = rf.trim.x0, y0 = rf.trim.y0;
    const double hot_b = worst_hot_excess(rb.stacked, rb.trim.x0, rb.trim.y0);
    const double hot_f = worst_hot_excess(rf.stacked, x0, y0);
    INFO("hot excess: bias only " << hot_b << " ADU, bias+dark " << hot_f << " ADU");
    REQUIRE(hot_b > 1000.0);
    REQUIRE(hot_f < 20.0);

    // Pedestal: the uncalibrated light sits at bias + dark + sky; the
    // calibrated stack's background is the sky alone (3000 ADU), divided by a
    // flat normalised to its own level.
    std::vector<float> bg;
    for (int y = 0; y < rf.stacked.height(); ++y)
        for (int x = 0; x < rf.stacked.width(); ++x)
            if (stars(x + x0, y + y0) < 1.0) bg.push_back(rf.stacked.at(x, y, 0));
    std::nth_element(bg.begin(), bg.begin() + bg.size() / 2, bg.end());
    const double level = bg[bg.size() / 2] * 65535.0;
    INFO("background " << level << " ADU");
    REQUIRE(level > 2850.0);
    REQUIRE(level < 3150.0);
}

TEST_CASE("Calibrated stack: a light the masters do not fit stops the batch, by name", "[integration][darkbias][mismatch]") {
    Fixture fx;
    CalibrationSet cal;
    cal.biases.push_back(fx.master(MasterKind::BIAS, fx.biases));
    cal.darks.push_back(fx.master(MasterKind::DARK, fx.darks));
    Hdr odd; odd.gain = 100;
    write_u16(fx.lights[3], [](int x, int y) { return bias_adu(x, y) + 3000.0; }, odd);
    Log log;
    auto r = StackingEngine(fx.config()).execute(fx.lights, fx.flats, cal, &log);
    REQUIRE_FALSE(r.ok);
    REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("light_3.fit"));
    REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("gain 100 vs 0"));
    REQUIRE(r.n_frames_processed == 0);
}

TEST_CASE("Calibrated stack: flats with no pedestal master are refused", "[integration][darkbias][mismatch]") {
    Fixture fx;
    CalibrationSet cal;
    cal.darks.push_back(fx.master(MasterKind::DARK, fx.darks));
    auto r = StackingEngine(fx.config()).execute(fx.lights, fx.flats, cal, nullptr);
    REQUIRE_FALSE(r.ok);
    REQUIRE_THAT(r.error, Catch::Matchers::ContainsSubstring("dark-flat or a master bias"));
    // Without flats the same darks are fine (an unscaled dark carries the bias).
    auto ok = StackingEngine(fx.config()).execute(fx.lights, {}, cal, nullptr);
    INFO(ok.error);
    REQUIRE(ok.ok);
}

// ── The flat-only path's order ──────────────────────────────────────

namespace {
void write_cfa_float(const std::string& path, const std::function<float(int, int)>& v, const char* imagetyp) {
    std::vector<float> px(static_cast<std::size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) px[static_cast<std::size_t>(y) * kW + x] = v(x, y);
    fs::remove(path);
    fitsfile* fp = nullptr;
    int status = 0;
    fits_create_diskfile(&fp, path.c_str(), &status);
    long naxes[2] = {kW, kH};
    fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
    fits_write_img(fp, TFLOAT, 1, static_cast<LONGLONG>(px.size()), px.data(), &status);
    fits_update_key_str(fp, "IMAGETYP", imagetyp, nullptr, &status);
    fits_update_key_str(fp, "BAYERPAT", "RGGB", nullptr, &status);
    fits_update_key_str(fp, "INSTRUME", "ASI585MC", nullptr, &status);
    fits_close_file(fp, &status);
    REQUIRE(status == 0);
}
/// A flat whose photosites differ by colour (R 0.5, G 1.0, B 0.75) as well
/// as by vignetting -- a tinted flat panel.
float cfa_gain(int x, int y) {
    const bool r = (y % 2 == 0) && (x % 2 == 0), b = (y % 2 == 1) && (x % 2 == 1);
    const float v = 1.0f - 0.3f * static_cast<float>(((x - kW / 2.0) * (x - kW / 2.0) + (y - kH / 2.0) * (y - kH / 2.0)) / (kW * kW / 4.0));
    return (r ? 0.5f : b ? 0.75f : 1.0f) * v;
}
double interior_cv(const Image& img, int ch) {
    double s = 0, s2 = 0; int n = 0;
    for (int y = 8; y < img.height() - 8; ++y)
        for (int x = 8; x < img.width() - 8; ++x) { const double v = img.at(x, y, ch); s += v; s2 += v * v; ++n; }
    const double m = s / n;
    return std::sqrt(std::max(0.0, s2 / n - m * m)) / std::fabs(m);
}
} // namespace

TEST_CASE("Flat-only path: a CFA flat divides each photosite before debayer", "[integration][flat]") {
    fs::path dir;
    std::random_device rd;
    for (;;) {
        dir = fs::temp_directory_path() / ("nukex_cfaflat_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        if (fs::create_directory(dir)) break;
    }
    std::vector<std::string> flats, lights;
    for (int f = 0; f < 3; ++f) {
        flats.push_back((dir / ("flat_" + std::to_string(f) + ".fit")).string());
        write_cfa_float(flats.back(), [f](int x, int y) { return (0.4f + 0.01f * f) * cfa_gain(x, y); }, "Flat");
    }
    // A uniformly grey sky seen through that optical path: every photosite
    // is sky x its own flat.
    lights.push_back((dir / "light.fit").string());
    write_cfa_float(lights.back(), [](int x, int y) { return 0.3f * cfa_gain(x, y); }, "Light");

    StackingEngine::Config c;
    c.cache_dir = dir.string();
    c.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    auto r = StackingEngine(c).execute(lights, flats, nullptr);
    REQUIRE(r.ok);
    REQUIRE(r.stacked.n_channels() >= 3);
    for (int ch = 0; ch < 3; ++ch) {
        INFO("channel " << ch << " CV " << interior_cv(r.stacked, ch));
        // Divided per photosite: a grey field, flat in every channel. Divided
        // after debayer it is the CFA pattern again (CV ~0.3).
        REQUIRE(interior_cv(r.stacked, ch) < 1e-3);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Calibrated stack: a scaled dark's fit is reported, and a poorly conditioned one loudly", "[integration][darkbias]") {
    Fixture fx;
    MasterBuilder::Config mc; mc.cache_dir = fx.dir.string();
    CalibrationSet good, flat_dark;
    good.biases.push_back(fx.master(MasterKind::BIAS, fx.biases));
    flat_dark.biases.push_back(fx.master(MasterKind::BIAS, fx.biases));
    // 100 s darks: with ~3% structured pixels (fit well conditioned), and
    // with a uniform dark current only (nothing to fit against). The lights
    // carry the same structure at 300 s.
    auto structure = [](int x, int y) {
        const unsigned h = static_cast<unsigned>(x) * 73856093u ^ static_cast<unsigned>(y) * 19349663u;
        return (h % 33u == 0u) ? 80.0 + static_cast<double>((h / 33u) % 400u) : 0.0;
    };
    std::vector<std::string> lights;
    std::mt19937 rng(9);
    std::normal_distribution<double> n(0.0, 6.0);
    for (int f = 0; f < 5; ++f) {
        lights.push_back((fx.dir / ("sl_" + std::to_string(f) + ".fit")).string());
        write_u16(lights.back(), [&](int x, int y) { return bias_adu(x, y) + 3.0 + 3.0 * structure(x, y) + 3000 + stars(x, y) + n(rng); }, Hdr{});
    }
    std::vector<std::string> hot100, flat100;
    for (int f = 0; f < 5; ++f) {
        Hdr d; d.imagetyp = "Dark"; d.exptime = 100.0;
        hot100.push_back((fx.dir / ("dh_" + std::to_string(f) + ".fit")).string());
        write_u16(hot100.back(), [&, f](int x, int y) { return bias_adu(x, y) + 1.0 + structure(x, y) + ((x + y + f) % 3) - 1.0; }, d);
        flat100.push_back((fx.dir / ("df_" + std::to_string(f) + ".fit")).string());
        write_u16(flat100.back(), [f](int x, int y) { return bias_adu(x, y) + 1.0 + ((x + y + f) % 3) - 1.0; }, d);
    }
    good.darks.push_back(fx.master(MasterKind::DARK, hot100));
    flat_dark.darks.push_back(fx.master(MasterKind::DARK, flat100));
    Log lg, lf;
    auto rg = StackingEngine(fx.config()).execute(lights, {}, good, &lg);
    auto rf = StackingEngine(fx.config()).execute(lights, {}, flat_dark, &lf);
    INFO(rg.error << " | " << rf.error);
    REQUIRE(rg.ok); REQUIRE(rf.ok);
    REQUIRE(lg.saw("Calibration: dark scale fitted per light on 5 lights"));
    REQUIRE_FALSE(lg.saw("dark scale: "));
    REQUIRE(lf.saw("Calibration warning: light "));
    REQUIRE(lf.saw("header ratio 3.0000 is used instead"));
}
