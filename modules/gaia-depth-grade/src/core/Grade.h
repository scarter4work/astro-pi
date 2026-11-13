// The GaiaDepthGrade process' engine settings and the dialog's render/preview steps (plan Tasks
// 16-17; PCL-free). The process parameters map onto GradeSettings one to one; grade_config()
// turns them into the engine's GradeConfig, and grade_composite() / preview_pixels() are the
// sidecar's render + nebula + recombine and preview commands (cli.py) in one place, so the
// interface's Preview, its Execute and the headless ExecuteOn render exactly the same image.
#pragma once

#include "Catalog.h"
#include "Config.h"
#include "Image.h"
#include "Log.h"
#include "Pipeline.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gdg {

/// cli.HONESTY, verbatim: the DEPTHTAG keyword value and the HISTORY line of every output.
inline constexpr const char* kHonesty = "GAIA depth-derived; physically motivated, not per-pixel correct for gas";

/// Every process parameter the engine reads. The defaults are the dialog's
/// (GaiaDepthGradeDialog.js; plan D9); the engine-only fields keep config.py's.
struct GradeSettings {
    // Look (live): the render.
    double brightness = 0.65, size = 0.85, contrast = 0.12, saturation = 0.4;
    double halo_gain = 1.0;
    double halo_sigma = 2.0;  // base_sigma_px
    double p_low = 5.0, p_high = 95.0;
    bool nebula_enabled = false;
    double nebula_strength = 1.0, nebula_atmos = 1.0, nebula_structure = 1.0;
    double neutral_strength = 0.0;
    int halo_min_refs = 8;
    double halo_radius_sigma = 8.0, halo_moffat_beta = 2.5;
    // Matching (prepare-time).
    double detect_sigma = 6.0, detect_fwhm = 3.0;
    double match_tolerance = 4.0, min_match_rate = 0.3;
    bool online = false;  // false: local Gaia DR3 (the dialog's Offline box, on)
    bool mag_limit_enabled = true;
    double mag_limit = 18.0;
    double parallax_snr = 5.0;
};

/// The engine configuration for these settings: every field reaches GradeConfig (F43);
/// mag_limit_enabled = false is Python's gaia_mag_limit = None.
GradeConfig grade_config(const GradeSettings& s);

/// The nebula layer is graded only when it is enabled and its strength is positive (the
/// dialog's renderArgs: `nebEnabled && nebStrength > 0`; cli._graded_nebula: strength <= 0
/// returns the starless layer as is).
bool nebula_active(const GradeSettings& s);

/// True when two settings differ in a field Prepare depends on (detection, matching, the
/// catalogue): a cached Prepare is stale and must be run again. Render-only fields never are.
bool prepare_settings_differ(const GradeSettings& a, const GradeSettings& b);

/// A catalogue fetched before prepare_grade runs (the module fetches it on the thread the
/// network and database objects live on, then runs the PCL-free stages on a worker; plan C14).
/// distances_for() returns it for exactly the footprint it was fetched for, and throws
/// std::logic_error for any other (a different field would be a programming error).
class FixedCatalogSource : public CatalogSource {
public:
    FixedCatalogSource(const Footprint& fp, Catalog catalog) : fp_(fp), catalog_(std::move(catalog)) {}
    Catalog distances_for(const Footprint& footprint) override;

private:
    Footprint fp_;
    Catalog catalog_;
};

/// The recombined image: screen_blend(render_from_prep(stars), nebula_active ?
/// render_nebula(starless, table, strength, atmos, structure) : starless). The stars and
/// starless layers must have the same shape (std::invalid_argument otherwise).
Image grade_composite(const Image& stars, const Image& starless, const PrepResult& prep, const GradeSettings& s,
                      const Log& log);

/// The dialog's 1:1 inset rectangle (insetRegion): `size` square about (cx, cy), clamped so it
/// starts inside the image (it may extend past the far edge of an image smaller than `size`;
/// the crop clamps it, as numpy slicing does).
struct InsetRegion {
    int x = 0, y = 0, w = 0, h = 0;
};
InsetRegion inset_region(int width, int height, int cx, int cy, int size = 256);

/// The inset centre the dialog starts with: (Math.round(W/2), Math.round(H/2)).
void default_inset_center(int width, int height, int& cx, int& cy);

/// The full-resolution point a click at (px, py) on the preview picks: Math.round(p * scale),
/// scale = full_width / preview_width (the dialog's onMousePress; one scale for both axes).
void inset_center_from_click(int px, int py, int full_width, int preview_width, int& cx, int& cy);

/// The preview pixels: autostretch_u8(composite) (target 0.25, shadows -2.8), the full view
/// downscaled to fit inside max_width x max_height (fit_size: aspect kept, never upscaled; with
/// max_height < 1 only the width is bounded, which is the sidecar's preview command), and the
/// inset cropped 1:1 from the full-resolution stretch. Throws std::invalid_argument on an empty
/// composite.
struct PreviewPixels {
    int width = 0, height = 0, channels = 1;  // the downscaled full view
    std::vector<uint8_t> full;
    int inset_width = 0, inset_height = 0;
    std::vector<uint8_t> inset;
};
PreviewPixels preview_pixels(const Image& composite, int max_width, int max_height, const InsetRegion& inset);
/// preview_pixels with no height bound (the sidecar's --max-width only).
PreviewPixels preview_pixels(const Image& composite, int max_width, const InsetRegion& inset);

/// The dialog's QA status line, verbatim: "Prepared: %d/%d matched (rate %.2f, offset %.1f px)",
/// plus "  ⚠ LOW MATCH — depth grade unreliable" when the rate is below min_match_rate.
std::string qa_summary(const QA& qa);

}  // namespace gdg
