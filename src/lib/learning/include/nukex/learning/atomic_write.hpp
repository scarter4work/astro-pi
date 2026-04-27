#pragma once
#include <string>

namespace nukex::learning {

// Write `contents` to `<path>.tmp`, fsync, rename over `path`. Returns false
// on any I/O error; on failure the original file at `path` is left untouched.
// On POSIX, tmp is fsynced before the rename so a power-loss mid-write
// cannot leave a truncated file behind.
bool atomic_write_file(const std::string& path, const std::string& contents);

} // namespace nukex::learning
