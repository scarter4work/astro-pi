#include "nukex/core/run_temp_dir.hpp"

#include <stdlib.h>   // mkdtemp
#include <filesystem>
#include <system_error>
#include <vector>

namespace nukex {

RunTempDir::RunTempDir(const std::string& base, const std::string& prefix) {
    if (base.empty()) return;
    std::string tmpl = base;
    if (tmpl.back() != '/') tmpl += '/';
    tmpl += prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) != nullptr)   // creates the directory with mode 0700
        path_ = buf.data();
}

RunTempDir::~RunTempDir() {
    if (path_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);  // does not follow symlinks
}

std::string RunTempDir::file(const std::string& name) const {
    return path_.empty() ? std::string() : path_ + "/" + name;
}

} // namespace nukex
