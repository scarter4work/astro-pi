// The recon `reconstruct` pipeline, channels -> CavityModel (recon Part 2, native; PCL-free).
// One reentrant call that a PixInsight process drives: no global state, no stretching of data.
#pragma once

#include "Cavity.h"
#include "Ionizer.h"
#include "Model.h"
#include "core/Detect.h"
#include "core/Log.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gdg::recon {

/// Printed on every recon output (FITS RECONTAG, GLB asset.extras.honesty, docs).
inline constexpr const char* RECON_HONESTY = "Reconstructed HII cavity; physically motivated, not measured gas depth";

/// The `degraded` value and warning for an input that looks stretched.
inline constexpr const char* RECON_DEGRADED_STRETCHED = "stretched-input";
inline constexpr const char* RECON_STRETCHED_WARNING =
    "recon: input looks stretched (non-linear); ratio term off, geometry prior only. The result is not "
    "physically constrained.";

/// channel_mean(img): a mono image passes through; a colour image becomes its per-pixel channel
/// mean (numpy img.mean(axis=2)) rounded to float32. An OSC duoband master's total signal is
/// Ha + [O III], which is what from_duoband expects.
Image channel_mean(const Image& img);

struct ReconInputs {
    std::string mode;                      ///< "sho", "duoband" or "osc", as extract_channels
    std::map<std::string, Image> channels; ///< the keys extract_channels reads for `mode`
    std::optional<Image> hb;               ///< optional H-beta plane: adds the Balmer-decrement dust
    std::vector<StarRow> stars;            ///< matched stars, for the ionizer pick
    std::optional<Ionizer> ionizer_override;
    /// Degrees per pixel at (x, y); null = no astrometry. May return nullopt.
    std::function<std::optional<double>(double x, double y)> deg_per_px_at;
};

struct ReconResult {
    CavityModel model;
    IonizationMap ionization;
    Ionizer ionizer;
    Image lum;
    std::string degraded;  ///< "" or RECON_DEGRADED_STRETCHED
};

/// Throws std::invalid_argument for an unknown mode, missing keys, or planes (hb included) of
/// different (ny, nx).
ReconResult reconstruct(const ReconInputs& in, const ReconConfig& cfg, const Log& log);

}  // namespace gdg::recon
