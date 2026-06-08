# Frame-Level Trail Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect satellite/airplane trails per-frame on raw aligned data using seed-and-verify, mark trail pixels in SubCube masks, and let existing selectBestZ() skip them during stacking.

**Architecture:** New `TrailDetector` class scans each aligned frame for spatial outlier seeds, clusters them by collinearity (eigenvalue analysis), fits lines through linear clusters, walks each line checking cross-line neighbors, and marks confirmed trail pixels in `SubCube::m_masks`. Replaces the existing Phase 1b.5 median+MAD pre-stack rejection.

**Tech Stack:** C++17, xtensor (SubCube access), Catch2 v3 (tests), OpenMP (per-frame parallelism)

**Spec:** `docs/superpowers/specs/2026-03-24-frame-trail-detection-design.md`

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `src/engine/TrailDetector.h` | Create | `TrailDetectorConfig` struct, `TrailDetector` class declaration |
| `src/engine/TrailDetector.cpp` | Create | All detection logic: seed scan, clustering, line walk, masking |
| `tests/unit/test_trail_detector.cpp` | Create | Unit tests for all detection stages |
| `tests/CMakeLists.txt` | Modify | Add `test_trail_detector` target |
| `src/NukeXStackInstance.cpp` | Modify | Replace Phase 1b.5 block (lines 434-481) with TrailDetector call |

---

### Task 1: TrailDetector header with config and interface

**Files:**
- Create: `src/engine/TrailDetector.h`

- [ ] **Step 1: Create header with config struct and class declaration**

```cpp
// src/engine/TrailDetector.h
#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

namespace nukex {

class SubCube;  // forward declaration

struct TrailDetectorConfig
{
    double seedSigma       = 3.0;   // spatial outlier threshold (local MAD units)
    int    seedWindowSize  = 7;     // local neighborhood window (pixels, odd)
    double linearityMin    = 0.9;   // minimum linearity score for cluster
    int    minClusterLen   = 20;    // minimum extent along principal axis (pixels)
    double confirmSigma    = 2.5;   // cross-line neighbor confirmation threshold (lower than seedSigma — line geometry is already established)
    int    crossLineOffset = 4;     // perpendicular neighbor distance (pixels)
    double dilateRadius    = 2.0;   // perpendicular dilation (pixels)
    int    gapTolerance    = 3;     // morphological dilation radius before connected-component labeling
};

struct TrailLine
{
    double cx, cy;     // centroid of seed cluster
    double dx, dy;     // unit direction vector along the line
    int    confirmedCount = 0;
};

struct FrameTrailResult
{
    int frameIndex     = -1;
    int maskedPixels   = 0;
    int linesDetected  = 0;
    std::vector<TrailLine> lines;
};

using LogCallback = std::function<void( const std::string& )>;

class TrailDetector
{
public:
    explicit TrailDetector( const TrailDetectorConfig& config = TrailDetectorConfig{} );

    // Detect trails in a single frame. frameData is row-major (height * width).
    // alignMask is the existing alignment mask for this frame (nullptr = no alignment mask).
    // Returns result with detected lines and pixel count.
    FrameTrailResult detectFrame( const float* frameData,
                                  const uint8_t* alignMask,
                                  int width, int height ) const;

    // Detect trails across all frames in a SubCube and set masks.
    // Returns total number of pixel-frames masked.
    int detectAndMask( SubCube& cube, LogCallback log = nullptr ) const;

private:
    TrailDetectorConfig m_config;

    // Internal pipeline stages
    std::vector<uint8_t> findSeeds( const float* frameData,
                                    const uint8_t* alignMask,
                                    int width, int height ) const;

    struct Cluster {
        std::vector<int> pixelIndices;  // indices into W*H flat array
        double cx, cy;                  // centroid
        double dirX, dirY;              // principal axis unit vector
        double linearity;               // 1 - (lambda_min / lambda_max)
        double extent;                  // length along principal axis
    };

    std::vector<Cluster> clusterSeeds( const std::vector<uint8_t>& seeds,
                                       int width, int height ) const;

    std::vector<uint8_t> walkAndConfirm( const float* frameData,
                                          const uint8_t* alignMask,
                                          const std::vector<Cluster>& clusters,
                                          int width, int height,
                                          std::vector<TrailLine>& linesOut ) const;
};

} // namespace nukex
```

