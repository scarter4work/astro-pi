// Atomic whole-file writes (PCL-free, POSIX): used by the recon GLB and GIF exports.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdg {

/// Writes `size` bytes to `path` atomically: the bytes go to `path + ".tmp"` (created or truncated,
/// never through a symlink), which is fsynced, closed and renamed over `path`. A reader sees the old
/// file or the complete new one, never a partial file; an existing `path` is replaced only by the
/// rename. Any failure throws std::runtime_error("cannot write <path>: <strerror>") and removes the
/// temporary file if this call created it. Reentrant; no global state.
void write_file_atomic(const std::string& path, const void* data, std::size_t size);

inline void write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    write_file_atomic(path, bytes.data(), bytes.size());
}

}  // namespace gdg
