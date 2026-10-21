#pragma once
#include <cstdio>
#include <string>

namespace planetary::ser {

// The two halves of every write SERViewer does (Save as SER, Save pattern...): build the
// file under `<target>.partial`, then publish it under its real name. POSIX only (the module is
// linux-x64).

// Creates `partial` as a new regular file and returns it open for binary writing; the caller
// fcloses it. Its mode is 0666 less the umask (as an ofstream/fopen-created file). It never
// opens through a symbolic link: a stale regular file left by an interrupted write is removed
// first, but a symbolic link (dangling or not) or anything else that is not a regular file
// (directory, FIFO, device) is refused and left as it is. The file is then created with
// O_CREAT|O_EXCL|O_NOFOLLOW, so a link planted after the check makes the create fail instead
// of writing through it.
// Returns nullptr on failure with `whyNot` set to the reason, worded to follow
// "cannot create <partial>: " (e.g. "it is a symbolic link; SERViewer never writes through
// one", or the OS reason).
std::FILE* CreatePartialFile(const std::string& partial, std::string& whyNot);

enum class PublishResult { Published, TargetExists, Failed };
using LinkFn = int (*)(const char*, const char*);

// Publishes the complete `partial` as `target`.
// - replace: rename over whatever is there (the caller already asked).
// - otherwise never replaces: hard-links `partial` to `target`, which fails atomically when
//   anything (a symbolic link included) is already there (TargetExists), then removes the
//   partial name. Only where the filesystem has no hard links (link fails EPERM, EOPNOTSUPP,
//   ENOSYS or EXDEV -- e.g. some CIFS mounts) does it fall back to checking `target` and then
//   renaming; there, something created in the instant between the two is replaced: the one
//   race left.
// On TargetExists and Failed (`whyNot` = the OS reason, worded to follow "cannot publish
// <partial> as <target>: ") the partial is left for the caller to remove. A partial name that
// cannot be removed after a successful link is left too (the next write replaces it).
// `linkFn` (default ::link) is a test seam.
PublishResult PublishPartial(const std::string& partial, const std::string& target, bool replace,
                             std::string& whyNot, LinkFn linkFn = nullptr);

}  // namespace planetary::ser