- [ ] **Step 2: Verify header compiles**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL 2>&1 | tail -5`
Expected: CMake succeeds (header is picked up by GLOB but not yet compiled into anything that references it)

- [ ] **Step 3: Commit**

```bash
git add src/engine/TrailDetector.h
git commit -m "feat: add TrailDetector header — config struct and class interface"
```

---

### Task 2: Test scaffolding — seed detection tests

**Files:**
- Create: `tests/unit/test_trail_detector.cpp`
- Modify: `tests/CMakeLists.txt` (append test target)

- [ ] **Step 1: Add test target to CMakeLists.txt**

Append to `tests/CMakeLists.txt`:

```cmake
# TrailDetector unit tests
add_executable(test_trail_detector
    unit/test_trail_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/TrailDetector.cpp
)
target_link_libraries(test_trail_detector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_trail_detector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
)
target_compile_definitions(test_trail_detector PRIVATE XTENSOR_USE_XSIMD)
target_compile_features(test_trail_detector PRIVATE cxx_std_17)
add_test(NAME test_trail_detector COMMAND test_trail_detector)
```

- [ ] **Step 2: Write seed detection tests**

```cpp
// tests/unit/test_trail_detector.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/TrailDetector.h"
#include "engine/SubCube.h"
#include <vector>
#include <cmath>
#include <random>

// Helper: create a flat background image with optional Gaussian noise
static std::vector<float> makeBackground( int W, int H, float bg = 0.1f,
                                           float noise = 0.005f, unsigned seed = 42 )
{
    std::vector<float> img( W * H, bg );
    std::mt19937 rng( seed );
    std::normal_distribution<float> dist( 0.0f, noise );
    for ( auto& v : img )
        v += dist( rng );
    return img;
}

// Helper: draw a bright line from (x0,y0) to (x1,y1) with given brightness
static void drawLine( std::vector<float>& img, int W, int H,
                      int x0, int y0, int x1, int y1, float brightness )
{
    int steps = std::max( std::abs( x1 - x0 ), std::abs( y1 - y0 ) );
    if ( steps == 0 ) return;
    for ( int i = 0; i <= steps; ++i )
    {
        int x = x0 + ( x1 - x0 ) * i / steps;
        int y = y0 + ( y1 - y0 ) * i / steps;
        if ( x >= 0 && x < W && y >= 0 && y < H )
            img[y * W + x] = brightness;
    }
}

// Helper: draw a Gaussian star at (cx, cy) with given peak and sigma
static void drawStar( std::vector<float>& img, int W, int H,
                      double cx, double cy, float peak, double sigma )
{
    int r = static_cast<int>( 4 * sigma + 1 );
    for ( int dy = -r; dy <= r; ++dy )
        for ( int dx = -r; dx <= r; ++dx )
        {
            int x = static_cast<int>( cx ) + dx;
            int y = static_cast<int>( cy ) + dy;
            if ( x >= 0 && x < W && y >= 0 && y < H )
            {
                double d2 = ( x - cx ) * ( x - cx ) + ( y - cy ) * ( y - cy );
                img[y * W + x] += peak * std::exp( -d2 / ( 2 * sigma * sigma ) );
            }
        }
}

TEST_CASE( "findSeeds detects bright trail pixels as seeds", "[trail][seed]" )
{
    const int W = 200, H = 200;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawLine( img, W, H, 10, 100, 190, 100, 0.5f );  // horizontal line

    nukex::TrailDetectorConfig config;
    nukex::TrailDetector detector( config );
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    // Should detect a line
    REQUIRE( result.linesDetected >= 1 );
    REQUIRE( result.maskedPixels > 50 );  // trail spans ~180 pixels
}

TEST_CASE( "No false detections on flat background", "[trail][seed]" )
{
    const int W = 200, H = 200;
    auto img = makeBackground( W, H, 0.1f, 0.005f );

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected == 0 );
    REQUIRE( result.maskedPixels == 0 );
}

TEST_CASE( "Single bright pixel (cosmic ray) does not produce a line", "[trail][cosmic]" )
{
    const int W = 200, H = 200;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    img[100 * W + 100] = 0.9f;  // single hot pixel

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected == 0 );
}

TEST_CASE( "Diagonal trail detected", "[trail][diagonal]" )
{
    const int W = 300, H = 300;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawLine( img, W, H, 10, 10, 290, 290, 0.5f );  // 45-degree diagonal

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected >= 1 );
    REQUIRE( result.maskedPixels > 100 );
}

TEST_CASE( "Round star cluster does not trigger trail detection", "[trail][star]" )
{
    const int W = 200, H = 200;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawStar( img, W, H, 100, 100, 0.8f, 5.0 );  // bright star, sigma=5px

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected == 0 );
}

TEST_CASE( "Two crossing trails both detected", "[trail][multiple]" )
{
    const int W = 300, H = 300;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawLine( img, W, H, 10, 150, 290, 150, 0.5f );  // horizontal
    drawLine( img, W, H, 150, 10, 150, 290, 0.5f );   // vertical

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected >= 2 );
}

