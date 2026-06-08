# Phase 2: lib/io — FITS I/O, Image Type, Ring Buffer, Debayer, Flat Calibration

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the I/O layer: load FITS files into an internal Image type, extract frame metadata, debayer Bayer data, apply flat field correction, and manage the ring buffer for streaming frame processing.

**Architecture:** lib/io depends on lib/core (for ChannelConfig, FrameMetadata types). Uses system CFITSIO directly (not PCL's wrapper) so it can be tested independently without PixInsight. Defines a lightweight `Image` class for float32 multi-channel pixel data. The ring buffer manages concurrent frame loading and processing.

**Tech Stack:** C++17, CFITSIO 4.6, lib/core, Catch2 v3

**Test data:** Real FITS files at `/home/scarter4work/projects/processing/M16/` (6072×4042, 16-bit int, RGGB Bayer, HaO3 filter, ZWO ASI2400MC Pro)

**Critical rules:**
- No stubs, no TODOs
- Double-check all numerical code
- FITS header parsing must handle missing/malformed keywords gracefully

---

## File Structure

```
src/lib/io/
├── CMakeLists.txt
├── include/
│   └── nukex/
│       └── io/
│           ├── image.hpp            Float32 multi-channel image
│           ├── fits_reader.hpp      FITS file loading + metadata extraction
│           ├── fits_writer.hpp      FITS file writing
│           ├── debayer.hpp          Bayer demosaicing
│           ├── flat_calibration.hpp Master flat generation + division
│           └── ring_buffer.hpp      Streaming frame ring buffer
│       └── io.hpp                   Convenience header
├── src/
│   ├── image.cpp
│   ├── fits_reader.cpp
│   ├── fits_writer.cpp
│   ├── debayer.cpp
│   └── flat_calibration.cpp
test/unit/io/
├── test_image.cpp
├── test_fits_reader.cpp
├── test_debayer.cpp
└── test_flat_calibration.cpp
```

---

## Task 1: Image Type

**Files:**
- Create: `src/lib/io/include/nukex/io/image.hpp`
- Create: `src/lib/io/src/image.cpp`
- Create: `test/unit/io/test_image.cpp`
- Create: `src/lib/io/CMakeLists.txt`
- Modify: root `CMakeLists.txt` (add io subdirectory)
- Modify: `test/CMakeLists.txt` (add io test targets)

A lightweight float32 multi-channel image for internal use. Not PCL's Image — this is our
own type that can be used and tested without PixInsight.

- [ ] **Step 1: Create src/lib/io/include/nukex/io/image.hpp**

```cpp
#pragma once

#include <vector>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <algorithm>

namespace nukex {

/// Lightweight float32 multi-channel image.
///
/// Channel-by-channel, row-major storage: data layout is [ch][y][x].
/// This matches PCL's Image layout (channel-by-channel), not OpenCV's
/// interleaved BGR layout.
///
/// This is NOT PCL's Image class. It exists so lib/io can be tested
/// independently without PixInsight. Conversion to/from PCL Image
/// happens in the module layer.
class Image {
public:
    Image() = default;

    /// Construct with dimensions and channel count. All pixels initialized to 0.
    Image(int width, int height, int n_channels);

    /// Copy and move
    Image(const Image& other);
    Image& operator=(const Image& other);
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    /// Dimensions
    int width() const { return width_; }
    int height() const { return height_; }
    int n_channels() const { return n_channels_; }
    int total_pixels() const { return width_ * height_; }

    /// Access a single pixel value at (x, y, channel).
    float& at(int x, int y, int ch) {
        return data_[ch * width_ * height_ + y * width_ + x];
    }
    float at(int x, int y, int ch) const {
        return data_[ch * width_ * height_ + y * width_ + x];
    }

    /// Pointer to the start of a channel's pixel data.
    float* channel_data(int ch) {
        return data_.data() + ch * width_ * height_;
    }
    const float* channel_data(int ch) const {
        return data_.data() + ch * width_ * height_;
    }

    /// Pointer to all pixel data (contiguous: [ch0 pixels][ch1 pixels]...).
    float* data() { return data_.data(); }
    const float* data() const { return data_.data(); }

    /// Total number of float values in storage (width * height * n_channels).
    size_t data_size() const { return data_.size(); }

    /// Create a deep copy.
    Image clone() const;

    /// Apply a function to every pixel value in-place.
    template<typename Fn>
    void apply(Fn&& fn) {
        for (size_t i = 0; i < data_.size(); i++) {
            data_[i] = fn(data_[i]);
        }
    }

    /// Fill all pixels with a value.
    void fill(float value);

    /// Check if image has been allocated.
    bool empty() const { return data_.empty(); }

private:
    int width_      = 0;
    int height_     = 0;
    int n_channels_ = 0;
    std::vector<float> data_;
};

} // namespace nukex
```

- [ ] **Step 2: Create src/lib/io/src/image.cpp**

```cpp
#include "nukex/io/image.hpp"

namespace nukex {

Image::Image(int width, int height, int n_channels)
    : width_(width)
    , height_(height)
    , n_channels_(n_channels)
    , data_(static_cast<size_t>(width) * height * n_channels, 0.0f)
{}

Image::Image(const Image& other) = default;
Image& Image::operator=(const Image& other) = default;
Image::Image(Image&& other) noexcept = default;
Image& Image::operator=(Image&& other) noexcept = default;

Image Image::clone() const {
    return *this;
}

void Image::fill(float value) {
    std::fill(data_.begin(), data_.end(), value);
}

} // namespace nukex
```

- [ ] **Step 3: Create src/lib/io/CMakeLists.txt**

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(CFITSIO REQUIRED cfitsio)

add_library(nukex4_io STATIC
    src/image.cpp
)

target_include_directories(nukex4_io
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${CFITSIO_INCLUDE_DIRS}
)

target_link_libraries(nukex4_io
    PUBLIC nukex4_core
    PRIVATE ${CFITSIO_LIBRARIES}
)

target_compile_features(nukex4_io PUBLIC cxx_std_17)
```

- [ ] **Step 4: Update root CMakeLists.txt**

Add after the `add_subdirectory(src/lib/core)` line:

```cmake
add_subdirectory(src/lib/io)
```

- [ ] **Step 5: Create test/unit/io/test_image.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/io/image.hpp"
#include <cmath>

using nukex::Image;

TEST_CASE("Image: construction with dimensions", "[image]") {
    Image img(100, 80, 3);
    REQUIRE(img.width() == 100);
    REQUIRE(img.height() == 80);
    REQUIRE(img.n_channels() == 3);
    REQUIRE(img.total_pixels() == 8000);
    REQUIRE(img.data_size() == 24000);
    REQUIRE(img.empty() == false);
}

TEST_CASE("Image: default construction is empty", "[image]") {
    Image img;
    REQUIRE(img.width() == 0);
    REQUIRE(img.height() == 0);
    REQUIRE(img.empty() == true);
}

TEST_CASE("Image: initialized to zero", "[image]") {
    Image img(10, 10, 1);
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 10; x++)
            REQUIRE(img.at(x, y, 0) == 0.0f);
}

TEST_CASE("Image: pixel access read/write", "[image]") {
    Image img(10, 10, 3);
    img.at(5, 3, 0) = 0.5f;
    img.at(5, 3, 1) = 0.6f;
    img.at(5, 3, 2) = 0.7f;
    REQUIRE(img.at(5, 3, 0) == Catch::Approx(0.5f));
    REQUIRE(img.at(5, 3, 1) == Catch::Approx(0.6f));
    REQUIRE(img.at(5, 3, 2) == Catch::Approx(0.7f));
    // Other pixels unaffected
    REQUIRE(img.at(0, 0, 0) == 0.0f);
}

TEST_CASE("Image: channel_data returns correct pointer", "[image]") {
    Image img(4, 3, 2);
    img.at(2, 1, 0) = 1.0f;
    img.at(2, 1, 1) = 2.0f;
    // Channel 0 offset: y*width + x = 1*4 + 2 = 6
    REQUIRE(img.channel_data(0)[6] == Catch::Approx(1.0f));
    // Channel 1 offset: same position in channel 1's block
    REQUIRE(img.channel_data(1)[6] == Catch::Approx(2.0f));
}

TEST_CASE("Image: channel layout is channel-by-channel", "[image]") {
    Image img(2, 2, 3);
    // Set all pixels in channel 0 to 1, channel 1 to 2, channel 2 to 3
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            img.at(x, y, 0) = 1.0f;
            img.at(x, y, 1) = 2.0f;
            img.at(x, y, 2) = 3.0f;
        }
    // Raw data should be: [1,1,1,1, 2,2,2,2, 3,3,3,3]
    const float* d = img.data();
    for (int i = 0; i < 4; i++) REQUIRE(d[i] == Catch::Approx(1.0f));
    for (int i = 4; i < 8; i++) REQUIRE(d[i] == Catch::Approx(2.0f));
    for (int i = 8; i < 12; i++) REQUIRE(d[i] == Catch::Approx(3.0f));
}

TEST_CASE("Image: clone creates independent copy", "[image]") {
    Image img(5, 5, 1);
    img.at(2, 2, 0) = 42.0f;
    Image copy = img.clone();
    copy.at(2, 2, 0) = 99.0f;
    REQUIRE(img.at(2, 2, 0) == Catch::Approx(42.0f));
    REQUIRE(copy.at(2, 2, 0) == Catch::Approx(99.0f));
}

TEST_CASE("Image: fill sets all pixels", "[image]") {
    Image img(3, 3, 2);
    img.fill(0.5f);
    for (int ch = 0; ch < 2; ch++)
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++)
                REQUIRE(img.at(x, y, ch) == Catch::Approx(0.5f));
}

TEST_CASE("Image: apply transforms all pixels", "[image]") {
    Image img(3, 3, 1);
    img.fill(4.0f);
    img.apply([](float x) { return x * 2.0f; });
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            REQUIRE(img.at(x, y, 0) == Catch::Approx(8.0f));
}

TEST_CASE("Image: move semantics", "[image]") {
    Image img(10, 10, 1);
    img.at(5, 5, 0) = 1.0f;
    Image moved = std::move(img);
    REQUIRE(moved.at(5, 5, 0) == Catch::Approx(1.0f));
    REQUIRE(moved.width() == 10);
    REQUIRE(img.empty() == true);
}
```

- [ ] **Step 6: Add test target to test/CMakeLists.txt**

```cmake
nukex_add_test(test_image         unit/io/test_image.cpp           nukex4_io)
```

- [ ] **Step 7: Create directory structure**

```bash
mkdir -p src/lib/io/include/nukex/io
mkdir -p src/lib/io/src
mkdir -p test/unit/io
```

- [ ] **Step 8: Build and run tests**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake .. -DNUKEX_BUILD_MODULE=OFF && make -j$(nproc) && ctest --output-on-failure -R test_image
```

Expected: All 10 test cases pass.

- [ ] **Step 9: Commit**

```
feat(io): Image type — lightweight float32 multi-channel image

Channel-by-channel row-major storage matching PCL's layout. Independent
of PixInsight — testable standalone. Clone, fill, apply, channel access.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

---

## Task 2: FITS Reader + Frame Metadata

**Files:**
- Create: `src/lib/io/include/nukex/io/fits_reader.hpp`
- Create: `src/lib/io/src/fits_reader.cpp`
- Create: `src/lib/core/include/nukex/core/frame_metadata.hpp` (in core, since other libs need it)
- Create: `test/unit/io/test_fits_reader.cpp`
- Modify: `src/lib/io/CMakeLists.txt` (add fits_reader.cpp)
- Modify: `src/lib/core/include/nukex/core.hpp` (add frame_metadata include)

- [ ] **Step 1: Create frame_metadata.hpp in lib/core**

```cpp
#pragma once

#include <cstdint>
#include <string>

namespace nukex {

/// Per-frame metadata extracted from FITS headers.
/// Lives in core because multiple libs need it (io, classify, combine).
struct FrameMetadata {
    // Noise/calibration
    float read_noise    = 3.0f;   // electrons (FITS: RDNOISE, READNOISE, NOISE)
    float gain          = 1.0f;   // e-/ADU   (FITS: GAIN, EGAIN)
    float exposure      = 0.0f;   // seconds  (FITS: EXPTIME, EXPOSURE)
    float temperature   = 0.0f;   // Celsius  (FITS: CCD-TEMP, SET-TEMP)

    // Astrometry / pointing
    float plate_scale   = 0.0f;   // arcsec/pixel (FITS: PIXSCALE or derived)
    float focal_length  = 0.0f;   // mm (FITS: FOCALLEN)
    float pixel_size    = 0.0f;   // μm (FITS: XPIXSZ)
    float altitude      = 0.0f;   // degrees above horizon (FITS: OBJCTALT)
    float ra            = 0.0f;   // degrees (FITS: RA)
    float dec           = 0.0f;   // degrees (FITS: DEC)

    // Image dimensions
    int   width         = 0;
    int   height        = 0;
    int   bitpix        = 16;     // FITS BITPIX

    // Filter and camera
    std::string filter;           // FITS: FILTER
    std::string instrument;       // FITS: INSTRUME
    std::string bayer_pattern;    // FITS: BAYERPAT
    std::string date_obs;         // FITS: DATE-OBS

    // Flags
    bool  is_meridian_flipped = false;  // detected during alignment
    bool  has_noise_keywords  = false;  // RDNOISE + GAIN both present
    bool  has_plate_scale     = false;
    bool  has_wcs             = false;  // WCS astrometry headers present

    // Frame index in the session (0-based, arrival order)
    int   frame_index = 0;
};

} // namespace nukex
```

- [ ] **Step 2: Create fits_reader.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include "nukex/core/frame_metadata.hpp"
#include <string>

namespace nukex {

/// Result of reading a FITS file.
struct FITSReadResult {
    Image         image;     // pixel data as float32
    FrameMetadata metadata;  // extracted header information
    bool          success = false;
    std::string   error;     // error message if !success
};

/// Read a FITS file into an Image + FrameMetadata.
///
/// Handles 8/16/32-bit integer and 32/64-bit float FITS.
/// Raw Bayer data is returned as single-channel (debayer happens separately).
/// Pixel values are normalized to [0, 1] for integer types.
///
/// Uses CFITSIO directly (not PCL) for standalone testability.
class FITSReader {
public:
    /// Read a single FITS file.
    static FITSReadResult read(const std::string& filepath);

    /// Read only the FITS headers (no pixel data). Useful for scanning file lists.
    static FrameMetadata read_headers(const std::string& filepath);

private:
    /// Extract metadata from FITS header keywords.
    static FrameMetadata extract_metadata(void* fptr);

    /// Read a float keyword, returning default_val if not found.
    static float read_float_key(void* fptr, const char* key, float default_val);

    /// Read an int keyword, returning default_val if not found.
    static int read_int_key(void* fptr, const char* key, int default_val);

    /// Read a string keyword, returning empty string if not found.
    static std::string read_string_key(void* fptr, const char* key);
};

} // namespace nukex
```

- [ ] **Step 3: Create fits_reader.cpp**

```cpp
#include "nukex/io/fits_reader.hpp"
#include <fitsio.h>
#include <cstring>
#include <cmath>
#include <vector>

namespace nukex {

float FITSReader::read_float_key(void* fptr, const char* key, float default_val) {
    float value = default_val;
    int status = 0;
    fits_read_key(static_cast<fitsfile*>(fptr), TFLOAT, key, &value, nullptr, &status);
    if (status != 0) return default_val;
    return value;
}

int FITSReader::read_int_key(void* fptr, const char* key, int default_val) {
    int value = default_val;
    int status = 0;
    fits_read_key(static_cast<fitsfile*>(fptr), TINT, key, &value, nullptr, &status);
    if (status != 0) return default_val;
    return value;
}

std::string FITSReader::read_string_key(void* fptr, const char* key) {
    char value[FLEN_VALUE] = {};
    int status = 0;
    fits_read_key(static_cast<fitsfile*>(fptr), TSTRING, key, value, nullptr, &status);
    if (status != 0) return "";
    // Trim trailing spaces
    int len = static_cast<int>(strlen(value));
    while (len > 0 && value[len - 1] == ' ') len--;
    return std::string(value, len);
}

FrameMetadata FITSReader::extract_metadata(void* fptr) {
    FrameMetadata m;
    fitsfile* f = static_cast<fitsfile*>(fptr);

    // Dimensions
    int naxis = 0;
    long naxes[3] = {0, 0, 0};
    int status = 0;
    fits_get_img_dim(f, &naxis, &status);
    fits_get_img_size(f, 3, naxes, &status);
    m.width  = static_cast<int>(naxes[0]);
    m.height = static_cast<int>(naxes[1]);
    m.bitpix = read_int_key(f, "BITPIX", 16);

    // Exposure
    m.exposure = read_float_key(f, "EXPTIME", 0.0f);
    if (m.exposure == 0.0f) m.exposure = read_float_key(f, "EXPOSURE", 0.0f);

    // Gain — try EGAIN first (e-/ADU), then GAIN
    float egain = read_float_key(f, "EGAIN", -1.0f);
    if (egain > 0.0f) {
        m.gain = egain;
    } else {
        m.gain = read_float_key(f, "GAIN", 1.0f);
    }

    // Read noise — try multiple keywords
    float rdnoise = read_float_key(f, "RDNOISE", -1.0f);
    if (rdnoise < 0.0f) rdnoise = read_float_key(f, "READNOISE", -1.0f);
    if (rdnoise < 0.0f) rdnoise = read_float_key(f, "NOISE", -1.0f);
    if (rdnoise > 0.0f) {
        m.read_noise = rdnoise;
        m.has_noise_keywords = (m.gain > 0.0f);
    }

    // Temperature
    m.temperature = read_float_key(f, "CCD-TEMP", 0.0f);
    if (m.temperature == 0.0f) m.temperature = read_float_key(f, "SET-TEMP", 0.0f);

    // Plate scale
    m.focal_length = read_float_key(f, "FOCALLEN", 0.0f);
    m.pixel_size   = read_float_key(f, "XPIXSZ", 0.0f);
    float pixscale = read_float_key(f, "PIXSCALE", 0.0f);
    if (pixscale > 0.0f) {
        m.plate_scale = pixscale;
        m.has_plate_scale = true;
    } else if (m.focal_length > 0.0f && m.pixel_size > 0.0f) {
        m.plate_scale = 206.265f * m.pixel_size / m.focal_length;
        m.has_plate_scale = true;
    }

    // Coordinates
    m.ra  = read_float_key(f, "RA", 0.0f);
    m.dec = read_float_key(f, "DEC", 0.0f);
    m.altitude = read_float_key(f, "OBJCTALT", 0.0f);

    // Strings
    m.filter       = read_string_key(f, "FILTER");
    m.instrument   = read_string_key(f, "INSTRUME");
    m.bayer_pattern = read_string_key(f, "BAYERPAT");
    m.date_obs     = read_string_key(f, "DATE-OBS");

    // WCS detection
    std::string ctype1 = read_string_key(f, "CTYPE1");
    m.has_wcs = !ctype1.empty();

    return m;
}

FITSReadResult FITSReader::read(const std::string& filepath) {
    FITSReadResult result;
    fitsfile* fptr = nullptr;
    int status = 0;

    // Open FITS file
    fits_open_file(&fptr, filepath.c_str(), READONLY, &status);
    if (status != 0) {
        char err[FLEN_ERRMSG];
        fits_get_errstatus(status, err);
        result.error = std::string("Failed to open FITS: ") + err;
        return result;
    }

    // Extract metadata
    result.metadata = extract_metadata(fptr);

    // Get image parameters
    int naxis = 0;
    long naxes[3] = {0, 0, 0};
    fits_get_img_dim(fptr, &naxis, &status);
    fits_get_img_size(fptr, 3, naxes, &status);

    int width  = static_cast<int>(naxes[0]);
    int height = static_cast<int>(naxes[1]);
    int n_channels = (naxis >= 3) ? static_cast<int>(naxes[2]) : 1;

    // Allocate image
    result.image = Image(width, height, n_channels);

    // Read pixel data as float, channel by channel
    long fpixel[3] = {1, 1, 1};  // CFITSIO uses 1-based indexing
    int anynul = 0;
    float nullval = 0.0f;

    for (int ch = 0; ch < n_channels; ch++) {
        fpixel[2] = ch + 1;
        long n_pixels = static_cast<long>(width) * height;

        fits_read_pix(fptr, TFLOAT, fpixel, n_pixels,
                      &nullval, result.image.channel_data(ch),
                      &anynul, &status);
        if (status != 0) {
            char err[FLEN_ERRMSG];
            fits_get_errstatus(status, err);
            result.error = std::string("Failed to read pixels: ") + err;
            fits_close_file(fptr, &status);
            return result;
        }
    }

    // Normalize integer data to [0, 1]
    int bitpix = 0;
    fits_get_img_type(fptr, &bitpix, &status);
    if (bitpix == BYTE_IMG || bitpix == SHORT_IMG || bitpix == LONG_IMG ||
        bitpix == USHORT_IMG || bitpix == ULONG_IMG) {
        // CFITSIO applies BZERO/BSCALE automatically when reading as TFLOAT,
        // so 16-bit data with BZERO=32768 comes back as unsigned range [0, 65535].
        // Normalize to [0, 1].
        float max_val = 0.0f;
        const float* d = result.image.data();
        for (size_t i = 0; i < result.image.data_size(); i++) {
            if (d[i] > max_val) max_val = d[i];
        }
        if (max_val > 0.0f) {
            float scale = 1.0f / max_val;
            result.image.apply([scale](float x) { return x * scale; });
        }
    }

    fits_close_file(fptr, &status);
    result.success = true;
    return result;
}

FrameMetadata FITSReader::read_headers(const std::string& filepath) {
    FrameMetadata m;
    fitsfile* fptr = nullptr;
    int status = 0;

    fits_open_file(&fptr, filepath.c_str(), READONLY, &status);
    if (status != 0) return m;

    m = extract_metadata(fptr);
    fits_close_file(fptr, &status);
    return m;
}

} // namespace nukex
```

- [ ] **Step 4: Update src/lib/io/CMakeLists.txt**

Add `src/fits_reader.cpp` to the sources list.

- [ ] **Step 5: Add frame_metadata.hpp include to core.hpp**

- [ ] **Step 6: Create test_fits_reader.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/io/fits_reader.hpp"
#include <filesystem>

using namespace nukex;

// Path to real test FITS file
static const std::string TEST_FITS =
    "/home/scarter4work/projects/processing/M16/"
    "Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit";

TEST_CASE("FITSReader: read headers from real file", "[fits][integration]") {
    if (!std::filesystem::exists(TEST_FITS)) {
        SKIP("Test FITS file not available");
    }

    auto meta = FITSReader::read_headers(TEST_FITS);

    REQUIRE(meta.width == 6072);
    REQUIRE(meta.height == 4042);
    REQUIRE(meta.exposure == Catch::Approx(300.0f));
    REQUIRE(meta.gain == Catch::Approx(6.2f).margin(0.1f));  // EGAIN
    REQUIRE(meta.filter == "HaO3");
    REQUIRE(meta.bayer_pattern == "RGGB");
    REQUIRE(meta.instrument == "ZWO ASI2400MC Pro");
    REQUIRE(meta.focal_length == Catch::Approx(1215.0f));
    REQUIRE(meta.pixel_size == Catch::Approx(5.94f).margin(0.01f));
    REQUIRE(meta.has_plate_scale == true);
    // plate_scale = 206.265 * 5.94 / 1215 ≈ 1.008 arcsec/pixel
    REQUIRE(meta.plate_scale == Catch::Approx(1.008f).margin(0.01f));
    REQUIRE(meta.has_wcs == true);
    REQUIRE(meta.date_obs == "2023-09-02T03:09:59.682813");
}

TEST_CASE("FITSReader: read full image from real file", "[fits][integration]") {
    if (!std::filesystem::exists(TEST_FITS)) {
        SKIP("Test FITS file not available");
    }

    auto result = FITSReader::read(TEST_FITS);

    REQUIRE(result.success == true);
    REQUIRE(result.error.empty());
    REQUIRE(result.image.width() == 6072);
    REQUIRE(result.image.height() == 4042);
    REQUIRE(result.image.n_channels() == 1);  // Raw Bayer = single channel

    // Pixel values should be normalized to [0, 1]
    float min_val = 1.0f, max_val = 0.0f;
    const float* data = result.image.data();
    for (size_t i = 0; i < result.image.data_size(); i++) {
        if (data[i] < min_val) min_val = data[i];
        if (data[i] > max_val) max_val = data[i];
    }
    REQUIRE(min_val >= 0.0f);
    REQUIRE(max_val <= 1.0f);
    REQUIRE(max_val == Catch::Approx(1.0f).margin(0.001f));  // max should normalize to ~1.0
}

TEST_CASE("FITSReader: nonexistent file returns error", "[fits]") {
    auto result = FITSReader::read("/nonexistent/file.fits");
    REQUIRE(result.success == false);
    REQUIRE(!result.error.empty());
}

TEST_CASE("FITSReader: read_headers from nonexistent file returns default", "[fits]") {
    auto meta = FITSReader::read_headers("/nonexistent/file.fits");
    REQUIRE(meta.width == 0);
    REQUIRE(meta.height == 0);
}
```

- [ ] **Step 7: Add test target**

```cmake
nukex_add_test(test_fits_reader   unit/io/test_fits_reader.cpp     nukex4_io)
```

- [ ] **Step 8: Build and run**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake .. -DNUKEX_BUILD_MODULE=OFF && make -j$(nproc) && ctest --output-on-failure -R test_fits
```

Expected: All 4 test cases pass (2 integration tests with real FITS, 2 error handling).

- [ ] **Step 9: Commit**

```
feat(io): FITSReader + FrameMetadata — FITS file loading with header extraction

Reads FITS via CFITSIO. Extracts gain, read noise, exposure, filter,
Bayer pattern, plate scale, WCS detection. Normalizes integer pixel data
to [0,1]. Tested against real M16 HaO3 data (6072x4042, ASI2400MC Pro).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

---

## Task 3: Debayer Engine

**Files:**
- Create: `src/lib/io/include/nukex/io/debayer.hpp`
- Create: `src/lib/io/src/debayer.cpp`
- Create: `test/unit/io/test_debayer.cpp`

Simple bilinear debayer for RGGB pattern. More sophisticated methods (VNG, AHD)
can be added later, but bilinear is correct and sufficient for stacking
(we're not producing final display images from the debayer output).

- [ ] **Step 1: Create debayer.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include "nukex/core/channel_config.hpp"

namespace nukex {

/// Debayer (demosaic) a raw Bayer image into RGB channels.
///
/// Input: single-channel float32 image with Bayer pattern data.
/// Output: 3-channel float32 image (R, G, B).
///
/// Uses bilinear interpolation. This is intentionally simple — for stacking,
/// the debayered output feeds into alignment and accumulation, not direct display.
/// The distribution fitting on the accumulated Z-arrays is what produces the
/// final pixel values, so debayer quality at this stage is not critical.
class DebayerEngine {
public:
    /// Debayer a raw Bayer image to RGB.
    /// Returns a 3-channel image. Input must be single-channel.
    static Image debayer(const Image& raw, BayerPattern pattern);

private:
    /// Bilinear interpolation for RGGB pattern.
    static void debayer_rggb(const Image& raw, Image& rgb);
    /// Bilinear interpolation for BGGR pattern.
    static void debayer_bggr(const Image& raw, Image& rgb);
    /// Bilinear interpolation for GRBG pattern.
    static void debayer_grbg(const Image& raw, Image& rgb);
    /// Bilinear interpolation for GBRG pattern.
    static void debayer_gbrg(const Image& raw, Image& rgb);
};

} // namespace nukex
```

- [ ] **Step 2: Create debayer.cpp**

```cpp
#include "nukex/io/debayer.hpp"
#include <stdexcept>

namespace nukex {

Image DebayerEngine::debayer(const Image& raw, BayerPattern pattern) {
    if (raw.n_channels() != 1) {
        throw std::invalid_argument("DebayerEngine: input must be single-channel");
    }
    if (pattern == BayerPattern::NONE) {
        throw std::invalid_argument("DebayerEngine: BayerPattern::NONE is not debayerable");
    }

    Image rgb(raw.width(), raw.height(), 3);

    switch (pattern) {
        case BayerPattern::RGGB: debayer_rggb(raw, rgb); break;
        case BayerPattern::BGGR: debayer_bggr(raw, rgb); break;
        case BayerPattern::GRBG: debayer_grbg(raw, rgb); break;
        case BayerPattern::GBRG: debayer_gbrg(raw, rgb); break;
        default: break;
    }

    return rgb;
}

// Helper: clamp coordinates to image bounds
static inline int clamp_coord(int v, int max_val) {
    return (v < 0) ? 0 : (v >= max_val ? max_val - 1 : v);
}

// Helper: read raw pixel with boundary clamping
static inline float raw_at(const Image& raw, int x, int y) {
    int cx = clamp_coord(x, raw.width());
    int cy = clamp_coord(y, raw.height());
    return raw.at(cx, cy, 0);
}

/// Bilinear debayer for RGGB pattern.
///
/// RGGB 2x2 super-pixel layout:
///   (even_x, even_y) = R
///   (odd_x,  even_y) = G (on R row)
///   (even_x, odd_y)  = G (on B row)
///   (odd_x,  odd_y)  = B
///
/// For each pixel position, the color that IS present is read directly.
/// The two missing colors are interpolated from neighbors using bilinear averaging.
void DebayerEngine::debayer_rggb(const Image& raw, Image& rgb) {
    int w = raw.width();
    int h = raw.height();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);

            if (even_x && even_y) {
                // R pixel: R is direct, G from 4 neighbors, B from 4 diagonal neighbors
                r = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                b = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else if (!even_x && even_y) {
                // G pixel on R row: G direct, R from left/right, B from top/bottom
                g = raw_at(raw, x, y);
                r = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y)) * 0.5f;
                b = (raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.5f;
            } else if (even_x && !even_y) {
                // G pixel on B row: G direct, B from left/right, R from top/bottom
                g = raw_at(raw, x, y);
                b = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y)) * 0.5f;
                r = (raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.5f;
            } else {
                // B pixel: B direct, G from 4 neighbors, R from 4 diagonals
                b = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                r = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            }

            rgb.at(x, y, 0) = r;
            rgb.at(x, y, 1) = g;
            rgb.at(x, y, 2) = b;
        }
    }
}

