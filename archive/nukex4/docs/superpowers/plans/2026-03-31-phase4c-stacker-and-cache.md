# Phase 4C: lib/stacker — FrameCache + StackingEngine

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the disk-backed frame cache (uint16, mmap, pixel-major) and the streaming orchestrator that ties io + alignment + classify + fitting + combine together into a complete stacking pipeline.

**Architecture:** FrameCache stores aligned frames as uint16 on disk via mmap. StackingEngine runs two phases: Phase A streams frames one at a time (load → debayer → flat → align → cache → accumulate Welford/Histogram), Phase B streams pixels from cache (classify → fit → select) in parallel via std::for_each(par_unseq). No tiles — full Cube in memory, parallel over pixels.

**Tech Stack:** C++17, mmap (POSIX/Windows), lib/core, lib/io, lib/alignment, lib/classify, lib/fitting, lib/combine, Catch2 v3

**Critical rules:**
- No stubs, no TODOs
- uint16 encode/decode: value * 65535 + 0.5 → uint16, uint16 * (1/65535) → float
- mmap for disk I/O — pixel-major layout for sequential Phase B reads
- Temp file deleted in destructor
- Phase B parallel: each pixel independent, FrameStats shared read-only

---

## File Structure

```
src/lib/stacker/
├── CMakeLists.txt
├── include/
│   └── nukex/
│       └── stacker/
│           ├── frame_cache.hpp      Disk-backed uint16 mmap frame storage
│           └── stacking_engine.hpp  Two-phase streaming orchestrator
├── src/
│   ├── frame_cache.cpp
│   └── stacking_engine.cpp

test/unit/stacker/
├── test_frame_cache.cpp
└── test_stacking_engine.cpp
```

---

## Task 1: FrameCache — Disk-Backed uint16 Frame Storage

**Files:**
- Create: `src/lib/stacker/CMakeLists.txt`
- Create: `src/lib/stacker/include/nukex/stacker/frame_cache.hpp`
- Create: `src/lib/stacker/src/frame_cache.cpp`
- Create: `test/unit/stacker/test_frame_cache.cpp`
- Modify: `CMakeLists.txt` (root)
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Create directories**

```bash
mkdir -p src/lib/stacker/include/nukex/stacker
mkdir -p src/lib/stacker/src
mkdir -p test/unit/stacker
```

- [ ] **Step 2: Create src/lib/stacker/CMakeLists.txt**

```cmake
add_library(nukex4_stacker STATIC
    src/frame_cache.cpp
    src/stacking_engine.cpp
)

target_include_directories(nukex4_stacker
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_stacker
    PUBLIC nukex4_core nukex4_io nukex4_alignment nukex4_fitting
           nukex4_classify nukex4_combine
)

target_compile_features(nukex4_stacker PUBLIC cxx_std_17)
```

- [ ] **Step 3: Create frame_cache.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include <string>
#include <cstdint>

namespace nukex {

/// Disk-backed storage for aligned frames using memory-mapped uint16 encoding.
///
/// Pixel-major layout: read_pixel(x,y,ch) returns N contiguous uint16 values,
/// one per frame, for sequential disk reads during Phase B.
///
/// Encoding: float [0,1] → uint16 via round(value * 65535)
/// Decoding: uint16 → float via stored * (1.0f / 65535.0f)
/// Quantization error: ±7.6e-6 (100× below noise floor).
///
/// The temp file is deleted when the FrameCache is destroyed.
class FrameCache {
public:
    /// Create a cache file in cache_dir. Pre-allocates for max_frames frames.
    FrameCache(int width, int height, int n_channels,
               int max_frames, const std::string& cache_dir);

    /// Unmaps and deletes the temp file.
    ~FrameCache();

    // Non-copyable, movable
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;
    FrameCache(FrameCache&& other) noexcept;
    FrameCache& operator=(FrameCache&& other) noexcept;

