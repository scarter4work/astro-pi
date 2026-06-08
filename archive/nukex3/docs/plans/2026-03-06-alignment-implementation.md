# Image Alignment Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Align sub-exposure frames via star centroid matching with integer pixel shifts and autocrop, slotting into the existing stacking pipeline before quality weighting.

**Architecture:** Three new engine components (StarDetector, TriangleMatcher, FrameAligner) form a pipeline: detect stars per frame → match triangles to reference → compute integer offsets → autocrop and copy into SubCube. FrameLoader is modified to call FrameAligner after loading raw frames. The rest of the pipeline (QualityWeights, PixelSelector) is unchanged.

**Tech Stack:** C++17, xtensor (SubCube), Catch2 v3 (tests). No new dependencies — all algorithms are self-contained math.

---

### Task 1: StarDetector — Data Structures and Background Stats

**Files:**
- Create: `src/engine/StarDetector.h`
- Create: `src/engine/StarDetector.cpp`
- Create: `tests/unit/test_star_detector.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

In `tests/unit/test_star_detector.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include <algorithm>
#include <numeric>
#include "engine/StarDetector.h"

TEST_CASE("computeBackground returns median and MAD", "[star_detector]") {
    // 100 pixels: 90 at value 0.1, 10 at value 0.9
    std::vector<float> image(100, 0.1f);
    for (int i = 0; i < 10; ++i) image[i] = 0.9f;
    std::sort(image.begin(), image.end());

    auto [median, mad] = nukex::computeBackground(image.data(), image.size());
    REQUIRE(median == Catch::Approx(0.1).margin(0.01));
    REQUIRE(mad > 0.0);
}

TEST_CASE("computeBackground handles constant data", "[star_detector]") {
    std::vector<float> image(50, 0.5f);
    auto [median, mad] = nukex::computeBackground(image.data(), image.size());
    REQUIRE(median == Catch::Approx(0.5));
    REQUIRE(mad == Catch::Approx(0.0));
}
```

**Step 2: Run test to verify it fails**

```bash
cd build && cmake .. && make test_star_detector 2>&1
```
Expected: compilation error — `StarDetector.h` does not exist yet.

**Step 3: Write minimal header and implementation**

In `src/engine/StarDetector.h`:

```cpp
#pragma once

#include <vector>
#include <cstddef>
#include <utility>

namespace nukex {

struct StarPosition {
    double x;       // centroid x (sub-pixel precision from intensity weighting)
    double y;       // centroid y
    double flux;    // total intensity of the star blob
};

// Compute background statistics: returns (median, MAD)
std::pair<double, double> computeBackground(const float* data, size_t count);

} // namespace nukex
```

In `src/engine/StarDetector.cpp`:

```cpp
#include "StarDetector.h"
#include <algorithm>
#include <cmath>