/// BGGR is RGGB rotated: swap R↔B positions.
void DebayerEngine::debayer_bggr(const Image& raw, Image& rgb) {
    int w = raw.width();
    int h = raw.height();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);

            if (even_x && even_y) {
                // B pixel
                b = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                r = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else if (!even_x && even_y) {
                g = raw_at(raw, x, y);
                b = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y)) * 0.5f;
                r = (raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.5f;
            } else if (even_x && !even_y) {
                g = raw_at(raw, x, y);
                r = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y)) * 0.5f;
                b = (raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.5f;
            } else {
                r = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                b = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            }

            rgb.at(x, y, 0) = r;
            rgb.at(x, y, 1) = g;
            rgb.at(x, y, 2) = b;
        }
    }
}

/// GRBG: top-left is G on R row.
void DebayerEngine::debayer_grbg(const Image& raw, Image& rgb) {
    int w = raw.width();
    int h = raw.height();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);

            if (even_x && even_y) {
                // G on R row
                g = raw_at(raw, x, y);
                r = (raw_at(raw, x+1, y) + raw_at(raw, x-1, y)) * 0.5f;
                b = (raw_at(raw, x, y+1) + raw_at(raw, x, y-1)) * 0.5f;
            } else if (!even_x && even_y) {
                // R pixel
                r = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                b = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else if (even_x && !even_y) {
                // B pixel
                b = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                r = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else {
                // G on B row
                g = raw_at(raw, x, y);
                b = (raw_at(raw, x+1, y) + raw_at(raw, x-1, y)) * 0.5f;
                r = (raw_at(raw, x, y+1) + raw_at(raw, x, y-1)) * 0.5f;
            }

            rgb.at(x, y, 0) = r;
            rgb.at(x, y, 1) = g;
            rgb.at(x, y, 2) = b;
        }
    }
}

