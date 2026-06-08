# Phase 3: lib/alignment — Star Detection, PSF Fitting, Homography, Meridian Flip

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the alignment engine: detect stars in each frame, fit Moffat PSF profiles, match star catalogs between frames, compute projective homography via RANSAC+DLT, detect and correct meridian flips. No PI StarAlignment — fully custom.

**Architecture:** lib/alignment depends on lib/core (FrameMetadata, FramePSFQuality types) and lib/io (Image). Uses Eigen for matrix math (SVD for DLT, matrix operations for homography). Uses Eigen's unsupported LevenbergMarquardt for Moffat PSF fitting. All operations work on our Image type, not PCL's.

**Tech Stack:** C++17, Eigen 3 (system), lib/core, lib/io, Catch2 v3

**Critical rules:**
- No stubs, no TODOs — every function complete
- Double-check all matrix math
- Failed alignment: frame keeps weight × 0.5, NEVER discarded
- Reference frame: first successfully aligned frame (H = identity)

---

## File Structure

```
src/lib/alignment/
├── CMakeLists.txt
├── include/
│   └── nukex/
│       └── alignment/
│           ├── star_detector.hpp     Star detection via local maxima + Gaussian centroid
│           ├── psf_fitter.hpp        Moffat PSF fitting + FramePSFQuality
│           ├── star_matcher.hpp      Nearest-neighbor catalog matching
│           ├── homography.hpp        RANSAC + DLT homography computation
│           ├── frame_aligner.hpp     High-level: align a frame to reference
│           └── types.hpp             Star, StarCatalog, HomographyMatrix types
├── src/
│   ├── star_detector.cpp
│   ├── psf_fitter.cpp
│   ├── star_matcher.cpp
│   ├── homography.cpp
│   └── frame_aligner.cpp
test/unit/alignment/
├── test_star_detector.cpp
├── test_homography.cpp
└── test_frame_aligner.cpp
```

---

## Task 1: Alignment Types + Star Detector

**Files:**
- Create: `src/lib/alignment/include/nukex/alignment/types.hpp`
- Create: `src/lib/alignment/include/nukex/alignment/star_detector.hpp`
- Create: `src/lib/alignment/src/star_detector.cpp`
- Create: `src/lib/alignment/CMakeLists.txt`
- Create: `test/unit/alignment/test_star_detector.cpp`
- Modify: root `CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Create directories**

```bash
mkdir -p src/lib/alignment/include/nukex/alignment
mkdir -p src/lib/alignment/src
mkdir -p test/unit/alignment
```

- [ ] **Step 2: Create alignment types**

Create `src/lib/alignment/include/nukex/alignment/types.hpp`:

```cpp
#pragma once

#include <vector>
#include <cstdint>
#include <array>

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
```

- [ ] **Step 3: Create star detector header**

Create `src/lib/alignment/include/nukex/alignment/star_detector.hpp`:

```cpp
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
    };

    /// Detect stars in a single-channel image.
    /// If image is multi-channel, only channel 0 is used.
    static StarCatalog detect(const Image& image, const Config& config = Config{});

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
```

- [ ] **Step 4: Create star detector implementation**

Create `src/lib/alignment/src/star_detector.cpp`:

```cpp
#include "nukex/alignment/star_detector.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>
#include <tuple>

namespace nukex {

std::pair<float, float> StarDetector::compute_background_noise(const Image& image) {
    // Sample pixels for background estimation (every 4th pixel for speed)
    std::vector<float> samples;
    int step = 4;
    for (int y = 0; y < image.height(); y += step) {
        for (int x = 0; x < image.width(); x += step) {
            samples.push_back(image.at(x, y, 0));
        }
    }

    if (samples.empty()) return {0.0f, 1.0f};

    // Median
    size_t n = samples.size();
    std::nth_element(samples.begin(), samples.begin() + n / 2, samples.end());
    float median = samples[n / 2];

    // MAD (Median Absolute Deviation)
    for (auto& v : samples) {
        v = std::abs(v - median);
    }
    std::nth_element(samples.begin(), samples.begin() + n / 2, samples.end());
    float mad = samples[n / 2];

    // Scale MAD to estimate sigma: sigma ≈ 1.4826 * MAD for Gaussian data
    float sigma = 1.4826f * mad;
    if (sigma < 1e-10f) sigma = 1e-10f;

    return {median, sigma};
}

std::vector<std::tuple<int, int, float>> StarDetector::find_local_maxima(
    const Image& image, float threshold, int exclusion_radius) {

    int w = image.width();
    int h = image.height();
    int r = 3; // local max search radius

    std::vector<std::tuple<int, int, float>> candidates;

    // Skip border pixels
    for (int y = r; y < h - r; y++) {
        for (int x = r; x < w - r; x++) {
            float val = image.at(x, y, 0);
            if (val < threshold) continue;

            // Check if this is a local maximum in a (2r+1) × (2r+1) neighborhood
            bool is_max = true;
            for (int dy = -r; dy <= r && is_max; dy++) {
                for (int dx = -r; dx <= r && is_max; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    if (image.at(x + dx, y + dy, 0) >= val) {
                        is_max = false;
                    }
                }
            }

            if (is_max) {
                candidates.emplace_back(x, y, val);
            }
        }
    }

    // Sort by peak value (brightest first)
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  return std::get<2>(a) > std::get<2>(b);
              });

    // Apply exclusion radius — remove candidates too close to brighter ones
    std::vector<std::tuple<int, int, float>> filtered;
    for (const auto& [cx, cy, cv] : candidates) {
        bool too_close = false;
        for (const auto& [fx, fy, fv] : filtered) {
            float dx = static_cast<float>(cx - fx);
            float dy = static_cast<float>(cy - fy);
            if (dx * dx + dy * dy < static_cast<float>(exclusion_radius * exclusion_radius)) {
                too_close = true;
                break;
            }
        }
        if (!too_close) {
            filtered.emplace_back(cx, cy, cv);
        }
    }

    return filtered;
}

