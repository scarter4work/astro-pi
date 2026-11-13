// AutoContrast core (PCL-free): display-referred raster loading -- io/loaders.py (supported_suffixes,
// image_dimensions, downsample_factor, load_raster).
//
// The Python reference decodes with Pillow (JPEG via libjpeg-turbo, 8-bit PNG, WebP via libwebp), pypng (16-bit
// PNG) and tifffile (TIFF), then resizes with Pillow. Parity therefore needs the same decoders presenting the same
// arrays and the same resampler (core/io/Resample.h), not PixInsight's format modules:
//   - JPEG: libjpeg-turbo 3.1.4.1 (the version Pillow 12.3.0 bundles) with its defaults (JDCT_ISLOW, fancy
//     upsampling); 1 component -> "L", 3 -> "RGB", 4 -> "CMYK" with Pillow's Adobe inversion (255 - v).
//   - PNG <= 8 bits: Pillow's modes -- "1" (values 0/1), "L" (2/4-bit gray scaled by 0x55/0x11), "P" (palette
//     indices, plus Pillow's 256-entry palette), "LA", "RGB", "RGBA"; tRNS, gAMA and sBIT ignored.
//   - PNG 16 bits: pypng read() -- the stored samples of every plane (no sBIT shift, no tRNS alpha); malformed
//     tRNS / sBIT chunks refused as pypng's chunk parser refuses them.
//   - TIFF: tifffile's array of the first page at native depth (uint8, uint16, float32), raw samples.
//   - WebP: libwebp 1.6.0 (Pillow 12.3.0's) through WebPAnimDecoder, frame 0 of the RGBA canvas; "RGB" (alpha
//     dropped) when the file has no alpha, as Pillow's WebPImagePlugin presents it.
// Refusals throw std::invalid_argument (Python's ValueError) naming the file; I/O failures std::runtime_error.
#pragma once

#include "core/Image.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ac {

// The explicit allow-list (= Python's 11): .fits .fit .fts .tif .tiff .png .jpg .jpeg .jpe .jfif .webp.
std::set<std::string> supported_suffixes();

// Native (width, height) without decoding pixels. Refuses a suffix outside supported_suffixes() with Python's
// message. FITS: NAXIS1/NAXIS2 of the primary header; TIFF: page 0; otherwise the format found in the file's
// first bytes (as Pillow identifies it): PNG, JPEG or WebP.
std::pair<int, int> image_dimensions(const std::string& path);

// A decoded raster as the Python decoder presents it. Exactly one of u8 / u16 / f32 is filled, interleaved
// row-major (height x width x channels). `bits` is the file's bit depth per sample; `mode` is Pillow's mode for
// the 8-bit decoders ("1", "L", "P", "LA", "RGB", "RGBA", "CMYK"), "png16" for the pypng path and "tiff" for TIFF.
// `palette` (mode "P" only): Pillow's 256 RGB entries, the PLTE colours then (0, 0, 0).
struct Decoded {
   int width = 0, height = 0, channels = 0, bits = 0;
   std::string mode;
   std::vector<uint8_t> u8;
   std::vector<uint16_t> u16;
   std::vector<float> f32;
   std::vector<uint8_t> palette;
};

// Row-at-a-time JPEG decode with decode_jpeg's settings, so a render of any size decodes in a few MCU rows of
// memory (baseline JPEGs; libjpeg buffers a progressive file's coefficients whatever the caller does).
class JpegReader {
public:
   explicit JpegReader(const std::string& path);
   ~JpegReader();
   JpegReader(const JpegReader&) = delete;
   JpegReader& operator=(const JpegReader&) = delete;

   int width() const;
   int height() const;
   int channels() const;
   const std::string& mode() const;   // "L", "RGB" or "CMYK"
   // The next row, width() * channels() bytes as Pillow presents it (CMYK inverted). Throws past the last row.
   void read_row(uint8_t* out);

   struct Impl;   // opaque (libjpeg state)

private:
   Impl* impl_;
};

Decoded decode_jpeg(const std::string& path);
Decoded decode_png(const std::string& path);    // <= 8 bits as Pillow presents it, 16 bits as pypng read()
Decoded decode_tiff(const std::string& path);   // first page; more than one matching page or planar data refused
Decoded decode_webp(const std::string& path);   // "RGB" or "RGBA", as Pillow presents it

// load_raster(path, max_dim): an (H, W, C) float64 sRGB image in [0, 1] (C = 3; a TIFF with five or more samples
// keeps them all, as Python does). Dispatch as Python: TIFF suffix -> native-depth path; a PNG whose IHDR says 16
// bits -> native-depth path; otherwise the 8-bit Pillow path (JPEG streamed through the resampler row by row;
// palette images converted to their RGB colours before resizing). Gray -- and gray + alpha -- become three equal
// channels, a fourth channel is dropped, integer data is divided by its full scale (255 / 65535; never inferred
// from the pixel values), float data by 255 or 65535 only when its maximum exceeds 1, and the result is clipped to
// [0, 1] (NaN and -0.0 kept, as numpy does). max_dim downsizes the longest side (aspect preserved, banker's
// rounding, never upscaled; max_dim < 1 is refused, where Python would quietly produce a 1 x 1 image). FITS pixels
// are refused here: FITS opens through PixInsight.
// Memory: a resized baseline JPEG never holds its full-resolution pixels (an 18000 x 18000 render peaks near
// 120 MB). A PROGRESSIVE JPEG is the exception: libjpeg buffers every DCT coefficient of the file before the first
// row comes out (about 2 bytes per sample, so ~1.9 GB for 18000 x 18000 4:4:4), whatever this function does.
// Other formats are decoded whole, then written straight into the result.
Image load_raster(const std::string& path, std::optional<int> max_dim = std::nullopt);

// How many native pixels each pixel of `loaded` represents: max(native w, h) / max(loaded w, h).
double downsample_factor(const std::string& path, const Image& loaded);

} // namespace ac
