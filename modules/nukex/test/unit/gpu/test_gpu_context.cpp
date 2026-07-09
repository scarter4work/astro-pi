#include "catch_amalgamated.hpp"
#include "nukex/gpu/gpu_context.hpp"
#include <iostream>

using namespace nukex;

TEST_CASE("GPU: enumerate devices finds at least one GPU", "[gpu]") {
    auto devices = GPUContext::enumerate_devices();
    std::cout << "\nFound " << devices.size() << " GPU device(s):\n";
    for (const auto& d : devices) {
        std::cout << "  " << d.name << " (" << d.vendor << ")"
                  << " VRAM=" << (d.global_mem_bytes / (1024*1024)) << "MB"
                  << " CUs=" << d.max_compute_units
                  << " WG=" << d.max_work_group_size
                  << " fp64=" << d.supports_double << "\n";
    }
    REQUIRE(devices.size() >= 1);
}

TEST_CASE("GPU: create context succeeds with auto-select", "[gpu]") {
    auto ctx = GPUContext::create();
    std::cout << "Backend: " << (ctx.is_gpu_available() ? "OPENCL" : "CPU_FALLBACK") << "\n";
    if (ctx.is_gpu_available()) {
        std::cout << "Device: " << ctx.device_info().name << "\n";
        std::cout << "VRAM: " << (ctx.device_info().global_mem_bytes / (1024*1024)) << " MB\n";
    }
    REQUIRE(ctx.is_gpu_available());
}

TEST_CASE("GPU: force CPU fallback", "[gpu]") {
    GPUExecutorConfig config;
    config.force_cpu_fallback = true;
    auto ctx = GPUContext::create(config);
    REQUIRE(!ctx.is_gpu_available());
    REQUIRE(ctx.backend() == GPUBackend::CPU_FALLBACK);
}

TEST_CASE("GPU: batch size estimation is reasonable", "[gpu]") {
    auto ctx = GPUContext::create();
    int batch = ctx.estimate_batch_size(100, 3);  // 100 frames, 3 channels
    std::cout << "Estimated batch size (100 frames, 3 ch): " << batch << " voxels\n";
    // With 16 GB VRAM and ~2.5 KB per voxel, should be millions
    if (ctx.is_gpu_available()) {
        REQUIRE(batch > 100000);
    }
    // CPU fallback with 2 GB should still be substantial
    REQUIRE(batch > 1000);
}

TEST_CASE("GPU: batch size is bounded by host RAM, not just VRAM", "[gpu]") {
    // Regression for the Phase-B OOM kill: the shadow buffers live in host RAM
    // too, so a batch sized only to fit a large GPU's VRAM over-allocated system
    // RAM and got OOM-killed. The 48-frame / 4-channel broadband-OSC case that
    // blew up: ~1788 bytes/voxel.
    const size_t GB = 1024ULL * 1024 * 1024;
    const size_t vram = 13ULL * GB;   // ~85% of a 16 GB card

    // A tight host budget MUST cap the batch below the VRAM-only figure.
    int host_bound = GPUContext::batch_size_for_budgets(48, 4, vram, 4 * GB);
    int vram_only  = GPUContext::batch_size_for_budgets(48, 4, vram, 1000 * GB);
    REQUIRE(host_bound < vram_only);
    REQUIRE(host_bound == static_cast<int>((4 * GB) / 1788));  // exact host bound

    // Symmetric: a tiny VRAM budget caps it when the GPU is the scarce side.
    REQUIRE(GPUContext::batch_size_for_budgets(48, 4, 1 * GB, 1000 * GB)
            == static_cast<int>((1 * GB) / 1788));

    // Always at least one voxel, even with an absurdly small budget.
    REQUIRE(GPUContext::batch_size_for_budgets(48, 4, 0, 0) == 1);
}
