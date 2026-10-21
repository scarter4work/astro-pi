#include "PartialFile.h"

#include <cerrno>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace planetary::ser {

namespace {

std::string OsReason(int err) { return std::error_code(err, std::generic_category()).message(); }

}  // namespace

std::FILE* CreatePartialFile(const std::string& partial, std::string& whyNot) {
    struct stat st;
    if (::lstat(partial.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            whyNot = "it is a symbolic link; SERViewer never writes through one";
            return nullptr;
        }
        if (!S_ISREG(st.st_mode)) {
            whyNot = "it exists and is not a regular file";
            return nullptr;
        }
        // A stale partial from an interrupted write: unlink removes the name itself, and
        // never follows a link swapped in since the lstat.
        if (::unlink(partial.c_str()) != 0 && errno != ENOENT) {
            whyNot = "cannot remove the stale partial file: " + OsReason(errno);
            return nullptr;
        }
    }
    const int fd = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        const int err = errno;
        whyNot = err == EEXIST ? "it appeared while being replaced; not written" : OsReason(err);
        return nullptr;
    }
    std::FILE* f = ::fdopen(fd, "wb");
    if (!f) {
        const int err = errno;
        ::close(fd);
        ::unlink(partial.c_str());  // the regular file this call just created
        whyNot = OsReason(err);
        return nullptr;
    }
    return f;
}

}  // namespace planetary::ser