namespace nukex {

std::pair<double, double> computeBackground(const float* data, size_t count) {
    if (count == 0)
        return {0.0, 0.0};

    // Copy and sort for median
    std::vector<float> sorted(data, data + count);
    std::sort(sorted.begin(), sorted.end());

    double median;
    if (count % 2 == 0)
        median = 0.5 * (sorted[count/2 - 1] + sorted[count/2]);
    else
        median = sorted[count/2];

    // MAD = median(|x_i - median|)
    std::vector<float> absdev(count);
    for (size_t i = 0; i < count; ++i)
        absdev[i] = std::fabs(sorted[i] - static_cast<float>(median));
    std::sort(absdev.begin(), absdev.end());

    double mad;
    if (count % 2 == 0)
        mad = 0.5 * (absdev[count/2 - 1] + absdev[count/2]);
    else
        mad = absdev[count/2];

    return {median, mad};
}

} // namespace nukex
```

Add to `tests/CMakeLists.txt`:

```cmake
# StarDetector unit tests
add_executable(test_star_detector
    unit/test_star_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/StarDetector.cpp
)
target_link_libraries(test_star_detector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_star_detector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(test_star_detector PRIVATE cxx_std_17)
add_test(NAME test_star_detector COMMAND test_star_detector)
```

**Step 4: Run test to verify it passes**

```bash
cd build && cmake .. && make test_star_detector && ./tests/test_star_detector -v
```
Expected: 2 tests pass.

**Step 5: Commit**

```bash
git add src/engine/StarDetector.h src/engine/StarDetector.cpp tests/unit/test_star_detector.cpp tests/CMakeLists.txt
git commit -m "feat(alignment): StarDetector background stats — median and MAD"
```

---

### Task 2: StarDetector — Connected Components and Blob Extraction

**Files:**
- Modify: `src/engine/StarDetector.h`
- Modify: `src/engine/StarDetector.cpp`
- Modify: `tests/unit/test_star_detector.cpp`

**Step 1: Write the failing test**

Append to `tests/unit/test_star_detector.cpp`:

```cpp
TEST_CASE("extractBlobs finds connected bright regions", "[star_detector]") {
    // 10x10 image, background 0.1, one 3x3 bright blob centered at (5,5)
    std::vector<float> image(100, 0.1f);
    // Set a 3x3 bright patch
    image[4*10 + 4] = 0.9f; image[4*10 + 5] = 0.95f; image[4*10 + 6] = 0.9f;
    image[5*10 + 4] = 0.95f; image[5*10 + 5] = 1.0f; image[5*10 + 6] = 0.95f;
    image[6*10 + 4] = 0.9f; image[6*10 + 5] = 0.95f; image[6*10 + 6] = 0.9f;

    double threshold = 0.5; // well above background
    auto blobs = nukex::extractBlobs(image.data(), 10, 10, threshold);
    REQUIRE(blobs.size() == 1);
    REQUIRE(blobs[0].size() == 9); // 3x3 = 9 pixels
}

TEST_CASE("extractBlobs finds multiple separated blobs", "[star_detector]") {
    // 20x10 image with two blobs far apart
    std::vector<float> image(200, 0.1f);
    // Blob 1 at (2,2)
    image[2*20 + 2] = 1.0f; image[2*20 + 3] = 0.8f; image[3*20 + 2] = 0.8f;
    // Blob 2 at (7,17)
    image[7*20 + 17] = 1.0f; image[7*20 + 18] = 0.8f; image[8*20 + 17] = 0.8f;

    auto blobs = nukex::extractBlobs(image.data(), 20, 10, 0.5);
    REQUIRE(blobs.size() == 2);
}

TEST_CASE("extractBlobs returns empty for uniform image", "[star_detector]") {
    std::vector<float> image(100, 0.3f);
    auto blobs = nukex::extractBlobs(image.data(), 10, 10, 0.5);
    REQUIRE(blobs.empty());
}
```

**Step 2: Run test to verify it fails**

```bash
cd build && cmake .. && make test_star_detector 2>&1
```
Expected: compilation error — `extractBlobs` not declared.

**Step 3: Write implementation**

A `Blob` is a `std::vector<std::pair<int,int>>` (list of (x,y) pixel coordinates).

Add to `StarDetector.h`:

```cpp
#include <utility>

// A blob is a list of (x, y) pixel coordinates belonging to one connected component
typedef std::vector<std::pair<int, int>> Blob;

// Extract connected components of pixels above threshold (4-connectivity flood fill)
std::vector<Blob> extractBlobs(const float* image, int width, int height, double threshold);
```

Add to `StarDetector.cpp`:

```cpp
std::vector<Blob> extractBlobs(const float* image, int width, int height, double threshold) {
    std::vector<bool> visited(width * height, false);
    std::vector<Blob> blobs;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            if (visited[idx] || image[idx] <= threshold)
                continue;

            // Flood fill (BFS) with 4-connectivity
            Blob blob;
            std::vector<std::pair<int,int>> stack;
            stack.push_back({x, y});
            visited[idx] = true;

            while (!stack.empty()) {
                auto [cx, cy] = stack.back();
                stack.pop_back();
                blob.push_back({cx, cy});

                // 4 neighbors
                const int dx[] = {-1, 1, 0, 0};
                const int dy[] = {0, 0, -1, 1};
                for (int d = 0; d < 4; ++d) {
                    int nx = cx + dx[d];
                    int ny = cy + dy[d];
                    if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                        int ni = ny * width + nx;
                        if (!visited[ni] && image[ni] > threshold) {
                            visited[ni] = true;
                            stack.push_back({nx, ny});
                        }
                    }
                }
            }
            blobs.push_back(std::move(blob));
        }
    }
    return blobs;
}
```

**Step 4: Run test to verify it passes**

```bash
cd build && cmake .. && make test_star_detector && ./tests/test_star_detector -v
```
Expected: 5 tests pass.

**Step 5: Commit**

```bash
git add src/engine/StarDetector.h src/engine/StarDetector.cpp tests/unit/test_star_detector.cpp
git commit -m "feat(alignment): extractBlobs — connected component flood fill"
```

---

### Task 3: StarDetector — Blob Filtering and Centroid Calculation

**Files:**
- Modify: `src/engine/StarDetector.h`
- Modify: `src/engine/StarDetector.cpp`
- Modify: `tests/unit/test_star_detector.cpp`

**Step 1: Write the failing test**

Append to `tests/unit/test_star_detector.cpp`:

```cpp
TEST_CASE("blobToStar computes intensity-weighted centroid", "[star_detector]") {
    // 10x10 image, star centered at (5,5) with Gaussian-like profile
    std::vector<float> image(100, 0.0f);
    image[5*10 + 5] = 1.0f;   // center = brightest
    image[4*10 + 5] = 0.5f;   // above
    image[6*10 + 5] = 0.5f;   // below
    image[5*10 + 4] = 0.5f;   // left
    image[5*10 + 6] = 0.5f;   // right

    nukex::Blob blob = {{5,5}, {5,4}, {5,6}, {4,5}, {6,5}};
    auto star = nukex::blobToStar(blob, image.data(), 10, 10);
    REQUIRE(star.has_value());
    REQUIRE(star->x == Catch::Approx(5.0).margin(0.01));
    REQUIRE(star->y == Catch::Approx(5.0).margin(0.01));
    REQUIRE(star->flux == Catch::Approx(3.0).margin(0.01)); // 1.0 + 4*0.5
}

TEST_CASE("blobToStar rejects too-small blobs", "[star_detector]") {
    std::vector<float> image(100, 0.0f);
    image[5*10 + 5] = 1.0f;
    nukex::Blob blob = {{5,5}}; // only 1 pixel
    auto star = nukex::blobToStar(blob, image.data(), 10, 10, 3); // minSize=3
    REQUIRE(!star.has_value());
}

TEST_CASE("blobToStar rejects too-large blobs", "[star_detector]") {
    std::vector<float> image(10000, 1.0f);
    nukex::Blob blob;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 60; ++x)
            blob.push_back({x, y});
    auto star = nukex::blobToStar(blob, image.data(), 100, 100, 3, 50); // maxSize=50 pixels
    REQUIRE(!star.has_value());
}

TEST_CASE("detectStars end-to-end on synthetic image", "[star_detector]") {
    // 100x100 image, background ~0.1, 3 Gaussian stars
    std::vector<float> image(10000, 0.1f);

    // Star 1 at ~(20, 30)
    auto addStar = [&](int cx, int cy, float peak) {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                float r2 = float(dx*dx + dy*dy);
                float val = peak * std::exp(-r2 / 2.0f);
                image[(cy+dy)*100 + (cx+dx)] = 0.1f + val;
            }
    };
    addStar(20, 30, 0.8f);
    addStar(70, 50, 0.6f);
    addStar(40, 80, 0.9f);

    auto stars = nukex::detectStars(image.data(), 100, 100);
    REQUIRE(stars.size() == 3);
    // Stars should be sorted by flux descending
    REQUIRE(stars[0].flux >= stars[1].flux);
    REQUIRE(stars[1].flux >= stars[2].flux);
}
```

**Step 2: Run test to verify it fails**

```bash
cd build && cmake .. && make test_star_detector 2>&1
```
Expected: compilation error — `blobToStar` and `detectStars` not declared.

**Step 3: Write implementation**

Add to `StarDetector.h`:

```cpp
#include <optional>
#include <cmath>