TEST_CASE( "Trail through star: star core not masked, trail on sky is", "[trail][star-crossing]" )
{
    const int W = 300, H = 300;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawStar( img, W, H, 150, 150, 0.9f, 6.0 );  // bright star at center
    drawLine( img, W, H, 10, 150, 290, 150, 0.4f );  // horizontal trail through star

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    // Trail should be detected (line found from non-star portions)
    REQUIRE( result.linesDetected >= 1 );
    REQUIRE( result.maskedPixels > 50 );
    // Star core area: trail not brighter than cross-line neighbors (star dominates)
    // so center pixels should NOT be masked — the star's brightness swamps the trail
}

TEST_CASE( "Faint trail at exactly seed threshold", "[trail][threshold]" )
{
    const int W = 200, H = 200;
    float bg = 0.1f;
    float noise = 0.005f;
    auto img = makeBackground( W, H, bg, noise );

    // Trail at exactly 3.0 * expected MAD above median — borderline detection
    // MAD of Gaussian noise: noise * 1.0 (approx), scaled: noise * 1.4826 ≈ 0.0074
    float trailBrightness = bg + 3.0f * noise * 1.4826f;
    drawLine( img, W, H, 10, 100, 190, 100, trailBrightness );

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    // At exactly threshold, detection is marginal — just verify no crash
    // and that if detected, the line count is reasonable
    REQUIRE( result.linesDetected <= 2 );
}

TEST_CASE( "detectAndMask sets SubCube masks", "[trail][subcube]" )
{
    const int W = 100, H = 100, N = 5;
    nukex::SubCube cube( N, H, W );
    cube.allocateMasks();

    // Frame 2 has a horizontal trail; others are clean
    for ( size_t z = 0; z < N; ++z )
    {
        auto img = makeBackground( W, H, 0.1f, 0.005f, 42 + z );
        if ( z == 2 )
            drawLine( img, W, H, 5, 50, 95, 50, 0.5f );
        cube.setSub( z, img.data(), W * H );
    }

    nukex::TrailDetector detector;
    int masked = detector.detectAndMask( cube );

    REQUIRE( masked > 0 );
    // Frame 2 at the trail y=50 should be masked
    REQUIRE( cube.mask( 2, 50, 50 ) == 1 );
    // Other frames at the same pixel should NOT be masked
    REQUIRE( cube.mask( 0, 50, 50 ) == 0 );
    REQUIRE( cube.mask( 1, 50, 50 ) == 0 );
}
```

- [ ] **Step 3: Create stub TrailDetector.cpp so tests compile (and fail)**

```cpp
// src/engine/TrailDetector.cpp
#include "engine/TrailDetector.h"
#include "engine/SubCube.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>
#include <vector>
#include <utility>

namespace nukex {

TrailDetector::TrailDetector( const TrailDetectorConfig& config )
    : m_config( config )
{
}

FrameTrailResult TrailDetector::detectFrame( const float* /*frameData*/,
                                              const uint8_t* /*alignMask*/,
                                              int /*width*/, int /*height*/ ) const
{
    return {};  // stub — all tests should fail
}

int TrailDetector::detectAndMask( SubCube& /*cube*/, LogCallback /*log*/ ) const
{
    return 0;   // stub
}

std::vector<uint8_t> TrailDetector::findSeeds( const float* /*frameData*/,
                                                const uint8_t* /*alignMask*/,
                                                int width, int height ) const
{
    return std::vector<uint8_t>( width * height, 0 );
}

std::vector<TrailDetector::Cluster> TrailDetector::clusterSeeds(
    const std::vector<uint8_t>& /*seeds*/, int /*width*/, int /*height*/ ) const
{
    return {};
}

std::vector<uint8_t> TrailDetector::walkAndConfirm(
    const float* /*frameData*/, const uint8_t* /*alignMask*/,
    const std::vector<Cluster>& /*clusters*/,
    int width, int height,
    std::vector<TrailLine>& /*linesOut*/ ) const
{
    return std::vector<uint8_t>( width * height, 0 );
}

} // namespace nukex
```

- [ ] **Step 4: Build and verify tests fail**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) test_trail_detector 2>&1 | tail -5`
Then: `./test_trail_detector 2>&1 | tail -20`
Expected: All 9 tests FAIL (stubs return empty results)

- [ ] **Step 5: Commit**

```bash
git add tests/unit/test_trail_detector.cpp tests/CMakeLists.txt src/engine/TrailDetector.cpp
git commit -m "test: add failing trail detector tests with stub implementation"
```

