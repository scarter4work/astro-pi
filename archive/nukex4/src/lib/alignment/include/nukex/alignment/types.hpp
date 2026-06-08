#pragma once

#include <vector>
#include <cstdint>
#include <array>
#include <cmath>

namespace nukex {

/// A detected star with sub-pixel centroid and photometric properties.
struct Star {
    float x          = 0.0f;   // sub-pixel centroid X
    float y          = 0.0f;   // sub-pixel centroid Y
    float flux       = 0.0f;   // integrated flux (sum of pixel values in aperture)
    float peak       = 0.0f;   // peak pixel value
    float background = 0.0f;   // local background level
    float snr        = 0.0f;   // peak / background SNR

    // Moffat PSF parameters (filled by PSF fitter)
    float fwhm_x     = 0.0f;   // FWHM along semi-major axis
    float fwhm_y     = 0.0f;   // FWHM along semi-minor axis
    float fwhm       = 0.0f;   // geometric mean FWHM
    float eccentricity = 0.0f; // 1 - min(axis)/max(axis), [0=round, 1=line]
    float moffat_beta = 2.5f;  // Moffat β parameter
    bool  psf_valid   = false;  // true if Moffat fit succeeded
};

/// Collection of detected stars in a single frame.
struct StarCatalog {
    std::vector<Star> stars;

    /// Sort stars by flux (brightest first).
    void sort_by_flux();

    /// Keep only the top N brightest stars.
    void keep_top(int n);

    /// Number of stars.
    int size() const { return static_cast<int>(stars.size()); }

    /// Is catalog empty?
    bool empty() const { return stars.empty(); }
};

/// 3x3 projective homography matrix stored row-major.
/// Maps points from source frame to reference frame:
///   [x'] = H * [x]
///   [y']       [y]
///   [w']       [1]
/// Normalized so H[2][2] = 1.
struct HomographyMatrix {
    std::array<std::array<float, 3>, 3> H = {{{1,0,0}, {0,1,0}, {0,0,1}}};

    /// Access element (row, col).
    float& operator()(int row, int col) { return H[row][col]; }
    float  operator()(int row, int col) const { return H[row][col]; }

    /// Apply homography to a point. Returns (x', y').
    std::pair<float, float> transform(float x, float y) const {
        float w = H[2][0] * x + H[2][1] * y + H[2][2];
        if (std::abs(w) < 1e-10f) w = 1e-10f;
        float xp = (H[0][0] * x + H[0][1] * y + H[0][2]) / w;
        float yp = (H[1][0] * x + H[1][1] * y + H[1][2]) / w;
        return {xp, yp};
    }

    /// Check if this is approximately identity.
    bool is_identity(float tolerance = 1e-6f) const;

    /// Extract rotation angle in degrees from the homography.
    float rotation_degrees() const;

    /// Check if this represents a meridian flip (rotation ≈ 180°).
    bool is_meridian_flip(float angle_tolerance_deg = 10.0f) const;

    /// Return the identity matrix.
    static HomographyMatrix identity();
};

/// Result of a star matching attempt between two catalogs.
struct MatchResult {
    std::vector<std::pair<int, int>> matches;  // (source_idx, ref_idx) pairs
    int n_inliers = 0;
    float rms_error = 0.0f;
    bool success = false;
};

/// Result of frame alignment.
struct AlignmentResult {
    HomographyMatrix H;
    MatchResult match;
    bool is_meridian_flipped = false;
    bool alignment_failed = false;   // true if <8 matches found
    float weight_penalty = 1.0f;     // 0.5 if alignment failed
};

} // namespace nukex
