#include "catch_amalgamated.hpp"
#include "test_data_loader.hpp"
#include "png_writer.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/debayer.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "nukex/stretch/log_stretch.hpp"
#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/ghs_stretch.hpp"
#include "nukex/stretch/ots_stretch.hpp"
#include "nukex/stretch/sas_stretch.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"
#include "nukex/stretch/photometric_stretch.hpp"
#include <cmath>
#include <iostream>
#include <filesystem>
#include <vector>
#include <algorithm>

using namespace nukex;

// ── Percentile levels we measure at ──
static constexpr float kRefLevels[] = {0.50f, 0.75f, 0.85f, 0.90f, 0.95f, 0.97f, 0.99f, 0.995f, 0.999f};
static constexpr int kNLevels = 9;

// Reference profiles from ASI FITS viewer
static constexpr float kRefS2O3[] = {0.1494f, 0.1815f, 0.1989f, 0.2115f, 0.2313f, 0.2455f, 0.2774f, 0.3049f, 0.5107f};
static constexpr float kRefHaO3[] = {0.1508f, 0.1797f, 0.1978f, 0.2128f, 0.2444f, 0.2776f, 0.3692f, 0.4179f, 0.5766f};
static constexpr float kRefRGB[]  = {0.1538f, 0.1785f, 0.1936f, 0.2067f, 0.2353f, 0.2643f, 0.3365f, 0.4330f, 0.7609f};

// Active reference profile (set per test case)
static const float* kRefProfile = kRefS2O3;

// ── Helpers ──

static float percentile_arr(const float* data, int n, float pct) {
    std::vector<float> sorted(data, data + n);
    size_t idx = std::min(static_cast<size_t>(n * pct), static_cast<size_t>(n - 1));
    std::nth_element(sorted.begin(), sorted.begin() + idx, sorted.end());
    return sorted[idx];
}

static void compute_profile(const Image& img, float profile[kNLevels]) {
    int n = img.width() * img.height();
    std::vector<float> lum(n);
    for (int i = 0; i < n; i++) {
        float r = img.channel_data(0)[i];
        float g = (img.n_channels() > 1) ? img.channel_data(1)[i] : r;
        float b = (img.n_channels() > 2) ? img.channel_data(2)[i] : r;
        lum[i] = std::clamp(0.2126f * r + 0.7152f * g + 0.0722f * b, 0.0f, 1.0f);
    }
    for (int i = 0; i < kNLevels; i++)
        profile[i] = percentile_arr(lum.data(), n, kRefLevels[i]);
}

// ── Scoring ──

static float score_reference_match(const float profile[kNLevels]) {
    float ref_min = kRefProfile[0], ref_max = kRefProfile[kNLevels - 1];
    float ref_range = ref_max - ref_min;
    float out_min = profile[0], out_max = profile[kNLevels - 1];
    float out_range = out_max - out_min;
    if (ref_range < 1e-6f || out_range < 1e-6f) return 0.0f;

    float sse = 0.0f;
    for (int i = 0; i < kNLevels; i++) {
        float ref_norm = (kRefProfile[i] - ref_min) / ref_range;
        float out_norm = (profile[i] - out_min) / out_range;
        float diff = ref_norm - out_norm;
        sse += diff * diff;
    }
    return std::max(0.0f, 1.0f - std::sqrt(sse / kNLevels) * 5.0f);
}

static float score_contrast(const float profile[kNLevels]) {
    return std::clamp(profile[6] - profile[0], 0.0f, 1.0f);  // p99 - p50
}

static float score_dynamic_range(const float profile[kNLevels]) {
    float spread = 0.0f;
    for (int i = 1; i < kNLevels; i++)
        spread += std::max(0.0f, profile[i] - profile[i - 1]);
    int clipped = 0;
    for (int i = 0; i < kNLevels; i++)
        if (profile[i] <= 0.001f || profile[i] >= 0.999f) clipped++;
    return spread * (1.0f - clipped / static_cast<float>(kNLevels));
}

static float combined_score(const float profile[kNLevels]) {
    return 5.0f * score_reference_match(profile)
         + 3.0f * score_dynamic_range(profile)
         + 1.0f * score_contrast(profile);
}

struct SweepResult {
    float param, score, s1, s2, s3;
    float profile[kNLevels];
};