struct DetectorConfig {
    double sigmaThreshold = 6.0;  // detection threshold in MAD units above median
    int    minBlobSize    = 3;    // minimum pixels in a star blob
    int    maxBlobSize    = 200;  // maximum pixels (reject nebula cores)
    double maxEccentricity = 0.7; // reject elongated blobs (trails/satellites)
};

// Convert a blob to a StarPosition (returns nullopt if blob fails filtering)
std::optional<StarPosition> blobToStar(const Blob& blob, const float* image,
                                        int width, int height,
                                        int minSize = 3, int maxSize = 200,
                                        double maxEccentricity = 0.7);

// Full star detection pipeline: background → threshold → blobs → filter → centroids
// Returns stars sorted by flux (brightest first)
std::vector<StarPosition> detectStars(const float* image, int width, int height,
                                       const DetectorConfig& config = DetectorConfig{});
```

Add to `StarDetector.cpp`:

```cpp
#include <optional>

std::optional<StarPosition> blobToStar(const Blob& blob, const float* image,
                                        int width, int height,
                                        int minSize, int maxSize,
                                        double maxEccentricity) {
    int n = static_cast<int>(blob.size());
    if (n < minSize || n > maxSize)
        return std::nullopt;

    // Compute intensity-weighted centroid and total flux
    double sumX = 0, sumY = 0, sumFlux = 0;
    for (auto [x, y] : blob) {
        double val = image[y * width + x];
        sumX += x * val;
        sumY += y * val;
        sumFlux += val;
    }

    if (sumFlux <= 0)
        return std::nullopt;

    double cx = sumX / sumFlux;
    double cy = sumY / sumFlux;

    // Compute second moments for eccentricity filtering
    double Mxx = 0, Myy = 0, Mxy = 0;
    for (auto [x, y] : blob) {
        double val = image[y * width + x];
        double dx = x - cx;
        double dy = y - cy;
        Mxx += val * dx * dx;
        Myy += val * dy * dy;
        Mxy += val * dx * dy;
    }
    Mxx /= sumFlux;
    Myy /= sumFlux;
    Mxy /= sumFlux;

    // Eigenvalues of the 2x2 moment matrix → semi-axes
    double trace = Mxx + Myy;
    double det = Mxx * Myy - Mxy * Mxy;
    double disc = trace * trace - 4.0 * det;
    if (disc < 0) disc = 0;
    double sqrtDisc = std::sqrt(disc);
    double lambda1 = 0.5 * (trace + sqrtDisc);
    double lambda2 = 0.5 * (trace - sqrtDisc);

    if (lambda1 > 0 && lambda2 >= 0) {
        double ecc = std::sqrt(1.0 - lambda2 / lambda1);
        if (ecc > maxEccentricity)
            return std::nullopt;
    }

    return StarPosition{cx, cy, sumFlux};
}

std::vector<StarPosition> detectStars(const float* image, int width, int height,
                                       const DetectorConfig& config) {
    size_t count = static_cast<size_t>(width) * height;

    // 1. Background stats
    auto [median, mad] = computeBackground(image, count);

    // 2. Threshold
    double threshold = median + config.sigmaThreshold * mad * 1.4826; // MAD to sigma conversion

    // 3. Extract blobs
    auto blobs = extractBlobs(image, width, height, threshold);

    // 4. Filter and compute centroids
    std::vector<StarPosition> stars;
    for (const auto& blob : blobs) {
        auto star = blobToStar(blob, image, width, height,
                               config.minBlobSize, config.maxBlobSize,
                               config.maxEccentricity);
        if (star.has_value())
            stars.push_back(*star);
    }

    // 5. Sort by flux descending
    std::sort(stars.begin(), stars.end(),
              [](const StarPosition& a, const StarPosition& b) { return a.flux > b.flux; });

    return stars;
}
```

**Step 4: Run test to verify it passes**

```bash
cd build && cmake .. && make test_star_detector && ./tests/test_star_detector -v
```
Expected: 9 tests pass.

**Step 5: Commit**

```bash
git add src/engine/StarDetector.h src/engine/StarDetector.cpp tests/unit/test_star_detector.cpp
git commit -m "feat(alignment): StarDetector — blob filtering, centroid, full pipeline"
```

---

### Task 4: TriangleMatcher — Triangle Descriptor and Building

**Files:**
- Create: `src/engine/TriangleMatcher.h`
- Create: `src/engine/TriangleMatcher.cpp`
- Create: `tests/unit/test_triangle_matcher.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

In `tests/unit/test_triangle_matcher.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include "engine/TriangleMatcher.h"

TEST_CASE("TriangleDescriptor normalizes side ratios correctly", "[triangle]") {
    // Equilateral-ish triangle: all sides equal
    nukex::StarPosition s1{0, 0, 1.0};
    nukex::StarPosition s2{10, 0, 1.0};
    nukex::StarPosition s3{5, 8.66, 1.0}; // ~equilateral

    auto desc = nukex::makeTriangleDescriptor(s1, s2, s3, 0, 1, 2);
    // For equilateral: b/a ~ 1.0, c/a ~ 1.0
    REQUIRE(desc.ratioBA == Catch::Approx(1.0).margin(0.05));
    REQUIRE(desc.ratioCA == Catch::Approx(1.0).margin(0.05));
}

TEST_CASE("TriangleDescriptor is invariant to translation", "[triangle]") {
    nukex::StarPosition s1a{0, 0, 1.0}, s2a{10, 0, 1.0}, s3a{5, 8, 1.0};
    nukex::StarPosition s1b{100, 200, 1.0}, s2b{110, 200, 1.0}, s3b{105, 208, 1.0};

    auto a = nukex::makeTriangleDescriptor(s1a, s2a, s3a, 0, 1, 2);
    auto b = nukex::makeTriangleDescriptor(s1b, s2b, s3b, 0, 1, 2);
    REQUIRE(a.ratioBA == Catch::Approx(b.ratioBA).margin(0.001));
    REQUIRE(a.ratioCA == Catch::Approx(b.ratioCA).margin(0.001));
}

TEST_CASE("buildDescriptors produces correct count", "[triangle]") {
    // 4 stars → C(4,3) = 4 triangles
    std::vector<nukex::StarPosition> stars = {
        {0, 0, 1.0}, {10, 0, 0.9}, {5, 8, 0.8}, {15, 5, 0.7}
    };
    auto descs = nukex::buildDescriptors(stars, 4);
    REQUIRE(descs.size() == 4);
}
```

