#include "AtomicFile.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
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

std::optional<unsigned> umask_from_proc_status(const std::string& text) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.rfind("Umask:", 0) != 0) continue;
        size_t i = 6;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i == line.size()) return std::nullopt;
        unsigned v = 0;
        for (; i < line.size(); ++i) {
            if (line[i] < '0' || line[i] > '7') return std::nullopt;
            v = v * 8 + static_cast<unsigned>(line[i] - '0');
            if (v > 0777) return std::nullopt;
        }
        return v;
    }
    return std::nullopt;
}

std::optional<unsigned> process_umask() {
    std::ifstream in("/proc/self/status");
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return umask_from_proc_status(text);
}

void write_file_atomic(const std::string& path, const void* data, std::size_t size) {
    auto fail = [&](int err) -> void {
        throw std::runtime_error("cannot write " + path + ": " + std::strerror(err));
    };

    // The published mode is 0666 & ~umask; with no readable umask nothing is written.
    const std::optional<unsigned> um = process_umask();
    if (!um) throw std::runtime_error("cannot write " + path + ": cannot determine the process umask");

    // A unique temporary beside the target (same filesystem, so the rename is atomic). mkostemp
    // opens it O_EXCL, mode 0600: never another writer's file, never through a symlink. O_CLOEXEC:
    // PixInsight forks external processes, which must not inherit the descriptor.
    std::string tmpl = path + ".tmp.XXXXXX";
    const int fd = ::mkostemp(tmpl.data(), O_CLOEXEC);
    if (fd < 0) fail(errno);
    const std::string& tmp = tmpl;  // mkostemp wrote the generated name in place

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
    if (::fchmod(fd, static_cast<mode_t>(0666 & ~*um)) != 0) abandon(errno, true);
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
