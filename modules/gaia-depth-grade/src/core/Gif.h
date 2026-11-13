// A native animated GIF89a writer (PCL-free, reentrant, no global state): the recon orbit
// animation's container.
//
// File layout (the tests and their decoder rely on it):
// - "GIF89a"; logical screen descriptor (width, height, packed 0xF7 = global colour table, colour
//   resolution 8 bits, table size 7, i.e. 256 entries; background 0; aspect 0); the global colour
//   table, 256 RGB entries (unused entries black).
// - The NETSCAPE2.0 application extension with the loop count (0 = forever).
// - Per frame: one graphic control extension (disposal 1 "do not dispose", no transparency,
//   delay_cs), an image descriptor covering the whole screen (no local table, not interlaced),
//   LZW minimum code size 8 and the LZW data in sub-blocks of at most 255 bytes, then a 0 block.
// - Trailer 0x3B.
//
// Palette: one global palette for every frame, by median cut over the frames' pixels (every pixel
// up to GIF_PALETTE_MAX_SAMPLES, else every k-th in frame-major order, k = ceil(total / max)).
// The box with the widest channel range (lowest index on ties) is split along that channel (lowest
// channel on ties) at its pixel-weighted median, until every box holds one distinct colour or 256
// boxes exist; a box's colour is its pixels' mean, rounded. So any frames with at most 256
// distinct colours (all sampled) are reproduced exactly.
// Mapping: each pixel takes the palette entry nearest in squared RGB distance, the lowest index on
// ties. A 32^3 grid of cells only prunes candidates (the entries whose nearest possible distance
// to the cell is within the smallest farthest distance); the search over them is exact, so the
// grid never changes a result.
// LZW: clear 256, end 257, first free code 258, codes from 9 bits LSB-first; the width grows when
// the next code to assign reaches 1 << width (at most 12); a full table (4096 codes) emits a clear
// code and starts over.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdg {

/// Pixels sampled for the median-cut palette before sampling every k-th.
inline constexpr std::size_t GIF_PALETTE_MAX_SAMPLES = 4000000;

/// The GIF bytes of `frames_rgb` (each width * height * 3 bytes, RGB, row-major from the top row).
/// Throws std::invalid_argument for no frames, a frame of the wrong size, width or height outside
/// [1, 65535], delay_cs or loops outside [0, 65535]. Frames are mapped and LZW-coded in parallel;
/// the bytes are identical to a serial encode.
std::vector<std::uint8_t> encode_gif(const std::vector<std::vector<std::uint8_t>>& frames_rgb, int width, int height,
                                     int delay_cs, int loops = 0);

/// encode_gif() written atomically to `path` (gdg::write_file_atomic, as write_glb): the arguments
/// are checked before anything touches the filesystem; a write failure throws
/// std::runtime_error("cannot write <path>: <strerror>") and leaves no temporary file.
void write_gif(const std::string& path, const std::vector<std::vector<std::uint8_t>>& frames_rgb, int width,
               int height, int delay_cs, int loops = 0);

}  // namespace gdg
