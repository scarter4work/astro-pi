#include "nukex/learning/atomic_write.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace nukex::learning {

bool atomic_write_file(const std::string& path, const std::string& contents) {
    const std::string tmp = path + ".tmp";

    // Scoped so the ofstream closes (and flushes the C++ buffer) before we
    // reopen with POSIX open() for fsync.
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        f.flush();
        if (!f.good()) return false;
    }

#if defined(__unix__) || defined(__APPLE__)
    // fsync the tmp file so the data is on disk before the atomic rename.
    // Without this, a power loss between write() and rename() could leave
    // `path` referring to a zero-length file after the rename completes.
    int fd = ::open(tmp.c_str(), O_RDWR);
    if (fd < 0) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    int sync_rc = ::fsync(fd);
    ::close(fd);
    if (sync_rc != 0) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
#endif

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return false;
    }
    return true;
}

} // namespace nukex::learning
