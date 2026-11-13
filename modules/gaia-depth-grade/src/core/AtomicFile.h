// Atomic whole-file writes (PCL-free, POSIX): used by the recon GLB and GIF exports.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gdg {

/// The umask in a /proc/<pid>/status text: the octal value of its `Umask:` line (<= 0777), nullopt
/// when there is no such line or the value is not a plain octal number.
std::optional<unsigned> umask_from_proc_status(const std::string& status_text);

/// The process umask, read from the `Umask:` line of /proc/self/status. umask(2) cannot be used: it
/// can only be read by setting it, which mutates process state other threads depend on. nullopt
/// when /proc is unreadable or the line is missing or malformed.
std::optional<unsigned> process_umask();

/// Writes `size` bytes to `path` atomically (Ruling R10): the bytes go to a unique temporary
/// `<path>.tmp.XXXXXX` created by mkostemp (O_CLOEXEC) in the target's directory, which is chmod
/// chmod 0666 & ~umask (what a plain open(2) with mode 0666 would give; the umask is read from
/// /proc/self/status by process_umask(), and when that fails nothing is written and the error is
/// "cannot write <path>: cannot determine the process umask"), fsynced, closed and renamed over
/// `path`; the directory is then fsynced so the rename is durable. A reader
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
