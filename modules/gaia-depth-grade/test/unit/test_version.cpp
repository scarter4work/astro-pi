#include <catch_amalgamated.hpp>

#include <string>

#include "module/GaiaDepthGradeVersion.h"

TEST_CASE("version_is_1_0_0_0", "[version]")
{
   CHECK(std::string(GDG_VERSION_STRING) == "1.0.0.0");
}

TEST_CASE("built_as_cxx20", "[version]")
{
   // PCL 2.13's headers need C++20; the PCL-free core builds the same way.
   CHECK(__cplusplus >= 202002L);
}