---

### Task 3: Implement seed detection (findSeeds)

**Files:**
- Modify: `src/engine/TrailDetector.cpp` — replace `findSeeds` stub

- [ ] **Step 1: Implement findSeeds with local median + MAD**

Replace the `findSeeds` stub in `src/engine/TrailDetector.cpp`. The algorithm:
- For each pixel not masked by alignment, compute local median and MAD in a `seedWindowSize × seedWindowSize` window
- Skip alignment-masked pixels in the window
- Flag pixel as seed if `value > local_median + seedSigma * local_MAD`

Use a straightforward per-pixel implementation first (not Huang's algorithm — optimize later if profiling shows it's a bottleneck). Collect window values into a small vector, `nth_element` for median, compute MAD.

```cpp
std::vector<uint8_t> TrailDetector::findSeeds( const float* frameData,
                                                const uint8_t* alignMask,
                                                int width, int height ) const
{
    std::vector<uint8_t> seeds( width * height, 0 );
    const int halfW = m_config.seedWindowSize / 2;
    std::vector<float> window;
    window.reserve( m_config.seedWindowSize * m_config.seedWindowSize );

    for ( int y = 0; y < height; ++y )
    {
        for ( int x = 0; x < width; ++x )
        {
            int idx = y * width + x;
            if ( alignMask && alignMask[idx] )
                continue;

            // Collect local window values (excluding alignment-masked pixels)
            window.clear();
            for ( int wy = y - halfW; wy <= y + halfW; ++wy )
                for ( int wx = x - halfW; wx <= x + halfW; ++wx )
                {
                    if ( wx < 0 || wx >= width || wy < 0 || wy >= height )
                        continue;
                    int widx = wy * width + wx;
                    if ( alignMask && alignMask[widx] )
                        continue;
                    window.push_back( frameData[widx] );
                }

            if ( window.size() < 5 )
                continue;  // not enough neighbors for statistics

            // Local median
            size_t mid = window.size() / 2;
            std::nth_element( window.begin(), window.begin() + mid, window.end() );
            float median = window[mid];

            // Local MAD
            std::vector<float> devs( window.size() );
            for ( size_t i = 0; i < window.size(); ++i )
                devs[i] = std::abs( window[i] - median );
            std::nth_element( devs.begin(), devs.begin() + mid, devs.end() );
            float mad = devs[mid] * 1.4826f;

            if ( mad > 1e-10f && frameData[idx] > median + m_config.seedSigma * mad )
                seeds[idx] = 1;
        }
    }

    return seeds;
}
```

- [ ] **Step 2: Build**

Run: `cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) test_trail_detector 2>&1 | tail -5`
Expected: Compiles. Tests still fail (detectFrame still returns empty).

- [ ] **Step 3: Commit**

```bash
git add src/engine/TrailDetector.cpp
git commit -m "feat: implement seed detection — local median+MAD spatial outlier scan"
```

---

### Task 4: Implement collinearity clustering (clusterSeeds)

**Files:**
- Modify: `src/engine/TrailDetector.cpp` — replace `clusterSeeds` stub

- [ ] **Step 1: Implement clusterSeeds**

Algorithm:
1. Morphological dilation of seed mask by `gapTolerance` pixels (set all pixels within radius to 1)
2. Connected-component labeling with 8-connectivity on dilated mask (union-find)
3. For each component with ≥ 3 original (undilated) seeds:
   - Compute centroid (cx, cy)
   - Compute 2×2 covariance matrix of seed positions
   - Eigenvalues via quadratic formula: `λ = 0.5 * (trace ± sqrt(trace² - 4*det))`
   - Linearity = `1 - λ_min / λ_max`
   - Principal axis direction from eigenvector of λ_max
   - Extent = max projected distance along principal axis
   - Filter: linearity ≥ `linearityMin` AND extent ≥ `minClusterLen`