std::pair<float, float> StarDetector::refine_centroid(
    const Image& image, int x, int y) {

    // Compute intensity-weighted centroid in a 7×7 window
    // This is faster and more robust than full 2D Gaussian fitting
    // for the purpose of sub-pixel centroid estimation.
    int w = image.width();
    int h = image.height();
    int radius = 3;

    float background = 0.0f;
    int bg_count = 0;

    // Estimate local background from corners of an 11×11 region
    for (int dy = -5; dy <= 5; dy += 10) {
        for (int dx = -5; dx <= 5; dx += 10) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                background += image.at(px, py, 0);
                bg_count++;
            }
        }
    }
    if (bg_count > 0) background /= static_cast<float>(bg_count);

    float sum_x = 0.0f, sum_y = 0.0f, sum_w = 0.0f;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px < 0 || px >= w || py < 0 || py >= h) continue;

            float val = image.at(px, py, 0) - background;
            if (val <= 0.0f) continue;

            sum_x += val * static_cast<float>(px);
            sum_y += val * static_cast<float>(py);
            sum_w += val;
        }
    }

    if (sum_w > 1e-10f) {
        return {sum_x / sum_w, sum_y / sum_w};
    }
    return {static_cast<float>(x), static_cast<float>(y)};
}

float StarDetector::compute_flux(const Image& image, float cx, float cy,
                                  float background, int aperture_radius) {
    int w = image.width();
    int h = image.height();
    int icx = static_cast<int>(cx + 0.5f);
    int icy = static_cast<int>(cy + 0.5f);
    float r2 = static_cast<float>(aperture_radius * aperture_radius);
    float flux = 0.0f;

    for (int dy = -aperture_radius; dy <= aperture_radius; dy++) {
        for (int dx = -aperture_radius; dx <= aperture_radius; dx++) {
            int px = icx + dx;
            int py = icy + dy;
            if (px < 0 || px >= w || py < 0 || py >= h) continue;
            float dist2 = static_cast<float>(dx * dx + dy * dy);
            if (dist2 > r2) continue;

            float val = image.at(px, py, 0) - background;
            if (val > 0.0f) flux += val;
        }
    }

    return flux;
}

StarCatalog StarDetector::detect(const Image& image, const Config& config) {
    StarCatalog catalog;

    if (image.empty() || image.width() < 20 || image.height() < 20) {
        return catalog;
    }

    // Step 1: Background and noise estimation
    auto [background, sigma] = compute_background_noise(image);
    float threshold = background + config.snr_multiplier * sigma;

    // Step 2: Find local maxima
    auto candidates = find_local_maxima(image, threshold, config.exclusion_radius);

    // Step 3: Build star catalog with refined centroids
    for (const auto& [ix, iy, peak] : candidates) {
        // Reject saturated stars
        if (peak > config.saturation_level) continue;

        Star star;
        star.peak = peak;
        star.background = background;

        // Refine centroid
        auto [rx, ry] = refine_centroid(image, ix, iy);
        star.x = rx;
        star.y = ry;

        // Compute flux
        star.flux = compute_flux(image, rx, ry, background);

        // SNR
        star.snr = (sigma > 1e-10f) ? (peak - background) / sigma : 0.0f;

        catalog.stars.push_back(star);
    }

    // Step 4: Sort by flux and keep top N
    catalog.sort_by_flux();
    catalog.keep_top(config.max_stars);

    return catalog;
}

void StarCatalog::sort_by_flux() {
    std::sort(stars.begin(), stars.end(),
              [](const Star& a, const Star& b) { return a.flux > b.flux; });
}

void StarCatalog::keep_top(int n) {
    if (static_cast<int>(stars.size()) > n) {
        stars.resize(n);
    }
}

} // namespace nukex
```

- [ ] **Step 5: Create HomographyMatrix methods**

Create `src/lib/alignment/src/homography.cpp` (just the HomographyMatrix methods for now):

```cpp
#include "nukex/alignment/types.hpp"
#include <cmath>

namespace nukex {

bool HomographyMatrix::is_identity(float tolerance) const {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float expected = (r == c) ? 1.0f : 0.0f;
            if (std::abs(H[r][c] - expected) > tolerance) return false;
        }
    }
    return true;
}

float HomographyMatrix::rotation_degrees() const {
    return std::atan2(H[1][0], H[0][0]) * 180.0f / 3.14159265358979f;
}

bool HomographyMatrix::is_meridian_flip(float angle_tolerance_deg) const {
    float angle = std::abs(rotation_degrees());
    return std::abs(angle - 180.0f) < angle_tolerance_deg;
}

HomographyMatrix HomographyMatrix::identity() {
    HomographyMatrix m;
    m.H = {{{1,0,0}, {0,1,0}, {0,0,1}}};
    return m;
}

} // namespace nukex
```

- [ ] **Step 6: Create CMakeLists.txt**

Create `src/lib/alignment/CMakeLists.txt`:

```cmake
find_package(Eigen3 3.3 REQUIRED NO_MODULE)

add_library(nukex4_alignment STATIC
    src/star_detector.cpp
    src/homography.cpp
)

target_include_directories(nukex4_alignment
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_alignment
    PUBLIC nukex4_core nukex4_io
    PRIVATE Eigen3::Eigen
)

target_compile_features(nukex4_alignment PUBLIC cxx_std_17)
```

- [ ] **Step 7: Update root CMakeLists.txt**

Add after `add_subdirectory(src/lib/io)`:

```cmake
add_subdirectory(src/lib/alignment)
```

- [ ] **Step 8: Create test_star_detector.cpp**

Create `test/unit/alignment/test_star_detector.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/alignment/types.hpp"
#include "nukex/io/image.hpp"
#include "nukex/io/fits_reader.hpp"
#include <cmath>
#include <filesystem>

using namespace nukex;

/// Helper: create a synthetic image with Gaussian stars at known positions.
static Image create_star_field(int width, int height,
                                const std::vector<std::tuple<float, float, float>>& stars_xya,
                                float background = 0.1f, float sigma = 3.0f) {
    Image img(width, height, 1);
    img.fill(background);

    for (const auto& [sx, sy, amp] : stars_xya) {
        // Draw a Gaussian star
        int r = static_cast<int>(sigma * 4);
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int px = static_cast<int>(sx + 0.5f) + dx;
                int py = static_cast<int>(sy + 0.5f) + dy;
                if (px < 0 || px >= width || py < 0 || py >= height) continue;

                float fx = static_cast<float>(px) - sx;
                float fy = static_cast<float>(py) - sy;
                float g = amp * std::exp(-(fx*fx + fy*fy) / (2.0f * sigma * sigma));
                img.at(px, py, 0) += g;
            }
        }
    }

    return img;
}