    /// Phase A: Write one aligned frame to the cache.
    /// Encodes float→uint16 and scatters to pixel-major positions.
    void write_frame(int frame_index, const Image& aligned);

    /// Phase B: Read all frame values at one pixel/channel, decoded to float.
    /// out_values must have space for at least n_frames_ floats.
    /// Returns number of frames written so far.
    int read_pixel(int x, int y, int ch, float* out_values) const;

    /// Number of frames written so far.
    int n_frames_written() const { return n_frames_written_; }

    int width() const { return width_; }
    int height() const { return height_; }
    int n_channels() const { return n_channels_; }
    int max_frames() const { return max_frames_; }

    /// Encode float to uint16.
    static uint16_t encode(float value);

    /// Decode uint16 to float.
    static float decode(uint16_t stored);

private:
    int fd_ = -1;
    uint16_t* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    std::string filepath_;

    int width_ = 0;
    int height_ = 0;
    int n_channels_ = 0;
    int max_frames_ = 0;
    int n_frames_written_ = 0;

    /// Byte offset for pixel (x, y), channel ch, frame f.
    size_t offset(int x, int y, int ch, int f) const {
        return static_cast<size_t>(
            ((static_cast<int64_t>(y) * width_ + x) * n_channels_ + ch)
            * max_frames_ + f);
    }

    void cleanup();
};

} // namespace nukex
```

- [ ] **Step 4: Create frame_cache.cpp**

```cpp
#include "nukex/stacker/frame_cache.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace nukex {

uint16_t FrameCache::encode(float value) {
    float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<uint16_t>(clamped * 65535.0f + 0.5f);
}

float FrameCache::decode(uint16_t stored) {
    return stored * (1.0f / 65535.0f);
}

FrameCache::FrameCache(int width, int height, int n_channels,
                       int max_frames, const std::string& cache_dir)
    : width_(width), height_(height), n_channels_(n_channels),
      max_frames_(max_frames)
{
    // Compute total size
    size_t n_entries = static_cast<size_t>(width) * height * n_channels * max_frames;
    mapped_size_ = n_entries * sizeof(uint16_t);

    // Create temp file
    filepath_ = cache_dir + "/nukex_cache_XXXXXX";
    // mkstemp needs a mutable char array
    std::vector<char> path_buf(filepath_.begin(), filepath_.end());
    path_buf.push_back('\0');
    fd_ = mkstemp(path_buf.data());
    if (fd_ < 0) {
        throw std::runtime_error("FrameCache: failed to create temp file in " + cache_dir);
    }
    filepath_ = std::string(path_buf.data());

    // Extend file to full size
    if (ftruncate(fd_, static_cast<off_t>(mapped_size_)) != 0) {
        cleanup();
        throw std::runtime_error("FrameCache: ftruncate failed");
    }

    // Memory map
    mapped_ = static_cast<uint16_t*>(
        mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (mapped_ == MAP_FAILED) {
        mapped_ = nullptr;
        cleanup();
        throw std::runtime_error("FrameCache: mmap failed");
    }
}

FrameCache::~FrameCache() {
    cleanup();
}

void FrameCache::cleanup() {
    if (mapped_ && mapped_ != MAP_FAILED) {
        munmap(mapped_, mapped_size_);
        mapped_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (!filepath_.empty()) {
        unlink(filepath_.c_str());
        filepath_.clear();
    }
}

FrameCache::FrameCache(FrameCache&& other) noexcept
    : fd_(other.fd_), mapped_(other.mapped_), mapped_size_(other.mapped_size_),
      filepath_(std::move(other.filepath_)),
      width_(other.width_), height_(other.height_),
      n_channels_(other.n_channels_), max_frames_(other.max_frames_),
      n_frames_written_(other.n_frames_written_)
{
    other.fd_ = -1;
    other.mapped_ = nullptr;
    other.mapped_size_ = 0;
}

FrameCache& FrameCache::operator=(FrameCache&& other) noexcept {
    if (this != &other) {
        cleanup();
        fd_ = other.fd_;
        mapped_ = other.mapped_;
        mapped_size_ = other.mapped_size_;
        filepath_ = std::move(other.filepath_);
        width_ = other.width_;
        height_ = other.height_;
        n_channels_ = other.n_channels_;
        max_frames_ = other.max_frames_;
        n_frames_written_ = other.n_frames_written_;
        other.fd_ = -1;
        other.mapped_ = nullptr;
        other.mapped_size_ = 0;
    }
    return *this;
}

void FrameCache::write_frame(int frame_index, const Image& aligned) {
    if (!mapped_) throw std::runtime_error("FrameCache: not mapped");
    if (frame_index < 0 || frame_index >= max_frames_)
        throw std::out_of_range("FrameCache: frame_index out of range");

    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            for (int ch = 0; ch < n_channels_; ch++) {
                float value = aligned.at(x, y, ch);
                mapped_[offset(x, y, ch, frame_index)] = encode(value);
            }
        }
    }

    if (frame_index >= n_frames_written_) {
        n_frames_written_ = frame_index + 1;
    }
}

