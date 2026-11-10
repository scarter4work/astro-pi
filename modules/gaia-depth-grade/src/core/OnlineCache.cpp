// The online source's private cache directory (port of gdgWorkDir / gdgPrivateDir, PJSR).
#include "OnlineCache.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace gdg {

namespace {

constexpr mode_t kPrivate = 0700;

std::string refuse(const std::string& dir, const std::string& why) {
    return "Gaia Depth Grade: refusing to use " + dir + ": " + why;
}

std::string link_target(const std::string& path) {
    std::vector<char> buf(4096);
    const ssize_t n = readlink(path.c_str(), buf.data(), buf.size() - 1);
    return n < 0 ? std::string("?") : std::string(buf.data(), static_cast<size_t>(n));
}

void mkdirs(const std::string& path) {
    // File.createDirectory( base, true ): create missing parents.
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += (cur.empty() || cur.back() == '/') ? part : "/" + part;
        if (mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST)
            throw std::runtime_error("Gaia Depth Grade: cannot create " + cur + ": " + std::strerror(errno));
    }
}

std::string base_cache_dir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] == '/') return xdg;
    const char* home = std::getenv("HOME");
    if (!home || home[0] != '/') throw std::runtime_error("Gaia Depth Grade: cannot determine the home directory");
    return std::string(home) + "/.cache";
}

}  // namespace

void ensure_private_dir(const std::string& dir, unsigned uid) {
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0) {  // lstat: a symbolic link is never followed
        if (errno != ENOENT) throw std::runtime_error(refuse(dir, std::strerror(errno)));
        if (mkdir(dir.c_str(), kPrivate) != 0 && errno != EEXIST)
            throw std::runtime_error("Gaia Depth Grade: cannot create " + dir + ": " + std::strerror(errno));
        if (lstat(dir.c_str(), &st) != 0) throw std::runtime_error(refuse(dir, std::strerror(errno)));
    }
    if (S_ISLNK(st.st_mode))
        throw std::runtime_error(refuse(dir, "it is a symbolic link (to " + link_target(dir) +
                                                 "), not a directory. Remove it and try again."));
    if (!S_ISDIR(st.st_mode)) throw std::runtime_error(refuse(dir, "it is not a directory."));
    if (st.st_uid != uid)
        throw std::runtime_error(refuse(dir, "it is owned by user id " + std::to_string(st.st_uid) + ", not by you (" +
                                                 std::to_string(uid) + ")."));
    if ((st.st_mode & 07777) != kPrivate) {
        if (chmod(dir.c_str(), kPrivate) != 0 || lstat(dir.c_str(), &st) != 0 || (st.st_mode & 07777) != kPrivate)
            throw std::runtime_error("Gaia Depth Grade: cannot restrict " + dir + " to mode 0700.");
    }
}

std::string default_cache_dir() { return base_cache_dir() + "/GaiaDepthGrade/gaia"; }

OnlineCache::OnlineCache(const std::string& dir) : uid_(static_cast<unsigned>(geteuid())) {
    if (!dir.empty()) {
        dir_ = dir;
        ensure_private_dir(dir_, uid_);
        return;
    }
    const std::string base = base_cache_dir();
    struct stat st;
    if (lstat(base.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode) && st.st_uid != uid_)
            throw std::runtime_error(refuse(base, "it is a symbolic link owned by user id " + std::to_string(st.st_uid) +
                                                      ", not by you."));
    } else {
        mkdirs(base);
    }
    ensure_private_dir(base + "/GaiaDepthGrade", uid_);
    dir_ = base + "/GaiaDepthGrade/gaia";
    ensure_private_dir(dir_, uid_);
}

std::string OnlineCache::path_for(const std::string& key) const { return dir_ + "/gaia_" + key + ".csv"; }

std::optional<std::string> OnlineCache::read(const std::string& key) const {
    const std::string path = path_for(key);
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return std::nullopt;
        throw std::runtime_error("Gaia Depth Grade: cannot read " + path + ": " + std::strerror(errno));
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != uid_)
        throw std::runtime_error("Gaia Depth Grade: refusing cache entry " + path +
                                 ": not a regular file owned by you. Remove it and try again.");
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("Gaia Depth Grade: cannot read " + path + ": " + std::strerror(errno));
    std::string text;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            throw std::runtime_error("Gaia Depth Grade: cannot read " + path + ": " + std::strerror(errno));
        }
        if (n == 0) break;
        text.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return text;
}

void OnlineCache::write(const std::string& key, const std::string& text) const {
    const std::string path = path_for(key);
    std::string tmpl = dir_ + "/.gaia_" + key + ".XXXXXX";
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());  // mode 0600
    if (fd < 0) throw std::runtime_error("Gaia Depth Grade: cannot write the Gaia cache in " + dir_ + ": " + std::strerror(errno));
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            close(fd);
            unlink(name.data());
            throw std::runtime_error("Gaia Depth Grade: cannot write " + path + ": " + std::strerror(err));
        }
        done += static_cast<size_t>(n);
    }
    if (fchmod(fd, 0600) != 0 || close(fd) != 0 || rename(name.data(), path.c_str()) != 0) {
        const int err = errno;
        unlink(name.data());
        throw std::runtime_error("Gaia Depth Grade: cannot write " + path + ": " + std::strerror(err));
    }
}

}  // namespace gdg
