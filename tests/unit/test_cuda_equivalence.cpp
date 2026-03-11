// tests/unit/test_cuda_equivalence.cpp
// CUDA vs CPU equivalence tests for pixel selection
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/PixelSelector.h"
#include "engine/SubCube.h"

#ifdef NUKEX_HAS_CUDA
#include "engine/cuda/CudaRuntime.h"
#include "engine/cuda/CudaPixelSelector.h"
#endif

using Catch::Approx;

TEST_CASE("GPU and CPU produce equivalent results", "[cuda][equivalence]") {
#ifndef NUKEX_HAS_CUDA
    SKIP("CUDA support not compiled in");
#else
    if (!nukex::cuda::isGpuAvailable()) {
        SKIP("No CUDA-capable GPU available");
    }

    // Create a small SubCube with synthetic data
    constexpr size_t nSubs = 5;
    constexpr size_t H = 8;
    constexpr size_t W = 8;

    nukex::SubCube cpuCube(nSubs, H, W);
    nukex::SubCube gpuCube(nSubs, H, W);

    // Fill with known values: gentle gradient so every pixel is slightly different
    // value = 0.5 + 0.01*z + 0.001*y + 0.0001*x
    for (size_t z = 0; z < nSubs; ++z) {
        for (size_t y = 0; y < H; ++y) {
            for (size_t x = 0; x < W; ++x) {
                float val = 0.5f + 0.01f * z + 0.001f * y + 0.0001f * x;
                cpuCube.setPixel(z, y, x, val);
                gpuCube.setPixel(z, y, x, val);
            }
        }
    }

    // Add one outlier per pixel at frame 0 to test outlier detection
    for (size_t y = 0; y < H; ++y) {
        for (size_t x = 0; x < W; ++x) {
            float original = cpuCube.pixel(0, y, x);
            cpuCube.setPixel(0, y, x, original + 0.5f);
            gpuCube.setPixel(0, y, x, original + 0.5f);
        }
    }

    std::vector<double> weights(nSubs, 1.0 / nSubs);

    // CPU path
    nukex::PixelSelector::Config config;
    config.maxOutliers = 3;
    config.outlierAlpha = 0.05;
    config.adaptiveModels = false;
    nukex::PixelSelector cpuSelector(config);
    auto cpuResult = cpuSelector.processImage(cpuCube, weights, nullptr);

    // GPU path via direct CUDA API
    std::vector<float> gpuOutput(H * W);
    std::vector<uint8_t> gpuDistTypes(H * W);

    nukex::cuda::GpuStackConfig gpuConfig;
    gpuConfig.maxOutliers = config.maxOutliers;
    gpuConfig.outlierAlpha = config.outlierAlpha;
    gpuConfig.adaptiveModels = config.adaptiveModels;
    gpuConfig.nSubs = nSubs;
    gpuConfig.height = H;
    gpuConfig.width = W;

    auto gpuResult = nukex::cuda::processImageGPU(
        gpuCube.cube().data(), gpuOutput.data(), gpuDistTypes.data(), gpuConfig);

    REQUIRE(gpuResult.success);

    // Compare pixel values: every output pixel should match within float tolerance
    REQUIRE(cpuResult.size() == gpuOutput.size());
    for (size_t i = 0; i < cpuResult.size(); ++i) {
        REQUIRE(gpuOutput[i] == Approx(cpuResult[i]).margin(1e-4f));
    }

    // Distribution types should match for most pixels
    // (allow some float-precision divergence on boundary cases)
    size_t distMismatches = 0;
    for (size_t i = 0; i < gpuDistTypes.size(); ++i) {
        if (gpuDistTypes[i] != cpuCube.distType(i / W, i % W)) {
            ++distMismatches;
        }
    }
    // Allow up to 10% divergence in distribution type selection
    REQUIRE(distMismatches <= (H * W) / 10);
#endif
}

TEST_CASE("GPU gracefully falls back to CPU", "[cuda][fallback]") {
    // This test always runs regardless of CUDA availability.
    // processImageGPU should work even without a GPU (falls back to processImage).
    constexpr size_t nSubs = 5;
    constexpr size_t H = 4;
    constexpr size_t W = 4;

    nukex::SubCube cube(nSubs, H, W);
    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, 0.5f + 0.01f * z);

    std::vector<double> weights(nSubs, 1.0 / nSubs);
    std::vector<uint8_t> distTypes;

    nukex::PixelSelector selector;
    auto result = selector.processImageGPU(cube, weights, distTypes, nullptr);

    // Verify output has correct size and contains valid values
    REQUIRE(result.size() == H * W);
    for (float val : result) {
        REQUIRE(std::isfinite(val));
        REQUIRE(val > 0.0f);
        REQUIRE(val < 1.0f);
    }
}

TEST_CASE("GPU handles adaptive model selection", "[cuda][adaptive]") {
#ifndef NUKEX_HAS_CUDA
    SKIP("CUDA support not compiled in");
#else
    if (!nukex::cuda::isGpuAvailable()) {
        SKIP("No CUDA-capable GPU available");
    }

    // Tightly clustered Gaussian data -- should skip expensive fits
    constexpr size_t nSubs = 10;
    constexpr size_t H = 4;
    constexpr size_t W = 4;

    nukex::SubCube cpuCube(nSubs, H, W);
    nukex::SubCube gpuCube(nSubs, H, W);

    // Very tight Gaussian: values barely vary across subs
    for (size_t z = 0; z < nSubs; ++z) {
        for (size_t y = 0; y < H; ++y) {
            for (size_t x = 0; x < W; ++x) {
                float val = 0.5f + 0.0001f * z;
                cpuCube.setPixel(z, y, x, val);
                gpuCube.setPixel(z, y, x, val);
            }
        }
    }

    std::vector<double> weights(nSubs, 1.0 / nSubs);

    // CPU path with adaptive models
    nukex::PixelSelector::Config config;
    config.maxOutliers = 2;
    config.outlierAlpha = 0.05;
    config.adaptiveModels = true;
    nukex::PixelSelector cpuSelector(config);
    auto cpuResult = cpuSelector.processImage(cpuCube, weights, nullptr);

    // GPU path with adaptive models
    std::vector<float> gpuOutput(H * W);
    std::vector<uint8_t> gpuDistTypes(H * W);

    nukex::cuda::GpuStackConfig gpuConfig;
    gpuConfig.maxOutliers = config.maxOutliers;
    gpuConfig.outlierAlpha = config.outlierAlpha;
    gpuConfig.adaptiveModels = true;
    gpuConfig.nSubs = nSubs;
    gpuConfig.height = H;
    gpuConfig.width = W;

    auto gpuResult = nukex::cuda::processImageGPU(
        gpuCube.cube().data(), gpuOutput.data(), gpuDistTypes.data(), gpuConfig);

    REQUIRE(gpuResult.success);

    // Compare: adaptive results should also be equivalent
    REQUIRE(cpuResult.size() == gpuOutput.size());
    for (size_t i = 0; i < cpuResult.size(); ++i) {
        REQUIRE(gpuOutput[i] == Approx(cpuResult[i]).margin(1e-4f));
    }
#endif
}
