#pragma once

#include <string>

namespace nukex {

/// Where NukeX keeps its frame cache and cube backing by default:
/// $XDG_CACHE_HOME/nukex4/cache, else $HOME/.cache/nukex4/cache, else
/// /var/tmp. Never /tmp: on Fedora and most systemd distributions /tmp is a
/// RAM-backed tmpfs, so a "cache" there is memory -- the very thing the cache
/// exists to spare. A 33-frame 24 MP OSC session writes 13 GB of frame cache.
std::string default_cache_dir();

/// True when `path` sits on a RAM-backed filesystem (tmpfs, ramfs). Used to
/// warn loudly before a run, because the harm is silent: the run works, and
/// the machine swaps.
bool path_is_ram_backed(const std::string& path);

} // namespace nukex
