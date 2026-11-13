#include "AtomicFile.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace gdg {

void write_file_atomic(const std::string& path, const void* data, std::size_t size) {
    const std::string tmp = path + ".tmp";
    // Throws the contract's message; removes the temporary only when this call opened it.
    auto fail = [&](int err, bool remove_tmp) {
        if (remove_tmp) ::unlink(tmp.c_str());
        throw std::runtime_error("cannot write " + path + ": " + std::strerror(err));
    };

    // O_NOFOLLOW: a symlink planted at the temporary name is refused, never written through.
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0) fail(errno, false);

    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, bytes + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            ::close(fd);
            fail(err, true);
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int err = errno;
        ::close(fd);
        fail(err, true);
    }
    if (::close(fd) != 0) fail(errno, true);
    if (::rename(tmp.c_str(), path.c_str()) != 0) fail(errno, true);
}

}  // namespace gdg
