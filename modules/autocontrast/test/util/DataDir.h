// The real render corpus (tools/autocontrast/data: references/, amateur/, discovery_cache/), read-only.
// Taken from env AC_DATA_DIR, default the worktree's tools/autocontrast/data. Tests that need it link
// data_main.cpp, which exits 77 (ctest SKIP_RETURN_CODE) only when one of the three directories is absent;
// a present corpus with a missing or changed file FAILS.
#pragma once

#include <cstdlib>
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

// golden group -> corpus subdirectory (make_goldens.py run_real)
inline std::string group_dir(const std::string& group)
{
   if (group == "reference") return data_dir() + "/references";
   if (group == "amateur") return data_dir() + "/amateur";
   if (group == "cache") return data_dir() + "/discovery_cache";
   return data_dir() + "/<unknown group " + group + ">";
}

// Same rule as make_goldens.py resolve_data_dir(): the corpus is there when all three subdirectories resolve
// (stat follows symlinks, so a dangling discovery_cache link -- e.g. inside the build container -- is absent).
inline bool corpus_present(std::string* why)
{
   for (const char* sub : {"references", "amateur", "discovery_cache"}) {
      struct stat st {};
      const std::string p = data_dir() + "/" + sub;
      if (::stat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
         if (why != nullptr)
            *why = "render corpus absent: " + p + " is not a directory (set AC_DATA_DIR)";
         return false;
      }
   }
   return true;
}

} // namespace ac::test
