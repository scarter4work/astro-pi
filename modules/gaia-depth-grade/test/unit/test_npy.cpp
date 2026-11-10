// Npy.h: the golden reader. The self-test files are written by make_goldens.py.
#include <catch_amalgamated.hpp>

#include "Npy.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
const std::string kDir = std::string(GDG_GOLDEN_DIR) + "/synthetic/";

std::vector<char> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}
}  // namespace

TEST_CASE("npy: <f8 (3,2) round trip", "[npy]") {
    const auto a = gdg::test::load_npy(kDir + "npy_selftest_f8.npy");
    REQUIRE(a.shape == std::vector<size_t>{3, 2});
    REQUIRE(a.data.size() == 6);
    for (size_t i = 0; i < 6; ++i) REQUIRE(a.data[i] == static_cast<double>(i));
}

TEST_CASE("npy: |b1 round trip", "[npy]") {
    const auto a = gdg::test::load_npy(kDir + "npy_selftest_b1.npy");
    REQUIRE(a.shape == std::vector<size_t>{2, 3});
    const double expect[6] = {1, 0, 1, 0, 0, 1};
    for (size_t i = 0; i < 6; ++i) REQUIRE(a.data[i] == expect[i]);
}

TEST_CASE("npy: <f4, <i8 and |u1 dtypes", "[npy]") {
    const auto f4 = gdg::test::load_npy(kDir + "npy_selftest_f4.npy");
    REQUIRE(f4.shape == std::vector<size_t>{4});
    REQUIRE(f4.data[1] == 0.5);
    REQUIRE(f4.data[3] == -2.25);
    const auto i8 = gdg::test::load_npy(kDir + "npy_selftest_i8.npy");
    REQUIRE(i8.shape == std::vector<size_t>{3});
    REQUIRE(i8.data[0] == -7);
    REQUIRE(i8.data[2] == 123456789012.0);
    const auto u1 = gdg::test::load_npy(kDir + "npy_selftest_u1.npy");
    REQUIRE(u1.shape == std::vector<size_t>{2, 2});
    REQUIRE(u1.data[3] == 255);
}

TEST_CASE("npy: 0-d scalar and v2 header", "[npy]") {
    const auto s = gdg::test::load_npy(kDir + "npy_selftest_scalar.npy");
    REQUIRE(s.shape.empty());
    REQUIRE(s.data.size() == 1);
    REQUIRE(s.data[0] == 3.5);
    const auto v2 = gdg::test::load_npy(kDir + "npy_selftest_v2.npy");
    REQUIRE(v2.shape == std::vector<size_t>{2});
    REQUIRE(v2.data[1] == 2.0);
}

TEST_CASE("npy: truncated and malformed files throw", "[npy]") {
    const auto bytes = slurp(kDir + "npy_selftest_f8.npy");
    const std::string tmp = "npy_truncated.tmp";
    auto write = [&](size_t n) {
        std::ofstream out(tmp, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(n));
    };
    write(bytes.size() - 1);  // payload short by one byte
    REQUIRE_THROWS_AS(gdg::test::load_npy(tmp), std::runtime_error);
    write(8);  // header cut off
    REQUIRE_THROWS_AS(gdg::test::load_npy(tmp), std::runtime_error);
    write(0);
    REQUIRE_THROWS_AS(gdg::test::load_npy(tmp), std::runtime_error);
    std::remove(tmp.c_str());
    REQUIRE_THROWS_AS(gdg::test::load_npy(kDir + "does_not_exist.npy"), std::runtime_error);
}

TEST_CASE("npy: fortran order is refused", "[npy]") {
    REQUIRE_THROWS_AS(gdg::test::load_npy(kDir + "npy_selftest_fortran.npy"), std::runtime_error);
}

TEST_CASE("npy: committed stage goldens load with the documented shapes", "[npy]") {
    const auto stars = gdg::test::load_npy(kDir + "stars_rgb.npy");
    REQUIRE(stars.shape == std::vector<size_t>{144, 192, 3});
    for (const double v : stars.data) { REQUIRE(v >= 0.0); REQUIRE(v <= 1.0); }
    const auto u8 = gdg::test::load_npy(kDir + "autostretch_u8.npy");
    REQUIRE(u8.shape == std::vector<size_t>{144, 192, 3});
    const auto matched = gdg::test::load_npy(kDir + "match_table_matched.npy");
    REQUIRE(matched.shape.size() == 1);
    const auto x = gdg::test::load_npy(kDir + "match_table_x.npy");
    REQUIRE(x.shape == matched.shape);
}
