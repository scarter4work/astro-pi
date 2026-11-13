// The real render corpus (tools/autocontrast/data: references/, amateur/, discovery_cache/), read-only.
// Taken from env AC_DATA_DIR, default the worktree's tools/autocontrast/data. Tests that need it link
// data_main.cpp, which exits 77 (ctest SKIP_RETURN_CODE) only when that directory itself is absent. Once it
// exists, every subdirectory a test needs must resolve: a missing or dangling one FAILS the test, naming the path
// (require_corpus), and so does any missing or changed file.
#pragma once

#include <cstdlib>
#include <initializer_list>
#include <string>
#include <sys/stat.h>

namespace ac::test {

inline std::string data_dir()
{
   const char* env = std::getenv("AC_DATA_DIR");
   if (env != nullptr && *env != '\0')
      return env;
   return std::string(AC_TEST_DIR) + "/../../../tools/autocontrast/data";
}

inline bool is_dir(const std::string& p)
{
   struct stat st {};
   return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);   // stat follows symlinks: a dangling link is not
}

// golden group -> corpus subdirectory (make_goldens.py run_real)
inline std::string group_subdir(const std::string& group)
{
   if (group == "reference") return "references";
   if (group == "amateur") return "amateur";
   if (group == "cache") return "discovery_cache";
   return "<unknown group " + group + ">";
}

inline std::string group_dir(const std::string& group) { return data_dir() + "/" + group_subdir(group); }

// The skip rule: the corpus is absent only when the data directory itself is.
inline bool data_dir_present(std::string* why)
{
   if (is_dir(data_dir()))
      return true;
   if (why != nullptr)
      *why = "render corpus absent: " + data_dir() + " is not a directory (set AC_DATA_DIR)";
   return false;
}

// "" when every listed subdirectory resolves, else a message naming the first that does not (the test FAILS on it).
inline std::string missing_subdir(std::initializer_list<const char*> subs)
{
   for (const char* s : subs) {
      const std::string p = data_dir() + "/" + s;
      if (!is_dir(p))
         return "corpus subdirectory missing or a dangling link: " + p;
   }
   return "";
}

} // namespace ac::test