**Step 2: Run test to verify it fails**

```bash
cd build && cmake .. && make test_triangle_matcher 2>&1
```
Expected: compilation error — `TriangleMatcher.h` does not exist.

**Step 3: Write implementation**

In `src/engine/TriangleMatcher.h`:

```cpp
#pragma once

#include "StarDetector.h"

#include <vector>
#include <cstddef>

namespace nukex {

struct TriangleDescriptor {
    double ratioBA;     // b/a where a >= b >= c (side lengths)
    double ratioCA;     // c/a
    int idx0, idx1, idx2; // star indices that form this triangle
};

struct AlignmentResult {
    int dx;                 // integer translation offset x
    int dy;                 // integer translation offset y
    int numMatchedStars;    // number of confirmed star-to-star matches
    double convergenceRMS;  // RMS of position residuals after alignment
    bool valid;             // true if enough matches were found
};

// Create a descriptor for a triangle formed by 3 stars
TriangleDescriptor makeTriangleDescriptor(const StarPosition& s1,
                                           const StarPosition& s2,
                                           const StarPosition& s3,
                                           int idx1, int idx2, int idx3);

// Build descriptors from top-N brightest stars
// maxStars limits how many stars to use (takes brightest, assumes pre-sorted)
std::vector<TriangleDescriptor> buildDescriptors(const std::vector<StarPosition>& stars,
                                                  int maxStars = 50);

// Match a target frame's stars against reference stars, return alignment result
// minMatches: minimum confirmed star matches to consider result valid
AlignmentResult matchFrames(const std::vector<StarPosition>& refStars,
                             const std::vector<StarPosition>& targetStars,
                             int maxStars = 50,
                             double matchTolerance = 0.01,
                             int minMatches = 5);

} // namespace nukex
```

In `src/engine/TriangleMatcher.cpp`:

```cpp
#include "TriangleMatcher.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace nukex {

static double dist(const StarPosition& a, const StarPosition& b) {
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return std::sqrt(dx*dx + dy*dy);
}

TriangleDescriptor makeTriangleDescriptor(const StarPosition& s1,
                                           const StarPosition& s2,
                                           const StarPosition& s3,
                                           int idx1, int idx2, int idx3) {
    double sides[3] = {dist(s1, s2), dist(s2, s3), dist(s1, s3)};
    std::sort(sides, sides + 3); // sides[0] <= sides[1] <= sides[2]
    // a = longest, b = middle, c = shortest
    double a = sides[2];
    double b = sides[1];
    double c = sides[0];

    TriangleDescriptor desc;
    desc.ratioBA = (a > 0) ? b / a : 0;
    desc.ratioCA = (a > 0) ? c / a : 0;
    desc.idx0 = idx1;
    desc.idx1 = idx2;
    desc.idx2 = idx3;
    return desc;
}

std::vector<TriangleDescriptor> buildDescriptors(const std::vector<StarPosition>& stars,
                                                  int maxStars) {
    int n = std::min(static_cast<int>(stars.size()), maxStars);
    std::vector<TriangleDescriptor> descs;

    // All C(n,3) triplets
    for (int i = 0; i < n - 2; ++i)
        for (int j = i + 1; j < n - 1; ++j)
            for (int k = j + 1; k < n; ++k)
                descs.push_back(makeTriangleDescriptor(stars[i], stars[j], stars[k], i, j, k));

    return descs;
}

} // namespace nukex
```

Add to `tests/CMakeLists.txt`:

```cmake
# TriangleMatcher unit tests
add_executable(test_triangle_matcher
    unit/test_triangle_matcher.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/TriangleMatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/StarDetector.cpp
)
target_link_libraries(test_triangle_matcher PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_triangle_matcher PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(test_triangle_matcher PRIVATE cxx_std_17)
add_test(NAME test_triangle_matcher COMMAND test_triangle_matcher)
```

**Step 4: Run test to verify it passes**

```bash
cd build && cmake .. && make test_triangle_matcher && ./tests/test_triangle_matcher -v
```
Expected: 3 tests pass.

**Step 5: Commit**

```bash
git add src/engine/TriangleMatcher.h src/engine/TriangleMatcher.cpp tests/unit/test_triangle_matcher.cpp tests/CMakeLists.txt
git commit -m "feat(alignment): TriangleMatcher — descriptors and building"
```

---

### Task 5: TriangleMatcher — Frame Matching and Translation Voting

**Files:**
- Modify: `src/engine/TriangleMatcher.cpp`
- Modify: `tests/unit/test_triangle_matcher.cpp`

**Step 1: Write the failing test**

Append to `tests/unit/test_triangle_matcher.cpp`:

```cpp
TEST_CASE("matchFrames detects known integer translation", "[triangle]") {
    // Reference stars
    std::vector<nukex::StarPosition> ref = {
        {100, 100, 1.0}, {200, 150, 0.9}, {150, 250, 0.8},
        {300, 100, 0.7}, {250, 300, 0.6}, {50, 200, 0.5}
    };

    // Target stars shifted by (dx=+7, dy=-3)
    int trueDx = 7, trueDy = -3;
    std::vector<nukex::StarPosition> target;
    for (const auto& s : ref)
        target.push_back({s.x + trueDx, s.y + trueDy, s.flux});

    auto result = nukex::matchFrames(ref, target, 6);
    REQUIRE(result.valid);
    REQUIRE(result.dx == trueDx);
    REQUIRE(result.dy == trueDy);
    REQUIRE(result.numMatchedStars >= 4);
}

TEST_CASE("matchFrames detects zero offset (identical frames)", "[triangle]") {
    std::vector<nukex::StarPosition> stars = {
        {100, 100, 1.0}, {200, 150, 0.9}, {150, 250, 0.8},
        {300, 100, 0.7}, {250, 300, 0.6}
    };

    auto result = nukex::matchFrames(stars, stars, 5);
    REQUIRE(result.valid);
    REQUIRE(result.dx == 0);
    REQUIRE(result.dy == 0);
}

TEST_CASE("matchFrames handles large dither offset", "[triangle]") {
    std::vector<nukex::StarPosition> ref = {
        {100, 100, 1.0}, {200, 150, 0.9}, {150, 250, 0.8},
        {300, 100, 0.7}, {250, 300, 0.6}, {350, 200, 0.5}
    };

    int trueDx = -15, trueDy = 12;
    std::vector<nukex::StarPosition> target;
    for (const auto& s : ref)
        target.push_back({s.x + trueDx, s.y + trueDy, s.flux});

    auto result = nukex::matchFrames(ref, target, 6);
    REQUIRE(result.valid);
    REQUIRE(result.dx == trueDx);
    REQUIRE(result.dy == trueDy);
}

TEST_CASE("matchFrames returns invalid for too few stars", "[triangle]") {
    std::vector<nukex::StarPosition> ref = {{100, 100, 1.0}};
    std::vector<nukex::StarPosition> target = {{107, 97, 1.0}};

    auto result = nukex::matchFrames(ref, target, 1, 0.01, 3);
    REQUIRE(!result.valid);
}
```

**Step 2: Run test to verify it fails**

```bash
cd build && cmake .. && make test_triangle_matcher && ./tests/test_triangle_matcher -v
```
Expected: FAIL — `matchFrames` has no implementation body (linker error or returns default).

**Step 3: Write implementation**

Add `matchFrames` to `TriangleMatcher.cpp`:

```cpp
AlignmentResult matchFrames(const std::vector<StarPosition>& refStars,
                             const std::vector<StarPosition>& targetStars,
                             int maxStars,
                             double matchTolerance,
                             int minMatches) {
    AlignmentResult result{0, 0, 0, 0.0, false};

    if (refStars.size() < 3 || targetStars.size() < 3)
        return result;

    auto refDescs = buildDescriptors(refStars, maxStars);
    auto tgtDescs = buildDescriptors(targetStars, maxStars);

    if (refDescs.empty() || tgtDescs.empty())
        return result;

    // Sort reference descriptors for binary-search style matching
    // Use (ratioBA, ratioCA) as the sorting key
    std::sort(refDescs.begin(), refDescs.end(),
              [](const TriangleDescriptor& a, const TriangleDescriptor& b) {
                  if (a.ratioBA != b.ratioBA) return a.ratioBA < b.ratioBA;
                  return a.ratioCA < b.ratioCA;
              });

    // Vote map: (refStarIdx, tgtStarIdx) → count
    std::map<std::pair<int,int>, int> votes;

    for (const auto& td : tgtDescs) {
        // Find matching reference descriptors within tolerance
        for (const auto& rd : refDescs) {
            if (std::fabs(td.ratioBA - rd.ratioBA) < matchTolerance &&
                std::fabs(td.ratioCA - rd.ratioCA) < matchTolerance) {
                // This match votes for 3 star correspondences
                // We don't know the ordering, so vote for all 9 combos
                // and let the majority win
                int tgtIdx[3] = {td.idx0, td.idx1, td.idx2};
                int refIdx[3] = {rd.idx0, rd.idx1, rd.idx2};

                // Try to find best assignment by distance
                // For pure translation, all offsets should be the same
                for (int ti = 0; ti < 3; ++ti) {
                    for (int ri = 0; ri < 3; ++ri) {
                        votes[{refIdx[ri], tgtIdx[ti]}]++;
                    }
                }
            }
        }
    }

    // Find consistent star pairs: for each ref star, pick the target star with most votes
    int nRef = std::min(static_cast<int>(refStars.size()), maxStars);
    int nTgt = std::min(static_cast<int>(targetStars.size()), maxStars);

    struct StarPair { int refIdx; int tgtIdx; int voteCount; };
    std::vector<StarPair> bestPairs;

    for (int ri = 0; ri < nRef; ++ri) {
        int bestTgt = -1;
        int bestVotes = 0;
        for (int ti = 0; ti < nTgt; ++ti) {
            auto it = votes.find({ri, ti});
            if (it != votes.end() && it->second > bestVotes) {
                bestVotes = it->second;
                bestTgt = ti;
            }
        }
        if (bestTgt >= 0 && bestVotes >= 2)
            bestPairs.push_back({ri, bestTgt, bestVotes});
    }

    if (static_cast<int>(bestPairs.size()) < minMatches)
        return result;

    // Compute offsets from matched pairs
    std::vector<double> dxVals, dyVals;
    for (const auto& p : bestPairs) {
        dxVals.push_back(targetStars[p.tgtIdx].x - refStars[p.refIdx].x);
        dyVals.push_back(targetStars[p.tgtIdx].y - refStars[p.refIdx].y);
    }

    // Median offset (robust to outlier matches)
    std::sort(dxVals.begin(), dxVals.end());
    std::sort(dyVals.begin(), dyVals.end());
    size_t m = dxVals.size();
    double medDx = (m % 2 == 0) ? 0.5 * (dxVals[m/2-1] + dxVals[m/2]) : dxVals[m/2];
    double medDy = (m % 2 == 0) ? 0.5 * (dyVals[m/2-1] + dyVals[m/2]) : dyVals[m/2];

    // Round to integer
    result.dx = static_cast<int>(std::round(medDx));
    result.dy = static_cast<int>(std::round(medDy));

    // Compute RMS of residuals
    double sumSq = 0;
    int inlierCount = 0;
    for (size_t i = 0; i < bestPairs.size(); ++i) {
        double residX = dxVals[i] - medDx;  // note: dxVals is sorted, not original order
        double residY = dyVals[i] - medDy;
        sumSq += residX * residX + residY * residY;
        ++inlierCount;
    }

    result.convergenceRMS = (inlierCount > 0) ? std::sqrt(sumSq / inlierCount) : 0;
    result.numMatchedStars = static_cast<int>(bestPairs.size());
    result.valid = result.numMatchedStars >= minMatches;
    return result;
}
```

