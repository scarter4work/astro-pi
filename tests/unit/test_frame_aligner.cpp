#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include <cmath>
#include "engine/FrameAligner.h"

static std::vector<float> makeSyntheticFrame(int width, int height,
                                              const std::vector<std::pair<int,int>>& starPositions,
                                              float background = 0.1f, float peak = 0.9f) {
    std::vector<float> img(width * height, background);
    for (auto [sx, sy] : starPositions) {
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                int x = sx + dx, y = sy + dy;
                if (x >= 0 && x < width && y >= 0 && y < height) {
                    float r2 = float(dx*dx + dy*dy);
                    img[y * width + x] += peak * std::exp(-r2 / 2.0f);
                }
            }
        }
    }
    return img;
}

TEST_CASE("computeCropRegion computes correct bounding box", "[aligner]") {
    std::vector<nukex::AlignmentResult> offsets = {
        {0, 0, 10, 0.5, true},
        {5, -3, 8, 0.6, true},
        {-2, 4, 9, 0.4, true}
    };

    auto crop = nukex::computeCropRegion(offsets, 100, 80);
    REQUIRE(crop.x0 == 5);
    REQUIRE(crop.y0 == 4);
    REQUIRE(crop.x1 == 98);
    REQUIRE(crop.y1 == 77);
    REQUIRE(crop.width() == 93);
    REQUIRE(crop.height() == 73);
}

TEST_CASE("FrameAligner aligns shifted synthetic frames", "[aligner]") {
    int W = 200, H = 150;

    std::vector<std::pair<int,int>> starPos = {
        {50, 40}, {120, 30}, {80, 100}, {160, 70}, {30, 120}, {140, 110}
    };

    int offsets[][2] = {{0,0}, {7,-3}, {-4,5}, {10,2}};
    std::vector<std::vector<float>> frames;
    for (auto [dx, dy] : offsets) {
        std::vector<std::pair<int,int>> shifted;
        for (auto [sx, sy] : starPos)
            shifted.push_back({sx + dx, sy + dy});
        frames.push_back(makeSyntheticFrame(W, H, shifted));
    }

    std::vector<const float*> frameData;
    for (const auto& f : frames)
        frameData.push_back(f.data());

    auto result = nukex::alignFrames(frameData, W, H);
    REQUIRE(result.alignedCube.numSubs() == 4);
    REQUIRE(result.alignedCube.width() < static_cast<size_t>(W));
    REQUIRE(result.alignedCube.height() < static_cast<size_t>(H));
    REQUIRE(result.offsets.size() == 4);
    REQUIRE(result.offsets[0].dx == 0);
    REQUIRE(result.offsets[0].dy == 0);
}