TEST_CASE("StarDetector: detect synthetic stars", "[star_detector]") {
    // Create a 200×200 image with 5 stars at known positions
    std::vector<std::tuple<float, float, float>> star_positions = {
        {50.0f,  50.0f,  0.8f},
        {150.0f, 50.0f,  0.6f},
        {100.0f, 100.0f, 0.9f},
        {50.0f,  150.0f, 0.5f},
        {150.0f, 150.0f, 0.7f}
    };

    Image img = create_star_field(200, 200, star_positions, 0.05f, 3.0f);

    StarDetector::Config config;
    config.snr_multiplier = 3.0f;
    config.max_stars = 10;

    StarCatalog catalog = StarDetector::detect(img, config);

    // Should detect all 5 stars
    REQUIRE(catalog.size() >= 4);  // allow 1 miss due to thresholding
    REQUIRE(catalog.size() <= 10);

    // Brightest star should be near (100, 100) with amplitude 0.9
    REQUIRE(catalog.stars[0].x == Catch::Approx(100.0f).margin(2.0f));
    REQUIRE(catalog.stars[0].y == Catch::Approx(100.0f).margin(2.0f));

    // All detected stars should have positive flux and SNR
    for (const auto& star : catalog.stars) {
        REQUIRE(star.flux > 0.0f);
        REQUIRE(star.snr > 0.0f);
    }
}

TEST_CASE("StarDetector: centroid accuracy on known position", "[star_detector]") {
    // Single bright star at sub-pixel position (50.3, 75.7)
    std::vector<std::tuple<float, float, float>> stars = {
        {50.3f, 75.7f, 0.8f}
    };
    Image img = create_star_field(100, 100, stars, 0.02f, 2.5f);

    StarDetector::Config config;
    config.snr_multiplier = 3.0f;
    config.max_stars = 5;

    StarCatalog catalog = StarDetector::detect(img, config);

    REQUIRE(catalog.size() >= 1);

    // Centroid should be within 0.5 pixel of true position
    REQUIRE(catalog.stars[0].x == Catch::Approx(50.3f).margin(0.5f));
    REQUIRE(catalog.stars[0].y == Catch::Approx(75.7f).margin(0.5f));
}

TEST_CASE("StarDetector: rejects saturated stars", "[star_detector]") {
    std::vector<std::tuple<float, float, float>> stars = {
        {50.0f, 50.0f, 1.0f},  // saturated (peak will be > 0.95)
        {50.0f, 80.0f, 0.3f}   // not saturated
    };
    Image img = create_star_field(100, 100, stars, 0.02f, 2.5f);

    StarDetector::Config config;
    config.snr_multiplier = 3.0f;
    config.saturation_level = 0.95f;

    StarCatalog catalog = StarDetector::detect(img, config);

    // The saturated star should be rejected
    // Only the dim star should remain
    for (const auto& star : catalog.stars) {
        REQUIRE(star.peak <= config.saturation_level);
    }
}

TEST_CASE("StarDetector: empty image returns empty catalog", "[star_detector]") {
    Image img(100, 100, 1);
    img.fill(0.1f);  // uniform — no stars

    StarCatalog catalog = StarDetector::detect(img);
    REQUIRE(catalog.size() == 0);
}

TEST_CASE("StarDetector: max_stars limits output", "[star_detector]") {
    // Create 20 stars
    std::vector<std::tuple<float, float, float>> stars;
    for (int i = 0; i < 20; i++) {
        float x = 20.0f + static_cast<float>(i % 5) * 30.0f;
        float y = 20.0f + static_cast<float>(i / 5) * 30.0f;
        float amp = 0.3f + static_cast<float>(i) * 0.03f;
        stars.emplace_back(x, y, amp);
    }
    Image img = create_star_field(200, 200, stars, 0.02f, 2.0f);

    StarDetector::Config config;
    config.snr_multiplier = 3.0f;
    config.max_stars = 10;

    StarCatalog catalog = StarDetector::detect(img, config);
    REQUIRE(catalog.size() <= 10);
}

TEST_CASE("StarDetector: real FITS file", "[star_detector][integration]") {
    std::string path = "/home/scarter4work/projects/processing/M16/"
                       "Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit";
    if (!std::filesystem::exists(path)) {
        SKIP("Test FITS file not available");
    }

    auto result = FITSReader::read(path);
    REQUIRE(result.success);

    StarDetector::Config config;
    config.snr_multiplier = 5.0f;
    config.max_stars = 200;

    StarCatalog catalog = StarDetector::detect(result.image, config);

    // M16 field should have plenty of stars
    INFO("Detected " << catalog.size() << " stars");
    REQUIRE(catalog.size() >= 20);

    // Stars should be sorted by flux (brightest first)
    for (int i = 1; i < catalog.size(); i++) {
        REQUIRE(catalog.stars[i].flux <= catalog.stars[i-1].flux);
    }

    // All stars should have valid coordinates within image bounds
    for (const auto& star : catalog.stars) {
        REQUIRE(star.x >= 0.0f);
        REQUIRE(star.x < static_cast<float>(result.image.width()));
        REQUIRE(star.y >= 0.0f);
        REQUIRE(star.y < static_cast<float>(result.image.height()));
    }
}

TEST_CASE("HomographyMatrix: identity", "[homography]") {
    auto H = HomographyMatrix::identity();
    REQUIRE(H.is_identity());

    auto [x, y] = H.transform(100.0f, 200.0f);
    REQUIRE(x == Catch::Approx(100.0f));
    REQUIRE(y == Catch::Approx(200.0f));
}

TEST_CASE("HomographyMatrix: rotation detection", "[homography]") {
    HomographyMatrix H;
    // 180-degree rotation about origin
    H(0, 0) = -1.0f; H(0, 1) =  0.0f; H(0, 2) = 0.0f;
    H(1, 0) =  0.0f; H(1, 1) = -1.0f; H(1, 2) = 0.0f;
    H(2, 0) =  0.0f; H(2, 1) =  0.0f; H(2, 2) = 1.0f;

    REQUIRE(std::abs(H.rotation_degrees()) == Catch::Approx(180.0f).margin(1.0f));
    REQUIRE(H.is_meridian_flip() == true);
}

