#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/io/image.hpp"

namespace nukex {

/// Detect stars in an image via local maxima detection + Gaussian centroid refinement.
///
/// Process:
/// 1. Compute background level and noise (median + MAD)
/// 2. Find local maxima above SNR threshold (median + snr_multiplier * MAD)
/// 3. Refine centroid with 2D Gaussian fit on 7x7 neighborhood
/// 4. Compute flux, peak, SNR for each star
/// 5. Sort by flux, keep top max_stars
///
/// Input should be a single-channel (luminance) image. For RGB, extract
/// or compute luminance first.
class StarDetector {
public:
    struct Config {
        float snr_multiplier = 5.0f;   // detection threshold: median + snr_mult * MAD
        int   max_stars      = 200;    // keep top N brightest
        int   exclusion_radius = 5;    // minimum distance between detected stars (pixels)
        float saturation_level = 0.95f; // reject stars with peak above this
        float saturation_reject_fraction = 0.5f; // reject whole frame if this
                                                  // fraction of pixels is at
                                                  // saturation_level or above
    };

    /// Detect stars in a single-channel image.
    /// If image is multi-channel, only channel 0 is used.
    static StarCatalog detect(const Image& image, const Config& config);

    /// Detect stars using default configuration.
    static StarCatalog detect(const Image& image) { return detect(image, Config{}); }

    /// Fraction of pixels at or above `saturation_level`, measured on a 4x
    /// decimated sample of channel 0.  Callers use this to decide whether a
    /// frame is unusable before attempting expensive detection work.
    static float saturation_fraction(const Image& image, float saturation_level);

private:
    /// Compute robust background (median) and noise (MAD) of the image.
    static std::pair<float, float> compute_background_noise(const Image& image);

    /// Find local maxima above threshold. Returns (x, y, peak_value) triples.
    static std::vector<std::tuple<int, int, float>> find_local_maxima(
        const Image& image, float threshold, int exclusion_radius);

    /// Refine centroid with 2D Gaussian fit on a 7x7 neighborhood.
    /// Returns sub-pixel (x, y) or the input if fit fails.
    static std::pair<float, float> refine_centroid(
        const Image& image, int x, int y);

    /// Compute flux in a circular aperture of given radius.
    static float compute_flux(const Image& image, float cx, float cy,
                              float background, int aperture_radius = 5);
};

} // namespace nukex