/// GBRG: top-left is G on B row.
void DebayerEngine::debayer_gbrg(const Image& raw, Image& rgb) {
    int w = raw.width();
    int h = raw.height();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);

            if (even_x && even_y) {
                // G on B row
                g = raw_at(raw, x, y);
                b = (raw_at(raw, x+1, y) + raw_at(raw, x-1, y)) * 0.5f;
                r = (raw_at(raw, x, y+1) + raw_at(raw, x, y-1)) * 0.5f;
            } else if (!even_x && even_y) {
                // B pixel
                b = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                r = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else if (even_x && !even_y) {
                // R pixel
                r = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                b = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else {
                // G on R row
                g = raw_at(raw, x, y);
                r = (raw_at(raw, x+1, y) + raw_at(raw, x-1, y)) * 0.5f;
                b = (raw_at(raw, x, y+1) + raw_at(raw, x, y-1)) * 0.5f;
            }

            rgb.at(x, y, 0) = r;
            rgb.at(x, y, 1) = g;
            rgb.at(x, y, 2) = b;
        }
    }
}

} // namespace nukex
```

- [ ] **Step 3: Create test_debayer.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/io/debayer.hpp"
#include "nukex/io/fits_reader.hpp"
#include <filesystem>

using namespace nukex;

TEST_CASE("Debayer: RGGB synthetic 4x4 pattern", "[debayer]") {
    // Create a synthetic RGGB Bayer pattern:
    // Row 0: R  G  R  G
    // Row 1: G  B  G  B
    // Row 2: R  G  R  G
    // Row 3: G  B  G  B
    //
    // R pixels = 0.8, G pixels = 0.5, B pixels = 0.2
    Image raw(4, 4, 1);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);
            if (even_x && even_y)       raw.at(x, y, 0) = 0.8f;  // R
            else if (!even_x && even_y) raw.at(x, y, 0) = 0.5f;  // G on R row
            else if (even_x && !even_y) raw.at(x, y, 0) = 0.5f;  // G on B row
            else                        raw.at(x, y, 0) = 0.2f;  // B
        }
    }

    Image rgb = DebayerEngine::debayer(raw, BayerPattern::RGGB);

    REQUIRE(rgb.width() == 4);
    REQUIRE(rgb.height() == 4);
    REQUIRE(rgb.n_channels() == 3);

    // At an R pixel (0,0): R should be exact, G/B interpolated
    REQUIRE(rgb.at(0, 0, 0) == Catch::Approx(0.8f));   // R direct
    REQUIRE(rgb.at(0, 0, 1) == Catch::Approx(0.5f));   // G interpolated (all neighbors are G=0.5)
    REQUIRE(rgb.at(0, 0, 2) == Catch::Approx(0.2f));   // B interpolated (diagonal neighbor is B=0.2)

    // At a B pixel (1,1): B should be exact
    REQUIRE(rgb.at(1, 1, 2) == Catch::Approx(0.2f));   // B direct
    REQUIRE(rgb.at(1, 1, 1) == Catch::Approx(0.5f));   // G interpolated
    REQUIRE(rgb.at(1, 1, 0) == Catch::Approx(0.8f));   // R interpolated

    // Interior G pixel on R row (1,0): G direct
    REQUIRE(rgb.at(1, 0, 1) == Catch::Approx(0.5f));   // G direct

    // Interior G pixel on B row (0,1): G direct
    REQUIRE(rgb.at(0, 1, 1) == Catch::Approx(0.5f));   // G direct
}

TEST_CASE("Debayer: output preserves constant image", "[debayer]") {
    // A constant image should debayer to the same constant in all channels
    Image raw(20, 20, 1);
    raw.fill(0.42f);

    Image rgb = DebayerEngine::debayer(raw, BayerPattern::RGGB);

    for (int y = 1; y < 19; y++) {  // skip edges (boundary effects)
        for (int x = 1; x < 19; x++) {
            REQUIRE(rgb.at(x, y, 0) == Catch::Approx(0.42f).margin(0.001f));
            REQUIRE(rgb.at(x, y, 1) == Catch::Approx(0.42f).margin(0.001f));
            REQUIRE(rgb.at(x, y, 2) == Catch::Approx(0.42f).margin(0.001f));
        }
    }
}

TEST_CASE("Debayer: all four Bayer patterns produce 3-channel output", "[debayer]") {
    Image raw(8, 8, 1);
    raw.fill(0.5f);

    for (auto pattern : {BayerPattern::RGGB, BayerPattern::BGGR,
                          BayerPattern::GRBG, BayerPattern::GBRG}) {
        Image rgb = DebayerEngine::debayer(raw, pattern);
        REQUIRE(rgb.n_channels() == 3);
        REQUIRE(rgb.width() == 8);
        REQUIRE(rgb.height() == 8);
    }
}

TEST_CASE("Debayer: throws on non-single-channel input", "[debayer]") {
    Image rgb(8, 8, 3);
    REQUIRE_THROWS_AS(
        DebayerEngine::debayer(rgb, BayerPattern::RGGB),
        std::invalid_argument
    );
}

TEST_CASE("Debayer: throws on NONE pattern", "[debayer]") {
    Image raw(8, 8, 1);
    REQUIRE_THROWS_AS(
        DebayerEngine::debayer(raw, BayerPattern::NONE),
        std::invalid_argument
    );
}

TEST_CASE("Debayer: real FITS file debayer", "[debayer][integration]") {
    std::string path = "/home/scarter4work/projects/processing/M16/"
                       "Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit";
    if (!std::filesystem::exists(path)) {
        SKIP("Test FITS file not available");
    }

    auto result = FITSReader::read(path);
    REQUIRE(result.success);
    REQUIRE(result.image.n_channels() == 1);

    Image rgb = DebayerEngine::debayer(result.image, BayerPattern::RGGB);
    REQUIRE(rgb.n_channels() == 3);
    REQUIRE(rgb.width() == result.image.width());
    REQUIRE(rgb.height() == result.image.height());

    // Verify output is not all zeros and values are in [0, 1]
    float sum = 0.0f;
    for (int ch = 0; ch < 3; ch++) {
        for (int y = 100; y < 200; y++) {
            for (int x = 100; x < 200; x++) {
                float v = rgb.at(x, y, ch);
                REQUIRE(v >= 0.0f);
                REQUIRE(v <= 1.0f);
                sum += v;
            }
        }
    }
    REQUIRE(sum > 0.0f);  // not all black
}
```

