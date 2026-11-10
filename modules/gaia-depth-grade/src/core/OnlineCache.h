// The online source's private on-disk cache (plan Task 13; port of the PJSR gdgWorkDir /
// gdgPrivateDir hardening). POSIX, PCL-free. Every directory on the path the module creates is
// checked without following links: a real directory owned by this user, mode 0700. Anything
// else stops with an error; nothing is ever read or written through it.
#pragma once

#include <optional>
#include <string>

namespace gdg {

/// `dir` must be (or is created as) a real directory owned by `uid`, mode 0700 (repaired if
/// looser). Throws std::runtime_error naming the problem.
void ensure_private_dir(const std::string& dir, unsigned uid);

/// The cache base: $XDG_CACHE_HOME when it is an absolute path, else $HOME/.cache; then
/// GaiaDepthGrade/gaia below it. A base that is a symbolic link owned by another user is refused.
std::string default_cache_dir();

class OnlineCache {
public:
    /// Verifies (creating as needed) `<base>/GaiaDepthGrade/gaia` when `dir` is empty, else `dir`
    /// itself (tests); owner = this process' effective uid.
    explicit OnlineCache(const std::string& dir = std::string());

    const std::string& dir() const { return dir_; }
    /// `<dir>/gaia_<key>.csv`
    std::string path_for(const std::string& key) const;
    /// The cached text, or nothing when absent. A cache entry that is not a regular file owned
    /// by this user is refused (std::runtime_error).
    std::optional<std::string> read(const std::string& key) const;
    /// Atomic write (temporary file + rename), mode 0600.
    void write(const std::string& key, const std::string& text) const;

private:
    std::string dir_;
    unsigned uid_;
};

}  // namespace gdg
