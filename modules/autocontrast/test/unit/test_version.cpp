// AutoContrast: version macros parse and agree with AC_VERSION_STRING.
#include <catch_amalgamated.hpp>

#include "module/AutoContrastVersion.h"

#include <cstdio>
#include <string>

TEST_CASE("version macros parse and match AC_VERSION_STRING", "[version]")
{
   int a = -1, b = -1, c = -1, d = -1;
   char tail = 0;
   REQUIRE(std::sscanf(AC_VERSION_STRING, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4);
   CHECK(a == AC_MODULE_VERSION_MAJOR);
   CHECK(b == AC_MODULE_VERSION_MINOR);
   CHECK(c == AC_MODULE_VERSION_RELEASE);
   CHECK(d == AC_MODULE_VERSION_BUILD);
}

TEST_CASE("this build is 0.1.0.0", "[version]")
{
   CHECK(std::string(AC_VERSION_STRING) == "0.1.0.0");
}
