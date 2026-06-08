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
    REQUIRE(crop.x0 == 2);
    REQUIRE(crop.y0 == 3);
    REQUIRE(crop.x1 == 95);
    REQUIRE(crop.y1 == 76);
    REQUIRE(crop.width() == 93);
    REQUIRE(crop.height() == 73);
}

TEST_CASE("applyAlignment produces aligned SubCube for single channel", "[aligner]") {
    int W = 100, H = 80;
    int nFrames = 3;

    // Create per-frame pixel data with known patterns
    std::vector<std::vector<float>> channelFrameData(nFrames);
    for (int i = 0; i < nFrames; ++i) {
        channelFrameData[i].resize(W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                channelFrameData[i][y * W + x] = float(i * 1000 + y * W + x);
    }

    std::vector<nukex::AlignmentResult> offsets = {
        {0, 0, 10, 0.5, true},
        {3, -2, 8, 0.6, true},
        {-1, 4, 9, 0.4, true}
    };

    auto crop = nukex::computeCropRegion(offsets, W, H);
    auto cube = nukex::applyAlignment(channelFrameData, offsets, crop, W, H);

    REQUIRE(cube.numSubs() == 3);
    REQUIRE(cube.width() == static_cast<size_t>(crop.width()));
    REQUIRE(cube.height() == static_cast<size_t>(crop.height()));

    // Verify reference frame (offset 0,0) pixel at (0,0) in crop space
    float expected0 = float(0 * 1000 + crop.y0 * W + crop.x0);
    REQUIRE(cube.pixel(0, 0, 0) == Catch::Approx(expected0));

    // Verify frame 1 (offset 3,-2): source pixel at (crop.x0+0+3, crop.y0+0-2)
    int srcX1 = crop.x0 + 0 + 3;
    int srcY1 = crop.y0 + 0 + (-2);
    float expected1 = float(1 * 1000 + srcY1 * W + srcX1);
    REQUIRE(cube.pixel(1, 0, 0) == Catch::Approx(expected1));
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
