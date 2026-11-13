// Task 1: the .npy / JSON golden readers.
#include "Npy.h"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef AC_TEST_DIR
#error "AC_TEST_DIR must be defined by the build"
#endif

namespace {
std::string golden(const std::string& rel) {
    return std::string(AC_TEST_DIR) + "/golden/" + rel;
}
std::vector<char> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST_CASE("load_npy reads the f8 selftest array", "[npy]") {
    auto a = ac::test::load_npy(golden("synthetic/npy_selftest_f8.npy"));
    REQUIRE(a.dtype == "<f8");
    REQUIRE(a.shape == std::vector<size_t>{3, 2});
    REQUIRE(a.data.size() == 6);
    for (size_t i = 0; i < 6; ++i) REQUIRE(a.data[i] == static_cast<double>(i));
}

TEST_CASE("load_npy reads the b1 selftest array", "[npy]") {
    auto a = ac::test::load_npy(golden("synthetic/npy_selftest_b1.npy"));
    REQUIRE(a.dtype == "|b1");
    REQUIRE(a.shape == std::vector<size_t>{2, 3});
    const std::vector<double> want{1, 0, 1, 0, 0, 1};
    REQUIRE(a.data == want);
}

TEST_CASE("load_npy reads every supported dtype", "[npy]") {
    REQUIRE(ac::test::load_npy(golden("synthetic/npy_selftest_f4.npy")).dtype == "<f4");
    auto i8 = ac::test::load_npy(golden("synthetic/npy_selftest_i8.npy"));
    REQUIRE(i8.dtype == "<i8");
    REQUIRE(i8.data == std::vector<double>{-3, 0, 7, 1000000});
    auto u2 = ac::test::load_npy(golden("synthetic/npy_selftest_u2.npy"));
    REQUIRE(u2.dtype == "<u2");
    REQUIRE(u2.data == std::vector<double>{0, 1, 65535});
    auto u1 = ac::test::load_npy(golden("synthetic/npy_selftest_u1.npy"));
    REQUIRE(u1.dtype == "|u1");
    REQUIRE(u1.data == std::vector<double>{0, 128, 255});
}

TEST_CASE("load_npy rejects a truncated file", "[npy]") {
    auto bytes = slurp(golden("synthetic/npy_selftest_f8.npy"));
    REQUIRE(bytes.size() > 20);
    const std::string cut = "npy_truncated_test.npy";
    {
        std::ofstream o(cut, std::ios::binary);
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size() - 5));
    }
    REQUIRE_THROWS_AS(ac::test::load_npy(cut), std::runtime_error);
    {  // header cut short
        std::ofstream o(cut, std::ios::binary);
        o.write(bytes.data(), 12);
    }
    REQUIRE_THROWS_AS(ac::test::load_npy(cut), std::runtime_error);
    std::remove(cut.c_str());
}

TEST_CASE("load_npy and load_json throw on a missing file", "[npy]") {
    REQUIRE_THROWS_AS(ac::test::load_npy(golden("synthetic/does_not_exist.npy")),
                      std::runtime_error);
    REQUIRE_THROWS_AS(ac::test::load_json(golden("synthetic/does_not_exist.json")),
                      std::runtime_error);
}

TEST_CASE("load_json reads the MANIFEST", "[npy]") {
    auto j = ac::test::load_json(golden("MANIFEST.json"));
    REQUIRE(j.contains("versions"));
    REQUIRE(j["versions"].contains("numpy"));
}
