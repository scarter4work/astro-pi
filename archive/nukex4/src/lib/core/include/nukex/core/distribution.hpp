#pragma once

#include <cstdint>

namespace nukex {

enum class DistributionShape : uint8_t {
    GAUSSIAN      = 0,   // Student-t with ν > 30 (recovered Gaussian)
    BIMODAL       = 1,   // 2-component GMM, both components significant
    HEAVY_TAILED  = 2,   // Student-t with ν ≤ 30
    CONTAMINATED  = 3,   // Gaussian + uniform contamination model
    SPIKE_OUTLIER = 4,   // GMM with one tiny component (π > 0.95)
    UNIFORM       = 5,   // KDE fallback (non-parametric)
    UNKNOWN       = 6    // All fits failed
};

inline const char* distribution_shape_name(DistributionShape shape) {
    switch (shape) {
        case DistributionShape::GAUSSIAN:      return "GAUSSIAN";
        case DistributionShape::BIMODAL:       return "BIMODAL";
        case DistributionShape::HEAVY_TAILED:  return "HEAVY_TAILED";
        case DistributionShape::CONTAMINATED:  return "CONTAMINATED";
        case DistributionShape::SPIKE_OUTLIER: return "SPIKE_OUTLIER";
        case DistributionShape::UNIFORM:       return "UNIFORM";
        case DistributionShape::UNKNOWN:       return "UNKNOWN";
    }
    return "UNKNOWN";
}

/// Student-t location-scale family. When ν > 30, effectively Gaussian.
/// Reference: Lange, Little & Taylor (1989), JASA 84(408), 881-896.
struct StudentTParams {
    float mu    = 0.0f;   // Location parameter
    float sigma = 0.0f;   // Scale parameter
    float nu    = 0.0f;   // Degrees of freedom
};

/// Gaussian parameters for mixture model components.
struct GaussianParams {
    float mu        = 0.0f;
    float sigma     = 0.0f;
    float amplitude = 0.0f;
};

/// Two-component Gaussian mixture. Fitted via EM algorithm.
/// Reference: Dempster, Laird & Rubin (1977), JRSS-B 39(1), 1-38.
struct BimodalParams {
    GaussianParams comp1;
    GaussianParams comp2;
    float          mixing_ratio = 0.0f;  // Weight of comp1 (comp2 = 1 - mixing_ratio)
};

/// Gaussian signal + uniform contamination.
/// Reference: Hogg, Bovy & Lang (2010), arXiv:1008.4686.
struct ContaminationParams {
    float mu                 = 0.0f;  // Clean signal location
    float sigma              = 0.0f;  // Clean signal scale
    float contamination_frac = 0.0f;  // ε: fraction of outlier samples
};

struct ZDistribution {
    DistributionShape shape = DistributionShape::UNKNOWN;

    union {
        StudentTParams      student_t;
        BimodalParams       bimodal;
        ContaminationParams contamination;
    } params = {};

    float r_squared = 0.0f;
    float aicc      = 0.0f;

    float true_signal_estimate = 0.0f;
    float signal_uncertainty   = 0.0f;
    float confidence           = 0.0f;

    float kde_mode      = 0.0f;
    float kde_bandwidth = 0.0f;
    bool  used_nonparametric = false;
};

} // namespace nukex
