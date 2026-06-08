// tests/unit/test_cuda_equivalence.cpp
// CUDA vs CPU equivalence tests for pixel selection
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>
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

    // CPU path
    nukex::PixelSelector::Config config;
    config.maxOutliers = 3;
    config.outlierAlpha = 0.05;
    config.adaptiveModels = false;
    nukex::PixelSelector cpuSelector(config);
    auto cpuResult = cpuSelector.processImage(cpuCube, nullptr, nullptr);

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

    // Distribution types may differ between CPU (Boost.Math) and GPU
    // (hand-rolled EM/L-BFGS) due to different numerical paths on
    // boundary data.  This is metadata only — does not affect pixel values.
    // We verify pixel value equivalence above; dist type is informational.
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

    std::vector<uint8_t> distTypes;

    nukex::PixelSelector selector;
    auto result = selector.processImageGPU(cube, nullptr, distTypes, nullptr);

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

    // CPU path with adaptive models
    nukex::PixelSelector::Config config;
    config.maxOutliers = 2;
    config.outlierAlpha = 0.05;
    config.adaptiveModels = true;
    nukex::PixelSelector cpuSelector(config);
    auto cpuResult = cpuSelector.processImage(cpuCube, nullptr, nullptr);

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

TEST_CASE("GPU stacking with 100 subs matches CPU", "[cuda][equivalence][high-subs]") {
#ifndef NUKEX_HAS_CUDA
    SKIP("CUDA support not compiled in");
#else
    if (!nukex::cuda::isGpuAvailable()) {
        SKIP("No CUDA-capable GPU available");
    }

    constexpr size_t nSubs = 100;
    constexpr size_t H = 4, W = 4;

    nukex::SubCube cube(nSubs, H, W);
    std::mt19937 rng(12345);
    std::normal_distribution<double> noise(500.0, 15.0);

    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    cube.setPixel(50, 2, 2, 9999.0f);

    nukex::PixelSelector::Config cfg;
    cfg.maxOutliers = 3;
    cfg.outlierAlpha = 0.05;
    cfg.adaptiveModels = false;
    nukex::PixelSelector cpuSel(cfg);
    auto cpuResult = cpuSel.processImage(cube, nullptr, nullptr);

    std::vector<float> gpuOutput(H * W);
    std::vector<uint8_t> gpuDistTypes(H * W);

    nukex::cuda::GpuStackConfig gpuConfig;
    gpuConfig.maxOutliers = 3;
    gpuConfig.outlierAlpha = 0.05;
    gpuConfig.adaptiveModels = false;
    gpuConfig.nSubs = nSubs;
    gpuConfig.height = H;
    gpuConfig.width = W;

    auto result = nukex::cuda::processImageGPU(
        cube.cube().data(), gpuOutput.data(), gpuDistTypes.data(), gpuConfig);

    REQUIRE(result.success);

    for (size_t i = 0; i < cpuResult.size(); ++i) {
        REQUIRE(gpuOutput[i] == Catch::Approx(cpuResult[i]).margin(1e-3f));
    }
#endif
}

TEST_CASE("GPU stacking with 256 subs matches CPU", "[cuda][equivalence][high-subs]") {
#ifndef NUKEX_HAS_CUDA
    SKIP("CUDA support not compiled in");
#else
    if (!nukex::cuda::isGpuAvailable()) {
        SKIP("No CUDA-capable GPU available");
    }

    constexpr size_t nSubs = 256;
    constexpr size_t H = 4, W = 4;

    nukex::SubCube cube(nSubs, H, W);
    std::mt19937 rng(67890);
    std::normal_distribution<double> noise(300.0, 10.0);

    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    nukex::PixelSelector::Config cfg;
    cfg.maxOutliers = 5;
    cfg.outlierAlpha = 0.05;
    nukex::PixelSelector cpuSel(cfg);
    auto cpuResult = cpuSel.processImage(cube, nullptr, nullptr);

    std::vector<float> gpuOutput(H * W);
    std::vector<uint8_t> gpuDistTypes(H * W);

    nukex::cuda::GpuStackConfig gpuConfig;
    gpuConfig.maxOutliers = 5;
    gpuConfig.outlierAlpha = 0.05;
    gpuConfig.nSubs = nSubs;
    gpuConfig.height = H;
    gpuConfig.width = W;

    auto result = nukex::cuda::processImageGPU(
        cube.cube().data(), gpuOutput.data(), gpuDistTypes.data(), gpuConfig);

    REQUIRE(result.success);

    for (size_t i = 0; i < cpuResult.size(); ++i) {
        REQUIRE(gpuOutput[i] == Catch::Approx(cpuResult[i]).margin(1e-3f));
    }
#endif
}