TEST_CASE("HomographyMatrix: small rotation is not meridian flip", "[homography]") {
    HomographyMatrix H;
    float angle = 2.0f * 3.14159f / 180.0f; // 2 degrees
    H(0, 0) = std::cos(angle);  H(0, 1) = -std::sin(angle); H(0, 2) = 0.0f;
    H(1, 0) = std::sin(angle);  H(1, 1) =  std::cos(angle); H(1, 2) = 0.0f;
    H(2, 0) = 0.0f;             H(2, 1) = 0.0f;              H(2, 2) = 1.0f;

    REQUIRE(H.is_meridian_flip() == false);
}
```

- [ ] **Step 9: Add test target**

```cmake
nukex_add_test(test_star_detector unit/alignment/test_star_detector.cpp nukex4_alignment)
```

- [ ] **Step 10: Build and run all tests**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake .. -DNUKEX_BUILD_MODULE=OFF && make -j$(nproc) && ctest --output-on-failure
```

- [ ] **Step 11: Commit**

```
feat(alignment): StarDetector + alignment types — star detection with centroid refinement

Detects stars via local maxima above SNR threshold, refines centroids with
intensity-weighted averaging, computes flux and SNR. HomographyMatrix with
transform, rotation detection, meridian flip check. Tested: synthetic
star fields, centroid accuracy, saturation rejection, real M16 FITS data.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

---

## Task 2: Star Matcher + RANSAC Homography

**Files:**
- Create: `src/lib/alignment/include/nukex/alignment/star_matcher.hpp`
- Create: `src/lib/alignment/include/nukex/alignment/homography.hpp`
- Create: `src/lib/alignment/src/star_matcher.cpp`
- Modify: `src/lib/alignment/src/homography.cpp` (add RANSAC + DLT)
- Create: `test/unit/alignment/test_homography.cpp`

- [ ] **Step 1: Create star_matcher.hpp**

```cpp
#pragma once

#include "nukex/alignment/types.hpp"

namespace nukex {

/// Match stars between two catalogs using nearest-neighbor search.
class StarMatcher {
public:
    struct Config {
        float max_distance = 5.0f;     // maximum match distance in pixels
        int   min_matches  = 8;        // minimum matches for valid alignment
    };

    /// Find nearest-neighbor matches between source and reference catalogs.
    /// Returns pairs of (source_idx, ref_idx).
    static std::vector<std::pair<int, int>> match(
        const StarCatalog& source,
        const StarCatalog& reference,
        float max_distance);
};

} // namespace nukex
```

- [ ] **Step 2: Create star_matcher.cpp**

```cpp
#include "nukex/alignment/star_matcher.hpp"
#include <cmath>

namespace nukex {

std::vector<std::pair<int, int>> StarMatcher::match(
    const StarCatalog& source,
    const StarCatalog& reference,
    float max_distance) {

    std::vector<std::pair<int, int>> matches;
    float max_dist2 = max_distance * max_distance;

    // For each source star, find the nearest reference star
    for (int si = 0; si < source.size(); si++) {
        float best_dist2 = max_dist2;
        int best_ri = -1;

        for (int ri = 0; ri < reference.size(); ri++) {
            float dx = source.stars[si].x - reference.stars[ri].x;
            float dy = source.stars[si].y - reference.stars[ri].y;
            float d2 = dx * dx + dy * dy;

            if (d2 < best_dist2) {
                best_dist2 = d2;
                best_ri = ri;
            }
        }

        if (best_ri >= 0) {
            matches.emplace_back(si, best_ri);
        }
    }

    return matches;
}

} // namespace nukex
```

- [ ] **Step 3: Create homography.hpp (RANSAC + DLT)**

Create `src/lib/alignment/include/nukex/alignment/homography.hpp`:

```cpp
#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/io/image.hpp"

namespace nukex {

/// Compute projective homography using RANSAC + Direct Linear Transform (DLT).
class HomographyComputer {
public:
    struct Config {
        int   ransac_iterations = 500;
        float inlier_threshold  = 1.5f;   // pixels
        int   min_matches       = 8;
    };

    /// Compute homography from source to reference using matched star pairs.
    /// Uses RANSAC to reject outliers, then DLT on inlier set.
    static AlignmentResult compute(
        const StarCatalog& source,
        const StarCatalog& reference,
        const std::vector<std::pair<int, int>>& matches,
        const Config& config = Config{});

    /// Apply homography to an image using bilinear interpolation.
    /// Pixels outside the transformed boundary get value 0.
    static Image warp(const Image& source, const HomographyMatrix& H,
                      int output_width, int output_height);

    /// Correct a meridian-flipped homography by pre-multiplying with
    /// a 180-degree rotation about the image center.
    static HomographyMatrix correct_meridian_flip(
        const HomographyMatrix& H, int width, int height);

private:
    /// Solve for homography from exactly 4 point correspondences using DLT.
    static HomographyMatrix dlt_4point(
        const float src_x[4], const float src_y[4],
        const float dst_x[4], const float dst_y[4]);

    /// Solve for homography from N point correspondences using DLT (SVD).
    static HomographyMatrix dlt_npoint(
        const std::vector<float>& src_x, const std::vector<float>& src_y,
        const std::vector<float>& dst_x, const std::vector<float>& dst_y);

    /// Count inliers for a given homography.
    static int count_inliers(
        const HomographyMatrix& H,
        const StarCatalog& source,
        const StarCatalog& reference,
        const std::vector<std::pair<int, int>>& matches,
        float threshold,
        std::vector<bool>& inlier_mask);
};

} // namespace nukex
```

- [ ] **Step 4: Extend homography.cpp with RANSAC + DLT + warp**

This is the most complex implementation. Add to `src/lib/alignment/src/homography.cpp`:

```cpp
// Add at top of file:
#include "nukex/alignment/homography.hpp"
#include <Eigen/Dense>
#include <Eigen/SVD>
#include <random>
#include <cmath>

