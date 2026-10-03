#include "nukex/stacker/cache_paths.hpp"

#include <cstdlib>

#if defined(__linux__)
#include <sys/vfs.h>
#endif

namespace nukex {

std::string default_cache_dir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::string(xdg) + "/nukex4/cache";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.cache/nukex4/cache";
    return "/var/tmp";
}

bool path_is_ram_backed(const std::string& path) {
#if defined(__linux__)
    struct statfs st;
    if (::statfs(path.c_str(), &st) != 0) return false;
    constexpr long TMPFS_MAGIC_ = 0x01021994;
    constexpr long RAMFS_MAGIC_ = static_cast<long>(0x858458f6);
    return st.f_type == TMPFS_MAGIC_ || st.f_type == RAMFS_MAGIC_;
#else
    (void)path;
    return false;
#endif
}

} // namespace nukex