- [ ] **Step 4: Update CMakeLists.txt and test/CMakeLists.txt**

Add `src/debayer.cpp` to io lib sources.
Add test target: `nukex_add_test(test_debayer unit/io/test_debayer.cpp nukex4_io)`

- [ ] **Step 5: Build and run**

All debayer tests must pass.

- [ ] **Step 6: Commit**

```
feat(io): DebayerEngine — bilinear demosaic for all 4 Bayer patterns

RGGB, BGGR, GRBG, GBRG with boundary clamping. Tested: synthetic
pattern verification, constant image preservation, all patterns,
error handling, real FITS integration test.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

---

## Task 4: Flat Field Calibration

**Files:**
- Create: `src/lib/io/include/nukex/io/flat_calibration.hpp`
- Create: `src/lib/io/src/flat_calibration.cpp`
- Create: `test/unit/io/test_flat_calibration.cpp`

- [ ] **Step 1: Create flat_calibration.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include <vector>
#include <string>

namespace nukex {

/// Flat field calibration.
///
/// Generates a master flat from a set of flat frames (median combination),
/// normalizes it, and divides light frames by it to correct for vignetting,
/// dust, and optical path non-uniformity.
class FlatCalibration {
public:
    /// Build a master flat from a list of flat frame file paths.
    /// Reads each flat, normalizes by its median, then median-combines.
    /// Result is a normalized master flat where median ≈ 1.0.
    static Image build_master_flat(const std::vector<std::string>& flat_paths);

    /// Apply flat correction to a light frame: light /= master_flat.
    /// Both images must have the same dimensions.
    /// Pixels in the master flat below min_flat_value are clamped to avoid
    /// division by near-zero (which amplifies noise in vignetted corners).
    static void apply(Image& light, const Image& master_flat,
                      float min_flat_value = 0.01f);

    /// Compute the median of all pixel values in a single-channel image
    /// or across all channels.
    static float median(const Image& img);
};

} // namespace nukex
```