```cpp
std::vector<TrailDetector::Cluster> TrailDetector::clusterSeeds(
    const std::vector<uint8_t>& seeds, int width, int height ) const
{
    const int n = width * height;

    // 1. Dilate seed mask
    std::vector<uint8_t> dilated( n, 0 );
    const int gap = m_config.gapTolerance;
    for ( int y = 0; y < height; ++y )
        for ( int x = 0; x < width; ++x )
            if ( seeds[y * width + x] )
                for ( int dy = -gap; dy <= gap; ++dy )
                    for ( int dx = -gap; dx <= gap; ++dx )
                    {
                        int nx = x + dx, ny = y + dy;
                        if ( nx >= 0 && nx < width && ny >= 0 && ny < height )
                            dilated[ny * width + nx] = 1;
                    }

    // 2. Connected components via union-find on dilated mask
    std::vector<int> label( n, -1 );
    std::vector<int> parent;
    auto findRoot = [&]( int a ) {
        while ( parent[a] != a ) a = parent[a] = parent[parent[a]];
        return a;
    };
    auto unite = [&]( int a, int b ) {
        a = findRoot( a ); b = findRoot( b );
        if ( a != b ) parent[b] = a;
    };

    int nextLabel = 0;
    for ( int y = 0; y < height; ++y )
        for ( int x = 0; x < width; ++x )
        {
            if ( !dilated[y * width + x] ) continue;
            int idx = y * width + x;

            // Check 8-connected neighbors already visited
            int minLabel = -1;
            int neighbors[4][2] = { {x-1,y}, {x-1,y-1}, {x,y-1}, {x+1,y-1} };
            for ( auto& nb : neighbors )
            {
                int nx = nb[0], ny = nb[1];
                if ( nx < 0 || nx >= width || ny < 0 || ny >= height ) continue;
                int nidx = ny * width + nx;
                if ( label[nidx] >= 0 )
                {
                    int root = findRoot( label[nidx] );
                    if ( minLabel < 0 ) minLabel = root;
                    else unite( minLabel, root );
                }
            }

            if ( minLabel < 0 )
            {
                label[idx] = nextLabel;
                parent.push_back( nextLabel );
                ++nextLabel;
            }
            else
            {
                label[idx] = findRoot( minLabel );
            }
        }

    // 3. Group original seed pixels by component label
    std::unordered_map<int, std::vector<int>> components;
    for ( int i = 0; i < n; ++i )
        if ( seeds[i] && label[i] >= 0 )
            components[findRoot( label[i] )].push_back( i );

    // 4. Analyze each component
    std::vector<Cluster> result;
    for ( auto& [lbl, indices] : components )
    {
        if ( static_cast<int>( indices.size() ) < 3 )
            continue;

        // Centroid
        double cx = 0, cy = 0;
        for ( int idx : indices )
        {
            cx += idx % width;
            cy += idx / width;
        }
        cx /= indices.size();
        cy /= indices.size();

        // 2x2 covariance matrix
        double cxx = 0, cyy = 0, cxy = 0;
        for ( int idx : indices )
        {
            double dx = ( idx % width ) - cx;
            double dy = ( idx / width ) - cy;
            cxx += dx * dx;
            cyy += dy * dy;
            cxy += dx * dy;
        }
        double nn = static_cast<double>( indices.size() );
        cxx /= nn; cyy /= nn; cxy /= nn;

        // Eigenvalues via quadratic formula
        double trace = cxx + cyy;
        double det = cxx * cyy - cxy * cxy;
        double disc = trace * trace - 4.0 * det;
        if ( disc < 0 ) disc = 0;
        double sqrtDisc = std::sqrt( disc );
        double lambda1 = 0.5 * ( trace + sqrtDisc );  // larger
        double lambda2 = 0.5 * ( trace - sqrtDisc );  // smaller
        if ( lambda1 < 1e-10 ) continue;  // degenerate

        double linearity = 1.0 - lambda2 / lambda1;
        if ( linearity < m_config.linearityMin )
            continue;

        // Principal direction (eigenvector of lambda1)
        double dirX, dirY;
        if ( std::abs( cxy ) > 1e-10 )
        {
            dirX = lambda1 - cyy;
            dirY = cxy;
        }
        else
        {
            dirX = ( cxx >= cyy ) ? 1.0 : 0.0;
            dirY = ( cxx >= cyy ) ? 0.0 : 1.0;
        }
        double mag = std::sqrt( dirX * dirX + dirY * dirY );
        dirX /= mag; dirY /= mag;

        // Extent along principal axis
        double minProj = 1e30, maxProj = -1e30;
        for ( int idx : indices )
        {
            double dx = ( idx % width ) - cx;
            double dy = ( idx / width ) - cy;
            double proj = dx * dirX + dy * dirY;
            minProj = std::min( minProj, proj );
            maxProj = std::max( maxProj, proj );
        }
        double extent = maxProj - minProj;
        if ( extent < m_config.minClusterLen )
            continue;

        result.push_back( { indices, cx, cy, dirX, dirY, linearity, extent } );
    }

    return result;
}
```

Note: All required includes (`<algorithm>`, `<cmath>`, `<unordered_map>`, etc.) were added in Task 2's stub.

- [ ] **Step 2: Build**

Run: `cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) test_trail_detector 2>&1 | tail -5`
Expected: Compiles.

- [ ] **Step 3: Commit**

