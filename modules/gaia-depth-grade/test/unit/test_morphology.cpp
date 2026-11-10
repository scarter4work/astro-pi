// Task 5: scipy.ndimage.grey_closing (flat square, mode='reflect') against the Task 1 goldens
// (scipy 1.18.0) and hand-computed edge cases (cross-checked with scipy 1.18.0).
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/Morphology.h"
#include "core/Numerics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

using gdg::Image;
using gdg::test::count_unequal;
using gdg::test::golden_image;

namespace {
Image row(const std::vector<double>& v) {
    Image im(static_cast<int>(v.size()), 1, 1);
    im.data = v;
    return im;
}
Image column(const std::vector<double>& v) {
    Image im(1, static_cast<int>(v.size()), 1);
    im.data = v;
    return im;
}
}  // namespace

TEST_CASE("grey_closing_15_matches_scipy", "[morphology][golden]") {
    const Image in = golden_image("grey_closing_input");  // 64x48, smoothed noise + hot pixel
    const Image want = golden_image("grey_closing_15");
    const Image got = gdg::grey_closing(in, 15);
    REQUIRE(got.same_shape(want));
    CHECK(count_unequal(got.data, want.data) == 0);
}

TEST_CASE("grey_closing_5_matches_scipy", "[morphology][golden]") {
    const Image in = golden_image("grey_closing_input");
    const Image want = golden_image("grey_closing_5");
    const Image got = gdg::grey_closing(in, 5);
    REQUIRE(got.same_shape(want));
    CHECK(count_unequal(got.data, want.data) == 0);
}

TEST_CASE("grey_closing_edges_reflect", "[morphology]") {
    // An edge pit [0, 5, 5, 5, 5], size 3: reflect repeats the 0 beyond the edge, dilation lifts
    // it to 5 and erosion keeps 5 (constant mode, cval 0, would give 0 at both ends).
    CHECK(gdg::grey_closing(row({0, 5, 5, 5, 5}), 3).data == std::vector<double>{5, 5, 5, 5, 5});
    // size 5 on a width-3 row: the window reflects past the far edge.
    // dilation [i-2, i+2] over ...1 4 | 4 1 3 | 3 1... = 4 everywhere; erosion of 4s = 4.
    CHECK(gdg::grey_closing(row({4, 1, 3}), 5).data == std::vector<double>{4, 4, 4});
    // Interior pit narrower than the footprint fills; a wider one keeps its floor.
    CHECK(gdg::grey_closing(row({5, 5, 1, 5, 5}), 3).data == std::vector<double>{5, 5, 5, 5, 5});
    CHECK(gdg::grey_closing(row({5, 1, 1, 1, 5}), 3).data == std::vector<double>{5, 1, 1, 1, 5});
}

TEST_CASE("grey_closing_even_size_origin", "[morphology]") {
    // size 4 on [1, 9, 2, 8, 3]: dilation (origin -1) windows [i-1, i+2] -> [9, 9, 9, 8, 8];
    // erosion windows [i-2, i+1] -> [9, 9, 8, 8, 8]. scipy 1.18: [9, 9, 8, 8, 8], both along x
    // and along y (the one-pixel axis is a pure reflection).
    const std::vector<double> want{9, 9, 8, 8, 8};
    CHECK(gdg::grey_closing(row({1, 9, 2, 8, 3}), 4).data == want);
    CHECK(gdg::grey_closing(column({1, 9, 2, 8, 3}), 4).data == want);
}

TEST_CASE("grey_closing_parallel_matches_bruteforce", "[morphology]") {
    // 700x600 crosses the parallel threshold. Brute force: per-pixel max over the dilation
    // window, then min over the erosion window, both through scipy's reflect index.
    Image in(700, 600, 1);
    for (size_t i = 0; i < in.data.size(); ++i)
        in.data[i] = std::fmod(static_cast<double>(i) * 0.6180339887, 1.0) + 0.001 * static_cast<double>(i % 7);
    for (int size : {15, 4}) {
        INFO("size " << size);
        const long lo_d = size / 2 - (size % 2 == 0 ? 1 : 0);  // dilation window starts at i - lo_d
        const long lo_e = size / 2;                            // erosion window starts at i - lo_e
        auto pass = [&](const Image& src, bool is_max, long lo) {
            Image tmp = src, dst = src;
            for (int y = 0; y < src.height; ++y)  // axis 0
                for (int x = 0; x < src.width; ++x) {
                    double m = src.at(x, static_cast<int>(gdg::reflect_index(y - lo, src.height)));
                    for (long k = 1; k < size; ++k) {
                        const double v = src.at(x, static_cast<int>(gdg::reflect_index(y - lo + k, src.height)));
                        m = is_max ? std::max(m, v) : std::min(m, v);
                    }
                    tmp.at(x, y) = m;
                }
            for (int y = 0; y < src.height; ++y)  // axis 1
                for (int x = 0; x < src.width; ++x) {
                    double m = tmp.at(static_cast<int>(gdg::reflect_index(x - lo, src.width)), y);
                    for (long k = 1; k < size; ++k) {
                        const double v = tmp.at(static_cast<int>(gdg::reflect_index(x - lo + k, src.width)), y);
                        m = is_max ? std::max(m, v) : std::min(m, v);
                    }
                    dst.at(x, y) = m;
                }
            return dst;
        };
        const Image want = pass(pass(in, true, lo_d), false, lo_e);
        CHECK(count_unequal(gdg::grey_closing(in, size).data, want.data) == 0);
    }
}

TEST_CASE("grey_closing_size1_and_errors", "[morphology]") {
    const Image in = golden_image("grey_closing_input");
    CHECK(count_unequal(gdg::grey_closing(in, 1).data, in.data) == 0);
    CHECK_THROWS_AS(gdg::grey_closing(in, 0), std::invalid_argument);
    CHECK_THROWS_AS(gdg::grey_closing(Image(4, 4, 3), 3), std::invalid_argument);
    CHECK(gdg::grey_closing(Image(), 3).data.empty());
}