**Step 4: Run test to verify it passes**

```bash
cd build && cmake .. && make test_triangle_matcher && ./tests/test_triangle_matcher -v
```
Expected: 7 tests pass.

**Step 5: Commit**

```bash
git add src/engine/TriangleMatcher.cpp tests/unit/test_triangle_matcher.cpp
git commit -m "feat(alignment): matchFrames — triangle voting and translation"
```

---

### Task 6: FrameAligner — Orchestrator and Autocrop

**Files:**
- Create: `src/engine/FrameAligner.h`
- Create: `src/engine/FrameAligner.cpp`
- Create: `tests/unit/test_frame_aligner.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

In `tests/unit/test_frame_aligner.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include <cmath>
#include "engine/FrameAligner.h"

// Helper: create a synthetic star field image (row-major float array)
static std::vector<float> makeSyntheticFrame(int width, int height,
                                              const std::vector<std::pair<int,int>>& starPositions,
                                              float background = 0.1f, float peak = 0.9f) {
    std::vector<float> img(width * height, background);
    for (auto [sx, sy] : starPositions) {
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                int x = sx + dx, y = sy + dy;
                if (x >= 0 && x < width && y >= 0 && y < height) {
                    float r2 = float(dx*dx + dy*dy);
                    img[y * width + x] += peak * std::exp(-r2 / 2.0f);
                }
            }
        }
    }
    return img;
}

TEST_CASE("computeCropRegion computes correct bounding box", "[aligner]") {
    // 3 frames: offsets (0,0), (+5, -3), (-2, +4)
    std::vector<nukex::AlignmentResult> offsets = {
        {0, 0, 10, 0.5, true},
        {5, -3, 8, 0.6, true},
        {-2, 4, 9, 0.4, true}
    };

    auto crop = nukex::computeCropRegion(offsets, 100, 80);
    // x_min = max(0, max(0,5,-2)) = 5
    // y_min = max(0, max(0,-3,4)) = 4
    // x_max = min(100, 100 + min(0,5,-2)) = min(100, 98) = 98
    // y_max = min(80, 80 + min(0,-3,4)) = min(80, 77) = 77
    REQUIRE(crop.x0 == 5);
    REQUIRE(crop.y0 == 4);
    REQUIRE(crop.x1 == 98);
    REQUIRE(crop.y1 == 77);
    REQUIRE(crop.width() == 93);
    REQUIRE(crop.height() == 73);
}

TEST_CASE("FrameAligner aligns shifted synthetic frames", "[aligner]") {
    int W = 200, H = 150;

    // Star positions in reference frame coordinates
    std::vector<std::pair<int,int>> starPos = {
        {50, 40}, {120, 30}, {80, 100}, {160, 70}, {30, 120}, {140, 110}
    };

    // Create 4 frames with known integer offsets
    int offsets[][2] = {{0,0}, {7,-3}, {-4,5}, {10,2}};
    std::vector<std::vector<float>> frames;
    for (auto [dx, dy] : offsets) {
        std::vector<std::pair<int,int>> shifted;
        for (auto [sx, sy] : starPos)
            shifted.push_back({sx + dx, sy + dy});
        frames.push_back(makeSyntheticFrame(W, H, shifted));
    }

    // Build raw frame data as contiguous float arrays
    std::vector<const float*> frameData;
    for (const auto& f : frames)
        frameData.push_back(f.data());

    auto result = nukex::alignFrames(frameData, W, H);
    REQUIRE(result.alignedCube.numSubs() == 4);
    // Crop should be smaller than original
    REQUIRE(result.alignedCube.width() < static_cast<size_t>(W));
    REQUIRE(result.alignedCube.height() < static_cast<size_t>(H));
    // All offsets were recovered
    REQUIRE(result.offsets.size() == 4);
    REQUIRE(result.offsets[0].dx == 0);
    REQUIRE(result.offsets[0].dy == 0);
}
```

**Step 2: Run test to verify it fails**

```bash
cd build && cmake .. && make test_frame_aligner 2>&1
```
Expected: compilation error — `FrameAligner.h` does not exist.

**Step 3: Write implementation**

In `src/engine/FrameAligner.h`:

```cpp
#pragma once

#include "StarDetector.h"
#include "TriangleMatcher.h"
#include "SubCube.h"

#include <vector>