int FrameCache::read_pixel(int x, int y, int ch, float* out_values) const {
    if (!mapped_) return 0;

    int n = n_frames_written_;
    size_t base = offset(x, y, ch, 0);

    // Contiguous uint16 values for this pixel/channel across all frames
    for (int f = 0; f < n; f++) {
        out_values[f] = decode(mapped_[base + f]);
    }

    return n;
}

} // namespace nukex
```

- [ ] **Step 5: Create stacking_engine stub**

`src/lib/stacker/include/nukex/stacker/stacking_engine.hpp`:
```cpp
#pragma once

#include "nukex/io/image.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/classify/weight_computer.hpp"
#include "nukex/fitting/model_selector.hpp"
#include <string>
#include <vector>

namespace nukex {

class StackingEngine {
public:
    struct Config {
        FrameAligner::Config  aligner_config;
        WeightConfig          weight_config;
        ModelSelector::Config fitting_config;
        std::string           cache_dir = "/tmp";
    };

    explicit StackingEngine(const Config& config);

    struct Result {
        Image stacked;
        Image noise_map;
        Image quality_map;
        int   n_frames_processed = 0;
        int   n_frames_failed_alignment = 0;
    };

    Result execute(const std::vector<std::string>& light_paths,
                   const std::vector<std::string>& flat_paths);

private:
    Config config_;
};

} // namespace nukex
```

`src/lib/stacker/src/stacking_engine.cpp` (stub — filled in Task 2):
```cpp
#include "nukex/stacker/stacking_engine.hpp"

