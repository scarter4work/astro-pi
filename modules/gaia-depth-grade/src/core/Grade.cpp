#include "Grade.h"

#include "Display.h"
#include "Format.h"
#include "Nebula.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gdg {

GradeConfig grade_config(const GradeSettings& s) {
    GradeConfig c;
    c.gains.brightness = s.brightness;
    c.gains.size = s.size;
    c.gains.contrast = s.contrast;
    c.gains.saturation = s.saturation;
    c.gains.halo_gain = s.halo_gain;
    c.p_low = s.p_low;
    c.p_high = s.p_high;
    c.match_tolerance_px = s.match_tolerance;
    c.min_match_rate = s.min_match_rate;
    c.detect_fwhm = s.detect_fwhm;
    c.detect_threshold_sigma = s.detect_sigma;
    c.base_sigma_px = s.halo_sigma;
    c.neutral_strength = s.neutral_strength;
    c.gaia_mag_limit = s.mag_limit_enabled ? std::optional<double>(s.mag_limit) : std::nullopt;
    c.gaia_parallax_snr = s.parallax_snr;
    c.halo_min_refs = s.halo_min_refs;
    c.halo_radius_sigma = s.halo_radius_sigma;
    c.halo_moffat_beta = s.halo_moffat_beta;
    return c;
}

bool nebula_active(const GradeSettings& s) { return s.nebula_enabled && s.nebula_strength > 0.0; }

bool prepare_settings_differ(const GradeSettings& a, const GradeSettings& b) {
    return a.detect_sigma != b.detect_sigma || a.detect_fwhm != b.detect_fwhm || a.match_tolerance != b.match_tolerance ||
           a.min_match_rate != b.min_match_rate || a.online != b.online || a.mag_limit_enabled != b.mag_limit_enabled ||
           (a.mag_limit_enabled && a.mag_limit != b.mag_limit) || a.parallax_snr != b.parallax_snr;
}

Catalog FixedCatalogSource::distances_for(const Footprint& fp) {
    if (fp.center_ra != fp_.center_ra || fp.center_dec != fp_.center_dec || fp.radius_deg != fp_.radius_deg)
        throw std::logic_error(strformat("FixedCatalogSource: the catalogue was fetched for (%.8f, %.8f, r %.8f), "
                                         "not (%.8f, %.8f, r %.8f)",
                                         fp_.center_ra, fp_.center_dec, fp_.radius_deg, fp.center_ra, fp.center_dec,
                                         fp.radius_deg));
    return catalog_;
}

Image grade_composite(const Image& stars, const Image& starless, const PrepResult& prep, const GradeSettings& s,
                      const Log& log) {
    if (!stars.same_shape(starless))
        throw std::invalid_argument(strformat("the stars (%dx%dx%d) and starless (%dx%dx%d) layers differ in shape",
                                              stars.width, stars.height, stars.channels, starless.width,
                                              starless.height, starless.channels));
    const GradeConfig cfg = grade_config(s);
    const Image graded = render_from_prep(stars, prep, cfg, log);
    if (!nebula_active(s)) return screen_blend(graded, starless);
    const Image nebula = render_nebula(starless, prep.table, s.nebula_strength, s.nebula_atmos, s.nebula_structure);
    return screen_blend(graded, nebula);
}

InsetRegion inset_region(int width, int height, int cx, int cy, int size) {
    InsetRegion r;
    r.x = std::max(0, std::min(width - size, cx - size / 2));
    r.y = std::max(0, std::min(height - size, cy - size / 2));
    r.w = size;
    r.h = size;
    return r;
}

namespace {

// JavaScript Math.round: floor(v + 0.5).
int js_round(double v) { return static_cast<int>(std::floor(v + 0.5)); }

}  // namespace

void default_inset_center(int width, int height, int& cx, int& cy) {
    cx = js_round(width / 2.0);
    cy = js_round(height / 2.0);
}

void inset_center_from_click(int px, int py, int full_width, int preview_width, int& cx, int& cy) {
    if (preview_width < 1) throw std::invalid_argument("inset_center_from_click: no preview");
    const double scale = static_cast<double>(full_width) / preview_width;
    cx = js_round(px * scale);
    cy = js_round(py * scale);
}

PreviewPixels preview_pixels(const Image& composite, int max_width, const InsetRegion& inset) {
    const std::vector<uint8_t> stretched = autostretch_u8(composite);
    const int W = composite.width, H = composite.height, C = composite.channels;
    PreviewPixels p;
    p.channels = C;
    const auto [w, h] = downscale_size(W, H, max_width);
    p.width = w;
    p.height = h;
    p.full = downscale_u8(stretched, W, H, C, max_width);

    // a[y:y+h, x:x+w] on the full-resolution stretch.
    const int x0 = std::clamp(inset.x, 0, W), y0 = std::clamp(inset.y, 0, H);
    const int x1 = std::clamp(inset.x + inset.w, x0, W), y1 = std::clamp(inset.y + inset.h, y0, H);
    p.inset_width = x1 - x0;
    p.inset_height = y1 - y0;
    p.inset.reserve(static_cast<size_t>(p.inset_width) * p.inset_height * C);
    for (int y = y0; y < y1; ++y) {
        const size_t row = (static_cast<size_t>(y) * W + x0) * C;
        p.inset.insert(p.inset.end(), stretched.begin() + row, stretched.begin() + row + static_cast<size_t>(x1 - x0) * C);
    }
    return p;
}

std::string qa_summary(const QA& qa) {
    std::string s = strformat("Prepared: %d/%d matched (rate %.2f, offset %.1f px)", qa.n_matched, qa.n_detected,
                              qa.match_rate, qa.median_offset_px);
    if (qa.low_match_warning) s += "  \xE2\x9A\xA0 LOW MATCH \xE2\x80\x94 depth grade unreliable";
    return s;
}

}  // namespace gdg