namespace nukex {

struct CropRegion {
    int x0, y0;  // top-left of overlap region
    int x1, y1;  // bottom-right (exclusive)
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

struct AlignmentOutput {
    SubCube alignedCube;                       // cropped, aligned SubCube
    std::vector<AlignmentResult> offsets;       // per-frame alignment results
    CropRegion crop;                           // the crop region applied
    int referenceFrame;                        // index of the reference frame
};

// Compute the crop bounding box from alignment offsets
CropRegion computeCropRegion(const std::vector<AlignmentResult>& offsets,
                              int originalWidth, int originalHeight);

// Full alignment pipeline: detect stars, match, compute offsets, shift+crop into SubCube
// frameData: array of N pointers to row-major float images (width * height each)
// referenceIdx: which frame to use as reference (-1 = auto-select first)
AlignmentOutput alignFrames(const std::vector<const float*>& frameData,
                             int width, int height,
                             int referenceIdx = 0,
                             const DetectorConfig& detConfig = DetectorConfig{},
                             int matchMaxStars = 50);

} // namespace nukex
```

In `src/engine/FrameAligner.cpp`:

```cpp
#include "FrameAligner.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace nukex {

CropRegion computeCropRegion(const std::vector<AlignmentResult>& offsets,
                              int originalWidth, int originalHeight) {
    int maxDx = 0, maxDy = 0, minDx = 0, minDy = 0;
    for (const auto& o : offsets) {
        maxDx = std::max(maxDx, o.dx);
        maxDy = std::max(maxDy, o.dy);
        minDx = std::min(minDx, o.dx);
        minDy = std::min(minDy, o.dy);
    }

    CropRegion crop;
    crop.x0 = std::max(0, maxDx);
    crop.y0 = std::max(0, maxDy);
    crop.x1 = std::min(originalWidth, originalWidth + minDx);
    crop.y1 = std::min(originalHeight, originalHeight + minDy);

    if (crop.width() <= 0 || crop.height() <= 0)
        throw std::runtime_error("FrameAligner: no overlap region after alignment — offsets too large");

    return crop;
}

AlignmentOutput alignFrames(const std::vector<const float*>& frameData,
                             int width, int height,
                             int referenceIdx,
                             const DetectorConfig& detConfig,
                             int matchMaxStars) {
    size_t nFrames = frameData.size();
    if (nFrames < 2)
        throw std::invalid_argument("FrameAligner: need at least 2 frames");

    // 1. Detect stars in all frames
    std::vector<std::vector<StarPosition>> starLists(nFrames);
    for (size_t i = 0; i < nFrames; ++i)
        starLists[i] = detectStars(frameData[i], width, height, detConfig);

    // 2. Match each frame against reference
    std::vector<AlignmentResult> offsets(nFrames);
    offsets[referenceIdx] = {0, 0, static_cast<int>(starLists[referenceIdx].size()), 0.0, true};

    for (size_t i = 0; i < nFrames; ++i) {
        if (static_cast<int>(i) == referenceIdx) continue;
        offsets[i] = matchFrames(starLists[referenceIdx], starLists[i],
                                  matchMaxStars);
        if (!offsets[i].valid) {
            // Fallback: zero offset (keep the frame, don't reject)
            offsets[i] = {0, 0, 0, 0.0, true};
        }
    }

    // 3. Compute crop region
    CropRegion crop = computeCropRegion(offsets, width, height);

    // 4. Allocate aligned SubCube and copy shifted+cropped data
    SubCube cube(nFrames, crop.height(), crop.width());

    for (size_t i = 0; i < nFrames; ++i) {
        int dx = offsets[i].dx;
        int dy = offsets[i].dy;

        // For each pixel in the crop region, copy from the shifted source
        for (int cy = 0; cy < crop.height(); ++cy) {
            for (int cx = 0; cx < crop.width(); ++cx) {
                // Position in crop coordinates → position in original frame
                int srcX = crop.x0 + cx - dx;
                int srcY = crop.y0 + cy - dy;

                if (srcX >= 0 && srcX < width && srcY >= 0 && srcY < height) {
                    cube.setPixel(i, cy, cx, frameData[i][srcY * width + srcX]);
                }
            }
        }
    }

    return AlignmentOutput{std::move(cube), std::move(offsets), crop, referenceIdx};
}

} // namespace nukex
```

Add to `tests/CMakeLists.txt`:

```cmake
# FrameAligner unit tests
add_executable(test_frame_aligner
    unit/test_frame_aligner.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/FrameAligner.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/TriangleMatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/StarDetector.cpp
)
target_link_libraries(test_frame_aligner PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_frame_aligner PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
)
target_compile_definitions(test_frame_aligner PRIVATE XTENSOR_USE_XSIMD)
target_compile_features(test_frame_aligner PRIVATE cxx_std_17)
add_test(NAME test_frame_aligner COMMAND test_frame_aligner)
```

**Step 4: Run test to verify it passes**

```bash
cd build && cmake .. && make test_frame_aligner && ./tests/test_frame_aligner -v
```
Expected: 2 tests pass.

**Step 5: Commit**

```bash
git add src/engine/FrameAligner.h src/engine/FrameAligner.cpp tests/unit/test_frame_aligner.cpp tests/CMakeLists.txt
git commit -m "feat(alignment): FrameAligner — orchestrator with autocrop"
```

---

### Task 7: Integration — Wire Alignment into FrameLoader / ExecuteGlobal

**Files:**
- Modify: `src/engine/FrameLoader.h`
- Modify: `src/engine/FrameLoader.cpp`
- Modify: `src/NukeXStackInstance.cpp`

This task modifies the PCL-dependent pipeline code. It won't be testable in the unit test build (PCL link required), but the alignment components themselves are fully tested in Tasks 1-6.

**Step 1: Modify FrameLoader to return raw frames + metadata separately**

Add a new method to `FrameLoader` that loads frames into a temporary buffer (vector of float arrays) instead of directly into a SubCube. The existing `Load()` stays for backwards compatibility.

In `src/engine/FrameLoader.h`, add:

```cpp
#include "FrameAligner.h"

struct LoadedFrames {
    std::vector<std::vector<float>> pixelData;  // per-frame row-major pixel data
    std::vector<SubMetadata> metadata;           // per-frame metadata
    int width;
    int height;
};