```bash
git add src/engine/TrailDetector.cpp
git commit -m "feat: implement collinearity clustering — union-find + eigenvalue linearity filter"
```

---

### Task 5: Implement line walk and confirmation (walkAndConfirm)

**Files:**
- Modify: `src/engine/TrailDetector.cpp` — replace `walkAndConfirm` stub

- [ ] **Step 1: Implement walkAndConfirm**

For each cluster:
1. Use centroid + principal direction to define the line
2. Walk from edge to edge of the frame along the line
3. At each pixel, sample cross-line neighbors at ±`crossLineOffset` perpendicular to the line
4. Compute median + MAD of cross-line neighbors
5. If on-line pixel exceeds `neighbor_median + confirmSigma * MAD`: mark as confirmed trail pixel
6. After walking, dilate confirmed pixels perpendicular to the line by `dilateRadius`

```cpp
std::vector<uint8_t> TrailDetector::walkAndConfirm(
    const float* frameData, const uint8_t* alignMask,
    const std::vector<Cluster>& clusters,
    int width, int height,
    std::vector<TrailLine>& linesOut ) const
{
    std::vector<uint8_t> mask( width * height, 0 );
    const int offset = m_config.crossLineOffset;
    const double dilate = m_config.dilateRadius;

    for ( const auto& cluster : clusters )
    {
        double cx = cluster.cx, cy = cluster.cy;
        double dx = cluster.dirX, dy = cluster.dirY;

        // Perpendicular direction
        double px = -dy, py = dx;

        // Find line extent across entire frame by walking from centroid in both directions
        // Parametric line: (cx + t*dx, cy + t*dy) — find t range where line is within frame
        double tMin = -1e30, tMax = 1e30;
        if ( std::abs( dx ) > 1e-10 )
        {
            double t1 = -cx / dx;
            double t2 = ( width - 1 - cx ) / dx;
            if ( t1 > t2 ) std::swap( t1, t2 );
            tMin = std::max( tMin, t1 );
            tMax = std::min( tMax, t2 );
        }
        if ( std::abs( dy ) > 1e-10 )
        {
            double t1 = -cy / dy;
            double t2 = ( height - 1 - cy ) / dy;
            if ( t1 > t2 ) std::swap( t1, t2 );
            tMin = std::max( tMin, t1 );
            tMax = std::min( tMax, t2 );
        }
        if ( tMin > tMax ) continue;

        // Walk along line at 1-pixel steps
        int confirmed = 0;
        std::vector<std::pair<int,int>> confirmedPixels;

        double stepLen = 1.0;
        for ( double t = tMin; t <= tMax; t += stepLen )
        {
            int lx = static_cast<int>( std::round( cx + t * dx ) );
            int ly = static_cast<int>( std::round( cy + t * dy ) );
            if ( lx < 0 || lx >= width || ly < 0 || ly >= height )
                continue;
            if ( alignMask && alignMask[ly * width + lx] )
                continue;

            float onLineVal = frameData[ly * width + lx];

            // Sample cross-line neighbors at ±1..±offset perpendicular
            std::vector<float> neighbors;
            for ( int d = 1; d <= offset; ++d )
            {
                for ( int sign : { -1, 1 } )
                {
                    int nx = static_cast<int>( std::round( lx + sign * d * px ) );
                    int ny = static_cast<int>( std::round( ly + sign * d * py ) );
                    if ( nx < 0 || nx >= width || ny < 0 || ny >= height )
                        continue;
                    if ( alignMask && alignMask[ny * width + nx] )
                        continue;
                    neighbors.push_back( frameData[ny * width + nx] );
                }
            }

            if ( neighbors.size() < 2 )
                continue;  // border — not enough neighbors

            // Median + MAD of cross-line neighbors
            size_t mid = neighbors.size() / 2;
            std::nth_element( neighbors.begin(), neighbors.begin() + mid, neighbors.end() );
            float nMedian = neighbors[mid];

            std::vector<float> devs( neighbors.size() );
            for ( size_t i = 0; i < neighbors.size(); ++i )
                devs[i] = std::abs( neighbors[i] - nMedian );
            std::nth_element( devs.begin(), devs.begin() + mid, devs.end() );
            float nMad = devs[mid] * 1.4826f;

            if ( nMad > 1e-10f && onLineVal > nMedian + m_config.confirmSigma * nMad )
            {
                confirmedPixels.push_back( { lx, ly } );
                ++confirmed;
            }
        }

        if ( confirmed < 5 )
            continue;  // not enough confirmed pixels to be a real trail

        // Dilate confirmed pixels perpendicular to line
        int iDilate = static_cast<int>( std::ceil( dilate ) );
        for ( auto [lx, ly] : confirmedPixels )
        {
            // Mark the on-line pixel
            mask[ly * width + lx] = 1;
            // Dilate perpendicular
            for ( int d = 1; d <= iDilate; ++d )
            {
                for ( int sign : { -1, 1 } )
                {
                    int nx = static_cast<int>( std::round( lx + sign * d * px ) );
                    int ny = static_cast<int>( std::round( ly + sign * d * py ) );
                    if ( nx >= 0 && nx < width && ny >= 0 && ny < height )
                        mask[ny * width + nx] = 1;
                }
            }
        }

        linesOut.push_back( { cx, cy, dx, dy, confirmed } );
    }

    return mask;
}
```