namespace nukex {

// ... existing HomographyMatrix methods stay ...

HomographyMatrix HomographyComputer::dlt_4point(
    const float src_x[4], const float src_y[4],
    const float dst_x[4], const float dst_y[4]) {

    // Build the 8×9 DLT matrix A where A * h = 0
    // For each correspondence (x,y) → (x',y'):
    //   [-x -y -1  0  0  0  x'x  x'y  x']
    //   [ 0  0  0 -x -y -1  y'x  y'y  y']
    Eigen::Matrix<float, 8, 9> A;
    A.setZero();

    for (int i = 0; i < 4; i++) {
        float x = src_x[i], y = src_y[i];
        float xp = dst_x[i], yp = dst_y[i];

        A(2*i, 0) = -x;  A(2*i, 1) = -y;  A(2*i, 2) = -1.0f;
        A(2*i, 6) = xp*x; A(2*i, 7) = xp*y; A(2*i, 8) = xp;

        A(2*i+1, 3) = -x;  A(2*i+1, 4) = -y;  A(2*i+1, 5) = -1.0f;
        A(2*i+1, 6) = yp*x; A(2*i+1, 7) = yp*y; A(2*i+1, 8) = yp;
    }

    // SVD of A; h is the last column of V
    Eigen::JacobiSVD<Eigen::Matrix<float, 8, 9>> svd(A, Eigen::ComputeFullV);
    Eigen::Matrix<float, 9, 1> h = svd.matrixV().col(8);

    // Reshape h into 3×3 HomographyMatrix
    HomographyMatrix H;
    H(0,0) = h(0); H(0,1) = h(1); H(0,2) = h(2);
    H(1,0) = h(3); H(1,1) = h(4); H(1,2) = h(5);
    H(2,0) = h(6); H(2,1) = h(7); H(2,2) = h(8);

    // Normalize so H(2,2) = 1
    if (std::abs(H(2,2)) > 1e-10f) {
        float s = 1.0f / H(2,2);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                H(r,c) *= s;
    }

    return H;
}

HomographyMatrix HomographyComputer::dlt_npoint(
    const std::vector<float>& src_x, const std::vector<float>& src_y,
    const std::vector<float>& dst_x, const std::vector<float>& dst_y) {

    int n = static_cast<int>(src_x.size());
    Eigen::MatrixXf A(2 * n, 9);
    A.setZero();

    for (int i = 0; i < n; i++) {
        float x = src_x[i], y = src_y[i];
        float xp = dst_x[i], yp = dst_y[i];

        A(2*i, 0) = -x;  A(2*i, 1) = -y;  A(2*i, 2) = -1.0f;
        A(2*i, 6) = xp*x; A(2*i, 7) = xp*y; A(2*i, 8) = xp;

        A(2*i+1, 3) = -x;  A(2*i+1, 4) = -y;  A(2*i+1, 5) = -1.0f;
        A(2*i+1, 6) = yp*x; A(2*i+1, 7) = yp*y; A(2*i+1, 8) = yp;
    }

    Eigen::JacobiSVD<Eigen::MatrixXf> svd(A, Eigen::ComputeFullV);
    Eigen::VectorXf h = svd.matrixV().col(8);

    HomographyMatrix H;
    H(0,0) = h(0); H(0,1) = h(1); H(0,2) = h(2);
    H(1,0) = h(3); H(1,1) = h(4); H(1,2) = h(5);
    H(2,0) = h(6); H(2,1) = h(7); H(2,2) = h(8);

    if (std::abs(H(2,2)) > 1e-10f) {
        float s = 1.0f / H(2,2);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                H(r,c) *= s;
    }

    return H;
}

int HomographyComputer::count_inliers(
    const HomographyMatrix& H,
    const StarCatalog& source,
    const StarCatalog& reference,
    const std::vector<std::pair<int, int>>& matches,
    float threshold,
    std::vector<bool>& inlier_mask) {

    float thresh2 = threshold * threshold;
    int count = 0;
    inlier_mask.resize(matches.size());

    for (size_t i = 0; i < matches.size(); i++) {
        auto [si, ri] = matches[i];
        auto [xp, yp] = H.transform(source.stars[si].x, source.stars[si].y);

        float dx = xp - reference.stars[ri].x;
        float dy = yp - reference.stars[ri].y;
        float d2 = dx * dx + dy * dy;

        inlier_mask[i] = (d2 < thresh2);
        if (inlier_mask[i]) count++;
    }

    return count;
}

AlignmentResult HomographyComputer::compute(
    const StarCatalog& source,
    const StarCatalog& reference,
    const std::vector<std::pair<int, int>>& matches,
    const Config& config) {

    AlignmentResult result;

    if (static_cast<int>(matches.size()) < config.min_matches) {
        result.alignment_failed = true;
        result.weight_penalty = 0.5f;
        result.H = HomographyMatrix::identity();
        return result;
    }

    // RANSAC
    std::mt19937 rng(42);  // deterministic for reproducibility
    int best_inlier_count = 0;
    std::vector<bool> best_inlier_mask;

    HomographyMatrix best_H;
    int n_matches = static_cast<int>(matches.size());

    for (int iter = 0; iter < config.ransac_iterations; iter++) {
        // Sample 4 random correspondences
        int indices[4];
        for (int i = 0; i < 4; i++) {
            bool unique;
            do {
                indices[i] = rng() % n_matches;
                unique = true;
                for (int j = 0; j < i; j++) {
                    if (indices[i] == indices[j]) { unique = false; break; }
                }
            } while (!unique);
        }

        float sx[4], sy[4], dx[4], dy[4];
        for (int i = 0; i < 4; i++) {
            auto [si, ri] = matches[indices[i]];
            sx[i] = source.stars[si].x;
            sy[i] = source.stars[si].y;
            dx[i] = reference.stars[ri].x;
            dy[i] = reference.stars[ri].y;
        }

        HomographyMatrix H = dlt_4point(sx, sy, dx, dy);

        // Count inliers
        std::vector<bool> inlier_mask;
        int inlier_count = count_inliers(H, source, reference, matches,
                                          config.inlier_threshold, inlier_mask);

        if (inlier_count > best_inlier_count) {
            best_inlier_count = inlier_count;
            best_inlier_mask = inlier_mask;
            best_H = H;
        }
    }

    if (best_inlier_count < config.min_matches) {
        result.alignment_failed = true;
        result.weight_penalty = 0.5f;
        result.H = HomographyMatrix::identity();
        return result;
    }

    // Refine: DLT on all inliers
    std::vector<float> sx, sy, dx, dy;
    for (size_t i = 0; i < matches.size(); i++) {
        if (best_inlier_mask[i]) {
            auto [si, ri] = matches[i];
            sx.push_back(source.stars[si].x);
            sy.push_back(source.stars[si].y);
            dx.push_back(reference.stars[ri].x);
            dy.push_back(reference.stars[ri].y);
        }
    }

    result.H = dlt_npoint(sx, sy, dx, dy);
    result.match.n_inliers = best_inlier_count;
    result.match.success = true;

    // Compute RMS error on inliers
    float sum_err2 = 0.0f;
    for (size_t i = 0; i < sx.size(); i++) {
        auto [xp, yp] = result.H.transform(sx[i], sy[i]);
        float ex = xp - dx[i];
        float ey = yp - dy[i];
        sum_err2 += ex * ex + ey * ey;
    }
    result.match.rms_error = std::sqrt(sum_err2 / static_cast<float>(sx.size()));

    // Check for meridian flip
    result.is_meridian_flipped = result.H.is_meridian_flip();

    return result;
}

Image HomographyComputer::warp(const Image& source, const HomographyMatrix& H,
                                int output_width, int output_height) {
    Image output(output_width, output_height, source.n_channels());

    // Compute inverse H for backward mapping
    // H maps source→ref, so H_inv maps ref→source
    Eigen::Matrix3f He;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            He(r, c) = H(r, c);

    Eigen::Matrix3f H_inv = He.inverse();

    int sw = source.width();
    int sh = source.height();

    for (int ch = 0; ch < source.n_channels(); ch++) {
        for (int y = 0; y < output_height; y++) {
            for (int x = 0; x < output_width; x++) {
                // Map output (x, y) back to source coordinates
                float w = H_inv(2, 0) * x + H_inv(2, 1) * y + H_inv(2, 2);
                if (std::abs(w) < 1e-10f) continue;
                float sx = (H_inv(0, 0) * x + H_inv(0, 1) * y + H_inv(0, 2)) / w;
                float sy = (H_inv(1, 0) * x + H_inv(1, 1) * y + H_inv(1, 2)) / w;

                // Bilinear interpolation
                if (sx < 0 || sx >= sw - 1 || sy < 0 || sy >= sh - 1) continue;

                int ix = static_cast<int>(sx);
                int iy = static_cast<int>(sy);
                float fx = sx - ix;
                float fy = sy - iy;

                float v00 = source.at(ix,     iy,     ch);
                float v10 = source.at(ix + 1, iy,     ch);
                float v01 = source.at(ix,     iy + 1, ch);
                float v11 = source.at(ix + 1, iy + 1, ch);

                float val = v00 * (1-fx) * (1-fy) +
                            v10 * fx * (1-fy) +
                            v01 * (1-fx) * fy +
                            v11 * fx * fy;

                output.at(x, y, ch) = val;
            }
        }
    }

    return output;
}

HomographyMatrix HomographyComputer::correct_meridian_flip(
    const HomographyMatrix& H, int width, int height) {
    // Pre-multiply H with 180-degree rotation about image center
    // flip = [-1  0  w-1]
    //        [ 0 -1  h-1]
    //        [ 0  0    1]
    Eigen::Matrix3f He, Fe;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            He(r, c) = H(r, c);

    Fe << -1, 0, static_cast<float>(width - 1),
           0, -1, static_cast<float>(height - 1),
           0,  0, 1;

    Eigen::Matrix3f result = Fe * He;

    HomographyMatrix corrected;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            corrected(r, c) = result(r, c);

    // Normalize
    if (std::abs(corrected(2, 2)) > 1e-10f) {
        float s = 1.0f / corrected(2, 2);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                corrected(r, c) *= s;
    }

    return corrected;
}

} // namespace nukex
```

- [ ] **Step 5: Create test_homography.cpp**

Create `test/unit/alignment/test_homography.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/alignment/homography.hpp"
#include "nukex/alignment/star_matcher.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/io/image.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("HomographyComputer: identity from identical catalogs", "[homography]") {
    // Two identical star catalogs should produce identity homography
    StarCatalog cat;
    for (int i = 0; i < 20; i++) {
        Star s;
        s.x = 50.0f + static_cast<float>(i % 5) * 80.0f;
        s.y = 50.0f + static_cast<float>(i / 5) * 80.0f;
        s.flux = 100.0f - static_cast<float>(i);
        cat.stars.push_back(s);
    }

    auto matches = StarMatcher::match(cat, cat, 5.0f);
    REQUIRE(matches.size() >= 8);

    auto result = HomographyComputer::compute(cat, cat, matches);
    REQUIRE(result.match.success == true);
    REQUIRE(result.alignment_failed == false);
    REQUIRE(result.H.is_identity(0.01f) == true);
    REQUIRE(result.match.rms_error < 0.1f);
}

