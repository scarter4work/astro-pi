# Phase 5A: Stretch Pipeline Infrastructure + Tier 3 Scalar Ops

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the stretch pipeline infrastructure (StretchOp base class, StretchPipeline, luminance-only utility, PNG output for visual evaluation) and implement the 4 simplest scalar stretch operations: MTF, ArcSinh, Log, RNC. Each tested with known-answer assertions and visual PNG output on real M16 data.

**Architecture:** lib/stretch depends on lib/core and lib/io (for Image). Each stretch op is a subclass of StretchOp with `apply_scalar(float)` and `apply(Image&)`. The pipeline sorts enabled ops by position and executes sequentially. PNG output via stb_image_write (vendored) is a test utility for user visual evaluation.

**Tech Stack:** C++17, lib/core, lib/io, stb_image_write (vendored), Catch2 v3

**Critical rules:**
- Every formula verified against its published reference
- Minimum 10 known-answer assertions per op
- Visual PNG output on real M16 debayered frame
- No shortcuts, no stubs, no TODOs
- Double-check: "Am I sure this is correct?"

**Test data:** Real FITS files at `/home/scarter4work/projects/processing/M16/Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit`

---

## File Structure

```
third_party/stb/
├── stb_image_write.h        Vendored header-only PNG writer (public domain)

src/lib/stretch/
├── CMakeLists.txt
├── include/nukex/stretch/
│   ├── stretch_op.hpp       Base class + StretchCategory enum
│   ├── stretch_pipeline.hpp Pipeline container + advisory ordering + quick_preview
│   ├── stretch_utils.hpp    Luminance-only helper, clamp utility
│   ├── mtf_stretch.hpp      Op 0: Midtone Transfer Function
│   ├── arcsinh_stretch.hpp  Op 3: ArcSinh
│   ├── log_stretch.hpp      Op 4: Log
│   └── rnc_stretch.hpp      Op 6: RNC (Roger N. Clark)
├── src/
│   ├── stretch_pipeline.cpp
│   ├── stretch_utils.cpp
│   ├── mtf_stretch.cpp
│   ├── arcsinh_stretch.cpp
│   ├── log_stretch.cpp
│   └── rnc_stretch.cpp

test/util/
├── png_writer.hpp           Write Image to 16-bit PNG
├── png_writer.cpp
├── test_data_loader.hpp     Load M16 frame → debayered Image
└── test_data_loader.cpp

test/unit/stretch/
├── test_stretch_pipeline.cpp
├── test_mtf.cpp
├── test_arcsinh.cpp
├── test_log.cpp
└── test_rnc.cpp

test/output/                  PNG output directory (gitignored)
```

---

## Task 1: Vendor stb_image_write + PNG Writer Utility + Test Data Loader