// Load raw frame data without building SubCube (for alignment pipeline)
static LoadedFrames LoadRaw( const std::vector<FramePath>& frames );
```

**Step 2: Implement LoadRaw**

In `src/engine/FrameLoader.cpp`, add `LoadRaw()` — largely a copy of `Load()` but stores into `LoadedFrames` instead of SubCube:

```cpp
LoadedFrames FrameLoader::LoadRaw( const std::vector<FramePath>& frames )
{
    std::vector<const FramePath*> enabled;
    for ( const auto& f : frames )
        if ( f.enabled )
            enabled.push_back( &f );

    if ( enabled.empty() )
        throw pcl::Error( "FrameLoader: no enabled frames to load" );

    pcl::Console console;
    console.WriteLn( pcl::String().Format(
        "<end><cbr>FrameLoader: loading %d frames (raw)...", int(enabled.size()) ) );

    // Open first frame for dimensions
    pcl::String ext0 = pcl::File::ExtractExtension( enabled[0]->path ).Lowercase();
    pcl::FileFormat format0( ext0, true, false );
    pcl::FileFormatInstance file0( format0 );
    pcl::ImageDescriptionArray images0;
    if ( !file0.Open( images0, enabled[0]->path ) )
        throw pcl::Error( "FrameLoader: failed to open: " + enabled[0]->path );
    if ( images0.IsEmpty() ) { file0.Close(); throw pcl::Error( "No image data" ); }

    int refWidth = images0[0].info.width;
    int refHeight = images0[0].info.height;
    file0.Close();

    LoadedFrames result;
    result.width = refWidth;
    result.height = refHeight;
    result.pixelData.resize( enabled.size() );
    result.metadata.resize( enabled.size() );

    for ( size_t i = 0; i < enabled.size(); ++i )
    {
        const pcl::String& path = enabled[i]->path;
        pcl::String ext = pcl::File::ExtractExtension( path ).Lowercase();
        pcl::FileFormat format( ext, true, false );
        pcl::FileFormatInstance file( format );
        pcl::ImageDescriptionArray images;

        if ( !file.Open( images, path ) )
            throw pcl::Error( "FrameLoader: failed to open: " + path );
        if ( images.IsEmpty() ) { file.Close(); throw pcl::Error( "No image data" ); }

        int w = images[0].info.width;
        int h = images[0].info.height;
        if ( w != refWidth || h != refHeight ) {
            file.Close();
            throw pcl::Error( pcl::String().Format(
                "Dimension mismatch frame %d: %dx%d vs %dx%d",
                int(i+1), w, h, refWidth, refHeight ) + path );
        }

        pcl::FITSKeywordArray keywords;
        if ( format.CanStoreKeywords() )
            file.ReadFITSKeywords( keywords );

        pcl::Image img;
        if ( !file.ReadImage( img ) ) { file.Close(); throw pcl::Error( "Read failed" ); }
        file.Close();

        // Copy first channel to vector
        const pcl::Image::sample* src = img.PixelData( 0 );
        size_t numPx = size_t(refWidth) * refHeight;
        result.pixelData[i].assign( src, src + numPx );
        result.metadata[i] = ExtractMetadata( keywords );
    }

    return result;
}
```

**Step 3: Modify ExecuteGlobal to use alignment**

In `src/NukeXStackInstance.cpp`, replace Phase 1 with:

```cpp
// Phase 1: Load frames and align
Console().WriteLn( String().Format( "<br>Phase 1: Loading %d frames...", framePaths.size() ) );
nukex::LoadedFrames raw = nukex::FrameLoader::LoadRaw( framePaths );

Console().WriteLn( "<br>Phase 1b: Aligning frames..." );
std::vector<const float*> framePtrs;
for ( const auto& f : raw.pixelData )
    framePtrs.push_back( f.data() );

nukex::AlignmentOutput aligned = nukex::alignFrames(
    framePtrs, raw.width, raw.height );

Console().WriteLn( String().Format( "  Aligned %d frames, crop: %dx%d (from %dx%d)",
    aligned.offsets.size(),
    aligned.crop.width(), aligned.crop.height(),
    raw.width, raw.height ) );

// Copy metadata into aligned cube
for ( size_t i = 0; i < raw.metadata.size(); ++i )
    aligned.alignedCube.setMetadata( i, raw.metadata[i] );

nukex::SubCube cube = std::move( aligned.alignedCube );
```

**Step 4: Commit**

```bash
git add src/engine/FrameLoader.h src/engine/FrameLoader.cpp src/NukeXStackInstance.cpp
git commit -m "feat(alignment): integrate alignment into FrameLoader and ExecuteGlobal"
```

---

### Task 8: Run Full Test Suite and Verify

**Files:** None modified — verification only.

**Step 1: Build and run all tests**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -20
ctest --output-on-failure
```

Expected: All existing tests pass (test_sub_cube, test_quality_weights, test_numerical_utils, test_distribution_fitter, test_skew_normal_fitter, test_gaussian_mix_em, test_outlier_detector, test_pixel_selector, test_full_pipeline) plus the 3 new test targets (test_star_detector, test_triangle_matcher, test_frame_aligner).

**Step 2: If any failures, fix and re-run**

Fix compilation or logic errors, then re-run.

**Step 3: Commit any fixes**

```bash
git add -u && git commit -m "fix(alignment): test suite fixes"
```

---

## Summary

| Task | Component | Key Deliverable |
|------|-----------|----------------|
| 1 | StarDetector | Background stats (median + MAD) |
| 2 | StarDetector | Connected component blob extraction |
| 3 | StarDetector | Blob filtering, centroid, full `detectStars()` pipeline |
| 4 | TriangleMatcher | Triangle descriptors and building |
| 5 | TriangleMatcher | Frame matching with voting and translation |
| 6 | FrameAligner | Orchestrator with autocrop and SubCube output |
| 7 | Integration | Wire into FrameLoader + ExecuteGlobal pipeline |
| 8 | Verification | Full test suite green |

**Dependencies:** Tasks 1→2→3 (StarDetector), Task 4→5 (TriangleMatcher), then 3+5→6 (FrameAligner), then 6→7→8. Tasks 1-3 and 4-5 can run in parallel.