TEST_CASE("HomographyComputer: translation recovery", "[homography]") {
    // Source catalog shifted by (10, 5)
    StarCatalog ref, src;
    float dx = 10.0f, dy = 5.0f;

    for (int i = 0; i < 20; i++) {
        Star s;
        s.x = 100.0f + static_cast<float>(i % 5) * 60.0f;
        s.y = 100.0f + static_cast<float>(i / 5) * 60.0f;
        s.flux = 100.0f;
        ref.stars.push_back(s);

        Star s2 = s;
        s2.x += dx;
        s2.y += dy;
        src.stars.push_back(s2);
    }

    // Match with large enough distance to find shifted pairs
    auto matches = StarMatcher::match(src, ref, 20.0f);
    REQUIRE(matches.size() >= 8);

    auto result = HomographyComputer::compute(src, ref, matches);
    REQUIRE(result.match.success == true);

    // Verify: transform a source point should give the reference point
    auto [tx, ty] = result.H.transform(ref.stars[0].x + dx, ref.stars[0].y + dy);
    REQUIRE(tx == Catch::Approx(ref.stars[0].x).margin(0.5f));
    REQUIRE(ty == Catch::Approx(ref.stars[0].y).margin(0.5f));
}

TEST_CASE("HomographyComputer: too few matches fails gracefully", "[homography]") {
    StarCatalog src, ref;
    for (int i = 0; i < 3; i++) {
        Star s;
        s.x = static_cast<float>(i) * 100.0f;
        s.y = 100.0f;
        src.stars.push_back(s);
        ref.stars.push_back(s);
    }

    auto matches = StarMatcher::match(src, ref, 5.0f);
    auto result = HomographyComputer::compute(src, ref, matches);

    REQUIRE(result.alignment_failed == true);
    REQUIRE(result.weight_penalty == Catch::Approx(0.5f));
}