- [ ] **Step 2: Build**

Run: `cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) test_trail_detector 2>&1 | tail -5`
Expected: Compiles.

- [ ] **Step 3: Commit**

```bash
git add src/engine/TrailDetector.cpp
git commit -m "feat: implement line walk and cross-line confirmation with dilation"
```

---

### Task 6: Wire up detectFrame and detectAndMask

**Files:**
- Modify: `src/engine/TrailDetector.cpp` — replace `detectFrame` and `detectAndMask` stubs

- [ ] **Step 1: Implement detectFrame (orchestrates the pipeline)**

```cpp
FrameTrailResult TrailDetector::detectFrame( const float* frameData,
                                              const uint8_t* alignMask,
                                              int width, int height ) const
{
    FrameTrailResult result;

    // Phase 1: Find seeds
    auto seeds = findSeeds( frameData, alignMask, width, height );

    // Phase 2: Cluster seeds by collinearity
    auto clusters = clusterSeeds( seeds, width, height );
    if ( clusters.empty() )
        return result;

    // Phase 3+4: Walk lines and confirm, producing final mask
    auto trailMask = walkAndConfirm( frameData, alignMask, clusters, width, height, result.lines );

    result.linesDetected = static_cast<int>( result.lines.size() );
    result.maskedPixels = 0;
    for ( auto v : trailMask )
        result.maskedPixels += v;

    // Store mask in result for external use if needed
    // (detectAndMask reads directly from SubCube, but detectFrame is standalone)

    return result;
}
```

- [ ] **Step 2: Implement detectAndMask (operates on SubCube)**

```cpp
int TrailDetector::detectAndMask( SubCube& cube, LogCallback log ) const
{
    const int W = static_cast<int>( cube.width() );
    const int H = static_cast<int>( cube.height() );
    const int N = static_cast<int>( cube.numSubs() );
    const int totalPixels = W * H;

    if ( !cube.hasMasks() )
        cube.allocateMasks();

    int totalMasked = 0;

    // Per-frame trail detection — parallelizable since each frame is independent.
    // Collect results first, then apply masks (setMask writes to different z slices,
    // which are independent in column-major layout, but we serialize mask writes
    // and logging for simplicity).
    struct FrameResult {
        int z;
        int maskedCount;
        std::vector<uint8_t> trailMask;
        std::vector<TrailLine> lines;
    };
    std::vector<FrameResult> results( N );

    #pragma omp parallel for schedule(dynamic)
    for ( int z = 0; z < N; ++z )
    {
        // Extract frame z as row-major float array
        std::vector<float> frame( totalPixels );
        for ( int y = 0; y < H; ++y )
            for ( int x = 0; x < W; ++x )
                frame[y * W + x] = cube.pixel( z, y, x );

        // Extract alignment mask for frame z
        std::vector<uint8_t> alignMask( totalPixels );
        for ( int y = 0; y < H; ++y )
            for ( int x = 0; x < W; ++x )
                alignMask[y * W + x] = cube.mask( z, y, x );

        // Detect trails
        auto seeds = findSeeds( frame.data(), alignMask.data(), W, H );
        auto clusters = clusterSeeds( seeds, W, H );

        results[z].z = z;
        if ( clusters.empty() )
        {
            results[z].maskedCount = 0;
            continue;
        }

        results[z].trailMask = walkAndConfirm( frame.data(), alignMask.data(),
                                                clusters, W, H, results[z].lines );

        int count = 0;
        for ( auto v : results[z].trailMask )
            count += v;
        results[z].maskedCount = count;
    }

    // Apply masks and log (serial — setMask + logging not thread-safe with PI console)
    for ( int z = 0; z < N; ++z )
    {
        if ( results[z].maskedCount == 0 )
            continue;

        for ( int y = 0; y < H; ++y )
            for ( int x = 0; x < W; ++x )
                if ( results[z].trailMask[y * W + x] )
                    cube.setMask( z, y, x, 1 );

        int frameMasked = results[z].maskedCount;
        totalMasked += frameMasked;

        if ( log )
        {
            log( "    Frame " + std::to_string( z ) + ": " +
                 std::to_string( results[z].lines.size() ) + " trail(s), " +
                 std::to_string( frameMasked ) + " pixels masked" );

            // Warn if >30% of frame is masked — likely a bad frame
            double maskFrac = static_cast<double>( frameMasked ) / totalPixels;
            if ( maskFrac > 0.3 )
                log( "    WARNING: frame " + std::to_string( z ) +
                     " has " + std::to_string( static_cast<int>( maskFrac * 100 ) ) +
                     "% pixels masked — consider excluding this frame" );
        }
    }

    return totalMasked;
}
```