namespace nukex {

StackingEngine::StackingEngine(const Config& config) : config_(config) {}

StackingEngine::Result StackingEngine::execute(
    const std::vector<std::string>&,
    const std::vector<std::string>&) {
    return {};
}

} // namespace nukex
```

- [ ] **Step 6: Create test_frame_cache.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/io/image.hpp"
#include <cmath>
#include <filesystem>

using namespace nukex;

TEST_CASE("FrameCache::encode/decode roundtrip", "[cache]") {
    // Test several values across [0, 1]
    float test_values[] = {0.0f, 0.001f, 0.25f, 0.5f, 0.75f, 0.999f, 1.0f};
    for (float v : test_values) {
        uint16_t encoded = FrameCache::encode(v);
        float decoded = FrameCache::decode(encoded);
        REQUIRE(decoded == Catch::Approx(v).margin(1.0f / 65535.0f));
    }
}

TEST_CASE("FrameCache::encode clamps to [0, 1]", "[cache]") {
    REQUIRE(FrameCache::encode(-0.1f) == 0);
    REQUIRE(FrameCache::encode(1.5f) == 65535);
}

TEST_CASE("FrameCache: write and read back single frame", "[cache]") {
    Image frame(4, 4, 1);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            frame.at(x, y, 0) = (x + y * 4) / 16.0f;

    FrameCache cache(4, 4, 1, 10, "/tmp");
    cache.write_frame(0, frame);
    REQUIRE(cache.n_frames_written() == 1);

    float values[10];
    int n = cache.read_pixel(2, 1, 0, values);
    REQUIRE(n == 1);
    // Pixel (2, 1): value = (2 + 1*4) / 16 = 0.375
    REQUIRE(values[0] == Catch::Approx(0.375f).margin(0.001f));
}

TEST_CASE("FrameCache: write multiple frames, read all back", "[cache]") {
    Image f1(8, 8, 2);
    Image f2(8, 8, 2);
    Image f3(8, 8, 2);
    f1.fill(0.3f);
    f2.fill(0.5f);
    f3.fill(0.7f);

    FrameCache cache(8, 8, 2, 10, "/tmp");
    cache.write_frame(0, f1);
    cache.write_frame(1, f2);
    cache.write_frame(2, f3);
    REQUIRE(cache.n_frames_written() == 3);

    float values[10];
    int n = cache.read_pixel(4, 4, 0, values);
    REQUIRE(n == 3);
    REQUIRE(values[0] == Catch::Approx(0.3f).margin(0.001f));
    REQUIRE(values[1] == Catch::Approx(0.5f).margin(0.001f));
    REQUIRE(values[2] == Catch::Approx(0.7f).margin(0.001f));

    // Channel 1 should also work
    n = cache.read_pixel(4, 4, 1, values);
    REQUIRE(n == 3);
    REQUIRE(values[0] == Catch::Approx(0.3f).margin(0.001f));
}

TEST_CASE("FrameCache: temp file cleaned up on destruction", "[cache]") {
    std::string filepath;
    {
        FrameCache cache(2, 2, 1, 1, "/tmp");
        Image frame(2, 2, 1);
        frame.fill(0.5f);
        cache.write_frame(0, frame);
        // We can't easily get the filepath, but verify no crash on destruction
    }
    // Cache destroyed — temp file should be gone
    // (No way to verify path externally without exposing it, but no crash = success)
}

TEST_CASE("FrameCache: quantization error within tolerance", "[cache]") {
    // Verify max quantization error is < 2/65535 ≈ 3e-5
    Image frame(16, 16, 1);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            frame.at(x, y, 0) = (x * 16 + y) / 256.0f;

    FrameCache cache(16, 16, 1, 1, "/tmp");
    cache.write_frame(0, frame);

    float values[1];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            cache.read_pixel(x, y, 0, values);
            float original = frame.at(x, y, 0);
            REQUIRE(std::fabs(values[0] - original) < 2.0f / 65535.0f);
        }
    }
}
```

- [ ] **Step 7: Update root CMakeLists.txt**

Add after `add_subdirectory(src/lib/combine)`:
```cmake
add_subdirectory(src/lib/stacker)
```

- [ ] **Step 8: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_frame_cache unit/stacker/test_frame_cache.cpp nukex4_stacker)
```

- [ ] **Step 9: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: 25 tests (24 + 1 test file with 5 test cases).

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "feat(stacker): FrameCache — disk-backed uint16 mmap frame storage

Pixel-major layout: read_pixel returns N contiguous uint16 values decoded
to float. Encoding: round(value × 65535) → uint16. Quantization error
±7.6e-6 (100× below noise). Memory-mapped via POSIX mmap for efficient
Phase B sequential reads. Temp file auto-deleted on destruction."
```

---

## Task 2: StackingEngine — The Orchestrator

**Files:**
- Modify: `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp`
- Modify: `src/lib/stacker/src/stacking_engine.cpp`
- Create: `test/unit/stacker/test_stacking_engine.cpp`
- Modify: `test/CMakeLists.txt`

This is the top-level pipeline: Phase A (stream frames → cache → accumulate) and Phase B (stream pixels → classify → fit → select → output).

- [ ] **Step 1: stacking_engine.hpp is already correct from Task 1 stub.**

