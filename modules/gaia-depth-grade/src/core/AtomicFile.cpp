#include "AtomicFile.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace gdg {

namespace {

/// The directory holding `path` ("." for a bare name, "/" for a file in the root).
std::string parent_dir(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

}  // namespace

void write_file_atomic(const std::string& path, const void* data, std::size_t size) {
    auto fail = [&](int err) -> void {
        throw std::runtime_error("cannot write " + path + ": " + std::strerror(err));
    };

    // A unique temporary beside the target (same filesystem, so the rename is atomic). mkstemp
    // opens it O_EXCL, mode 0600: never another writer's file, never through a symlink.
    std::string tmpl = path + ".tmp.XXXXXX";
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) fail(errno);
    const std::string& tmp = tmpl;  // mkstemp wrote the generated name in place

    // Closes the descriptor (when still open), removes the temporary, throws.
    auto abandon = [&](int err, bool close_fd) {
        if (close_fd) ::close(fd);
        ::unlink(tmp.c_str());
        fail(err);
    };

    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, bytes + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            abandon(errno, true);
        }
        if (n == 0) abandon(EIO, true);  // no progress on a non-empty remainder: never spin
        done += static_cast<std::size_t>(n);
    }
    if (::fchmod(fd, 0644) != 0) abandon(errno, true);
    if (::fsync(fd) != 0) abandon(errno, true);
    if (::close(fd) != 0) abandon(errno, false);
    if (::rename(tmp.c_str(), path.c_str()) != 0) abandon(errno, false);

    // Make the rename itself durable.
    const int dfd = ::open(parent_dir(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) fail(errno);
    if (::fsync(dfd) != 0) {
        const int err = errno;
        ::close(dfd);
        fail(err);
    }
    if (::close(dfd) != 0) fail(errno);
}

}  // namespace gdg