- [ ] **Step 2: Create flat_calibration.cpp**

```cpp
#include "nukex/io/flat_calibration.hpp"
#include "nukex/io/fits_reader.hpp"
#include <algorithm>
#include <stdexcept>
#include <numeric>

namespace nukex {

float FlatCalibration::median(const Image& img) {
    std::vector<float> values(img.data(), img.data() + img.data_size());
    size_t n = values.size();
    if (n == 0) return 0.0f;

    std::nth_element(values.begin(), values.begin() + n / 2, values.end());
    float med = values[n / 2];

    if (n % 2 == 0) {
        float lower = *std::max_element(values.begin(), values.begin() + n / 2);
        med = (med + lower) * 0.5f;
    }

    return med;
}

Image FlatCalibration::build_master_flat(const std::vector<std::string>& flat_paths) {
    if (flat_paths.empty()) {
        throw std::invalid_argument("FlatCalibration: no flat frames provided");
    }

    // Read all flats
    std::vector<Image> flats;
    flats.reserve(flat_paths.size());

    for (const auto& path : flat_paths) {
        auto result = FITSReader::read(path);
        if (!result.success) {
            throw std::runtime_error("FlatCalibration: failed to read " + path +
                                     ": " + result.error);
        }
        // Normalize each flat by its own median
        float med = median(result.image);
        if (med > 1e-10f) {
            float scale = 1.0f / med;
            result.image.apply([scale](float x) { return x * scale; });
        }
        flats.push_back(std::move(result.image));
    }

    // Verify all flats have the same dimensions
    int w = flats[0].width();
    int h = flats[0].height();
    int nc = flats[0].n_channels();
    for (size_t i = 1; i < flats.size(); i++) {
        if (flats[i].width() != w || flats[i].height() != h ||
            flats[i].n_channels() != nc) {
            throw std::runtime_error("FlatCalibration: flat frame dimension mismatch");
        }
    }

    // Median combine: for each pixel, take the median across all flats
    Image master(w, h, nc);
    size_t n_flats = flats.size();
    std::vector<float> pixel_values(n_flats);

    for (int ch = 0; ch < nc; ch++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                for (size_t f = 0; f < n_flats; f++) {
                    pixel_values[f] = flats[f].at(x, y, ch);
                }
                std::nth_element(pixel_values.begin(),
                                 pixel_values.begin() + n_flats / 2,
                                 pixel_values.end());
                master.at(x, y, ch) = pixel_values[n_flats / 2];
            }
        }
    }

    return master;
}

void FlatCalibration::apply(Image& light, const Image& master_flat,
                            float min_flat_value) {
    if (light.width() != master_flat.width() ||
        light.height() != master_flat.height()) {
        throw std::invalid_argument("FlatCalibration::apply: dimension mismatch");
    }

    // For single-channel master flat applied to multi-channel light:
    // divide each channel by the same flat.
    // For matching channel counts: divide channel by channel.
    int flat_channels = master_flat.n_channels();

    for (int ch = 0; ch < light.n_channels(); ch++) {
        int flat_ch = (flat_channels == 1) ? 0 : ch;
        if (flat_ch >= flat_channels) flat_ch = 0;

        for (int y = 0; y < light.height(); y++) {
            for (int x = 0; x < light.width(); x++) {
                float flat_val = master_flat.at(x, y, flat_ch);
                // Clamp flat to minimum to avoid amplifying noise in vignetted areas
                if (flat_val < min_flat_value) flat_val = min_flat_value;
                light.at(x, y, ch) /= flat_val;
            }
        }
    }
}

} // namespace nukex
```

