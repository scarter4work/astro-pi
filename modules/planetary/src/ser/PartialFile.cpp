#include "PartialFile.h"

#include <cerrno>
#include <cstdio>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace planetary::ser {

namespace {

std::string OsReason(int err) { return std::error_code(err, std::generic_category()).message(); }

bool NoHardLinks(int err) {
    return err == EPERM || err == EOPNOTSUPP || err == ENOTSUP || err == ENOSYS || err == EXDEV;
}

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
        if (::unlink(partial.c_str()) != 0) {
            const int err = errno;
            if (err != ENOENT) {
                whyNot = "cannot remove the stale partial file: " + OsReason(err);
                return nullptr;
            }
        }
    }
    const int fd = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0) {
        const int err = errno;
        whyNot = err == EEXIST ? "it was created by something else just now; not written" : OsReason(err);
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

PublishResult PublishPartial(const std::string& partial, const std::string& target, bool replace,
                             std::string& whyNot, LinkFn linkFn) {
    if (!replace) {
        if ((linkFn ? linkFn : &::link)(partial.c_str(), target.c_str()) == 0) {
            ::unlink(partial.c_str());
            return PublishResult::Published;
        }
        const int err = errno;
        if (err == EEXIST) return PublishResult::TargetExists;
        if (!NoHardLinks(err)) {
            whyNot = OsReason(err);
            return PublishResult::Failed;
        }
        // No hard links here: check, then rename (the race documented in the header).
        struct stat st;
        if (::lstat(target.c_str(), &st) == 0) return PublishResult::TargetExists;
    }
    if (std::rename(partial.c_str(), target.c_str()) != 0) {
        const int err = errno;
        whyNot = OsReason(err);
        return PublishResult::Failed;
    }
    return PublishResult::Published;
}

}  // namespace planetary::ser