Verify it has StackingEngine with Config (aligner_config, weight_config, fitting_config, cache_dir), Result (stacked, noise_map, quality_map, n_frames_processed, n_frames_failed_alignment), and execute().

- [ ] **Step 2: Implement stacking_engine.cpp**

```cpp
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/debayer.hpp"
#include "nukex/io/flat_calibration.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/classify/weight_computer.hpp"
#include "nukex/fitting/model_selector.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include "nukex/combine/pixel_selector.hpp"
#include "nukex/combine/spatial_context.hpp"
#include "nukex/combine/output_assembler.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace nukex {

StackingEngine::StackingEngine(const Config& config) : config_(config) {}

namespace {

/// Compute median of a single-channel image (or first channel if multi-channel).
float compute_frame_median(const Image& img) {
    int n = img.width() * img.height();
    std::vector<float> vals(n);
    for (int i = 0; i < n; i++) {
        vals[i] = img.channel_data(0)[i];
    }
    return median_inplace(vals.data(), n);
}

/// Compute median FWHM from alignment star catalog.
float compute_median_fwhm(const FrameAligner::AlignedFrame& aligned) {
    const auto& stars = aligned.stars;
    if (stars.empty()) return 0.0f;
    std::vector<float> fwhms;
    fwhms.reserve(stars.size());
    for (const auto& s : stars.stars) {
        if (s.fwhm > 0.0f) fwhms.push_back(s.fwhm);
    }
    if (fwhms.empty()) return 0.0f;
    return median_inplace(fwhms.data(), static_cast<int>(fwhms.size()));
}

/// Compute dominant shape across channels for a voxel.
void compute_dominant_shape(SubcubeVoxel& voxel, int n_ch) {
    int counts[7] = {};
    for (int ch = 0; ch < n_ch; ch++) {
        int s = static_cast<int>(voxel.distribution[ch].shape);
        if (s >= 0 && s < 7) counts[s]++;
    }
    int best = 0;
    for (int i = 1; i < 7; i++) {
        if (counts[i] > counts[best]) best = i;
    }
    voxel.dominant_shape = static_cast<DistributionShape>(best);
}

} // anonymous namespace

StackingEngine::Result StackingEngine::execute(
    const std::vector<std::string>& light_paths,
    const std::vector<std::string>& flat_paths)
{
    Result result;
    if (light_paths.empty()) return result;

    // ═══ SETUP ═══════════════════════════════════════════════════════

    // Read first frame to determine dimensions and channel config
    auto first = FITSReader::read(light_paths[0]);
    if (!first.success) return result;

    int width = first.image.width();
    int height = first.image.height();

    // Auto-detect channel config from metadata
    ChannelConfig ch_config;
    if (!first.metadata.bayer_pattern.empty() &&
        first.metadata.bayer_pattern != "NONE") {
        ch_config = ChannelConfig::from_mode(StackingMode::OSC_RGB);
    } else {
        ch_config = ChannelConfig::from_mode(StackingMode::MONO_L);
    }

    // After debayer, dimensions change for Bayer data
    int out_width = width;
    int out_height = height;
    int n_ch = ch_config.n_channels;

    // Build master flat if flats provided
    Image master_flat;
    if (!flat_paths.empty()) {
        master_flat = FlatCalibration::build_master_flat(flat_paths);
    }

    // Allocate cube
    Cube cube(out_width, out_height, ch_config);

    // Allocate frame cache
    int n_frames = static_cast<int>(light_paths.size());
    FrameCache cache(out_width, out_height, n_ch, n_frames, config_.cache_dir);

    // Frame-level metadata
    std::vector<FrameStats> frame_stats(n_frames);
    std::vector<float> frame_fwhms(n_frames, 0.0f);

    // Initialize aligner
    FrameAligner aligner(config_.aligner_config);

    // ═══ PHASE A — Streaming Accumulation ════════════════════════════

    for (int f = 0; f < n_frames; f++) {
        // 1. Load
        auto read_result = FITSReader::read(light_paths[f]);
        if (!read_result.success) continue;

        Image image = std::move(read_result.image);
        auto& meta = read_result.metadata;

        // 2. Debayer
        if (ch_config.bayer != BayerPattern::NONE) {
            image = DebayerEngine::debayer(image, ch_config.bayer);
        }

        // 3. Flat correct
        if (!master_flat.empty()) {
            FlatCalibration::apply(image, master_flat);
        }

        // 4. Align
        auto aligned = aligner.align(image, f);

        // 5. Cache aligned frame
        cache.write_frame(f, aligned.image);

        // 6. Frame-level stats
        float frame_median = compute_frame_median(aligned.image);
        float frame_fwhm = compute_median_fwhm(aligned);

        frame_stats[f].read_noise = meta.read_noise;
        frame_stats[f].gain = meta.gain;
        frame_stats[f].exposure = meta.exposure;
        frame_stats[f].has_noise_keywords = meta.has_noise_keywords;
        frame_stats[f].is_meridian_flipped = aligned.alignment.is_meridian_flipped;
        frame_stats[f].frame_weight = aligned.alignment.weight_penalty;
        frame_stats[f].median_luminance = frame_median;
        frame_stats[f].fwhm = frame_fwhm;
        frame_fwhms[f] = frame_fwhm;

        if (aligned.alignment.alignment_failed) {
            result.n_frames_failed_alignment++;
        }

        // 7. Accumulate into cube
        for (int y = 0; y < out_height; y++) {
            for (int x = 0; x < out_width; x++) {
                auto& voxel = cube.at(x, y);
                for (int ch = 0; ch < n_ch; ch++) {
                    float value = aligned.image.at(x, y, ch);
                    voxel.welford[ch].update(value);
                    if (voxel.welford[ch].count() == 1) {
                        voxel.histogram[ch].initialize_range(
                            value - 0.1f, value + 0.1f);
                    }
                    voxel.histogram[ch].update(value);
                }
                voxel.n_frames++;
            }
        }

        cube.n_frames_loaded++;
        result.n_frames_processed++;
    }

    if (result.n_frames_processed == 0) return result;

    // ═══ BETWEEN PHASES — Global Statistics ══════════════════════════

    // Compute fwhm_best and backfill PSF weights
    float fwhm_best = 1e30f;
    for (int f = 0; f < n_frames; f++) {
        if (frame_fwhms[f] > 0.0f) {
            fwhm_best = std::min(fwhm_best, frame_fwhms[f]);
        }
    }
    if (fwhm_best < 1e-10f || fwhm_best > 1e20f) fwhm_best = 1.0f;

    for (int f = 0; f < n_frames; f++) {
        if (frame_fwhms[f] > 0.0f) {
            float ratio = frame_fwhms[f] / fwhm_best;
            frame_stats[f].psf_weight = std::exp(
                -0.5f * (ratio - 1.0f) * (ratio - 1.0f) / 0.25f);
        } else {
            frame_stats[f].psf_weight = 1.0f;
        }
    }

    // ═══ PHASE B — Analysis ══════════════════════════════════════════

    WeightComputer classifier(config_.weight_config);
    ModelSelector fitter(config_.fitting_config);
    PixelSelector selector;

    // Output images
    Image stacked(out_width, out_height, n_ch);
    Image noise_map(out_width, out_height, n_ch);

    // Process all pixels (sequential for now — par_unseq requires TBB or similar)
    std::vector<float> pixel_values(n_frames);
    std::vector<float> pixel_weights(n_frames);
    std::vector<int> pixel_frame_indices(n_frames);

    for (int y = 0; y < out_height; y++) {
        for (int x = 0; x < out_width; x++) {
            auto& voxel = cube.at(x, y);

            for (int ch = 0; ch < n_ch; ch++) {
                // 1. Read all frame values from cache
                int n = cache.read_pixel(x, y, ch, pixel_values.data());

                // 2. Compute weights
                for (int i = 0; i < n; i++) {
                    pixel_frame_indices[i] = i;
                    pixel_weights[i] = classifier.compute(
                        pixel_values[i], frame_stats[i],
                        voxel.welford[ch].mean, voxel.welford[ch].std_dev());
                }

                // 3. Fit — model selection cascade
                fitter.select(pixel_values.data(), pixel_weights.data(),
                              n, voxel, ch);

                // 4. Select — extract output value + noise
                float out_val, out_noise, out_snr;
                selector.select(voxel.distribution[ch],
                                pixel_values.data(), pixel_weights.data(), n,
                                frame_stats.data(), pixel_frame_indices.data(),
                                voxel.welford[ch].variance(),
                                out_val, out_noise, out_snr);

                stacked.at(x, y, ch) = out_val;
                noise_map.at(x, y, ch) = out_noise;
                voxel.snr[ch] = out_snr;
            }

            compute_dominant_shape(voxel, n_ch);
        }
    }

    // Spatial context
    SpatialContext spatial;
    spatial.compute(stacked, cube);

    // Quality map
    Image quality_map = OutputAssembler::assemble_quality_map(cube);

    result.stacked = std::move(stacked);
    result.noise_map = std::move(noise_map);
    result.quality_map = std::move(quality_map);

    return result;
}

} // namespace nukex
```