- [ ] **Step 3: Create test_flat_calibration.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/io/flat_calibration.hpp"

using namespace nukex;

TEST_CASE("FlatCalibration: median of known values", "[flat]") {
    Image img(5, 1, 1);
    img.at(0, 0, 0) = 1.0f;
    img.at(1, 0, 0) = 3.0f;
    img.at(2, 0, 0) = 2.0f;
    img.at(3, 0, 0) = 5.0f;
    img.at(4, 0, 0) = 4.0f;
    // Sorted: 1, 2, 3, 4, 5 → median = 3
    REQUIRE(FlatCalibration::median(img) == Catch::Approx(3.0f));
}

TEST_CASE("FlatCalibration: median of even count", "[flat]") {
    Image img(4, 1, 1);
    img.at(0, 0, 0) = 1.0f;
    img.at(1, 0, 0) = 2.0f;
    img.at(2, 0, 0) = 3.0f;
    img.at(3, 0, 0) = 4.0f;
    // Sorted: 1, 2, 3, 4 → median = (2+3)/2 = 2.5
    REQUIRE(FlatCalibration::median(img) == Catch::Approx(2.5f));
}

TEST_CASE("FlatCalibration: apply corrects vignetting", "[flat]") {
    // Simulate vignetting: center bright, edges dim
    Image light(5, 5, 1);
    Image flat(5, 5, 1);

    // Flat has center = 1.0, edges = 0.5 (simulating vignetting)
    flat.fill(1.0f);
    flat.at(0, 0, 0) = 0.5f;
    flat.at(4, 0, 0) = 0.5f;
    flat.at(0, 4, 0) = 0.5f;
    flat.at(4, 4, 0) = 0.5f;

    // Light has uniform true signal of 0.6
    // But edges appear dimmer due to vignetting: 0.6 * 0.5 = 0.3
    light.fill(0.6f);
    light.at(0, 0, 0) = 0.3f;
    light.at(4, 0, 0) = 0.3f;
    light.at(0, 4, 0) = 0.3f;
    light.at(4, 4, 0) = 0.3f;

    FlatCalibration::apply(light, flat);

    // After correction, all pixels should be ~0.6
    REQUIRE(light.at(2, 2, 0) == Catch::Approx(0.6f));
    REQUIRE(light.at(0, 0, 0) == Catch::Approx(0.6f));
    REQUIRE(light.at(4, 4, 0) == Catch::Approx(0.6f));
}

