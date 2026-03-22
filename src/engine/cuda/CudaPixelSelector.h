// src/engine/cuda/CudaPixelSelector.h
// CUDA pixel selection kernel — GPU reimplementation of selectBestZ() pipeline
#pragma once

#ifdef NUKEX_HAS_CUDA

#include <cstddef>
#include <cstdint>
#include <string>

namespace nukex {
namespace cuda {

struct GpuStackConfig {
    int maxOutliers = 3;
    double outlierAlpha = 0.05;
    bool adaptiveModels = false;
    bool enableMetadataTiebreaker = false;
    size_t nSubs = 0;
    size_t height = 0;
    size_t width = 0;
    const double* qualityScores = nullptr;  // host pointer, copied to device constant memory
    uint32_t* provenanceOut = nullptr;      // device output, optional (nullptr OK)
};

struct GpuStackResult {
    bool success;
    std::string errorMessage;
};

// Run pixel selection on GPU.
// cubeData: column-major float array (nSubs x height x width), Z-column contiguous
// outputPixels: pre-allocated float array (height x width), row-major
// distTypes: pre-allocated uint8_t array (height x width), row-major
GpuStackResult processImageGPU(
    const float* cubeData,
    float* outputPixels,
    uint8_t* distTypes,
    const GpuStackConfig& config);

} // namespace cuda
} // namespace nukex

#endif // NUKEX_HAS_CUDA
