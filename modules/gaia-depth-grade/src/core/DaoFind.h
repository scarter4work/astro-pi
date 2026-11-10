// DAOFIND star detection: an exact port of photutils 3.0.0 DAOStarFinder (PCL-free, reentrant).
//
// Ported from photutils 3.0.0 (detection/daofinder.py, detection/core.py, detection/peakfinder.py,
// utils/_convolution.py, utils/cutouts.py). Copyright (c) 2011-2026, Photutils Developers. All
// rights reserved. Used under the 3-clause BSD licence; the full notice is in DaoFind.cpp and in
// modules/gaia-depth-grade/THIRD_PARTY_NOTICES.
#pragma once

#include "Image.h"

#include <limits>
#include <vector>

namespace gdg {

/// DAOStarFinder's constructor arguments. A range set to (-inf, +inf) is photutils' `None` (no
/// filtering); a negative min_separation is photutils' `None` (2.5 * fwhm); 0 uses the kernel
/// footprint for peak finding, as photutils does.
struct DaoParams {
    double threshold = 0.0;
    double fwhm = 3.0;
    double ratio = 1.0, theta = 0.0, sigma_radius = 1.5;
    double sharp_lo = 0.2, sharp_hi = 1.0, round_lo = -1.0, round_hi = 1.0;
    double min_separation = -1.0;
    bool exclude_border = false;
    bool scale_threshold = true;

    static constexpr double kNoLimit = std::numeric_limits<double>::infinity();
};

/// One row of DAOStarFinder's output table (x = x_centroid, y = y_centroid).
struct DaoStar {
    double x, y, flux, peak, sharpness, roundness1, roundness2;
};

/// photutils' _StarFinderKernel (normalize_zerosum=True). Arrays are (ny, nx) C order.
struct DaoKernel {
    int nx = 0, ny = 0, x_radius = 0, y_radius = 0;
    double x_sigma = 0.0, y_sigma = 0.0, rel_err = 0.0;
    long n_pixels = 0;
    std::vector<double> data;      // zero-sum density-enhancement kernel (masked)
    std::vector<double> unmasked;  // gaussian_kernel_unmasked, peak 1.0
    std::vector<int> mask;         // 1 inside the footprint
};

/// Builds the kernel exactly as photutils does. Throws std::invalid_argument on fwhm <= 0,
/// ratio outside (0, 1] or sigma_radius <= 0 (photutils' ValueErrors).
DaoKernel dao_kernel(double fwhm, double ratio = 1.0, double theta = 0.0, double sigma_radius = 1.5);

/// scipy.ndimage.convolve(data, kernel.data, mode='constant', cval=0.0) -- photutils'
/// _filter_data. Exposed for the parity tests.
Image dao_convolve(const Image& mono, const DaoKernel& kernel);

/// DAOStarFinder(...)(mono): peaks of the convolved image above threshold (scaled by the
/// kernel's rel_err unless scale_threshold is false), the DAOFIND marginal-fit centroids,
/// sharpness and roundness, the finite and range filters. Rows are in photutils' order (peak
/// row-major order). An empty vector is photutils' `None`. Throws std::invalid_argument for a
/// multi-channel image and for the kernel's argument errors.
std::vector<DaoStar> dao_find(const Image& mono, const DaoParams& params);

}  // namespace gdg