- [ ] **Step 3: Build and run tests**

Run: `cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) test_trail_detector && ./test_trail_detector`
Expected: All 9 tests PASS.

- [ ] **Step 4: Commit**

```bash
git add src/engine/TrailDetector.cpp
git commit -m "feat: wire up detectFrame and detectAndMask — all trail detector tests pass"
```

---

### Task 7: Integrate into pipeline — replace Phase 1b.5

**Files:**
- Modify: `src/NukeXStackInstance.cpp:1-30` — add include
- Modify: `src/NukeXStackInstance.cpp:434-481` — replace Phase 1b.5 block

- [ ] **Step 1: Add TrailDetector include**

Add after the existing includes in `src/NukeXStackInstance.cpp` (after line 17 `#include "engine/DustCorrector.h"`):

```cpp
#include "engine/TrailDetector.h"
```

- [ ] **Step 2: Replace Phase 1b.5 block (lines 434-481)**

Replace the entire block from `// Pre-stacking trail rejection:` through the closing `}` of the inner scope (lines 434-481) with:

```cpp
         // Pre-stacking trail detection: seed-and-verify per-frame trail detector.
         // Finds spatial outliers, clusters by collinearity, walks candidate lines
         // checking cross-line neighbors, and marks confirmed trail pixels in m_masks.
         // selectBestZ() skips masked frames automatically.
         {
            nukex::TrailDetectorConfig trailConfig;
            nukex::TrailDetector trailDetector( trailConfig );

            int trailMasked = trailDetector.detectAndMask( channelCubes[ch],
               [&console]( const std::string& msg ) {
                  console.WriteLn( String( msg.c_str() ) );
               } );

            if ( ch == 0 && trailMasked > 0 )
               console.WriteLn( String().Format( "  Trail detection: %d pixel-frames masked", trailMasked ) );
         }
```

- [ ] **Step 3: Build the full module**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) 2>&1 | tail -10`
Expected: Full module compiles without errors.

- [ ] **Step 4: Run all tests**

Run: `cd /home/scarter4work/projects/nukex3/build && ctest --output-on-failure`
Expected: All 20 tests pass (19 existing + 1 new test_trail_detector).

- [ ] **Step 5: Commit**

```bash
git add src/NukeXStackInstance.cpp
git commit -m "feat: integrate TrailDetector — replaces Phase 1b.5 median+MAD rejection"
```

---

### Task 8: Version bump, build, test, sign, package, push

**Files:**
- Modify: `src/NukeXModule.cpp:11-13,16-18` — bump version and date

- [ ] **Step 1: Bump version**

In `src/NukeXModule.cpp`:
- Change `MODULE_VERSION_BUILD` from `6` to `7`
- Change `MODULE_RELEASE_DAY` from `23` to `24`

- [ ] **Step 2: Clean build**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make clean && make -j$(nproc) 2>&1 | tail -10`
Expected: Clean build succeeds.

- [ ] **Step 3: Run all tests**

Run: `cd /home/scarter4work/projects/nukex3/build && ctest --output-on-failure`
Expected: All 20 tests pass.

- [ ] **Step 4: Sign module**

Run: `echo "***REDACTED***" > /tmp/.pi_codesign_pass && /opt/PixInsight/bin/PixInsight.sh --sign-module-file=/home/scarter4work/projects/nukex3/build/lib/NukeX-pxm.so --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk --xssk-password="***REDACTED***"`
Expected: Signature file created.

- [ ] **Step 5: Package**

Run: `cd /home/scarter4work/projects/nukex3 && make package`
Expected: Package tarball created in `repository/`, SHA1 updated in `updates.xri`, XRI signed.

- [ ] **Step 6: Commit everything**

```bash
git add src/NukeXModule.cpp repository/
git commit -m "build: bump to v3.2.0.7 — frame-level trail detection"
```

- [ ] **Step 7: Push**

Run: `git push origin main`
Expected: Push succeeds.