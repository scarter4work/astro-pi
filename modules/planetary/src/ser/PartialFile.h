#pragma once
#include <cstdio>
#include <string>

namespace planetary::ser {

// Creates `partial` (the temporary file a writer renames into place when it is complete) as a
// new regular file and returns it open for binary writing; the caller fcloses it. It never
// opens through a symbolic link: a stale regular file left by an interrupted write is removed
// first, but a symbolic link (dangling or not) or anything else that is not a regular file
// (directory, FIFO, device) is refused and left as it is. The file is then created with
// O_CREAT|O_EXCL|O_NOFOLLOW, so a link planted after the check makes the create fail instead
// of writing through it. POSIX only (the module is linux-x64).
// Returns nullptr on failure with `whyNot` set to the reason, worded to follow
// "cannot create <partial>: " (e.g. "it is a symbolic link; SERViewer never writes through
// one", or the OS reason).
std::FILE* CreatePartialFile(const std::string& partial, std::string& whyNot);

}  // namespace planetary::ser
