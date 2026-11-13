// Atomic whole-file writes (PCL-free, POSIX): used by the recon GLB and GIF exports.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdg {

/// Writes `size` bytes to `path` atomically (Ruling R10): the bytes go to a unique temporary
/// `<path>.tmp.XXXXXX` created by mkostemp (O_CLOEXEC) in the target's directory, which is chmod
/// 0644, fsynced, closed and renamed over `path`; the directory is then fsynced so the rename is durable. A reader
/// sees the old file or a complete new one, never a partial file; an existing `path` is replaced
/// only by the rename. Concurrent writers to one path each use their own temporary, so the last
/// complete rename wins. Any failure throws std::runtime_error("cannot write <path>: <strerror>")
/// and removes this call's temporary (a failed directory fsync comes after the rename, so the new
/// file is already in place then). Reentrant; no global state.
void write_file_atomic(const std::string& path, const void* data, std::size_t size);

inline void write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    write_file_atomic(path, bytes.data(), bytes.size());
}

}  // namespace gdg