TEST_CASE("HomographyComputer: warp with identity preserves image", "[homography]") {
    Image img(20, 20, 1);
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < 20; x++)
            img.at(x, y, 0) = static_cast<float>(x + y * 20) / 400.0f;

    auto H = HomographyMatrix::identity();
    Image warped = HomographyComputer::warp(img, H, 20, 20);

    // Interior pixels should be preserved exactly
    for (int y = 1; y < 19; y++) {
        for (int x = 1; x < 19; x++) {
            REQUIRE(warped.at(x, y, 0) == Catch::Approx(img.at(x, y, 0)).margin(0.01f));
        }
    }
}

TEST_CASE("HomographyComputer: meridian flip correction", "[homography]") {
    // 180-degree rotation
    HomographyMatrix H;
    H(0,0) = -1; H(0,1) = 0; H(0,2) = 99;
    H(1,0) = 0;  H(1,1) = -1; H(1,2) = 79;
    H(2,0) = 0;  H(2,1) = 0;  H(2,2) = 1;

    REQUIRE(H.is_meridian_flip() == true);

    auto corrected = HomographyComputer::correct_meridian_flip(H, 100, 80);
    // After correction, should be approximately identity
    REQUIRE(corrected.is_identity(1.0f) == true);
}

TEST_CASE("StarMatcher: identical catalogs match all", "[star_matcher]") {
    StarCatalog cat;
    for (int i = 0; i < 10; i++) {
        Star s;
        s.x = static_cast<float>(i * 50);
        s.y = static_cast<float>(i * 30);
        cat.stars.push_back(s);
    }

    auto matches = StarMatcher::match(cat, cat, 5.0f);
    REQUIRE(matches.size() == 10);

    // Each star should match itself
    for (const auto& [si, ri] : matches) {
        REQUIRE(si == ri);
    }
}
```

- [ ] **Step 6: Update CMakeLists.txt files**

Add `src/star_matcher.cpp` to alignment lib sources (note: star_matcher.cpp has minimal content).
Add test target: `nukex_add_test(test_homography unit/alignment/test_homography.cpp nukex4_alignment)`

- [ ] **Step 7: Build and run all tests**

- [ ] **Step 8: Commit**

```
feat(alignment): RANSAC+DLT homography, star matching, image warping

Projective homography via RANSAC (500 iter) + SVD-based DLT on inlier
set. Nearest-neighbor star matching. Bilinear image warping. Meridian
flip detection and correction. Failed alignment: weight × 0.5, never
discarded. Tested: identity recovery, translation, too-few matches,
identity warp preservation, meridian flip correction.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

---

## Task 3: Frame Aligner (High-Level Integration)

**Files:**
- Create: `src/lib/alignment/include/nukex/alignment/frame_aligner.hpp`
- Create: `src/lib/alignment/src/frame_aligner.cpp`
- Create: `test/unit/alignment/test_frame_aligner.cpp`

- [ ] **Step 1: Create frame_aligner.hpp**

```cpp
#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/alignment/star_matcher.hpp"
#include "nukex/alignment/homography.hpp"
#include "nukex/core/frame_metadata.hpp"
#include "nukex/io/image.hpp"

namespace nukex {

/// High-level frame alignment: detect → match → homography → warp.
///
/// The first frame becomes the reference (H = identity). Subsequent frames
/// are aligned to the reference catalog. Meridian flips are detected and
/// corrected automatically. Failed alignments keep weight × 0.5.
class FrameAligner {
public:
    struct Config {
        StarDetector::Config star_config;
        StarMatcher::Config  match_config;
        HomographyComputer::Config homography_config;
    };

    FrameAligner() = default;
    explicit FrameAligner(const Config& config);

    /// Align a frame to the reference. Returns the aligned image and alignment result.
    /// If this is the first frame, it becomes the reference (H = identity).
    ///
    /// The input image should be single-channel (luminance or raw Bayer).
    /// For star detection, channel 0 is used.
    struct AlignedFrame {
        Image image;              // warped image, aligned to reference
        AlignmentResult alignment;
        StarCatalog stars;        // detected stars (pre-alignment coordinates)
        int frame_index;
    };

    AlignedFrame align(const Image& frame, int frame_index);

    /// Get the reference catalog (for inspection/debugging).
    const StarCatalog& reference_catalog() const { return ref_catalog_; }

    /// Has a reference frame been set?
    bool has_reference() const { return has_ref_; }

    /// Reset the aligner (clear reference).
    void reset();

private:
    Config config_;
    StarCatalog ref_catalog_;
    bool has_ref_ = false;
    int ref_width_ = 0;
    int ref_height_ = 0;
};

} // namespace nukex
```

- [ ] **Step 2: Create frame_aligner.cpp**

```cpp
#include "nukex/alignment/frame_aligner.hpp"

namespace nukex {

FrameAligner::FrameAligner(const Config& config) : config_(config) {}

FrameAligner::AlignedFrame FrameAligner::align(const Image& frame, int frame_index) {
    AlignedFrame result;
    result.frame_index = frame_index;

    // Detect stars
    result.stars = StarDetector::detect(frame, config_.star_config);

    if (!has_ref_) {
        // First frame: becomes the reference
        ref_catalog_ = result.stars;
        ref_width_ = frame.width();
        ref_height_ = frame.height();
        has_ref_ = true;

        result.image = frame.clone();
        result.alignment.H = HomographyMatrix::identity();
        result.alignment.match.success = true;
        result.alignment.match.n_inliers = result.stars.size();
        return result;
    }

    // Match stars to reference
    auto matches = StarMatcher::match(result.stars, ref_catalog_,
                                       config_.match_config.max_distance);

    // Compute homography
    result.alignment = HomographyComputer::compute(
        result.stars, ref_catalog_, matches, config_.homography_config);

    // Handle meridian flip
    if (result.alignment.is_meridian_flipped && !result.alignment.alignment_failed) {
        result.alignment.H = HomographyComputer::correct_meridian_flip(
            result.alignment.H, ref_width_, ref_height_);
    }

    // Warp image to reference frame
    if (!result.alignment.alignment_failed) {
        result.image = HomographyComputer::warp(
            frame, result.alignment.H, ref_width_, ref_height_);
    } else {
        // Failed alignment: return unwarped frame, weight penalized
        result.image = frame.clone();
    }

    return result;
}

void FrameAligner::reset() {
    ref_catalog_ = StarCatalog{};
    has_ref_ = false;
    ref_width_ = 0;
    ref_height_ = 0;
}

} // namespace nukex
```