TEST_CASE("GPU mask pre-filtering matches CPU", "[cuda][equivalence][masks]") {
#ifndef NUKEX_HAS_CUDA
    SKIP("CUDA support not compiled in");
#else
    if (!nukex::cuda::isGpuAvailable()) {
        SKIP("No CUDA-capable GPU available");
    }

    // 8 frames, 4x4 image. Frames 2 and 5 have a bright "trail" at specific
    // pixels. Masks mark those pixels so the selector should skip them.
    constexpr size_t nSubs = 8;
    constexpr size_t H = 4;
    constexpr size_t W = 4;

    nukex::SubCube cpuCube(nSubs, H, W);
    nukex::SubCube gpuCube(nSubs, H, W);

    // Fill with clean Gaussian data around 0.5
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0.5, 0.01);
    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x) {
                float val = static_cast<float>(noise(rng));
                cpuCube.setPixel(z, y, x, val);
                gpuCube.setPixel(z, y, x, val);
            }

    // Inject bright trail pixels at (y=1, x=0..3) in frames 2 and 5
    for (size_t x = 0; x < W; ++x) {
        cpuCube.setPixel(2, 1, x, 0.95f);
        cpuCube.setPixel(5, 1, x, 0.92f);
        gpuCube.setPixel(2, 1, x, 0.95f);
        gpuCube.setPixel(5, 1, x, 0.92f);
    }

    // Allocate masks and mark the trail pixels
    cpuCube.allocateMasks();
    gpuCube.allocateMasks();
    for (size_t x = 0; x < W; ++x) {
        cpuCube.setMask(2, 1, x, 1);
        cpuCube.setMask(5, 1, x, 1);
        gpuCube.setMask(2, 1, x, 1);
        gpuCube.setMask(5, 1, x, 1);
    }

    // CPU path (masks flow through processImage → selectBestZ)
    nukex::PixelSelector::Config config;
    config.maxOutliers = 3;
    config.outlierAlpha = 0.05;
    config.adaptiveModels = false;
    nukex::PixelSelector cpuSelector(config);
    auto cpuResult = cpuSelector.processImage(cpuCube, nullptr, nullptr);

    // GPU path via direct CUDA API with masks
    std::vector<float> gpuOutput(H * W);
    std::vector<uint8_t> gpuDistTypes(H * W);

    nukex::cuda::GpuStackConfig gpuConfig;
    gpuConfig.maxOutliers = config.maxOutliers;
    gpuConfig.outlierAlpha = config.outlierAlpha;
    gpuConfig.adaptiveModels = config.adaptiveModels;
    gpuConfig.nSubs = nSubs;
    gpuConfig.height = H;
    gpuConfig.width = W;
    gpuConfig.maskData = gpuCube.maskTensorData();

    auto gpuResult = nukex::cuda::processImageGPU(
        gpuCube.cube().data(), gpuOutput.data(), gpuDistTypes.data(), gpuConfig);

    REQUIRE(gpuResult.success);

    // GPU output should match CPU output (both use masks)
    REQUIRE(cpuResult.size() == gpuOutput.size());
    for (size_t i = 0; i < cpuResult.size(); ++i) {
        REQUIRE(gpuOutput[i] == Approx(cpuResult[i]).margin(1e-4f));
    }

    // Trail row pixels should be near 0.5 (clean data), not near 0.9 (trail)
    for (size_t x = 0; x < W; ++x) {
        float trailRowPixel = gpuOutput[1 * W + x];
        REQUIRE(trailRowPixel < 0.6f);
        REQUIRE(trailRowPixel > 0.4f);
    }
#endif
}

TEST_CASE("GPU mask pre-filtering via PixelSelector wrapper", "[cuda][equivalence][masks]") {
#ifndef NUKEX_HAS_CUDA
    SKIP("CUDA support not compiled in");
#else
    if (!nukex::cuda::isGpuAvailable()) {
        SKIP("No CUDA-capable GPU available");
    }

    // Test the full PixelSelector::processImageGPU path with masks.
    constexpr size_t nSubs = 10;
    constexpr size_t H = 4;
    constexpr size_t W = 4;

    nukex::SubCube cube(nSubs, H, W);

    // Uniform clean data at 0.3
    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, 0.3f);

    // One frame has a bright trail at pixel (2,2)
    cube.setPixel(3, 2, 2, 0.99f);

    // Mark it
    cube.allocateMasks();
    cube.setMask(3, 2, 2, 1);

    std::vector<uint8_t> distTypes;
    nukex::PixelSelector::Config cfg;
    cfg.useGPU = true;
    nukex::PixelSelector selector(cfg);
    auto result = selector.processImageGPU(cube, nullptr, distTypes, nullptr);

    REQUIRE(result.size() == H * W);

    // The masked pixel should come out ~0.3 (from the 9 clean frames), not ~0.37
    // (which would be the mean including the 0.99 trail pixel)
    float maskedPixel = result[2 * W + 2];
    REQUIRE(maskedPixel == Approx(0.3f).margin(0.01f));
#endif
}
