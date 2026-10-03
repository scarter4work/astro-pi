#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/io/image.hpp"

namespace nukex {

/// The channel star detection and channel registration both use as their
/// reference: green for a colour image, the only channel for a mono one.
///
/// Free rather than a member because channel registration needs the same
/// answer and must not depend on StarDetector to get it.
inline int default_reference_channel(int n_channels) {
    return n_channels >= 3 ? 1 : 0;
}

/// Resolve a StarDetector::Config::channel value against an image.
///
/// Star detection and channel registration MUST agree on this channel: the
/// frame-to-frame homography is fitted from centroids on the detection
/// channel, and channel registration measures every other channel against
/// its reference. Deriving them from two separate expressions let a caller
/// who set Config::channel silently pull them apart -- detection on red,
/// registration referenced to green -- with no test able to see it. One
/// definition, used by both.
inline int resolve_detection_channel(int configured, int n_channels) {
    return (configured >= 0 && configured < n_channels)
         ? configured
         : default_reference_channel(n_channels);
}

/// Detect stars in an image via local maxima detection + Gaussian centroid refinement.
///
/// Process:
/// 1. Compute background level and noise (median + MAD)
/// 2. Find local maxima above SNR threshold (median + snr_multiplier * MAD)
/// 3. Refine centroid with 2D Gaussian fit on 7x7 neighborhood
/// 4. Compute flux, peak, SNR for each star
/// 5. Sort by flux, keep top max_stars
///
/// Works on single-channel (mono) or multi-channel (colour) images. For a
/// colour image, detection runs on green by default -- see Config::channel.
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

        /// Which channel to detect stars on. -1 means auto: green (channel 1)
        /// for any image with 3 or more channels, channel 0 otherwise.
        ///
        /// Green is the right default for a colour frame. It has two of every
        /// four photosites on an RGGB sensor, so its centroids are the least
        /// noisy available, and through a multi-band filter it is not the
        /// starved channel. Detecting on channel 0 -- red, after debayer --
        /// registers frames using the channel that carries the lateral-colour
        /// error, which is exactly backwards.
        int channel = -1;
    };

    /// Detect stars in an image. The channel used is Config::channel, or the
    /// auto choice from default_reference_channel() when it is left at -1.
    static StarCatalog detect(const Image& image, const Config& config);

    /// Detect stars using default configuration.
    static StarCatalog detect(const Image& image) { return detect(image, Config{}); }

    /// Fraction of pixels at or above `saturation_level`, measured on a 4x
    /// decimated sample of channel 0.  Callers use this to decide whether a
    /// frame is unusable before attempting expensive detection work.
    static float saturation_fraction(const Image& image, float saturation_level);

private:
    /// Compute robust background (median) and noise (MAD) of the image.
    static std::pair<float, float> compute_background_noise(const Image& image, int ch);

    /// Find local maxima above threshold. Returns (x, y, peak_value) triples.
    static std::vector<std::tuple<int, int, float>> find_local_maxima(
        const Image& image, float threshold, int exclusion_radius, int ch);

    /// Refine centroid with 2D Gaussian fit on a 7x7 neighborhood.
    /// Returns sub-pixel (x, y) or the input if fit fails.
    static std::pair<float, float> refine_centroid(
        const Image& image, int x, int y, int ch);

    /// Compute flux in a circular aperture of given radius.
    static float compute_flux(const Image& image, float cx, float cy,
                              float background, int ch, int aperture_radius = 5);
};

} // namespace nukex
