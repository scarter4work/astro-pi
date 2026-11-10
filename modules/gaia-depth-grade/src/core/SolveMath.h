// Plate-solver numerics (plan Tasks 14-15), PCL-free: the parts of ImageSolver 6.6
// (/opt/PixInsight/src/scripts/ImageSolver/ImageSolverEngine.js and
// /opt/PixInsight/include/pjsr/astrometry/AstrometricMetadata.js, PCL License 2.0) that are plain
// arithmetic, so they are unit-tested without PixInsight. The module's Solver drives them with
// PCL's star detector, Gaia database, StarAlignment and spline world transformation.
//
// Ported from ImageSolver 6.6.0: Copyright (c) 2013-2026 Andres del Pozo, Copyright (c) 2019-2026
// Juan Conejero (PTeam); PixInsight Class Library License Version 2.0.
#pragma once

#include <functional>
#include <optional>
#include <vector>

namespace gdg {

/// Initial centre and scale of a seeded solve. ra/dec in degrees; scale_deg in degrees per pixel.
struct Seed {
    std::optional<double> ra, dec, focal_mm, pixel_um, scale_deg;
};

/// AstrometricMetadata.ResolutionFromFocal: 0.001 * deg(pixel_um / focal_mm) degrees per pixel,
/// zero when focal_mm <= 0.
double resolution_from_focal(double pixel_um, double focal_mm);

/// The acquisition-metadata branch of AstrometricMetadata.ExtractMetadata (no astrometric
/// solution): ra/dec from OBJCTRA/OBJCTDEC (decoded to degrees by the caller), focal length from
/// FOCALLEN when > 0, and the scale from focal length and pixel size when both are > 0.
Seed seed_from_metadata(std::optional<double> ra_deg, std::optional<double> dec_deg,
                        std::optional<double> focal_mm, std::optional<double> pixel_um);

/// ImageSolver.hasValidSeed: finite ra in [0,360], dec in [-90,90], and 0 < scale <= 0.5 deg/px.
bool seed_is_valid(const Seed& s);

/// WCSKeywords.Read's observation time (JD, UTC): none without a start time; the end is DATE-END
/// (start and end swapped when reversed), else start + exposure; the middle of start and end
/// when there is an end, else the start.
std::optional<double> observation_jd(std::optional<double> start_jd, std::optional<double> end_jd,
                                     std::optional<double> exposure_s);

/// ImageSolver.findLimitMagnitudeAndCatalog's empirical start value for a field of view of
/// fov_deg (the largest image side in degrees): 14.5 * fov^-0.179 clamped to [7, 20] and
/// rounded to 0.01 mag.
double auto_limit_magnitude(double fov_deg);

/// The adaptive magnitude-limit search against a local Gaia database: bisection from `m`
/// between 7 and `m_db_high` until count(m) is within [target, 1.05 * target], an interval
/// narrower than 0.05 mag, or 100 iterations. `count(m)` is the number of catalogue stars of
/// magnitude <= m in the field (capped at 1.5 * target is fine).
double optimal_limit_magnitude(double m, double m_db_high, long target, const std::function<long(double)>& count);

/// detectImageStars step 5: max(2, trunc(0.75 * (d - 2))) for the smallest PSF box side d.
int star_matching_tolerance(double min_psf_box_side);

/// ImageSolver.blindBinningFactor: min(trunc(fwhm/3), 8, trunc(min(w,h)/400)), or 1 below 2.
int blind_binning_factor(double fwhm, int width, int height);

/// ImageSolver.medianBrightFWHM: the median (upper middle element) FWHM of the 200 stars of
/// largest signal; zero for none. Inputs are parallel arrays.
double median_bright_fwhm(const std::vector<double>& signal, const std::vector<double>& fwhm);

/// The score of a star matching: round(numValid / (1 + rms), 3 decimals).
double match_score(int num_valid, double rms);

/// An image position (pixels, PixInsight image coordinates).
struct Pt {
    double x = 0, y = 0;
};

/// An axis-aligned closed rectangle.
struct Box {
    double x0, y0, x1, y1;
};

/// The search semantics of PJSR's BRQuadTree (pjsr/BRQuadTree.jsh), which ImageSolver's star
/// tree uses: an object is found by a query rectangle when their closed rectangles intersect.
/// A uniform grid replaces the quadtree; the results are the same sets.
class RectIndex {
public:
    explicit RectIndex(std::vector<Box> objects);
    /// Indices of the objects whose rectangle intersects `q`, in increasing order.
    std::vector<int> search(const Box& q) const;
    /// How many objects intersect `q`.
    int count(const Box& q) const;
    size_t size() const { return obj_.size(); }
    const Box& object(int i) const { return obj_[static_cast<size_t>(i)]; }

private:
    std::vector<Box> obj_;
    double gx0_ = 0, gy0_ = 0, cell_ = 1;
    int nx_ = 1, ny_ = 1;
    std::vector<std::vector<int>> cells_;
    template <typename F>
    void visit(const Box& q, F&& f) const;
};

/// detectImageStars step 3: keeps the stars whose rectangle is the only one intersecting the box
/// (x - 1, y - 1, x + 1, y + 1) around their own position. Returns the kept indices in order.
std::vector<int> remove_conflicting_sources(const std::vector<Pt>& pos, const std::vector<Box>& rect);

}  // namespace gdg
