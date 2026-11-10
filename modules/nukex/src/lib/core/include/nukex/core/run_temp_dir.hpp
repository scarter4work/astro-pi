#pragma once

#include <string>

namespace nukex {

/// A private directory for one run's scratch files.
///
/// The constructor creates `<base>/<prefix>XXXXXX` with mkdtemp(), so the name
/// is unpredictable and the directory is mode 0700 and owned by this process's
/// user: no other local user can plant a symlink or a file inside it. The
/// destructor removes the directory and everything in it.
///
/// If the directory cannot be created, ok() is false, path() is empty and
/// file() returns an empty string; callers skip writing in that case.
class RunTempDir {
public:
    explicit RunTempDir(const std::string& base, const std::string& prefix = "nukex-run-");
    ~RunTempDir();

    RunTempDir(const RunTempDir&) = delete;
    RunTempDir& operator=(const RunTempDir&) = delete;

    bool ok() const { return !path_.empty(); }
    const std::string& path() const { return path_; }

    /// `<path()>/<name>`, or "" when !ok().
    std::string file(const std::string& name) const;

private:
    std::string path_;
};

} // namespace nukex
