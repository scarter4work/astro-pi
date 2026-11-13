// Catch2 main for the real-render tests: exit 77 (ctest SKIP) only when the data directory is absent (DataDir.h).
// ac_catch2_nomain is built with CATCH_AMALGAMATED_CUSTOM_MAIN (PUBLIC), so this is the only main.
#include <catch_amalgamated.hpp>

#include "DataDir.h"

#include <cstdio>
#include <string>

int main(int argc, char* argv[])
{
   std::string why;
   if (!ac::test::data_dir_present(&why)) {
      std::printf("SKIP: %s\n", why.c_str());
      return 77;
   }
   return Catch::Session().run(argc, argv);
}