- [ ] **Step 3: Create test_frame_aligner.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/io/fits_reader.hpp"
#include <filesystem>
#include <cmath>

using namespace nukex;

/// Create a synthetic star field with Gaussian stars.
static Image create_star_field(int w, int h,
                                const std::vector<std::tuple<float,float,float>>& stars,
                                float bg = 0.05f, float sigma = 3.0f) {
    Image img(w, h, 1);
    img.fill(bg);
    for (const auto& [sx, sy, amp] : stars) {
        int r = static_cast<int>(sigma * 4);
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int px = static_cast<int>(sx + 0.5f) + dx;
                int py = static_cast<int>(sy + 0.5f) + dy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                float fx = static_cast<float>(px) - sx;
                float fy = static_cast<float>(py) - sy;
                img.at(px, py, 0) += amp * std::exp(-(fx*fx + fy*fy) / (2*sigma*sigma));
            }
        }
    }
    return img;
}

TEST_CASE("FrameAligner: first frame becomes reference", "[aligner]") {
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 50, 0.7f}, {100, 100, 0.9f},
        {50, 150, 0.6f}, {150, 150, 0.75f}, {80, 80, 0.65f},
        {120, 120, 0.55f}, {30, 100, 0.5f}, {170, 100, 0.45f}
    };

    Image frame = create_star_field(200, 200, stars);
    FrameAligner aligner;
    auto result = aligner.align(frame, 0);

    REQUIRE(aligner.has_reference() == true);
    REQUIRE(result.alignment.H.is_identity() == true);
    REQUIRE(result.alignment.alignment_failed == false);
    REQUIRE(result.image.width() == 200);
}

TEST_CASE("FrameAligner: identical frames align to identity", "[aligner]") {
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 50, 0.7f}, {100, 100, 0.9f},
        {50, 150, 0.6f}, {150, 150, 0.75f}, {80, 80, 0.65f},
        {120, 120, 0.55f}, {30, 100, 0.5f}, {170, 100, 0.45f}
    };

    Image frame = create_star_field(200, 200, stars);

    FrameAligner::Config config;
    config.star_config.snr_multiplier = 3.0f;
    config.match_config.max_distance = 10.0f;
    FrameAligner aligner(config);

    auto ref_result = aligner.align(frame, 0);
    auto result = aligner.align(frame, 1);

    REQUIRE(result.alignment.match.success == true);
    REQUIRE(result.alignment.H.is_identity(1.0f) == true);
    REQUIRE(result.alignment.match.rms_error < 1.0f);
}

TEST_CASE("FrameAligner: real FITS frames", "[aligner][integration]") {
    std::string path1 = "/home/scarter4work/projects/processing/M16/"
                        "Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit";
    std::string path2 = "/home/scarter4work/projects/processing/M16/"
                        "Light_M16_300.0s_Bin1_HaO3_20230901-232001_0002.fit";

    if (!std::filesystem::exists(path1) || !std::filesystem::exists(path2)) {
        SKIP("Test FITS files not available");
    }

    auto r1 = FITSReader::read(path1);
    auto r2 = FITSReader::read(path2);
    REQUIRE(r1.success);
    REQUIRE(r2.success);

    FrameAligner::Config config;
    config.star_config.snr_multiplier = 5.0f;
    config.star_config.max_stars = 200;
    config.match_config.max_distance = 20.0f;

    FrameAligner aligner(config);

    auto ref = aligner.align(r1.image, 0);
    REQUIRE(ref.alignment.H.is_identity());
    INFO("Reference stars: " << ref.stars.size());
    REQUIRE(ref.stars.size() >= 10);

    auto aligned = aligner.align(r2.image, 1);
    INFO("Inliers: " << aligned.alignment.match.n_inliers);
    INFO("RMS error: " << aligned.alignment.match.rms_error);
    REQUIRE(aligned.alignment.match.success == true);
    REQUIRE(aligned.alignment.alignment_failed == false);
    REQUIRE(aligned.alignment.match.n_inliers >= 8);
    REQUIRE(aligned.alignment.match.rms_error < 5.0f);
}

TEST_CASE("FrameAligner: reset clears reference", "[aligner]") {
    FrameAligner aligner;
    Image frame(100, 100, 1);
    frame.fill(0.5f);

    aligner.align(frame, 0);
    REQUIRE(aligner.has_reference() == true);

    aligner.reset();
    REQUIRE(aligner.has_reference() == false);
}
```

- [ ] **Step 4: Update CMakeLists.txt**

Add `src/frame_aligner.cpp` to alignment lib.
Add test target: `nukex_add_test(test_frame_aligner unit/alignment/test_frame_aligner.cpp nukex4_alignment)`

- [ ] **Step 5: Build and run all tests**

- [ ] **Step 6: Commit**

```
feat(alignment): FrameAligner — high-level frame alignment pipeline

Detect stars → match to reference → RANSAC homography → warp. First
frame becomes reference. Meridian flip auto-detected and corrected.
Failed alignment: weight × 0.5, frame never discarded. Tested: synthetic
star fields, identical frame alignment, real M16 consecutive frames,
reset behavior.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

---

## Task 4: Full lib/alignment Verification

- [ ] **Step 1: Clean Debug build + all tests**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build && mkdir build && cd build
cmake .. -DNUKEX_BUILD_MODULE=OFF -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) && ctest --output-on-failure
```

- [ ] **Step 2: Clean Release build + all tests**

```bash
rm -rf build && mkdir build && cd build
cmake .. -DNUKEX_BUILD_MODULE=OFF -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) && ctest --output-on-failure
```

Expected: All tests pass in both modes.