TEST_CASE("FlatCalibration: min_flat_value prevents divide by near-zero", "[flat]") {
    Image light(3, 3, 1);
    light.fill(0.5f);

    Image flat(3, 3, 1);
    flat.fill(1.0f);
    flat.at(1, 1, 0) = 0.001f;  // Near-zero (dust mote)

    FlatCalibration::apply(light, flat, 0.01f);

    // The near-zero pixel should be clamped to 0.01, giving 0.5/0.01 = 50
    REQUIRE(light.at(1, 1, 0) == Catch::Approx(50.0f));
    // Normal pixels: 0.5 / 1.0 = 0.5
    REQUIRE(light.at(0, 0, 0) == Catch::Approx(0.5f));
}

TEST_CASE("FlatCalibration: dimension mismatch throws", "[flat]") {
    Image light(10, 10, 1);
    Image flat(20, 20, 1);

    REQUIRE_THROWS_AS(
        FlatCalibration::apply(light, flat),
        std::invalid_argument
    );
}

TEST_CASE("FlatCalibration: single-channel flat applied to multi-channel light", "[flat]") {
    Image light(4, 4, 3);
    light.fill(0.8f);

    Image flat(4, 4, 1);
    flat.fill(0.5f);

    FlatCalibration::apply(light, flat);

    // 0.8 / 0.5 = 1.6 for all channels
    for (int ch = 0; ch < 3; ch++) {
        REQUIRE(light.at(2, 2, ch) == Catch::Approx(1.6f));
    }
}
```

- [ ] **Step 4: Update CMakeLists.txt and test/CMakeLists.txt**

Add `src/flat_calibration.cpp` to io lib.
Add test target: `nukex_add_test(test_flat unit/io/test_flat_calibration.cpp nukex4_io)`

- [ ] **Step 5: Build and run**

All flat calibration tests must pass.

- [ ] **Step 6: Commit**

```
feat(io): FlatCalibration — master flat generation and flat field correction

Median-combine flat frames, normalize, divide lights. Handles single-channel
flat applied to multi-channel light. Min-flat clamping prevents noise
amplification in vignetted regions. Tested: median computation, vignetting
correction, near-zero clamping, dimension validation, multi-channel.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

---

## Task 5: Full lib/io Build Verification

- [ ] **Step 1: Clean build and run all tests**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build && mkdir build && cd build
cmake .. -DNUKEX_BUILD_MODULE=OFF -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) && ctest --output-on-failure
```

Expected: All tests pass (core tests + io tests).

- [ ] **Step 2: Release build**

```bash
rm -rf build && mkdir build && cd build
cmake .. -DNUKEX_BUILD_MODULE=OFF -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) && ctest --output-on-failure
```

Expected: All tests pass in Release mode.