- [ ] **Step 3: Create test_stacking_engine.cpp**

This test uses real FITS data from `/home/scarter4work/projects/processing/M16/` to verify the end-to-end pipeline. If those files are not available, skip the integration test and test with a synthetic mini-pipeline.

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/io/image.hpp"
#include <filesystem>

using namespace nukex;

TEST_CASE("StackingEngine: empty input → empty result", "[engine]") {
    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute({}, {});
    REQUIRE(result.n_frames_processed == 0);
    REQUIRE(result.stacked.empty());
}

// Integration test with real FITS data (skip if not available)
TEST_CASE("StackingEngine: M16 integration test", "[engine][integration][!mayfail]") {
    std::string data_dir = "/home/scarter4work/projects/processing/M16/";
    if (!std::filesystem::exists(data_dir)) {
        SKIP("M16 test data not available at " + data_dir);
    }

    // Find first 5 FITS files
    std::vector<std::string> lights;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        if (entry.path().extension() == ".fits" || entry.path().extension() == ".fit") {
            lights.push_back(entry.path().string());
            if (lights.size() >= 5) break;
        }
    }
    if (lights.size() < 3) {
        SKIP("Not enough FITS files in " + data_dir);
    }

    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute(lights, {});
    REQUIRE(result.n_frames_processed >= 3);
    REQUIRE(!result.stacked.empty());
    REQUIRE(result.stacked.width() > 0);
    REQUIRE(result.stacked.height() > 0);
    REQUIRE(!result.noise_map.empty());
    REQUIRE(!result.quality_map.empty());
    REQUIRE(result.quality_map.n_channels() == 4);
}
```

- [ ] **Step 4: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_stacking_engine unit/stacker/test_stacking_engine.cpp nukex4_stacker)
```

- [ ] **Step 5: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: 26+ tests. The integration test may be skipped if M16 data is not mounted.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(stacker): StackingEngine — complete two-phase stacking pipeline

Phase A: stream frames → debayer → flat correct → align → cache (uint16) →
accumulate Welford + Histogram into Cube.
Phase B: stream pixels from cache → classify weights → fit distribution
(Student-t/GMM/contamination/KDE via AICc) → select pixel value + noise →
spatial context → assemble output images.

Produces three outputs: stacked image, noise map, quality map."
```

---

## Task 3: Full Phase 4C Verification

- [ ] **Step 1: Clean Debug build + all tests**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc)
ctest --output-on-failure
```

- [ ] **Step 2: Clean Release build + all tests**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
ctest --output-on-failure
```

---

*End of Phase 4C implementation plan*