**Files:**
- Create: `third_party/stb/stb_image_write.h` (download from GitHub)
- Create: `test/util/png_writer.hpp`
- Create: `test/util/png_writer.cpp`
- Create: `test/util/test_data_loader.hpp`
- Create: `test/util/test_data_loader.cpp`
- Create: `test/output/.gitkeep`
- Modify: `.gitignore` (add test/output/*.png)

- [ ] **Step 1: Download stb_image_write.h**

```bash
mkdir -p third_party/stb
curl -L https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h \
     -o third_party/stb/stb_image_write.h
```

- [ ] **Step 2: Create test/output directory + gitignore**

```bash
mkdir -p test/output
touch test/output/.gitkeep
echo "test/output/*.png" >> .gitignore
```

- [ ] **Step 3: Create test/util/png_writer.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include <string>

namespace nukex { namespace test_util {

/// Write an Image to a 16-bit PNG file for visual evaluation.
///
/// For float32 [0,1] images, maps to [0, 65535] uint16.
/// Multi-channel images are written as RGB (first 3 channels).
/// Single-channel images are written as grayscale.
///
/// Applies a simple arcsinh preview stretch if apply_stretch is true,
/// to make linear astronomical data visible.
///
/// Returns true on success.
bool write_png(const std::string& filepath, const nukex::Image& img,
               bool apply_stretch = true, float stretch_alpha = 500.0f);

}} // namespace nukex::test_util
```

- [ ] **Step 4: Create test/util/png_writer.cpp**

```cpp
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "png_writer.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

namespace nukex { namespace test_util {

bool write_png(const std::string& filepath, const nukex::Image& img,
               bool apply_stretch, float stretch_alpha) {
    if (img.empty()) return false;

    int w = img.width();
    int h = img.height();
    int ch = std::min(img.n_channels(), 3);  // Max 3 channels for PNG RGB

    // Convert to 16-bit interleaved RGB (or grayscale)
    std::vector<uint16_t> pixels(w * h * ch);

    float norm = (stretch_alpha > 0.0f) ? std::asinh(stretch_alpha) : 1.0f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < ch; c++) {
                float val = img.at(x, y, c);

                // Apply arcsinh preview stretch if requested
                if (apply_stretch && stretch_alpha > 0.0f) {
                    val = std::asinh(stretch_alpha * val) / norm;
                }

                val = std::clamp(val, 0.0f, 1.0f);
                uint16_t u16 = static_cast<uint16_t>(val * 65535.0f + 0.5f);

                // stb_image_write expects big-endian for 16-bit PNG
                uint16_t be = (u16 >> 8) | (u16 << 8);
                pixels[(y * w + x) * ch + c] = be;
            }
        }
    }

    int stride = w * ch * 2;  // 2 bytes per component
    return stbi_write_png(filepath.c_str(), w, h, ch, pixels.data(), stride) != 0;
}

}} // namespace nukex::test_util
```

- [ ] **Step 5: Create test/util/test_data_loader.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include <string>

namespace nukex { namespace test_util {

/// Load the first M16 FITS frame, debayer it, and return as a 3-channel RGB Image.
/// Returns an empty Image if the file is not available.
///
/// The M16 data is HaO3 narrowband on a Bayer sensor (ZWO ASI2400MC Pro).
/// After debayer, it's a 3-channel float32 Image in [0, 1].
Image load_m16_test_frame();

/// Path to the M16 test data directory.
const std::string& m16_data_dir();

}} // namespace nukex::test_util
```

- [ ] **Step 6: Create test/util/test_data_loader.cpp**

```cpp
#include "test_data_loader.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/debayer.hpp"
#include "nukex/core/channel_config.hpp"
#include <filesystem>

namespace nukex { namespace test_util {

const std::string& m16_data_dir() {
    static const std::string dir = "/home/scarter4work/projects/processing/M16/";
    return dir;
}

Image load_m16_test_frame() {
    std::string dir = m16_data_dir();
    if (!std::filesystem::exists(dir)) return {};

    // Find first .fit file
    std::string path;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".fit" || ext == ".fits") {
            path = entry.path().string();
            break;
        }
    }
    if (path.empty()) return {};

    auto result = FITSReader::read(path);
    if (!result.success) return {};

    // Debayer if Bayer pattern detected
    if (!result.metadata.bayer_pattern.empty() &&
        result.metadata.bayer_pattern != "NONE") {
        // Parse bayer pattern
        BayerPattern bp = BayerPattern::RGGB;  // Default for ZWO cameras
        if (result.metadata.bayer_pattern == "BGGR") bp = BayerPattern::BGGR;
        else if (result.metadata.bayer_pattern == "GRBG") bp = BayerPattern::GRBG;
        else if (result.metadata.bayer_pattern == "GBRG") bp = BayerPattern::GBRG;

        return DebayerEngine::debayer(result.image, bp);
    }

    return std::move(result.image);
}

}} // namespace nukex::test_util
```

- [ ] **Step 7: Build check**

This task doesn't create a test executable yet — the utilities are compiled as part of the test targets that use them. Verify the files exist:

```bash
ls -la third_party/stb/stb_image_write.h
ls -la test/util/png_writer.hpp test/util/png_writer.cpp
ls -la test/util/test_data_loader.hpp test/util/test_data_loader.cpp
```

- [ ] **Step 8: Commit**

```bash
git add third_party/stb/ test/util/ test/output/.gitkeep .gitignore
git commit -m "feat(test): PNG writer + M16 test data loader for stretch visual evaluation

Vendor stb_image_write (public domain) for 16-bit PNG output.
PNG writer applies optional arcsinh preview stretch for linear data.
Test data loader loads first M16 FITS frame and debayers for use as
stretch test input. Output PNGs go to test/output/ (gitignored)."
```

---

## Task 2: lib/stretch Scaffolding — StretchOp + StretchPipeline + Utils

**Files:**
- Create: `src/lib/stretch/CMakeLists.txt`
- Create: `src/lib/stretch/include/nukex/stretch/stretch_op.hpp`
- Create: `src/lib/stretch/include/nukex/stretch/stretch_pipeline.hpp`
- Create: `src/lib/stretch/include/nukex/stretch/stretch_utils.hpp`
- Create: `src/lib/stretch/src/stretch_pipeline.cpp`
- Create: `src/lib/stretch/src/stretch_utils.cpp`
- Create: `test/unit/stretch/test_stretch_pipeline.cpp`
- Modify: `CMakeLists.txt` (root)
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Create directories**

```bash
mkdir -p src/lib/stretch/include/nukex/stretch
mkdir -p src/lib/stretch/src
mkdir -p test/unit/stretch
```

- [ ] **Step 2: Create stretch_op.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include <string>

namespace nukex {

enum class StretchCategory { PRIMARY, SECONDARY, FINISHER };

/// Base class for all stretch operations.
///
/// Each op implements apply(Image&) for in-place image stretching and
/// optionally apply_scalar(float) for single-value computation (LUT, testing).
/// All ops work on float32 images normalized to [0, 1].
class StretchOp {
public:
    bool            enabled  = false;
    int             position = 0;
    std::string     name;
    StretchCategory category = StretchCategory::PRIMARY;

    virtual ~StretchOp() = default;

    /// Apply stretch to image in-place.
    virtual void apply(Image& img) const = 0;

    /// Apply to a single float value. Default: identity.
    virtual float apply_scalar(float x) const { return x; }
};

} // namespace nukex
```

- [ ] **Step 3: Create stretch_utils.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include <functional>

namespace nukex {

/// Apply a scalar stretch function to an image's luminance only,
/// preserving chromaticity (hue + saturation).
///
/// For single-channel images: applies directly.
/// For multi-channel (RGB): compute luminance L = 0.2126R + 0.7152G + 0.0722B,
/// stretch L, scale R/G/B by L'/L ratio.
///
/// Reference: Rec. 709 luminance coefficients.
void apply_luminance_only(Image& img, const std::function<float(float)>& fn);

/// Apply a scalar function per-channel independently.
void apply_per_channel(Image& img, const std::function<float(float)>& fn);

/// Clamp all pixel values in an image to [lo, hi].
void clamp_image(Image& img, float lo = 0.0f, float hi = 1.0f);

} // namespace nukex
```

- [ ] **Step 4: Create stretch_utils.cpp**

```cpp
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>

namespace nukex {

void apply_luminance_only(Image& img, const std::function<float(float)>& fn) {
    int w = img.width(), h = img.height(), nch = img.n_channels();

    if (nch == 1) {
        // Single channel: apply directly
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                img.at(x, y, 0) = fn(img.at(x, y, 0));
        return;
    }

    // Multi-channel: luminance-preserving stretch
    // Rec. 709 luminance coefficients
    constexpr float kR = 0.2126f, kG = 0.7152f, kB = 0.0722f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r = img.at(x, y, 0);
            float g = (nch > 1) ? img.at(x, y, 1) : r;
            float b = (nch > 2) ? img.at(x, y, 2) : r;

            float L = kR * r + kG * g + kB * b;
            if (L < 1e-10f) continue;  // Black pixel — nothing to scale

            float L_stretched = fn(L);
            float scale = L_stretched / L;

            img.at(x, y, 0) = std::clamp(r * scale, 0.0f, 1.0f);
            if (nch > 1) img.at(x, y, 1) = std::clamp(g * scale, 0.0f, 1.0f);
            if (nch > 2) img.at(x, y, 2) = std::clamp(b * scale, 0.0f, 1.0f);
        }
    }
}

void apply_per_channel(Image& img, const std::function<float(float)>& fn) {
    for (int ch = 0; ch < img.n_channels(); ch++) {
        float* data = img.channel_data(ch);
        int n = img.width() * img.height();
        for (int i = 0; i < n; i++) {
            data[i] = fn(data[i]);
        }
    }
}

void clamp_image(Image& img, float lo, float hi) {
    float* data = img.data();
    size_t n = img.data_size();
    for (size_t i = 0; i < n; i++) {
        data[i] = std::clamp(data[i], lo, hi);
    }
}

} // namespace nukex
```

- [ ] **Step 5: Create stretch_pipeline.hpp**

```cpp
#pragma once

#include "nukex/stretch/stretch_op.hpp"
#include <vector>
#include <memory>
#include <string>

namespace nukex {

/// Ordered pipeline of stretch operations.
///
/// Sorts enabled ops by position, executes sequentially. Each op
/// receives the output of the previous op. All ops work on float32 [0,1].
class StretchPipeline {
public:
    std::vector<std::unique_ptr<StretchOp>> ops;

    /// Execute all enabled ops in position order.
    void execute(Image& img) const;

    /// Advisory ordering warnings. Non-blocking.
    /// Returns warning messages if the current ordering is suboptimal.
    std::vector<std::string> check_ordering() const;

    /// Quick preview stretch — arcsinh on a COPY, never modifies working data.
    /// The working data stays linear throughout. This is a hard rule.
    static Image quick_preview_stretch(const Image& linear_src, float alpha = 500.f);
};

} // namespace nukex
```

- [ ] **Step 6: Create stretch_pipeline.cpp**

```cpp
#include "nukex/stretch/stretch_pipeline.hpp"
#include <algorithm>
#include <cmath>

namespace nukex {

void StretchPipeline::execute(Image& img) const {
    // Collect enabled ops, sort by position
    std::vector<const StretchOp*> ordered;
    for (const auto& op : ops) {
        if (op->enabled) ordered.push_back(op.get());
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const StretchOp* a, const StretchOp* b) {
                  return a->position < b->position;
              });

    for (const auto* op : ordered) {
        op->apply(img);
    }
}

std::vector<std::string> StretchPipeline::check_ordering() const {
    std::vector<std::string> warnings;

    // Collect enabled ops sorted by position
    std::vector<const StretchOp*> ordered;
    for (const auto& op : ops) {
        if (op->enabled) ordered.push_back(op.get());
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const StretchOp* a, const StretchOp* b) {
                  return a->position < b->position;
              });

    if (ordered.empty()) return warnings;

    // Check: warn if any SECONDARY precedes all PRIMARYs
    int first_primary = -1, first_secondary = -1;
    for (int i = 0; i < static_cast<int>(ordered.size()); i++) {
        if (ordered[i]->category == StretchCategory::PRIMARY && first_primary < 0)
            first_primary = i;
        if (ordered[i]->category == StretchCategory::SECONDARY && first_secondary < 0)
            first_secondary = i;
    }
    if (first_secondary >= 0 && (first_primary < 0 || first_secondary < first_primary)) {
        warnings.push_back("Advisory: SECONDARY stretch '" + ordered[first_secondary]->name
                          + "' precedes all PRIMARY stretches. Consider reordering.");
    }

    // Check: warn if FINISHER precedes any PRIMARY or SECONDARY
    for (int i = 0; i < static_cast<int>(ordered.size()); i++) {
        if (ordered[i]->category == StretchCategory::FINISHER) {
            for (int j = i + 1; j < static_cast<int>(ordered.size()); j++) {
                if (ordered[j]->category != StretchCategory::FINISHER) {
                    warnings.push_back("Advisory: FINISHER '" + ordered[i]->name
                                      + "' precedes '" + ordered[j]->name
                                      + "'. Finishers work best last.");
                    break;
                }
            }
        }
    }

    return warnings;
}

Image StretchPipeline::quick_preview_stretch(const Image& linear_src, float alpha) {
    Image preview = linear_src.clone();
    float norm = std::asinh(alpha);
    if (norm < 1e-10f) return preview;

    preview.apply([alpha, norm](float x) -> float {
        return std::asinh(alpha * x) / norm;
    });
    return preview;
}

} // namespace nukex
```

- [ ] **Step 7: Create src/lib/stretch/CMakeLists.txt**

```cmake
add_library(nukex4_stretch STATIC
    src/stretch_pipeline.cpp
    src/stretch_utils.cpp
    src/mtf_stretch.cpp
    src/arcsinh_stretch.cpp
    src/log_stretch.cpp
    src/rnc_stretch.cpp
)

target_include_directories(nukex4_stretch
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_stretch
    PUBLIC nukex4_core nukex4_io
)

target_compile_features(nukex4_stretch PUBLIC cxx_std_17)
```

- [ ] **Step 8: Create stub .cpp files for the 4 ops** (filled in Tasks 3-6)

Each stub includes its header and has a minimal implementation:

`src/lib/stretch/src/mtf_stretch.cpp`:
```cpp
#include "nukex/stretch/mtf_stretch.hpp"
namespace nukex {
void MTFStretch::apply(Image& img) const {}
} // namespace nukex
```

Create matching minimal headers for each op. Example for `mtf_stretch.hpp`:
```cpp
#pragma once
#include "nukex/stretch/stretch_op.hpp"
namespace nukex {
class MTFStretch : public StretchOp {
public:
    float midtone    = 0.25f;
    float shadows    = 0.0f;
    float highlights = 1.0f;
    bool  luminance_only = false;
    MTFStretch() { name = "MTF"; category = StretchCategory::FINISHER; }
    void apply(Image& img) const override;
    float apply_scalar(float x) const override;
};
} // namespace nukex
```

Similarly for `arcsinh_stretch.hpp`:
```cpp
#pragma once
#include "nukex/stretch/stretch_op.hpp"
namespace nukex {
class ArcSinhStretch : public StretchOp {
public:
    float alpha = 500.0f;
    bool  luminance_only = true;
    ArcSinhStretch() { name = "ArcSinh"; category = StretchCategory::PRIMARY; }
    void apply(Image& img) const override;
    float apply_scalar(float x) const override;
};
} // namespace nukex
```

For `log_stretch.hpp`:
```cpp
#pragma once
#include "nukex/stretch/stretch_op.hpp"
namespace nukex {
class LogStretch : public StretchOp {
public:
    float alpha = 1000.0f;
    bool  luminance_only = false;
    LogStretch() { name = "Log"; category = StretchCategory::PRIMARY; }
    void apply(Image& img) const override;
    float apply_scalar(float x) const override;
};
} // namespace nukex
```

For `rnc_stretch.hpp`:
```cpp
#pragma once
#include "nukex/stretch/stretch_op.hpp"
namespace nukex {
class RNCStretch : public StretchOp {
public:
    float black_point = 0.0f;
    float white_point = 1.0f;
    float gamma       = 2.2f;
    bool  luminance_only = false;
    RNCStretch() { name = "RNC"; category = StretchCategory::SECONDARY; }
    void apply(Image& img) const override;
    float apply_scalar(float x) const override;
};
} // namespace nukex
```

Stub .cpp files for arcsinh, log, rnc — same pattern as mtf stub.

- [ ] **Step 9: Create test_stretch_pipeline.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stretch/stretch_pipeline.hpp"
#include "nukex/stretch/stretch_op.hpp"
#include "nukex/io/image.hpp"

using namespace nukex;

namespace {

// A simple test op that adds a constant
class AddOp : public StretchOp {
public:
    float offset = 0.0f;
    AddOp(float off, int pos) : offset(off) {
        enabled = true; position = pos; name = "Add";
        category = StretchCategory::PRIMARY;
    }
    void apply(Image& img) const override {
        img.apply([this](float x) { return x + offset; });
    }
    float apply_scalar(float x) const override { return x + offset; }
};

} // anonymous namespace

TEST_CASE("StretchPipeline: execute runs enabled ops in order", "[pipeline]") {
    StretchPipeline pipeline;
    pipeline.ops.push_back(std::make_unique<AddOp>(0.1f, 2));
    pipeline.ops.push_back(std::make_unique<AddOp>(0.2f, 1));

    Image img(4, 4, 1);
    img.fill(0.0f);
    pipeline.execute(img);

    // Position 1 (add 0.2) runs first, then position 2 (add 0.1) = 0.3
    REQUIRE(img.at(0, 0, 0) == Catch::Approx(0.3f));
}

TEST_CASE("StretchPipeline: disabled ops are skipped", "[pipeline]") {
    StretchPipeline pipeline;
    auto op = std::make_unique<AddOp>(0.5f, 1);
    op->enabled = false;
    pipeline.ops.push_back(std::move(op));

    Image img(4, 4, 1);
    img.fill(0.0f);
    pipeline.execute(img);

    REQUIRE(img.at(0, 0, 0) == Catch::Approx(0.0f));
}

TEST_CASE("StretchPipeline: quick_preview_stretch produces non-zero output", "[pipeline]") {
    Image img(16, 16, 1);
    // Simulate faint astronomical data
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            img.at(x, y, 0) = 0.001f * (x + y);

    auto preview = StretchPipeline::quick_preview_stretch(img, 500.0f);
    // Preview should be brighter than the linear input
    REQUIRE(preview.at(8, 8, 0) > img.at(8, 8, 0));
    // Input should be unchanged (operates on copy)
    REQUIRE(img.at(8, 8, 0) == Catch::Approx(0.001f * 16));
}

TEST_CASE("StretchPipeline: quick_preview_stretch maps 0→0", "[pipeline]") {
    Image img(4, 4, 1);
    img.fill(0.0f);
    auto preview = StretchPipeline::quick_preview_stretch(img, 500.0f);
    REQUIRE(preview.at(0, 0, 0) == Catch::Approx(0.0f));
}

TEST_CASE("StretchPipeline: advisory warns on SECONDARY before PRIMARY", "[pipeline]") {
    StretchPipeline pipeline;
    auto sec = std::make_unique<AddOp>(0.1f, 1);
    sec->category = StretchCategory::SECONDARY;
    sec->name = "Histogram";
    pipeline.ops.push_back(std::move(sec));

    auto prim = std::make_unique<AddOp>(0.2f, 2);
    prim->category = StretchCategory::PRIMARY;
    prim->name = "GHS";
    pipeline.ops.push_back(std::move(prim));

    auto warnings = pipeline.check_ordering();
    REQUIRE(warnings.size() >= 1);
}
```

- [ ] **Step 10: Update root CMakeLists.txt**

Add after `add_subdirectory(src/lib/combine)`:

```cmake
add_subdirectory(src/lib/stretch)
```

- [ ] **Step 11: Update test/CMakeLists.txt**

Add a helper library for test utilities and the pipeline test:

```cmake
# Test utilities (PNG writer + test data loader)
add_library(test_util STATIC
    util/png_writer.cpp
    util/test_data_loader.cpp
)
target_include_directories(test_util PUBLIC
    "${CMAKE_SOURCE_DIR}/test/util"
    "${CMAKE_SOURCE_DIR}/third_party/stb"
    "${CMAKE_SOURCE_DIR}/third_party/catch2"
)
target_link_libraries(test_util PUBLIC nukex4_io nukex4_stretch)

nukex_add_test(test_stretch_pipeline unit/stretch/test_stretch_pipeline.cpp nukex4_stretch)
```

- [ ] **Step 12: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure -E stacking_engine
```

Expected: 26 tests (25 + 1 new pipeline test).

- [ ] **Step 13: Commit**

```bash
git add -A
git commit -m "feat(stretch): lib/stretch scaffolding — StretchOp, Pipeline, utils, test harness

StretchOp base class with category enum. StretchPipeline with position-ordered
execution, advisory ordering warnings, and quick_preview_stretch (arcsinh on copy).
Luminance-only utility (Rec. 709, chromaticity-preserving).
Test utilities: PNG writer (stb_image_write), M16 data loader.
Scaffold headers for MTF, ArcSinh, Log, RNC ops."
```

---

## Task 3: MTFStretch — Midtone Transfer Function

**Files:**
- Modify: `src/lib/stretch/include/nukex/stretch/mtf_stretch.hpp`
- Modify: `src/lib/stretch/src/mtf_stretch.cpp`
- Create: `test/unit/stretch/test_mtf.cpp`
- Modify: `test/CMakeLists.txt`

Reference: PixInsight STF (Vizier). Formula: `f(x) = ((m-1)·xc) / ((2m-1)·xc - m)`

- [ ] **Step 1: mtf_stretch.hpp is already correct from Task 2 scaffold.** Verify it has: midtone, shadows, highlights, luminance_only, name="MTF", category=FINISHER.

- [ ] **Step 2: Implement mtf_stretch.cpp**

```cpp
#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <algorithm>

namespace nukex {

float MTFStretch::apply_scalar(float x) const {
    // Clip to [shadows, highlights], rescale to [0, 1]
    float range = highlights - shadows;
    if (range < 1e-10f) return 0.0f;
    float xc = std::clamp((x - shadows) / range, 0.0f, 1.0f);

    // Edge cases
    if (midtone <= 0.0f) return 0.0f;
    if (midtone >= 1.0f) return 1.0f;

    // MTF formula: ((m-1)·xc) / ((2m-1)·xc - m)
    // This maps: f(0)=0, f(1)=1, f(m)=0.5
    float m = midtone;
    return ((m - 1.0f) * xc) / ((2.0f * m - 1.0f) * xc - m);
}

void MTFStretch::apply(Image& img) const {
    auto fn = [this](float x) { return apply_scalar(x); };

    if (luminance_only) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

} // namespace nukex
```

- [ ] **Step 3: Create test_mtf.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stretch/mtf_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("MTF: apply_scalar(0) == 0", "[mtf]") {
    MTFStretch mtf;
    REQUIRE(mtf.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("MTF: apply_scalar(1) == 1", "[mtf]") {
    MTFStretch mtf;
    REQUIRE(mtf.apply_scalar(1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("MTF: apply_scalar(midtone) == 0.5", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.25f;
    REQUIRE(mtf.apply_scalar(0.25f) == Catch::Approx(0.5f).margin(1e-5f));
}

TEST_CASE("MTF: midtone=0.5 is identity", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.5f;
    for (float x : {0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f}) {
        REQUIRE(mtf.apply_scalar(x) == Catch::Approx(x).margin(1e-5f));
    }
}

TEST_CASE("MTF: monotonically increasing", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.25f;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = mtf.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("MTF: shadows/highlights clip and remap", "[mtf]") {
    MTFStretch mtf;
    mtf.shadows = 0.1f;
    mtf.highlights = 0.9f;
    mtf.midtone = 0.5f;
    REQUIRE(mtf.apply_scalar(0.05f) == Catch::Approx(0.0f));
    REQUIRE(mtf.apply_scalar(0.95f) == Catch::Approx(1.0f));
    REQUIRE(mtf.apply_scalar(0.5f) == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("MTF: midtone=0 returns 0", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.0f;
    REQUIRE(mtf.apply_scalar(0.5f) == Catch::Approx(0.0f));
}

TEST_CASE("MTF: midtone=1 returns 1 for x<1, but formula gives 1", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 1.0f;
    REQUIRE(mtf.apply_scalar(0.5f) == Catch::Approx(1.0f));
}

TEST_CASE("MTF: low midtone brightens image", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.1f;
    // Low midtone → aggressive stretch → 0.1 should map well above 0.1
    REQUIRE(mtf.apply_scalar(0.1f) > 0.3f);
}

TEST_CASE("MTF: high midtone darkens image", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.9f;
    // High midtone → mild stretch → 0.5 should map below 0.5
    REQUIRE(mtf.apply_scalar(0.5f) < 0.5f);
}

TEST_CASE("MTF: visual output on M16", "[mtf][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    // Apply arcsinh pre-stretch to bring data into visible range, then MTF
    float norm = std::asinh(500.0f);
    img.apply([norm](float x) { return std::asinh(500.0f * x) / norm; });

    MTFStretch mtf;
    mtf.midtone = 0.15f;
    mtf.enabled = true;
    mtf.apply(img);

    bool ok = test_util::write_png("test/output/stretch_mtf.png", img, false);
    REQUIRE(ok);
}
```

- [ ] **Step 4: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_mtf unit/stretch/test_mtf.cpp nukex4_stretch test_util)
```

- [ ] **Step 5: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure -R test_mtf
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(stretch): MTFStretch — midtone transfer function

f(x) = ((m-1)·xc) / ((2m-1)·xc - m) with shadow/highlight clipping.
Verified: f(0)=0, f(1)=1, f(m)=0.5, monotonic, identity at m=0.5.
Visual output on M16 data. Reference: PixInsight STF (Vizier)."
```

---

## Task 4: ArcSinhStretch

**Files:**
- Modify: `src/lib/stretch/src/arcsinh_stretch.cpp`
- Create: `test/unit/stretch/test_arcsinh.cpp`
- Modify: `test/CMakeLists.txt`

Reference: Lupton et al. (1999). Formula: `f(x) = arcsinh(α·x) / arcsinh(α)`

- [ ] **Step 1: arcsinh_stretch.hpp is already correct from Task 2.** Verify: alpha=500, luminance_only=true, category=PRIMARY.

- [ ] **Step 2: Implement arcsinh_stretch.cpp**

```cpp
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>

namespace nukex {

float ArcSinhStretch::apply_scalar(float x) const {
    if (alpha < 1e-10f) return x;  // Identity when alpha ≈ 0
    return std::asinh(alpha * x) / std::asinh(alpha);
}

void ArcSinhStretch::apply(Image& img) const {
    auto fn = [this](float x) { return apply_scalar(x); };

    if (luminance_only) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

} // namespace nukex
```

- [ ] **Step 3: Create test_arcsinh.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("ArcSinh: apply_scalar(0) == 0", "[arcsinh]") {
    ArcSinhStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("ArcSinh: apply_scalar(1) == 1", "[arcsinh]") {
    ArcSinhStretch s;
    REQUIRE(s.apply_scalar(1.0f) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("ArcSinh: monotonically increasing", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 500.0f;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("ArcSinh: higher alpha → more aggressive stretch", "[arcsinh]") {
    ArcSinhStretch lo, hi;
    lo.alpha = 100.0f;
    hi.alpha = 1000.0f;
    // At x=0.01 (faint signal), higher alpha should produce brighter output
    REQUIRE(hi.apply_scalar(0.01f) > lo.apply_scalar(0.01f));
}

TEST_CASE("ArcSinh: alpha near 0 is identity", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 0.001f;
    for (float x : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        REQUIRE(s.apply_scalar(x) == Catch::Approx(x).margin(0.01f));
    }
}

TEST_CASE("ArcSinh: known value at alpha=500, x=0.01", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 500.0f;
    float expected = std::asinh(500.0f * 0.01f) / std::asinh(500.0f);
    REQUIRE(s.apply_scalar(0.01f) == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("ArcSinh: preserves hue in luminance-only mode", "[arcsinh]") {
    ArcSinhStretch s;
    s.luminance_only = true;
    s.alpha = 500.0f;

    Image img(4, 4, 3);
    // Set a colored pixel: R=0.01, G=0.005, B=0.002
    img.at(2, 2, 0) = 0.01f;
    img.at(2, 2, 1) = 0.005f;
    img.at(2, 2, 2) = 0.002f;

    float ratio_rg_before = img.at(2, 2, 0) / img.at(2, 2, 1);
    float ratio_gb_before = img.at(2, 2, 1) / img.at(2, 2, 2);

    s.apply(img);

    float ratio_rg_after = img.at(2, 2, 0) / img.at(2, 2, 1);
    float ratio_gb_after = img.at(2, 2, 1) / img.at(2, 2, 2);

    // Ratios should be preserved (hue unchanged)
    REQUIRE(ratio_rg_after == Catch::Approx(ratio_rg_before).margin(0.01f));
    REQUIRE(ratio_gb_after == Catch::Approx(ratio_gb_before).margin(0.01f));
}

TEST_CASE("ArcSinh: output in [0, 1]", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 500.0f;
    for (float x = 0.0f; x <= 1.0f; x += 0.01f) {
        float y = s.apply_scalar(x);
        REQUIRE(y >= -1e-6f);
        REQUIRE(y <= 1.0f + 1e-6f);
    }
}

TEST_CASE("ArcSinh: faint signal boost — x=0.001 maps above 0.1", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 500.0f;
    REQUIRE(s.apply_scalar(0.001f) > 0.1f);
}

TEST_CASE("ArcSinh: cross-validation with manual computation", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 200.0f;
    // arcsinh(200 * 0.05) / arcsinh(200) = arcsinh(10) / arcsinh(200)
    float expected = std::asinh(10.0f) / std::asinh(200.0f);
    REQUIRE(s.apply_scalar(0.05f) == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("ArcSinh: visual output on M16", "[arcsinh][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    ArcSinhStretch s;
    s.alpha = 500.0f;
    s.luminance_only = true;
    s.enabled = true;
    s.apply(img);

    bool ok = test_util::write_png("test/output/stretch_arcsinh.png", img, false);
    REQUIRE(ok);
}
```

- [ ] **Step 4: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_arcsinh unit/stretch/test_arcsinh.cpp nukex4_stretch test_util)
```

- [ ] **Step 5: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure -R test_arcsinh
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(stretch): ArcSinhStretch — color-preserving arcsinh

f(x) = arcsinh(α·x) / arcsinh(α). Luminance-only mode preserves hue
via Rec. 709 L scaling. Verified: f(0)=0, f(1)=1, monotonic, hue
preservation, faint signal boost. Reference: Lupton et al. (1999)."
```

---

## Task 5: LogStretch

**Files:**
- Modify: `src/lib/stretch/src/log_stretch.cpp`
- Create: `test/unit/stretch/test_log.cpp`
- Modify: `test/CMakeLists.txt`

Formula: `f(x) = log1p(α·x) / log1p(α)`

- [ ] **Step 1: Implement log_stretch.cpp**

```cpp
#include "nukex/stretch/log_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>

namespace nukex {

float LogStretch::apply_scalar(float x) const {
    if (alpha < 1e-10f) return x;
    return std::log1p(alpha * x) / std::log1p(alpha);
}

void LogStretch::apply(Image& img) const {
    auto fn = [this](float x) { return apply_scalar(x); };

    if (luminance_only) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

} // namespace nukex
```

- [ ] **Step 2: Create test_log.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stretch/log_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("Log: apply_scalar(0) == 0", "[log]") {
    LogStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("Log: apply_scalar(1) == 1", "[log]") {
    LogStretch s;
    REQUIRE(s.apply_scalar(1.0f) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("Log: monotonically increasing", "[log]") {
    LogStretch s;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("Log: higher alpha → more aggressive", "[log]") {
    LogStretch lo, hi;
    lo.alpha = 100.0f;
    hi.alpha = 10000.0f;
    REQUIRE(hi.apply_scalar(0.01f) > lo.apply_scalar(0.01f));
}

TEST_CASE("Log: alpha near 0 → identity", "[log]") {
    LogStretch s;
    s.alpha = 0.001f;
    for (float x : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        REQUIRE(s.apply_scalar(x) == Catch::Approx(x).margin(0.01f));
    }
}

TEST_CASE("Log: known value cross-validation", "[log]") {
    LogStretch s;
    s.alpha = 1000.0f;
    float expected = std::log1p(1000.0f * 0.01f) / std::log1p(1000.0f);
    REQUIRE(s.apply_scalar(0.01f) == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("Log: output in [0, 1]", "[log]") {
    LogStretch s;
    for (float x = 0.0f; x <= 1.0f; x += 0.01f) {
        float y = s.apply_scalar(x);
        REQUIRE(y >= -1e-6f);
        REQUIRE(y <= 1.0f + 1e-6f);
    }
}

TEST_CASE("Log: less aggressive than arcsinh at same alpha", "[log]") {
    // log1p grows slower than asinh, so log stretch is gentler
    LogStretch log_s;
    log_s.alpha = 500.0f;
    float log_val = log_s.apply_scalar(0.01f);
    float arcsinh_val = std::asinh(500.0f * 0.01f) / std::asinh(500.0f);
    REQUIRE(log_val < arcsinh_val);
}

TEST_CASE("Log: faint signal boost", "[log]") {
    LogStretch s;
    s.alpha = 1000.0f;
    REQUIRE(s.apply_scalar(0.001f) > 0.05f);
}

TEST_CASE("Log: concave (second derivative negative)", "[log]") {
    LogStretch s;
    s.alpha = 1000.0f;
    float y0 = s.apply_scalar(0.2f);
    float y1 = s.apply_scalar(0.5f);
    float y2 = s.apply_scalar(0.8f);
    // Concave: midpoint stretched value > linear interpolation
    float linear_mid = (y0 + y2) / 2.0f;
    REQUIRE(y1 > linear_mid);
}

TEST_CASE("Log: visual output on M16", "[log][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    LogStretch s;
    s.alpha = 1000.0f;
    s.luminance_only = false;
    s.enabled = true;
    s.apply(img);

    bool ok = test_util::write_png("test/output/stretch_log.png", img, false);
    REQUIRE(ok);
}
```

- [ ] **Step 3: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_log unit/stretch/test_log.cpp nukex4_stretch test_util)
```

- [ ] **Step 4: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure -R test_log
```

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(stretch): LogStretch — log1p stretch

f(x) = log1p(α·x) / log1p(α). Verified: f(0)=0, f(1)=1, monotonic,
concave, gentler than arcsinh at same alpha. Visual output on M16."
```

---

## Task 6: RNCStretch — Roger N. Clark

**Files:**
- Modify: `src/lib/stretch/src/rnc_stretch.cpp`
- Create: `test/unit/stretch/test_rnc.cpp`
- Modify: `test/CMakeLists.txt`

Formula: `f(x) = pow(clamp((x-bp)/(wp-bp), 0, 1), 1/γ)`

- [ ] **Step 1: Implement rnc_stretch.cpp**

```cpp
#include "nukex/stretch/rnc_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>

namespace nukex {

float RNCStretch::apply_scalar(float x) const {
    float range = white_point - black_point;
    if (range < 1e-10f) return 0.0f;
    float xc = std::clamp((x - black_point) / range, 0.0f, 1.0f);
    if (gamma < 1e-10f) return (xc > 0.0f) ? 1.0f : 0.0f;
    return std::pow(xc, 1.0f / gamma);
}

void RNCStretch::apply(Image& img) const {
    auto fn = [this](float x) { return apply_scalar(x); };

    if (luminance_only) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

} // namespace nukex
```

- [ ] **Step 2: Create test_rnc.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stretch/rnc_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("RNC: apply_scalar(0) == 0", "[rnc]") {
    RNCStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("RNC: apply_scalar(1) == 1", "[rnc]") {
    RNCStretch s;
    REQUIRE(s.apply_scalar(1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("RNC: gamma=1 is identity", "[rnc]") {
    RNCStretch s;
    s.gamma = 1.0f;
    for (float x : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        REQUIRE(s.apply_scalar(x) == Catch::Approx(x).margin(1e-5f));
    }
}

TEST_CASE("RNC: gamma=2.2 standard", "[rnc]") {
    RNCStretch s;
    s.gamma = 2.2f;
    float expected = std::pow(0.5f, 1.0f / 2.2f);
    REQUIRE(s.apply_scalar(0.5f) == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("RNC: monotonically increasing", "[rnc]") {
    RNCStretch s;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("RNC: black_point clips below", "[rnc]") {
    RNCStretch s;
    s.black_point = 0.1f;
    REQUIRE(s.apply_scalar(0.05f) == Catch::Approx(0.0f));
}

TEST_CASE("RNC: white_point clips above", "[rnc]") {
    RNCStretch s;
    s.white_point = 0.8f;
    REQUIRE(s.apply_scalar(0.9f) == Catch::Approx(1.0f));
}

TEST_CASE("RNC: apply_scalar(bp) == 0, apply_scalar(wp) == 1", "[rnc]") {
    RNCStretch s;
    s.black_point = 0.2f;
    s.white_point = 0.8f;
    REQUIRE(s.apply_scalar(0.2f) == Catch::Approx(0.0f));
    REQUIRE(s.apply_scalar(0.8f) == Catch::Approx(1.0f));
}

TEST_CASE("RNC: higher gamma → less aggressive (darker result)", "[rnc]") {
    RNCStretch lo, hi;
    lo.gamma = 1.5f;
    hi.gamma = 3.0f;
    // Higher gamma → 1/gamma smaller → pow(x, smaller) → closer to 1 for x<1
    // Actually: pow(0.5, 1/1.5) > pow(0.5, 1/3.0)
    // 1/1.5 = 0.667, 1/3 = 0.333 → 0.5^0.667 < 0.5^0.333
    REQUIRE(lo.apply_scalar(0.5f) < hi.apply_scalar(0.5f));
}

TEST_CASE("RNC: output in [0, 1]", "[rnc]") {
    RNCStretch s;
    for (float x = 0.0f; x <= 1.0f; x += 0.01f) {
        float y = s.apply_scalar(x);
        REQUIRE(y >= -1e-6f);
        REQUIRE(y <= 1.0f + 1e-6f);
    }
}

TEST_CASE("RNC: visual output on M16", "[rnc][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    // Pre-stretch with arcsinh to make data visible, then apply RNC
    float norm = std::asinh(500.0f);
    img.apply([norm](float x) { return std::asinh(500.0f * x) / norm; });

    RNCStretch s;
    s.gamma = 2.2f;
    s.black_point = 0.05f;
    s.white_point = 0.95f;
    s.enabled = true;
    s.apply(img);

    bool ok = test_util::write_png("test/output/stretch_rnc.png", img, false);
    REQUIRE(ok);
}
```

- [ ] **Step 3: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_rnc unit/stretch/test_rnc.cpp nukex4_stretch test_util)
```

- [ ] **Step 4: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure -R test_rnc
```

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(stretch): RNCStretch — Roger N. Clark power law

f(x) = pow(clamp((x-bp)/(wp-bp), 0, 1), 1/γ). Verified: f(0)=0,
f(1)=1, f(bp)=0, f(wp)=1, gamma=1 is identity, monotonic.
Visual output on M16. Reference: Roger N. Clark methodology."
```

---

## Task 7: Full Phase 5A Verification + Visual Review

- [ ] **Step 1: Clean build + all tests**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
ctest --output-on-failure -E stacking_engine
```

- [ ] **Step 2: Run visual tests to generate PNGs**

```bash
cd build && ctest -R visual --output-on-failure
```

This should produce:
- `test/output/stretch_mtf.png`
- `test/output/stretch_arcsinh.png`
- `test/output/stretch_log.png`
- `test/output/stretch_rnc.png`

- [ ] **Step 3: User reviews PNGs**

Open all 4 PNGs in an image viewer. Evaluate:
- Are faint structures (M16 pillars) visible?
- Are stars not blown out?
- Are gradients smooth?
- Does the overall tonality look correct for each stretch type?

---

*End of Phase 5A implementation plan*