// Load a specific FITS file with full debayer pipeline
static Image load_specific_frame(const std::string& path) {
    auto result = FITSReader::read(path);
    if (!result.success) return {};
    if (!result.metadata.bayer_pattern.empty() &&
        result.metadata.bayer_pattern != "NONE") {
        BayerPattern bp = BayerPattern::RGGB;
        if (result.metadata.bayer_pattern == "BGGR") bp = BayerPattern::BGGR;
        else if (result.metadata.bayer_pattern == "GRBG") bp = BayerPattern::GRBG;
        else if (result.metadata.bayer_pattern == "GBRG") bp = BayerPattern::GBRG;
        DebayerEngine::equalize_bayer_background(result.image, bp);
        Image rgb = DebayerEngine::debayer(result.image, bp);
        DebayerEngine::suppress_banding(rgb);
        return rgb;
    }
    return std::move(result.image);
}

// Run the full optimizer sweep and output best PNGs
static void run_sweep(Image& img, const char* prefix) {
    // ── ArcSinh: 20 values from 1 to 30 ──
    std::cout << "\n--- ArcSinh sweep ---\n";
    std::vector<SweepResult> a_res;
    for (int i = 0; i < 20; i++) {
        float alpha = 1.0f + i * 1.5f;
        Image out = img.clone();
        ArcSinhStretch s; s.alpha = alpha; s.luminance_only = true; s.apply(out);
        SweepResult r; r.param = alpha;
        compute_profile(out, r.profile);
        r.s1 = score_reference_match(r.profile);
        r.s2 = score_contrast(r.profile);
        r.s3 = score_dynamic_range(r.profile);
        r.score = combined_score(r.profile);
        a_res.push_back(r);
        std::cout << "  a=" << alpha << " score=" << r.score << " match=" << r.s1 << "\n";
    }
    std::sort(a_res.begin(), a_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        ArcSinhStretch s; s.alpha = a_res[0].param; s.luminance_only = true; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_arcsinh.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── Log: 20 values from 1 to 60 ──
    std::cout << "\n--- Log sweep ---\n";
    std::vector<SweepResult> l_res;
    for (int i = 0; i < 20; i++) {
        float alpha = 1.0f + i * 3.0f;
        Image out = img.clone();
        LogStretch s; s.alpha = alpha; s.luminance_only = true; s.apply(out);
        SweepResult r; r.param = alpha;
        compute_profile(out, r.profile);
        r.s1 = score_reference_match(r.profile);
        r.s2 = score_contrast(r.profile);
        r.s3 = score_dynamic_range(r.profile);
        r.score = combined_score(r.profile);
        l_res.push_back(r);
        std::cout << "  a=" << alpha << " score=" << r.score << " match=" << r.s1 << "\n";
    }
    std::sort(l_res.begin(), l_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        LogStretch s; s.alpha = l_res[0].param; s.luminance_only = true; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_log.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── MTF: 20 values from 0.05 to 0.95 ──
    std::cout << "\n--- MTF sweep ---\n";
    std::vector<SweepResult> m_res;
    for (int i = 0; i < 20; i++) {
        float mid = 0.05f + i * 0.0474f;
        Image out = img.clone();
        MTFStretch s; s.midtone = mid; s.luminance_only = true; s.apply(out);
        SweepResult r; r.param = mid;
        compute_profile(out, r.profile);
        r.s1 = score_reference_match(r.profile);
        r.s2 = score_contrast(r.profile);
        r.s3 = score_dynamic_range(r.profile);
        r.score = combined_score(r.profile);
        m_res.push_back(r);
        std::cout << "  m=" << mid << " score=" << r.score << " match=" << r.s1 << "\n";
    }
    std::sort(m_res.begin(), m_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        MTFStretch s; s.midtone = m_res[0].param; s.luminance_only = true; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_mtf.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── GHS: sweep D for b ∈ {-1, 0, 1}, SP=0, LP=0, HP=1 ──
    // b=-1 (log), b=0 (exp), b=1 (harmonic) — the 3 most distinct families
    std::cout << "\n--- GHS sweep ---\n";
    struct GHSResult { float D; float b; float score, s1; float profile[kNLevels]; };
    std::vector<GHSResult> g_res;
    for (float b_val : {-1.0f, 0.0f, 1.0f}) {
        for (int i = 0; i < 20; i++) {
            float D = 1.0f + i * 1.5f;  // 1.0 to 29.5
            Image out = img.clone();
            GHSStretch s; s.D = D; s.b = b_val; s.SP = 0.0f; s.LP = 0.0f; s.HP = 1.0f;
            s.luminance_only = true; s.apply(out);
            GHSResult r; r.D = D; r.b = b_val;
            compute_profile(out, r.profile);
            r.s1 = score_reference_match(r.profile);
            r.score = combined_score(r.profile);
            g_res.push_back(r);
            std::cout << "  D=" << D << " b=" << b_val << " score=" << r.score << " match=" << r.s1 << "\n";
        }
    }
    std::sort(g_res.begin(), g_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        GHSStretch s; s.D = g_res[0].D; s.b = g_res[0].b; s.SP = 0.0f; s.LP = 0.0f; s.HP = 1.0f;
        s.luminance_only = true; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_ghs.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── OTS: try each target type + Gaussian mu sweep ──
    std::cout << "\n--- OTS sweep ---\n";
    struct OTSResult { OTSTarget target; float gauss_mu; float score, s1; float profile[kNLevels]; };
    std::vector<OTSResult> o_res;
    // Fixed targets
    for (auto tgt : {OTSTarget::MUNSELL, OTSTarget::SQRT, OTSTarget::UNIFORM}) {
        Image out = img.clone();
        OTSStretch s; s.target = tgt; s.luminance_only = true; s.apply(out);
        OTSResult r; r.target = tgt; r.gauss_mu = 0.0f;
        compute_profile(out, r.profile);
        r.s1 = score_reference_match(r.profile);
        r.score = combined_score(r.profile);
        o_res.push_back(r);
        const char* names[] = {"Munsell", "Sqrt", "Uniform", "Gaussian"};
        std::cout << "  target=" << names[static_cast<int>(tgt)] << " score=" << r.score << " match=" << r.s1 << "\n";
    }
    // Gaussian with mu sweep
    for (float mu : {0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f}) {
        Image out = img.clone();
        OTSStretch s; s.target = OTSTarget::GAUSSIAN; s.gauss_mu = mu; s.gauss_sigma = 0.15f;
        s.luminance_only = true; s.apply(out);
        OTSResult r; r.target = OTSTarget::GAUSSIAN; r.gauss_mu = mu;
        compute_profile(out, r.profile);
        r.s1 = score_reference_match(r.profile);
        r.score = combined_score(r.profile);
        o_res.push_back(r);
        std::cout << "  target=Gaussian mu=" << mu << " score=" << r.score << " match=" << r.s1 << "\n";
    }
    std::sort(o_res.begin(), o_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        OTSStretch s; s.target = o_res[0].target;
        if (s.target == OTSTarget::GAUSSIAN) { s.gauss_mu = o_res[0].gauss_mu; s.gauss_sigma = 0.15f; }
        s.luminance_only = true; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_ots.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── SAS: sweep target_median ──
    std::cout << "\n--- SAS sweep ---\n";
    std::vector<SweepResult> s_res;
    for (float tm : {0.05f, 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f}) {
        Image out = img.clone();
        SASStretch s; s.tile_size = 256; s.tile_overlap = 0.5f;
        s.target_median = tm; s.max_D = 15.0f; s.ghs_b = 0.0f;
        s.luminance_only = true; s.apply(out);
        SweepResult r; r.param = tm;
        compute_profile(out, r.profile);
        r.s1 = score_reference_match(r.profile);
        r.s2 = score_contrast(r.profile);
        r.s3 = score_dynamic_range(r.profile);
        r.score = combined_score(r.profile);
        s_res.push_back(r);
        std::cout << "  target_median=" << tm << " score=" << r.score << " match=" << r.s1 << "\n";
    }
    std::sort(s_res.begin(), s_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        SASStretch s; s.tile_size = 256; s.tile_overlap = 0.5f;
        s.target_median = s_res[0].param; s.max_D = 15.0f; s.ghs_b = 0.0f;
        s.luminance_only = true; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_sas.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── Lupton: sweep Q with fixed stretch=5, then stretch with best Q ──
    std::cout << "\n--- Lupton sweep ---\n";
    struct LuptonResult { float Q_val; float stretch_val; float score, s1; float profile[kNLevels]; };
    std::vector<LuptonResult> lup_res;
    for (float Q_val : {2.0f, 4.0f, 8.0f, 15.0f, 25.0f, 40.0f, 60.0f, 100.0f}) {
        for (float str : {0.5f, 1.0f, 2.0f, 5.0f}) {
            Image out = img.clone();
            LuptonStretch s; s.Q = Q_val; s.stretch = str; s.apply(out);
            LuptonResult r; r.Q_val = Q_val; r.stretch_val = str;
            compute_profile(out, r.profile);
            r.s1 = score_reference_match(r.profile);
            r.score = combined_score(r.profile);
            lup_res.push_back(r);
            std::cout << "  Q=" << Q_val << " stretch=" << str << " score=" << r.score << " match=" << r.s1 << "\n";
        }
    }
    std::sort(lup_res.begin(), lup_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        LuptonStretch s; s.Q = lup_res[0].Q_val; s.stretch = lup_res[0].stretch_val; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_lupton.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── VeraLux: sweep log_D and protect_b ──
    std::cout << "\n--- VeraLux sweep ---\n";
    struct VLResult { float logD; float pb; float score, s1; float profile[kNLevels]; };
    std::vector<VLResult> vl_res;
    for (float logD : {0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f}) {
        for (float pb : {2.0f, 6.0f, 10.0f}) {
            Image out = img.clone();
            VeraLuxStretch s; s.log_D = logD; s.protect_b = pb; s.apply(out);
            VLResult r; r.logD = logD; r.pb = pb;
            compute_profile(out, r.profile);
            r.s1 = score_reference_match(r.profile);
            r.score = combined_score(r.profile);
            vl_res.push_back(r);
            std::cout << "  logD=" << logD << " b=" << pb << " score=" << r.score << " match=" << r.s1 << "\n";
        }
    }
    std::sort(vl_res.begin(), vl_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        VeraLuxStretch s; s.log_D = vl_res[0].logD; s.protect_b = vl_res[0].pb; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_veralux.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── CLAHE: sweep clip_limit (applied after ArcSinh α=4 as finisher) ──
    std::cout << "\n--- CLAHE sweep (after ArcSinh a=4) ---\n";
    std::vector<SweepResult> cl_res;
    for (float clip : {1.2f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f}) {
        Image out = img.clone();
        // Primary stretch first
        ArcSinhStretch pre; pre.alpha = 4.0f; pre.luminance_only = true; pre.apply(out);
        // Then CLAHE finisher
        CLAHEStretch s; s.clip_limit = clip; s.tile_cols = 8; s.tile_rows = 8;
        s.luminance_only = true; s.apply(out);
        SweepResult r; r.param = clip;
        compute_profile(out, r.profile);
        r.s1 = score_reference_match(r.profile);
        r.s2 = score_contrast(r.profile);
        r.s3 = score_dynamic_range(r.profile);
        r.score = combined_score(r.profile);
        cl_res.push_back(r);
        std::cout << "  clip=" << clip << " score=" << r.score << " match=" << r.s1 << "\n";
    }
    std::sort(cl_res.begin(), cl_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        ArcSinhStretch pre; pre.alpha = 4.0f; pre.luminance_only = true; pre.apply(out);
        CLAHEStretch s; s.clip_limit = cl_res[0].param; s.tile_cols = 8; s.tile_rows = 8;
        s.luminance_only = true; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_clahe.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── Photometric: sweep mu_faint (display range) ──
    std::cout << "\n--- Photometric sweep ---\n";
    std::vector<SweepResult> ph_res;
    for (float faint : {24.0f, 25.0f, 26.0f, 27.0f, 28.0f, 29.0f, 30.0f}) {
        // Photometric operates on unprepared data (does its own sky subtraction)
        // We need the original img before prepare_for_stretch, but we only have prepared.
        // Use prepared data with sky_level=0 (already subtracted).
        Image out = img.clone();
        PhotometricStretch s; s.mu_faint = faint; s.sky_level = 0.001f;
        s.luminance_only = true; s.apply(out);
        SweepResult r; r.param = faint;
        compute_profile(out, r.profile);
        r.s1 = score_reference_match(r.profile);
        r.s2 = score_contrast(r.profile);
        r.s3 = score_dynamic_range(r.profile);
        r.score = combined_score(r.profile);
        ph_res.push_back(r);
        std::cout << "  mu_faint=" << faint << " score=" << r.score << " match=" << r.s1 << "\n";
    }
    std::sort(ph_res.begin(), ph_res.end(), [](auto& a, auto& b) { return a.score > b.score; });
    {
        Image out = img.clone();
        PhotometricStretch s; s.mu_faint = ph_res[0].param; s.sky_level = 0.001f;
        s.luminance_only = true; s.apply(out);
        char name[128]; snprintf(name, sizeof(name), "test/output/%s_best_photometric.png", prefix);
        test_util::write_png_8bit(name, out);
    }

    // ── Print all winners ──
    const char* ots_names[] = {"Munsell", "Sqrt", "Uniform", "Gaussian"};
    std::cout << "\n=== WINNERS (" << prefix << ") ===\n";
    std::cout << "ArcSinh:      alpha=" << a_res[0].param << " (score=" << a_res[0].score << " match=" << a_res[0].s1 << ")\n";
    std::cout << "Log:          alpha=" << l_res[0].param << " (score=" << l_res[0].score << " match=" << l_res[0].s1 << ")\n";
    std::cout << "MTF:          midtone=" << m_res[0].param << " (score=" << m_res[0].score << " match=" << m_res[0].s1 << ")\n";
    std::cout << "GHS:          D=" << g_res[0].D << " b=" << g_res[0].b << " (score=" << g_res[0].score << " match=" << g_res[0].s1 << ")\n";
    std::cout << "OTS:          target=" << ots_names[static_cast<int>(o_res[0].target)];
    if (o_res[0].target == OTSTarget::GAUSSIAN) std::cout << " mu=" << o_res[0].gauss_mu;
    std::cout << " (score=" << o_res[0].score << " match=" << o_res[0].s1 << ")\n";
    std::cout << "SAS:          target_median=" << s_res[0].param << " (score=" << s_res[0].score << " match=" << s_res[0].s1 << ")\n";
    std::cout << "Lupton:       Q=" << lup_res[0].Q_val << " stretch=" << lup_res[0].stretch_val << " (score=" << lup_res[0].score << " match=" << lup_res[0].s1 << ")\n";
    std::cout << "VeraLux:      logD=" << vl_res[0].logD << " b=" << vl_res[0].pb << " (score=" << vl_res[0].score << " match=" << vl_res[0].s1 << ")\n";
    std::cout << "CLAHE:        clip=" << cl_res[0].param << " (score=" << cl_res[0].score << " match=" << cl_res[0].s1 << ")\n";
    std::cout << "Photometric:  mu_faint=" << ph_res[0].param << " (score=" << ph_res[0].score << " match=" << ph_res[0].s1 << ")\n";
}

// ── Test cases ──

// Phase-5 stretch parameter optimisation sweeps (dev-time exploration,
// not CI regression).  Each sweep runs hundreds of candidate parameter
// vectors and writes visual output PNGs.  Tagged [.sweep] so ctest
// skips them by default.  Invoke explicitly with:
//   ./test/test_calibrate [calibrate]
TEST_CASE("CALIBRATE: S2O3 optimized sweep", "[.sweep][calibrate][s2o3]") {
    kRefProfile = kRefS2O3;
    auto img = test_util::load_m16_test_frame();
    REQUIRE(!img.empty());
    std::filesystem::create_directories("test/output");
    test_util::prepare_for_stretch(img);
    std::cout << "\n=== S2O3 optimization ===\n";
    run_sweep(img, "s2o3");
    REQUIRE(true);
}

TEST_CASE("CALIBRATE: HaO3 optimized sweep", "[.sweep][calibrate][hao3]") {
    kRefProfile = kRefHaO3;
    std::string path = test_util::m16_data_dir() +
        "Light_M16_300.0s_Bin1_HaO3_20230901-233542_0005.fit";
    auto img = load_specific_frame(path);
    REQUIRE(!img.empty());
    std::filesystem::create_directories("test/output");
    test_util::prepare_for_stretch(img);
    std::cout << "\n=== HaO3 optimization ===\n";
    run_sweep(img, "hao3");
    REQUIRE(true);
}

TEST_CASE("CALIBRATE: RGB optimized sweep", "[.sweep][calibrate][rgb]") {
    kRefProfile = kRefRGB;
    std::string path = test_util::m16_data_dir() +
        "Light_M16_300.0s_Bin1_LPro_20230831-225018_0005.fit";
    auto img = load_specific_frame(path);
    REQUIRE(!img.empty());
    std::filesystem::create_directories("test/output");
    test_util::prepare_for_stretch(img);
    std::cout << "\n=== RGB (LPro broadband) optimization ===\n";
    run_sweep(img, "rgb");
    REQUIRE(true);
}
