#include "catch_amalgamated.hpp"
#include "nukex/gpu/gpu_context.hpp"
#include "nukex/gpu/gpu_kernels.hpp"
#include <iostream>

using namespace nukex;

TEST_CASE("GPU Kernels: all 4 kernels compile successfully", "[gpu][kernels]") {
    auto ctx = GPUContext::create();
    if (!ctx.is_gpu_available()) {
        SKIP("No OpenCL GPU available");
    }

    GPUKernels kernels;
    bool ok = kernels.compile(ctx);

    std::cout << "Kernel compilation: " << (ok ? "SUCCESS" : "FAILED") << "\n";
    REQUIRE(ok);
    REQUIRE(kernels.is_compiled());

#if NUKEX_HAS_OPENCL
    REQUIRE(kernels.classify_weights_kernel() != nullptr);
    REQUIRE(kernels.robust_stats_kernel() != nullptr);
    REQUIRE(kernels.select_pixels_kernel() != nullptr);
    REQUIRE(kernels.spatial_context_kernel() != nullptr);
#endif

    std::cout << "All 4 OpenCL kernels compiled and ready.\n";
}
