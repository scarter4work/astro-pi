#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/SubCube.h"

TEST_CASE("SubCube allocation") {
    SECTION("small cube allocates successfully") {
        nukex::SubCube cube(10, 64, 64);
        REQUIRE(cube.numSubs() == 10);
        REQUIRE(cube.height() == 64);
        REQUIRE(cube.width() == 64);
    }
    SECTION("pixel values are zero-initialized") {
        nukex::SubCube cube(5, 8, 8);
        for (size_t z = 0; z < 5; z++)
            REQUIRE(cube.pixel(z, 4, 4) == 0.0f);
    }
}

TEST_CASE("SubCube Z-column extraction") {
    nukex::SubCube cube(3, 4, 4);
    cube.setPixel(0, 2, 3, 1.0f);
    cube.setPixel(1, 2, 3, 2.0f);
    cube.setPixel(2, 2, 3, 3.0f);

    auto col = cube.zColumn(2, 3);
    REQUIRE(col.size() == 3);
    REQUIRE(col(0) == Catch::Approx(1.0f));
    REQUIRE(col(1) == Catch::Approx(2.0f));
    REQUIRE(col(2) == Catch::Approx(3.0f));
}

TEST_CASE("SubCube Z-column is contiguous in memory") {
    nukex::SubCube cube(100, 16, 16);
    cube.setPixel(0, 8, 8, 42.0f);
    cube.setPixel(1, 8, 8, 43.0f);

    const float* ptr = cube.zColumnPtr(8, 8);
    REQUIRE(ptr[0] == Catch::Approx(42.0f));
    REQUIRE(ptr[1] == Catch::Approx(43.0f));
    REQUIRE((&ptr[1] - &ptr[0]) == 1);  // contiguous
}

TEST_CASE("SubCube metadata") {
    nukex::SubCube cube(3, 8, 8);
    nukex::SubMetadata meta;
    meta.fwhm = 2.5;
    meta.filter = "Ha";
    cube.setMetadata(0, meta);
    REQUIRE(cube.metadata(0).fwhm == Catch::Approx(2.5));
    REQUIRE(cube.metadata(0).filter == "Ha");
}

TEST_CASE("SubCube provenance map") {
    nukex::SubCube cube(10, 8, 8);
    cube.setProvenance(3, 5, 7);
    REQUIRE(cube.provenance(3, 5) == 7);
}

TEST_CASE("SubCube distribution type map") {
    nukex::SubCube cube(5, 8, 8);
    cube.setDistType(2, 3, 2);  // SkewNormal
    REQUIRE(cube.distType(2, 3) == 2);
}

TEST_CASE("SubCube setSub writes full slice") {
    nukex::SubCube cube(2, 2, 3);
    float data[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    cube.setSub(0, data, 6);
    REQUIRE(cube.pixel(0, 0, 0) == Catch::Approx(1.0f));
    REQUIRE(cube.pixel(0, 0, 1) == Catch::Approx(2.0f));
    REQUIRE(cube.pixel(0, 0, 2) == Catch::Approx(3.0f));
    REQUIRE(cube.pixel(0, 1, 0) == Catch::Approx(4.0f));
}
